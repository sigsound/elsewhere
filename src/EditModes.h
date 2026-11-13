// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/** What Shuffle and Spot do to clips (see EditViewState::EditMode). */
namespace EditModes
{
    inline juce::Array<te::Clip*> getOtherClips (te::ClipTrack& track, const te::Clip* except)
    {
        juce::Array<te::Clip*> list;

        for (auto c : track.getClips())
            if (c != except)
                list.add (c);

        std::sort (list.begin(), list.end(), [] (auto a, auto b) { return a->getPosition().getStart() < b->getPosition().getStart(); });
        return list;
    }

    /** Moves every clip on the track that starts at or after `from` by `delta`. */
    inline void shiftClipsFrom (te::ClipTrack& track, te::TimePosition from, te::TimeDuration delta, const te::Clip* except = nullptr)
    {
        if (delta == te::TimeDuration())
            return;

        auto clips = getOtherClips (track, except);

        // Moving right, go from the end so clips never pass over each other
        if (delta > te::TimeDuration())
            std::reverse (clips.begin(), clips.end());

        for (auto c : clips)
            if (c->getPosition().getStart() >= from - te::TimeDuration::fromSeconds (1.0e-6))
                c->setStart (juce::jmax (te::TimePosition(), c->getPosition().getStart() + delta), false, true);
    }

    /** Closes the gap a clip leaves behind (after it's deleted or moved away). */
    inline void closeGap (te::ClipTrack& track, te::TimeRange removed, const te::Clip* except = nullptr)
    {
        shiftClipsFrom (track, removed.getEnd(), te::TimeDuration() - removed.getLength(), except);
    }

    /** Shuffle drop: the clip slots in at the clip boundary nearest where it was dropped, and the
        clips after it move along to make room. `from` is where it came from (on `fromTrack`). */
    inline void shuffleDrop (te::Clip& clip, te::ClipTrack* fromTrack, te::TimeRange from)
    {
        auto track = dynamic_cast<te::ClipTrack*> (clip.getTrack());

        if (track == nullptr)
            return;

        // The hole it left closes up first
        if (fromTrack != nullptr)
            closeGap (*fromTrack, from, &clip);

        auto others = getOtherClips (*track, &clip);

        if (others.isEmpty())
            return;

        auto dropped = clip.getPosition().getStart();
        auto len = clip.getPosition().getLength();

        // Boundaries: every other clip's start and end
        auto best = others.getFirst()->getPosition().getStart();

        for (auto c : others)
            for (auto b : { c->getPosition().getStart(), c->getPosition().getEnd() })
                if (std::abs ((b - dropped).inSeconds()) < std::abs ((best - dropped).inSeconds()))
                    best = b;

        shiftClipsFrom (*track, best, len, &clip);
        clip.setStart (best, false, true);
    }

    /** Deletes clips; in Shuffle the clips after each one close up. */
    inline void deleteClips (const juce::Array<te::Clip*>& clips, bool shuffle)
    {
        struct Removed { te::ClipTrack::Ptr track; te::TimeRange range; };
        std::vector<Removed> removed;

        for (auto c : clips)
        {
            if (shuffle && c->getClipSlot() == nullptr)
                if (auto ct = dynamic_cast<te::ClipTrack*> (c->getTrack()))
                    removed.push_back ({ ct, c->getPosition().time });

            c->removeFromParent();
        }

        // Latest first, so each gap closes with the earlier ones still in place
        std::sort (removed.begin(), removed.end(), [] (auto& a, auto& b) { return a.range.getStart() > b.range.getStart(); });

        for (auto& r : removed)
            closeGap (*r.track, r.range);
    }

    //==============================================================================
    inline juce::String formatSpotBars (te::Edit& edit, te::TimePosition t)
    {
        auto bb = edit.tempoSequence.toBarsAndBeats (t);
        auto sixteenths = bb.getFractionalBeats().inBeats() * 4.0;
        auto rounded = std::round (sixteenths * 100.0) / 100.0;

        return juce::String (bb.bars + 1) + "." + juce::String (bb.getWholeBeats() + 1) + "."
                 + (std::abs (rounded - std::round (rounded)) < 0.001 ? juce::String ((int) std::round (rounded) + 1)
                                                                       : juce::String (rounded + 1.0, 2));
    }

    /** "5.1.3" (bar, beat, sixteenth; the beat and sixteenth are optional) to a time. */
    inline std::optional<te::TimePosition> parseSpotBars (te::Edit& edit, const juce::String& text)
    {
        auto parts = juce::StringArray::fromTokens (text.trim(), ".", {});
        parts.removeEmptyStrings();

        if (parts.isEmpty() || parts.size() > 3 || ! parts[0].containsOnly ("0123456789"))
            return {};

        auto bar = parts[0].getIntValue();
        auto beat = parts.size() > 1 ? parts[1].getDoubleValue() : 1.0;
        auto sixteenth = parts.size() > 2 ? parts[2].getDoubleValue() : 1.0;

        if (bar < 1 || beat < 1.0 || sixteenth < 1.0)
            return {};

        te::tempo::BarsAndBeats bb { bar - 1, te::BeatDuration::fromBeats ((beat - 1.0) + (sixteenth - 1.0) / 4.0) };
        return edit.tempoSequence.toTime (bb);
    }

    /** "1:23.456", "83.456" or "1:23" to a time. */
    inline std::optional<te::TimePosition> parseSpotTime (const juce::String& text)
    {
        auto t = text.trim();

        if (t.isEmpty() || ! t.containsOnly ("0123456789:."))
            return {};

        double secs = 0.0;

        for (auto& part : juce::StringArray::fromTokens (t, ":", {}))
            secs = secs * 60.0 + part.getDoubleValue();

        return te::TimePosition::fromSeconds (secs);
    }

    /** Spot: asks for the clip's exact start, in bars or in minutes and seconds. */
    inline void showSpotDialog (te::Clip& clip)
    {
        auto& edit = clip.edit;
        auto start = clip.getPosition().getStart();
        auto barsText = formatSpotBars (edit, start);
        auto timeText = EngineHelpers::formatTime (start);

        auto* aw = Theme::newAlertWindow ("Spot Clip", "Move \"" + clip.getName() + "\" to start at:");
        Theme::addTextField (*aw, "bars", barsText, "Bar.Beat.16th");
        Theme::addTextField (*aw, "time", timeText, "Min:Sec");
        aw->addButton ("Move", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, ref = te::Clip::Ptr (&clip), barsText, timeText] (int result)
        {
            if (result != 1)
                return;

            auto bars = aw->getTextEditorContents ("bars");
            auto time = aw->getTextEditorContents ("time");

            // Whichever field was changed wins; the time is the more exact one
            auto newStart = time != timeText ? parseSpotTime (time)
                          : bars != barsText ? parseSpotBars (ref->edit, bars)
                          : std::optional<te::TimePosition>();

            if (! newStart)
            {
                if (time != timeText || bars != barsText)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Spot Clip",
                                                            "That isn't a position Elsewhere understands. Try a bar like 5.1.1 or a time like 1:23.456.");
                return;
            }

            ref->edit.getUndoManager().beginNewTransaction ("Spot clip");
            ref->setStart (*newStart, false, true);
        }), true);
    }
}

} // namespace elsewhere
