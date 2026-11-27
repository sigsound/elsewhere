// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Plain C++ DSP building blocks shared by Elsewhere's devices: the standard library and
// DspMath.h only, so they can move to another framework if ever needed.

#include <cmath>
#include <complex>
#include <vector>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include "DspMath.h"

namespace elsewhere
{
//==============================================================================
/** RBJ "Audio EQ Cookbook" biquad (transposed direct form II), per channel state. */
struct Biquad
{
    enum class Type { bell, lowShelf, highShelf, highPass, lowPass };

    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] {}, z2[2] {};

    void set (Type type, double freq, double q, double gainDb, double sampleRate)
    {
        freq = dspmath::jlimit (5.0, sampleRate * 0.49, freq);
        const auto w0 = dspmath::MathConstants<double>::twoPi * freq / sampleRate;
        const auto cosw = std::cos (w0), sinw = std::sin (w0);
        const auto alpha = sinw / (2.0 * dspmath::jmax (0.05, q));
        const auto A = std::pow (10.0, gainDb / 40.0);
        const auto sqA = std::sqrt (A);
        double B0 = 1, B1 = 0, B2 = 0, A0 = 1, A1 = 0, A2 = 0;

        switch (type)
        {
            case Type::bell:
                B0 = 1 + alpha * A;  B1 = -2 * cosw;  B2 = 1 - alpha * A;
                A0 = 1 + alpha / A;  A1 = -2 * cosw;  A2 = 1 - alpha / A;
                break;
            case Type::lowShelf:
                B0 = A * ((A + 1) - (A - 1) * cosw + 2 * sqA * alpha);
                B1 = 2 * A * ((A - 1) - (A + 1) * cosw);
                B2 = A * ((A + 1) - (A - 1) * cosw - 2 * sqA * alpha);
                A0 = (A + 1) + (A - 1) * cosw + 2 * sqA * alpha;
                A1 = -2 * ((A - 1) + (A + 1) * cosw);
                A2 = (A + 1) + (A - 1) * cosw - 2 * sqA * alpha;
                break;
            case Type::highShelf:
                B0 = A * ((A + 1) + (A - 1) * cosw + 2 * sqA * alpha);
                B1 = -2 * A * ((A - 1) + (A + 1) * cosw);
                B2 = A * ((A + 1) + (A - 1) * cosw - 2 * sqA * alpha);
                A0 = (A + 1) - (A - 1) * cosw + 2 * sqA * alpha;
                A1 = 2 * ((A - 1) - (A + 1) * cosw);
                A2 = (A + 1) - (A - 1) * cosw - 2 * sqA * alpha;
                break;
            case Type::highPass:
                B0 = (1 + cosw) / 2;  B1 = -(1 + cosw);  B2 = (1 + cosw) / 2;
                A0 = 1 + alpha;       A1 = -2 * cosw;    A2 = 1 - alpha;
                break;
            case Type::lowPass:
                B0 = (1 - cosw) / 2;  B1 = 1 - cosw;     B2 = (1 - cosw) / 2;
                A0 = 1 + alpha;       A1 = -2 * cosw;    A2 = 1 - alpha;
                break;
        }

        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }

    void setCoefficients (double nb0, double nb1, double nb2, double na1, double na2)
    {
        b0 = nb0; b1 = nb1; b2 = nb2; a1 = na1; a2 = na2;
    }

    void reset()    { z1[0] = z1[1] = z2[0] = z2[1] = 0; }

    float process (float x, int ch) noexcept
    {
        auto y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return (float) y;
    }

    /** Magnitude response at a frequency, in dB (for drawing the curve). */
    double magnitudeDb (double freq, double sampleRate) const
    {
        const auto w = dspmath::MathConstants<double>::twoPi * freq / sampleRate;
        std::complex<double> z = std::polar (1.0, -w), z2c = z * z;
        auto h = (b0 + b1 * z + b2 * z2c) / (1.0 + a1 * z + a2 * z2c);
        return 20.0 * std::log10 (dspmath::jmax (1.0e-9, std::abs (h)));
    }
};

//==============================================================================
/** One-pole smoother for parameters (avoids zipper noise). */
struct Smoothed
{
    float value = 0.0f, target = 0.0f, coeff = 0.999f;

    void setTimeMs (float ms, double sampleRate)    { coeff = std::exp (-1.0f / (ms * 0.001f * (float) sampleRate)); }
    void reset (float v)                            { value = target = v; }
    float next() noexcept                           { value = target + coeff * (value - target); return value; }
};

/** Linear-interpolating circular delay line. */
struct DelayLine
{
    std::vector<float> data;
    int pos = 0;

    void setSize (int n)    { data.assign ((size_t) std::max (4, n), 0.0f); pos = 0; }
    void clear()            { std::fill (data.begin(), data.end(), 0.0f); }
    void push (float x)     { data[(size_t) pos] = x; if (++pos >= (int) data.size()) pos = 0; }

    float read (float delay) const
    {
        const int len = (int) data.size();
        delay = dspmath::jlimit (1.0f, (float) (len - 2), delay);
        float rp = (float) pos - delay;
        if (rp < 0) rp += (float) len;
        if (rp >= (float) len) rp = 0.0f;   // (a tiny negative plus len rounds up to len itself)
        int i0 = (int) rp;
        float frac = rp - (float) i0;
        int i1 = i0 + 1 >= len ? 0 : i0 + 1;
        return data[(size_t) i0] + frac * (data[(size_t) i1] - data[(size_t) i0]);
    }
};

//==============================================================================
/**
    Loudness meter following ITU-R BS.1770-4 / EBU R128: K-weighting, 400 ms blocks with
    75% overlap, momentary (400 ms), short-term (3 s) and gated integrated loudness in LUFS.
*/
class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        rate = sampleRate;
        hopSamples = std::max (1, (int) std::lround (sampleRate * 0.1));

        // K-weighting exactly as BS.1770 specifies (these reproduce its 48 kHz coefficients)
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (dspmath::MathConstants<double>::pi * f0 / sampleRate);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& f : preFilter)
                f.setCoefficients ((Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                                   2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (dspmath::MathConstants<double>::pi * f0 / sampleRate);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& f : rlbFilter)
                f.setCoefficients (1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0);
        }

        reset();
    }

    void reset()
    {
        for (auto& f : preFilter) f.reset();
        for (auto& f : rlbFilter) f.reset();
        hops.fill (0.0);
        hopCount = 0;
        hopAccum = 0.0;
        hopFill = 0;
        binCount.fill (0);
        binEnergy.fill (0.0);
        totalBlocks = 0;
    }

    /** Feeds stereo audio (right may equal left for mono). */
    void process (const float* left, const float* right, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            double l = rlbFilter[0].process (preFilter[0].process (left[i], 0), 0);
            double r = rlbFilter[1].process (preFilter[1].process (right[i], 0), 0);
            hopAccum += l * l + r * r;

            if (++hopFill >= hopSamples)
            {
                hops[(size_t) (hopCount % (long long) hops.size())] = hopAccum / hopSamples;
                ++hopCount;
                hopAccum = 0.0;
                hopFill = 0;

                if (hopCount >= 4 && integrating)
                    addBlock (meanOfLastHops (4));
            }
        }
    }

    /** Whether 400 ms blocks are collected for the integrated measurement. */
    bool integrating = true;

    double getMomentary() const         { return hopCount >= 4 ? toLufs (meanOfLastHops (4)) : -120.0; }
    double getShortTerm() const         { return hopCount >= 30 ? toLufs (meanOfLastHops (30)) : getMomentary(); }
    double getSecondsMeasured() const   { return (double) totalBlocks * 0.1; }

    /** Seconds of audio louder than the absolute gate (i.e. not silence). */
    double getSecondsAudible() const
    {
        long long n = 0;
        for (auto c : binCount) n += c;
        return (double) n * 0.1;
    }

    /** Gated integrated loudness. Blocks are kept in a histogram (0.1 LU bins, -70..+10 LUFS),
        so this costs the same however long the measurement runs. */
    double getIntegrated() const
    {
        double sum = 0.0;
        long long count = 0;
        for (size_t b = 0; b < numBins; ++b) { sum += binEnergy[b]; count += binCount[b]; }

        if (count == 0)
            return -120.0;

        // Relative gate: 10 LU below the mean of the blocks above the absolute gate
        const double relGate = toLufs (sum / (double) count) - 10.0;
        sum = 0.0; count = 0;
        for (size_t b = binFor (relGate); b < numBins; ++b) { sum += binEnergy[b]; count += binCount[b]; }

        return count > 0 ? toLufs (sum / (double) count) : -120.0;
    }

    static double toLufs (double meanSquare)    { return meanSquare > 1.0e-12 ? -0.691 + 10.0 * std::log10 (meanSquare) : -120.0; }

private:
    static constexpr size_t numBins = 800;       // -70 .. +10 LUFS in 0.1 LU steps
    double rate = 48000.0;
    int hopSamples = 4800, hopFill = 0;
    long long hopCount = 0, totalBlocks = 0;
    double hopAccum = 0.0;
    std::array<double, 30> hops {};
    std::array<long long, numBins> binCount {};
    std::array<double, numBins> binEnergy {};
    std::array<Biquad, 2> preFilter, rlbFilter;

    static size_t binFor (double lufs)  { return (size_t) dspmath::jlimit (0.0, (double) numBins - 1.0, std::floor ((lufs + 70.0) * 10.0)); }

    void addBlock (double meanSquare)
    {
        ++totalBlocks;
        const auto lufs = toLufs (meanSquare);
        if (lufs <= -70.0)
            return;

        const auto b = binFor (lufs);
        ++binCount[b];
        binEnergy[b] += meanSquare;
    }

    double meanOfLastHops (int n) const
    {
        double s = 0.0;
        for (int i = 1; i <= n; ++i)
            s += hops[(size_t) ((hopCount - i) % (long long) hops.size())];
        return s / n;
    }
};

//==============================================================================
/**
    Stereo lookahead limiter with inter-sample ("true") peak estimation.

    Gain needed per sample (instant attack, smooth release) -> minimum over the lookahead
    window -> moving average over the same window, while the audio is delayed by the
    window. This guarantees the gain has fully reached its target by the time a peak
    arrives, so nothing passes the ceiling.
*/
class TruePeakLimiter
{
public:
    void prepare (double sampleRate, double lookaheadMs = 1.5)
    {
        rate = sampleRate;
        look = std::max (1, (int) std::lround (lookaheadMs * 0.001 * sampleRate));
        // Audio delay = look + tpDelay - 1: then every min-window averaged for an output sample
        // includes that sample's own gain requirement
        for (auto& d : delay) d.assign ((size_t) (look + tpDelay), 0.0f);
        prepareInterpolator();
        minWindow.assign ((size_t) look, 1.0f);
        avgWindow.assign ((size_t) look, 1.0);
        writePos = minPos = avgPos = 0;
        avgSum = (double) look;
        env = 1.0f;
        hist.fill ({});
    }

    int getLatencySamples() const   { return look + tpDelay - 1; }

    void setReleaseMs (float ms)    { release = std::exp (-1.0f / (std::max (1.0f, ms) * 0.001f * (float) rate)); }

    /** Processes in place; ceiling is linear gain (e.g. 0.891 for -1 dBTP). Returns the lowest gain applied. */
    float process (float* left, float* right, int n, float ceiling)
    {
        float lowest = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            const float l = left[i], r = right[i];
            const float peak = std::max (truePeak (0, l), truePeak (1, r));
            const float needed = peak > ceiling ? ceiling / peak : 1.0f;

            // Instant attack, exponential release, never above what's needed
            env = std::min (needed, 1.0f - release * (1.0f - env));

            // Minimum over the lookahead window (small window: a linear scan is cheap and robust)
            minWindow[(size_t) minPos] = env;
            if (++minPos >= look) minPos = 0;
            float g = 1.0f;
            for (auto v : minWindow) g = std::min (g, v);

            // Moving average over the window
            avgSum += g - avgWindow[(size_t) avgPos];
            avgWindow[(size_t) avgPos] = g;
            if (++avgPos >= look) avgPos = 0;
            const float smoothed = (float) std::min (1.0, avgSum / look);

            // Delay the audio by the window
            const int readPos = (writePos + 1) % (look + tpDelay);
            delay[0][(size_t) writePos] = l;
            delay[1][(size_t) writePos] = r;
            left[i] = delay[0][(size_t) readPos] * smoothed;
            right[i] = delay[1][(size_t) readPos] * smoothed;
            writePos = readPos;

            lowest = std::min (lowest, smoothed);
        }

        return lowest;
    }

private:
    double rate = 48000.0;
    int look = 64, writePos = 0, minPos = 0, avgPos = 0;
    float env = 1.0f, release = 0.999f;
    double avgSum = 0.0;
    std::array<std::vector<float>, 2> delay;
    std::vector<float> minWindow;
    std::vector<double> avgWindow;
    // True-peak estimation: 4x windowed-sinc interpolation (16 taps per phase). The estimate
    // refers to the sample tpDelay samples ago, so the audio path is delayed by that too.
    static constexpr int tpTaps = 16, tpDelay = tpTaps / 2;
    std::array<std::array<float, tpTaps * 2>, 2> hist {};     // doubled ring buffer: contiguous reads
    std::array<std::array<float, tpTaps>, 3> phase {};
    int histPos = 0;
    std::array<float, 2> lastInterval {};

    void prepareInterpolator()
    {
        for (int p = 1; p <= 3; ++p)
        {
            double sum = 0.0;
            for (int k = 0; k < tpTaps; ++k)
            {
                const double t = (double) k - (tpDelay - 1) - p / 4.0;          // distance from the interpolated point
                const double sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (dspmath::MathConstants<double>::pi * t) / (dspmath::MathConstants<double>::pi * t);
                const double w = 0.42 + 0.5 * std::cos (dspmath::MathConstants<double>::pi * t / (tpTaps / 2 + 1))
                                      + 0.08 * std::cos (2.0 * dspmath::MathConstants<double>::pi * t / (tpTaps / 2 + 1));   // Blackman
                phase[(size_t) p - 1][(size_t) k] = (float) (sinc * w);
                sum += sinc * w;
            }
            for (auto& c : phase[(size_t) p - 1]) c = (float) (c / sum);
        }
        for (auto& h : hist) h.fill (0.0f);
        histPos = 0;
    }

    float truePeak (int ch, float x)
    {
        auto& h = hist[(size_t) ch];
        h[(size_t) histPos] = h[(size_t) (histPos + tpTaps)] = x;
        const float* s = h.data() + histPos + 1;        // oldest .. newest
        if (ch == 1 && ++histPos >= tpTaps) histPos = 0;

        // Peak of this sample and the curve on both sides of it
        float after = 0.0f;
        for (auto& ph : phase)
        {
            float y = 0.0f;
            for (int k = 0; k < tpTaps; ++k)
                y += s[k] * ph[(size_t) k];
            after = std::max (after, std::abs (y));
        }
        const float peak = std::max ({ std::abs (s[tpDelay - 1]), after, lastInterval[(size_t) ch] });
        lastInterval[(size_t) ch] = after;
        return peak;
    }
};

//==============================================================================
/** Small in-place radix-2 complex FFT (sizes are powers of two). Plain C++, no scaling on
    the forward transform; the inverse divides by N. */
class Fft
{
public:
    using C = std::complex<float>;

    explicit Fft (int order) : n (1 << order)
    {
        twiddles.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
            twiddles[(size_t) i] = std::polar (1.0f, -2.0f * dspmath::MathConstants<float>::pi * (float) i / (float) n);

        bitrev.resize ((size_t) n);
        for (int i = 0, j = 0; i < n; ++i)
        {
            bitrev[(size_t) i] = j;
            int bit = n >> 1;
            while (j & bit) { j ^= bit; bit >>= 1; }
            j |= bit;
        }
    }

    int size() const    { return n; }

    void forward (C* d) const    { transform (d, false); }
    void inverse (C* d) const
    {
        transform (d, true);
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i) d[i] *= s;
    }

private:
    int n;
    std::vector<C> twiddles;
    std::vector<int> bitrev;

    void transform (C* d, bool inv) const
    {
        for (int i = 0; i < n; ++i)
            if (i < bitrev[(size_t) i]) std::swap (d[i], d[bitrev[(size_t) i]]);

        for (int len = 2; len <= n; len <<= 1)
        {
            const int step = n / len, half = len / 2;
            for (int i = 0; i < n; i += len)
                for (int k = 0; k < half; ++k)
                {
                    C w = twiddles[(size_t) (k * step)];
                    if (inv) w = std::conj (w);
                    const C a = d[i + k], b = d[i + k + half] * w;
                    d[i + k] = a + b;
                    d[i + k + half] = a - b;
                }
        }
    }
};

//==============================================================================
/**
    Uniformly partitioned FFT convolution (overlap-save with a frequency-domain delay line),
    for cabinet impulse responses. Latency is one partition (64 samples).

    The impulse response can be swapped from another thread: build an Ir with makeIr() on
    the message thread and hand it over with setIr(); the audio thread picks it up at the
    next partition and crossfades from the old one.
*/
class Convolver
{
public:
    static constexpr int partition = 64, fftSize = partition * 2, bins = partition + 1;

    struct Ir
    {
        std::vector<std::vector<std::complex<float>>> parts;    // spectra of each partition
    };

    static std::unique_ptr<Ir> makeIr (const float* ir, int length)
    {
        Fft fft (7);
        auto result = std::make_unique<Ir>();
        const int numParts = std::max (1, (length + partition - 1) / partition);
        std::vector<std::complex<float>> buf ((size_t) fftSize);

        for (int p = 0; p < numParts; ++p)
        {
            std::fill (buf.begin(), buf.end(), std::complex<float>());
            for (int i = 0; i < partition; ++i)
            {
                const int idx = p * partition + i;
                if (idx < length) buf[(size_t) i] = ir[idx];
            }
            fft.forward (buf.data());
            result->parts.emplace_back (buf.begin(), buf.begin() + bins);
        }

        return result;
    }

    Convolver() : fft (7) {}

    ~Convolver()
    {
        delete pending.exchange (nullptr);
        delete trash.exchange (nullptr);
    }

    int getLatencySamples() const   { return partition; }

    /** Sets the IR immediately (not while the audio thread runs this convolver). */
    void setIrNow (std::unique_ptr<Ir> ir)
    {
        current = std::move (ir);
        reset();
    }

    /** Hands a new IR to the audio thread (any other thread). */
    void setIr (std::unique_ptr<Ir> ir)
    {
        delete pending.exchange (ir.release());
        delete trash.exchange (nullptr);        // anything the audio thread finished with
    }

    void reset()
    {
        const size_t numParts = current != nullptr ? current->parts.size() : 1;
        fdl.assign (std::max<size_t> (numParts, maxParts), std::vector<std::complex<float>> ((size_t) bins));
        fdlPos = 0;
        std::fill (std::begin (inputBlock), std::end (inputBlock), 0.0f);
        std::fill (std::begin (outputBlock), std::end (outputBlock), 0.0f);
        fill = 0;
    }

    /** Processes in place. */
    void process (float* data, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            inputBlock[partition + fill] = data[i];
            data[i] = outputBlock[fill];

            if (++fill == partition)
            {
                fill = 0;
                processPartition();
            }
        }
    }

    bool hasIr() const  { return current != nullptr && ! current->parts.empty(); }

    /** The largest IR (in partitions) this convolver can take without reallocating. */
    static constexpr size_t maxParts = 48000 / partition;       // ~1 s at 48 kHz

private:
    Fft fft;
    std::unique_ptr<Ir> current;
    std::atomic<Ir*> pending { nullptr }, trash { nullptr };
    std::vector<std::vector<std::complex<float>>> fdl;
    size_t fdlPos = 0;
    float inputBlock[fftSize] {};
    float outputBlock[partition] {};
    int fill = 0;
    std::complex<float> work[fftSize], acc[fftSize], accOld[fftSize];

    void processPartition()
    {
        // Spectrum of the last two input blocks (overlap-save)
        for (int i = 0; i < fftSize; ++i) work[i] = inputBlock[i];
        fft.forward (work);
        std::copy (inputBlock + partition, inputBlock + fftSize, inputBlock);

        if (fdl.empty()) reset();
        fdlPos = (fdlPos + fdl.size() - 1) % fdl.size();
        std::copy (work, work + bins, fdl[fdlPos].begin());

        // Pick up a new IR, keeping the old one for a crossfade
        std::unique_ptr<Ir> old;
        if (auto* p = pending.exchange (nullptr))
        {
            if (p->parts.size() <= fdl.size())
            {
                old = std::move (current);
                current.reset (p);
            }
            else
            {
                delete trash.exchange (p);   // too long for our buffers (shouldn't happen)
            }
        }

        if (current == nullptr)
        {
            std::fill (std::begin (outputBlock), std::end (outputBlock), 0.0f);
            return;
        }

        accumulate (*current, acc);

        if (old != nullptr)
        {
            accumulate (*old, accOld);
            for (int i = 0; i < partition; ++i)
            {
                const float t = (float) i / (float) partition;
                outputBlock[i] = acc[partition + i].real() * t + accOld[partition + i].real() * (1.0f - t);
            }
            delete trash.exchange (old.release());
        }
        else
        {
            for (int i = 0; i < partition; ++i)
                outputBlock[i] = acc[partition + i].real();
        }
    }

    void accumulate (const Ir& ir, std::complex<float>* out)
    {
        std::fill (out, out + fftSize, std::complex<float>());
        const size_t numParts = std::min (ir.parts.size(), fdl.size());

        for (size_t p = 0; p < numParts; ++p)
        {
            const auto& x = fdl[(fdlPos + p) % fdl.size()];
            const auto& h = ir.parts[p];
            for (int k = 0; k < bins; ++k)
                out[k] += x[(size_t) k] * h[(size_t) k];
        }

        // Real signal: mirror the spectrum, then back to time
        for (int k = 1; k < partition; ++k)
            out[fftSize - k] = std::conj (out[k]);
        fft.inverse (out);
    }
};

//==============================================================================
/** Turns a magnitude response into a minimum-phase impulse response (real cepstrum). */
inline std::vector<float> minimumPhaseIr (const std::vector<float>& magnitude /* fftSize/2+1 bins, linear */, int fftOrder, int length)
{
    Fft fft (fftOrder);
    const int n = fft.size();
    std::vector<std::complex<float>> x ((size_t) n);

    for (int k = 0; k <= n / 2; ++k)
    {
        const float m = std::log (std::max (1.0e-6f, magnitude[(size_t) k]));
        x[(size_t) k] = m;
        if (k > 0 && k < n / 2) x[(size_t) (n - k)] = m;
    }

    fft.inverse (x.data());     // real cepstrum

    // Fold: keep c[0] and c[n/2], double the causal part, drop the rest
    for (int i = 1; i < n / 2; ++i)   x[(size_t) i] = 2.0f * x[(size_t) i].real();
    for (int i = n / 2 + 1; i < n; ++i) x[(size_t) i] = 0.0f;
    x[0] = x[0].real();
    x[(size_t) n / 2] = x[(size_t) n / 2].real();

    fft.forward (x.data());
    for (auto& v : x) v = std::exp (v);
    fft.inverse (x.data());

    std::vector<float> ir ((size_t) length);
    for (int i = 0; i < length && i < n; ++i)
        ir[(size_t) i] = x[(size_t) i].real();

    // Fade out the last quarter so truncation doesn't click
    const int fade = length / 4;
    for (int i = 0; i < fade; ++i)
        ir[(size_t) (length - fade + i)] *= 0.5f * (1.0f + std::cos (dspmath::MathConstants<float>::pi * (float) i / (float) fade));

    return ir;
}

/** Scales an IR so its average power gain between 200 Hz and 4 kHz is 0 dB, so every cabinet
    (built-in or loaded) plays at a similar level. */
inline void normaliseIr (std::vector<float>& ir, double sampleRate)
{
    int order = 10;
    while ((1 << order) < (int) ir.size() * 2 && order < 18) ++order;
    Fft fft (order);
    std::vector<std::complex<float>> x ((size_t) fft.size());
    for (size_t i = 0; i < ir.size() && i < x.size(); ++i) x[i] = ir[i];
    fft.forward (x.data());

    double sum = 0.0; int count = 0;
    for (int k = 1; k < fft.size() / 2; ++k)
    {
        const double f = k * sampleRate / fft.size();
        if (f >= 200.0 && f <= 4000.0) { sum += std::norm (x[(size_t) k]); ++count; }
    }

    if (count > 0 && sum > 0.0)
    {
        const float g = (float) (1.0 / std::sqrt (sum / count));
        for (auto& v : ir) v *= g;
    }
}

} // namespace elsewhere
