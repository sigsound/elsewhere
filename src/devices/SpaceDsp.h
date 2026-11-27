// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DspMath.h"
#include <array>
#include <cmath>
#include <vector>

namespace elsewhere::OrganicDsp
{
    //==============================================================================
    /** A lush little space: 8-line feedback delay network with gentle modulation. */
    struct Space
    {
        static constexpr int lines = 8;
        std::array<std::vector<float>, lines> buf;
        std::array<int, lines> pos {}, len {};
        std::array<float, lines> lp {};
        std::vector<float> pre;
        int prePos = 0;
        double rate = 48000.0;
        float phase = 0.0f;

        void prepare (double sr)
        {
            rate = sr;
            for (auto& b : buf) b.assign ((size_t) (sr * 0.25) + 8, 0.0f);
            pre.assign ((size_t) (sr * 0.03) + 4, 0.0f);
            pos.fill (0); lp.fill (0.0f); prePos = 0; phase = 0.0f;
        }

        void process (float* L, float* R, int n, float size, float mix, float width) noexcept
        {
            if (mix <= 0.0005f || buf[0].empty())
                return;

            static const float ms[lines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f };
            const float scale = 0.6f + 1.9f * size;
            const float t60 = 0.8f + 9.0f * size * size;
            const float damp = 0.35f + 0.4f * size;
            for (int k = 0; k < lines; ++k)
                len[(size_t) k] = dspmath::jlimit (8, (int) buf[(size_t) k].size() - 4, (int) (ms[k] * scale * 0.001 * rate));

            const float wet = std::sin (mix * dspmath::MathConstants<float>::halfPi), dry = std::cos (mix * dspmath::MathConstants<float>::halfPi);
            const float phInc = (float) (0.37 / rate);

            for (int i = 0; i < n; ++i)
            {
                // Pre-delay
                const float in = (L[i] + R[i]) * 0.5f;
                const int preLen = (int) pre.size() - 2;
                const float delayed = pre[(size_t) ((prePos + 1) % preLen)];
                pre[(size_t) prePos] = in;
                prePos = (prePos + 1) % preLen;

                phase += phInc; if (phase > 1.0f) phase -= 1.0f;
                const float mod = std::sin (phase * dspmath::MathConstants<float>::twoPi);

                std::array<float, lines> x {};
                float sum = 0.0f;
                for (size_t k = 0; k < (size_t) lines; ++k)
                {
                    const int extra = (k == 1 || k == 6) ? (int) ((k == 1 ? mod : -mod) * 12.0f) : 0;
                    const int sz = (int) buf[k].size();
                    int rp = pos[k] - len[k] - extra; while (rp < 0) rp += sz;
                    float v = buf[k][(size_t) (rp % sz)];
                    lp[k] += (v - lp[k]) * (1.0f - damp * 0.8f);
                    v = lp[k];
                    const float gfb = std::pow (10.0f, -3.0f * (float) len[k] / (float) rate / t60);
                    x[k] = v * gfb;
                    sum += x[k];
                }
                const float h = sum * (2.0f / (float) lines);
                float outL = 0.0f, outR = 0.0f;
                for (size_t k = 0; k < (size_t) lines; ++k)
                {
                    const float fb = x[k] - h;
                    buf[k][(size_t) pos[k]] = fb + delayed * ((k % 2 == 0) ? 0.35f : -0.35f);
                    pos[k] = (pos[k] + 1) % (int) buf[k].size();
                    if (k % 2 == 0) outL += x[k]; else outR += x[k];
                }
                const float mid = (outL + outR) * 0.5f, side = (outL - outR) * 0.5f * width;
                L[i] = L[i] * dry + (mid + side) * wet * 0.55f;
                R[i] = R[i] * dry + (mid - side) * wet * 0.55f;
            }
        }
    };
} // namespace elsewhere::OrganicDsp
