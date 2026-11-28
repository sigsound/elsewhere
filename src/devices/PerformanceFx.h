// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Dsp.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace elsewhere
{
//==============================================================================
/*
    Performance effects, one at a time, played from an XY pad. Filter, stutter, tape stop,
    bitcrush and lo-fi are inserts that fade in and out as the pad is touched and let go. Delay
    and wash are sends: letting go stops feeding them and their tails ring out.

        filter     X: low-pass at the left, off in the middle, high-pass at the right; Y: resonance
        delay      X: a 1/32 to a 1/4 note, in time with the song; Y: feedback (ping-pong)
        stutter    X: repeats a slice from a 1/4 note down to a 1/32; Y: each repeat fades
        tapeStop   X: how long the tape takes to stop (0.15 to 2 s)
        bitcrush   X: lowers the sample rate; Y: the bit depth
        wash       X: size; Y: level (a reverb)
        lofi       X: a slow wow to a fast warble; Y: from chorus to vibrato

    The numbering is the one Somewhere Link sends, so keep it.
*/
namespace perf
{
/** The stutter's slice and the delay's time in beats, picked by X. */
inline double sliceBeats (float x)  { static const double v[] { 1.0, 0.5, 0.25, 0.125 }; return v[std::clamp ((int) (x * 4.0f), 0, 3)]; }
inline double delayBeats (float x)  { static const double v[] { 0.125, 0.25, 0.5, 0.75, 1.0 }; return v[std::clamp ((int) (x * 5.0f), 0, 4)]; }

/** Comb and allpass filters for the wash. */
struct Comb
{
    std::vector<float> buf;
    int pos = 0;
    float store = 0;

    void setSize (int n)    { buf.assign ((size_t) std::max (1, n), 0.0f); pos = 0; store = 0; }

    float process (float in, float feedback, float damp) noexcept
    {
        auto out = buf[(size_t) pos];
        store = out * (1.0f - damp) + store * damp;
        buf[(size_t) pos] = in + store * feedback;
        if (++pos >= (int) buf.size()) pos = 0;
        return out;
    }
};

struct Allpass
{
    std::vector<float> buf;
    int pos = 0;

    void setSize (int n)    { buf.assign ((size_t) std::max (1, n), 0.0f); pos = 0; }

    float process (float in) noexcept
    {
        auto b = buf[(size_t) pos];
        buf[(size_t) pos] = in + b * 0.5f;
        if (++pos >= (int) buf.size()) pos = 0;
        return b - in;
    }
};

class PerformanceFx
{
public:
    enum Effect { filter, delay, stutter, tapeStop, bitcrush, wash, lofi, numEffects };

    std::atomic<int> type { filter };
    std::atomic<float> x { 0.5f }, y { 0.5f };
    std::atomic<bool> active { false };

    // For drawing: the loudest input since the last look (so hits show), and how fast the tape
    // runs (1, or slowing to 0 during a tape stop)
    std::atomic<float> peak { 0 }, tapeSpeedOut { 1 };

    void prepare (double sampleRate)
    {
        if (sampleRate == rate && ! ringL.empty())
            return;

        rate = sampleRate;
        ringL.assign ((size_t) (rate * 4), 0.0f);
        ringR.assign ((size_t) (rate * 4), 0.0f);
        writePos = 0;
        tailsQuiet = true;
        wasActive = false;
        quietNeeded = (int) (rate * 2.6);
        quietFor = quietNeeded;

        for (auto& d : echo)
            d.setSize ((int) (rate * 2.5));

        static const int combTuning[] { 1116, 1188, 1277, 1356 }, allpassTuning[] { 556, 441 };
        const double scale = rate / 44100.0;

        for (int ch = 0; ch < 2; ++ch)
        {
            for (int i = 0; i < 4; ++i) combs[ch][i].setSize ((int) ((combTuning[i] + ch * 23) * scale));
            for (int i = 0; i < 2; ++i) allpasses[ch][i].setSize ((int) ((allpassTuning[i] + ch * 23) * scale));
        }

        for (auto* s : { &sx, &sy })
        {
            s->setTimeMs (30.0f, rate);
            s->reset (0.5f);
        }
        wet.setTimeMs (3.0f, rate);     // cut in hard, like hardware, without clicking
        send.setTimeMs (8.0f, rate);
        delayTime.setTimeMs (25.0f, rate);   // a new time lands at once (a long glide felt like lag)
        delayTime.reset ((float) (rate * 0.25));
    }

    void process (float* L, float* R, int n, double bpm)
    {
        const int t = type.load();
        const bool on = active.load();
        const double beat = rate * 60.0 / std::clamp (bpm, 20.0, 400.0);

        sx.target = x.load();
        sy.target = y.load();

        // Effects that start from "now" when you touch the pad. Jump to where the finger
        // landed; smoothing is only for dragging.
        if (on && ! wasActive)
        {
            sx.reset (sx.target);
            sy.reset (sy.target);
            if (t == stutter)  stutterStart (beat);
            if (t == tapeStop) { tapePos = writePos; tapeSpeed = 1.0; }
            if (t == lofi)     { lofiTone[0] = lofiTone[1] = lofiTone[2] = lofiTone[3] = 0; }
            sweep.reset();
        }
        wasActive = on;

        const bool insert = t == filter || t == stutter || t == tapeStop
                         || t == bitcrush || t == lofi;
        wet.target = on && insert ? 1.0f : 0.0f;
        send.target = on && ! insert ? 1.0f : 0.0f;

        const int ringSize = (int) ringL.size();
        float loudest = 0;

        // Off, faded out and with the tails rung out, the sound passes through untouched; the
        // last few seconds are still kept, for a stutter or tape stop to start from
        if (! on && tailsQuiet && wet.value < 1.0e-4f && send.value < 1.0e-4f)
        {
            for (int i = 0; i < n; ++i)
            {
                loudest = std::max (loudest, std::max (std::abs (L[i]), std::abs (R[i])));
                ringL[(size_t) writePos] = L[i];
                ringR[(size_t) writePos] = R[i];
                if (++writePos >= ringSize)
                    writePos = 0;
            }

            if (loudest > peak.load (std::memory_order_relaxed))
                peak.store (loudest, std::memory_order_relaxed);
            tapeSpeedOut.store (1.0f, std::memory_order_relaxed);
            return;
        }


        for (int i = 0; i < n; ++i)
        {
            const float inL = L[i], inR = R[i];
            loudest = std::max (loudest, std::max (std::abs (inL), std::abs (inR)));
            ringL[(size_t) writePos] = inL;
            ringR[(size_t) writePos] = inR;

            const float px = sx.next(), py = sy.next(), w = wet.next(), s = send.next();
            float outL = inL, outR = inR;

            if (w > 0.0001f)
            {
                float fxL = inL, fxR = inR;

                switch (t)
                {
                    case filter:
                        if ((filterCounter++ & 15) == 0)
                            setFilter (px, py);
                        fxL = sweep.process (inL, 0);
                        fxR = sweep.process (inR, 1);
                        break;

                    case stutter:
                    {
                        // X picks the slice (a 1/4 note down to a 1/32); Y fades each repeat
                        auto len = std::max (64, (int) (sliceBeats (px) * beat));
                        if (len != sliceLength) { sliceLength = len; slicePos %= sliceLength; }
                        auto idx = (sliceStart + slicePos) % ringSize;
                        auto edge = std::min (1.0f, std::min (slicePos, sliceLength - slicePos) / 48.0f);   // declick the loop point
                        fxL = ringL[(size_t) idx] * sliceGain * edge;
                        fxR = ringR[(size_t) idx] * sliceGain * edge;
                        if (++slicePos >= sliceLength)
                        {
                            slicePos = 0;
                            sliceGain *= 1.0f - 0.35f * py;
                        }
                        break;
                    }

                    case tapeStop:
                    {
                        // X sets how long the stop takes (0.15-2 s); the tape drags behind "now"
                        auto seconds = 0.15 + 1.85 * px;
                        tapeSpeed = std::max (0.0, tapeSpeed - 1.0 / (seconds * rate));
                        auto pos = tapePos;
                        auto i0 = (int) std::floor (pos);
                        auto frac = (float) (pos - i0);
                        auto a = ((i0 % ringSize) + ringSize) % ringSize, b = (a + 1) % ringSize;
                        auto g = (float) std::min (1.0, tapeSpeed * 3.0);
                        fxL = (ringL[(size_t) a] + frac * (ringL[(size_t) b] - ringL[(size_t) a])) * g;
                        fxR = (ringR[(size_t) a] + frac * (ringR[(size_t) b] - ringR[(size_t) a])) * g;
                        tapePos += tapeSpeed;
                        break;
                    }

                    case bitcrush:
                    {
                        // X lowers the sample rate (down to 1/40), Y the bit depth (16 down to 3)
                        holdPhase += 1.0 / (1.0 + 39.0 * px);
                        if (holdPhase >= 1.0)
                        {
                            holdPhase -= 1.0;
                            auto q = std::pow (2.0f, 15.0f - 13.0f * py);
                            holdL = std::round (inL * q) / q;
                            holdR = std::round (inR * q) / q;
                        }
                        fxL = holdL;
                        fxR = holdR;
                        break;
                    }

                    case lofi:
                    {
                        // A worn vibrato/chorus: the last few ms replayed through a wobbling delay, the
                        // wobble a slow wow at the left of the pad and a fast warble at the right. Low on
                        // the pad it's blended with the dry sound (chorus); at the top it's all wet, so
                        // the pitch itself bends (vibrato), and deeper. The wet side is darkened, a little
                        // saturated and given some random flutter, like a tired tape or bucket brigade.
                        const double hz = 0.35 * std::pow (24.0, (double) px);          // 0.35 to 8.4 Hz
                        lofiPhase += hz / rate;
                        if (lofiPhase >= 1.0) lofiPhase -= 1.0;
                        flutterPhase += 13.0 / rate;
                        if (flutterPhase >= 1.0) flutterPhase -= 1.0;

                        const double depthMs = 0.8 + 4.2 * py;
                        const double flutter = 0.12 * std::sin (dspmath::MathConstants<double>::twoPi * flutterPhase);
                        const double twoPi = dspmath::MathConstants<double>::twoPi;
                        const double spread = 0.17 * (1.0 - py);                          // stereo width as a chorus
                        const double dL = (6.0 + depthMs * (0.5 + 0.5 * std::sin (twoPi * lofiPhase)) + flutter) * 0.001 * rate;
                        const double dR = (6.0 + depthMs * (0.5 + 0.5 * std::sin (twoPi * (lofiPhase + spread))) + flutter) * 0.001 * rate;

                        auto tap = [&] (const std::vector<float>& ring, double delaySamples)
                        {
                            const double pos = writePos - delaySamples;
                            const int i0 = (int) std::floor (pos);
                            const float frac = (float) (pos - i0);
                            const int a = ((i0 % ringSize) + ringSize) % ringSize, b = (a + 1) % ringSize;
                            return ring[(size_t) a] + frac * (ring[(size_t) b] - ring[(size_t) a]);
                        };
                        float wetL = tap (ringL, dL), wetR = tap (ringR, dR);

                        // Two gentle one-pole low-passes (about 4.5 kHz) and a soft saturation
                        const float k = (float) (1.0 - std::exp (-twoPi * 4500.0 / rate));
                        lofiTone[0] += k * (wetL - lofiTone[0]);  lofiTone[1] += k * (lofiTone[0] - lofiTone[1]);
                        lofiTone[2] += k * (wetR - lofiTone[2]);  lofiTone[3] += k * (lofiTone[2] - lofiTone[3]);
                        wetL = std::tanh (1.4f * lofiTone[1]) / 1.4f;
                        wetR = std::tanh (1.4f * lofiTone[3]) / 1.4f;

                        const float mix = 0.5f + 0.5f * py;                               // chorus 50/50 → vibrato all wet
                        fxL = inL + mix * (wetL - inL);
                        fxR = inR + mix * (wetR - inR);
                        break;
                    }

                    default: break;
                }

                outL = inL + w * (fxL - inL);
                outR = inR + w * (fxR - inR);
            }

            // Sends keep running so their tails ring out after you let go
            float echoOut = 0;
            {
                auto target = (float) (delayBeats (t == delay ? px : lastDelayX) * beat);
                if (t == delay) lastDelayX = px;
                delayTime.target = std::clamp (target, 16.0f, (float) (rate * 2.4));
                auto d = delayTime.next();
                auto feedback = 0.85f * (t == delay ? py : lastDelayY);
                if (t == delay) lastDelayY = py;

                auto dl = echo[0].read (d), dr = echo[1].read (d);
                auto sendIn = t == delay ? s : 0.0f;
                echo[0].push (inL * sendIn + dr * feedback);   // ping-pong
                echo[1].push (inR * sendIn + dl * feedback);
                echoOut = std::max (std::abs (dl), std::abs (dr));
                outL += 0.7f * dl;
                outR += 0.7f * dr;
            }
            {
                auto washIn = (t == wash ? s : 0.0f) * 0.03f * (inL + inR);
                if (t == wash) { washSize = px; washMix = py; }
                auto feedback = 0.72f + 0.27f * washSize;
                float wl = 0, wr = 0;
                for (auto& c : combs[0]) wl += c.process (washIn, feedback, 0.3f);
                for (auto& c : combs[1]) wr += c.process (washIn, feedback, 0.3f);
                for (auto& a : allpasses[0]) wl = a.process (wl);
                for (auto& a : allpasses[1]) wr = a.process (wr);
                // Rung out once the sends have been silent for longer than the longest echo takes to come back
                if (s > 1.0e-4f || echoOut > 1.0e-6f || std::abs (wl) > 1.0e-6f || std::abs (wr) > 1.0e-6f)
                    quietFor = 0;
                else if (quietFor < quietNeeded)
                    ++quietFor;

                outL += (0.3f + 1.2f * washMix) * wl;
                outR += (0.3f + 1.2f * washMix) * wr;
            }

            // Resonant filters and feedback can get loud: round off anything over full scale
            L[i] = softClip (outL);
            R[i] = softClip (outR);

            if (++writePos >= ringSize)
                writePos = 0;
        }

        tailsQuiet = quietFor >= quietNeeded;

        if (loudest > peak.load (std::memory_order_relaxed))
            peak.store (loudest, std::memory_order_relaxed);
        tapeSpeedOut.store (on && t == tapeStop ? (float) tapeSpeed : 1.0f, std::memory_order_relaxed);
    }

private:
    double rate = 0;
    std::vector<float> ringL, ringR;    // the last 4 s of output, for stutter and tape stop
    int writePos = 0;
    bool wasActive = false, tailsQuiet = true;
    int quietFor = 0, quietNeeded = 1;

    Smoothed sx, sy, wet, send, delayTime;

    Biquad sweep;
    int filterCounter = 0;

    int sliceStart = 0, sliceLength = 1, slicePos = 0;
    float sliceGain = 1;

    double tapePos = 0, tapeSpeed = 1;
    double holdPhase = 1;
    float holdL = 0, holdR = 0;

    double lofiPhase = 0, flutterPhase = 0;
    float lofiTone[4] {};

    DelayLine echo[2];
    float lastDelayX = 0.5f, lastDelayY = 0.5f;

    Comb combs[2][4];
    Allpass allpasses[2][2];
    float washSize = 0.5f, washMix = 0.5f;

    void stutterStart (double beat)
    {
        // Repeat the slice that just played
        sliceLength = std::max (64, (int) (sliceBeats (sx.target) * beat));
        sliceStart = ((writePos - sliceLength) % (int) ringL.size() + (int) ringL.size()) % (int) ringL.size();
        slicePos = 0;
        sliceGain = 1;
    }

    /** Left half closes a low-pass (20 kHz to 80 Hz), right half opens a high-pass (20 Hz to 8 kHz). Y is resonance. */
    void setFilter (float px, float py)
    {
        const double q = 0.707 + 7.0 * py;
        if (px < 0.5f)
            sweep.set (Biquad::Type::lowPass, 20000.0 * std::pow (80.0 / 20000.0, (0.5 - px) * 2.0), q, 0, rate);
        else
            sweep.set (Biquad::Type::highPass, 20.0 * std::pow (8000.0 / 20.0, (px - 0.5) * 2.0), q, 0, rate);
    }

    static float softClip (float v) noexcept
    {
        return std::abs (v) <= 0.9f ? v : std::copysign (0.9f + 0.1f * std::tanh ((std::abs (v) - 0.9f) * 10.0f), v);
    }
};
} // namespace perf

using perf::PerformanceFx;
} // namespace elsewhere
