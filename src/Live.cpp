// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Live.h"
#include "RealtimeAudio.h"

// libdatachannel's headers don't build cleanly with JUCE's strict warning set
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextra-semi"
#pragma clang diagnostic ignored "-Winconsistent-missing-destructor-override"
#pragma clang diagnostic ignored "-Wshadow-field-in-constructor"
#pragma clang diagnostic ignored "-Wshadow-all"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include <rtc/rtc.hpp>
#pragma clang diagnostic pop

#include <opus.h>

namespace elsewhere
{
namespace Live
{
//==============================================================================
/**
    Master-bus plugin: captures your output per interval and mixes in collaborators'
    previous interval. The audio thread never blocks: shared state is guarded by a
    spin lock that the audio thread only try-locks (skipping a block if it's busy),
    and finished captures are handed over through per-slot atomic states.
*/
class LiveTapPlugin : public te::Plugin
{
public:
    static const char* xmlTypeName;

    LiveTapPlugin (te::PluginCreationInfo info) : te::Plugin (info)
    {
        for (auto& g : peerGains)
            g = 1.0f;
    }
    ~LiveTapPlugin() override   { notifyListenersOfDeletion(); }

    juce::String getName() const override                  { return "Live Session"; }
    juce::String getPluginType() override                  { return xmlTypeName; }
    juce::String getSelectableDescription() override       { return "Live Session"; }
    int getNumOutputChannelsGivenInputs (int n) override   { return juce::jmin (n, 2); }
    BusLayout getBusses() const override                   { return BusLayout::singleStereoInOut(); }
    bool producesAudioWhenNoAudioInput() override          { return true; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        laneScratch.setSize (maxLanes * 2, juce::jmax (info.blockSizeSamples, 4096));
    }

    void deinitialise() override {}

    //==============================================================================
    // Recording each collaborator onto their own track (what you hear from them)
    static constexpr int maxLanes = 16;

    struct LaneResult { double startSeconds = -1.0; juce::int64 samples = 0; };

    /** Starts writing a player's audio to a file (message thread). */
    void startLane (int peer, std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer)
    {
        if (! juce::isPositiveAndBelow (peer, maxLanes))
            return;

        auto& lane = lanes[(size_t) peer];
        const juce::SpinLock::ScopedLockType sl (lane.lock);
        lane.writer = std::move (writer);
        lane.startSeconds = -1.0;
        lane.written = 0;
        anyLaneActive = true;
    }

    /** Stops a lane; the file is complete when this returns (message thread). */
    LaneResult stopLane (int peer)
    {
        if (! juce::isPositiveAndBelow (peer, maxLanes))
            return {};

        auto& lane = lanes[(size_t) peer];
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> old;
        LaneResult result;
        {
            const juce::SpinLock::ScopedLockType sl (lane.lock);
            old = std::move (lane.writer);
            result = { lane.startSeconds, lane.written };
        }
        old.reset();    // flushes and closes the file

        bool any = false;
        for (auto& l : lanes) { const juce::SpinLock::ScopedLockType sl (l.lock); any = any || l.writer != nullptr; }
        anyLaneActive = any;
        return result;
    }

    //==============================================================================
    // Message-thread side
    struct Capture
    {
        juce::AudioBuffer<float> buffer;
        std::atomic<int> state { 0 };        // 0 free, 1 being written, 2 finished
        juce::int64 interval = -1;
        int startOffset = 0, numSamples = 0;
        double sampleRate = 0.0;
    };

    static constexpr int numCaptures = 4, maxRemote = 24;

    /** Sets tempo and interval length; (re)allocates capture buffers. Call while not in a hurry. */
    void configure (double bpm, double beatsPerInterval)
    {
        auto sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
        auto intervalSamples = (int) std::ceil (beatsPerInterval * 60.0 / bpm * sr) + 4096;

        std::array<juce::AudioBuffer<float>, numCaptures> fresh;
        for (auto& b : fresh)
            b.setSize (2, intervalSamples);

        const juce::SpinLock::ScopedLockType sl (lock);
        tempoBpm = bpm;
        intervalBeats = beatsPerInterval;
        current = nullptr;

        for (int i = 0; i < numCaptures; ++i)
        {
            std::swap (captures[(size_t) i].buffer, fresh[(size_t) i]);
            captures[(size_t) i].state = 0;
        }

        configured = true;
    }

    Capture* getFinishedCapture()
    {
        for (auto& c : captures)
            if (c.state == 2)
                return &c;

        return nullptr;
    }

    void releaseCapture (Capture& c)    { c.state = 0; }

    struct RemoteAudio
    {
        int peer = -1;
        juce::int64 interval = -1;
        int startOffset = 0;
        std::shared_ptr<juce::AudioBuffer<float>> buffer;
    };

    void addRemote (RemoteAudio r)
    {
        const juce::SpinLock::ScopedLockType sl (lock);

        // Replace the oldest slot for this peer (or an empty one)
        int target = -1;
        juce::int64 oldest = std::numeric_limits<juce::int64>::max();

        for (int i = 0; i < maxRemote; ++i)
        {
            auto& s = remote[(size_t) i];

            if (s.buffer == nullptr) { target = i; break; }
            if (s.interval < oldest) { oldest = s.interval; target = i; }
        }

        remote[(size_t) target] = std::move (r);
    }

    void setPeerGain (int peer, float gain)
    {
        if (juce::isPositiveAndBelow (peer, (int) peerGains.size()))
            peerGains[(size_t) peer] = gain;
    }

    float getPeerLevel (int peer)
    {
        if (! juce::isPositiveAndBelow (peer, (int) peerLevels.size()))
            return 0.0f;

        return peerLevels[(size_t) peer].exchange (0.0f);
    }

    juce::int64 getCurrentInterval() const   { return currentInterval; }
    double getIntervalProgress() const       { return intervalProgress; }

    std::array<std::atomic<float>, 16> peerGains {}, peerLevels {};

    //==============================================================================
    // Real-time (low-latency) mode: continuous streams instead of intervals.
    // One lock-free FIFO carries our output to the sender thread; one FIFO per
    // collaborator is filled by the network thread and drained here. Each of those
    // is a jitter buffer: playback waits until it holds `targetSamples`, and if it
    // drifts far above that, the excess is skipped so latency can't creep up.
    std::atomic<bool> realtime { false };
    static constexpr int rtCapacity = Realtime::capacity;

    using Stream = Realtime::JitterBuffer;       // shared with Somewhere monitoring

    juce::AbstractFifo sendFifo { rtCapacity };
    juce::AudioBuffer<float> sendBuffer { 2, rtCapacity };
    std::array<Stream, 16> streams;

    /** Sender thread: takes up to `max` samples of our output. */
    int popSend (juce::AudioBuffer<float>& dest, int max)
    {
        int s1, n1, s2, n2;
        sendFifo.prepareToRead (juce::jmin (max, sendFifo.getNumReady()), s1, n1, s2, n2);

        for (int ch = 0; ch < 2; ++ch)
        {
            dest.copyFrom (ch, 0, sendBuffer, ch, s1, n1);
            if (n2 > 0) dest.copyFrom (ch, n1, sendBuffer, ch, s2, n2);
        }

        sendFifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

    /** The interface's rate. (The plug-in's own isn't set until the engine has put it into the
        playback graph, which is after a session starts: until then it reads 44100.) */
    double getSampleRate() const
    {
        auto rate = edit.engine.getDeviceManager().getSampleRate();
        return rate > 0.0 ? rate : sampleRate;
    }

    //==============================================================================
    // Audio thread

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        recordingThisBlock = anyLaneActive.load() && fc.isPlaying && ! fc.isRendering
                             && fc.bufferNumSamples <= laneScratch.getNumSamples();
        if (recordingThisBlock)
            laneScratch.clear (0, fc.bufferNumSamples);

        applyToBufferInternal (fc);

        if (recordingThisBlock)
            writeLanes (fc.editTime.getStart().inSeconds(), fc.bufferNumSamples);
    }

    void applyToBufferInternal (const te::PluginRenderContext& fc)
    {
        if (fc.destBuffer != nullptr && realtime)
        {
            // An export or bounce passes through untouched: nobody's live playing goes into the
            // file, and the render (faster than real time) isn't sent to the others
            if (fc.isRendering)
                return;

            return processRealtime (*fc.destBuffer, fc.bufferStartSample, fc.bufferNumSamples);
        }

        if (fc.destBuffer == nullptr || ! configured)
            return;

        const juce::SpinLock::ScopedTryLockType sl (lock);

        if (! sl.isLocked())
            return;

        if (! fc.isPlaying || fc.isRendering)
        {
            finishCurrent();
            return;
        }

        const double sr = sampleRate;
        const double samplesPerBeat = 60.0 / tempoBpm * sr;
        const auto intervalLength = (juce::int64) std::llround (intervalBeats * samplesPerBeat);

        if (intervalLength <= 0)
            return;

        const double startBeat = fc.editTime.getStart().inSeconds() * tempoBpm / 60.0;
        auto k = (juce::int64) std::floor (startBeat / intervalBeats);
        auto pos = (juce::int64) std::llround ((startBeat - (double) k * intervalBeats) * samplesPerBeat);

        auto& buffer = *fc.destBuffer;
        const int numChannels = juce::jmin (2, buffer.getNumChannels());
        int done = 0;

        while (done < fc.bufferNumSamples)
        {
            if (pos >= intervalLength)
            {
                finishCurrent();
                ++k;
                pos -= intervalLength;
            }

            const int n = (int) juce::jmin ((juce::int64) (fc.bufferNumSamples - done), intervalLength - pos);
            const int start = fc.bufferStartSample + done;

            // 1. Capture what we're playing (before adding anyone else's audio)
            if (current == nullptr || current->interval != k || current->startOffset + current->numSamples != (int) pos)
            {
                finishCurrent();
                startCapture (k, (int) pos);
            }

            if (current != nullptr)
            {
                auto space = current->buffer.getNumSamples() - current->numSamples;
                auto toCopy = juce::jmin (n, space);

                for (int ch = 0; ch < 2; ++ch)
                    current->buffer.copyFrom (ch, current->numSamples, buffer, juce::jmin (ch, numChannels - 1), start, toCopy);

                current->numSamples += toCopy;
            }

            // 2. Mix in collaborators' previous interval, lined up to our bars
            for (auto& r : remote)
            {
                if (r.buffer == nullptr || r.interval != k - 1)
                    continue;

                auto srcStart = (int) pos - r.startOffset;
                auto available = r.buffer->getNumSamples() - srcStart;
                auto skip = juce::jmax (0, -srcStart);
                auto count = juce::jmin (n - skip, available - skip);

                if (count <= 0)
                    continue;

                auto gain = juce::isPositiveAndBelow (r.peer, (int) peerGains.size()) ? peerGains[(size_t) r.peer].load() : 1.0f;

                for (int ch = 0; ch < numChannels; ++ch)
                    buffer.addFrom (ch, start + skip, *r.buffer, juce::jmin (ch, r.buffer->getNumChannels() - 1), srcStart + skip, count, gain);

                if (recordingThisBlock && juce::isPositiveAndBelow (r.peer, maxLanes))
                    for (int ch = 0; ch < 2; ++ch)
                        laneScratch.addFrom (r.peer * 2 + ch, done + skip, *r.buffer, juce::jmin (ch, r.buffer->getNumChannels() - 1), srcStart + skip, count);

                if (juce::isPositiveAndBelow (r.peer, (int) peerLevels.size()))
                {
                    auto peak = r.buffer->getMagnitude (srcStart + skip, count) * gain;
                    auto& lvl = peerLevels[(size_t) r.peer];
                    lvl = juce::jmax (lvl.load(), peak);
                }
            }

            done += n;
            pos += n;
        }

        currentInterval = k;
        intervalProgress = (double) pos / (double) intervalLength;
    }

private:
    struct RecordLane
    {
        juce::SpinLock lock;
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        double startSeconds = -1.0;
        juce::int64 written = 0;
    };

    std::array<RecordLane, maxLanes> lanes;
    juce::AudioBuffer<float> laneScratch;
    std::atomic<bool> anyLaneActive { false };
    bool recordingThisBlock = false;

    void writeLanes (double blockStartSeconds, int n)
    {
        for (int i = 0; i < maxLanes; ++i)
        {
            auto& lane = lanes[(size_t) i];
            const juce::SpinLock::ScopedTryLockType sl (lane.lock);

            if (! sl.isLocked() || lane.writer == nullptr)
                continue;

            if (lane.startSeconds < 0.0)
                lane.startSeconds = blockStartSeconds;

            const float* chans[] = { laneScratch.getReadPointer (i * 2), laneScratch.getReadPointer (i * 2 + 1) };
            if (lane.writer->write (chans, n))
                lane.written += n;
        }
    }

    juce::SpinLock lock;
    std::array<Capture, numCaptures> captures;
    std::array<RemoteAudio, maxRemote> remote;
    Capture* current = nullptr;
    double tempoBpm = 120.0, intervalBeats = 16.0;   // (sampleRate comes from te::Plugin)
    std::atomic<bool> configured { false };
    std::atomic<juce::int64> currentInterval { -1 };
    std::atomic<double> intervalProgress { 0.0 };

    void processRealtime (juce::AudioBuffer<float>& buffer, int start, int n)
    {
        const int numChannels = juce::jmin (2, buffer.getNumChannels());

        // Our output -> sender thread (dropped if it has fallen behind)
        {
            int s1, n1, s2, n2;
            sendFifo.prepareToWrite (n, s1, n1, s2, n2);

            for (int ch = 0; ch < 2; ++ch)
            {
                auto src = juce::jmin (ch, numChannels - 1);
                sendBuffer.copyFrom (ch, s1, buffer, src, start, n1);
                if (n2 > 0) sendBuffer.copyFrom (ch, s2, buffer, src, start + n1, n2);
            }

            sendFifo.finishedWrite (n1 + n2);
        }

        // Everyone else, through their jitter buffers
        for (size_t i = 0; i < streams.size(); ++i)
        {
            auto& st = streams[i];

            if (! st.active)
                continue;

            auto ready = st.fifo.getNumReady();
            auto target = st.targetSamples.load();

            if (! st.primed)
            {
                if (ready < target)
                    continue;

                st.primed = true;
                st.resuming = true;
            }

            // Too much queued (e.g. after a network hiccup): skip ahead to keep latency low
            if (ready > target * 2 + 2048)
            {
                st.fifo.finishedRead (ready - target);
                ready = target;
            }

            auto toRead = juce::jmin (n, ready);
            auto gain = peerGains[i].load();
            int s1, n1, s2, n2;
            st.fifo.prepareToRead (toRead, s1, n1, s2, n2);

            // Running dry fades out and starting again fades in (about 1.5 ms), so a network
            // hiccup is a dip, not a click
            const bool runningDry = toRead < n;
            const int fade = juce::jmin (64, toRead);

            auto add = [&] (int from, int count, float g0, float g1)     // a stretch of what's being read
            {
                for (int ch = 0; ch < numChannels && count > 0; ++ch)
                {
                    const int first = juce::jlimit (0, count, n1 - from);     // how much of it is before the wrap

                    if (first > 0)
                        buffer.addFromWithRamp (ch, start + from, st.buffer.getReadPointer (ch, s1 + from), first,
                                                g0, g0 + (g1 - g0) * (float) first / (float) count);

                    if (count > first)
                        buffer.addFromWithRamp (ch, start + from + first, st.buffer.getReadPointer (ch, s2 + from + first - n1), count - first,
                                                g0 + (g1 - g0) * (float) first / (float) count, g1);
                }
            };

            if (st.resuming || runningDry)
            {
                const int in = st.resuming ? fade : 0, out = runningDry ? juce::jmin (fade, toRead - in) : 0;
                add (0, in, 0.0f, gain);
                add (in, toRead - in - out, gain, gain);
                add (toRead - out, out, gain, 0.0f);
            }
            else
            {
                add (0, toRead, gain, gain);
            }

            st.resuming = false;

            if (recordingThisBlock && (int) i < maxLanes)
                for (int ch = 0; ch < 2; ++ch)
                {
                    laneScratch.addFrom ((int) i * 2 + ch, 0, st.buffer, ch, s1, n1);
                    if (n2 > 0) laneScratch.addFrom ((int) i * 2 + ch, n1, st.buffer, ch, s2, n2);
                }

            auto peak = st.buffer.getMagnitude (s1, n1) * gain;
            peerLevels[i] = juce::jmax (peerLevels[i].load(), peak);
            st.fifo.finishedRead (n1 + n2);

            if (runningDry)
            {
                ++st.underruns;      // ran dry: wait to refill before playing again
                st.primed = false;
            }
        }
    }

    void startCapture (juce::int64 k, int pos)
    {
        current = nullptr;

        for (auto& c : captures)
        {
            if (c.state == 0)
            {
                c.interval = k;
                c.startOffset = pos;
                c.numSamples = 0;
                c.sampleRate = sampleRate;
                c.state = 1;
                current = &c;
                return;
            }
        }
        // All slots busy (encoder is behind): skip this interval
    }

    void finishCurrent()
    {
        if (current != nullptr)
        {
            current->state = current->numSamples > 0 ? 2 : 0;
            current = nullptr;
        }
    }
};

const char* LiveTapPlugin::xmlTypeName = tapPluginType;

te::Plugin::Ptr createTapPlugin (const te::PluginCreationInfo& info)
{
    return new LiveTapPlugin (info);
}

void removeTapPlugins (te::Edit& edit)
{
    for (auto p : edit.getMasterPluginList().getPlugins())
        if (p->getPluginType() == tapPluginType)
            p->deleteFromParent();
}

//==============================================================================
// Wire format (binary data-channel messages; first byte is the type)
enum MessageType : uint8_t { helloMsg = 1, chunkMsg = 2, pingMsg = 3, pongMsg = 4, audioPacketMsg = 5, launcherMsg = 6 };

inline const juce::Identifier liveModeProp { "elsewhereLiveMode" };
static constexpr int opusRate = 48000, opusFrame = 240;   // 5 ms frames

//==============================================================================
/** Streaming resampler (device rate <-> 48 kHz); see RealtimeAudio.h. */
using StreamResampler = Realtime::Resampler;

//==============================================================================
/** Keeps the tap plugin (which owns the jitter buffers) alive. A base class listed before the
    Receiver, so it's released only after the Receiver has finished with the buffer. */
struct TapHolder
{
    te::Plugin::Ptr tap;
};

/** Decodes one collaborator's real-time stream into their jitter buffer. Runs on the network thread. */
struct RealtimeReceiver : TapHolder, Realtime::Receiver
{
    RealtimeReceiver (LiveTapPlugin& p, int index, double deviceRate)
        : TapHolder { &p },
          // Test aid: pretend the other player's clock runs fast/slow by ELSEWHERE_TEST_DRIFT_PPM
          Realtime::Receiver (p.streams[(size_t) index], opusRate, deviceRate,
                              juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_DRIFT_PPM", "0").getDoubleValue())
    {
        int err = 0;
        decoder = opus_decoder_create (opusRate, 2, &err);
        correctDrift = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_NO_DRIFT_FIX", {}).isEmpty();
    }

    ~RealtimeReceiver() override
    {
        if (decoder != nullptr) opus_decoder_destroy (decoder);
    }

    void handlePacket (const std::byte* data, size_t size)
    {
        if (size < 4 || decoder == nullptr)
            return;

        uint32_t seq;
        std::memcpy (&seq, data, 4);

        if (haveSeq)
        {
            auto gap = (int32_t) (seq - expected);

            if (gap < -500)
                gap = 0;   // far behind: they started sending again, counting from zero
            else if (gap < 0)
                return;    // late duplicate / reordered: too late to use

            // Lost packets: let Opus conceal them (up to a few)
            for (int i = 0; i < juce::jmin (gap, 3); ++i)
                decode (nullptr, 0);
        }

        haveSeq = true;
        expected = seq + 1;
        decode (reinterpret_cast<const unsigned char*> (data + 4), (int) size - 4);
    }

    void decode (const unsigned char* packet, int size)
    {
        float pcm[opusFrame * 2];
        auto n = opus_decode_float (decoder, packet, size, pcm, opusFrame, 0);

        if (n <= 0)
            return;

        float l[opusFrame], r[opusFrame];
        for (int i = 0; i < n; ++i) { l[i] = pcm[i * 2]; r[i] = pcm[i * 2 + 1]; }

        add (l, r, n, (double) opusFrame / opusRate);
    }

    OpusDecoder* decoder = nullptr;
    uint32_t expected = 0;
    bool haveSeq = false;
};
static constexpr size_t chunkSize = 16 * 1024;

static rtc::binary makeMessage (MessageType type, const void* header, size_t headerSize, const void* payload = nullptr, size_t payloadSize = 0)
{
    rtc::binary b (1 + headerSize + payloadSize);
    b[0] = (std::byte) type;
    if (headerSize > 0)  std::memcpy (b.data() + 1, header, headerSize);
    if (payloadSize > 0) std::memcpy (b.data() + 1 + headerSize, payload, payloadSize);
    return b;
}

#pragma pack (push, 1)
struct ChunkHeader
{
    int64_t interval;
    int32_t startOffset;
    int32_t sampleRate;
    uint16_t index, count;
};
#pragma pack (pop)

//==============================================================================
struct Session::Impl : private juce::Timer
{
    Impl (Project& p, Collab::Session& c) : project (p), edit (p.getEdit()), collab (c) {}

    ~Impl() override
    {
        stop();
    }

    //==============================================================================
    struct Peer
    {
        juce::String userId, name;
        juce::Colour colour;
        int index = 0;                                  // slot in the plugin's gain/level arrays
        bool isOfferer = false;
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> dc, rtDc;          // reliable control/interval channel, real-time audio channel
        /** The receiver the network thread hands audio packets to (swapped by the message thread). */
        struct RxSlot
        {
            juce::SpinLock lock;
            std::shared_ptr<RealtimeReceiver> rx;

            void set (std::shared_ptr<RealtimeReceiver> r)  { const juce::SpinLock::ScopedLockType sl (lock); rx = std::move (r); }
            std::shared_ptr<RealtimeReceiver> get()         { const juce::SpinLock::ScopedLockType sl (lock); return rx; }
        };

        std::shared_ptr<RxSlot> rxSlot = std::make_shared<RxSlot>();
        int lastUnderruns = 0, stableSeconds = 0;
        juce::String theirMode;
        juce::String nonce, handledNonce;
        juce::int64 attemptStarted = 0;
        bool answerApplied = false, connected = false;
        juce::String state = "Waiting...";
        double rttMs = 0.0, theirBpm = 0.0;
        juce::int64 lastInterval = -1;
        float gain = 1.0f;
        bool muted = false;

        struct Incoming { std::vector<juce::MemoryBlock> chunks; int received = 0; int32_t startOffset = 0, sampleRate = 0; };
        std::map<juce::int64, Incoming> incoming;
    };

    Project& project;
    te::Edit& edit;
    Collab::Session& collab;
    bool active = false;
    LiveTapPlugin* tap = nullptr;
    te::Plugin::Ptr tapRef;
    std::map<juce::String, std::shared_ptr<Peer>> peers;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    juce::ThreadPool pool { 2 };
    int tick = 0, intervalsSent = 0, intervalsReceived = 0, nextPeerIndex = 0;
    double configuredBpm = 0.0, configuredBeats = 0.0, configuredRate = 0.0, modeRate = 0.0;
    std::function<void()> onPeersChanged;
    std::function<void (const juce::String&, const juce::var&)> onLauncherAction;

    //==============================================================================
    // Recording the session: while you record, each player's audio (as you hear it) goes
    // to its own file; when you stop, each becomes a track lined up on the timeline
    struct LiveTake { juce::String name; juce::Colour colour; juce::File file; };
    std::map<int, LiveTake> takes;
    juce::TimeSliceThread recordThread { "Elsewhere live recorder" };

    void updateRecording()
    {
        const bool shouldRecord = tap != nullptr && edit.getTransport().isRecording();

        if (shouldRecord)
        {
            for (auto& [id, p] : peers)
                if (p->connected && takes.count (p->index) == 0)
                    startTake (*p);
        }
        else if (! takes.empty())
        {
            finishTakes();
        }
    }

    void startTake (Peer& p)
    {
        auto folder = project.getFolder().getChildFile ("Samples/Live");
        folder.createDirectory();
        auto base = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S ") + juce::File::createLegalFileName (p.name.isNotEmpty() ? p.name : juce::String ("Player"));
        auto file = folder.getNonexistentChildFile (base, ".wav", false);

        std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
        if (stream == nullptr)
            return;

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (tap->getSampleRate())
                                                                                       .withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr)
            return;

        if (! recordThread.isThreadRunning())
            recordThread.startThread();

        // ~5 s of buffering between the audio thread and the disk
        tap->startLane (p.index, std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), recordThread,
                                                                                             juce::roundToInt (tap->getSampleRate() * 5.0)));
        takes[p.index] = { p.name.isNotEmpty() ? p.name : juce::String ("Player"), p.colour, file };
    }

    void finishTakes()
    {
        for (auto& [index, take] : takes)
        {
            auto result = tap != nullptr ? tap->stopLane (index) : LiveTapPlugin::LaneResult();
            const double rate = tap != nullptr ? tap->getSampleRate() : 44100.0;
            const double seconds = (double) result.samples / rate;

            if (result.startSeconds < 0.0 || seconds < 0.5)
            {
                take.file.deleteFile();
                continue;
            }

            auto tracks = te::getAudioTracks (edit);
            auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, tracks.getLast()), nullptr);
            if (track == nullptr)
                continue;

            track->setName (take.name + " (live)");
            if (! take.colour.isTransparent()) track->setColour (take.colour);
            track->getClipSlotList().ensureNumberOfSlots (edit.getSceneList().getScenes().size());

            const auto start = te::TimePosition::fromSeconds (result.startSeconds);
            track->insertWaveClip (take.name + " live", take.file, { { start, start + te::TimeDuration::fromSeconds (seconds) }, {} }, false);
            std::cout << "LIVE-REC " << take.name << " " << juce::String (seconds, 1) << "s at " << juce::String (result.startSeconds, 2) << "s" << std::endl;
        }

        takes.clear();
    }

    void sendLauncherAction (const juce::var& v)
    {
        auto text = juce::JSON::toString (v, true).toStdString();
        for (auto& [id, p] : peers)
            if (p->connected && p->dc && p->dc->isOpen())
                try { p->dc->send (makeMessage (launcherMsg, text.data(), text.size())); } catch (...) {}
    }

    juce::File getFolder() const    { return collab.getFolder().getChildFile ("live"); }

    //==============================================================================
    // Real-time mode
    bool isRealtime() const         { return edit.state[liveModeProp].toString() == "realtime"; }

    struct Sender : public juce::Thread
    {
        Sender (Impl& o) : juce::Thread ("Elsewhere live sender"), owner (o)
        {
            int err = 0;
            encoder = opus_encoder_create (opusRate, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
            opus_encoder_ctl (encoder, OPUS_SET_BITRATE (128000));
            opus_encoder_ctl (encoder, OPUS_SET_COMPLEXITY (5));
        }

        ~Sender() override
        {
            stopThread (1000);
            opus_encoder_destroy (encoder);
        }

        void run() override
        {
            auto* livePlugin = owner.tap;
            resampler.setRates (livePlugin->getSampleRate() > 0 ? livePlugin->getSampleRate() : opusRate, opusRate);
            juce::AudioBuffer<float> in (2, 4096), frame (2, opusFrame);
            int frameFill = 0;
            uint32_t seq = 0;

            while (! threadShouldExit())
            {
                auto got = livePlugin->popSend (in, 4096);

                if (got == 0)
                {
                    wait (1);
                    continue;
                }

                auto feed = [&] (const float* l, const float* r, int n)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        frame.setSample (0, frameFill, l[i]);
                        frame.setSample (1, frameFill, r[i]);

                        if (++frameFill == opusFrame)
                        {
                            sendFrame (frame, seq++);
                            frameFill = 0;
                        }
                    }
                };

                if (resampler.isPassThrough())
                {
                    feed (in.getReadPointer (0), in.getReadPointer (1), got);
                }
                else
                {
                    resampler.addInput (in.getReadPointer (0), in.getReadPointer (1), got);
                    float l[512], r[512];

                    while (auto n = resampler.produce (l, r, 256))
                        feed (l, r, n);
                }
            }
        }

        void sendFrame (const juce::AudioBuffer<float>& frame, uint32_t seq)
        {
            float interleaved[opusFrame * 2];
            for (int i = 0; i < opusFrame; ++i) { interleaved[i * 2] = frame.getSample (0, i); interleaved[i * 2 + 1] = frame.getSample (1, i); }

            unsigned char packet[1500];
            std::memcpy (packet, &seq, 4);
            auto bytes = opus_encode_float (encoder, interleaved, opusFrame, packet + 4, (opus_int32) sizeof (packet) - 4);

            if (bytes <= 0)
                return;

            auto msg = makeMessage (audioPacketMsg, packet, (size_t) bytes + 4);
            std::vector<std::shared_ptr<rtc::DataChannel>> channels;

            {
                const juce::ScopedLock sl (owner.rtChannelsLock);
                channels = owner.rtChannels;
            }

            for (auto& c : channels)
                if (c->isOpen())
                    try { c->send (msg); } catch (...) {}
        }

        Impl& owner;
        OpusEncoder* encoder = nullptr;
        StreamResampler resampler;
    };

    std::unique_ptr<Sender> sender;
    juce::CriticalSection rtChannelsLock;
    std::vector<std::shared_ptr<rtc::DataChannel>> rtChannels;

    void updateRealtimeChannels()
    {
        std::vector<std::shared_ptr<rtc::DataChannel>> channels;

        for (auto& [id, p] : peers)
            if (p->connected && p->rtDc != nullptr)
                channels.push_back (p->rtDc);

        const juce::ScopedLock sl (rtChannelsLock);
        rtChannels = std::move (channels);
    }

    /** A mixer slot nobody present is using (each has its own jitter buffer, gain and meter), with
        the gain a new arrival should start at. */
    int freePeerIndex()
    {
        for (int tries = 0; tries < 16; ++tries)
        {
            auto index = nextPeerIndex++ % 16;
            bool taken = false;

            for (auto& [id, p] : peers)
                taken = taken || p->index == index;

            if (! taken)
            {
                if (tap != nullptr)
                    tap->setPeerGain (index, 1.0f);

                return index;
            }
        }

        return nextPeerIndex++ % 16;    // more than 16 people: slots are shared
    }

    /** Starts or stops the real-time path to match the project's live mode. */
    void applyMode()
    {
        if (tap == nullptr)
            return;

        bool wantRealtime = isRealtime();

        // (The interface's rate too: the sender and receivers convert to and from it)
        auto rate = tap->getSampleRate();

        if (wantRealtime == tap->realtime.load() && (sender != nullptr) == wantRealtime
             && (! wantRealtime || juce::approximatelyEqual (rate, modeRate)))
            return;

        modeRate = rate;
        sender.reset();
        tap->realtime = wantRealtime;

        for (auto& [id, p] : peers)
            p->rxSlot->set (nullptr);

        if (wantRealtime)
        {
            for (auto& [id, p] : peers)
                if (p->connected)
                    startReceiver (*p);

            tap->sendFifo.reset();
            sender = std::make_unique<Sender> (*this);
            sender->startThread (juce::Thread::Priority::highest);
        }
    }

    void startReceiver (Peer& p)
    {
        if (tap != nullptr && tap->realtime)
            p.rxSlot->set (std::make_shared<RealtimeReceiver> (*tap, p.index, edit.engine.getDeviceManager().getSampleRate()));
    }

    /** Grows a jitter buffer when it runs dry, shrinks it slowly when things are steady. */
    void adaptJitterBuffers()
    {
        if (tap == nullptr || ! tap->realtime)
            return;

        auto sr = tap->getSampleRate() > 0 ? tap->getSampleRate() : 48000.0;

        for (auto& [id, p] : peers)
            tap->streams[(size_t) p->index].adapt (p->lastUnderruns, p->stableSeconds, sr);
    }
    juce::String myId() const       { return collab.getIdentity().userId; }

    //==============================================================================
    void start()
    {
        if (active)
            return;

        static bool loggerStarted = false;
        if (! loggerStarted) { rtc::InitLogger (rtc::LogLevel::Warning); loggerStarted = true; }

        getFolder().createDirectory();
        removeTapPlugins (edit);

        // (Custom plugin types have to be created from a state tree: creating by type name
        // only knows the engine's built-in plugins)
        juce::ValueTree tapState (te::IDs::PLUGIN);
        tapState.setProperty (te::IDs::type, tapPluginType, nullptr);

        if (auto p = edit.getPluginCache().createNewPlugin (tapState))
        {
            // Before the master meter, so the meter shows what you hear
            auto& list = edit.getMasterPluginList();
            int index = list.size();

            for (int i = 0; i < list.size(); ++i)
                if (dynamic_cast<te::LevelMeterPlugin*> (list[i]) != nullptr)
                    index = i;

            list.insertPlugin (p, index, nullptr);
            tapRef = p;
            tap = dynamic_cast<LiveTapPlugin*> (p.get());
        }

        active = true;
        reconfigureIfNeeded();
        applyMode();
        writePresence();
        startTimer (50);
    }

    void stop()
    {
        if (! active)
            return;

        finishTakes();
        stopTimer();
        sender.reset();
        *alive = false;
        alive = std::make_shared<std::atomic<bool>> (true);

        for (auto& [id, peer] : peers)
            closePeer (*peer);

        peers.clear();
        updateRealtimeChannels();
        pool.removeAllJobs (true, 2000);

        if (tapRef != nullptr)
            tapRef->deleteFromParent();

        tap = nullptr;
        tapRef = nullptr;

        // Tidy our signalling files
        for (auto f : getFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
            if (f.getFileName().contains (myId()))
                f.deleteFile();

        active = false;
    }

    //==============================================================================
    double getBpm() const               { return edit.tempoSequence.getTempo (0)->getBpm(); }

    int getBarsPerInterval() const
    {
        auto bars = (int) edit.state.getProperty (barsPerIntervalProp, 4);
        return juce::jlimit (1, 16, bars);
    }

    double getBeatsPerInterval() const
    {
        auto* ts = edit.tempoSequence.getTimeSig (0);
        auto beatsPerBar = ts->numerator.get() * 4.0 / ts->denominator.get();
        return getBarsPerInterval() * beatsPerBar;
    }

    void reconfigureIfNeeded()
    {
        if (tap == nullptr)
            return;

        auto bpm = getBpm(), beats = getBeatsPerInterval();

        auto rate = tap->getSampleRate();

        // (The rate as well: an interval is a different number of samples at another rate)
        if (! juce::approximatelyEqual (bpm, configuredBpm) || ! juce::approximatelyEqual (beats, configuredBeats)
             || ! juce::approximatelyEqual (rate, configuredRate))
        {
            tap->configure (bpm, beats);
            configuredBpm = bpm;
            configuredBeats = beats;
            configuredRate = rate;
        }
    }

    //==============================================================================
    void timerCallback() override
    {
        ++tick;
        collectCaptures();
        updateRecording();

        if (tick % 20 == 0)     // every second
        {
            applyMode();
            adaptJitterBuffers();
            reconfigureIfNeeded();
            updateSignalling();
            sendPings();
        }

        if (tick % 60 == 0)
            writePresence();
    }

    //==============================================================================
    // Sending our intervals
    void collectCaptures()
    {
        if (tap == nullptr)
            return;

        while (auto* c = tap->getFinishedCapture())
        {
            auto audio = std::make_shared<juce::AudioBuffer<float>> (2, c->numSamples);
            for (int ch = 0; ch < 2; ++ch)
                audio->copyFrom (ch, 0, c->buffer, ch, 0, c->numSamples);

            auto interval = c->interval;
            auto startOffset = c->startOffset;
            auto sr = c->sampleRate;
            tap->releaseCapture (*c);

            // Skip intervals that are pure silence (e.g. nothing playing yet)
            if (audio->getMagnitude (0, audio->getNumSamples()) < 1.0e-5f || ! hasConnectedPeer())
                continue;

            auto aliveToken = alive;
            pool.addJob ([this, aliveToken, audio, interval, startOffset, sr]
            {
                auto encoded = encode (*audio, sr);

                juce::MessageManager::callAsync ([this, aliveToken, encoded, interval, startOffset, sr]
                {
                    if (*aliveToken)
                        sendInterval (encoded, interval, startOffset, (int) sr);
                });
            });
        }
    }

    static juce::MemoryBlock encode (const juce::AudioBuffer<float>& audio, double sampleRate)
    {
        juce::MemoryBlock result;
        juce::OggVorbisAudioFormat ogg;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (result, false);

        // Quality index 4 of Vorbis's options is roughly 128 kb/s for stereo
        auto options = juce::AudioFormatWriterOptions().withSampleRate (sampleRate).withNumChannels (2)
                                                       .withBitsPerSample (16).withQualityOptionIndex (4);

        if (auto writer = ogg.createWriterFor (stream, options))
        {
            writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
            writer.reset();   // flushes into result
        }

        return result;
    }

    void sendInterval (const juce::MemoryBlock& data, juce::int64 interval, int startOffset, int sampleRate)
    {
        const auto count = (uint16_t) ((data.getSize() + chunkSize - 1) / chunkSize);

        for (auto& [id, peer] : peers)
        {
            if (! peer->connected || peer->dc == nullptr || ! peer->dc->isOpen())
                continue;

            for (uint16_t i = 0; i < count; ++i)
            {
                ChunkHeader h { interval, startOffset, sampleRate, i, count };
                auto offset = (size_t) i * chunkSize;
                auto size = juce::jmin (chunkSize, data.getSize() - offset);

                try { peer->dc->send (makeMessage (chunkMsg, &h, sizeof (h), (const char*) data.getData() + offset, size)); }
                catch (...) {}
            }
        }

        ++intervalsSent;
    }

    bool hasConnectedPeer() const
    {
        for (auto& [id, p] : peers)
            if (p->connected)
                return true;

        return false;
    }

    //==============================================================================
    // Receiving
    void handleMessage (Peer& peer, const rtc::binary& msg)
    {
        if (msg.empty())
            return;

        auto type = (MessageType) msg[0];
        auto* body = reinterpret_cast<const char*> (msg.data() + 1);
        auto bodySize = msg.size() - 1;

        if (type == helloMsg)
        {
            auto json = juce::JSON::parse (juce::String::fromUTF8 (body, (int) bodySize));
            peer.name = json["name"].toString();
            peer.colour = juce::Colour::fromString (json["colour"].toString());
            peer.theirBpm = (double) json["bpm"];
            peer.theirMode = json["mode"].toString();
            notifyPeersChanged();
        }
        else if (type == launcherMsg)
        {
            auto json = juce::JSON::parse (juce::String::fromUTF8 (body, (int) bodySize));
            if (onLauncherAction && json.isObject())
                onLauncherAction (peer.name, json);
        }
        else if (type == pingMsg && bodySize >= sizeof (double))
        {
            if (peer.dc && peer.dc->isOpen())
                try { peer.dc->send (makeMessage (pongMsg, body, sizeof (double))); } catch (...) {}
        }
        else if (type == pongMsg && bodySize >= sizeof (double))
        {
            double sent;
            std::memcpy (&sent, body, sizeof (double));
            peer.rttMs = juce::Time::getMillisecondCounterHiRes() - sent;
        }
        else if (type == chunkMsg && bodySize >= sizeof (ChunkHeader))
        {
            ChunkHeader h;
            std::memcpy (&h, body, sizeof (h));
            auto& in = peer.incoming[h.interval];

            if (in.chunks.size() != h.count)
                in.chunks.assign (h.count, {});

            if (h.index < h.count && in.chunks[h.index].isEmpty())
            {
                in.chunks[h.index].append (body + sizeof (h), bodySize - sizeof (h));
                in.startOffset = h.startOffset;
                in.sampleRate = h.sampleRate;

                if (++in.received == h.count)
                {
                    juce::MemoryBlock whole;
                    for (auto& c : in.chunks)
                        whole.append (c.getData(), c.getSize());

                    // (from the header: erasing frees what `in` refers to)
                    peer.incoming.erase (h.interval);
                    decodeAndInstall (peer, std::move (whole), h.interval, h.startOffset, h.sampleRate);
                }
            }

            // Forget partial intervals that are long gone
            for (auto it = peer.incoming.begin(); it != peer.incoming.end();)
                it = (it->first < h.interval - 4) ? peer.incoming.erase (it) : std::next (it);
        }
    }

    void decodeAndInstall (Peer& peer, juce::MemoryBlock data, juce::int64 interval, int startOffset, int theirRate)
    {
        auto aliveToken = alive;
        auto peerIndex = peer.index;
        auto userId = peer.userId;
        auto ourRate = tap != nullptr ? edit.engine.getDeviceManager().getSampleRate() : 0.0;

        pool.addJob ([this, aliveToken, encoded = std::move (data), interval, startOffset, theirRate, peerIndex, userId, ourRate]
        {
            juce::OggVorbisAudioFormat ogg;
            std::unique_ptr<juce::AudioFormatReader> reader (ogg.createReaderFor (new juce::MemoryInputStream (encoded, false), true));

            if (reader == nullptr)
                return;

            auto audio = std::make_shared<juce::AudioBuffer<float>> (2, (int) reader->lengthInSamples);
            reader->read (audio.get(), 0, (int) reader->lengthInSamples, 0, true, true);
            auto offset = startOffset;

            // Different sample rates on the two machines: resample to ours
            if (ourRate > 0.0 && theirRate > 0 && ! juce::approximatelyEqual (ourRate, (double) theirRate))
            {
                auto ratio = (double) theirRate / ourRate;
                auto numIn = audio->getNumSamples();
                auto outLength = (int) std::ceil (numIn / ratio);
                auto resampled = std::make_shared<juce::AudioBuffer<float>> (2, outLength);

                // The interpolator reads a little past the last sample it needs: give it the
                // last sample held, not whatever follows the buffer in memory
                const int padding = (int) std::ceil (ratio) + 8;
                audio->setSize (2, numIn + padding, true, true, false);

                for (int ch = 0; ch < 2; ++ch)
                {
                    for (int i = 0; i < padding && numIn > 0; ++i)
                        audio->setSample (ch, numIn + i, audio->getSample (ch, numIn - 1));

                    juce::LagrangeInterpolator interp;
                    interp.process (ratio, audio->getReadPointer (ch), resampled->getWritePointer (ch), outLength);
                }

                audio = resampled;
                offset = (int) std::llround (offset / ratio);
            }

            juce::MessageManager::callAsync ([this, aliveToken, audio, interval, offset, peerIndex, userId]
            {
                if (! *aliveToken || tap == nullptr)
                    return;

                tap->addRemote ({ peerIndex, interval, offset, audio });
                ++intervalsReceived;

                if (auto it = peers.find (userId); it != peers.end())
                    it->second->lastInterval = interval;
            });
        });
    }

    //==============================================================================
    // Connecting: signalling through the shared folder
    void writePresence()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("name", collab.getIdentity().name);
        o->setProperty ("colour", collab.getIdentity().colour.toString());
        o->setProperty ("time", juce::Time::currentTimeMillis());
        getFolder().getChildFile ("presence-" + myId() + ".json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }

    juce::File offerFile (const juce::String& offerer, const juce::String& answerer) const
    {
        return getFolder().getChildFile ("offer-" + offerer + "-" + answerer + ".json");
    }

    juce::File answerFile (const juce::String& offerer, const juce::String& answerer) const
    {
        return getFolder().getChildFile ("answer-" + offerer + "-" + answerer + ".json");
    }

    void updateSignalling()
    {
        // Who else is live right now?
        std::set<juce::String> seen;

        for (auto f : getFolder().findChildFiles (juce::File::findFiles, false, "presence-*.json"))
        {
            auto uid = f.getFileNameWithoutExtension().fromFirstOccurrenceOf ("presence-", false, false);

            if (uid == myId())
                continue;

            auto json = juce::JSON::parse (f.loadFileAsString());
            auto age = juce::Time::currentTimeMillis() - (juce::int64) json["time"];

            if (age > 60000)
                continue;

            seen.insert (uid);
            auto& peer = peers[uid];

            if (peer == nullptr)
            {
                peer = std::make_shared<Peer>();
                peer->userId = uid;
                peer->index = freePeerIndex();
                peer->isOfferer = myId() < uid;   // the lower ID makes the offer
                notifyPeersChanged();
            }

            peer->name = json["name"].toString();
            peer->colour = juce::Colour::fromString (json["colour"].toString());
        }

        // People who left
        for (auto it = peers.begin(); it != peers.end();)
        {
            // Presence comes through the synced folder, which can lag by minutes (and the two
            // computers' clocks can disagree): a connection that's up doesn't depend on it.
            // When they really leave, the connection closes, and then this applies
            if (! seen.count (it->first) && ! it->second->connected)
            {
                closePeer (*it->second);
                it = peers.erase (it);
                notifyPeersChanged();
            }
            else
            {
                ++it;
            }
        }

        for (auto& [uid, peer] : peers)
            peer->isOfferer ? progressOfferer (*peer) : progressAnswerer (*peer);
    }

    void progressOfferer (Peer& peer)
    {
        auto now = juce::Time::currentTimeMillis();

        // (Re)start if never tried, or it's been too long without connecting
        if (peer.pc == nullptr || (! peer.connected && now - peer.attemptStarted > 90000))
        {
            closePeer (peer);
            peer.nonce = juce::Uuid().toDashedString();
            peer.attemptStarted = now;
            peer.answerApplied = false;
            peer.state = "Connecting...";
            createConnection (peer);
            peer.dc = peer.pc->createDataChannel ("elsewhere");
            attachChannel (peer, peer.dc);

            // Real-time audio: unordered and never retransmitted (a late packet is useless)
            rtc::DataChannelInit rtInit;
            rtInit.reliability.unordered = true;
            rtInit.reliability.maxRetransmits = 0;
            peer.rtDc = peer.pc->createDataChannel ("elsewhere-rt", rtInit);
            attachRealtimeChannel (peer, peer.rtDc);
            peer.pc->setLocalDescription();   // offer; written to the folder once ICE gathering completes
            return;
        }

        if (! peer.answerApplied)
        {
            auto f = answerFile (myId(), peer.userId);
            auto json = juce::JSON::parse (f.loadFileAsString());

            if (json["nonce"].toString() == peer.nonce && json["sdp"].toString().isNotEmpty())
            {
                try
                {
                    peer.pc->setRemoteDescription (rtc::Description (json["sdp"].toString().toStdString(), "answer"));
                    peer.answerApplied = true;
                    peer.state = "Connecting (answer received)...";
                }
                catch (...) {}
            }
        }
    }

    void progressAnswerer (Peer& peer)
    {
        auto json = juce::JSON::parse (offerFile (peer.userId, myId()).loadFileAsString());
        auto nonce = json["nonce"].toString();

        if (nonce.isEmpty() || nonce == peer.handledNonce || json["sdp"].toString().isEmpty())
            return;

        // A fresh offer: start a new connection for it
        closePeer (peer);
        peer.handledNonce = nonce;
        peer.nonce = nonce;
        peer.state = "Connecting...";
        createConnection (peer);

        try { peer.pc->setRemoteDescription (rtc::Description (json["sdp"].toString().toStdString(), "offer")); }
        catch (...) { peer.state = "Couldn't read their connection details"; }
    }

    void createConnection (Peer& peer)
    {
        rtc::Configuration config;
        config.iceServers.emplace_back ("stun:stun.l.google.com:19302");
        config.iceServers.emplace_back ("stun:stun1.l.google.com:19302");
        config.maxMessageSize = 256 * 1024;

        auto pc = std::make_shared<rtc::PeerConnection> (config);
        peer.pc = pc;

        auto aliveToken = alive;
        auto uid = peer.userId;
        std::weak_ptr<rtc::PeerConnection> weakPc = pc;

        // Once all our addresses are known, write the offer/answer for the other side to pick up
        pc->onGatheringStateChange ([this, aliveToken, uid, weakPc] (rtc::PeerConnection::GatheringState s)
        {
            if (s != rtc::PeerConnection::GatheringState::Complete)
                return;

            auto pcLocked = weakPc.lock();
            if (pcLocked == nullptr)
                return;

            auto desc = pcLocked->localDescription();
            if (! desc.has_value())
                return;

            auto sdp = juce::String (std::string (*desc));
            auto type = juce::String (desc->typeString());

            juce::MessageManager::callAsync ([this, aliveToken, uid, sdp, type]
            {
                if (! *aliveToken)
                    return;

                auto it = peers.find (uid);
                if (it == peers.end())
                    return;

                auto& p = *it->second;
                juce::DynamicObject::Ptr o = new juce::DynamicObject();
                o->setProperty ("nonce", p.nonce);
                o->setProperty ("sdp", sdp);

                auto file = type == "offer" ? offerFile (myId(), uid) : answerFile (uid, myId());
                file.replaceWithText (juce::JSON::toString (juce::var (o.get())));
                p.state = type == "offer" ? "Waiting for " + p.name + " to answer..." : "Answered, connecting...";
                notifyPeersChanged();
            });
        });

        pc->onStateChange ([this, aliveToken, uid] (rtc::PeerConnection::State s)
        {
            juce::MessageManager::callAsync ([this, aliveToken, uid, s]
            {
                if (! *aliveToken)
                    return;

                if (auto it = peers.find (uid); it != peers.end())
                {
                    auto& p = *it->second;

                    if (s == rtc::PeerConnection::State::Failed)
                    {
                        p.connected = false;
                        p.state = "Couldn't connect (retrying)";
                        p.attemptStarted = 0;   // offerer will retry
                        if (! p.isOfferer) p.handledNonce = {};
                    }
                    else if (s == rtc::PeerConnection::State::Disconnected || s == rtc::PeerConnection::State::Closed)
                    {
                        p.connected = false;
                        p.state = "Disconnected";
                    }

                    updateRealtimeChannels();

                    notifyPeersChanged();
                }
            });
        });

        // The answering side receives the channel the offerer created
        pc->onDataChannel ([this, aliveToken, uid] (std::shared_ptr<rtc::DataChannel> dc)
        {
            juce::MessageManager::callAsync ([this, aliveToken, uid, dc]
            {
                if (! *aliveToken)
                    return;

                if (auto it = peers.find (uid); it != peers.end())
                {
                    if (dc->label() == "elsewhere-rt")
                    {
                        it->second->rtDc = dc;
                        attachRealtimeChannel (*it->second, dc);
                        updateRealtimeChannels();
                        return;
                    }

                    it->second->dc = dc;
                    attachChannel (*it->second, dc);

                    if (dc->isOpen())
                        onChannelOpen (uid);
                }
            });
        });
    }

    void attachChannel (Peer& peer, std::shared_ptr<rtc::DataChannel> dc)
    {
        auto aliveToken = alive;
        auto uid = peer.userId;

        dc->onOpen ([this, aliveToken, uid]
        {
            juce::MessageManager::callAsync ([this, aliveToken, uid] { if (*aliveToken) onChannelOpen (uid); });
        });

        dc->onClosed ([this, aliveToken, uid]
        {
            juce::MessageManager::callAsync ([this, aliveToken, uid]
            {
                if (! *aliveToken) return;
                if (auto it = peers.find (uid); it != peers.end())
                {
                    it->second->connected = false;
                    it->second->state = "Disconnected";
                    notifyPeersChanged();
                }
            });
        });

        dc->onMessage ([this, aliveToken, uid] (rtc::message_variant data)
        {
            if (! std::holds_alternative<rtc::binary> (data))
                return;

            auto bytes = std::make_shared<rtc::binary> (std::move (std::get<rtc::binary> (data)));

            juce::MessageManager::callAsync ([this, aliveToken, uid, bytes]
            {
                if (! *aliveToken) return;
                if (auto it = peers.find (uid); it != peers.end())
                    handleMessage (*it->second, *bytes);
            });
        });
    }

    /** Audio packets are decoded right on the network thread, straight into the jitter buffer. */
    void attachRealtimeChannel (Peer& peer, std::shared_ptr<rtc::DataChannel> dc)
    {
        auto slot = peer.rxSlot;

        dc->onMessage ([slot] (rtc::message_variant data)
        {
            if (auto* bytes = std::get_if<rtc::binary> (&data); bytes != nullptr && bytes->size() > 1
                                                                 && (*bytes)[0] == (std::byte) audioPacketMsg)
                if (auto rx = slot->get())
                    rx->handlePacket (bytes->data() + 1, bytes->size() - 1);
        });

        auto aliveToken = alive;
        dc->onOpen ([this, aliveToken] { juce::MessageManager::callAsync ([this, aliveToken] { if (*aliveToken) updateRealtimeChannels(); }); });
    }

    void onChannelOpen (const juce::String& uid)
    {
        auto it = peers.find (uid);
        if (it == peers.end())
            return;

        auto& p = *it->second;

        if (p.connected)
            return;

        p.connected = true;
        p.state = "Connected";
        startReceiver (p);
        updateRealtimeChannels();

        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("name", collab.getIdentity().name);
        o->setProperty ("colour", collab.getIdentity().colour.toString());
        o->setProperty ("bpm", getBpm());
        o->setProperty ("bars", getBarsPerInterval());
        o->setProperty ("mode", isRealtime() ? "realtime" : "interval");
        auto text = juce::JSON::toString (juce::var (o.get())).toStdString();

        try { p.dc->send (makeMessage (helloMsg, text.data(), text.size())); } catch (...) {}
        notifyPeersChanged();
    }

    void sendPings()
    {
        auto now = juce::Time::getMillisecondCounterHiRes();

        for (auto& [id, p] : peers)
            if (p->connected && p->dc && p->dc->isOpen())
                try { p->dc->send (makeMessage (pingMsg, &now, sizeof (now))); } catch (...) {}
    }

    void closePeer (Peer& peer)
    {
        peer.rxSlot->set (nullptr);
        if (peer.rtDc) { peer.rtDc->resetCallbacks(); try { peer.rtDc->close(); } catch (...) {} }
        peer.rtDc = nullptr;
        if (peer.dc) { peer.dc->resetCallbacks(); try { peer.dc->close(); } catch (...) {} }
        if (peer.pc) { peer.pc->resetCallbacks(); try { peer.pc->close(); } catch (...) {} }
        peer.dc = nullptr;
        peer.pc = nullptr;
        peer.connected = false;
        peer.incoming.clear();
        updateRealtimeChannels();
    }

    /** Peak level of a peer's audio over roughly the last quarter second. The plugin's value
        resets when read, so it's sampled here and shared by everyone who asks (UI, tests). */
    float getLevel (int index)
    {
        if (tap == nullptr || ! juce::isPositiveAndBelow (index, (int) levelCache.size()))
            return 0.0f;

        auto now = juce::Time::getMillisecondCounter();
        auto& c = levelCache[(size_t) index];
        auto fresh = tap->getPeerLevel (index);

        if (fresh > c.level || now - c.time > 250)
        {
            c.level = juce::jmax (fresh, now - c.time > 250 ? 0.0f : c.level);
            c.time = now;
        }

        return c.level;
    }

    struct LevelCache { float level = 0.0f; juce::uint32 time = 0; };
    std::array<LevelCache, 16> levelCache;

    void notifyPeersChanged()
    {
        if (onPeersChanged)
            onPeersChanged();
    }
};

//==============================================================================
Session::Session (Project& p, Collab::Session& c) : impl (std::make_unique<Impl> (p, c))
{
    removeTapPlugins (p.getEdit());
    impl->onPeersChanged = [this] { if (onPeersChanged) onPeersChanged(); };
    impl->onLauncherAction = [this] (const juce::String& from, const juce::var& v) { if (onLauncherAction) onLauncherAction (from, v); };
}

Session::~Session() = default;

void Session::start()            { impl->start(); }
void Session::stop()             { impl->stop(); }
bool Session::isActive() const   { return impl->active; }

int Session::getBarsPerInterval() const  { return impl->getBarsPerInterval(); }

void Session::setBarsPerInterval (int bars)
{
    impl->edit.state.setProperty (barsPerIntervalProp, juce::jlimit (1, 16, bars), nullptr);
    impl->reconfigureIfNeeded();
}

std::vector<PeerStatus> Session::getPeers() const
{
    std::vector<PeerStatus> result;

    for (auto& [id, p] : impl->peers)
    {
        PeerStatus s;
        s.userId = p->userId;
        s.name = p->name;
        s.colour = p->colour;
        s.state = p->state;
        s.connected = p->connected;
        s.roundTripMs = p->rttMs;
        s.lastIntervalReceived = p->lastInterval;
        s.level = impl->getLevel (p->index);
        s.gain = p->gain;
        s.muted = p->muted;
        s.theirBpm = p->theirBpm;
        s.theirMode = p->theirMode;

        if (impl->tap != nullptr && impl->tap->realtime)
        {
            auto& st = impl->tap->streams[(size_t) p->index];
            auto sr = impl->tap->getSampleRate() > 0 ? impl->tap->getSampleRate() : 48000.0;
            s.jitterBufferMs = st.targetSamples.load() * 1000.0 / sr;
            s.underruns = st.underruns.load();
            s.bufferedMs = st.fifo.getNumReady() * 1000.0 / sr;
            s.clockDriftPpm = st.driftPpm.load();

            // One way: half the round trip + their 5 ms frame + our jitter buffer + one audio block each end
            auto blockMs = impl->edit.engine.getDeviceManager().getBlockSize() * 1000.0 / sr;
            s.estimatedLatencyMs = p->rttMs * 0.5 + opusFrame * 1000.0 / opusRate + s.jitterBufferMs + 2.0 * blockMs;
        }

        result.push_back (s);
    }

    return result;
}

Stats Session::getStats() const
{
    Stats s;

    if (impl->tap != nullptr)
    {
        s.currentInterval = impl->tap->getCurrentInterval();
        s.intervalProgress = impl->tap->getIntervalProgress();
    }

    s.intervalsSent = impl->intervalsSent;
    s.realtime = impl->isRealtime();

    s.intervalsReceived = impl->intervalsReceived;
    return s;
}

bool Session::isRealtime() const     { return impl->isRealtime(); }

static const juce::Identifier shareLaunchesProp { "elsewhereLiveShareLaunches" };

bool Session::isSharingLaunches() const
{
    return (bool) impl->edit.state.getProperty (shareLaunchesProp, true);
}

void Session::setSharingLaunches (bool share)
{
    impl->edit.state.setProperty (shareLaunchesProp, share, nullptr);
}

void Session::sendLauncherAction (const juce::var& v)
{
    if (isActive() && isSharingLaunches())
        impl->sendLauncherAction (v);
}

void Session::setRealtime (bool shouldBeRealtime)
{
    // Stored in the project, so it syncs and everyone switches together
    impl->edit.state.setProperty (liveModeProp, shouldBeRealtime ? "realtime" : "interval", nullptr);
    impl->applyMode();
}

void Session::setPeerGain (const juce::String& userId, float gain)
{
    if (auto it = impl->peers.find (userId); it != impl->peers.end())
    {
        it->second->gain = gain;

        if (impl->tap != nullptr)
            impl->tap->setPeerGain (it->second->index, it->second->muted ? 0.0f : gain);
    }
}

void Session::setPeerMuted (const juce::String& userId, bool muted)
{
    if (auto it = impl->peers.find (userId); it != impl->peers.end())
    {
        it->second->muted = muted;

        if (impl->tap != nullptr)
            impl->tap->setPeerGain (it->second->index, muted ? 0.0f : it->second->gain);
    }
}

} // namespace Live

} // namespace elsewhere
