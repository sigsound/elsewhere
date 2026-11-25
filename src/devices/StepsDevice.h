// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "MidiEffects.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Steps: a step sequencer made of rows.

    Each row plays one note and has a name ("Kick", "Bass", whatever it is for). Along the row
    are up to 32 steps, each on or off, and each with:

      - Velocity
      - Length: how long the note lasts, as a share of the Gate. Past 100 % it runs into the
        next step, which a synth with glide plays as a slide
      - Chance: how likely the step is to play
      - Repeat: plays the note several times within the step
      - Nudge: moves the step earlier or later
      - Pitch: semitones away from the row's note, kept to the Scale
      - Every: plays only on some trips round (the 1st of 2, the 3rd of 4 ...)

    A row can go round in its own number of steps, so rows of 16, 12 and 5 drift apart and meet
    again. Rows can run forwards, backwards, back and forth, or in a random order.

    It plays with the transport, or (Follow Keys) only while you hold a key, moved up or down by
    how far that key is from the Root.

    Which step plays when is worked out from where the song is, not counted, so a pattern sounds
    the same from wherever you start. Chance and Random are rolled per step and per time through,
    and repeat exactly on a mixdown.

    The rows live in the device's state as ROW children (a line of text per kind of value), not
    as parameters: eight rows of 32 steps would be two thousand of them. They are undone, saved
    and shared like everything else in the project.
*/
class StepsDevice : public MidiEffectDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereSteps";
    static constexpr int numSteps = 32, maxRows = 8;

    enum Lane { velocityLane, lengthLane, chanceLane, repeatLane, nudgeLane, pitchLane, everyLane, numLanes };

    /** A kind of value a step has: its name, the property a row keeps it in, and its range. */
    struct LaneInfo { const char* name; const char* id; int min, max, normal; };

    static const LaneInfo& laneInfo (int lane)
    {
        static const LaneInfo all[numLanes] = {
            { "Velocity", "vel",    1,   127, 100 },
            { "Length",   "len",    10,  400, 100 },
            { "Chance",   "chance", 0,   100, 100 },
            { "Repeat",   "rep",    1,   8,   1 },
            { "Nudge",    "nudge",  -50, 50,  0 },
            { "Pitch",    "pitch",  -24, 24,  0 },
            { "Every",    "every",  0,   9,   0 },
        };
        return all[juce::jlimit (0, numLanes - 1, lane)];
    }

    static juce::StringArray laneNames()
    {
        juce::StringArray names;
        for (int l = 0; l < numLanes; ++l) names.add (laneInfo (l).name);
        return names;
    }

    enum Direction { forward, backward, bounce, randomOrder, numDirections };
    static juce::StringArray directionNames()   { return { "Forward", "Back", "Bounce", "Random" }; }

    /** Scales, as the semitones above the root that belong to them. */
    struct Scale { const char* name; std::vector<int> notes; };

    static const std::vector<Scale>& scales()
    {
        static const std::vector<Scale> all = {
            { "Any Note",         { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
            { "Major",            { 0, 2, 4, 5, 7, 9, 11 } },
            { "Minor",            { 0, 2, 3, 5, 7, 8, 10 } },
            { "Dorian",           { 0, 2, 3, 5, 7, 9, 10 } },
            { "Phrygian",         { 0, 1, 3, 5, 7, 8, 10 } },
            { "Lydian",           { 0, 2, 4, 6, 7, 9, 11 } },
            { "Mixolydian",       { 0, 2, 4, 5, 7, 9, 10 } },
            { "Harmonic Minor",   { 0, 2, 3, 5, 7, 8, 11 } },
            { "Major Pentatonic", { 0, 2, 4, 7, 9 } },
            { "Minor Pentatonic", { 0, 3, 5, 7, 10 } },
            { "Blues",            { 0, 3, 5, 6, 7, 10 } },
            { "Whole Tone",       { 0, 2, 4, 6, 8, 10 } },
        };
        return all;
    }

    /** Moves a pitch (semitones from the root) down to the nearest note of a scale. */
    static int snapToScale (int semitones, int scaleIndex)
    {
        const auto& notes = scales()[(size_t) juce::jlimit (0, (int) scales().size() - 1, scaleIndex)].notes;
        const int octave = (int) std::floor (semitones / 12.0), within = semitones - octave * 12;
        int best = 0;
        for (int n : notes)
            if (n <= within)
                best = n;
        return octave * 12 + best;
    }

    static juce::StringArray everyNames()
    {
        return { "Always", "1st of 2", "2nd of 2", "1st of 3", "2nd of 3", "3rd of 3", "1st of 4", "2nd of 4", "3rd of 4", "4th of 4" };
    }

    /** An Every setting the short way, for a cell: "2:3". */
    static juce::String everyShort (int setting)
    {
        if (setting <= 0) return {};
        const int of = setting <= 2 ? 2 : setting <= 5 ? 3 : 4;
        return juce::String (setting - (of == 2 ? 0 : of == 3 ? 2 : 5)) + ":" + juce::String (of);
    }

    /** Whether an Every setting lets a step play on this trip round (counted from 0). */
    static bool everyAllows (int setting, long long trip)
    {
        if (setting <= 0) return true;
        const int of = setting <= 2 ? 2 : setting <= 5 ? 3 : 4;
        const int which = setting - (of == 2 ? 1 : of == 3 ? 3 : 6);
        return trip % of == which;
    }

    //==============================================================================
    /** The rows as the audio thread (and the panel) reads them. */
    struct Row
    {
        int note = 48, steps = 0;      // steps: 0 goes round with the pattern
        bool mute = false;
        std::array<bool, numSteps> on {};
        std::array<std::array<short, numSteps>, numLanes> value {};
    };

    struct Pattern
    {
        int numRows = 0;
        std::array<Row, maxRows> rows;
    };

    StepsDevice (te::PluginCreationInfo info) : MidiEffectDevice (info)
    {
        auto wholeText = [] (float v) { return juce::String (juce::roundToInt (v)); };

        rateP  = addParameter ("rate", "Rate", 3.0f, stepped (0, 5), [] (float v) { return rateNames()[juce::jlimit (0, 5, juce::roundToInt (v))]; });
        length = addParameter ("length", "Steps", 16.0f, stepped (1, numSteps), wholeText);
        swing  = addParameter ("swing", "Swing", 0.0f, { 0.0f, 75.0f }, percentText);
        gate   = addParameter ("gate", "Gate", 50.0f, { 5.0f, 200.0f }, percentText);
        root   = addParameter ("root", "Root", 48.0f, stepped (24, 96), [] (float v) { return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, true, 3); });
        follow = addParameter ("follow", "Follow Keys", 0.0f, stepped (0, 1), onOffText);
        direction = addParameter ("direction", "Direction", (float) forward, stepped (0, numDirections - 1),
                                  [] (float v) { return directionNames()[juce::jlimit (0, numDirections - 1, juce::roundToInt (v))]; });
        scale  = addParameter ("scale", "Scale", 0.0f, stepped (0, (float) scales().size() - 1.0f),
                               [] (float v) { return juce::String (scales()[(size_t) juce::jlimit (0, (int) scales().size() - 1, juce::roundToInt (v))].name); });

        if (getNumRowTrees() == 0)
            state.appendChild (rowFromOldSteps (state), nullptr);

        state.addListener (&watcher);
        refresh();
    }

    ~StepsDevice() override     { state.removeListener (&watcher); }

    juce::String getName() const override              { return "Elsewhere Steps"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Steps"; }
    int getDeviceViewWidth() const override            { return 980; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1120, 620 }; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        turn = 0;   // (so a mixdown rolls the same dice every time)
        MidiEffectDevice::initialise (info);
    }

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        ElsewhereDevice::restorePluginStateFromValueTree (v);

        for (int i = state.getNumChildren(); --i >= 0;)
            if (state.getChild (i).hasType (rowType()))
                state.removeChild (i, nullptr);

        bool any = false;
        for (auto child : v)
            if (child.hasType (rowType()) && getNumRowTrees() < maxRows)
            {
                state.appendChild (child.createCopy(), nullptr);
                any = true;
            }

        // (a preset or project from before there were rows: one row, made from its steps)
        if (! any)
            state.appendChild (rowFromOldSteps (v), nullptr);

        refresh();
    }

    //==============================================================================
    // Reading the pattern (on the message thread)
    const Pattern& getPattern() const       { return shown; }
    int getNumRows() const                  { return shown.numRows; }
    int patternSteps() const                { return juce::jlimit (1, numSteps, juce::roundToInt (length.get())); }
    int stepsInRow (int row) const          { return stepsIn (shown.rows[(size_t) row], patternSteps()); }
    juce::String getRowName (int row) const { return rowTree (row).getProperty (nameId()).toString(); }

    /** Goes up whenever the rows change, so a panel knows to redraw. */
    int getChangeCount() const              { return version.load(); }

    /** Which step is under the playhead in a row of `steps` steps at step count `k`. */
    int indexFor (int steps, long long k, int row = 0) const
    {
        const auto count = (long long) juce::jmax (1, steps);

        switch ((Direction) juce::jlimit (0, numDirections - 1, juce::roundToInt (direction.get())))
        {
            case backward:      return (int) (count - 1 - k % count);
            case bounce:
            {
                if (count < 2) return 0;
                const auto there = k % (2 * count - 2);
                return (int) (there < count ? there : 2 * count - 2 - there);
            }
            case randomOrder:   return (int) (roll (k, 0x51ed + (juce::uint32) row * 977u) % (juce::uint32) count);
            case forward: case numDirections: break;
        }
        return (int) (k % count);
    }

    /** The step count the pattern is at (for the playheads), -1 when idle. */
    long long getCurrentCount() const       { return currentCount.load(); }

    //==============================================================================
    // Changing the pattern (on the message thread). Each change can be undone; call beginEdit
    // first to name what a run of changes is.
    void beginEdit (const juce::String& what)       { edit.getUndoManager().beginNewTransaction (what); }

    void setOn (int row, int step, bool shouldBeOn)
    {
        if (! isRow (row) || ! juce::isPositiveAndBelow (step, numSteps)) return;
        auto text = onText (shown.rows[(size_t) row]);
        text = text.replaceSection (step, 1, shouldBeOn ? "1" : "0");
        rowTree (row).setProperty (onId(), text, um());
    }

    void setStep (int row, int lane, int step, int value)
    {
        if (! isRow (row) || ! juce::isPositiveAndBelow (step, numSteps)) return;
        auto values = shown.rows[(size_t) row].value[(size_t) lane];
        values[(size_t) step] = (short) juce::jlimit (laneInfo (lane).min, laneInfo (lane).max, value);
        rowTree (row).setProperty (laneInfo (lane).id, laneText (values), um());
    }

    void setRowName (int row, const juce::String& name)     { if (isRow (row)) rowTree (row).setProperty (nameId(), name.trim().substring (0, 24), um()); }
    void setRowNote (int row, int note)                     { if (isRow (row)) rowTree (row).setProperty (noteId(), juce::jlimit (0, 127, note), um()); }
    void setRowMute (int row, bool mute)                    { if (isRow (row)) rowTree (row).setProperty (muteId(), mute, um()); }
    void setRowSteps (int row, int steps)                   { if (isRow (row)) rowTree (row).setProperty (stepsId(), juce::jlimit (0, numSteps, steps), um()); }

    bool canAddRow() const      { return getNumRows() < maxRows; }

    /** Adds an empty row; returns where it is, or -1 if there are as many as there can be. */
    int addRow (const juce::String& name, int note)
    {
        if (! canAddRow()) return -1;
        state.appendChild (newRowTree (name, note), um());
        return getNumRows() - 1;
    }

    void removeRow (int row)
    {
        if (isRow (row) && getNumRows() > 1)
            state.removeChild (rowTree (row), um());
    }

    void duplicateRow (int row)
    {
        if (! isRow (row) || ! canAddRow()) return;
        auto copy = rowTree (row).createCopy();
        state.addChild (copy, state.indexOf (rowTree (row)) + 1, um());
    }

    void clearRow (int row)
    {
        if (isRow (row))
            rowTree (row).setProperty (onId(), juce::String::repeatedString ("0", numSteps), um());
    }

    /** Different values for one kind of thing along a row. */
    void randomiseRow (int row, int lane)
    {
        if (! isRow (row)) return;
        juce::Random r;
        auto values = shown.rows[(size_t) row].value[(size_t) lane];

        for (int i = 0; i < stepsInRow (row); ++i)
        {
            int v = 0;
            switch (lane)
            {
                case velocityLane:  v = r.nextInt ({ 45, 128 }); break;
                case lengthLane:    { static const int l[] = { 50, 100, 100, 100, 150, 200 }; v = l[r.nextInt (6)]; break; }
                case chanceLane:    v = r.nextFloat() < 0.5f ? 100 : r.nextInt ({ 30, 100 }); break;
                case repeatLane:    v = r.nextFloat() < 0.75f ? 1 : r.nextInt ({ 2, 5 }); break;
                case nudgeLane:     v = r.nextInt ({ -15, 16 }); break;
                case pitchLane:     v = snapToScale (r.nextInt ({ -12, 16 }), juce::roundToInt (scale.get())); break;
                case everyLane:     v = r.nextFloat() < 0.7f ? 0 : r.nextInt ({ 1, 10 }); break;
                default: break;
            }
            values[(size_t) i] = (short) v;
        }

        rowTree (row).setProperty (laneInfo (lane).id, laneText (values), um());
    }

    /** Different steps playing along a row. */
    void randomiseRowSteps (int row)
    {
        if (! isRow (row)) return;
        juce::Random r;
        juce::String text;
        for (int i = 0; i < numSteps; ++i)
            text << (i < stepsInRow (row) && (r.nextFloat() < 0.45f) ? "1" : "0");
        rowTree (row).setProperty (onId(), text, um());
    }

    /** Sets every step of a row back to the ordinary value for one kind of thing. */
    void resetRow (int row, int lane)
    {
        if (! isRow (row)) return;
        std::array<short, numSteps> values;
        values.fill ((short) laneInfo (lane).normal);
        rowTree (row).setProperty (laneInfo (lane).id, laneText (values), um());
    }

    /** Moves a row (all it holds) one step later (+1) or earlier (-1), round its end. */
    void shiftRow (int row, int delta)
    {
        if (! isRow (row)) return;
        const auto& was = shown.rows[(size_t) row];
        const int len = stepsInRow (row);
        auto from = [len, delta] (int i) { return ((i - delta) % len + len) % len; };

        Row moved = was;
        for (int i = 0; i < len; ++i)
        {
            moved.on[(size_t) i] = was.on[(size_t) from (i)];
            for (int l = 0; l < numLanes; ++l)
                moved.value[(size_t) l][(size_t) i] = was.value[(size_t) l][(size_t) from (i)];
        }

        writeRow (row, moved);
    }

    void shiftPattern (int delta)
    {
        for (int r = 0; r < getNumRows(); ++r)
            shiftRow (r, delta);
    }

    bool canDouble() const      { return patternSteps() * 2 <= numSteps; }

    /** Twice as many steps, the second half a copy of the first, ready to be changed. */
    void doubleLength()
    {
        if (! canDouble()) return;
        const int len = patternSteps();

        for (int r = 0; r < getNumRows(); ++r)
        {
            Row row = shown.rows[(size_t) r];
            if (row.steps > 0) continue;    // (a row with its own count keeps going round as it was)

            for (int i = 0; i < len; ++i)
            {
                row.on[(size_t) (i + len)] = row.on[(size_t) i];
                for (int l = 0; l < numLanes; ++l)
                    row.value[(size_t) l][(size_t) (i + len)] = row.value[(size_t) l][(size_t) i];
            }
            writeRow (r, row);
        }

        length.param().setParameter ((float) (len * 2), juce::sendNotification);
    }

    /** Back to the pattern a new Step Sequencer starts with. */
    void loadDefaults()
    {
        for (auto& p : params)
            p->getParameter().setParameter (p->getParameter().getDefaultValue().value_or (p->getParameter().getCurrentBaseValue()), juce::sendNotification);

        for (int i = state.getNumChildren(); --i >= 0;)
            if (state.getChild (i).hasType (rowType()))
                state.removeChild (i, um());

        state.appendChild (rowFromOldSteps ({}), um());
    }

    /** Rows for a drum kit: the first pads of Elsewhere's Drums, named, with a plain beat. */
    void loadDrumRows()
    {
        static const char* names[] = { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat" };
        static const char* beats[] = { "10000000100000001000000010000000",
                                       "00001000000010000000100000001000",
                                       "00000000000000000000000000000000",
                                       "10101010101010101010101010101010",
                                       "00000000000000000000000000000000" };

        for (int i = state.getNumChildren(); --i >= 0;)
            if (state.getChild (i).hasType (rowType()))
                state.removeChild (i, um());

        for (int i = 0; i < 5; ++i)
        {
            auto row = newRowTree (names[i], 36 + i);
            row.setProperty (onId(), juce::String (beats[i]), nullptr);
            state.appendChild (row, um());
        }
    }

    Param rateP, length, swing, gate, root, follow, direction, scale;

private:
    static const juce::Identifier& rowType()    { static const juce::Identifier i ("ROW"); return i; }
    static const juce::Identifier& nameId()     { static const juce::Identifier i ("name"); return i; }
    static const juce::Identifier& noteId()     { static const juce::Identifier i ("note"); return i; }
    static const juce::Identifier& muteId()     { static const juce::Identifier i ("mute"); return i; }
    static const juce::Identifier& stepsId()    { static const juce::Identifier i ("steps"); return i; }
    static const juce::Identifier& onId()       { static const juce::Identifier i ("on"); return i; }

    juce::UndoManager* um()     { return &edit.getUndoManager(); }
    bool isRow (int row) const  { return juce::isPositiveAndBelow (row, getNumRows()); }

    int getNumRowTrees() const
    {
        int n = 0;
        for (auto child : state)
            if (child.hasType (rowType())) ++n;
        return n;
    }

    juce::ValueTree rowTree (int row) const
    {
        int n = 0;
        for (auto child : state)
            if (child.hasType (rowType()) && n++ == row)
                return child;
        return {};
    }

    static int stepsIn (const Row& row, int patternLength)      { return row.steps > 0 ? juce::jmin (row.steps, numSteps) : patternLength; }

    static juce::String onText (const Row& row)
    {
        juce::String text;
        for (bool b : row.on) text << (b ? "1" : "0");
        return text;
    }

    static juce::String laneText (const std::array<short, numSteps>& values)
    {
        juce::StringArray parts;
        for (auto v : values) parts.add (juce::String ((int) v));
        return parts.joinIntoString (",");
    }

    static juce::ValueTree newRowTree (const juce::String& name, int note)
    {
        juce::ValueTree row (rowType());
        row.setProperty (nameId(), name, nullptr);
        row.setProperty (noteId(), juce::jlimit (0, 127, note), nullptr);
        row.setProperty (onId(), juce::String::repeatedString ("0", numSteps), nullptr);
        return row;
    }

    /** The one row a pattern had before there were rows, read from the properties its steps
        were kept in (or, where those are missing, the pattern a new sequencer starts with). */
    static juce::ValueTree rowFromOldSteps (const juce::ValueTree& old)
    {
        // A simple starting pattern: root on the beats, a fifth and an octave in between
        static const int defaultOn[16]    = { 1,0,1,0, 1,0,1,1, 1,0,1,0, 1,0,1,1 };
        static const int defaultPitch[16] = { 0,0,7,0, 12,0,7,10, 0,0,7,0, 12,0,15,12 };
        static const char* oldIds[numLanes] = { "vel", "len", "chance", "rep", "nudge", "pitch", "every" };

        Row row;
        row.note = juce::roundToInt ((float) old.getProperty ("root", 48.0f));

        for (int i = 0; i < numSteps; ++i)
        {
            const auto id = "s" + juce::String (i + 1);
            row.on[(size_t) i] = (float) old.getProperty (juce::Identifier (id + "on"), i < 16 ? (float) defaultOn[i] : 0.0f) > 0.5f;

            for (int l = 0; l < numLanes; ++l)
            {
                float normal = (float) laneInfo (l).normal;
                if (l == velocityLane)  normal = i % 4 == 0 ? 110.0f : 85.0f;
                if (l == pitchLane)     normal = i < 16 ? (float) defaultPitch[i] : 0.0f;
                row.value[(size_t) l][(size_t) i] = (short) juce::jlimit (laneInfo (l).min, laneInfo (l).max,
                                                                          juce::roundToInt ((float) old.getProperty (juce::Identifier (id + oldIds[l]), normal)));
            }
        }

        auto tree = newRowTree ("Notes", row.note);
        tree.setProperty (onId(), onText (row), nullptr);
        for (int l = 0; l < numLanes; ++l)
            tree.setProperty (laneInfo (l).id, laneText (row.value[(size_t) l]), nullptr);
        return tree;
    }

    void writeRow (int row, const Row& r)
    {
        auto tree = rowTree (row);
        tree.setProperty (onId(), onText (r), um());
        for (int l = 0; l < numLanes; ++l)
            tree.setProperty (laneInfo (l).id, laneText (r.value[(size_t) l]), um());
    }

    /** Reads the rows out of the state, and hands them to the audio thread. */
    void refresh()
    {
        Pattern p;

        for (auto child : state)
        {
            if (! child.hasType (rowType()) || p.numRows >= maxRows)
                continue;

            auto& row = p.rows[(size_t) p.numRows++];
            row.note = juce::jlimit (0, 127, (int) child.getProperty (noteId(), 48));
            row.steps = juce::jlimit (0, numSteps, (int) child.getProperty (stepsId(), 0));
            row.mute = (bool) child.getProperty (muteId(), false);

            const auto on = child.getProperty (onId()).toString();
            for (int i = 0; i < numSteps; ++i)
                row.on[(size_t) i] = i < on.length() && on[i] == '1';

            for (int l = 0; l < numLanes; ++l)
            {
                const auto parts = juce::StringArray::fromTokens (child.getProperty (laneInfo (l).id).toString(), ",", "");
                for (int i = 0; i < numSteps; ++i)
                    row.value[(size_t) l][(size_t) i] = (short) (i < parts.size() ? juce::jlimit (laneInfo (l).min, laneInfo (l).max, parts[i].getIntValue())
                                                                                 : laneInfo (l).normal);
            }
        }

        shown = p;

        {
            const juce::SpinLock::ScopedLockType lock (patternLock);
            handed = p;
        }

        ++version;
    }

    struct Watcher : public juce::ValueTree::Listener
    {
        Watcher (StepsDevice& d) : device (d) {}
        StepsDevice& device;

        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier&) override   { if (v.hasType (rowType())) device.refresh(); }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override               { if (c.hasType (rowType())) device.refresh(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override        { if (c.hasType (rowType())) device.refresh(); }
        void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override                { if (p == device.state) device.refresh(); }
    };

    Watcher watcher { *this };
    Pattern shown;                      // for the message thread
    Pattern handed, playing;            // handed over under the lock; the audio thread's own copy
    juce::SpinLock patternLock;
    std::atomic<int> version { 0 };
    int heard = -1;                     // the version the audio thread has

    std::array<int, 128> keysDown {};
    int transposeNote = -1;
    double freeOrigin = -1.0;           // stopped, following keys: the beat the first key went down on
    juce::uint32 turn = 0;              // changes each time the pattern starts over, so chance differs each time round a loop
    std::atomic<long long> currentCount { -1 };

    void onReset() override
    {
        freeOrigin = -1.0;
        currentStep = -1;
        currentCount = -1;
        turn = rendering ? 1 : turn + 1;    // (a mixdown always rolls the same dice)
    }

    /** A dice roll that is always the same for the same step count, use and time through. */
    juce::uint32 roll (long long k, juce::uint32 use) const
    {
        auto x = (juce::uint64) k * 0x9E3779B97F4A7C15ull + (juce::uint64) (turn * 7919u + use) * 0xBF58476D1CE4E5B9ull;
        x ^= x >> 31; x *= 0x94D049BB133111EBull; x ^= x >> 29;
        return (juce::uint32) (x >> 16);
    }

    void process (const te::MidiMessageArray& in, const Block& blk) override
    {
        // Pick up a changed pattern (never waiting: if it is being changed right now, next time)
        if (heard != version.load())
        {
            const juce::SpinLock::ScopedTryLockType lock (patternLock);

            if (lock.isLocked())
            {
                heard = version.load();
                playing = handed;
            }
        }

        const bool followKeys = follow.get() > 0.5f;
        int firstKeyOffset = -1;

        for (auto& m : in)
        {
            const int offset = juce::roundToInt (m.getTimeStamp() * rate);

            if (followKeys && m.isNoteOn())
            {
                if (transposeNote < 0) firstKeyOffset = offset;
                keysDown[(size_t) m.getNoteNumber()] = 1;
                transposeNote = m.getNoteNumber();
            }
            else if (followKeys && m.isNoteOff())
            {
                keysDown[(size_t) m.getNoteNumber()] = 0;
                if (m.getNoteNumber() == transposeNote)
                {
                    transposeNote = -1;
                    for (int k = 127; k >= 0; --k) if (keysDown[(size_t) k] != 0) { transposeNote = k; break; }
                }
            }
            else
            {
                emit (m, offset);   // without Follow Keys, what you play passes through on top of the pattern
            }
        }

        if (followKeys && transposeNote < 0)
        {
            onReset();
            return;
        }

        // The grid the steps sit on: the song's beats, or (stopped, following keys) beats counted from the first key
        double origin = 0.0;

        if (! blk.playing)
        {
            if (! followKeys)
            {
                currentStep = -1;
                currentCount = -1;
                return;
            }

            if (freeOrigin < 0.0)
                freeOrigin = blk.startBeat + juce::jmax (0, firstKeyOffset) * blk.beatsPerSample;
            origin = freeOrigin;
        }

        const int len = patternSteps();
        const double stepBeats = rateInBeats (rateP.get());
        const double start = blk.startBeat - origin, end = blk.endBeat() - origin;
        const double gateShare = gate.get() / 100.0;
        const int rootNote = juce::roundToInt (root.get());
        const int moved = followKeys ? transposeNote - rootNote : 0;    // the Root key plays it as written
        const int scaleIndex = juce::roundToInt (scale.get());
        const float swingAmount = swing.get();
        const auto now = (long long) std::floor (start / stepBeats);

        if (now >= 0)
        {
            currentCount = now;
            currentStep = indexFor (len, now);
        }

        // A step can sound up to half a step early (Nudge) and, with Swing, Nudge and Repeat, well
        // into the steps after it: look at the ones either side of this block
        for (auto k = juce::jmax (0LL, now - 2); ((double) k - 0.5) * stepBeats < end; ++k)
        {
            for (int r = 0; r < playing.numRows; ++r)
            {
                const auto& row = playing.rows[(size_t) r];
                if (row.mute)
                    continue;

                const int rowLen = stepsIn (row, len);
                const auto i = (size_t) indexFor (rowLen, k, r);
                if (! row.on[i])
                    continue;

                if (! everyAllows (row.value[everyLane][i], k / rowLen))
                    continue;

                const int chance = row.value[chanceLane][i];
                if (chance < 100 && (int) (roll (k, 0xc4a9 + (juce::uint32) r * 131u) % 100u) >= chance)
                    continue;

                const double nudge = row.value[nudgeLane][i] / 100.0;
                const double at = juce::jmax (0.0, ((double) k + nudge) * stepBeats + swingOffset (k, stepBeats, swingAmount));
                const int repeats = juce::jlimit (1, 8, (int) row.value[repeatLane][i]);
                const double each = stepBeats / repeats;
                const double share = gateShare * row.value[lengthLane][i] / 100.0;
                const double lengthSamples = each * (repeats > 1 ? juce::jmin (0.95, share) : share) / blk.beatsPerSample;

                // (the scale is counted from the Root, whichever note the row is on)
                const int written = row.note + row.value[pitchLane][i];
                const int note = rootNote + snapToScale (written - rootNote, scaleIndex) + moved;
                const float velocity = row.value[velocityLane][i] / 127.0f;

                for (int j = 0; j < repeats; ++j)
                {
                    const double hit = at + j * each;
                    if (hit >= start - 1.0e-9 && hit < end - 1.0e-9)
                        playNote (1, note, velocity, blk.sampleAt (hit + origin), lengthSamples);
                }
            }
        }
    }
};

//==============================================================================
/** The rows, to draw on. Click a step to turn it on or off (drag to paint a run of them);
    drag on a step that is on to set the value the tab above is showing. Each kind of value is
    drawn the way it sounds: a nudged step sits off the grid, a long one reaches over its
    neighbours, an unlikely one is faint, a repeated one is cut into pieces. */
class StepRows : public juce::Component, private juce::Timer
{
public:
    StepRows (StepsDevice& d) : dev (d)
    {
        setTitle ("Steps");
        startTimerHz (30);
    }

    std::function<void()> onSelectionChanged;

    void setLane (int l)            { lane = l; repaint(); }
    int getLane() const             { return lane; }
    int getSelectedRow() const      { return juce::jlimit (0, juce::jmax (0, dev.getNumRows() - 1), selectedRow); }

    /** How wide the names down the left are. */
    void setHeaderWidth (int w)     { headerW = w; repaint(); }

    static juce::Colour rowColour (int row)     { return Colours::clips[(size_t) ((row * 5 + 1) % (int) Colours::clips.size())]; }

    /** 16 across until something needs more. */
    int columns() const
    {
        if (dev.patternSteps() > 16) return 32;
        for (int r = 0; r < dev.getNumRows(); ++r)
            if (dev.stepsInRow (r) > 16) return 32;
        return 16;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& pattern = dev.getPattern();
        const int cols = columns();
        const auto count = dev.getCurrentCount();
        const float rowH = rowHeight();

        for (int r = 0; r < pattern.numRows; ++r)
        {
            const auto& row = pattern.rows[(size_t) r];
            const auto colour = rowColour (r);
            const int len = dev.stepsInRow (r);
            const int playing = count >= 0 && ! row.mute ? dev.indexFor (len, count, r) : -1;
            auto area = juce::Rectangle<float> (0.0f, rowH * (float) r, (float) getWidth(), rowH).reduced (0.0f, 1.0f);

            // Name, note, and the dot that mutes it
            auto header = area.removeFromLeft ((float) headerW);
            if (r == getSelectedRow())
            {
                g.setColour (Colours::panelLight.withAlpha (0.6f));
                g.fillRoundedRectangle (header.withTrimmedRight (4.0f), 3.0f);
            }

            auto dot = header.removeFromLeft (16.0f).withSizeKeepingCentre (9.0f, 9.0f);
            g.setColour (colour);
            if (row.mute) g.drawEllipse (dot.reduced (0.5f), 1.2f);
            else          g.fillEllipse (dot);

            const float fontH = juce::jlimit (9.5f, 13.0f, rowH * 0.6f);
            auto noteBox = header.removeFromRight (34.0f).withTrimmedRight (6.0f);
            g.setFont (juce::FontOptions (fontH - 1.0f));
            g.setColour (Colours::textDim);
            g.drawText (juce::MidiMessage::getMidiNoteName (row.note, true, true, 3), noteBox, juce::Justification::centredRight);
            g.setFont (juce::FontOptions (fontH, juce::Font::bold));
            g.setColour (row.mute ? Colours::textDim : Colours::text);
            g.drawText (dev.getRowName (r) + (row.steps > 0 ? juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (row.steps) : juce::String()),
                        header.withTrimmedLeft (2.0f), juce::Justification::centredLeft, true);

            // The steps
            const float cellW = area.getWidth() / (float) cols;

            for (int i = 0; i < cols; ++i)
            {
                auto cell = juce::Rectangle<float> (area.getX() + cellW * (float) i, area.getY(), cellW, area.getHeight()).reduced (1.0f, 0.0f);
                const bool inRow = i < len;
                g.setColour (((i / 4) % 2 == 0 ? Colours::panelLight : Colours::panelLight.interpolatedWith (Colours::panelDark, 0.45f))
                                 .withMultipliedAlpha (inRow ? (row.mute ? 0.5f : 1.0f) : 0.25f));
                g.fillRoundedRectangle (cell, 2.5f);

                if (i == playing)
                {
                    g.setColour (Colours::text.withAlpha (0.9f));
                    g.drawRoundedRectangle (cell.reduced (0.5f), 2.5f, 1.5f);
                }
            }

            for (int i = 0; i < cols; ++i)
                if (row.on[(size_t) i])
                    paintStep (g, row, i, juce::Rectangle<float> (area.getX() + cellW * (float) i, area.getY(), cellW, area.getHeight()).reduced (1.0f, 0.0f),
                               colour.withMultipliedAlpha (i < len && ! row.mute ? 1.0f : 0.3f), i == dragStep && r == dragRow && adjusting);
        }

        if (pattern.numRows < StepsDevice::maxRows)
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (10.5f));
            g.drawText ("Add rows from Rows", juce::Rectangle<float> (0.0f, rowH * (float) pattern.numRows, (float) getWidth(), juce::jmin (rowH, 16.0f)).withTrimmedLeft (18.0f),
                        juce::Justification::centredLeft);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragRow = rowAt (e.position.y);
        dragStep = -1;
        adjusting = painting = moved = false;

        if (dragRow < 0)
            return;

        select (dragRow);

        if (e.mods.isPopupMenu())
        {
            showRowMenu (dragRow);
            dragRow = -1;
            return;
        }

        if (e.position.x < (float) headerW)
        {
            // The dot mutes; anywhere else on the name opens the row's menu
            if (e.position.x < 16.0f)
            {
                dev.beginEdit ("Mute Row");
                dev.setRowMute (dragRow, ! dev.getPattern().rows[(size_t) dragRow].mute);
            }
            else
            {
                showRowMenu (dragRow);
            }

            dragRow = -1;
            return;
        }

        dragStep = stepAt (e.position.x);
        if (dragStep < 0)
            return;

        const auto& row = dev.getPattern().rows[(size_t) dragRow];

        if (! row.on[(size_t) dragStep])
        {
            dev.beginEdit ("Draw Steps");
            dev.setOn (dragRow, dragStep, true);
            painting = true;
        }
        else
        {
            startValue = row.value[(size_t) lane][(size_t) dragStep];
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragRow < 0 || dragStep < 0)
            return;

        if (painting)
        {
            // Carry on along the row
            if (auto i = stepAt (e.position.x); i >= 0)
                dev.setOn (dragRow, i, true);
            return;
        }

        if (! moved && e.getDistanceFromDragStart() < 4)
            return;

        if (! moved)
        {
            moved = adjusting = true;
            dev.beginEdit ("Change " + juce::String (StepsDevice::laneInfo (lane).name));
        }

        // Length and Nudge are set by dragging along the row, the way they lie; the rest up and down
        const auto& info = StepsDevice::laneInfo (lane);
        const bool sideways = lane == StepsDevice::lengthLane || lane == StepsDevice::nudgeLane;
        const float travel = sideways ? (float) e.getDistanceFromDragStartX() : (float) -e.getDistanceFromDragStartY();
        float change = 0.0f;

        if (lane == StepsDevice::lengthLane)        change = travel / cellWidth() * 100.0f;
        else if (lane == StepsDevice::nudgeLane)    change = travel / cellWidth() * 100.0f;
        else                                        change = travel / 110.0f * (float) (info.max - info.min);

        dev.setStep (dragRow, lane, dragStep, startValue + juce::roundToInt (change));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // A click on a step that was on turns it off
        if (dragRow >= 0 && dragStep >= 0 && ! painting && ! moved)
        {
            dev.beginEdit ("Draw Steps");
            dev.setOn (dragRow, dragStep, false);
        }

        dragRow = dragStep = -1;
        adjusting = painting = moved = false;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto r = rowAt (e.position.y); r >= 0 && e.position.x >= 16.0f && e.position.x < (float) headerW)
            renameRow (r);
    }

    void showRowMenu (int row)
    {
        juce::PopupMenu m, notes, steps;
        auto safe = juce::Component::SafePointer<StepRows> (this);
        const auto& r = dev.getPattern().rows[(size_t) row];
        const auto laneName = juce::String (StepsDevice::laneInfo (lane).name);
        auto act = [safe] (const juce::String& what, std::function<void (StepsDevice&)> f)
        {
            return [safe, what, f]
            {
                if (safe == nullptr) return;
                safe->dev.beginEdit (what);
                f (safe->dev);
            };
        };

        for (int octave = 0; octave <= 8; ++octave)
        {
            juce::PopupMenu inOctave;
            for (int n = 0; n < 12; ++n)
            {
                const int note = 12 + octave * 12 + n;
                inOctave.addItem (juce::MidiMessage::getMidiNoteName (note, true, true, 3), true, note == r.note,
                                  act ("Row Note", [row, note] (StepsDevice& d) { d.setRowNote (row, note); }));
            }
            notes.addSubMenu ("Octave " + juce::String (octave - 1), inOctave);
        }

        steps.addItem ("Same as the Pattern", true, r.steps == 0, act ("Row Steps", [row] (StepsDevice& d) { d.setRowSteps (row, 0); }));
        steps.addSeparator();
        for (int i = 1; i <= StepsDevice::numSteps; ++i)
        {
            if (i == 17) steps.addColumnBreak();
            steps.addItem (juce::String (i) + (i == 1 ? " step" : " steps"), true, r.steps == i, act ("Row Steps", [row, i] (StepsDevice& d) { d.setRowSteps (row, i); }));
        }

        m.addItem ("Rename...", [safe, row] { if (safe != nullptr) safe->renameRow (row); });
        m.addSubMenu ("Note", notes);
        m.addSubMenu ("Steps", steps);
        m.addItem ("Mute", true, r.mute, act ("Mute Row", [row, mute = ! r.mute] (StepsDevice& d) { d.setRowMute (row, mute); }));
        m.addSeparator();
        m.addItem ("Randomise Which Steps Play", act ("Randomise Row", [row] (StepsDevice& d) { d.randomiseRowSteps (row); }));
        m.addItem ("Randomise " + laneName, act ("Randomise " + laneName, [row, l = lane] (StepsDevice& d) { d.randomiseRow (row, l); }));
        m.addItem ("Reset " + laneName, act ("Reset " + laneName, [row, l = lane] (StepsDevice& d) { d.resetRow (row, l); }));
        m.addSeparator();
        m.addItem ("Shift Left", act ("Shift Row", [row] (StepsDevice& d) { d.shiftRow (row, -1); }));
        m.addItem ("Shift Right", act ("Shift Row", [row] (StepsDevice& d) { d.shiftRow (row, 1); }));
        m.addItem ("Clear", act ("Clear Row", [row] (StepsDevice& d) { d.clearRow (row); }));
        m.addSeparator();
        m.addItem ("Duplicate", dev.canAddRow(), false, act ("Duplicate Row", [row] (StepsDevice& d) { d.duplicateRow (row); }));
        m.addItem ("Delete", dev.getNumRows() > 1, false, act ("Delete Row", [row] (StepsDevice& d) { d.removeRow (row); }));
        m.showMenuAsync ({});
    }

    void renameRow (int row)
    {
        auto* aw = Theme::newAlertWindow ("Rename Row", "What this row plays:");
        Theme::addTextField (*aw, "name", dev.getRowName (row));
        aw->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<StepRows> (this), aw, row] (int result)
        {
            if (safe != nullptr && result == 1 && aw->getTextEditorContents ("name").trim().isNotEmpty())
            {
                safe->dev.beginEdit ("Rename Row");
                safe->dev.setRowName (row, aw->getTextEditorContents ("name"));
            }
        }), true);
    }

private:
    StepsDevice& dev;
    int lane = StepsDevice::velocityLane, headerW = 150;
    int selectedRow = 0, dragRow = -1, dragStep = -1, startValue = 0;
    bool painting = false, adjusting = false, moved = false;
    long long lastCount = -2;
    int lastChange = -1, lastCols = 0;

    float rowHeight() const
    {
        // Room for a few rows even when there is only one, so a row isn't a slab
        return juce::jmin (54.0f, (float) getHeight() / (float) juce::jmax (4, dev.getNumRows() + (dev.getNumRows() < StepsDevice::maxRows ? 1 : 0)));
    }

    float cellWidth() const     { return (float) (getWidth() - headerW) / (float) columns(); }

    int rowAt (float y) const
    {
        const int r = (int) (y / rowHeight());
        return r >= 0 && r < dev.getNumRows() ? r : -1;
    }

    int stepAt (float x) const
    {
        if (x < (float) headerW) return -1;
        const int i = (int) ((x - (float) headerW) / cellWidth());
        return i >= 0 && i < columns() ? i : -1;
    }

    void select (int row)
    {
        if (row == selectedRow) return;
        selectedRow = row;
        repaint();
        if (onSelectionChanged) onSelectionChanged();
    }

    /** One step that is on, drawn as what it will do. */
    void paintStep (juce::Graphics& g, const StepsDevice::Row& row, int i, juce::Rectangle<float> cell, juce::Colour colour, bool showValue)
    {
        const auto si = (size_t) i;
        const int velocity = row.value[StepsDevice::velocityLane][si], lengthPc = row.value[StepsDevice::lengthLane][si];
        const int chance = row.value[StepsDevice::chanceLane][si], repeats = row.value[StepsDevice::repeatLane][si];
        const int nudge = row.value[StepsDevice::nudgeLane][si], pitch = row.value[StepsDevice::pitchLane][si];
        const int every = row.value[StepsDevice::everyLane][si];

        // Nudged steps sit off the grid; unlikely ones are faint; "every so often" ones are hollow
        auto block = cell.translated (cell.getWidth() * (float) nudge / 100.0f, 0.0f).reduced (0.5f, 1.5f);
        colour = colour.withMultipliedAlpha (0.3f + 0.7f * (float) chance / 100.0f);
        juce::String text;

        // A long note reaches over the steps it sounds through
        if (lengthPc > 100 || lane == StepsDevice::lengthLane)
        {
            auto reach = block.withWidth (juce::jmax (3.0f, (cell.getWidth() + 2.0f) * (float) lengthPc / 100.0f - 1.0f));
            g.setColour (colour.withMultipliedAlpha (lane == StepsDevice::lengthLane ? 0.9f : 0.35f));
            g.fillRoundedRectangle (lane == StepsDevice::lengthLane ? reach.reduced (0.0f, block.getHeight() * 0.22f) : reach.removeFromBottom (3.0f), 2.0f);
        }

        if (lane != StepsDevice::lengthLane)
        {
            // Repeats cut the step into pieces
            const int pieces = juce::jlimit (1, 8, repeats);
            const float pieceW = block.getWidth() / (float) pieces;

            for (int p = 0; p < pieces; ++p)
            {
                auto piece = juce::Rectangle<float> (block.getX() + pieceW * (float) p, block.getY(), pieceW, block.getHeight()).reduced (pieces > 1 ? 0.6f : 0.0f, 0.0f);

                // How hard it is hit is how full it is
                const float full = lane == StepsDevice::velocityLane ? (float) velocity / 127.0f : 0.45f + 0.55f * (float) velocity / 127.0f;
                g.setColour (colour.withMultipliedAlpha (0.28f));
                g.fillRoundedRectangle (piece, 2.0f);
                g.setColour (colour);

                if (every > 0)
                    g.drawRoundedRectangle (piece.reduced (0.75f), 2.0f, 1.5f);
                else
                    g.fillRoundedRectangle (piece.withTop (piece.getBottom() - juce::jmax (2.0f, piece.getHeight() * full)), 2.0f);
            }

            // Pitch: a line across at its height
            if (lane == StepsDevice::pitchLane)
            {
                const float y = block.getCentreY() - (float) pitch / 24.0f * block.getHeight() * 0.5f;
                g.setColour (Colours::text);
                g.fillRect (block.getX(), juce::jlimit (block.getY(), block.getBottom() - 2.0f, y - 1.0f), block.getWidth(), 2.0f);
            }
        }

        switch (lane)
        {
            case StepsDevice::velocityLane: if (showValue) text = juce::String (velocity); break;
            case StepsDevice::lengthLane:   if (showValue || lengthPc != 100) text = juce::String (lengthPc); break;
            case StepsDevice::chanceLane:   if (showValue || chance < 100) text = juce::String (chance); break;
            case StepsDevice::repeatLane:   if (showValue || repeats > 1) text = juce::String (repeats) + "x"; break;
            case StepsDevice::nudgeLane:    if (showValue || nudge != 0) text = (nudge > 0 ? "+" : "") + juce::String (nudge); break;
            case StepsDevice::pitchLane:    if (showValue || pitch != 0) text = (pitch > 0 ? "+" : "") + juce::String (pitch); break;
            case StepsDevice::everyLane:    text = StepsDevice::everyShort (every); break;
            default: break;
        }

        if (text.isNotEmpty() && block.getHeight() >= 11.0f)
        {
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (juce::jlimit (8.0f, 10.5f, block.getHeight() * 0.55f), juce::Font::bold));
            g.drawText (text, block.expanded (6.0f, 0.0f), juce::Justification::centred);
        }
    }

    void timerCallback() override
    {
        const auto c = dev.getCurrentCount();
        const int change = dev.getChangeCount(), cols = columns();

        if (c != lastCount || change != lastChange || cols != lastCols)
        {
            lastCount = c; lastChange = change; lastCols = cols;
            repaint();
        }
    }
};

/** Chooses which kind of value the rows are showing. */
class StepTabs : public juce::Component
{
public:
    std::function<void (int)> onChange;
    int current = 0;

    StepTabs()      { setTitle ("Show"); }

    void paint (juce::Graphics& g) override
    {
        const auto names = StepsDevice::laneNames();
        const float w = (float) getWidth() / (float) names.size();

        for (int i = 0; i < names.size(); ++i)
        {
            auto cell = juce::Rectangle<float> (w * (float) i, 0.0f, w, (float) getHeight()).reduced (1.0f);
            const bool on = i == current;
            g.setColour (on ? Colours::accent : Colours::panelDark);
            g.fillRoundedRectangle (cell, 3.0f);
            g.setColour (on ? Colours::textInverse : Colours::textDim);
            g.setFont (juce::FontOptions (11.0f, on ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (names[i], cell.toNearestInt().reduced (2), juce::Justification::centred, 1);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        current = juce::jlimit (0, StepsDevice::numLanes - 1, e.x * StepsDevice::numLanes / juce::jmax (1, getWidth()));
        repaint();
        if (onChange) onChange (current);
    }
};

/** One row's values for the tab that is showing, as bars to drag: the close-up under the rows
    in the sequencer's window. */
class StepBars : public juce::Component, private juce::Timer
{
public:
    StepBars (StepsDevice& d, StepRows& r) : dev (d), rows (r)
    {
        setTitle ("Values");
        startTimerHz (20);
    }

    void setHeaderWidth (int w)     { headerW = w; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const int row = rows.getSelectedRow(), lane = rows.getLane();
        if (row >= dev.getNumRows()) return;

        const auto& r = dev.getPattern().rows[(size_t) row];
        const auto& info = StepsDevice::laneInfo (lane);
        const auto colour = StepRows::rowColour (row);
        const int cols = rows.columns(), len = dev.stepsInRow (row);
        const auto count = dev.getCurrentCount();
        const int playing = count >= 0 ? dev.indexFor (len, count, row) : -1;

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (dev.getRowName (row), juce::Rectangle<int> (4, 0, headerW - 10, 20), juce::Justification::centredLeft, true);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (info.name, juce::Rectangle<int> (4, 20, headerW - 10, 16), juce::Justification::centredLeft);

        for (int i = 0; i < cols; ++i)
        {
            auto box = barBox (i);
            g.setColour (i == playing ? Colours::display.interpolatedWith (Colours::text, 0.14f) : Colours::display);
            g.fillRect (box);

            const float v = (float) r.value[(size_t) lane][(size_t) i];
            const float n = (v - (float) info.min) / (float) (info.max - info.min);
            g.setColour (colour.withAlpha ((r.on[(size_t) i] ? 0.9f : 0.3f) * (i < len ? 1.0f : 0.3f)));

            if (info.min < 0)
            {
                const float mid = box.getCentreY(), y = box.getBottom() - n * box.getHeight();
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (box.getX(), juce::jmin (mid, y), box.getRight(), juce::jmax (mid, y) + 1.0f));
            }
            else
            {
                g.fillRect (box.withTop (box.getBottom() - juce::jmax (1.0f, n * box.getHeight())));
            }

            auto text = lane == StepsDevice::everyLane ? StepsDevice::everyShort ((int) v)
                      : (int) v == info.normal ? juce::String()
                      : (info.min < 0 && v > 0 ? "+" : "") + juce::String ((int) v);

            if (text.isNotEmpty())
            {
                g.setColour (Colours::text.withAlpha (i < len ? 1.0f : 0.3f));
                g.setFont (juce::FontOptions (cols > 16 ? 8.5f : 9.5f));
                g.drawText (text, box.toNearestInt().removeFromBottom (12), juce::Justification::centred);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dev.beginEdit ("Change " + juce::String (StepsDevice::laneInfo (rows.getLane()).name));
        mouseDrag (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int cols = rows.columns();
        const int i = (int) ((e.position.x - (float) headerW) / ((float) (getWidth() - headerW) / (float) cols));
        if (e.position.x < (float) headerW || i < 0 || i >= cols) return;

        const auto& info = StepsDevice::laneInfo (rows.getLane());
        auto box = barBox (i);
        const float n = juce::jlimit (0.0f, 1.0f, (box.getBottom() - e.position.y) / box.getHeight());
        dev.setStep (rows.getSelectedRow(), rows.getLane(), i, info.min + juce::roundToInt (n * (float) (info.max - info.min)));
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int cols = rows.columns();
        const int i = (int) ((e.position.x - (float) headerW) / ((float) (getWidth() - headerW) / (float) cols));
        if (e.position.x >= (float) headerW && i >= 0 && i < cols)
            dev.setStep (rows.getSelectedRow(), rows.getLane(), i, StepsDevice::laneInfo (rows.getLane()).normal);
    }

private:
    StepsDevice& dev;
    StepRows& rows;
    int headerW = 150, lastChange = -1, lastLane = -1, lastRow = -1;
    long long lastCount = -2;

    juce::Rectangle<float> barBox (int i) const
    {
        const float w = (float) (getWidth() - headerW) / (float) rows.columns();
        return juce::Rectangle<float> ((float) headerW + w * (float) i, 0.0f, w, (float) getHeight()).reduced (1.5f, 0.0f);
    }

    void timerCallback() override
    {
        const auto c = dev.getCurrentCount();
        const int change = dev.getChangeCount(), l = rows.getLane(), r = rows.getSelectedRow();

        if (c != lastCount || change != lastChange || l != lastLane || r != lastRow)
        {
            lastCount = c; lastChange = change; lastLane = l; lastRow = r;
            repaint();
        }
    }
};

//==============================================================================
/** The sequencer's panel: in the chain (compact), or in its own window (more room for the rows,
    and one row's values as bars underneath). */
class StepsView : public juce::Component, private juce::Timer
{
public:
    StepsView (StepsDevice& d, bool inWindow = false)
        : dev (d), window (inWindow), rows (d), bars (d, rows),
          rateChoice (d.rateP.param(), MidiEffectDevice::rateNames(), Colours::solo),
          directionChoice (d.direction.param(), StepsDevice::directionNames()),
          length (d.length.param()), swing (d.swing.param()), gate (d.gate.param()), root (d.root.param()),
          follow (d.follow.param(), "Follow Keys")
    {
        for (juce::Component* c : { (juce::Component*) &rows, (juce::Component*) &tabs, (juce::Component*) &rateChoice, (juce::Component*) &directionChoice,
                                    (juce::Component*) &length, (juce::Component*) &swing, (juce::Component*) &gate, (juce::Component*) &root,
                                    (juce::Component*) &follow, (juce::Component*) &scaleButton, (juce::Component*) &rowsButton })
            addAndMakeVisible (c);

        if (window)
            addAndMakeVisible (bars);

        rateChoice.setTitle ("Rate");
        directionChoice.setTitle ("Direction");
        follow.setTitle ("Follow Keys");
        scaleButton.setTooltip ("Keeps every pitch to the notes of a scale, counted from the Root");
        rowsButton.setTooltip ("Add a row, start a drum pattern, shift or double the whole pattern, or start over");

        rows.setHeaderWidth (window ? 190 : 150);
        bars.setHeaderWidth (190);
        tabs.onChange = [this] (int l) { rows.setLane (l); };
        scaleButton.onClick = [this] { showScaleMenu(); };
        rowsButton.onClick = [this] { showRowsMenu(); };
        timerCallback();
        startTimerHz (5);
    }

    void resized() override
    {
        auto r = getLocalBounds();

        if (window)
        {
            // Across the top: how it plays
            auto top = r.removeFromTop (86);
            auto choices = top.removeFromLeft (300);
            rateChoice.setBounds (choices.removeFromTop (24));
            choices.removeFromTop (6);
            directionChoice.setBounds (choices.removeFromTop (24));
            top.removeFromLeft (16);
            for (auto* k : { &length, &root, &swing, &gate })
                k->setBounds (top.removeFromLeft (76));
            top.removeFromLeft (16);
            auto buttons = top.removeFromLeft (200);
            scaleButton.setBounds (buttons.removeFromTop (24));
            buttons.removeFromTop (6);
            follow.setBounds (buttons.removeFromTop (24));
            r.removeFromTop (6);

            auto tabRow = r.removeFromTop (24);
            rowsButton.setBounds (tabRow.removeFromLeft (184));
            tabRow.removeFromLeft (6);
            tabs.setBounds (tabRow);
            r.removeFromTop (6);

            bars.setBounds (r.removeFromBottom (juce::jmax (90, r.getHeight() / 4)));
            r.removeFromBottom (8);
            rows.setBounds (r);
            return;
        }

        auto side = r.removeFromLeft (216);
        rateChoice.setBounds (side.removeFromTop (22));
        side.removeFromTop (3);
        directionChoice.setBounds (side.removeFromTop (20));
        side.removeFromTop (3);
        auto bottom = side.removeFromBottom (22);
        follow.setBounds (bottom.removeFromRight (84));
        bottom.removeFromRight (4);
        scaleButton.setBounds (bottom);
        side.removeFromBottom (3);
        const int w = side.getWidth() / 4;
        for (auto* k : { &length, &root, &swing, &gate })
            k->setBounds (side.removeFromLeft (w));

        r.removeFromLeft (8);
        auto top = r.removeFromTop (20);
        rowsButton.setBounds (top.removeFromLeft (144));
        top.removeFromLeft (6);
        tabs.setBounds (top);
        r.removeFromTop (4);
        rows.setBounds (r);
    }

private:
    StepsDevice& dev;
    const bool window;
    StepRows rows;
    StepBars bars;
    StepTabs tabs;
    DeviceUI::Choice rateChoice, directionChoice;
    DeviceUI::Knob length, swing, gate, root;
    DeviceUI::Toggle follow;
    juce::TextButton scaleButton, rowsButton { "Rows" };

    void showScaleMenu()
    {
        juce::PopupMenu m;
        const int current = juce::roundToInt (dev.scale.get());
        for (int i = 0; i < (int) StepsDevice::scales().size(); ++i)
            m.addItem (i + 1, StepsDevice::scales()[(size_t) i].name, true, i == current);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&scaleButton), [safe = juce::Component::SafePointer<StepsView> (this)] (int result)
        {
            if (safe == nullptr || result <= 0) return;
            auto& p = safe->dev.scale.param();
            p.parameterChangeGestureBegin();
            p.setParameter ((float) (result - 1), juce::sendNotification);
            p.parameterChangeGestureEnd();
        });
    }

    void showRowsMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Add a Row", dev.canAddRow());
        m.addItem (2, "Start a Drum Pattern");
        m.addSeparator();
        m.addItem (3, "Shift Pattern Left");
        m.addItem (4, "Shift Pattern Right");
        m.addItem (5, "Double Pattern", dev.canDouble());
        m.addSeparator();
        m.addItem (6, "Start Over");
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&rowsButton), [safe = juce::Component::SafePointer<StepsView> (this)] (int result)
        {
            if (safe == nullptr) return;
            auto& d = safe->dev;

            switch (result)
            {
                case 1:
                {
                    // The next note up from the last row, so a new row is something else to play
                    d.beginEdit ("Add Row");
                    const int last = d.getNumRows() - 1;
                    const int row = d.addRow ("Row " + juce::String (d.getNumRows() + 1), juce::jmin (127, d.getPattern().rows[(size_t) last].note + 1));
                    if (row >= 0) safe->rows.renameRow (row);
                    break;
                }
                case 2: d.beginEdit ("Start a Drum Pattern"); d.loadDrumRows(); break;
                case 3: d.beginEdit ("Shift Pattern"); d.shiftPattern (-1); break;
                case 4: d.beginEdit ("Shift Pattern"); d.shiftPattern (1); break;
                case 5: d.beginEdit ("Double Pattern"); d.doubleLength(); break;
                case 6: d.beginEdit ("Start Over"); d.loadDefaults(); break;
                default: break;
            }
        });
    }

    void timerCallback() override
    {
        scaleButton.setButtonText ("Scale: " + dev.scale.param().getCurrentValueAsString());
        tabs.current = rows.getLane();
    }
};

inline std::unique_ptr<juce::Component> StepsDevice::createDeviceView()    { return std::make_unique<StepsView> (*this); }
inline std::unique_ptr<juce::Component> StepsDevice::createWindowView()    { return std::make_unique<StepsView> (*this, true); }

} // namespace elsewhere
