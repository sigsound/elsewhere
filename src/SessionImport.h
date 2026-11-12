// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Opening songs made in other DAWs: Pro Tools sessions, Ableton Live Sets, Cubase projects, AAF (Pro Tools, Logic,
    Media Composer, Premiere, Resolve) and DAWproject (Bitwig, Studio One, Cubase). Each format has its own
    Importer; File > Import Session goes through this interface.
*/
namespace SessionImport
{
    inline constexpr const char* wildcard = "*.als;*.cpr;*.npr;*.dawproject;*.aaf;*.ptx;*.ptf;*.pts";

    struct Summary
    {
        juce::String applicationName;           // the program that wrote the file, if it says
        int tracks = 0, audioClips = 0, midiClips = 0;
        juce::StringArray missingFiles;         // audio the project refers to that couldn't be found
        int skippedReturnTracks = 0;
        double firstSampleRate = 0.0;
        bool mixedSampleRates = false;
        juce::StringArray notes;                // what doesn't come across, etc.
        double headroomDb = 0.0;                // how far the master was turned down so it doesn't clip

        juce::String describe() const;
    };

    /** Reads a session in three steps, so the slow audio copying can run in the background. */
    class Importer
    {
    public:
        virtual ~Importer() = default;

        /** Set if the file couldn't be opened. */
        virtual juce::String getError() const = 0;

        /** Copies every audio file the session uses into destDir. Safe to call from a background
            thread. The callback gets 0..1 and returns false to cancel. */
        virtual bool extractAudio (const juce::File& destDir, std::function<bool (double)> progress = {}) = 0;

        /** Builds the arrangement into an Edit (meant for a new, empty one: tracks without clips
            are removed first). Call on the message thread after extractAudio(). */
        virtual Summary build (te::Edit&) = 0;
    };

    bool canImport (const juce::File&);

    /** The right importer for the file's type, already opened. */
    std::unique_ptr<Importer> open (const juce::File&);

    //==============================================================================
    // Shared by the importers

    /** New projects start with a placeholder track or two. */
    void removePlaceholderTracks (te::Edit&);

    /** A plain (not tempo-following) audio clip. sourceStart is in the file's seconds, and speed is
        source seconds per timeline second. */
    te::WaveAudioClip::Ptr addAudioClip (te::AudioTrack&, const juce::String& name, const juce::File&,
                                          double start, double end, double sourceStart, double speed = 1.0, bool stretch = true);

    /** One clip whose speed changes along the way (a song warped to the grid, say): points map
        timeline seconds to seconds in the file, in order. Uses Elsewhere's warp markers, so it plays
        from a warped copy rendered in the background. */
    te::WaveAudioClip::Ptr addWarpedAudioClip (te::AudioTrack&, const juce::String& name, const juce::File&,
                                                const std::vector<std::pair<double, double>>& timelineToSource);

    /** Finds a file by name (any case) in a folder or its subfolders: for audio a session refers
        to by a path on another computer. The index is filled on first use, so reuse it. */
    juce::File findFileNear (const juce::File& folder, const juce::String& fileName, std::map<juce::String, juce::File>& index);

    /** -1 or 1 for one side of a stereo pair split into mono files ("Gtr.L", "Gtr_R_03.wav"...). */
    std::optional<double> stereoSideFromName (const juce::String& fileName);

    /** Plays the arrangement through once (offline) and, if it would clip, turns the master down
        so it peaks at -1 dB. Tracks keep their balance. Returns how far it was turned down (dB).
        Imports without the original faders (Pro Tools, Cubase) come in with every track at 0 dB,
        which on a big session goes well over. */
    double addHeadroom (te::Edit&);

    /** Gives MIDI-only tracks a synth, adds clip slots and sends, and notes the sample rates used. */
    void finish (te::Edit&, const std::set<te::AudioTrack*>& midiTracks, const juce::Array<juce::File>& audioFiles, Summary&);
}

} // namespace elsewhere
