// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Building blocks for hearing someone else's audio in real time, shared by live jamming
    (Opus over WebRTC, src/Live.cpp) and Somewhere monitoring (PCM over UDP, src/SomewhereLink.h).

    A network thread feeds a Receiver, which follows the sender's clock and fills a JitterBuffer;
    the audio thread plays from the JitterBuffer.
*/
namespace Realtime
{
    static constexpr int capacity = 96000;

    //==============================================================================
    /**
        A lock-free FIFO that holds back playback until it has `targetSamples` queued, so
        uneven packet arrival doesn't cause gaps. If it runs dry, it waits to refill; if too
        much builds up (after a network hiccup), the excess is skipped so latency can't creep.
    */
    struct JitterBuffer
    {
        juce::AbstractFifo fifo { capacity };
        juce::AudioBuffer<float> buffer { 2, capacity };
        std::atomic<int> targetSamples { 1024 };
        std::atomic<bool> active { false }, primed { false };
        std::atomic<int> underruns { 0 };
        std::atomic<float> driftPpm { 0.0f };       // clock-drift correction currently applied
        bool resuming = false;                       // audio thread: fade in after running dry

        void reset()    { fifo.reset(); primed = false; underruns = 0; driftPpm = 0.0f; resuming = false; }

        /** Network thread: adds audio. */
        void push (const float* left, const float* right, int n)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (n, s1, n1, s2, n2);
            buffer.copyFrom (0, s1, left, n1);           buffer.copyFrom (1, s1, right, n1);
            if (n2 > 0) { buffer.copyFrom (0, s2, left + n1, n2); buffer.copyFrom (1, s2, right + n1, n2); }
            fifo.finishedWrite (n1 + n2);
        }

        /** Audio thread: adds up to n samples into the outputs; returns the block's peak. Running dry
            fades out, and starting again fades in (about 1.5 ms), so a Wi-Fi hiccup doesn't click. */
        float mixInto (float* left, float* right, int n, float gain)
        {
            if (! active)
                return 0.0f;

            auto ready = fifo.getNumReady();
            auto target = targetSamples.load();

            if (! primed)
            {
                if (ready < target)
                    return 0.0f;
                primed = true;
                resuming = true;
            }

            if (ready > target * 2 + 2048)
            {
                fifo.finishedRead (ready - target);
                ready = target;
            }

            auto toRead = juce::jmin (n, ready);
            const bool runningDry = toRead < n;
            const int fade = juce::jmin (64, toRead);
            int s1, n1, s2, n2;
            fifo.prepareToRead (toRead, s1, n1, s2, n2);
            float peak = 0.0f;

            for (int ch = 0; ch < 2; ++ch)
            {
                auto* out = ch == 0 ? left : right;
                if (out == nullptr)
                    continue;

                for (int i = 0; i < toRead; ++i)
                {
                    auto v = i < n1 ? buffer.getSample (ch, s1 + i) : buffer.getSample (ch, s2 + i - n1);
                    auto g = gain;
                    if (resuming && i < fade)              g *= (float) i / (float) fade;
                    if (runningDry && i >= toRead - fade)  g *= (float) (toRead - 1 - i) / (float) fade;
                    out[i] += v * g;
                    peak = juce::jmax (peak, std::abs (v * g));
                }
            }

            resuming = false;
            fifo.finishedRead (n1 + n2);

            if (runningDry)
            {
                ++underruns;      // ran dry: wait to refill before playing again
                primed = false;
            }

            return peak;
        }

        /** Grows the buffer when it ran dry since last time, shrinks it slowly when steady. Call once a second. */
        void adapt (int& lastUnderruns, int& stableSeconds, double sampleRate, double minMs = 10.0, double maxMs = 150.0,
                    int shrinkAfterSeconds = 10)
        {
            auto ms = [sampleRate] (double m) { return (int) (m * sampleRate / 1000.0); };
            auto u = underruns.load();
            auto target = targetSamples.load();

            if (u > lastUnderruns)
            {
                targetSamples = juce::jmin (ms (maxMs), target + ms (5));
                stableSeconds = 0;
            }
            else if (++stableSeconds >= shrinkAfterSeconds && target > ms (minMs))
            {
                targetSamples = juce::jmax (ms (minMs), target - ms (2));
                stableSeconds = 0;
            }

            lastUnderruns = u;
        }
    };

    //==============================================================================
    /** Streaming resampler (sender rate <-> device rate) that keeps its state between calls. */
    struct Resampler
    {
        double nominal = 1.0;  // input rate / output rate
        double ratio = 1.0;    // nominal, adjusted for clock drift
        juce::LagrangeInterpolator interp[2];
        juce::AudioBuffer<float> pending { 2, 0 };
        int pendingCount = 0;

        void setRates (double inRate, double outRate)
        {
            nominal = ratio = inRate / outRate;
            interp[0].reset(); interp[1].reset();
            pendingCount = 0;
        }

        bool isPassThrough() const  { return juce::approximatelyEqual (ratio, 1.0); }

        /** Adjusts the ratio by a tiny amount (parts per million) to follow the other clock. */
        void setCorrection (double ppm)     { ratio = nominal * (1.0 + ppm * 1.0e-6); }

        void addInput (const float* l, const float* r, int n)
        {
            if (pending.getNumSamples() < pendingCount + n)
                pending.setSize (2, pendingCount + n + 4096, true);

            pending.copyFrom (0, pendingCount, l, n);
            pending.copyFrom (1, pendingCount, r, n);
            pendingCount += n;
        }

        /** Produces up to `wanted` output samples if there's enough input; returns how many. */
        int produce (float* outL, float* outR, int wanted)
        {
            auto needed = (int) std::ceil (wanted * ratio) + 4;

            if (pendingCount < needed)
                return 0;

            auto used = interp[0].process (ratio, pending.getReadPointer (0), outL, wanted);
            interp[1].process (ratio, pending.getReadPointer (1), outR, wanted);

            // Drop the input we consumed
            for (int ch = 0; ch < 2; ++ch)
                std::memmove (pending.getWritePointer (ch), pending.getReadPointer (ch) + used, sizeof (float) * (size_t) (pendingCount - used));

            pendingCount -= used;
            return wanted;
        }
    };

    //==============================================================================
    /**
        Takes the other side's audio (already decoded) as it arrives on the network thread,
        follows their clock, and fills a JitterBuffer.

        Clock-drift correction: two audio interfaces never run at exactly the same rate, so the
        jitter buffer slowly fills or drains. A gentle PI controller watches its (smoothed) level
        and adjusts the resampling ratio by a few ppm so it stays at its target.
    */
    struct Receiver
    {
        Receiver (JitterBuffer& jb, double senderRate, double deviceRate, double simulatedDriftPpm = 0.0)
            : stream (jb), rate (deviceRate)
        {
            resampler.setRates (senderRate * (1.0 + simulatedDriftPpm * 1.0e-6), deviceRate);
            stream.reset();
            stream.active = true;
        }

        virtual ~Receiver()     { stream.active = false; }

        /** `packetSeconds` is how much audio each call usually carries (for the smoothing). */
        void add (const float* l, const float* r, int n, double packetSeconds)
        {
            updateDriftCorrection (packetSeconds);

            // Always resampled (even 48k -> 48k) so the ratio can follow the other clock.
            // Convert as much as the input allows, in small steps, so no fractional samples are
            // lost (rounding 220.5 down to 220 per packet slowly drains the jitter buffer)
            resampler.addInput (l, r, n);
            float outL[64], outR[64];

            while (auto got = resampler.produce (outL, outR, 64))
                stream.push (outL, outR, got);
        }

        void updateDriftCorrection (double dt)
        {
            if (! correctDrift)
                return;

            if (! stream.primed.load())
            {
                smoothedFill = -1.0;
                return;
            }

            const double fill = (double) stream.fifo.getNumReady() + resampler.pendingCount / resampler.ratio;
            const double alpha = juce::jmin (1.0, 0.8 * dt);    // ~1.25 s time constant
            smoothedFill = smoothedFill < 0.0 ? fill : smoothedFill + alpha * (fill - smoothedFill);

            // The buffer error (seconds) changes at (drift - correction) x 1e-6 per second, so with
            // Kp = 2 zeta w / 1e-6 and Ki = w^2 / 1e-6 the loop is critically damped (zeta = 1)
            // with w = 0.1 rad/s: it settles in ~10 s and never audibly "hunts"
            const double errorSeconds = (smoothedFill - (double) stream.targetSamples.load()) / rate;
            constexpr double kp = 200000.0, ki = 10000.0;     // ppm per second of error, ppm per second^2
            integral = juce::jlimit (-2000.0, 2000.0, integral + ki * errorSeconds * dt);
            const double ppm = juce::jlimit (-2000.0, 2000.0, kp * errorSeconds + integral);

            resampler.setCorrection (ppm);
            stream.driftPpm = (float) ppm;
        }

        JitterBuffer& stream;
        double rate = 48000.0, smoothedFill = -1.0, integral = 0.0;
        bool correctDrift = true;
        Resampler resampler;
    };
}
} // namespace elsewhere
