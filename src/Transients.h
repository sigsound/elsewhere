// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"

namespace elsewhere
{
//==============================================================================
/**
    Tab to transient: moves the playhead to the next (or previous) hit in the focused track's
    audio, using the engine's transient detection (which runs in the background the first
    time a file is asked about).
*/
namespace Transients
{
    /** A clip's transients in edit time, or nothing yet if they're still being found. Clips that
        follow the tempo (auto-tempo) aren't supported: their audio doesn't map simply. */
    inline std::optional<juce::Array<te::TimePosition>> getTimes (te::AudioClipBase& clip)
    {
        if (clip.getAutoTempo())
            return juce::Array<te::TimePosition>();

        auto& wtm = clip.getWarpTimeManager();
        auto [ready, sourceTimes] = wtm.getTransientTimes();

        if (! ready)
            return {};

        auto pos = clip.getPosition();
        auto speed = clip.getSpeedRatio() > 0.0 ? clip.getSpeedRatio() : 1.0;
        juce::Array<te::TimePosition> times;

        for (auto t : sourceTimes)
        {
            auto local = clip.getWarpTime() ? wtm.sourceTimeToWarpTime (t) : t;
            auto editTime = pos.getStart() + te::TimeDuration::fromSeconds (local.inSeconds() / speed) - pos.getOffset();

            if (editTime >= pos.getStart() && editTime < pos.getEnd())
                times.add (editTime);
        }

        return times;
    }

    /** Finds the next transient on the track after `from` (or before it, going back). Returns
        nothing while detection is still running on a clip it needs. */
    inline std::optional<std::optional<te::TimePosition>> find (te::ClipTrack& track, te::TimePosition from, bool forwards)
    {
        const auto tolerance = te::TimeDuration::fromSeconds (0.002);
        std::optional<te::TimePosition> best;

        for (auto c : track.getClips())
        {
            auto ac = dynamic_cast<te::AudioClipBase*> (c);

            if (ac == nullptr)
                continue;

            auto pos = ac->getPosition();

            // Only clips that could hold the answer
            if (forwards ? pos.getEnd() <= from : pos.getStart() >= from)
                continue;

            auto times = getTimes (*ac);

            if (! times)
                return std::optional<std::optional<te::TimePosition>>();   // still detecting

            for (auto t : *times)
            {
                if (forwards && t > from + tolerance && (! best || t < *best))
                    best = t;
                else if (! forwards && t < from - tolerance && (! best || t > *best))
                    best = t;
            }
        }

        return best;
    }

    /** Moves the playhead. Returns false if detection is still running (try again shortly). */
    inline bool moveToNext (EditViewState& evs, bool forwards)
    {
        auto track = dynamic_cast<te::ClipTrack*> (evs.focusedTrack.get());

        if (track == nullptr)
            return true;

        auto& transport = evs.edit.getTransport();
        auto result = find (*track, transport.getPosition(), forwards);

        if (! result)
            return false;

        if (*result)
            transport.setPosition (**result);

        return true;
    }
}

} // namespace elsewhere
