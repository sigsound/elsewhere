// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cmath>

namespace elsewhere
{
//==============================================================================
/**
    Brings audio rendered at 2x or 4x the sample rate back down, in plain C++.

    Each halving is a polyphase pair of allpass chains (a half-band IIR): flat to within a
    hair up to 0.45 of the lower rate, and over 90 dB down above the Nyquist it folds back
    from. The coefficients are designed once, in prepare(); process() neither allocates nor
    locks. Run one per channel.
*/
class Downsampler
{
public:
    static constexpr int maxCoefs = 16;

    /** stages: 1 halves (2x to 1x), 2 halves twice (4x to 1x). */
    void prepare (int numStages)
    {
        stages = numStages < 1 ? 1 : (numStages > 2 ? 2 : numStages);
        numCoefs = design (coefs, 90.0, 0.05);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stage)
            s = Stage();
    }

    int factor() const noexcept     { return 1 << stages; }

    /** n * factor() samples in, n out, in place: data[0..n) holds the result. */
    void process (float* data, int n) noexcept
    {
        int len = n * factor();
        for (int s = 0; s < stages; ++s)
        {
            len /= 2;
            stage[(size_t) s].process (data, len, coefs.data(), numCoefs);
        }
    }

    /** The delay through it at low frequencies, in samples at the lower rate. */
    double latencySamples() const noexcept
    {
        // An allpass (c + z^-1) / (1 + c z^-1) delays DC by (1 - c) / (1 + c) of its own samples.
        // The even branch takes the later input, so a halving delays by (even + odd - 0.5) / 2;
        // the first of two halvings runs at twice the rate, so counts half.
        double even = 0.0, odd = 0.0;
        for (int i = 0; i < numCoefs; ++i)
            (i % 2 == 0 ? even : odd) += (1.0 - coefs[(size_t) i]) / (1.0 + coefs[(size_t) i]);
        const double one = 0.5 * (even + odd - 0.5);
        return stages == 2 ? one * 1.5 : one;
    }

private:
    struct Stage
    {
        std::array<double, maxCoefs> x {}, y {};

        void process (float* data, int outLen, const double* c, int count) noexcept
        {
            for (int i = 0; i < outLen; ++i)
            {
                double a = data[2 * i + 1], b = data[2 * i];
                for (int k = 0; k < count; k += 2)
                {
                    const double out = c[k] * (a - y[(size_t) k]) + x[(size_t) k];
                    x[(size_t) k] = a; y[(size_t) k] = out; a = out;
                }
                for (int k = 1; k < count; k += 2)
                {
                    const double out = c[k] * (b - y[(size_t) k]) + x[(size_t) k];
                    x[(size_t) k] = b; y[(size_t) k] = out; b = out;
                }
                data[i] = (float) (0.5 * (a + b));
            }
            for (auto& v : y) if (std::abs (v) < 1.0e-30) v = 0.0;
        }
    };

    //==============================================================================
    // The half-band design: elliptic-style coefficients for a given stopband and transition
    static int design (std::array<double, maxCoefs>& out, double attenuationDb, double transition)
    {
        const double pi = 3.14159265358979323846;
        double k = std::tan ((1.0 - transition * 2.0) * pi / 4.0);
        k *= k;
        const double kk = std::pow (1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - kk) / (1.0 + kk);
        const double e4 = e * e * e * e;
        const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

        const double attn = std::pow (10.0, -attenuationDb / 10.0);
        const double a = attn / (1.0 - attn);
        int order = (int) std::ceil (std::log (a * a / 16.0) / std::log (q));
        if (order % 2 == 0) ++order;
        if (order < 3) order = 3;
        int n = (order - 1) / 2;
        if (n > maxCoefs) n = maxCoefs;
        order = n * 2 + 1;

        for (int index = 0; index < n; ++index)
        {
            const int c = index + 1;
            double num = 0.0, den = 0.0, term;
            int i = 0, sign = 1;
            do {
                term = std::pow (q, (double) (i * (i + 1))) * std::sin ((double) ((i * 2 + 1) * c) * pi / order) * sign;
                num += term; sign = -sign; ++i;
            } while (std::abs (term) > 1.0e-100);
            i = 1; sign = -1;
            do {
                term = std::pow (q, (double) (i * i)) * std::cos ((double) (i * 2 * c) * pi / order) * sign;
                den += term; sign = -sign; ++i;
            } while (std::abs (term) > 1.0e-100);

            const double ww = num * std::pow (q, 0.25) / (den + 0.5);
            const double wwsq = ww * ww;
            const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
            out[(size_t) index] = (1.0 - x) / (1.0 + x);
        }
        return n;
    }

    std::array<double, maxCoefs> coefs {};
    int numCoefs = 0, stages = 1;
    std::array<Stage, 2> stage {};
};

} // namespace elsewhere
