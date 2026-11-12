// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MidiLanesTests.h"
#include "MidiLanes.h"
#include "Project.h"

namespace elsewhere::MidiLanesTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
    };

    juce::String velocities (te::MidiList& seq)
    {
        juce::StringArray v;
        for (auto n : seq.getNotes())
            v.add (juce::String (n->getVelocity()));
        return v.joinIntoString (" ");
    }

    /** "beat:value" for each event of a type, values as the lane shows them. */
    juce::String events (te::MidiList& seq, int type)
    {
        juce::StringArray v;
        for (auto e : MidiLanes::getEvents (seq, type))
            v.add (juce::String (e->getBeatPosition().inBeats(), 3).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + ":"
                   + MidiLanes::valueText (type, e->getControllerValue()));
        return v.joinIntoString (" ");
    }

    /** Sends a press, drags and a release to a component, as the mouse would. */
    void gesture (juce::Component& c, std::initializer_list<juce::Point<float>> points, bool rightButton = false)
    {
        auto source = juce::Desktop::getInstance().getMainMouseSource();
        auto mods = rightButton ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier;
        auto first = *points.begin();
        auto now = juce::Time::getCurrentTime();
        auto event = [&] (juce::Point<float> p, juce::ModifierKeys m)
        {
            return juce::MouseEvent (source, p, m, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, first, now, 1, false);
        };

        c.mouseDown (event (first, mods));
        for (auto p : points)
            c.mouseDrag (event (p, mods));
        c.mouseUp (event (*std::prev (points.end()), {}));
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    Project p (engine, Project::createProjectFolder (outDir, "Lanes"));
    auto& edit = p.getEdit();
    auto& um = edit.getUndoManager();
    auto& track = *te::getAudioTracks (edit).getFirst();
    auto clip = dynamic_cast<te::MidiClip*> (track.insertNewClip (te::TrackItem::Type::midi, "Lanes",
                                                                  { 0_tp, te::TimePosition::fromSeconds (8.0) }, nullptr));
    log.check (clip != nullptr, "a MIDI clip to edit");

    if (clip == nullptr)
        return 1;

    auto& seq = clip->getSequence();

    for (int i = 0; i < 8; ++i)
        seq.addNote (60 + i, te::BeatPosition::fromBeats (i * 0.5), te::BeatDuration::fromBeats (0.5), 100, 0, nullptr);

    // Velocity
    um.beginNewTransaction();
    MidiLanes::paintVelocities (seq, {}, 0.0, 20, 3.5, 90, &um);
    log.check (velocities (seq) == "20 30 40 50 60 70 80 90", "painting a line sets each note it passes (" + velocities (seq) + ")");

    um.beginNewTransaction();
    MidiLanes::paintVelocities (seq, { seq.getNotes()[1], seq.getNotes()[2] }, 0.0, 127, 3.5, 127, &um);
    log.check (velocities (seq) == "20 127 127 50 60 70 80 90", "with notes selected, only they change (" + velocities (seq) + ")");

    um.undo();
    log.check (velocities (seq) == "20 30 40 50 60 70 80 90", "and that undoes in one step");

    MidiLanes::setVelocity (*seq.getNotes()[0], 0, nullptr);
    MidiLanes::setVelocity (*seq.getNotes()[1], 300, nullptr);
    log.check (seq.getNotes()[0]->getVelocity() == 1 && seq.getNotes()[1]->getVelocity() == 127, "velocity stays within 1-127");

    // Controllers
    um.beginNewTransaction();
    MidiLanes::drawControllers (seq, 1, 0.0, 0, 1.0, 127 << 7, 0.25, &um);
    log.check (events (seq, 1) == "0:0 0.25:31 0.5:63 0.75:95 1:127", "a drawn mod wheel ramp, a point a step (" + events (seq, 1) + ")");

    um.beginNewTransaction();
    MidiLanes::drawControllers (seq, 1, 0.5, 64 << 7, 2.0, 64 << 7, 0.25, &um);
    log.check (events (seq, 1) == "0:0 0.25:31 0.5:64", "drawing over it replaces what was there, and a held value is one point (" + events (seq, 1) + ")");

    um.undo();
    log.check (events (seq, 1) == "0:0 0.25:31 0.5:63 0.75:95 1:127", "undo brings the ramp back");

    MidiLanes::drawControllers (seq, MidiLanes::pitchType, 0.0, 8192, 0.0, 8192, 0.25, nullptr);
    MidiLanes::drawControllers (seq, MidiLanes::pitchType, 1.0, 16383, 1.0, 16383, 0.25, nullptr);
    log.check (events (seq, MidiLanes::pitchType) == "0:+0 1:+8191", "pitch bend keeps its 14 bits, centred on 0 (" + events (seq, MidiLanes::pitchType) + ")");
    log.check (events (seq, 1).startsWith ("0:0 0.25:31"), "and leaves the mod wheel alone");

    log.check (MidiLanes::quantiseValue (11, (100 << 7) + 90) == (100 << 7), "a CC is stored on its 7-bit steps");
    log.check (MidiLanes::valueText (64, 64 << 7) == "On" && MidiLanes::valueText (64, 10 << 7) == "Off", "sustain reads on or off");
    log.check (MidiLanes::hasData (seq, 1) && ! MidiLanes::hasData (seq, 11), "the lanes with something in them are known");
    log.check (MidiLanes::laneName (74).startsWith ("CC 74"), "other controllers are named by number (" + MidiLanes::laneName (74) + ")");

    // The lane itself, 100 px a beat with the values from y = 10 (top) to 80 (bottom)
    juce::Array<te::MidiNote*> selection;
    MidiLanes::Host host;
    host.keyboardW = 56;
    host.beatToX = [] (double b) { return 56.0f + (float) b * 100.0f; };
    host.xToBeat = [] (float x) { return (x - 56.0) / 100.0; };
    host.snap = [] (double b) { return std::round (b * 4.0) / 4.0; };
    host.getGrid = [] { return 0.25; };
    host.getSelection = [&selection] { return selection; };

    MidiLanes::Lane lane (*clip, host);
    lane.setSize (900, MidiLanes::Lane::openHeight);
    lane.setType (MidiLanes::velocityType);
    log.check (lane.isOpen(), "the lane starts open");

    // Note 3 (beat 1.5) is at x = 206: drag its stem to the top
    gesture (lane, { { 206.0f, 60.0f }, { 206.0f, 30.0f }, { 206.0f, 0.0f } });
    log.check (seq.getNotes()[3]->getVelocity() == 127 && seq.getNotes()[2]->getVelocity() == 40, "dragging a stem sets that note");

    // With two selected, dragging one moves both by the same amount
    selection = { seq.getNotes()[4], seq.getNotes()[5] };
    auto before4 = seq.getNotes()[4]->getVelocity(), before5 = seq.getNotes()[5]->getVelocity();
    gesture (lane, { { 256.0f, 50.0f }, { 256.0f, 40.0f } });
    auto d4 = seq.getNotes()[4]->getVelocity() - before4, d5 = seq.getNotes()[5]->getVelocity() - before5;
    log.check (d4 > 0 && d4 == d5, "selected stems move together, keeping their shape (+" + juce::String (d4) + ", +" + juce::String (d5) + ")");
    um.undo();
    log.check (seq.getNotes()[4]->getVelocity() == before4, "one drag is one undo");
    selection.clear();

    // Sweep the bottom from between two stems across the first half: those notes go quiet
    gesture (lane, { { 80.0f, 80.0f }, { 50.0f, 80.0f }, { 240.0f, 80.0f } });
    log.check (velocities (seq).startsWith ("1 1 1 1"), "sweeping paints the notes passed (" + velocities (seq) + ")");

    // Expression: click to place a point, drag it, double-click it away
    lane.setType (11);
    gesture (lane, { { 156.0f, 45.0f } });
    auto exp = MidiLanes::getEvents (seq, 11);
    log.check (exp.size() == 1 && exp[0]->getBeatPosition().inBeats() == 1.0, "a click adds a point on the grid (" + events (seq, 11) + ")");

    if (exp.size() == 1)
    {
        auto y = 80.0f - 70.0f * (float) exp[0]->getControllerValue() / 16383.0f;
        gesture (lane, { { 156.0f, y }, { 182.0f, 10.0f } });
        log.check (events (seq, 11) == "1.25:127", "dragging a point moves it, snapped (" + events (seq, 11) + ")");
    }

    gesture (lane, { { 256.0f, 80.0f }, { 356.0f, 10.0f } });
    log.check (MidiLanes::getEvents (seq, 11).size() > 3, "dragging across empty space draws (" + juce::String (MidiLanes::getEvents (seq, 11).size()) + " points)");

    std::cout << "MIDI lanes tests\n" << log.text << (log.failures == 0 ? "All passed\n" : juce::String (log.failures) + " failed\n");
    return log.failures;
}
}
