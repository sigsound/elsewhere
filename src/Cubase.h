// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SessionImport.h"

namespace elsewhere
{
//==============================================================================
/**
    Cubase and Nuendo projects (.cpr, .npr), from Cubase SX to Cubase 10 and later.

    Steinberg doesn't document the format; this reads it from what's been worked out of
    real projects. A project is a big-endian RIFF file ("NUND") of named, serialized objects:
    each object is introduced by its class name the first time ("FFFFFFFF", after any base
    classes "FFFFFFFE"), or by the offset of that first declaration later (0x80000000 | offset),
    then its byte length and contents. Something written once (an audio clip used by several
    events) is referred to again by the offset of its length field.

    In the arrangement: tracks (MAudioTrackEvent) hold a list (MListNode: the track's name and
    timebase) of audio events (MAudioEvent: start, length and offset into the clip), each
    playing an audio clip (PAudioClip: file name and folder, sample rate). Starts are in ticks
    (480 per quarter note) on musical tracks and seconds on linear ones; lengths and offsets
    are in samples. Where events overlap, the later one plays, as in Cubase.

    Checked against stems bounced from Cubase: every clip lands on the sample. Not read yet:
    tempo changes (a project's first tempo is used), volume, pan, mute, fades and MIDI.
*/
namespace Cubase
{
    inline constexpr const char* fileExtensions = "cpr;npr";

    class Importer : public SessionImport::Importer
    {
    public:
        explicit Importer (const juce::File& project);
        ~Importer() override;

        juce::String getError() const override;
        bool extractAudio (const juce::File& destDir, std::function<bool (double)> progress = {}) override;
        SessionImport::Summary build (te::Edit&) override;

        /** The DAWproject the project was converted to (for checking). */
        const juce::XmlElement* getConvertedProject() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

} // namespace elsewhere
