// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Capture MIDI: the engine keeps the last 30 seconds played on every MIDI input, recording
    or not. Capturing turns what's there into a clip on the track the input plays (the selected
    instrument track), placed where it was played if the song was playing, or ending at the
    playhead if it wasn't. The clip is widened to whole bars, so it loops and moves cleanly.
*/
namespace MidiCapture
{
    /** Makes clips from what was played; returns them (empty if nothing was played). */
    inline juce::Array<te::Clip*> capture (te::Edit& edit)
    {
        juce::Array<te::Clip*> clips;
        auto* context = edit.getCurrentPlaybackContext();

        if (context == nullptr)
            return clips;

        for (auto* instance : context->getAllInputs())
        {
            auto type = instance->getInputDevice().getDeviceType();

            if (type != te::InputDevice::physicalMidiDevice && type != te::InputDevice::virtualMidiDevice)
                continue;

            clips.addArray (instance->applyRetrospectiveRecord (false));
        }

        auto& tempo = edit.tempoSequence;

        for (auto* clip : clips)
        {
            auto midi = dynamic_cast<te::MidiClip*> (clip);

            if (midi == nullptr)
                continue;

            // Trim to the notes, then out to the bars around them
            auto& seq = midi->getSequence();

            if (seq.getNumNotes() == 0)
                continue;

            auto first = te::BeatPosition::fromBeats (std::numeric_limits<double>::max());
            auto last = te::BeatPosition();

            for (auto* n : seq.getNotes())
            {
                first = std::min (first, n->getStartBeat());
                last = std::max (last, n->getEndBeat());
            }

            auto toTime = [midi] (te::BeatPosition b) { return midi->getTimeOfContentBeat (b); };
            auto barStart = [&tempo] (te::TimePosition t)
            {
                auto bb = tempo.toBarsAndBeats (t);
                return tempo.toTime (te::tempo::BarsAndBeats { bb.bars, {} });
            };

            auto start = barStart (toTime (first));
            auto end = barStart (toTime (last));

            if (end < toTime (last))
            {
                auto bb = tempo.toBarsAndBeats (end);
                end = tempo.toTime (te::tempo::BarsAndBeats { bb.bars + 1, {} });
            }

            midi->setStart (start, true, false);
            midi->setEnd (end, true);
            midi->setName ("Captured");
        }

        return clips;
    }
}

} // namespace elsewhere
