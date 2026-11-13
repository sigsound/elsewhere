// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Markers.h"

namespace elsewhere
{
inline constexpr const char* clipDragPrefix = "elsewhere-clip:";

//==============================================================================
/**
    The panel on the right: every clip in the arrangement (click to find it, drag one onto a
    track to place a copy) and every marker (click to go there).
*/
class ClipList : public juce::Component,
                 private juce::ValueTree::Listener,
                 private juce::ChangeListener,
                 private juce::AsyncUpdater
{
public:
    static constexpr int width = 250;
    enum class Tab { clips, markers };

    ClipList (EditViewState& evs) : viewState (evs)
    {
        for (auto* b : { &clipsTab, &markersTab })
        {
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonOnColourId, Colours::accent);
            b->setColour (juce::TextButton::textColourOnId, Colours::background);
            b->setColour (juce::TextButton::buttonColourId, Colours::panelLight);
            addAndMakeVisible (b);
        }

        clipsTab.setConnectedEdges (juce::Button::ConnectedOnRight);
        markersTab.setConnectedEdges (juce::Button::ConnectedOnLeft);
        clipsTab.onClick = [this] { setTab (Tab::clips); };
        markersTab.onClick = [this] { setTab (Tab::markers); };

        filter.setTextToShowWhenEmpty ("Find", Colours::textDim);
        filter.setIndents (10, filter.getTopIndent());
        filter.onTextChange = [this] { rebuild(); };
        filter.onEscapeKey = [this] { filter.clear(); rebuild(); unfocusAllComponents(); };
        addAndMakeVisible (filter);

        addMarker.setTooltip ("Add a marker at the playhead (Return)");
        addMarker.onClick = [this] { Markers::add (viewState.edit, viewState.edit.getTransport().getPosition()); };
        addChildComponent (addMarker);

        list.setModel (&model);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, Colours::panel);
        addAndMakeVisible (list);

        viewState.edit.state.addListener (this);
        viewState.edit.getMarkerManager().addChangeListener (this);
        viewState.selectionManager.addChangeListener (this);
        setTab (Tab::clips);
    }

    ~ClipList() override
    {
        viewState.edit.state.removeListener (this);
        viewState.edit.getMarkerManager().removeChangeListener (this);
        viewState.selectionManager.removeChangeListener (this);
    }

    Tab getTab() const      { return tab; }

    void setTab (Tab t)
    {
        tab = t;
        clipsTab.setToggleState (t == Tab::clips, juce::dontSendNotification);
        markersTab.setToggleState (t == Tab::markers, juce::dontSendNotification);
        addMarker.setVisible (t == Tab::markers);
        list.setRowHeight (t == Tab::clips ? 34 : 24);
        resized();
        rebuild();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panel);
        g.setColour (Colours::outline);
        g.drawVerticalLine (0, 0.0f, (float) getHeight());

        if (model.rows.empty())
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (12.0f));
            auto text = tab == Tab::markers ? "No markers yet.\nPress Return to add one at the playhead."
                                            : filter.isEmpty() ? "No clips yet." : "Nothing matches.";
            g.drawFittedText (text, list.getBounds().withHeight (60).reduced (12, 0), juce::Justification::centred, 3);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (1).reduced (6, 0);
        r.removeFromTop (5);
        auto tabs = r.removeFromTop (22);
        clipsTab.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2));
        markersTab.setBounds (tabs);
        r.removeFromTop (5);
        auto search = r.removeFromTop (22);

        if (addMarker.isVisible())
        {
            addMarker.setBounds (search.removeFromRight (26));
            search.removeFromRight (4);
        }

        filter.setBounds (search);
        r.removeFromTop (5);
        list.setBounds (r.withTrimmedLeft (-6).withWidth (getWidth() - 1));
    }

private:
    struct Row
    {
        te::Clip::Ptr clip;
        juce::String name, detail, time;
        juce::Colour colour;
    };

    struct Model : public juce::ListBoxModel
    {
        Model (ClipList& o) : owner (o) {}

        int getNumRows() override   { return (int) rows.size(); }

        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (! juce::isPositiveAndBelow (row, (int) rows.size()))
                return;

            auto& r = rows[(size_t) row];
            auto area = juce::Rectangle<int> (0, 0, w, h);

            if (selected)
            {
                g.setColour (Colours::panelLight);
                g.fillRect (area);
            }

            g.setColour (r.colour);
            g.fillRect (area.removeFromLeft (4).reduced (0, 3));
            area.removeFromLeft (6);
            area.removeFromRight (6);

            g.setFont (juce::FontOptions (11.0f));
            g.setColour (Colours::textDim);
            g.drawText (r.time, area.removeFromRight (70), juce::Justification::centredRight);

            if (owner.tab == Tab::clips)
            {
                auto top = area.removeFromTop (h / 2).withTrimmedTop (3);
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (r.name, top, juce::Justification::centredLeft, true);
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (r.detail, area.withTrimmedBottom (3), juce::Justification::centredLeft, true);
            }
            else
            {
                g.setColour (Colours::textDim);
                g.drawText (r.detail, area.removeFromLeft (22), juce::Justification::centredLeft);
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (r.name, area, juce::Justification::centredLeft, true);
            }

            g.setColour (Colours::outline.withAlpha (0.5f));
            g.drawHorizontalLine (h - 1, 0.0f, (float) w);
        }

        void listBoxItemClicked (int row, const juce::MouseEvent& e) override
        {
            if (! juce::isPositiveAndBelow (row, (int) rows.size()))
                return;

            auto clip = rows[(size_t) row].clip;

            if (e.mods.isPopupMenu())
                return owner.showMenu (*clip);

            owner.reveal (*clip);
        }

        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            if (! juce::isPositiveAndBelow (row, (int) rows.size()))
                return;

            if (auto m = dynamic_cast<te::MarkerClip*> (rows[(size_t) row].clip.get()))
                Markers::rename (*m);
            else
                owner.renameClip (*rows[(size_t) row].clip);
        }

        void deleteKeyPressed (int row) override
        {
            if (juce::isPositiveAndBelow (row, (int) rows.size()))
                if (auto m = dynamic_cast<te::MarkerClip*> (rows[(size_t) row].clip.get()))
                    Markers::remove (*m);
        }

        juce::var getDragSourceDescription (const juce::SparseSet<int>& selected) override
        {
            if (owner.tab != Tab::clips || selected.isEmpty() || ! juce::isPositiveAndBelow (selected[0], (int) rows.size()))
                return {};

            return juce::String (clipDragPrefix) + rows[(size_t) selected[0]].clip->itemID.toString();
        }

        ClipList& owner;
        std::vector<Row> rows;
    };

    EditViewState& viewState;
    Tab tab = Tab::clips;
    juce::TextButton clipsTab { "Clips" }, markersTab { "Markers" }, addMarker { "+" };
    juce::TextEditor filter;
    Model model { *this };
    juce::ListBox list;

    void rebuild()
    {
        auto& edit = viewState.edit;
        auto needle = filter.getText().trim();
        model.rows.clear();

        if (tab == Tab::markers)
        {
            for (auto m : Markers::getSorted (edit))
                if (needle.isEmpty() || m->getName().containsIgnoreCase (needle))
                    model.rows.push_back ({ m, m->getName(), juce::String (m->getMarkerID()),
                                            EngineHelpers::formatBarsBeats (edit, m->getPosition().getStart()), Markers::getColour (*m) });
        }
        else
        {
            for (auto t : te::getClipTracks (edit))
            {
                if (t->isMarkerTrack() || t->isChordTrack() || t->isArrangerTrack())
                    continue;

                for (auto c : t->getClips())
                {
                    if (needle.isNotEmpty() && ! c->getName().containsIgnoreCase (needle) && ! t->getName().containsIgnoreCase (needle))
                        continue;

                    model.rows.push_back ({ c, c->getName().isNotEmpty() ? c->getName() : juce::String ("(unnamed)"),
                                            t->getName() + juce::String::fromUTF8 ("  \xc2\xb7  ") + formatLength (*c),
                                            EngineHelpers::formatBarsBeats (edit, c->getPosition().getStart()),
                                            Theme::ofTrack (*t) });
                }
            }

            std::stable_sort (model.rows.begin(), model.rows.end(), [] (auto& a, auto& b) { return a.name.compareNatural (b.name) < 0; });
        }

        list.updateContent();
        list.repaint();
        repaint();
    }

    static juce::String formatLength (te::Clip& c)
    {
        auto secs = c.getPosition().getLength().inSeconds();
        return secs >= 60.0 ? juce::String ((int) secs / 60) + ":" + juce::String ((int) secs % 60).paddedLeft ('0', 2)
                            : juce::String (secs, 1) + " s";
    }

    /** Selects the clip and puts the playhead at its start, scrolling the arrangement to it. */
    void reveal (te::Clip& c)
    {
        auto start = c.getPosition().getStart();

        if (dynamic_cast<te::MarkerClip*> (&c) == nullptr)
        {
            viewState.selectionManager.selectOnly (&c);
            viewState.focusTrack (c.getTrack());
        }

        viewState.edit.getTransport().setPosition (start);

        if (viewState.revealTime)
            viewState.revealTime (start);
    }

    void renameClip (te::Clip& c)
    {
        auto* aw = Theme::newAlertWindow ("Rename Clip", {});
        Theme::addTextField (*aw, "name", c.getName());
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, ref = te::Clip::Ptr (&c)] (int res)
        {
            if (res == 1)
            {
                ref->edit.getUndoManager().beginNewTransaction ("Rename clip");
                ref->setName (aw->getTextEditorContents ("name"));
            }
        }), true);
    }

    void showMenu (te::Clip& c)
    {
        auto ref = te::Clip::Ptr (&c);
        juce::PopupMenu m;
        m.addItem ("Go To", [this, ref] { reveal (*ref); });

        if (auto mc = dynamic_cast<te::MarkerClip*> (&c))
        {
            m.addItem ("Rename...", [ref] { if (auto mk = dynamic_cast<te::MarkerClip*> (ref.get())) Markers::rename (*mk); });
            m.addSeparator();
            m.addItem ("Delete", [ref] { if (auto mk = dynamic_cast<te::MarkerClip*> (ref.get())) Markers::remove (*mk); });
        }
        else
        {
            m.addItem ("Rename...", [this, ref] { renameClip (*ref); });
        }

        m.showMenuAsync ({});
    }

    void handleAsyncUpdate() override                                   { rebuild(); }
    void changeListenerCallback (juce::ChangeBroadcaster* b) override
    {
        if (b == &viewState.selectionManager)
        {
            // Keep the list's highlight on the selected clip
            auto selected = viewState.getSelectedClip();

            for (int i = 0; i < (int) model.rows.size(); ++i)
                if (model.rows[(size_t) i].clip.get() == selected)
                    return list.selectRow (i, true, true);

            return;
        }

        triggerAsyncUpdate();
    }

    static bool isRelevant (const juce::ValueTree& v)
    {
        return te::Clip::isClipState (v) || te::TrackList::isTrack (v);
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override         { if (isRelevant (c)) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override  { if (isRelevant (c)) triggerAsyncUpdate(); }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (isRelevant (v) && (id == te::IDs::name || id == te::IDs::start || id == te::IDs::length || id == te::IDs::colour))
            triggerAsyncUpdate();
    }
};

} // namespace elsewhere
