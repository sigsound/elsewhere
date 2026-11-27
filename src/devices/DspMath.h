// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The few maths helpers Elsewhere's sound code uses, in plain C++, so that code builds
// with nothing but the standard library (Dsp.h, SynthCore.h, SpaceDsp.h, HazeEngine.h,
// PerformanceFx.h).

namespace elsewhere::dspmath
{
    template <typename T> constexpr T jlimit (T lower, T upper, T v) noexcept   { return v < lower ? lower : (upper < v ? upper : v); }
    template <typename T> constexpr T jmax (T a, T b) noexcept                   { return a < b ? b : a; }
    template <typename T> constexpr T jmin (T a, T b) noexcept                   { return b < a ? b : a; }

    template <typename T>
    struct MathConstants
    {
        static constexpr T pi = static_cast<T> (3.141592653589793238L);
        static constexpr T twoPi = static_cast<T> (2 * 3.141592653589793238L);
        static constexpr T halfPi = static_cast<T> (3.141592653589793238L / 2);
    };
}
