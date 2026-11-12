// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Theme.h"
#include "DrumPatterns.h"

namespace elsewhere
{
//==============================================================================
/**
    Drum tracks (Create > New Drum Track).

    A drum track is an instrument track marked as such, whose clips are "sections":
    ordinary MIDI clips that also carry drummer settings (style, complexity, loudness,
    fills ...). A section's notes are generated from its settings whenever you change
    them or resize the section, so playback, copy/paste, undo, export and collaboration
    all work exactly as for any MIDI clip. "Convert to MIDI" drops the settings and
    leaves the notes to edit by hand.

    Notes are written for whatever instrument the track uses: Elsewhere Drums' pads, or the
    General MIDI drum map for drum plug-ins (Sitala, MT Power Drum Kit ...).
*/
namespace DrumTrack
{
    inline const juce::Identifier trackProp { "elsewhereDrumTrack" };

    /** Per-track note layout: empty = automatic, else "gm", "c1", or a custom list of 15 notes
        (one per drums::Piece, comma separated). */
    inline const juce::Identifier noteMapProp { "tdNoteMap" };

    // Section settings, stored on the MIDI clip's state
    namespace Props
    {
        inline const juce::Identifier section { "tdSection" }, style { "tdStyle" }, label { "tdLabel" },
                                      complexity { "tdComplexity" }, loudness { "tdLoudness" },
                                      part0 { "tdPart0" }, part1 { "tdPart1" }, part2 { "tdPart2" },
                                      var0 { "tdVar0" }, var1 { "tdVar1" }, var2 { "tdVar2" },
                                      fills { "tdFills" }, fillAtEnd { "tdFillEnd" }, crash { "tdCrash" },
                                      swing { "tdSwing" }, seed { "tdSeed" };

        inline bool isSettingsProperty (const juce::Identifier& id)
        {
            return id.toString().startsWith ("td");
        }
    }

    inline bool isDrumTrack (const te::Track& t)      { return (bool) t.state.getProperty (trackProp, false); }
    inline bool isSection (const juce::ValueTree& clipState) { return (bool) clipState.getProperty (Props::section, false); }
    inline bool isSection (const te::Clip& c)          { return isSection (c.state); }

    //==============================================================================
    inline drums::SectionSettings readSettings (const juce::ValueTree& v)
    {
        drums::SectionSettings s;
        s.style = (drums::Style) juce::jlimit (0, (int) drums::Style::count - 1, (int) v.getProperty (Props::style, 0));
        s.complexity = (double) v.getProperty (Props::complexity, 0.4);
        s.loudness = (double) v.getProperty (Props::loudness, 0.6);
        s.partOn = { (bool) v.getProperty (Props::part0, true), (bool) v.getProperty (Props::part1, true), (bool) v.getProperty (Props::part2, true) };
        s.variation = { (double) v.getProperty (Props::var0, 0.3), (double) v.getProperty (Props::var1, 0.3), (double) v.getProperty (Props::var2, 0.3) };
        s.fills = (double) v.getProperty (Props::fills, 0.35);
        s.fillAtEnd = (bool) v.getProperty (Props::fillAtEnd, true);
        s.crashAtStart = (bool) v.getProperty (Props::crash, true);
        s.swing = (double) v.getProperty (Props::swing, -1.0);
        s.seed = (uint32_t) (int) v.getProperty (Props::seed, 1);
        return s;
    }

    inline juce::String getLabel (const juce::ValueTree& v)   { return v.getProperty (Props::label, "Section").toString(); }

    //==============================================================================
    /** Elsewhere Drums' pad for each piece (its pads play notes 36-51 in its own order). */
    inline int elsewhereDrumsNote (drums::Piece p)
    {
        using P = drums::Piece;
        switch (p)
        {
            case P::kick:        return 36;
            case P::snare:       return 37;
            case P::clap:        return 38;
            case P::closedHat:   return 39;
            case P::pedalHat:    return 39;
            case P::openHat:     return 40;
            case P::tomLow:      return 41;
            case P::tomMid:      return 42;
            case P::tomHigh:     return 43;
            case P::rim:         return 44;
            case P::cowbell:     return 45;
            case P::crash:       return 46;
            case P::ride:        return 47;
            case P::shaker:      return 48;
            case P::tambourine:  return 48;
            case P::count:       break;
        }
        return 36;
    }

    /** Pads laid out from C1 upwards in Elsewhere Drums' order (Sitala, MPC-style samplers). */
    inline int chromaticNote (drums::Piece p)     { return elsewhereDrumsNote (p); }

    inline juce::Array<int> parseCustomMap (const juce::String& s)
    {
        juce::Array<int> notes;
        for (auto t : juce::StringArray::fromTokens (s, ",", {}))
            notes.add (juce::jlimit (0, 127, t.trim().getIntValue()));
        return notes;
    }

    inline juce::String customMapFrom (const std::function<int (drums::Piece)>& map)
    {
        juce::StringArray s;
        for (int i = 0; i < (int) drums::Piece::count; ++i) s.add (juce::String (map ((drums::Piece) i)));
        return s.joinIntoString (",");
    }

    /** The note map for the track: its chosen layout, or automatic from its instrument. */
    inline std::function<int (drums::Piece)> noteMapFor (te::Track& track)
    {
        const auto chosen = track.state.getProperty (noteMapProp).toString();

        if (chosen == "gm")  return drums::gmNote;
        if (chosen == "c1")  return chromaticNote;

        if (chosen.containsChar (','))
        {
            auto notes = parseCustomMap (chosen);
            if (notes.size() == (int) drums::Piece::count)
                return [notes] (drums::Piece p) { return notes[(int) p]; };
        }

        for (auto p : track.pluginList)
            if (p->isSynth())
                return p->getPluginType() == "elsewhereDrums" ? std::function<int (drums::Piece)> (elsewhereDrumsNote)
                                                            : std::function<int (drums::Piece)> (drums::gmNote);

        return drums::gmNote;
    }

    /** Which piece a note is, for drawing sections (reverse of the note map). */
    inline std::optional<drums::Piece> pieceForNote (const std::function<int (drums::Piece)>& map, int note)
    {
        for (int i = 0; i < (int) drums::Piece::count; ++i)
            if (map ((drums::Piece) i) == note)
                return (drums::Piece) i;
        return std::nullopt;
    }

    //==============================================================================
    /** Rewrites a section's notes from its settings. By default this joins the current undo
        transaction; pass nullptr for changes that shouldn't be undoable (a collaborator's). */
    inline void regenerate (te::MidiClip& clip, std::optional<juce::UndoManager*> undo = std::nullopt)
    {
        if (! isSection (clip) || clip.getTrack() == nullptr)
            return;

        auto& edit = clip.edit;
        auto* um = undo ? *undo : &edit.getUndoManager();
        const int beatsPerBar = juce::jmax (1, edit.tempoSequence.getTimeSig (0)->numerator.get());

        // Sections don't loop or offset: the pattern follows the section's own position
        if (um != nullptr)
        {
            if (clip.isLooping())
                clip.disableLooping();
            if (clip.getPosition().getOffset() != te::TimeDuration())
                clip.setOffset ({});
        }

        const auto start = clip.getStartBeat().inBeats();
        const auto length = (clip.getEndBeat() - clip.getStartBeat()).inBeats();
        const auto hits = drums::generate (readSettings (clip.state), start, length, beatsPerBar);
        const auto map = noteMapFor (*clip.getTrack());

        auto& seq = clip.getSequence();
        seq.clear (um);

        for (auto& h : hits)
            seq.addNote (map (h.piece), te::BeatPosition::fromBeats (h.beat), te::BeatDuration::fromBeats (h.length),
                         juce::jlimit (1, 127, juce::roundToInt (h.velocity * 127.0f)), 0, um);
    }

    /** Creates a section on a drum track. */
    inline te::MidiClip* createSection (te::AudioTrack& track, double startBeat, double endBeat,
                                        const juce::String& label = "Verse", drums::Style style = drums::Style::rock)
    {
        auto& ts = track.edit.tempoSequence;
        auto clip = track.insertMIDIClip (label, { ts.toTime (te::BeatPosition::fromBeats (startBeat)),
                                                   ts.toTime (te::BeatPosition::fromBeats (endBeat)) }, nullptr);
        if (clip == nullptr)
            return nullptr;

        auto& v = clip->state;
        v.setProperty (Props::section, true, nullptr);
        v.setProperty (Props::label, label, nullptr);
        v.setProperty (Props::style, (int) style, nullptr);
        v.setProperty (Props::seed, juce::Random::getSystemRandom().nextInt (1000000) + 1, nullptr);
        regenerate (*clip);
        return clip.get();
    }

    /** Turns a section into an ordinary MIDI clip (keeps its notes). */
    inline void convertToMidi (te::MidiClip& clip)
    {
        auto* um = &clip.edit.getUndoManager();

        // The section flag goes first, so removing the rest doesn't regenerate anything
        clip.state.removeProperty (Props::section, um);

        for (int i = clip.state.getNumProperties(); --i >= 0;)
        {
            auto name = clip.state.getPropertyName (i);
            if (Props::isSettingsProperty (name))
                clip.state.removeProperty (name, um);
        }
    }

    inline juce::Colour sectionColour (const juce::String& label)
    {
        auto l = label.toLowerCase();
        if (l.contains ("intro"))  return Colours::sectionIntro;
        if (l.contains ("verse"))  return Colours::sectionVerse;
        if (l.contains ("chorus")) return Colours::sectionChorus;
        if (l.contains ("bridge")) return Colours::sectionBridge;
        if (l.contains ("outro"))  return Colours::sectionOutro;
        if (l.contains ("break"))  return Colours::sectionBreak;
        return Colours::clips[1];
    }

    //==============================================================================
    /**
        Keeps sections' notes in step with their settings and position, for local edits and
        collaborators' alike (section notes aren't synced: each machine generates them). Local
        regeneration joins the edit's undo transaction; undo/redo restores notes with settings.
    */
    class Watcher : private juce::ValueTree::Listener
    {
    public:
        Watcher (te::Edit& e, std::function<bool()> applyingRemote)
            : edit (e), isRemote (std::move (applyingRemote))
        {
            edit.state.addListener (this);
        }

        ~Watcher() override     { edit.state.removeListener (this); }

    private:
        te::Edit& edit;
        std::function<bool()> isRemote;
        bool busy = false;

        bool shouldIgnore() const
        {
            return busy || edit.isLoading() || edit.getUndoManager().isPerformingUndoRedo();
        }

        void regenerateState (const juce::ValueTree& clipState)
        {
            if (shouldIgnore() || ! isSection (clipState))
                return;

            if (auto clip = dynamic_cast<te::MidiClip*> (te::findClipForID (edit, te::EditItemID::fromID (clipState))))
            {
                const juce::ScopedValueSetter<bool> svs (busy, true);
                const bool remote = isRemote && isRemote();
                regenerate (*clip, remote ? std::optional<juce::UndoManager*> (nullptr) : std::nullopt);
            }
        }

        void regenerateTrack (const juce::ValueTree& trackState)
        {
            if (shouldIgnore() || ! (bool) trackState.getProperty (trackProp, false))
                return;

            for (auto c : trackState)
                if (c.hasType (te::IDs::MIDICLIP) && isSection (c))
                    regenerateState (c);
        }

        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& prop) override
        {
            // A different kit note layout for the whole track
            if (prop == noteMapProp && v.hasType (te::IDs::TRACK))
                return regenerateTrack (v);

            if (! v.hasType (te::IDs::MIDICLIP) || ! isSection (v))
                return;

            // Settings changed, or the section was moved/resized
            if (Props::isSettingsProperty (prop) || prop == te::IDs::start || prop == te::IDs::length || prop == te::IDs::offset)
                regenerateState (v);
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
        {
            // A new instrument on a drum track: its note layout may differ
            if (child.hasType (te::IDs::PLUGIN) && parent.hasType (te::IDs::TRACK))
                regenerateTrack (parent);
        }

        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
        {
            if (child.hasType (te::IDs::PLUGIN) && parent.hasType (te::IDs::TRACK))
                regenerateTrack (parent);
        }
    };
}

} // namespace elsewhere
