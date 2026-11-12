// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DrumTrack.h"
#include "../Theme.h"

namespace elsewhere
{
//==============================================================================
/** Simple <-> Complex (x) and Soft <-> Loud (y) for a drum section. */
class DrumXYPad : public juce::Component
{
public:
    std::function<void()> onGestureStart;
    std::function<void (double complexity, double loudness)> onChange;
    double x = 0.4, y = 0.6;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 5.0f);

        g.setColour (Colours::gridLine);
        for (int i = 1; i < 4; ++i)
        {
            g.drawVerticalLine (juce::roundToInt (r.getWidth() * (float) i / 4.0f), r.getY(), r.getBottom());
            g.drawHorizontalLine (juce::roundToInt (r.getHeight() * (float) i / 4.0f), r.getX(), r.getRight());
        }

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        auto t = getLocalBounds().reduced (6, 4);
        g.drawText ("Loud", t, juce::Justification::centredTop);
        g.drawText ("Soft", t, juce::Justification::centredBottom);
        g.drawText ("Simple", t, juce::Justification::centredLeft);
        g.drawText ("Complex", t, juce::Justification::centredRight);

        auto pos = juce::Point<float> ((float) (x * r.getWidth()), (float) ((1.0 - y) * r.getHeight()));
        g.setColour (Colours::accent.withAlpha (0.25f));
        g.fillEllipse (juce::Rectangle<float> (30, 30).withCentre (pos));
        g.setColour (Colours::accent);
        g.fillEllipse (juce::Rectangle<float> (14, 14).withCentre (pos));
    }

    void mouseDown (const juce::MouseEvent& e) override   { if (onGestureStart) onGestureStart(); set (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override   { set (e.position); }

private:
    void set (juce::Point<float> p)
    {
        x = juce::jlimit (0.0, 1.0, (double) p.x / juce::jmax (1, getWidth()));
        y = juce::jlimit (0.0, 1.0, 1.0 - (double) p.y / juce::jmax (1, getHeight()));
        repaint();
        if (onChange) onChange (x, y);
    }
};

//==============================================================================
/** The section's pattern, one row per drum piece, like a drum machine grid. */
class DrumPatternPreview : public juce::Component
{
public:
    void setClip (te::MidiClip* c)     { clip = c; repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 4.0f);

        if (clip == nullptr || clip->getTrack() == nullptr)
            return;

        const auto map = DrumTrack::noteMapFor (*clip->getTrack());
        const double length = (clip->getEndBeat() - clip->getStartBeat()).inBeats();
        const int beatsPerBar = juce::jmax (1, clip->edit.tempoSequence.getTimeSig (0)->numerator.get());

        // Which pieces appear, in kit order
        std::vector<drums::Piece> rows;
        std::array<bool, (size_t) drums::Piece::count> used {};
        for (auto n : clip->getSequence().getNotes())
            if (auto p = DrumTrack::pieceForNote (map, n->getNoteNumber()))
                used[(size_t) *p] = true;
        for (int i = (int) drums::Piece::count; --i >= 0;)
            if (used[(size_t) i]) rows.push_back ((drums::Piece) i);

        if (rows.empty() || length <= 0.0)
            return;

        // Short panel: group into the three parts instead of one row per drum
        const bool grouped = (r.getHeight() - 8.0f) / (float) rows.size() < 10.0f;
        auto rowOf = [&] (drums::Piece p) -> int
        {
            if (grouped) return 2 - (int) drums::partOf (p);
            auto it = std::find (rows.begin(), rows.end(), p);
            return it == rows.end() ? -1 : (int) (it - rows.begin());
        };
        const int numRows = grouped ? 3 : (int) rows.size();

        const float labelW = grouped ? 100.0f : 70.0f;
        auto grid = r.reduced (4.0f).withTrimmedLeft (labelW);
        const float rowH = grid.getHeight() / (float) numRows;
        auto xFor = [&] (double beat) { return grid.getX() + (float) (beat / length) * grid.getWidth(); };

        // Bars and beats
        for (double b = 0; b < length; b += 1.0)
        {
            const bool bar = std::fmod (b, beatsPerBar) < 1.0e-6;
            g.setColour (bar ? Colours::gridBar : Colours::gridLine.withAlpha (0.6f));
            g.drawVerticalLine (juce::roundToInt (xFor (b)), grid.getY(), grid.getBottom());
        }

        g.setFont (juce::FontOptions (9.5f));
        static const char* partNames[] = { "Toms & Perc", "Hats & Cymbals", "Kick & Snare" };
        for (int i = 0; i < numRows; ++i)
        {
            auto row = juce::Rectangle<float> (r.getX() + 4.0f, grid.getY() + rowH * (float) i, labelW - 6.0f, rowH);
            g.setColour (Colours::textDim);
            g.drawText (grouped ? juce::String (partNames[i]) : juce::String (drums::pieceName (rows[(size_t) i])), row.toNearestInt(), juce::Justification::centredLeft);
        }

        const float dot = juce::jlimit (3.0f, 8.0f, rowH * 0.7f);
        for (auto n : clip->getSequence().getNotes())
        {
            auto p = DrumTrack::pieceForNote (map, n->getNoteNumber());
            if (! p) continue;
            const int row = rowOf (*p);
            if (row < 0) continue;

            const float y = grid.getY() + rowH * ((float) row + 0.5f);
            const float v = (float) n->getVelocity() / 127.0f;
            g.setColour (Colours::accent.withAlpha (0.25f + 0.75f * v));
            g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre ({ xFor (n->getStartBeat().inBeats()), y }));
        }
    }

private:
    te::MidiClip::Ptr clip;
};

//==============================================================================
/** Settings for one drum section (shown in the Clip panel when a section is selected). */
class DrumSectionEditor : public juce::Component,
                          private juce::ValueTree::Listener
{
public:
    DrumSectionEditor (te::MidiClip& c) : clip (&c)
    {
        label.setJustification (juce::Justification::centredLeft);
        label.onReturnKey = label.onFocusLost = [this] { setProp (DrumTrack::Props::label, label.getText().trim(), "Rename section"); };
        addAndMakeVisible (label);

        for (auto name : { "Intro", "Verse", "Chorus", "Bridge", "Outro", "Break" })
        {
            auto* b = labelButtons.add (new juce::TextButton (name));
            b->onClick = [this, name] { setProp (DrumTrack::Props::label, juce::String (name), "Rename section"); };
            addAndMakeVisible (b);
        }

        for (int i = 0; i < (int) drums::Style::count; ++i)
        {
            auto* b = styleButtons.add (new juce::TextButton (drums::styleName ((drums::Style) i)));
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonOnColourId, Colours::accent.withAlpha (0.55f));
            b->onClick = [this, i] { setProp (DrumTrack::Props::style, i, "Drum style"); };
            addAndMakeVisible (b);
        }

        xy.onGestureStart = [this] { clip->edit.getUndoManager().beginNewTransaction ("Drum feel"); };
        xy.onChange = [this] (double cx, double ly)
        {
            clip->state.setProperty (DrumTrack::Props::complexity, cx, &clip->edit.getUndoManager());
            clip->state.setProperty (DrumTrack::Props::loudness, ly, &clip->edit.getUndoManager());
        };
        addAndMakeVisible (xy);

        const juce::Identifier partProps[] = { DrumTrack::Props::part0, DrumTrack::Props::part1, DrumTrack::Props::part2 };
        const juce::Identifier varProps[]  = { DrumTrack::Props::var0, DrumTrack::Props::var1, DrumTrack::Props::var2 };
        const char* partNames[] = { "Kick & Snare", "Hats & Cymbals", "Toms & Percussion" };

        for (int i = 0; i < 3; ++i)
        {
            auto* t = partToggles.add (new juce::ToggleButton (partNames[i]));
            t->onClick = [this, t, prop = partProps[i]] { setProp (prop, t->getToggleState(), "Drum parts"); };
            addAndMakeVisible (t);

            auto* s = variationSliders.add (makeSlider ("Variation"));
            s->onDragStart = [this] { clip->edit.getUndoManager().beginNewTransaction ("Drum variation"); };
            s->onValueChange = [this, s, prop = varProps[i]] { clip->state.setProperty (prop, s->getValue() / 100.0, &clip->edit.getUndoManager()); };
            addAndMakeVisible (s);
        }

        fills.reset (makeSlider ("Fills"));
        fills->onDragStart = [this] { clip->edit.getUndoManager().beginNewTransaction ("Drum fills"); };
        fills->onValueChange = [this] { clip->state.setProperty (DrumTrack::Props::fills, fills->getValue() / 100.0, &clip->edit.getUndoManager()); };
        addAndMakeVisible (*fills);

        swing.reset (makeSlider ("Swing"));
        swing->setTooltip ("Double-click for the style's own feel");
        swing->setDoubleClickReturnValue (true, -1.0);
        swing->setRange (-1.0, 100.0, 1.0);
        swing->textFromValueFunction = [] (double v) { return v < 0.0 ? juce::String ("Swing: style") : "Swing: " + juce::String (juce::roundToInt (v)) + " %"; };
        swing->onDragStart = [this] { clip->edit.getUndoManager().beginNewTransaction ("Drum swing"); };
        swing->onValueChange = [this] { clip->state.setProperty (DrumTrack::Props::swing, swing->getValue() < 0.0 ? -1.0 : swing->getValue() / 100.0, &clip->edit.getUndoManager()); };
        addAndMakeVisible (*swing);

        fillAtEnd.setButtonText ("Fill at the end");
        fillAtEnd.onClick = [this] { setProp (DrumTrack::Props::fillAtEnd, fillAtEnd.getToggleState(), "Drum fill"); };
        addAndMakeVisible (fillAtEnd);

        crash.setButtonText ("Crash at the start");
        crash.onClick = [this] { setProp (DrumTrack::Props::crash, crash.getToggleState(), "Drum crash"); };
        addAndMakeVisible (crash);

        newVariation.setTooltip ("Same settings, a new take on the pattern");
        newVariation.onClick = [this] { setProp (DrumTrack::Props::seed, juce::Random::getSystemRandom().nextInt (1000000) + 1, "New variation"); };
        addAndMakeVisible (newVariation);

        kitNotes.setTooltip ("Which notes the drums are played on: depends on the drum instrument");
        kitNotes.onClick = [this] { showKitNotesMenu(); };
        addAndMakeVisible (kitNotes);

        convert.setTooltip ("Turn this section into an ordinary MIDI clip to edit the notes by hand");
        convert.onClick = [this]
        {
            clip->edit.getUndoManager().beginNewTransaction ("Convert to MIDI");
            DrumTrack::convertToMidi (*clip);
            if (onConverted) onConverted();
        };
        addAndMakeVisible (convert);

        preview.setClip (clip.get());
        addAndMakeVisible (preview);

        clip->state.addListener (this);
        refresh();
    }

    ~DrumSectionEditor() override   { clip->state.removeListener (this); }

    /** Called after Convert to MIDI (the Clip panel switches to the piano roll). */
    std::function<void()> onConverted;

    te::MidiClip* getClip() const   { return clip.get(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panelDark);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("SECTION", sectionTitle, juce::Justification::centredLeft);
        g.drawText ("STYLE", styleTitle, juce::Justification::centredLeft);
        g.drawText ("FEEL", feelTitle, juce::Justification::centredLeft);
        g.drawText ("PARTS", partsTitle, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (juce::jmax (150, r.getHeight() - 70));
        r.removeFromTop (6);
        preview.setBounds (r);

        // Section name
        auto col = top.removeFromLeft (170);
        sectionTitle = col.removeFromTop (14);
        label.setBounds (col.removeFromTop (24));
        col.removeFromTop (4);
        auto lb = col.removeFromTop (46);
        for (int i = 0; i < labelButtons.size(); ++i)
            labelButtons[i]->setBounds (lb.getX() + (i % 3) * (lb.getWidth() / 3), lb.getY() + (i / 3) * 23, lb.getWidth() / 3 - 2, 21);
        col.removeFromTop (6);
        auto buttons = col.removeFromTop (24);
        newVariation.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        buttons.removeFromLeft (4);
        kitNotes.setBounds (buttons);
        col.removeFromTop (4);
        convert.setBounds (col.removeFromTop (24));

        top.removeFromLeft (12);
        col = top.removeFromLeft (200);
        styleTitle = col.removeFromTop (14);
        auto rowH = juce::jmin (26, col.getHeight() / 4);
        for (int i = 0; i < styleButtons.size(); ++i)
            styleButtons[i]->setBounds (col.getX() + (i % 2) * (col.getWidth() / 2), col.getY() + (i / 2) * rowH, col.getWidth() / 2 - 2, rowH - 2);

        top.removeFromLeft (12);
        col = top.removeFromLeft (juce::jmin (top.getHeight(), 170));
        feelTitle = col.removeFromTop (14);
        xy.setBounds (col.withHeight (juce::jmin (col.getHeight(), col.getWidth())));

        top.removeFromLeft (12);
        col = top.removeFromLeft (juce::jmin (330, top.getWidth()));
        partsTitle = col.removeFromTop (14);
        for (int i = 0; i < 3; ++i)
        {
            auto row = col.removeFromTop (24);
            partToggles[i]->setBounds (row.removeFromLeft (150));
            variationSliders[i]->setBounds (row.reduced (0, 2));
        }
        col.removeFromTop (6);
        auto row = col.removeFromTop (24);
        fills->setBounds (row.removeFromLeft (row.getWidth() / 2).reduced (0, 2));
        row.removeFromLeft (6);
        swing->setBounds (row.reduced (0, 2));
        row = col.removeFromTop (24);
        fillAtEnd.setBounds (row.removeFromLeft (row.getWidth() / 2));
        crash.setBounds (row);
    }

private:
    te::MidiClip::Ptr clip;
    juce::TextEditor label;
    juce::OwnedArray<juce::TextButton> labelButtons, styleButtons;
    DrumXYPad xy;
    juce::OwnedArray<juce::ToggleButton> partToggles;
    juce::OwnedArray<juce::Slider> variationSliders;
    std::unique_ptr<juce::Slider> fills, swing;
    juce::ToggleButton fillAtEnd, crash;
    juce::TextButton newVariation { "New Variation" }, convert { "Convert to MIDI" }, kitNotes { "Kit Notes..." };
    DrumPatternPreview preview;
    juce::Rectangle<int> sectionTitle, styleTitle, feelTitle, partsTitle;

    void showKitNotesMenu()
    {
        auto track = clip->getTrack();
        if (track == nullptr) return;

        const auto current = track->state.getProperty (DrumTrack::noteMapProp).toString();
        auto set = [tr = te::Track::Ptr (track)] (const juce::String& v)
        {
            tr->edit.getUndoManager().beginNewTransaction ("Kit notes");
            tr->state.setProperty (DrumTrack::noteMapProp, v, &tr->edit.getUndoManager());
        };

        juce::PopupMenu m;
        m.addItem ("Automatic (from the instrument)", true, current.isEmpty(), [set] { set ({}); });
        m.addItem ("General MIDI (MT Power Drum Kit, most drum plug-ins)", true, current == "gm", [set] { set ("gm"); });
        m.addItem ("Pads from C1 (Sitala, MPC-style samplers)", true, current == "c1", [set] { set ("c1"); });
        m.addSeparator();
        m.addItem ("Custom...", true, current.containsChar (','), [this, tr = te::Track::Ptr (track)]
        {
            auto* editor = new KitNotesEditor (*tr);
            juce::CallOutBox::launchAsynchronously (std::unique_ptr<juce::Component> (editor), kitNotes.getScreenBounds(), nullptr);
        });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitNotes));
    }

    /** Type in the note each drum plays on. */
    struct KitNotesEditor : public juce::Component
    {
        KitNotesEditor (te::Track& t) : track (&t)
        {
            auto map = DrumTrack::noteMapFor (t);
            for (int i = 0; i < (int) drums::Piece::count; ++i)
            {
                auto* l = labels.add (new juce::Label ({}, drums::pieceName ((drums::Piece) i)));
                l->setFont (juce::FontOptions (11.0f));
                addAndMakeVisible (l);

                auto* e = editors.add (new juce::TextEditor());
                e->setInputRestrictions (3, "0123456789");
                e->setText (juce::String (map ((drums::Piece) i)), false);
                e->onReturnKey = e->onFocusLost = [this] { apply(); };
                addAndMakeVisible (e);
            }
            setSize (2 * 170, ((int) drums::Piece::count + 1) / 2 * 24 + 8);
        }

        void resized() override
        {
            for (int i = 0; i < labels.size(); ++i)
            {
                auto cell = juce::Rectangle<int> ((i % 2) * 170 + 4, 4 + (i / 2) * 24, 166, 22);
                labels[i]->setBounds (cell.removeFromLeft (100));
                editors[i]->setBounds (cell.reduced (2, 1));
            }
        }

        void apply()
        {
            juce::StringArray notes;
            for (auto* e : editors) notes.add (juce::String (juce::jlimit (0, 127, e->getText().getIntValue())));
            track->edit.getUndoManager().beginNewTransaction ("Kit notes");
            track->state.setProperty (DrumTrack::noteMapProp, notes.joinIntoString (","), &track->edit.getUndoManager());
        }

        te::Track::Ptr track;
        juce::OwnedArray<juce::Label> labels;
        juce::OwnedArray<juce::TextEditor> editors;
    };

    juce::Slider* makeSlider (const juce::String& name)
    {
        auto* s = new juce::Slider (juce::Slider::LinearBar, juce::Slider::TextBoxLeft);
        s->setRange (0.0, 100.0, 1.0);
        s->textFromValueFunction = [name] (double v) { return name + ": " + juce::String (juce::roundToInt (v)) + " %"; };
        s->setColour (juce::Slider::trackColourId, Colours::accent.withAlpha (0.45f));
        return s;
    }

    void setProp (const juce::Identifier& id, const juce::var& value, const juce::String& undoName)
    {
        clip->edit.getUndoManager().beginNewTransaction (undoName);
        clip->state.setProperty (id, value, &clip->edit.getUndoManager());
    }

    void refresh()
    {
        const auto s = DrumTrack::readSettings (clip->state);
        if (! label.hasKeyboardFocus (true))
            label.setText (DrumTrack::getLabel (clip->state), juce::dontSendNotification);

        for (int i = 0; i < styleButtons.size(); ++i)
            styleButtons[i]->setToggleState (i == (int) s.style, juce::dontSendNotification);

        xy.x = s.complexity;
        xy.y = s.loudness;
        xy.repaint();

        for (int i = 0; i < 3; ++i)
        {
            partToggles[i]->setToggleState (s.partOn[(size_t) i], juce::dontSendNotification);
            variationSliders[i]->setValue (s.variation[(size_t) i] * 100.0, juce::dontSendNotification);
            variationSliders[i]->setEnabled (s.partOn[(size_t) i]);
        }

        fills->setValue (s.fills * 100.0, juce::dontSendNotification);
        swing->setValue (s.swing < 0.0 ? -1.0 : s.swing * 100.0, juce::dontSendNotification);
        fillAtEnd.setToggleState (s.fillAtEnd, juce::dontSendNotification);
        crash.setToggleState (s.crashAtStart, juce::dontSendNotification);
        preview.repaint();
    }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override   { refresh(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override               { preview.repaint(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override        { preview.repaint(); }
};

} // namespace elsewhere
