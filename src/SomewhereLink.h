// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Project.h"
#include "EditViewState.h"
#include "Mixing.h"
#include "Theme.h"
#include "RealtimeAudio.h"

#if JUCE_MAC
 #include <dns_sd.h>
 #include <arpa/inet.h>
 #include <dispatch/dispatch.h>
#elif JUCE_WINDOWS
namespace elsewhere::somewherelink
{
    // SomewhereLinkWindows.cpp: DNS-SD through windns.h, kept out of this header
    void* advertise (int port, const std::string& projectName);
    void unadvertise (void* handle);
    bool canWrite (int socket, int timeoutMs);
}
#endif

#if ! JUCE_WINDOWS
 #include <poll.h>
namespace elsewhere::somewherelink
{
    /** Whether a socket can take a write within the time. Asked of the socket itself: JUCE's
        waitUntilReady shares a lock with read(), which the connection's reader thread holds the
        whole time it waits for the phone, so it always says no while the phone is quiet. */
    inline bool canWrite (int socket, int timeoutMs)
    {
        pollfd p { socket, POLLOUT, 0 };
        return ::poll (&p, 1, timeoutMs) == 1 && (p.revents & POLLOUT) != 0;
    }
}
#endif

namespace elsewhere
{
//==============================================================================
/**
    Somewhere Link: Somewhere (the iPhone and Android app) on the same Wi-Fi controlling Elsewhere.

    Elsewhere listens on a TCP port and advertises it with Bonjour as "_somewhere._tcp". A phone
    connects, says hello, and the first time you're asked whether to allow it. After that it can:
      - move the focused track's fader and pan,
      - play the Chaos pad, which moves two controls of the track's Mono / Poly / Modular synth
        and springs them back when the finger lifts,
      - drop a sample, which is saved to Samples/Somewhere/<Phone>/ and placed on the focused track
        at the playhead.
    Elsewhere tells the phone which track is focused and keeps its fader in step.

    Tempo sync: Elsewhere is the clock. Ten times a second it sends its tempo, whether it's
    playing, and the playhead in beats (plus its output latency), so Somewhere's sequencer can
    lock to the bar. The phone can also set the tempo and start/stop Elsewhere, and pings
    to measure the network delay.

    Monitoring: a phone can also stream what it plays to Elsewhere's speakers, uncompressed over
    UDP (16-bit, 5 ms packets: about 1.5 Mbit/s, nothing for home Wi-Fi). Packets go through
    the same jitter buffer and clock-drift correction as low-latency live jamming
    (RealtimeAudio.h) and are mixed into Elsewhere's audio output. They're monitoring only:
    takes are what get recorded.

    Takes: while Elsewhere records, Somewhere captures its own output (pads, sequencer, FX). When the
    recording stops it sends the take with the beat it started on, and Elsewhere puts it on the
    phone's own "Somewhere (Name)" track at exactly that beat.

    Frames are [u32 BE header length][header JSON][u32 BE payload length][payload], the same as
    ios/Somewhere/Link/LinkProtocol.swift (which documents every message).

    Moves go through the parameters like a mouse drag does, so they're undoable, sync to
    collaborators, and are written as automation when AUTO is on.
*/
/**
    Plays phones' live audio through Elsewhere's output. Packets:
        [u32 token][u32 sequence][u32 sample rate][int16 stereo interleaved...]   (little-endian)
    The token is handed to an allowed phone over the TCP link, so nothing else can play through
    your speakers.
*/
class SomewhereMonitor : public juce::AudioIODeviceCallback, private juce::Thread
{
public:
    static constexpr int maxPhones = 4;

    explicit SomewhereMonitor (te::Engine& e) : juce::Thread ("Somewhere monitor"), engine (e)
    {
        if (socket.bindToPort (0))
        {
            port = socket.getBoundPort();
            startThread (juce::Thread::Priority::highest);
        }

        engine.getDeviceManager().deviceManager.addAudioCallback (this);
    }

    ~SomewhereMonitor() override
    {
        engine.getDeviceManager().deviceManager.removeAudioCallback (this);
        signalThreadShouldExit();
        socket.shutdown();
        stopThread (2000);
    }

    int getPort() const     { return port; }

    /** How much buffering to trade for steadiness (the phone's BUFFER setting). */
    struct Profile
    {
        double startMs, minMs, maxMs;
        int shrinkAfterSeconds;     // steady this long before trimming 2 ms

        static Profile named (const juce::String& name)
        {
            if (name == "tight") return { 15.0, 8.0, 80.0, 20 };
            if (name == "safe")  return { 60.0, 40.0, 200.0, 60 };
            return { 30.0, 15.0, 120.0, 30 };
        }
    };

    /** Gives a phone a slot; returns its token (0 if all slots are taken). */
    juce::uint32 open (int connectionId, const Profile& profile)
    {
        for (auto& s : slots)
            if (s.owner == 0)
            {
                s.owner = connectionId;
                s.lastUnderruns = s.stableSeconds = 0;
                s.profile = profile;
                s.buffer.targetSamples = (int) (profile.startMs * 0.001 * deviceRate.load());   // adapt() tunes it from here
                s.restart = true;
                s.packets = 0;
                auto token = (juce::uint32) juce::Random::getSystemRandom().nextInt (0x7fffffff) | 1u;
                s.token = token;
                return token;
            }

        return 0;
    }

    /** Changes the buffering without restarting the stream. */
    bool setProfile (int connectionId, const Profile& profile)
    {
        for (auto& s : slots)
            if (s.owner == connectionId)
            {
                s.profile = profile;
                s.stableSeconds = 0;
                s.buffer.targetSamples = (int) (profile.startMs * 0.001 * deviceRate.load());
                return true;
            }
        return false;
    }

    void close (int connectionId)
    {
        for (auto& s : slots)
            if (s.owner == connectionId)
            {
                s.token = 0;
                s.owner = 0;
                s.buffer.active = false;
            }
    }

    /** How far behind live a phone's audio is heard: its jitter buffer, in seconds. */
    double getDelaySeconds (int connectionId) const
    {
        for (auto& s : slots)
            if (s.owner == connectionId)
                return s.buffer.targetSamples.load() / deviceRate.load();
        return 0.0;
    }

    /** Packets received from a phone since it started monitoring. */
    int getPackets (int connectionId) const
    {
        for (auto& s : slots)
            if (s.owner == connectionId)
                return s.packets.load();
        return 0;
    }

    /** Once a second (message thread): tune each jitter buffer to the Wi-Fi. */
    void adapt()
    {
        for (auto& s : slots)
            if (s.owner != 0)
                s.buffer.adapt (s.lastUnderruns, s.stableSeconds, deviceRate.load(),
                                s.profile.minMs, s.profile.maxMs, s.profile.shrinkAfterSeconds);
    }

    //==============================================================================
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        deviceRate = device->getCurrentSampleRate() > 0 ? device->getCurrentSampleRate() : 48000.0;
        for (auto& s : slots)
            s.restart = true;
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* outputs, int numOutputs,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputs[ch], numSamples);

        if (numOutputs < 1)
            return;

        for (auto& s : slots)
            if (s.owner != 0)
                s.buffer.mixInto (outputs[0], numOutputs > 1 ? outputs[1] : nullptr, numSamples, 1.0f);
    }

private:
    struct Slot
    {
        Realtime::JitterBuffer buffer;
        std::atomic<juce::uint32> token { 0 };
        std::atomic<int> owner { 0 };                  // connection id, 0 = free
        std::atomic<bool> restart { false };
        std::atomic<int> packets { 0 };
        int lastUnderruns = 0, stableSeconds = 0;      // message thread (adapt)
        Profile profile = Profile::named ("normal");

        // Network thread only
        std::unique_ptr<Realtime::Receiver> receiver;
        juce::uint32 expected = 0, senderRate = 0;
        bool haveSeq = false;
        float lastL[1024] {}, lastR[1024] {};
        int lastFrames = 0;
    };

    te::Engine& engine;
    juce::DatagramSocket socket { false };
    int port = 0;
    std::atomic<double> deviceRate { 48000.0 };
    std::array<Slot, maxPhones> slots;

    static juce::uint32 readU32 (const juce::uint8* p)
    {
        return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24);
    }

    void run() override
    {
        juce::uint8 packet[4096];
        float l[1024], r[1024];

        while (! threadShouldExit())
        {
            if (socket.waitUntilReady (true, 100) <= 0)
                continue;

            auto size = socket.read (packet, (int) sizeof (packet), false);
            if (size < 16)
                continue;

            auto token = readU32 (packet), seq = readU32 (packet + 4), rate = readU32 (packet + 8);
            auto frames = juce::jmin (1024, (size - 12) / 4);

            if (rate < 8000 || rate > 192000)
                continue;   // not a rate any phone sends: converting from it would never finish

            for (auto& s : slots)
            {
                if (token == 0 || s.token.load() != token)
                    continue;

                if (s.restart.exchange (false) || s.receiver == nullptr || rate != s.senderRate)
                {
                    s.receiver.reset();
                    s.receiver = std::make_unique<Realtime::Receiver> (s.buffer, (double) rate, deviceRate.load());
                    s.senderRate = rate;
                    s.haveSeq = false;
                }

                if (s.haveSeq)
                {
                    auto gap = (juce::int32) (seq - s.expected);
                    if (gap < -500)
                        gap = 0;  // far behind: the phone started sending again, counting from zero
                    else if (gap < 0)
                        break;    // late or duplicated: too late to use

                    // Lost packets (a few at most): keep time by repeating the last one, fading it
                    // away across the gap, instead of hard silence (which clicks)
                    const int lost = juce::jmin (gap, 3);
                    for (int k = 0; k < lost && s.lastFrames > 0; ++k)
                    {
                        const int m = s.lastFrames, total = lost * m;
                        for (int i = 0; i < m; ++i)
                        {
                            const float g = 1.0f - (float) (k * m + i + 1) / (float) total;
                            l[i] = s.lastL[i] * g;
                            r[i] = s.lastR[i] * g;
                        }
                        s.receiver->add (l, r, m, m / (double) rate);
                    }
                }

                const bool afterGap = s.haveSeq && seq != s.expected;
                s.haveSeq = true;
                s.expected = seq + 1;
                ++s.packets;

                auto* pcm = packet + 12;
                for (int i = 0; i < frames; ++i)
                {
                    auto fadeIn = afterGap && i < 64 ? (float) i / 64.0f : 1.0f;   // back in from the faded gap
                    l[i] = fadeIn * (juce::int16) (pcm[i * 4] | (pcm[i * 4 + 1] << 8)) / 32768.0f;
                    r[i] = fadeIn * (juce::int16) (pcm[i * 4 + 2] | (pcm[i * 4 + 3] << 8)) / 32768.0f;
                }

                std::copy (l, l + frames, s.lastL);
                std::copy (r, r + frames, s.lastR);
                s.lastFrames = frames;

                s.receiver->add (l, r, frames, frames / (double) rate);
                break;
            }
        }
    }
};

//==============================================================================
class SomewhereLink : private juce::Timer
{
public:
    static constexpr const char* serviceType = "_somewhere._tcp";
    static constexpr int protocolVersion = 1;

    /** Elsewhere listens here when it's free, so a phone can also connect by IP address alone. */
    static constexpr int defaultPort = 47820;

    SomewhereLink (Project& p, EditViewState& vs, juce::PropertiesFile& properties)
        : project (p), viewState (vs), props (properties)
    {
        server = std::make_unique<Server> (*this);
        monitor = std::make_unique<SomewhereMonitor> (project.getEdit().engine);

        if (server->start())
            advertise (server->port);

        startTimerHz (10);
    }

    ~SomewhereLink() override
    {
        stopTimer();
        unadvertise();
        server.reset();          // stops accepting
        monitor.reset();         // stops playing phones' audio
        connections.clear();     // closes sockets and joins reader threads
        masterReference.clear();
    }

    /** Puts a dropped sample on the focused track at the playhead (MainComponent::importFile). */
    std::function<void (const juce::File&)> onSampleDropped;

    /** Puts a recorded take on the phone's track at a beat. Returns the track's name. */
    std::function<juce::String (const juce::File&, double beat, const juce::String& phoneName)> onTake;

    /** Names of the phones connected right now, for the status line. */
    juce::StringArray getConnectedNames() const
    {
        juce::StringArray names;
        for (auto* c : connections)
            if (c->allowed)
                names.add (c->name);
        return names;
    }

    int getPort() const     { return server != nullptr ? server->port : 0; }

    //==============================================================================
    /** Wire format helpers (public so tests can check them). */
    static juce::MemoryBlock encode (const juce::var& header, const void* payload = nullptr, size_t payloadSize = 0)
    {
        auto json = juce::JSON::toString (header, true).toStdString();
        juce::MemoryBlock out;
        auto putU32 = [&out] (juce::uint32 v)
        {
            const juce::uint8 b[] = { (juce::uint8) (v >> 24), (juce::uint8) (v >> 16), (juce::uint8) (v >> 8), (juce::uint8) v };
            out.append (b, 4);
        };
        putU32 ((juce::uint32) json.size());
        out.append (json.data(), json.size());
        putU32 ((juce::uint32) payloadSize);
        if (payloadSize > 0)
            out.append (payload, payloadSize);
        return out;
    }

    static juce::var message (const char* type)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("type", type);
        return juce::var (o);
    }

    /** Which two controls the Chaos pad moves on a track (empty when it has no Elsewhere synth). */
    struct ChaosTargets
    {
        juce::String synth;
        te::AutomatableParameter::Ptr x, y;
    };

    static ChaosTargets findChaosTargets (te::Track& track)
    {
        for (auto p : track.pluginList)
        {
            auto type = p->getPluginType();
            auto model = [&p]
            {
                auto m = p->getAutomatableParameterByID ("model");
                return m != nullptr ? juce::roundToInt (m->getCurrentValue()) : 0;
            };
            auto make = [&p] (const char* name, const char* x, const char* y)
            {
                return ChaosTargets { name, p->getAutomatableParameterByID (x), p->getAutomatableParameterByID (y) };
            };

            // X is the brightness of the sound, Y its bite
            if (type == "elsewhereMono")
            {
                switch (model())
                {
                    case 1:  return make ("Mono", "accutoff", "acres");     // Acid
                    case 2:  return make ("Mono", "sycutoff", "sybpitch");  // Sync Lead
                    default: return make ("Mono", "cutoff", "emph");        // Seventy
                }
            }

            if (type == "elsewherePoly")
            {
                switch (model())
                {
                    case 1:  return make ("Poly", "qlp", "qlpres");         // Quad
                    case 2:  return make ("Poly", "vcutoff", "vres");       // Velvet
                    default: return make ("Poly", "pcutoff", "pres");       // Prism
                }
            }

            if (type == "elsewhereModular")
                return make ("Modular", "macro1", "macro2");
        }

        return {};
    }

private:
    //==============================================================================
    struct Connection;

    /** Accepts phones on a background thread. */
    struct Server : public juce::Thread
    {
        Server (SomewhereLink& o) : juce::Thread ("Somewhere Link server"), owner (o) {}

        ~Server() override
        {
            signalThreadShouldExit();
            listener.close();
            stopThread (2000);
        }

        bool start()
        {
            // The usual port, so "connect by IP" works; if it's taken (a second Elsewhere), let the
            // system choose and rely on Bonjour to tell the phone which one
            if (! listener.createListener (defaultPort) && ! listener.createListener (0))
                return false;

            port = listener.getBoundPort();
            startThread (juce::Thread::Priority::low);
            return true;
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                std::unique_ptr<juce::StreamingSocket> socket (listener.waitForNextConnection());

                if (socket == nullptr || threadShouldExit())
                    continue;

                juce::MessageManager::callAsync ([weak = juce::WeakReference<SomewhereLink> (&owner), s = socket.release()]
                {
                    std::unique_ptr<juce::StreamingSocket> owned (s);
                    if (auto* link = weak.get())
                        link->accept (std::move (owned));
                });
            }
        }

        SomewhereLink& owner;
        juce::StreamingSocket listener;
        int port = 0;
    };

    /** One phone. Reads frames on its own thread and hands them to the message thread. */
    struct Connection : public juce::Thread
    {
        Connection (SomewhereLink& o, std::unique_ptr<juce::StreamingSocket> s, int connectionId)
            : juce::Thread ("Somewhere Link connection"), owner (o), socket (std::move (s)), id (connectionId)
        {
            startThread (juce::Thread::Priority::normal);
        }

        ~Connection() override
        {
            signalThreadShouldExit();
            socket->close();
            stopThread (2000);
        }

        void send (const juce::MemoryBlock& frame)
        {
            const juce::ScopedLock sl (writeLock);

            if (! socket->isConnected())
                return;

            // A phone that's asleep or out of range stops reading, and once the buffers fill a
            // write waits for it: that's the whole app waiting, since this is the message thread.
            // If it can't take a write now, drop the connection; the phone connects again
            if (! somewherelink::canWrite (socket->getRawSocketHandle(), 50))
            {
                socket->close();
                return;
            }

            socket->write (frame.getData(), (int) frame.getSize());
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                juce::uint32 headerSize = 0, payloadSize = 0;
                juce::MemoryBlock header, payload;

                if (! readU32 (headerSize) || headerSize > 64 * 1024 || ! readBlock (header, headerSize)
                     || ! readU32 (payloadSize) || payloadSize > 200 * 1024 * 1024 || ! readBlock (payload, payloadSize))
                    break;

                auto json = juce::JSON::parse (header.toString());

                juce::MessageManager::callAsync ([weak = juce::WeakReference<SomewhereLink> (&owner), cid = id, json, p = std::move (payload)]
                {
                    if (auto* link = weak.get())
                        link->handle (cid, json, p);
                });
            }

            juce::MessageManager::callAsync ([weak = juce::WeakReference<SomewhereLink> (&owner), cid = id]
            {
                if (auto* link = weak.get())
                    link->closed (cid);
            });
        }

        bool readBlock (juce::MemoryBlock& block, juce::uint32 size)
        {
            block.setSize (size);
            return size == 0 || socket->read (block.getData(), (int) size, true) == (int) size;
        }

        bool readU32 (juce::uint32& v)
        {
            juce::uint8 b[4];
            if (socket->read (b, 4, true) != 4)
                return false;
            v = (juce::uint32 (b[0]) << 24) | (juce::uint32 (b[1]) << 16) | (juce::uint32 (b[2]) << 8) | b[3];
            return true;
        }

        SomewhereLink& owner;
        std::unique_ptr<juce::StreamingSocket> socket;
        juce::CriticalSection writeLock;
        const int id;

        // Message-thread state
        juce::String name, tag;
        bool allowed = false;
        juce::String lastTrackState;
        juce::Time lastFaderMove;
        bool lastTransportPlaying = true;      // so the first tick always reports the transport
        juce::uint32 monitorToken = 0;
        double lastTransportBpm = -1;

        // Chaos pad touch in progress: where the controls were before, to spring back to
        ChaosTargets chaos;
        float chaosStartX = 0, chaosStartY = 0;
    };

    //==============================================================================
    Project& project;
    EditViewState& viewState;
    juce::PropertiesFile& props;
    std::unique_ptr<Server> server;
    std::unique_ptr<SomewhereMonitor> monitor;
    int adaptCounter = 0;
    juce::OwnedArray<Connection> connections;
    int nextId = 1;

   #if JUCE_MAC
    DNSServiceRef bonjour = nullptr;
   #elif JUCE_WINDOWS
    void* windowsRegistration = nullptr;
   #endif

    void advertise (int port)
    {
       #if JUCE_MAC
        // Shown on the phone under the Mac's own name, with the project as a subtitle
        TXTRecordRef txt;
        TXTRecordCreate (&txt, 0, nullptr);
        auto projectName = project.getName().toStdString().substr (0, 200);
        TXTRecordSetValue (&txt, "project", (uint8_t) projectName.size(), projectName.data());
        TXTRecordSetValue (&txt, "v", 1, "1");

        auto err = DNSServiceRegister (&bonjour, 0, 0, nullptr, serviceType, nullptr, nullptr, htons ((uint16_t) port),
                                       TXTRecordGetLength (&txt), TXTRecordGetBytesPtr (&txt), registered, nullptr);
        TXTRecordDeallocate (&txt);

        // The registration only goes live while its replies are being handled
        if (err == kDNSServiceErr_NoError)
            DNSServiceSetDispatchQueue (bonjour, dispatch_get_main_queue());
        else
            bonjour = nullptr;
       #elif JUCE_WINDOWS
        windowsRegistration = somewherelink::advertise (port, project.getName().toStdString());
       #else
        juce::ignoreUnused (port);
       #endif
    }

   #if JUCE_MAC
    static void registered (DNSServiceRef, DNSServiceFlags, DNSServiceErrorType err, const char* name, const char*, const char*, void*)
    {
        if (err != kDNSServiceErr_NoError)
            DBG ("Somewhere Link: Bonjour registration failed (" << (int) err << ")");
        else
            DBG ("Somewhere Link: advertising as " << name);
    }
   #endif

    void unadvertise()
    {
       #if JUCE_MAC
        if (bonjour != nullptr)
            DNSServiceRefDeallocate (bonjour);
        bonjour = nullptr;
       #elif JUCE_WINDOWS
        somewherelink::unadvertise (windowsRegistration);
        windowsRegistration = nullptr;
       #endif
    }

    Connection* find (int id)
    {
        for (auto* c : connections)
            if (c->id == id)
                return c;
        return nullptr;
    }

    void accept (std::unique_ptr<juce::StreamingSocket> socket)
    {
        connections.add (new Connection (*this, std::move (socket), nextId++));
    }

    void closed (int id)
    {
        if (auto* c = find (id))
        {
            endChaos (*c);
            monitor->close (id);
            connections.removeObject (c);
        }
    }

    //==============================================================================
    void handle (int id, const juce::var& msg, const juce::MemoryBlock& payload)
    {
        auto* c = find (id);
        if (c == nullptr)
            return;

        const auto type = msg["type"].toString();

        if (type == "hello")
            return hello (*c, msg);

        if (! c->allowed)
            return;

        if (type == "ping")         return pong (*c, msg);
        if (type == "tempo")        return setTempo ((double) msg["bpm"]);
        if (type == "play")         return setPlaying (true);
        if (type == "stop")         return setPlaying (false);
        if (type == "record")       return setRecording ((bool) msg["on"]);
        if (type == "take")         return take (*c, msg, payload);
        if (type == "monitor")      return setMonitoring (*c, (bool) msg["on"], msg["buffer"].toString());

        if (type == "fader")        setFader (*c, (float) (double) msg["position"]);
        else if (type == "pan")     setPan ((float) (double) msg["value"]);
        else if (type == "chaos")   moveChaos (*c, (float) (double) msg["x"], (float) (double) msg["y"], (bool) msg["touching"]);
        else if (type == "drop")    drop (*c, msg["name"].toString(), payload);
    }

    /** Phones you've allowed. ELSEWHERE_SOMEWHERE_TRUST=<tag> pre-allows one (for scripted tests). */
    juce::StringArray trustedTags()
    {
        auto tags = juce::StringArray::fromTokens (props.getValue ("somewhereTrusted"), ",", {});
        tags.addIfNotAlreadyThere (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_SOMEWHERE_TRUST", {}));
        tags.removeEmptyStrings();
        return tags;
    }

    void hello (Connection& c, const juce::var& msg)
    {
        c.name = msg["name"].toString().substring (0, 60);
        c.tag = msg["tag"].toString().retainCharacters ("0123456789abcdef").substring (0, 16);

        if (c.name.isEmpty())
            c.name = "Phone";

        if (c.tag.isNotEmpty() && trustedTags().contains (c.tag))
            return welcome (c);

        c.send (encode (message ("waiting")));

        auto text = c.name + " (" + c.tag + ") wants to connect to Elsewhere from Somewhere.\n\n"
                    "It will be able to move the selected track's fader and synth controls, and add samples to the project.";

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, "Allow this phone?", text, "Allow", "Don't Allow", nullptr,
            juce::ModalCallbackFunction::create ([weak = juce::WeakReference<SomewhereLink> (this), id = c.id] (int result)
            {
                auto* link = weak.get();
                auto* conn = link != nullptr ? link->find (id) : nullptr;
                if (conn == nullptr)
                    return;

                if (result == 1)
                {
                    auto trusted = link->trustedTags();
                    trusted.addIfNotAlreadyThere (conn->tag);
                    link->props.setValue ("somewhereTrusted", trusted.joinIntoString (","));
                    link->welcome (*conn);
                }
                else
                {
                    conn->send (encode (message ("denied")));
                    link->connections.removeObject (conn);
                }
            }));
    }

    void welcome (Connection& c)
    {
        c.allowed = true;
        auto m = message ("welcome");
        m.getDynamicObject()->setProperty ("project", project.getName());
        c.send (encode (m));
        c.lastTrackState = {};
        sendTrackState (c);
    }

    //==============================================================================
    te::Track* target()
    {
        auto* t = viewState.focusedTrack.get();
        return t != nullptr && ! t->isMasterTrack() ? t : nullptr;
    }

    juce::var trackState()
    {
        auto* t = target();
        if (t == nullptr)
        {
            auto m = message ("track");
            m.getDynamicObject()->setProperty ("name", "No track selected");
            return m;
        }

        auto m = message ("track");
        auto* o = m.getDynamicObject();
        o->setProperty ("name", t->getName());

        auto colour = Theme::ofTrack (*t);
        o->setProperty ("colour", "#" + colour.toDisplayString (false));

        if (auto* vp = Mixing::findVolume (*t))
        {
            o->setProperty ("position", vp->getSliderPos());
            o->setProperty ("pan", vp->getPan());
        }

        auto chaos = findChaosTargets (*t);
        if (chaos.x != nullptr && chaos.y != nullptr)
        {
            o->setProperty ("synth", chaos.synth);
            o->setProperty ("chaosX", chaos.x->getParameterName());
            o->setProperty ("chaosY", chaos.y->getParameterName());
        }

        return m;
    }

    void sendTrackState (Connection& c)
    {
        auto state = trackState();
        auto json = juce::JSON::toString (state, true);

        // Don't echo the fader back while the phone is still moving it
        if (json == c.lastTrackState || juce::Time::getCurrentTime() - c.lastFaderMove < juce::RelativeTime::milliseconds (400))
            return;

        c.lastTrackState = json;
        c.send (encode (state));
    }

    void setMonitoring (Connection& c, bool on, const juce::String& buffer)
    {
        auto profile = SomewhereMonitor::Profile::named (buffer);

        // Already monitoring and only the BUFFER setting changed: no need to restart the stream
        if (on && c.monitorToken != 0 && monitor->setProfile (c.id, profile))
        {
            auto reply = message ("monitor");
            auto* o = reply.getDynamicObject();
            o->setProperty ("on", true);
            o->setProperty ("port", monitor->getPort());
            o->setProperty ("token", (juce::int64) c.monitorToken);
            c.send (encode (reply));
            return;
        }

        monitor->close (c.id);
        c.monitorToken = 0;
        auto reply = message ("monitor");
        auto* o = reply.getDynamicObject();

        if (on && monitor->getPort() > 0)
        {
            auto token = monitor->open (c.id, profile);
            c.monitorToken = token;
            o->setProperty ("on", token != 0);
            o->setProperty ("port", monitor->getPort());
            o->setProperty ("token", (juce::int64) token);
            if (token == 0)
                o->setProperty ("message", "Elsewhere is already monitoring " + juce::String (SomewhereMonitor::maxPhones) + " phones.");
        }
        else
        {
            o->setProperty ("on", false);
        }

        c.send (encode (reply));
    }

    void timerCallback() override
    {
        if (++adaptCounter % 10 == 0)
            monitor->adapt();

        auto transport = transportState();

        for (auto* c : connections)
            if (c->allowed)
            {
                sendTrackState (*c);
                sendTransport (*c, transport);
            }
    }

    //==============================================================================
    struct TransportState
    {
        double bpm = 120, beat = 0, latency = 0;
        bool playing = false, recording = false;
        int beatsPerBar = 4;
    };

    TransportState transportState()
    {
        auto& edit = project.getEdit();
        auto& transport = edit.getTransport();
        TransportState t;
        t.playing = transport.isPlaying();
        t.recording = transport.isRecording();
        t.bpm = edit.tempoSequence.getTempo (0)->getBpm();
        t.beat = edit.tempoSequence.toBeats (transport.getPosition()).inBeats();
        t.beatsPerBar = edit.tempoSequence.getTimeSig (0)->numerator;
        t.latency = edit.engine.getDeviceManager().getOutputLatencySeconds();
        return t;
    }

    void sendTransport (Connection& c, const TransportState& t)
    {
        // While playing the phone needs a steady stream to stay locked; when stopped, only changes
        if (! t.playing && ! c.lastTransportPlaying && std::abs (t.bpm - c.lastTransportBpm) < 0.001)
            return;

        c.lastTransportPlaying = t.playing;
        c.lastTransportBpm = t.bpm;

        auto m = message ("transport");
        auto* o = m.getDynamicObject();
        o->setProperty ("bpm", t.bpm);
        o->setProperty ("playing", t.playing);
        o->setProperty ("recording", t.recording);
        o->setProperty ("beat", t.beat);
        o->setProperty ("beatsPerBar", t.beatsPerBar);
        o->setProperty ("latency", t.latency);
        o->setProperty ("monitorDelay", monitor->getDelaySeconds (c.id));   // 0 when not monitoring
        o->setProperty ("monitorPackets", monitor->getPackets (c.id));
        c.send (encode (m));
    }

    void pong (Connection& c, const juce::var& ping)
    {
        auto m = message ("pong");
        m.getDynamicObject()->setProperty ("t", ping["t"]);
        c.send (encode (m));
    }

    void setTempo (double bpm)
    {
        if (bpm < 20.0 || bpm > 400.0)
            return;

        auto& edit = project.getEdit();
        edit.getUndoManager().beginNewTransaction ("Somewhere tempo");
        edit.tempoSequence.getTempo (0)->setBpm (bpm);
    }

    void setRecording (bool on)
    {
        if (project.getEdit().getTransport().isRecording() != on)
            EngineHelpers::toggleRecord (project.getEdit());
    }

    void take (Connection& c, const juce::var& msg, const juce::MemoryBlock& wav)
    {
        auto folder = project.getFolder().getChildFile ("Samples/Somewhere")
                          .getChildFile (juce::File::createLegalFileName (c.name + " (" + c.tag + ")"))
                          .getChildFile ("Takes");
        auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
        auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (stamp + " " + msg["name"].toString()), ".wav", false);

        auto reply = message ("taken");
        auto* o = reply.getDynamicObject();

        if (folder.createDirectory() && file.replaceWithData (wav.getData(), wav.getSize()) && onTake)
        {
            o->setProperty ("track", onTake (file, (double) msg["beat"], c.name));
            o->setProperty ("seconds", project.getEdit().tempoSequence.toTime (te::BeatPosition::fromBeats ((double) msg["beat"])).inSeconds());
        }
        else
        {
            reply = message ("error");
            reply.getDynamicObject()->setProperty ("message", "Elsewhere couldn't save the take.");
        }

        c.send (encode (reply));
    }

    void setPlaying (bool play)
    {
        auto& transport = project.getEdit().getTransport();

        if (play && ! transport.isPlaying())
            transport.play (false);
        else if (! play && transport.isPlaying())
            transport.stop (false, false);
    }

    //==============================================================================
    void setFader (Connection& c, float position)
    {
        if (auto* t = target())
            if (auto* vp = Mixing::findVolume (*t))
            {
                // One undo step per gesture rather than per message
                if (juce::Time::getCurrentTime() - c.lastFaderMove > juce::RelativeTime::milliseconds (600))
                    t->edit.getUndoManager().beginNewTransaction ("Somewhere fader");

                c.lastFaderMove = juce::Time::getCurrentTime();
                vp->setSliderPos (juce::jlimit (0.0f, 1.0f, position));
            }
    }

    void setPan (float pan)
    {
        if (auto* t = target())
            if (auto* vp = Mixing::findVolume (*t))
                vp->setPan (juce::jlimit (-1.0f, 1.0f, pan));
    }

    void moveChaos (Connection& c, float x, float y, bool touching)
    {
        if (! touching)
            return endChaos (c);

        if (c.chaos.x == nullptr)
        {
            auto* t = target();
            if (t == nullptr)
                return;

            c.chaos = findChaosTargets (*t);
            if (c.chaos.x == nullptr || c.chaos.y == nullptr)
            {
                c.chaos = {};
                return;
            }

            t->edit.getUndoManager().beginNewTransaction ("Somewhere chaos");
            c.chaosStartX = c.chaos.x->getCurrentNormalisedValue();
            c.chaosStartY = c.chaos.y->getCurrentNormalisedValue();
            c.chaos.x->parameterChangeGestureBegin();
            c.chaos.y->parameterChangeGestureBegin();
        }

        c.chaos.x->setNormalisedParameter (juce::jlimit (0.0f, 1.0f, x), juce::sendNotification);
        c.chaos.y->setNormalisedParameter (juce::jlimit (0.0f, 1.0f, y), juce::sendNotification);
    }

    /** Finger lifted (or phone gone): spring back to where the controls were. */
    void endChaos (Connection& c)
    {
        if (c.chaos.x == nullptr)
            return;

        c.chaos.x->setNormalisedParameter (c.chaosStartX, juce::sendNotification);
        c.chaos.y->setNormalisedParameter (c.chaosStartY, juce::sendNotification);
        c.chaos.x->parameterChangeGestureEnd();
        c.chaos.y->parameterChangeGestureEnd();
        c.chaos = {};
    }

    //==============================================================================
    void drop (Connection& c, const juce::String& sampleName, const juce::MemoryBlock& wav)
    {
        auto reply = [&c] (const char* type, const juce::String& key, const juce::String& value, const juce::String& track = {})
        {
            auto m = message (type);
            m.getDynamicObject()->setProperty (key, value);
            if (track.isNotEmpty())
                m.getDynamicObject()->setProperty ("track", track);
            c.send (encode (m));
        };

        // Same folder the phone uses when it sends through the cloud
        auto folder = project.getFolder().getChildFile ("Samples/Somewhere")
                          .getChildFile (juce::File::createLegalFileName (c.name + " (" + c.tag + ")"));
        auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H.%M.%S");
        auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (stamp + " " + sampleName), ".wav", false);

        if (! folder.createDirectory() || ! file.replaceWithData (wav.getData(), wav.getSize()))
            return reply ("error", "message", "Elsewhere couldn't save the sample.");

        auto* t = target();
        auto trackName = t != nullptr ? t->getName() : juce::String ("a new track");

        if (onSampleDropped)
            onSampleDropped (file);

        reply ("dropped", "name", sampleName, trackName);
    }

    JUCE_DECLARE_WEAK_REFERENCEABLE (SomewhereLink)
};

} // namespace elsewhere
