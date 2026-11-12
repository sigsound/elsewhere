// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Drum pattern generator for Drum tracks (plain C++, no JUCE/Tracktion types).
//
// A section's settings (style, complexity, loudness, per-part variation, fills, swing) are
// turned into a list of hits: which drum piece, when (in beats from the section start),
// how hard and how long. The same settings and seed always give the same pattern, so a
// section sounds the same on every collaborator's machine.

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

namespace elsewhere::drums
{
//==============================================================================
enum class Piece
{
    kick, snare, rim, clap, closedHat, pedalHat, openHat, crash, ride,
    tomHigh, tomMid, tomLow, shaker, tambourine, cowbell,
    count
};

inline const char* pieceName (Piece p)
{
    static const char* names[] = { "Kick", "Snare", "Rim", "Clap", "Closed Hat", "Pedal Hat", "Open Hat", "Crash", "Ride",
                                   "High Tom", "Mid Tom", "Low Tom", "Shaker", "Tambourine", "Cowbell" };
    return names[(int) p];
}

/** General MIDI drum map: what drum plug-ins (Sitala, MT Power Drum Kit ...) expect. */
inline int gmNote (Piece p)
{
    static const int notes[] = { 36, 38, 37, 39, 42, 44, 46, 49, 51, 48, 45, 41, 82, 54, 56 };
    return notes[(int) p];
}

/** Which of the three part groups a piece belongs to (the per-part toggles and variation). */
enum class Part { kickSnare, hatsCymbals, tomsPerc };

inline Part partOf (Piece p)
{
    switch (p)
    {
        case Piece::kick: case Piece::snare: case Piece::rim: case Piece::clap:                 return Part::kickSnare;
        case Piece::closedHat: case Piece::pedalHat: case Piece::openHat:
        case Piece::crash: case Piece::ride:                                                     return Part::hatsCymbals;
        case Piece::tomHigh: case Piece::tomMid: case Piece::tomLow:
        case Piece::shaker: case Piece::tambourine: case Piece::cowbell: case Piece::count:     return Part::tomsPerc;
    }
    return Part::tomsPerc;
}

//==============================================================================
enum class Style { rock, pop, funk, hipHop, trap, house, drumAndBass, halfTime, count };

inline const char* styleName (Style s)
{
    static const char* names[] = { "Rock", "Pop", "Funk", "Hip-Hop", "Trap", "House", "Drum & Bass", "Half-time" };
    return names[(int) s];
}

/**
    A style is one bar of 16th-note steps per piece. Each step character:
      '.'  never
      'A'  always, accented          'X'  always
      '1'-'9'  appears once complexity reaches that many tenths (e.g. '5' at 50 %)
      'g'  ghost note (quiet), appears from moderate complexity when variation allows
*/
struct StyleDef
{
    struct Line { Piece piece; const char* steps; };
    std::vector<Line> lines;
    bool hatRolls = false;          // trap-style 32nd/triplet hat rolls at higher complexity
    bool loudUsesRide = false;      // swap closed hats for ride when it gets loud
    double defaultSwing = 0.0;
};

inline const StyleDef& styleDef (Style s)
{
    static const std::array<StyleDef, (size_t) Style::count> defs = {{
        // Rock: 8th hats, kick on 1 and 3, snare backbeat
        { { { Piece::kick,      "A.....6.X.3...7." },
            { Piece::snare,     "....A..g....A.g." },
            { Piece::closedHat, "X8X8X8X8X8X8X8X8" },
            { Piece::openHat,   "..............5." },
            { Piece::tambourine,"....6.......6..." } }, false, true, 0.0 },
        // Pop
        { { { Piece::kick,      "A.....X.A.4....." },
            { Piece::snare,     "....A.......A..." },
            { Piece::clap,      "....7.......7..." },
            { Piece::closedHat, "X6X6X6X6X6X6X6X6" },
            { Piece::shaker,    "5454545454545454" } }, false, true, 0.0 },
        // Funk: busy 16th hats, syncopated kick, ghosted snare
        { { { Piece::kick,      "A..4..X...3.X.5." },
            { Piece::snare,     "..g.A..g.g..A..g" },
            { Piece::closedHat, "XXXXXXXXXXXXXXXX" },
            { Piece::openHat,   ".......4........" },
            { Piece::cowbell,   "8...8...8...8..." } }, false, false, 0.1 },
        // Hip-Hop (boom bap), swung
        { { { Piece::kick,      "A......4.XX...5." },
            { Piece::snare,     "....A.......A..." },
            { Piece::closedHat, "X6X6X6X6X6X6X6X6" },
            { Piece::openHat,   "...........5...." },
            { Piece::rim,       "......7.......7." } }, false, false, 0.55 },
        // Trap (half-time backbeat, rolling hats)
        { { { Piece::kick,      "A.....X...X.3..5" },
            { Piece::snare,     "........A......." },
            { Piece::clap,      "........X......." },
            { Piece::closedHat, "X.X.X.X.X.X.X.X." },
            { Piece::openHat,   "..............6." } }, true, false, 0.0 },
        // House: four on the floor, off-beat open hats
        { { { Piece::kick,      "A...A...A...A..." },
            { Piece::clap,      "....A.......A..." },
            { Piece::closedHat, "6.6.6.6.6.6.6.6." },
            { Piece::openHat,   "..X...X...X...X." },
            { Piece::shaker,    "3333333333333333" } }, false, false, 0.1 },
        // Drum & Bass: two-step
        { { { Piece::kick,      "A.........X....." },
            { Piece::snare,     "....A..g..g.A..g" },
            { Piece::closedHat, "X6X6X6X6X6X6X6X6" },
            { Piece::ride,      "7...7...7...7..." } }, false, false, 0.0 },
        // Half-time: snare on 3
        { { { Piece::kick,      "A.....5...X....." },
            { Piece::snare,     "........A......g" },
            { Piece::closedHat, "X.X.X.X.X.X.X.X." },
            { Piece::openHat,   "..............5." },
            { Piece::tomLow,    "...........8...." } }, false, true, 0.0 },
    }};
    return defs[(size_t) s];
}

//==============================================================================
struct SectionSettings
{
    Style style = Style::rock;
    double complexity = 0.4;        // 0 simple .. 1 complex
    double loudness = 0.6;          // 0 soft .. 1 loud
    std::array<bool, 3> partOn { true, true, true };
    std::array<double, 3> variation { 0.3, 0.3, 0.3 };
    double fills = 0.35;            // 0 none .. 1 frequent and long
    bool fillAtEnd = true;          // a fill leading out of the section
    bool crashAtStart = true;
    double swing = -1.0;            // 0..1, or < 0 for the style's default
    uint32_t seed = 1;
};

struct Hit
{
    double beat;        // from the section start
    Piece piece;
    float velocity;     // 0..1
    double length;      // beats
};

/** Small deterministic random generator (same results on every machine). */
struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed) : s (seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint32_t next()     { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t) (s >> 16); }
    double unit()       { return (next() & 0xffffff) / (double) 0x1000000; }
};

//==============================================================================
/**
    Generates the hits for a section.
    @param startBeat     the section's start in song beats (patterns line up with the song's bars)
    @param lengthBeats   the section's length
    @param beatsPerBar   from the time signature (patterns assume quarter-note beats)
*/
inline std::vector<Hit> generate (const SectionSettings& in, double startBeat, double lengthBeats, int beatsPerBar)
{
    std::vector<Hit> hits;
    if (lengthBeats <= 0.0 || beatsPerBar <= 0)
        return hits;

    const auto& def = styleDef (in.style);
    const double complexity = std::clamp (in.complexity, 0.0, 1.0);
    const double loudness = std::clamp (in.loudness, 0.0, 1.0);
    const double swing = std::clamp (in.swing < 0.0 ? def.defaultSwing : in.swing, 0.0, 1.0);
    const int stepsPerBar = beatsPerBar * 4;
    const double stepBeats = 0.25;
    const double velScale = 0.45 + 0.55 * loudness;
    const double endBeat = startBeat + lengthBeats;

    // Fill every N bars (a musical phrase), or never
    const int phrase = in.fills <= 0.02 ? 0 : in.fills < 0.34 ? 8 : in.fills < 0.67 ? 4 : 2;
    const double fillBeats = (in.fills > 0.5 && complexity > 0.45) ? 2.0 : 1.0;

    const int firstBar = (int) std::floor (startBeat / beatsPerBar + 1.0e-9);
    const int lastBar = (int) std::ceil (endBeat / beatsPerBar - 1.0e-9);
    const int sectionLastBar = lastBar - 1;

    auto add = [&] (double songBeat, Piece p, double vel, double len, Rng& rng)
    {
        if (songBeat < startBeat - 1.0e-9 || songBeat >= endBeat - 1.0e-9)
            return;

        // Humanise: a little timing and velocity movement
        const double jitter = (rng.unit() - 0.5) * 0.012;
        const double v = std::clamp (vel * velScale * (0.93 + 0.14 * rng.unit()), 0.05, 1.0);
        hits.push_back ({ std::max (0.0, songBeat - startBeat + jitter), p, (float) v, len });
    };

    for (int bar = firstBar; bar < lastBar; ++bar)
    {
        Rng rng ((uint64_t) in.seed * 1000003ull + (uint64_t) (bar + 100000));
        const double barBeat = (double) bar * beatsPerBar;
        const int barInSection = bar - firstBar;
        const int barInPhrase = phrase > 0 ? barInSection % phrase : -1;
        const bool phraseVariation = (barInSection % 4) == 3;     // the 4th bar of a phrase moves a bit more

        // Fill at the end of the phrase, and/or at the end of the section
        double fillStart = 1.0e9;
        const bool fillHere = (phrase > 0 && barInPhrase == phrase - 1) || (in.fillAtEnd && bar == sectionLastBar);
        if (fillHere && in.partOn[0])
            fillStart = barBeat + beatsPerBar - fillBeats;

        for (const auto& line : def.lines)
        {
            Piece piece = line.piece;
            const auto part = (int) partOf (piece);
            if (! in.partOn[(size_t) part])
                continue;

            const double var = in.variation[(size_t) part] * (phraseVariation ? 1.5 : 1.0);

            // When it's loud, some styles move from the hats to the ride
            if (def.loudUsesRide && piece == Piece::closedHat && loudness > 0.72)
                piece = Piece::ride;

            for (int step = 0; step < stepsPerBar; ++step)
            {
                const char c = line.steps[step % 16];
                double vel = 0.0;

                if (c == 'A')        vel = 1.0;
                else if (c == 'X')   vel = 0.85;
                else if (c >= '1' && c <= '9')
                {
                    if (complexity * 10.0 + 1.0e-9 >= (double) (c - '0')) vel = 0.72;
                }
                else if (c == 'g')
                {
                    if (complexity >= 0.4 && rng.unit() < 0.4 + var) vel = 0.28;
                }

                // Variation: occasionally drop a non-accented hit, or add an extra one
                if (vel > 0.0 && c != 'A' && rng.unit() < var * 0.12)
                    vel = 0.0;
                else if (vel <= 0.0 && c == '.' && (piece == Piece::kick) && step % 2 == 1 && rng.unit() < var * complexity * 0.18)
                    vel = 0.6;

                // Hats/shakers: accent the beats, softer in between
                if ((piece == Piece::closedHat || piece == Piece::shaker || piece == Piece::ride) && vel > 0.0)
                    vel *= (step % 4 == 0) ? 1.0 : (step % 2 == 0 ? 0.85 : 0.62);

                if (vel <= 0.0)
                    continue;

                double beat = barBeat + step * stepBeats;
                if (step % 2 == 1)
                    beat += swing * stepBeats / 3.0;

                // The fill replaces the groove (except the kick underneath)
                if (beat >= fillStart && piece != Piece::kick)
                    continue;

                // Open hats sometimes swap in on off-beats with more variation
                Piece actual = piece;
                if (piece == Piece::closedHat && step % 4 == 2 && rng.unit() < var * 0.15)
                    actual = Piece::openHat;

                add (beat, actual, vel, actual == Piece::openHat ? 0.5 : 0.2, rng);
            }

            // Trap hat rolls: 32nd or triplet bursts on a random beat
            if (def.hatRolls && piece == Piece::closedHat && complexity > 0.45)
            {
                const int rolls = complexity > 0.8 ? 2 : 1;
                for (int r = 0; r < rolls; ++r)
                {
                    const int beatIdx = (int) (rng.unit() * beatsPerBar);
                    const bool triplet = rng.unit() < 0.5;
                    const int n = triplet ? 6 : 4;
                    const double span = triplet ? 1.0 : 0.5;
                    const double at = barBeat + beatIdx + (triplet ? 0.0 : 0.5);
                    for (int k = 0; k < n; ++k)
                        if (at + k * span / n < fillStart)
                            add (at + k * span / n, Piece::closedHat, 0.55 + 0.1 * (k % 2), 0.1, rng);
                }
            }
        }

        // Crash on the section's first downbeat
        if (bar == firstBar && in.crashAtStart && in.partOn[1] && std::abs (barBeat - startBeat) < 1.0e-6)
            add (barBeat, Piece::crash, 0.9, 1.0, rng);

        // The fill itself: snare into toms, getting louder
        if (fillStart < 1.0e8)
        {
            static const Piece order[] = { Piece::snare, Piece::snare, Piece::tomHigh, Piece::tomHigh,
                                           Piece::tomMid, Piece::tomMid, Piece::tomLow, Piece::tomLow };
            const bool thirtySeconds = complexity > 0.75;
            const double res = thirtySeconds ? 0.125 : 0.25;
            const int n = (int) std::round (fillBeats / res);
            const bool toms = in.partOn[2];

            for (int k = 0; k < n; ++k)
            {
                // Leave a few gaps so it breathes, fewer when complex
                if (k % 2 == 1 && rng.unit() > 0.45 + 0.5 * complexity)
                    continue;

                auto idx = (size_t) std::min (7, k * 8 / std::max (1, n));
                Piece p = toms ? order[idx] : Piece::snare;
                const double vel = 0.6 + 0.4 * (double) k / (double) std::max (1, n - 1);
                add (fillStart + k * res, p, vel, 0.2, rng);
            }
        }
    }

    std::sort (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.beat < b.beat; });
    return hits;
}

} // namespace elsewhere::drums
