// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/** Markers (memory locations): numbered places in the song to jump back to. */
namespace Markers
{
    inline juce::Array<te::MarkerClip*> getSorted (te::Edit& edit)
    {
        juce::Array<te::MarkerClip*> list;

        for (auto m : edit.getMarkerManager().getMarkers())
            list.add (m);

        std::sort (list.begin(), list.end(), [] (auto a, auto b) { return a->getPosition().getStart() < b->getPosition().getStart(); });
        return list;
    }

    inline te::MarkerClip* add (te::Edit& edit, te::TimePosition at, const juce::String& name = {})
    {
        edit.getUndoManager().beginNewTransaction ("Add marker");
        edit.ensureMarkerTrack();

        auto& mm = edit.getMarkerManager();
        auto number = mm.getNextUniqueID();
        auto m = mm.createMarker (number, at, te::TimeDuration::fromSeconds (0.0), nullptr);

        if (m != nullptr)
            m->setName (name.isNotEmpty() ? name : "Marker " + juce::String (number));

        return dynamic_cast<te::MarkerClip*> (m.get());
    }

    inline bool goTo (te::Edit& edit, int number)
    {
        if (auto m = edit.getMarkerManager().getMarkerByID (number))
        {
            edit.getTransport().setPosition (m->getPosition().getStart());
            return true;
        }

        return false;
    }

    inline void goToNext (te::Edit& edit, bool forwards)
    {
        auto& transport = edit.getTransport();
        auto& mm = edit.getMarkerManager();
        auto now = transport.getPosition();

        if (auto m = forwards ? mm.getNextMarker (now) : mm.getPrevMarker (now - te::TimeDuration::fromSeconds (0.001)))
            transport.setPosition (m->getPosition().getStart());
        else if (! forwards)
            transport.setPosition (te::TimePosition());
    }

    inline void remove (te::MarkerClip& m)
    {
        m.edit.getUndoManager().beginNewTransaction ("Delete marker");
        m.removeFromParent();
    }

    inline void rename (te::MarkerClip& m)
    {
        auto* aw = Theme::newAlertWindow ("Rename Marker", {});
        Theme::addTextField (*aw, "name", m.getName());
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, ref = te::Clip::Ptr (&m)] (int res)
        {
            if (res == 1)
            {
                ref->edit.getUndoManager().beginNewTransaction ("Rename marker");
                ref->setName (aw->getTextEditorContents ("name"));
            }
        }), true);
    }

    /** Markers are ink: only tracks have colours (any colour saved on a marker is kept, not shown). */
    inline juce::Colour getColour (te::MarkerClip&)     { return Colours::text; }
}

//==============================================================================
/**
    The markers along the top of the Arrangement. Click one to jump there, drag to move it,
    double-click to rename, right-click for more; double-click an empty spot to add one.
*/
class MarkerStrip : public juce::Component,
                    private juce::ChangeListener
{
public:
    static constexpr int height = 17;

    MarkerStrip (EditViewState& evs) : viewState (evs)
    {
        viewState.edit.getMarkerManager().addChangeListener (this);
        viewState.addChangeListener (this);
    }

    ~MarkerStrip() override
    {
        viewState.edit.getMarkerManager().removeChangeListener (this);
        viewState.removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        // Orbit: markers are quiet capsules (ink while you drag one); only tracks have colours
        g.fillAll (Colours::background);
        g.setFont (juce::FontOptions (10.0f));

        auto markers = Markers::getSorted (viewState.edit);

        for (int i = 0; i < markers.size(); ++i)
        {
            auto* m = markers[i];
            auto x = viewState.timeToX (m->getPosition().getStart());
            auto nextX = i + 1 < markers.size() ? viewState.timeToX (markers[i + 1]->getPosition().getStart()) : (float) getWidth();

            if (nextX < 0.0f || x > (float) getWidth())
                continue;

            const bool held = m == dragging.get();
            auto label = m->getName().isNotEmpty() ? m->getName().toLowerCase() : juce::String (m->getMarkerID());
            auto w = juce::jmin (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), label) + 20.0f, juce::jmax (12.0f, nextX - x - 2.0f));
            auto tag = juce::Rectangle<float> (x, 1.5f, w, (float) getHeight() - 3.0f);

            g.setColour (held ? Colours::text : Colours::panelLight);
            g.fillRoundedRectangle (tag, tag.getHeight() * 0.5f);
            g.setColour (held ? Colours::textInverse : Colours::text);
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ tag.getX() + tag.getHeight() * 0.5f, tag.getCentreY() }));
            g.setColour (held ? Colours::textInverse : Colours::text);
            g.drawText (label, tag.withTrimmedLeft (tag.getHeight()).withTrimmedRight (4.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = findMarkerAt ((float) e.x);
        dragMoved = false;

        if (dragging == nullptr)
            return;

        if (e.mods.isPopupMenu())
        {
            if (auto m = dynamic_cast<te::MarkerClip*> (dragging.get()))
                showMenu (*m);

            dragging = nullptr;
            return;
        }

        dragStart = dragging->getPosition().getStart();
        viewState.edit.getUndoManager().beginNewTransaction ("Move marker");
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging == nullptr || e.mods.isPopupMenu() || std::abs (e.getDistanceFromDragStartX()) < 3)
            return;

        dragMoved = true;
        auto beat = viewState.snapBeat (viewState.timeToBeat (dragStart) + e.getDistanceFromDragStartX() / viewState.pixelsPerBeat,
                                        e.mods.isCommandDown());
        dragging->setStart (viewState.beatToTime (beat), false, true);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragging != nullptr && ! dragMoved && ! e.mods.isPopupMenu() && e.getNumberOfClicks() == 1)
            viewState.edit.getTransport().setPosition (dragging->getPosition().getStart());

        dragging = nullptr;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto m = findMarkerAt ((float) e.x))
            return Markers::rename (*m);

        Markers::add (viewState.edit, viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) e.x))));
    }

private:
    EditViewState& viewState;
    te::Clip::Ptr dragging;
    te::TimePosition dragStart;
    bool dragMoved = false;

    /** The marker whose tag is under x: the last one starting at or before it, if its tag reaches. */
    te::MarkerClip* findMarkerAt (float x)
    {
        te::MarkerClip* found = nullptr;

        for (auto m : Markers::getSorted (viewState.edit))
        {
            auto mx = viewState.timeToX (m->getPosition().getStart());

            if (mx - 3.0f <= x && x < mx + 120.0f)
                found = m;
        }

        return found;
    }

    void showMenu (te::MarkerClip& m)
    {
        auto ref = te::Clip::Ptr (&m);
        juce::PopupMenu menu;
        menu.addItem ("Go To", [ref] { ref->edit.getTransport().setPosition (ref->getPosition().getStart()); });
        menu.addItem ("Rename...", [ref] { if (auto mc = dynamic_cast<te::MarkerClip*> (ref.get())) Markers::rename (*mc); });

        menu.addSeparator();
        menu.addItem ("Delete", [ref] { if (auto mc = dynamic_cast<te::MarkerClip*> (ref.get())) Markers::remove (*mc); });
        menu.showMenuAsync ({});
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override     { repaint(); }
};

} // namespace elsewhere
