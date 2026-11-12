// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/**
    The lane under the piano roll for what isn't notes: velocity, pitch bend, channel pressure
    and any controller (mod wheel, expression, sustain...).

    Velocity shows as a stem for each note with a circle on top. Drag a stem to set it (all the
    selected notes move together when it's one of them), or drag across the lane to paint
    velocities onto the notes you pass. With notes selected, painting only changes those.

    Controllers show as a held line with a circle at each change. Click to add a point, drag a
    point to move it, drag across empty space to draw, double-click a point to delete it.

    Times and values are the engine's: beats in the clip's sequence, and controller values in
    14 bits (a 7-bit CC value << 7; pitch bend 0-16383 with 8192 in the middle).
*/
namespace MidiLanes
{
    inline constexpr int velocityType = -1;
    inline constexpr int pitchType = te::MidiControllerEvent::pitchWheelType;
    inline constexpr int pressureType = te::MidiControllerEvent::channelPressureType;

    inline juce::String laneName (int type)
    {
        switch (type)
        {
            case velocityType:  return "Velocity";
            case pitchType:     return "Pitch Bend";
            case pressureType:  return "Pressure";
            case 1:             return "Mod Wheel";
            case 11:            return "Expression";
            case 64:            return "Sustain";
            default:            break;
        }

        if (auto name = juce::MidiMessage::getControllerName (type))
            return "CC " + juce::String (type) + " " + juce::String (name);

        return "CC " + juce::String (type);
    }

    /** A name that fits beside the keyboard. */
    inline juce::String shortName (int type)
    {
        switch (type)
        {
            case velocityType:  return "velocity";
            case pitchType:     return "bend";
            case pressureType:  return "pressure";
            case 1:             return "mod";
            case 11:            return "expression";
            case 64:            return "sustain";
            default:            return "cc " + juce::String (type);
        }
    }

    /** The largest value a lane holds (velocity 1-127; controllers in 14 bits). */
    inline int maxValue (int type)      { return type == velocityType ? 127 : 16383; }
    inline int minValue (int type)      { return type == velocityType ? 1 : 0; }

    /** The value as you'd read it: velocity and CCs 0-127, bend from -8192 to 8191. */
    inline juce::String valueText (int type, int value)
    {
        if (type == velocityType)   return juce::String (value);
        if (type == pitchType)      return (value >= 8192 ? "+" : "") + juce::String (value - 8192);
        if (type == 64)             return (value >> 7) >= 64 ? "On" : "Off";
        return juce::String (value >> 7);
    }

    /** Stores a value the way the lane can show it: CCs and pressure on their 7-bit steps. */
    inline int quantiseValue (int type, int value)
    {
        value = juce::jlimit (minValue (type), maxValue (type), value);
        return (type == velocityType || type == pitchType) ? value : (value >> 7) << 7;
    }

    //==============================================================================
    /** Edits on a clip's MIDI, so the lane and the tests share them. */
    inline void setVelocity (te::MidiNote& n, int velocity, juce::UndoManager* um)
    {
        n.setVelocity (juce::jlimit (1, 127, velocity), um);
    }

    /** Paints velocities from (b1, v1) to (b2, v2), on the notes starting in between. */
    inline void paintVelocities (te::MidiList& seq, const juce::Array<te::MidiNote*>& only, double b1, int v1, double b2, int v2,
                                 juce::UndoManager* um)
    {
        if (b2 < b1)
            std::swap (b1, b2), std::swap (v1, v2);

        for (auto n : seq.getNotes())
        {
            auto b = n->getStartBeat().inBeats();

            if (b < b1 - 1.0e-6 || b > b2 + 1.0e-6 || (! only.isEmpty() && ! only.contains (n)))
                continue;

            auto t = b2 - b1 > 1.0e-9 ? (b - b1) / (b2 - b1) : 0.0;
            setVelocity (*n, juce::roundToInt (v1 + (v2 - v1) * t), um);
        }
    }

    inline juce::Array<te::MidiControllerEvent*> getEvents (te::MidiList& seq, int type)
    {
        juce::Array<te::MidiControllerEvent*> events;

        for (auto e : seq.getControllerEvents())
            if (e->getType() == type)
                events.add (e);

        std::sort (events.begin(), events.end(), [] (auto a, auto b) { return a->getBeatPosition() < b->getBeatPosition(); });
        return events;
    }

    /** Draws a line of controller values from (b1, v1) to (b2, v2), replacing what was there,
        a point every `step` beats. */
    inline void drawControllers (te::MidiList& seq, int type, double b1, int v1, double b2, int v2, double step, juce::UndoManager* um)
    {
        if (b2 < b1)
            std::swap (b1, b2), std::swap (v1, v2);

        b1 = juce::jmax (0.0, b1);
        seq.removeControllersBetween (type, te::BeatPosition::fromBeats (b1 - 1.0e-6), te::BeatPosition::fromBeats (b2 + 1.0e-6), um);

        int last = -1;

        for (double b = b1; b <= b2 + 1.0e-6; b += step)
        {
            auto t = b2 - b1 > 1.0e-9 ? (b - b1) / (b2 - b1) : 1.0;
            auto v = quantiseValue (type, juce::roundToInt (v1 + (v2 - v1) * t));

            // A held value needs only one point
            if (v != last)
                seq.addControllerEvent (te::BeatPosition::fromBeats (b), type, v, um);

            last = v;
        }
    }

    inline bool hasData (te::MidiList& seq, int type)
    {
        return type == velocityType ? seq.getNumNotes() > 0 : seq.containsController (type);
    }

    //==============================================================================
    /** What the lane needs from the piano roll above it. */
    struct Host
    {
        std::function<float (double)> beatToX;
        std::function<double (float)> xToBeat;
        std::function<double (double)> snap;                // to the grid (nearest)
        std::function<double()> getGrid;
        std::function<juce::Array<te::MidiNote*>()> getSelection;
        std::function<juce::Colour()> getColour;
        std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> wheel;
        int keyboardW = 56;
    };

    class Lane : public juce::Component
    {
    public:
        static constexpr int openHeight = 88, closedHeight = 20;

        Lane (te::MidiClip& c, Host h) : clip (&c), host (std::move (h)) {}

        int getType() const     { return type; }
        void setType (int t)    { type = t; lastType() = t; repaint(); }
        bool isOpen() const     { return open; }

        std::function<void()> onOpenChanged;

        // The lane you picked and whether it's open, kept for the next clip you open
        static int& lastType()      { static int t = velocityType; return t; }
        static bool& lastOpen()     { static bool o = true; return o; }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds();
            g.fillAll (Theme::lift (Colours::background, 0.015f));
            g.setColour (Colours::outline);
            g.drawHorizontalLine (0, 0.0f, (float) getWidth());

            // The lane's name in the keyboard column, with a dot that opens and closes it
            auto label = r.removeFromLeft (host.keyboardW).removeFromTop (closedHeight);
            g.setColour (Colours::panelDark);
            g.fillRect (getLocalBounds().withWidth (host.keyboardW).withTrimmedTop (1));
            auto disc = juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ 9.0f, (float) label.getCentreY() });
            g.setColour (Colours::textDim);
            if (open) g.fillEllipse (disc); else g.drawEllipse (disc.reduced (0.5f), 1.0f);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (9.5f).withKerningFactor (0.1f));
            g.drawFittedText (shortName (type), label.withTrimmedLeft (17).withTrimmedRight (2), juce::Justification::centredLeft, 1, 0.8f);

            if (! open)
                return;

            auto area = getValueArea();
            g.reduceClipRegion (r);

            // Beat lines, as in the notes above
            auto grid = host.getGrid();
            for (double b = std::floor (host.xToBeat ((float) area.getX()) / grid) * grid; host.beatToX (b) < (float) getWidth(); b += grid)
            {
                bool isBeat = std::abs (std::fmod (b, 1.0)) < 1.0e-6;
                g.setColour (isBeat ? Colours::gridBar.withAlpha (0.6f) : Colours::gridLine.withAlpha (0.6f));
                g.drawVerticalLine (juce::roundToInt (host.beatToX (b)), area.getY(), area.getBottom());
            }

            auto colour = host.getColour ? host.getColour() : Colours::accent;

            if (type == velocityType)
                paintVelocity (g, area, colour);
            else
                paintControllers (g, area, colour);

            // The value being set, beside the mouse
            if (readout.isNotEmpty())
            {
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
                auto box = juce::Rectangle<float> ((float) readoutPos.x + 10.0f, juce::jlimit (area.getY(), area.getBottom() - 14.0f, (float) readoutPos.y - 16.0f), 80.0f, 14.0f);
                g.drawText (readout, box, juce::Justification::centredLeft);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            readout = {};

            if (e.x < host.keyboardW)
            {
                if (e.y < closedHeight && e.x < 18)
                    return toggleOpen();

                return showTypeMenu();
            }

            if (! open)
                return toggleOpen();

            if (e.mods.isPopupMenu())
                return showLaneMenu();

            auto& seq = clip->getSequence();
            um().beginNewTransaction (type == velocityType ? "Velocity" : "Edit " + laneName (type));
            lastPos = e.position;

            if (type == velocityType)
            {
                grabbed = getStemAt ((float) e.x);
                grabStarts.clear();

                if (grabbed != nullptr)
                {
                    // Dragging a selected note's stem moves all the selected notes together
                    auto sel = host.getSelection();
                    grabNotes = sel.contains (grabbed) ? sel : juce::Array<te::MidiNote*> { grabbed };

                    for (auto n : grabNotes)
                        grabStarts.push_back (n->getVelocity());

                    setVelocity (*grabbed, valueAt ((float) e.y), &um());
                }
                else
                {
                    paintVelocities (seq, host.getSelection(), beatAt (e.position.x), valueAt (e.position.y),
                                     beatAt (e.position.x), valueAt (e.position.y), &um());
                }
            }
            else
            {
                movingEvent = getEventAt (e.position);

                if (movingEvent == nullptr && e.getNumberOfClicks() == 1)
                {
                    // A new point where you click, which the drag then draws on from
                    auto b = snappedBeat (e.position.x, e.mods);
                    drawControllers (seq, type, b, valueAt (e.position.y), b, valueAt (e.position.y), drawStep(), &um());
                    lastBeat = b;
                }
            }

            updateReadout (e.position);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! open || e.getMouseDownX() < host.keyboardW)
                return;

            auto& seq = clip->getSequence();

            if (type == velocityType)
            {
                if (grabbed != nullptr)
                {
                    // Relative for a selection, so its shape is kept
                    auto delta = valueAt (e.position.y) - valueAt ((float) e.getMouseDownY());

                    for (int i = 0; i < grabNotes.size(); ++i)
                        setVelocity (*grabNotes[i], grabNotes.size() == 1 ? valueAt (e.position.y) : grabStarts[(size_t) i] + delta, &um());
                }
                else
                {
                    paintVelocities (seq, host.getSelection(), beatAt (lastPos.x), valueAt (lastPos.y),
                                     beatAt (e.position.x), valueAt (e.position.y), &um());
                }
            }
            else if (movingEvent != nullptr)
            {
                movingEvent->setBeatPosition (te::BeatPosition::fromBeats (juce::jmax (0.0, snappedBeat (e.position.x, e.mods))), &um());
                movingEvent->setControllerValue (quantiseValue (type, valueAt (e.position.y)), &um());
            }
            else
            {
                auto b = juce::jmax (0.0, std::round (beatAt (e.position.x) / drawStep()) * drawStep());
                drawControllers (seq, type, lastBeat, valueAt (lastPos.y), b, valueAt (e.position.y), drawStep(), &um());
                lastBeat = b;
            }

            lastPos = e.position;
            updateReadout (e.position);
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            grabbed = nullptr;
            grabNotes.clear();
            movingEvent = nullptr;
            readout = {};
            repaint();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (type != velocityType && open)
                if (auto ev = getEventAt (e.position))
                    clip->getSequence().removeControllerEvent (*ev, &um());
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (host.wheel)
                host.wheel (e, w);
        }

    private:
        te::MidiClip::Ptr clip;
        Host host;
        int type = lastType();
        bool open = lastOpen();
        juce::Point<float> lastPos, readoutPos;
        double lastBeat = 0.0;
        juce::String readout;
        te::MidiNote* grabbed = nullptr;
        juce::Array<te::MidiNote*> grabNotes;
        std::vector<int> grabStarts;
        te::MidiControllerEvent* movingEvent = nullptr;

        juce::UndoManager& um()     { return clip->edit.getUndoManager(); }

        juce::Rectangle<float> getValueArea() const
        {
            return getLocalBounds().toFloat().withTrimmedLeft ((float) host.keyboardW).reduced (0.0f, 8.0f).withTrimmedTop (2.0f);
        }

        float valueToY (int v) const
        {
            auto a = getValueArea();
            return a.getBottom() - a.getHeight() * (float) (v - minValue (type)) / (float) (maxValue (type) - minValue (type));
        }

        int valueAt (float y) const
        {
            auto a = getValueArea();
            auto t = juce::jlimit (0.0f, 1.0f, (a.getBottom() - y) / a.getHeight());
            return minValue (type) + juce::roundToInt (t * (float) (maxValue (type) - minValue (type)));
        }

        double beatAt (float x) const   { return host.xToBeat (x); }

        double snappedBeat (float x, juce::ModifierKeys mods) const
        {
            return mods.isCommandDown() ? beatAt (x) : host.snap (beatAt (x));
        }

        /** Drawn controllers get a point every 1/32 of a beat at most, fewer when zoomed out. */
        double drawStep() const
        {
            auto pxPerBeat = host.beatToX (1.0) - host.beatToX (0.0);
            return juce::jmax (1.0 / 32.0, 6.0 / juce::jmax (1.0f, pxPerBeat));
        }

        te::MidiNote* getStemAt (float x) const
        {
            te::MidiNote* best = nullptr;
            float bestD = 5.0f;

            for (auto n : clip->getSequence().getNotes())
                if (auto d = std::abs (host.beatToX (n->getStartBeat().inBeats()) - x); d < bestD)
                    best = n, bestD = d;

            return best;
        }

        te::MidiControllerEvent* getEventAt (juce::Point<float> p) const
        {
            for (auto ev : getEvents (clip->getSequence(), type))
                if (juce::Point<float> (host.beatToX (ev->getBeatPosition().inBeats()), valueToY (ev->getControllerValue())).getDistanceFrom (p) < 6.0f)
                    return ev;

            return nullptr;
        }

        void updateReadout (juce::Point<float> p)
        {
            readoutPos = p;
            int v = valueAt (p.y);

            if (type == velocityType && grabbed != nullptr)
                v = grabbed->getVelocity();
            else if (movingEvent != nullptr)
                v = movingEvent->getControllerValue();

            readout = valueText (type, type == velocityType ? v : quantiseValue (type, v));
            repaint();
        }

        void paintVelocity (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
        {
            auto sel = host.getSelection();

            for (auto n : clip->getSequence().getNotes())
            {
                auto x = host.beatToX (n->getStartBeat().inBeats());

                if (x < area.getX() - 4.0f || x > area.getRight() + 4.0f)
                    continue;

                auto y = valueToY (n->getVelocity());
                const bool on = sel.contains (n) || n == grabbed;
                auto c = on ? Colours::text : colour;

                // A faint line for the note's length, then the stem and its circle
                auto x2 = host.beatToX (n->getEndBeat().inBeats());
                g.setColour (c.withAlpha (0.25f));
                g.drawHorizontalLine (juce::roundToInt (y), x, juce::jmax (x + 1.0f, x2 - 2.0f));
                g.setColour (c.withAlpha (on ? 1.0f : 0.8f));
                g.fillRect (juce::Rectangle<float> (x - 0.75f, y, 1.5f, area.getBottom() - y));
                auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y });
                g.setColour (Colours::background);
                g.fillEllipse (dot);
                g.setColour (c);
                g.drawEllipse (dot.reduced (0.5f), 1.5f);
            }
        }

        void paintControllers (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
        {
            if (type == pitchType)
            {
                g.setColour (Colours::textDim.withAlpha (0.4f));
                g.drawHorizontalLine (juce::roundToInt (valueToY (8192)), area.getX(), area.getRight());
            }

            auto events = getEvents (clip->getSequence(), type);

            if (events.isEmpty())
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText ("Click or drag to draw " + laneName (type).toLowerCase(), area.toNearestInt(), juce::Justification::centred);
                return;
            }

            // Each value holds until the next, then on to the end of the view
            juce::Path line, fill;
            auto baseY = valueToY (type == pitchType ? 8192 : 0);
            float x = host.beatToX (events.getFirst()->getBeatPosition().inBeats());
            float y = valueToY (events.getFirst()->getControllerValue());
            line.startNewSubPath (x, y);
            fill.startNewSubPath (x, baseY);
            fill.lineTo (x, y);

            for (int i = 1; i < events.size(); ++i)
            {
                auto nx = host.beatToX (events[i]->getBeatPosition().inBeats());
                auto ny = valueToY (events[i]->getControllerValue());
                line.lineTo (nx, y);
                line.lineTo (nx, ny);
                fill.lineTo (nx, y);
                fill.lineTo (nx, ny);
                y = ny;
            }

            line.lineTo (area.getRight(), y);
            fill.lineTo (area.getRight(), y);
            fill.lineTo (area.getRight(), baseY);
            fill.closeSubPath();

            g.setColour (colour.withAlpha (0.14f));
            g.fillPath (fill);
            g.setColour (colour);
            g.strokePath (line, juce::PathStrokeType (1.5f));

            // Circles where it changes, but only where they'd stand apart: a drawn curve stays a line
            float lastX = -1.0e6f;

            for (int i = 0; i < events.size(); ++i)
            {
                auto ev = events[i];
                auto p = juce::Point<float> (host.beatToX (ev->getBeatPosition().inBeats()), valueToY (ev->getControllerValue()));
                auto nextX = i + 1 < events.size() ? host.beatToX (events[i + 1]->getBeatPosition().inBeats()) : 1.0e6f;

                if (p.x < area.getX() - 4.0f || p.x > area.getRight() + 4.0f
                     || (ev != movingEvent && (p.x - lastX < 12.0f || nextX - p.x < 12.0f)))
                    continue;

                lastX = p.x;

                auto dot = juce::Rectangle<float> (7.0f, 7.0f).withCentre (p);
                g.setColour (Colours::background);
                g.fillEllipse (dot);
                g.setColour (ev == movingEvent ? Colours::text : colour);
                g.drawEllipse (dot.reduced (0.5f), 1.5f);
            }
        }

        void toggleOpen()
        {
            open = ! open;
            lastOpen() = open;

            if (onOpenChanged)
                onOpenChanged();
        }

        void showTypeMenu()
        {
            auto& seq = clip->getSequence();
            juce::PopupMenu m;
            auto item = [&] (juce::PopupMenu& menu, int t)
            {
                menu.addItem (laneName (t) + (t != velocityType && hasData (seq, t) ? juce::String::fromUTF8 ("  \xe2\x80\xa2") : juce::String()),
                              true, t == type, [this, t] { if (! open) toggleOpen(); setType (t); });
            };

            for (int t : { velocityType, pitchType, 1, 11, 64, pressureType })
                item (m, t);

            juce::PopupMenu more;
            for (int cc = 0; cc < 128; ++cc)
                if (cc != 1 && cc != 11 && cc != 64)
                    item (more, cc);

            m.addSubMenu ("More Controllers", more);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (getLocalBounds().withWidth (host.keyboardW).withHeight (closedHeight))));
        }

        void showLaneMenu()
        {
            juce::PopupMenu m;

            if (type == velocityType)
            {
                for (int v : { 127, 100, 80, 64 })
                    m.addItem ("Set " + juce::String (host.getSelection().isEmpty() ? "All" : "Selected") + " to " + juce::String (v), [this, v]
                    {
                        um().beginNewTransaction ("Velocity");
                        auto sel = host.getSelection();
                        for (auto n : sel.isEmpty() ? clip->getSequence().getNotes() : sel)
                            setVelocity (*n, v, &um());
                    });
            }
            else
            {
                m.addItem ("Clear " + laneName (type), hasData (clip->getSequence(), type), false, [this]
                {
                    um().beginNewTransaction ("Clear " + laneName (type));
                    for (auto ev : getEvents (clip->getSequence(), type))
                        clip->getSequence().removeControllerEvent (*ev, &um());
                });
            }

            m.showMenuAsync ({});
        }
    };
}

} // namespace elsewhere
