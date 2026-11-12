// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PerfTest.h"
#include "Project.h"

namespace elsewhere::PerfTest
{
namespace
{
    /** Elsewhere's own engine settings (so devices get the names projects route to), with no
        system audio devices and a chosen number of audio threads. */
    struct Behaviour : public ElsewhereEngineBehaviour
    {
        explicit Behaviour (int cpus) : ElsewhereEngineBehaviour (false), numCpus (cpus) {}

        bool addSystemAudioIODeviceTypes() override     { return false; }
        int getNumberOfCPUsToUseForAudio() override     { return numCpus; }

        int numCpus;
    };

    struct Result
    {
        int cpus = 0, blocks = 0, overruns = 0;
        double budgetMs = 0, meanMs = 0, p99Ms = 0, maxMs = 0;
    };

    /** The "host" playhead: playing from the start, moving on one buffer per call. */
    struct HostPlayHead : public juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (true);
            info.setTimeInSamples (samples);
            info.setTimeInSeconds ((double) samples / rate);
            info.setBpm (bpm);
            info.setTimeSignature (TimeSignature { 4, 4 });
            info.setPpqPosition ((double) samples / rate * bpm / 60.0);
            return info;
        }

        juce::int64 samples = 0;
        double rate = 44100.0, bpm = 120.0;
    };

    /** Calls the engine once per buffer period from a real-time thread, like a device. */
    struct Driver : public juce::Thread
    {
        Driver (te::HostedAudioDeviceInterface& h, te::Edit& e, int block, double rate, int numBlocks)
            : juce::Thread ("Perf audio"), hosted (h), sync (e), blockSize (block), sampleRate (rate), total (numBlocks)
        {
            playHead.rate = rate;
            playHead.bpm = e.tempoSequence.getTempo (0)->getBpm();
        }

        void run() override
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            juce::MidiBuffer midi;
            auto period = blockSize / sampleRate;
            auto next = juce::Time::getMillisecondCounterHiRes();

            for (int i = 0; i < total && ! threadShouldExit(); ++i)
            {
                buffer.clear();
                auto t0 = juce::Time::getMillisecondCounterHiRes();
                sync.synchronise (playHead);
                hosted.processBlock (buffer, midi);
                times.push_back (juce::Time::getMillisecondCounterHiRes() - t0);
                playHead.samples += blockSize;
                peak = std::max (peak, buffer.getMagnitude (0, blockSize));

                next += period * 1000.0;
                auto wait = next - juce::Time::getMillisecondCounterHiRes();

                if (wait > 0)
                    juce::Thread::sleep ((int) wait);
            }
        }

        te::HostedAudioDeviceInterface& hosted;
        te::ExternalPlayheadSynchroniser sync;
        HostPlayHead playHead;
        int blockSize;
        double sampleRate;
        int total;
        std::vector<double> times;
        float peak = 0;
    };

    Result measure (const juce::File& editFile, int cpus, int blockSize, double seconds, juce::String& log)
    {
        const double rate = 44100.0;
        Result r;
        r.cpus = cpus;

        te::Engine engine ("Elsewhere Perf", nullptr, std::make_unique<Behaviour> (cpus));
        auto& hosted = engine.getDeviceManager().getHostedAudioDeviceInterface();
        te::HostedAudioDeviceInterface::Parameters params;
        params.sampleRate = rate;
        params.blockSize = blockSize;
        params.inputChannels = 0;
        params.outputChannels = 2;
        hosted.initialise (params);
        hosted.prepareToPlay (rate, blockSize);

        {
            Project project (engine, editFile);
            auto& edit = project.getEdit();
            int tracks = 0, clips = 0;

            for (auto t : te::getAudioTracks (edit))
            {
                ++tracks;
                clips += t->getClips().size();

                // Play from the start, from files on disk (not the rendered proxies' wait)
                for (auto* c : t->getClips())
                    if (auto* a = dynamic_cast<te::AudioClipBase*> (c))
                        a->beginRenderingNewProxyIfNeeded();
            }

            // Let any background renders (warped copies, proxies) finish first
            for (int i = 0; i < 1200; ++i)
            {
                bool busy = false;

                for (auto t : te::getAudioTracks (edit))
                    for (auto* c : t->getClips())
                        if (auto* a = dynamic_cast<te::AudioClipBase*> (c))
                            busy = busy || a->isGeneratingPlaybackFile();

                if (! busy)
                    break;

                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            }

            auto& transport = edit.getTransport();
            transport.ensureContextAllocated();
            transport.setPosition (0s);

            // Like a plug-in: the host's playhead says it's playing, and the edit follows it
            Driver driver (hosted, edit, blockSize, rate, (int) (seconds * rate / blockSize));
            driver.startRealtimeThread (juce::Thread::RealtimeOptions().withPeriodMs (blockSize / rate * 1000.0));

            while (driver.isThreadRunning())
                juce::MessageManager::getInstance()->runDispatchLoopUntil (20);


            if (cpus == 1)
                log << "transport playing " << (int) transport.isPlaying() << ", position " << juce::String (transport.getPosition().inSeconds(), 2)
                    << " s, context " << (int) (transport.getCurrentPlaybackContext() != nullptr) << "\n";

            transport.stop (false, true);

            auto times = driver.times;
            r.blocks = (int) times.size();
            r.budgetMs = blockSize / rate * 1000.0;

            if (! times.empty())
            {
                r.meanMs = std::accumulate (times.begin(), times.end(), 0.0) / (double) times.size();
                r.overruns = (int) std::count_if (times.begin(), times.end(), [&] (double t) { return t > r.budgetMs; });
                std::sort (times.begin(), times.end());
                r.p99Ms = times[(size_t) ((double) (times.size() - 1) * 0.99)];
                r.maxMs = times.back();
            }

            if (cpus == 1)
            {
                double firstClip = 1.0e9;
                for (auto t : te::getAudioTracks (edit))
                    for (auto* c : t->getClips())
                        firstClip = std::min (firstClip, c->getPosition().getStart().inSeconds());

                log << tracks << " audio tracks, " << clips << " clips (first at " << juce::String (firstClip, 2)
                    << " s); output peak " << juce::String (driver.peak, 3) << ", output device "
                    << (engine.getDeviceManager().getDefaultWaveOutDevice() != nullptr ? engine.getDeviceManager().getDefaultWaveOutDevice()->getName() : "none") << "\n";
            }
        }

        return r;
    }
}

int run (const juce::StringArray& args)
{
    auto i = args.indexOf ("--perf-test");
    auto editFile = juce::File (args[i + 1]);
    auto outDir = juce::File (args[i + 2]);
    auto blockSize = args[i + 3].getIntValue() > 0 ? args[i + 3].getIntValue() : 256;
    auto seconds = args[i + 4].getDoubleValue() > 0 ? args[i + 4].getDoubleValue() : 20.0;

    juce::String log;
    log << "Playing " << editFile.getFileName() << " for " << seconds << " s, buffer " << blockSize << " at 44.1 kHz\n";

    // --strategies: every thread pool strategy, with all the CPUs
    std::vector<std::pair<int, int>> runs;   // CPUs, strategy (-1 = the engine's default)

    if (args.contains ("--strategies"))
        for (int strategy = 0; strategy < 6; ++strategy)
            runs.push_back ({ juce::SystemStats::getNumCpus(), strategy });
    else
        for (auto cpus : { 1, 2, 4, juce::SystemStats::getNumCpus() })
            runs.push_back ({ cpus, -1 });

    const char* strategyNames[] = { "conditionVariable", "realTime", "hybrid", "semaphore", "lightweightSemaphore", "lightweightSemHybrid" };

    for (auto [cpus, strategy] : runs)
    {
        if (strategy >= 0)
            te::EditPlaybackContext::setThreadPoolStrategy (strategy);

        auto r = measure (editFile, cpus, blockSize, seconds, log);

        if (strategy >= 0)
            log << strategyNames[strategy] << ": ";

        log << juce::String (r.cpus).paddedLeft (' ', 2) << " CPUs (" << (r.cpus - 1) << " worker threads): "
            << "mean " << juce::String (r.meanMs, 3) << " ms, 99% " << juce::String (r.p99Ms, 3) << " ms, worst "
            << juce::String (r.maxMs, 3) << " ms of " << juce::String (r.budgetMs, 2) << " ms; late "
            << r.overruns << "/" << r.blocks << "\n";
    }

    outDir.createDirectory();
    outDir.getChildFile ("perf.txt").replaceWithText (log);
    std::cout << log;
    return 0;
}

} // namespace elsewhere::PerfTest
