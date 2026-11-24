// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "TrackHeader.h"

namespace elsewhere
{
//==============================================================================
/**
    Every track in the project, in order, for big sessions: click the dot to show or hide a track
    (in the Arrangement and the Mixer), click a name to select it, drag names to reorder (or onto a group's name to move them into
    the group).
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
        menuButton.setTitle ("Show or Hide Tracks");
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

    /** Where dragged names would land. */
    struct Drop
    {
        te::Track* target = nullptr;
        Mixing::TrackDrop where = Mixing::TrackDrop::before;
    };

    //==============================================================================
    struct Rows : public juce::Component
    {
        Rows (TrackList& o) : owner (o) {}

        void paint (juce::Graphics& g) override
        {
            for (int i = 0; i < (int) owner.tracks.size(); ++i)
                owner.paintRow (g, *owner.tracks[(size_t) i], getRowArea (i));

            if (auto i = drop.has_value() ? owner.indexOf (drop->target) : -1; i >= 0)
            {
                g.setColour (Colours::accent);

                if (drop->where == Mixing::TrackDrop::into)
                {
                    g.drawRect (getRowArea (i).withTrimmedLeft (eyeW), 2);
                }
                else
                {
                    // At the depth it lands at, so going into a group or out of one shows
                    auto x = eyeW + juce::jmax (0, drop->target->getTrackDepth()) * 10;
                    auto y = (drop->where == Mixing::TrackDrop::before ? i : i + 1) * rowH;
                    g.fillRect (x, y - 1, getWidth() - x, 2);
                }
            }
        }

        juce::Rectangle<int> getRowArea (int i) const   { return { 0, i * rowH, getWidth(), rowH }; }
        int getRowAt (int y) const                      { return y >= 0 ? y / rowH : -1; }

        void mouseDown (const juce::MouseEvent& e) override
        {
            pressed = getRowAt (e.y);
            drop.reset();
            selectOnlyOnMouseUp = false;

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

            // A plain click on one of several selected tracks waits for the mouse up, so the
            // whole selection can be dragged
            if (e.mods.isCommandDown() || e.mods.isShiftDown())
                sm.select (t.get(), true);
            else if (! e.mods.isPopupMenu() && sm.isSelected (t.get()) && sm.getItemsOfType<te::Track>().size() > 1)
                selectOnlyOnMouseUp = true;
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
            if (pressed < 0 || std::abs (e.getDistanceFromDragStartY()) < 4)
                return;

            selectOnlyOnMouseUp = false;
            drop = owner.dropAt (e.y, owner.draggedTracks (pressed));
            repaint();

            if (auto vp = findParentComponentOfClass<juce::Viewport>())
            {
                auto p = vp->getLocalPoint (this, e.getPosition());
                vp->autoScroll (p.x, p.y, 20, 8);
            }
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (pressed >= 0 && drop.has_value())
                owner.move (owner.draggedTracks (pressed), *drop);
            else if (selectOnlyOnMouseUp && juce::isPositiveAndBelow (pressed, (int) owner.tracks.size()))
                owner.viewState.selectionManager.selectOnly (owner.tracks[(size_t) pressed].get());

            pressed = -1;
            selectOnlyOnMouseUp = false;
            drop.reset();
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            auto i = getRowAt (e.y);

            if (e.x >= eyeW && juce::isPositiveAndBelow (i, (int) owner.tracks.size()))
                renameTrack (*owner.tracks[(size_t) i]);
        }

        TrackList& owner;
        int pressed = -1;
        bool selectOnlyOnMouseUp = false;
        std::optional<Drop> drop;
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

    int indexOf (const te::Track* t) const
    {
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].get() == t)
                return (int) i;

        return -1;
    }

    /** What dragging the name in `row` moves: the selection if it's in it, or just that track. */
    juce::Array<te::Track*> draggedTracks (int row)
    {
        if (! juce::isPositiveAndBelow (row, (int) tracks.size()))
            return {};

        return TrackDrag::tracksFor (viewState, *tracks[(size_t) row]);
    }

    /** Where a drag at `y` lands: into a group when over the middle of its name, otherwise
        between two names, alongside the track below the line (in its group, if it has one). */
    std::optional<Drop> dropAt (int y, const juce::Array<te::Track*>& moving) const
    {
        const int num = (int) tracks.size();

        if (num == 0 || moving.isEmpty())
            return std::nullopt;

        auto canDrop = [&] (Drop d) -> std::optional<Drop>
        {
            if (Mixing::canMoveTracks (viewState.edit, moving, *d.target, d.where))
                return d;

            return std::nullopt;
        };

        if (auto row = y / rowH; y >= 0 && row < num && tracks[(size_t) row]->isFolderTrack())
        {
            auto within = y - row * rowH;

            if (within > rowH / 4 && within < rowH - rowH / 4)
                return canDrop ({ tracks[(size_t) row].get(), Mixing::TrackDrop::into });
        }

        auto index = juce::jlimit (0, num, (y + rowH / 2) / rowH);

        if (index < num)
            return canDrop ({ tracks[(size_t) index].get(), Mixing::TrackDrop::before });

        return canDrop ({ tracks.back().get(), Mixing::TrackDrop::after });
    }

    void move (const juce::Array<te::Track*>& moving, Drop d)
    {
        auto& edit = viewState.edit;
        edit.getUndoManager().beginNewTransaction (d.where == Mixing::TrackDrop::into ? "Move to group"
                                                   : moving.size() == 1 ? "Move " + moving.getFirst()->getName()
                                                                        : juce::String ("Move tracks"));
        Mixing::moveTracks (edit, moving, *d.target, d.where);
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
