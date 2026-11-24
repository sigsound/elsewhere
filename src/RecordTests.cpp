// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "RecordTests.h"
#include "Project.h"

namespace elsewhere::RecordTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
        void note (const juce::String& what)            { text << "        " << what << "\n"; }
        void title (const juce::String& what)           { text << "\n" << what << "\n"; }
    };

    /** Elsewhere's own engine settings, with no system audio devices. */
    struct Behaviour : public ElsewhereEngineBehaviour
    {
        Behaviour() : ElsewhereEngineBehaviour (false) {}
        bool addSystemAudioIODeviceTypes() override     { return false; }
    };

    /** White noise at about -6 dBFS, already on 24-bit steps, different on every channel: it never
        repeats, so any stretch of a recording can be matched to exactly one place in it. */
    float testSample (int channel, juce::int64 n)
    {
        auto x = (juce::uint64) n * 0x9E3779B97F4A7C15ull + (juce::uint64) (channel + 1) * 0xD1B54A32D192ED03ull;
        x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 29; x *= 0x94D049BB133111EBull; x ^= x >> 32;
        auto steps = (int) (x & 0x7fffff) - 0x400000;      // +/- 2^22 of 2^23
        return (float) steps / 8388608.0f;
    }

    constexpr float oneStep24 = 1.0f / 8388608.0f;

    //==============================================================================
    /** One engine, one project and a pretend interface, pumped a buffer at a time. */
    struct Rig
    {
        Rig (const juce::File& dir, const juce::String& name, double sampleRate, int block, int numInputs,
             int inputLatency = 0, int outputLatency = 0)
            : rate (sampleRate), blockSize (block), inputs (numInputs),
              engine ("Elsewhere Record Test", nullptr, std::make_unique<Behaviour>()),
              hosted (engine.getDeviceManager().getHostedAudioDeviceInterface())
        {
            te::HostedAudioDeviceInterface::Parameters params;
            params.sampleRate = rate;
            params.blockSize = blockSize;
            params.inputChannels = inputs;
            params.outputChannels = 2;
            params.inputLatencyNumSamples = inputLatency;
            params.outputLatencyNumSamples = outputLatency;
            hosted.initialise (params);
            hosted.prepareToPlay (rate, blockSize);
            engine.getDeviceManager().dispatchPendingUpdates();

            project = std::make_unique<Project> (engine, Project::createProjectFolder (dir, name));
            auto& edit = project->getEdit();
            edit.ensureNumberOfAudioTracks (inputs);
            edit.getTransport().ensureContextAllocated();

            // One mono input to each track, armed, not monitored
            int next = 0;
            auto tracks = te::getAudioTracks (edit);

            for (auto instance : edit.getAllInputDevices())
            {
                auto wave = dynamic_cast<te::WaveInputDevice*> (&instance->getInputDevice());

                if (wave == nullptr || wave->getChannels().size() != 1 || next >= inputs)
                    continue;

                wave->setMonitorMode (te::InputDevice::MonitorMode::off);
                auto& track = *tracks[next++];
                track.setName ("In" + juce::String (next));
                [[maybe_unused]] auto res = instance->setTarget (track.itemID, true, nullptr, 0);
                instance->setRecordingEnabled (track.itemID, true);
            }

            armed = next;
            edit.dispatchPendingUpdatesSynchronously();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        }

        ~Rig()
        {
            project.reset();
            engine.getDeviceManager().deviceManager.closeAudioDevice();
            engine.getDeviceManager().removeHostedAudioDeviceInterface();
        }

        te::Edit& edit()                { return project->getEdit(); }
        te::TransportControl& transport()   { return edit().getTransport(); }
        juce::File recordedFolder()     { return project->getFolder().getChildFile ("Samples/Recorded"); }

        /** Runs the engine for numSamples, feeding the test signal. Remembers where the playhead
            was for each buffer, so a recorded sample's place on the timeline can be checked. */
        void pump (juce::int64 numSamples)
        {
            juce::AudioBuffer<float> buffer (juce::jmax (2, inputs), blockSize);
            juce::MidiBuffer midi;
            auto* context = transport().getCurrentPlaybackContext();

            while (numSamples > 0)
            {
                auto n = (int) juce::jmin ((juce::int64) blockSize, numSamples);
                juce::AudioBuffer<float> block (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), n);
                block.clear();

                for (int ch = 0; ch < inputs; ++ch)
                    for (int i = 0; i < n; ++i)
                        block.setSample (ch, i, testSample (ch, fed + i));

                hosted.processBlock (block, midi);

                if (firstLoudOutput < 0)
                    for (int i = 0; i < n && firstLoudOutput < 0; ++i)
                        if (std::abs (block.getSample (0, i)) > 0.25f)
                            firstLoudOutput = fed + i;

                blockStarts.push_back (fed);
                playheadAfter.push_back (context != nullptr ? context->getPosition().inSeconds() : 0.0);
                fed += n;
                numSamples -= n;
            }
        }

        /** Whole buffers only, as an interface delivers them. */
        void pumpSeconds (double s)     { pump ((juce::int64) std::llround (s * rate / blockSize) * blockSize); }

        /** Where the playhead was when input sample n went in. */
        double playheadAt (juce::int64 n) const
        {
            auto it = std::upper_bound (blockStarts.begin(), blockStarts.end(), n);
            auto b = (size_t) juce::jmax ((std::ptrdiff_t) 0, (it - blockStarts.begin()) - 1);
            auto blockEnd = b + 1 < blockStarts.size() ? blockStarts[b + 1] : fed;
            return playheadAfter[b] - (double) (blockEnd - n) / rate;
        }

        void settle (int ms = 300)      { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

        double rate;
        int blockSize, inputs, armed = 0;
        te::Engine engine;
        te::HostedAudioDeviceInterface& hosted;
        std::unique_ptr<Project> project;
        juce::int64 fed = 0;
        juce::int64 firstLoudOutput = -1;     // when the first loud sample came out of the outputs
        std::vector<juce::int64> blockStarts;
        std::vector<double> playheadAfter;
    };

    //==============================================================================
    /** A recorded file, matched against the signal that went in. */
    struct Match
    {
        bool readable = false;
        juce::int64 length = 0;
        int bits = 0, channels = 0;
        double fileRate = 0;
        juce::int64 firstInput = -1;        // the input sample the file starts with
        int breaks = 0;                     // places where the file jumps to another part of the input
        juce::int64 missing = 0;            // input samples skipped at those places
        juce::int64 unmatched = 0;          // samples that match nothing nearby
        float worstError = 0;               // biggest difference where it does match
        std::vector<std::pair<juce::int64, juce::int64>> runs;   // file sample -> input sample, at the start of each run
    };

    Match matchFile (te::Engine& engine, const juce::File& file, int channel, juce::int64 inputLength)
    {
        Match m;
        std::unique_ptr<juce::AudioFormatReader> reader (engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));

        if (reader == nullptr)
            return m;

        m.readable = true;
        m.length = reader->lengthInSamples;
        m.bits = (int) reader->bitsPerSample;
        m.channels = (int) reader->numChannels;
        m.fileRate = reader->sampleRate;

        juce::AudioBuffer<float> data ((int) reader->numChannels, (int) m.length);
        reader->read (&data, 0, (int) m.length, 0, true, true);
        auto* d = data.getReadPointer (0);
        const float tolerance = 2.5f * oneStep24;

        auto matchesAt = [&] (juce::int64 k, juce::int64 n, int count)
        {
            if (n < 0 || k + count > m.length)
                return false;

            for (int i = 0; i < count; ++i)
                if (std::abs (d[k + i] - testSample (channel, n + i)) > tolerance)
                    return false;

            return true;
        };

        auto findInput = [&] (juce::int64 k, juce::int64 from) -> juce::int64
        {
            const int probe = (int) juce::jmin ((juce::int64) 32, m.length - k);

            if (probe < 8)
                return -1;

            for (juce::int64 n = juce::jmax ((juce::int64) 0, from); n < inputLength; ++n)
                if (matchesAt (k, n, probe))
                    return n;

            return -1;
        };

        juce::int64 k = 0, n = findInput (0, 0);

        // Silence (or anything else) before the first matching sample
        while (n < 0 && k < m.length - 64)
        {
            ++k; ++m.unmatched;
            n = findInput (k, 0);
        }

        if (n < 0)
        {
            m.unmatched = m.length;
            return m;
        }

        m.firstInput = n - k;
        m.runs.push_back ({ k, n });

        while (k < m.length)
        {
            auto diff = std::abs (d[k] - testSample (channel, n));

            if (diff <= tolerance)
            {
                m.worstError = juce::jmax (m.worstError, diff);
                ++k; ++n;
                continue;
            }

            // Doesn't follow on: find where in the input the file carries on from. Loop passes go
            // back over time they have already covered, but the input itself only moves forward
            auto next = findInput (k, n);

            if (next < 0)
            {
                ++m.unmatched; ++k; ++n;
                continue;
            }

            ++m.breaks;
            m.missing += next - n;
            n = next;
            m.runs.push_back ({ k, n });
        }

        return m;
    }

    juce::Array<juce::File> recordedFiles (Rig& rig)
    {
        auto files = rig.recordedFolder().findChildFiles (juce::File::findFiles, false, "*.wav");
        files.sort();
        return files;
    }

    juce::String samplesText (double s)     { return juce::String (s, 2) + " samples"; }

    //==============================================================================
    /** A straight take: is every sample there, in order, where it was played? */
    void straightTake (Log& log, const juce::File& dir, double rate, int block, int inLatency = 0, int outLatency = 0)
    {
        auto name = juce::String ((int) rate) + " Hz, buffer " + juce::String (block)
                      + (inLatency + outLatency > 0 ? ", latency in " + juce::String (inLatency) + " out " + juce::String (outLatency) : juce::String());
        log.title ("Straight take: " + name);

        Rig rig (dir, "take " + juce::String ((int) rate) + " " + juce::String (block) + " " + juce::String (inLatency), rate, block, 1, inLatency, outLatency);
        log.check (rig.armed == 1, "one input armed");

        // Something to play along to: a click on another track, 1000 samples into a clip at 1.5 s
        const double clickTime = 1.5 + 1000.0 / rate;
        {
            auto clickFile = rig.project->getFolder().getChildFile ("Samples/Imported/click.wav");
            juce::AudioBuffer<float> click (1, (int) rate);
            click.clear();
            click.setSample (0, 1000, 0.9f);
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> out (clickFile.createOutputStream().release());

            if (std::unique_ptr<juce::AudioFormatWriter> w { wav.createWriterFor (out.get(), rate, 1, 24, {}, 0) })
            {
                out.release();
                w->writeFromAudioSampleBuffer (click, 0, click.getNumSamples());
            }

            auto clickTrack = rig.edit().insertNewAudioTrack (te::TrackInsertPoint (nullptr, te::getAllTracks (rig.edit()).getLast()), nullptr);
            auto c = te::insertWaveClip (*clickTrack, "click", clickFile, { { te::TimePosition::fromSeconds (1.5), te::TimePosition::fromSeconds (2.5) } },
                                         te::DeleteExistingClips::no);
            c->setUsesProxy (false);
            c->setAutoTempo (false);
            rig.edit().dispatchPendingUpdatesSynchronously();
            rig.settle (500);
        }

        rig.transport().setPosition (1s);
        rig.pumpSeconds (0.25);
        rig.firstLoudOutput = -1;
        auto recordFrom = rig.fed;
        EngineHelpers::toggleRecord (rig.edit());
        rig.pumpSeconds (4.0);
        auto recordTo = rig.fed;
        EngineHelpers::toggleRecord (rig.edit());
        rig.settle();
        rig.pumpSeconds (0.1);

        auto track = te::getAudioTracks (rig.edit())[0];
        auto clip = track->getClips().isEmpty() ? nullptr : dynamic_cast<te::WaveAudioClip*> (track->getClips()[0]);
        log.check (clip != nullptr, "a clip was put on the track");

        if (clip == nullptr)
            return;

        auto file = clip->getSourceFileReference().getFile();
        auto m = matchFile (rig.engine, file, 0, rig.fed);
        log.check (m.readable, "the recording opens: " + file.getFileName());
        log.note (juce::String (m.bits) + "-bit, " + juce::String (m.fileRate) + " Hz, " + juce::String (m.channels) + " channel(s), "
                  + juce::String (m.length) + " samples");
        log.check (m.bits == 24 && juce::approximatelyEqual (m.fileRate, rate), "24-bit at the interface's rate");
        log.check (m.breaks == 0 && m.unmatched == 0, "every sample in order (" + juce::String (m.breaks) + " breaks, "
                                                      + juce::String (m.unmatched) + " that match nothing)");
        log.check (m.worstError <= 1.01f * oneStep24, "within one 24-bit step (worst " + juce::String (m.worstError / oneStep24, 2) + " steps, "
                                                      + juce::String (juce::Decibels::gainToDecibels (juce::jmax (m.worstError, 1.0e-12f)), 1) + " dBFS)");

        if (m.firstInput < 0)
            return;

        auto lostAtStart = m.firstInput - recordFrom;
        auto lostAtEnd = recordTo - (m.firstInput + m.length);
        log.note ("starts " + juce::String (lostAtStart) + " samples after record was pressed, ends " + juce::String (lostAtEnd)
                  + " samples before stop (buffer " + juce::String (block) + ", input latency " + juce::String (inLatency) + ")");
        log.check (lostAtStart >= 0 && lostAtStart <= block + inLatency, "nothing missing at the start beyond one buffer and the input latency");
        log.check (std::abs (lostAtEnd) <= block + inLatency, "nothing missing at the end beyond one buffer and the input latency");

        auto pos = clip->getPosition();
        auto placed = pos.getStart().inSeconds() - pos.getOffset().inSeconds();
        log.note ("clip " + juce::String (pos.getStart().inSeconds(), 5) + " to " + juce::String (pos.getEnd().inSeconds(), 5) + " s, offset "
                  + juce::String (pos.getOffset().inSeconds(), 5) + " s");
        log.check (rig.firstLoudOutput >= 0, "the click on the other track came out of the outputs");

        if (rig.firstLoudOutput >= 0)
        {
            // What went in as the click came out of the interface (which is the output latency
            // after the engine played it) belongs at the click's place on the timeline
            auto fileSample = rig.firstLoudOutput - m.firstInput;
            auto landed = placed + (double) fileSample / rate;
            auto byEar = (landed - clickTime) * rate;
            log.check (std::abs (byEar) < 1.0, "what is played on the click lands on the click (" + samplesText (byEar) + " late)");
        }
    }

    //==============================================================================
    /** Punch: only the brace is recorded, and it lands in the brace. */
    void punchTake (Log& log, const juce::File& dir)
    {
        log.title ("Punch 2 to 4 s, one bar of pre-roll, 48000 Hz, buffer 256");
        const double rate = 48000.0;
        Rig rig (dir, "punch", rate, 256, 1);
        auto& edit = rig.edit();

        rig.transport().setLoopRange ({ te::TimePosition::fromSeconds (2.0), te::TimePosition::fromSeconds (4.0) });
        EngineHelpers::setPunching (edit, true);
        EngineHelpers::setPreRollBars (edit, 1);
        rig.transport().setPosition (0s);
        rig.pumpSeconds (0.1);
        EngineHelpers::toggleRecord (edit);
        rig.pumpSeconds (6.0);
        EngineHelpers::toggleRecord (edit);
        rig.settle();

        auto track = te::getAudioTracks (edit)[0];
        auto clip = track->getClips().isEmpty() ? nullptr : dynamic_cast<te::WaveAudioClip*> (track->getClips()[0]);
        log.check (clip != nullptr, "a clip was put on the track");

        if (clip == nullptr)
            return;

        auto pos = clip->getPosition();
        auto m = matchFile (rig.engine, clip->getSourceFileReference().getFile(), 0, rig.fed);
        log.note ("clip " + juce::String (pos.getStart().inSeconds(), 5) + " to " + juce::String (pos.getEnd().inSeconds(), 5) + " s, offset "
                  + juce::String (pos.getOffset().inSeconds(), 5) + " s; file " + juce::String (m.length) + " samples");
        log.check (std::abs (pos.getStart().inSeconds() - 2.0) * rate < 1.0, "the clip starts at the punch-in (" + samplesText ((pos.getStart().inSeconds() - 2.0) * rate) + " out)");
        log.check (std::abs (pos.getEnd().inSeconds() - 4.0) * rate < 1.0, "and ends at the punch-out (" + samplesText ((pos.getEnd().inSeconds() - 4.0) * rate) + " out)");
        log.check (m.breaks == 0 && m.unmatched == 0, "every sample in order (" + juce::String (m.breaks) + " breaks, " + juce::String (m.unmatched) + " that match nothing)");

        if (m.firstInput >= 0)
        {
            auto error = (pos.getStart().inSeconds() - pos.getOffset().inSeconds() - rig.playheadAt (m.firstInput)) * rate;
            log.check (std::abs (error) < 1.0, "placed where it was played (" + samplesText (error) + " out)");
        }
    }

    //==============================================================================
    /** Loop recording: nothing is lost where the loop wraps. */
    void loopTake (Log& log, const juce::File& dir)
    {
        log.title ("Loop 1 to 3 s, three and a half passes, 44100 Hz, buffer 512");
        const double rate = 44100.0;
        Rig rig (dir, "loop", rate, 512, 1);
        auto& edit = rig.edit();

        rig.transport().setLoopRange ({ te::TimePosition::fromSeconds (1.0), te::TimePosition::fromSeconds (3.0) });
        EngineHelpers::setLooping (edit, true);
        rig.transport().setPosition (0s);
        rig.pumpSeconds (0.1);
        auto recordFrom = rig.fed;
        EngineHelpers::toggleRecord (edit);
        rig.pumpSeconds (7.0);
        auto recordTo = rig.fed;
        EngineHelpers::toggleRecord (edit);
        rig.settle();

        auto files = recordedFiles (rig);
        log.note (juce::String (files.size()) + " file(s) in Samples/Recorded");
        juce::int64 total = 0, expectedNext = -1;
        bool contiguous = true, clean = true;

        for (auto& f : files)
        {
            auto m = matchFile (rig.engine, f, 0, rig.fed);
            log.note (f.getFileName() + ": " + juce::String (m.length) + " samples from input sample " + juce::String (m.firstInput - recordFrom)
                      + ", " + juce::String (m.breaks) + " breaks, " + juce::String (m.missing) + " missing, " + juce::String (m.unmatched) + " that match nothing");
            clean = clean && m.readable && m.breaks == 0 && m.unmatched == 0;

            if (expectedNext >= 0 && m.firstInput != expectedNext)
                contiguous = false;

            expectedNext = m.firstInput + m.length;
            total += m.length;
        }

        log.check (! files.isEmpty() && clean, "each recorded file is unbroken");
        log.check (contiguous, "each file carries on from the sample the one before ended on");
        log.check (std::abs ((recordTo - recordFrom) - total) <= 2 * 512, "all 7 s is on disk (" + juce::String (recordTo - recordFrom - total) + " samples short)");

        auto track = te::getAudioTracks (edit)[0];
        juce::StringArray clips;

        for (auto c : track->getClips())
            clips.add (juce::String (c->getPosition().getStart().inSeconds(), 3) + "-" + juce::String (c->getPosition().getEnd().inSeconds(), 3));

        log.note ("clips on the track: " + clips.joinIntoString (" "));
    }

    //==============================================================================
    /** Sixteen inputs at once at 96 kHz, as fast as the engine will go: does the disk keep up? */
    void manyInputs (Log& log, const juce::File& dir)
    {
        const int inputs = 16;
        const double rate = 96000.0, seconds = 30.0;
        log.title ("16 inputs at once, 96000 Hz, buffer 128, 30 s as fast as it will go");
        Rig rig (dir, "sixteen", rate, 128, inputs);
        log.check (rig.armed == inputs, juce::String (rig.armed) + " inputs armed");

        rig.transport().setPosition (0s);
        rig.pumpSeconds (0.1);
        auto recordFrom = rig.fed;
        EngineHelpers::toggleRecord (rig.edit());
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        rig.pumpSeconds (seconds);
        auto pumped = juce::Time::getMillisecondCounterHiRes() - t0;
        auto recordTo = rig.fed;
        EngineHelpers::toggleRecord (rig.edit());
        auto stopped = juce::Time::getMillisecondCounterHiRes() - t0 - pumped;
        rig.settle();

        log.note (juce::String (seconds) + " s of audio in " + juce::String (pumped / 1000.0, 2) + " s (" + juce::String (seconds * 1000.0 / pumped, 1)
                  + " times real time); stopping and finishing the files took " + juce::String (stopped, 0) + " ms");

        int good = 0;
        juce::int64 bytes = 0;
        auto tracks = te::getAudioTracks (rig.edit());

        for (int i = 0; i < inputs; ++i)
        {
            auto clip = tracks[i]->getClips().isEmpty() ? nullptr : dynamic_cast<te::WaveAudioClip*> (tracks[i]->getClips()[0]);

            if (clip == nullptr)
                continue;

            auto file = clip->getSourceFileReference().getFile();
            bytes += file.getSize();
            auto m = matchFile (rig.engine, file, i, rig.fed);
            auto shortBy = (recordTo - recordFrom) - m.length;

            if (m.readable && m.breaks == 0 && m.unmatched == 0 && m.worstError <= 1.01f * oneStep24 && std::abs (shortBy) <= 256)
                ++good;
            else
                log.note ("input " + juce::String (i + 1) + ": " + juce::String (m.breaks) + " breaks, " + juce::String (m.missing) + " missing, "
                          + juce::String (m.unmatched) + " that match nothing, " + juce::String (shortBy) + " samples short");
        }

        log.check (good == inputs, juce::String (good) + " of " + juce::String (inputs) + " recordings complete, in order, and carrying their own input");
        log.note (juce::String ((double) bytes / 1048576.0, 0) + " MB written, " + juce::String ((double) bytes / 1048576.0 / (pumped / 1000.0), 0) + " MB/s");
    }

    //==============================================================================
    /** Wastes time in the audio callback, as a heavy project or a slow plug-in would. */
    struct Load : public juce::AudioIODeviceCallback
    {
        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            auto period = numSamples / rate * 1000.0;
            auto busy = period * steady;

            if (stallEvery > 0 && ++count % stallEvery == 0)
            {
                busy = period * stall;
                ++stalls;
            }

            auto until = juce::Time::getMillisecondCounterHiRes() + busy;

            while (juce::Time::getMillisecondCounterHiRes() < until) {}
        }

        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}

        double rate = 48000.0, steady = 0.0, stall = 0.0;
        int stallEvery = 0, count = 0, stalls = 0;
    };

    /** Calls the engine once per buffer period from a real-time thread, like a device. */
    struct Driver : public juce::Thread
    {
        Driver (Rig& r, double secs) : juce::Thread ("Record test audio"), rig (r), seconds (secs) {}

        void run() override
        {
            auto period = rig.blockSize / rig.rate * 1000.0;
            auto next = juce::Time::getMillisecondCounterHiRes();
            auto blocks = (int) (seconds * rig.rate / rig.blockSize);

            for (int i = 0; i < blocks && ! threadShouldExit(); ++i)
            {
                rig.pump (rig.blockSize);
                next += period;
                auto wait = next - juce::Time::getMillisecondCounterHiRes();

                if (wait > 0)
                    juce::Thread::sleep ((int) wait);
                else
                    next = juce::Time::getMillisecondCounterHiRes();    // late: carry on from now, as a device does
            }
        }

        Rig& rig;
        double seconds;
    };

    /** Records in real time while the audio callback is kept busy and now and then runs late.
        The pretend interface never loses input itself (a real one may, when the callback is late),
        so anything missing from the take here was thrown away by the engine. */
    void underLoad (Log& log, const juce::File& dir, double steady, double stall, int& lostOut)
    {
        const double rate = 48000.0, seconds = 12.0;
        const int block = 256;
        auto name = "steady load " + juce::String (juce::roundToInt (steady * 100)) + "%, a callback taking "
                      + juce::String (stall, 1) + " buffers every 2 s";
        Rig rig (dir, "load " + juce::String (juce::roundToInt (steady * 100)) + " " + juce::String (juce::roundToInt (stall * 10)),
                 rate, block, 1);

        Load load;
        load.rate = rate;
        load.steady = steady;
        load.stall = stall;
        load.stallEvery = stall > 0 ? (int) (2.0 * rate / block) : 0;
        rig.engine.getDeviceManager().deviceManager.addAudioCallback (&load);

        rig.transport().setPosition (0s);
        EngineHelpers::toggleRecord (rig.edit());

        {
            Driver driver (rig, seconds);
            driver.startRealtimeThread (juce::Thread::RealtimeOptions().withPeriodMs (block / rate * 1000.0));

            while (driver.isThreadRunning())
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }

        EngineHelpers::toggleRecord (rig.edit());
        rig.settle();
        rig.engine.getDeviceManager().deviceManager.removeAudioCallback (&load);

        auto files = recordedFiles (rig);
        Match m;

        if (! files.isEmpty())
            m = matchFile (rig.engine, files[0], 0, rig.fed);

        lostOut = (int) m.missing;
        log.note (name + ": " + juce::String (load.stalls) + " late callbacks, " + juce::String (m.breaks) + " breaks in the take, "
                  + juce::String (m.missing) + " samples missing (" + juce::String ((double) m.missing * 1000.0 / rate, 1) + " ms)");
    }
}

int run (const juce::StringArray& args)
{
    auto outDir = juce::File (args[args.indexOf ("--record-test") + 1]);
    outDir.deleteRecursively();
    outDir.createDirectory();
    Log log;

    for (auto rate : { 44100.0, 48000.0, 96000.0 })
        for (auto block : { 64, 256, 1024 })
            straightTake (log, outDir, rate, block);

    straightTake (log, outDir, 48000.0, 256, 96, 128);
    straightTake (log, outDir, 44100.0, 512, 1000, 250);
    punchTake (log, outDir);
    loopTake (log, outDir);
    manyInputs (log, outDir);

    log.title ("A late audio callback while recording, 48000 Hz, buffer 256, 12 s in real time");
    int lost = 0, lostQuiet = 0;
    underLoad (log, outDir, 0.3, 0.0, lostQuiet);
    log.check (lostQuiet == 0, "nothing missing with no late callbacks");

    for (auto [steady, stall] : { std::pair { 0.3, 1.5 }, { 0.3, 5.0 }, { 0.6, 3.0 }, { 0.85, 2.0 } })
    {
        int missing = 0;
        underLoad (log, outDir, steady, stall, missing);
        lost += missing;
    }

    log.check (lost == 0, "late callbacks cost the take nothing (" + juce::String (lost) + " samples missing in all)");

    // A project that can't be written says so (quitting and opening another project rely on it)
    {
        log.title ("Saving");
        Rig rig (outDir, "save", 48000.0, 256, 1);
        rig.edit().ensureNumberOfAudioTracks (3);
        log.check (rig.project->save (false), "a save that worked says so");

        auto folder = rig.project->getFolder();
        rig.edit().ensureNumberOfAudioTracks (4);
        rig.edit().markAsChanged();
        folder.setReadOnly (true, false);
        auto saved = rig.project->save (false);
        folder.setReadOnly (false, false);
        log.check (! saved, "a save into a folder that can't be written says it failed");
        log.check (rig.project->hasChangedSinceSaved(), "and the project still counts as changed");
        log.check (rig.project->save (false) && ! rig.project->hasChangedSinceSaved(), "saving again once the folder is writable works");
    }

    // Unsaved changes outlive a crash, and a damaged file is never opened as an empty project
    {
        log.title ("Recovery");
        Rig rig (outDir, "recover", 48000.0, 256, 1);
        auto file = rig.project->getEditFile();
        auto recovery = Project::getRecoveryFile (rig.engine, file);
        auto kept = recovery.getSiblingFile ("kept");
        auto backups = rig.project->getFolder().getChildFile ("Backups");
        auto trackName = [&] { return te::getAudioTracks (rig.edit())[0]->getName(); };
        auto change = [&] (const juce::String& name)
        {
            te::getAudioTracks (rig.edit())[0]->setName (name);
            rig.edit().markAsChanged();
        };
        auto reopen = [&] (bool recover)
        {
            rig.project.reset();
            rig.project = std::make_unique<Project> (rig.engine, file, recover);
        };
        auto crash = [&]    // the session ends without closing: its recovery copy is left behind
        {
            recovery.copyFileTo (kept);
            rig.project.reset();
            kept.moveFileTo (recovery);
            rig.project = std::make_unique<Project> (rig.engine, file);
        };

        recovery.deleteFile();
        rig.project->save (false);
        log.check (! rig.project->updateRecovery() && ! recovery.exists(), "nothing is kept while nothing has changed");

        change ("Not saved yet");
        log.check (rig.project->updateRecovery() && Project::isReadable (recovery), "unsaved changes are kept");
        log.check (! rig.project->updateRecovery(), "and not written again until something else changes");

        reopen (false);
        log.check (! recovery.exists() && ! rig.project->hasRecoveryPending(), "closing properly leaves nothing to recover");

        change ("Not saved yet");
        rig.project->updateRecovery();
        crash();
        log.check (rig.project->hasRecoveryPending() && Project::getRecoveryTime (rig.engine, file).has_value() && trackName() == "In1",
                   "after a crash the project opens as last saved, with the changes waiting");

        change ("Something else");
        log.check (! rig.project->updateRecovery(), "they aren't written over before the question is answered");
        reopen (false);
        log.check (rig.project->hasRecoveryPending(), "or lost by closing without answering");

        reopen (true);
        log.check (trackName() == "Not saved yet" && rig.project->hasChangedSinceSaved(), "Recover opens them, as changes still to save");
        log.check (rig.project->save (false) && ! recovery.exists(), "saving them leaves nothing to recover");

        change ("Set aside");
        rig.project->updateRecovery();
        crash();
        rig.project->setRecoveryAside();
        log.check (! recovery.exists() && ! rig.project->hasRecoveryPending()
                    && backups.findChildFiles (juce::File::findFiles, false, "* unsaved.elsewhere").size() == 1,
                   "Open Last Saved puts them in Backups for Version History");

        // A project file cut short, as a half-finished sync leaves it
        rig.project.reset();
        file.replaceWithText (file.loadFileAsString().substring (0, 300));
        auto size = file.getSize();
        log.check (Project::isDamaged (file), "a file cut short is seen as damaged");

        {
            Project p (rig.engine, file);
            log.check (p.isUnreadable() && ! p.save (false) && file.getSize() == size, "and can't be saved over, even if it's opened");
        }

        auto used = Project::restoreLatestBackup (file);
        log.check (used.has_value() && Project::isReadable (file) && ! Project::isDamaged (file), "the latest backup opens in its place");
        log.check (backups.findChildFiles (juce::File::findFiles, false, "*.damaged").size() == 1, "and the damaged file is kept");
        rig.project = std::make_unique<Project> (rig.engine, file);
        log.check (trackName() == "Not saved yet", "with what was last saved (" + trackName() + ")");
    }

    log.text << "\n" << (log.failures == 0 ? "ALL RECORD TESTS PASSED" : juce::String (log.failures) + " failed") << "\n";
    outDir.getChildFile ("record-test.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::RecordTests
