// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "TrackHeader.h"

namespace elsewhere
{
//==============================================================================
/**
    Every track in the project, in order, for big sessions: click the dot to show or hide a track
    (in the Arrangement and the Mixer), click a name to select it, drag names to reorder.
*/
class TrackList : public juce::Component,
                  private juce::ValueTree::Listener,
                  private juce::AsyncUpdater,
                  private juce::ChangeListener
{
public:
    static constexpr int width = 190;

    TrackList (EditViewState& evs) : viewState (evs)
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&rows, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (8);

        menuButton.setTooltip ("Show or hide tracks");
        menuButton.setColour (juce::TextButton::buttonColourId, Colours::panelLight);
        menuButton.onClick = [this] { showMenu(); };
        addAndMakeVisible (menuButton);

        viewState.edit.state.addListener (this);
        viewState.selectionManager.addChangeListener (this);
        viewState.addChangeListener (this);
        rebuild();
    }

    ~TrackList() override
    {
        viewState.edit.state.removeListener (this);
        viewState.selectionManager.removeChangeListener (this);
        viewState.removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panel);

        auto header = getLocalBounds().removeFromTop (headerH).reduced (8, 0);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("TRACKS", header, juce::Justification::centredLeft);

        int hidden = 0;
        for (auto& t : tracks)
            hidden += EditViewState::isTrackHidden (*t) ? 1 : 0;

        if (hidden > 0)
            g.drawText (juce::String (hidden) + " hidden", header.withTrimmedRight (menuButton.getWidth() + 6), juce::Justification::centredRight);

        g.setColour (Colours::outline);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedRight (1);
        auto header = r.removeFromTop (headerH);
        menuButton.setBounds (header.removeFromRight (30).reduced (4, 5));
        viewport.setBounds (r);
        rows.setSize (viewport.getMaximumVisibleWidth(), juce::jmax (viewport.getMaximumVisibleHeight(), (int) tracks.size() * rowH + 8));
    }

private:
    static constexpr int headerH = 26, rowH = 22, eyeW = 22;

    //==============================================================================
    struct Rows : public juce::Component
    {
        Rows (TrackList& o) : owner (o) {}

        void paint (juce::Graphics& g) override
        {
            for (int i = 0; i < (int) owner.tracks.size(); ++i)
                owner.paintRow (g, *owner.tracks[(size_t) i], getRowArea (i));

            if (dropIndex >= 0)
            {
                g.setColour (Colours::accent);
                g.fillRect (eyeW, dropIndex * rowH - 1, getWidth() - eyeW, 2);
            }
        }

        juce::Rectangle<int> getRowArea (int i) const   { return { 0, i * rowH, getWidth(), rowH }; }
        int getRowAt (int y) const                      { return y >= 0 ? y / rowH : -1; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            pressed = getRowAt (e.y);
            dropIndex = -1;

            if (! juce::isPositiveAndBelow (pressed, (int) owner.tracks.size()))
                return;

            auto t = owner.tracks[(size_t) pressed];

            // The dot shows / hides; alt-click does it to every track
            if (e.x < eyeW && ! e.mods.isPopupMenu())
            {
                auto hide = ! EditViewState::isTrackHidden (*t);

                if (e.mods.isAltDown())
                    for (auto& other : owner.tracks)
                        EditViewState::setTrackHidden (*other, hide);
                else
                    EditViewState::setTrackHidden (*t, hide);

                pressed = -1;
                return;
            }

            auto& sm = owner.viewState.selectionManager;

            if (e.mods.isCommandDown() || e.mods.isShiftDown())
                sm.select (t.get(), true);
            else if (! (e.mods.isPopupMenu() && sm.isSelected (t.get())))
                sm.selectOnly (t.get());

            owner.viewState.focusTrack (t.get());

            if (e.mods.isPopupMenu())
            {
                pressed = -1;
                showTrackMenu (*t, owner.viewState, [t] { renameTrack (*t); });
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (pressed < 0 || e.getDistanceFromDragStartY() == 0 || std::abs (e.getDistanceFromDragStartY()) < 4)
                return;

            auto index = juce::jlimit (0, (int) owner.tracks.size(), (e.y + rowH / 2) / rowH);
            dropIndex = owner.canMove (pressed, index) ? index : -1;
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (pressed >= 0 && dropIndex >= 0)
                owner.move (pressed, dropIndex);

            pressed = dropIndex = -1;
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            auto i = getRowAt (e.y);

            if (e.x >= eyeW && juce::isPositiveAndBelow (i, (int) owner.tracks.size()))
                renameTrack (*owner.tracks[(size_t) i]);
        }

        TrackList& owner;
        int pressed = -1, dropIndex = -1;
    };

    EditViewState& viewState;
    juce::Viewport viewport;
    Rows rows { *this };
    juce::TextButton menuButton { juce::String::fromUTF8 ("\xe2\x80\xa6") };
    std::vector<te::Track::Ptr> tracks;

    void rebuild()
    {
        tracks.clear();

        for (auto t : te::getAllTracks (viewState.edit))
            if (! t->isMasterTrack() && (t->isAudioTrack() || t->isFolderTrack()))
                tracks.push_back (t);

        resized();
        rows.repaint();
        repaint();
    }

    void paintRow (juce::Graphics& g, te::Track& t, juce::Rectangle<int> r)
    {
        bool hidden = EditViewState::isTrackHidden (t);
        bool selected = viewState.selectionManager.isSelected (&t);
        bool focused = viewState.focusedTrack == &t;

        if (selected || focused)
        {
            g.setColour (Theme::lift (Colours::panelLight, selected ? 0.08f : 0.0f));
            g.fillRect (r);
        }

        // Shown / hidden dot
        auto eye = r.removeFromLeft (eyeW).toFloat().withSizeKeepingCentre (9.0f, 9.0f);
        auto colour = Theme::ofTrack (t);

        if (hidden)
        {
            g.setColour (Colours::textDim.withAlpha (0.6f));
            g.drawEllipse (eye, 1.2f);
        }
        else
        {
            g.setColour (colour);
            g.fillEllipse (eye);
        }

        r.removeFromLeft (juce::jmax (0, t.getTrackDepth()) * 10);

        if (t.isFolderTrack())
        {
            juce::Path arrow;
            arrow.addTriangle (0.0f, 0.0f, 7.0f, 0.0f, 3.5f, 6.0f);
            g.setColour (Colours::textDim);
            g.fillPath (arrow, juce::AffineTransform::translation ((float) r.getX(), (float) r.getCentreY() - 3.0f));
            r.removeFromLeft (11);
        }

        g.setColour (hidden ? Colours::textDim : Colours::text);
        g.setFont (juce::FontOptions (12.0f, t.isFolderTrack() ? juce::Font::bold : juce::Font::plain));
        g.drawText (t.getName(), r.withTrimmedRight (4), juce::Justification::centredLeft, true);

        g.setColour (Colours::outline.withAlpha (0.5f));
        g.drawHorizontalLine (r.getBottom() - 1, 0.0f, (float) rows.getWidth());
    }

    /** Can the track at `from` move to just before row `to` (or the end)? Not into itself. */
    bool canMove (int from, int to) const
    {
        if (to == from || to == from + 1)
            return false;

        auto moving = tracks[(size_t) from].get();
        auto target = to < (int) tracks.size() ? tracks[(size_t) to].get() : tracks.back().get();

        for (auto t = target; t != nullptr; t = t->getParentTrack())
            if (t == moving)
                return false;

        return true;
    }

    void move (int from, int to)
    {
        auto moving = tracks[(size_t) from];
        auto& edit = viewState.edit;
        edit.getUndoManager().beginNewTransaction ("Move " + moving->getName());

        if (to < (int) tracks.size())
            edit.moveTrack (moving, te::TrackInsertPoint (*tracks[(size_t) to], true));
        else
            edit.moveTrack (moving, te::TrackInsertPoint (*tracks.back(), false));
    }

    static void renameTrack (te::Track& t)
    {
        auto* aw = Theme::newAlertWindow ("Rename Track", {});
        Theme::addTextField (*aw, "name", t.getName());
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, ref = te::Track::Ptr (&t)] (int res)
        {
            if (res == 1)
                ref->setName (aw->getTextEditorContents ("name"));
        }), true);
    }

    void showMenu()
    {
        juce::PopupMenu m;
        auto list = tracks;
        auto* evs = &viewState;

        m.addItem ("Show All Tracks", [list] { for (auto& t : list) EditViewState::setTrackHidden (*t, false); });
        m.addItem ("Show Only Selected Tracks", [list, evs]
        {
            auto& sm = evs->selectionManager;

            for (auto& t : list)
            {
                // A selected group's tracks stay too
                bool keep = false;
                for (auto p = t.get(); p != nullptr && ! keep; p = p->getParentTrack())
                    keep = sm.isSelected (p);

                // ...and so do the groups that hold a selected track
                for (auto& other : list)
                    for (auto p = other->getParentTrack(); p != nullptr && ! keep; p = p->getParentTrack())
                        keep = p == t.get() && sm.isSelected (other.get());

                EditViewState::setTrackHidden (*t, ! keep);
            }
        });
        m.addItem ("Hide Muted Tracks", [list] { for (auto& t : list) if (t->isMuted (false)) EditViewState::setTrackHidden (*t, true); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override     { rows.repaint(); }
    void handleAsyncUpdate() override                                   { rebuild(); }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override         { if (te::TrackList::isTrack (c)) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override  { if (te::TrackList::isTrack (c)) triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override            { triggerAsyncUpdate(); }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (te::TrackList::isTrack (v) && (id == te::IDs::name || id == te::IDs::colour || id == EditViewState::hiddenProperty))
            triggerAsyncUpdate();
    }
};

} // namespace elsewhere
