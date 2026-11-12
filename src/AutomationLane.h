// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Mixing.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
namespace Automation
{
    /** Every automatable parameter on a track, grouped for a popup menu. */
    inline void addParameterItems (juce::PopupMenu& m, te::Track& track, std::function<void (te::AutomatableParameter&)> choose)
    {
        auto addParam = [&choose] (juce::PopupMenu& menu, te::AutomatableParameter* p, const juce::String& label)
        {
            auto ptr = te::AutomatableParameter::Ptr (p);
            menu.addItem (label + (p->hasAutomationPoints() ? "  \xe2\x80\xa2" : ""), [ptr, choose] { choose (*ptr); });
        };

        if (auto vp = Mixing::findVolume (track))
        {
            addParam (m, vp->volParam.get(), "Volume");
            addParam (m, vp->panParam.get(), "Pan");
        }

        for (auto p : track.pluginList)
        {
            if (auto send = dynamic_cast<te::AuxSendPlugin*> (p))
            {
                addParam (m, send->gain.get(), "Send " + Mixing::busLetter (send->getBusNumber()));
                continue;
            }

            if (dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr || p == Mixing::findVolume (track)
                 || dynamic_cast<te::AuxReturnPlugin*> (p) != nullptr)
                continue;

            juce::PopupMenu sub;
            for (auto param : p->getAutomatableParameters())
                addParam (sub, param, param->getParameterName());

            if (sub.getNumItems() > 0)
                m.addSubMenu (p->getName(), sub);
        }
    }

    inline te::AutomatableParameter* getDefaultParameter (te::Track& track)
    {
        // Prefer a parameter that already has automation
        for (auto p : track.getAllAutomatableParams())
            if (p->hasAutomationPoints())
                return p;

        if (auto vp = Mixing::findVolume (track))
            return vp->volParam.get();

        return nullptr;
    }
}

//==============================================================================
/** Left part of an automation row: which parameter is shown. */
class AutomationLaneHeader : public juce::Component
{
public:
    AutomationLaneHeader (EditViewState& evs, te::Track& t) : viewState (evs), track (&t)
    {
        paramButton.onClick = [this]
        {
            juce::PopupMenu m;
            Automation::addParameterItems (m, *track, [this] (te::AutomatableParameter& p)
            {
                viewState.automationLanes[track->itemID] = &p;
                viewState.sendChangeMessage();
            });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&paramButton));
        };
        paramButton.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
        addAndMakeVisible (paramButton);

        clearButton.setTooltip ("Delete all automation for this parameter");
        clearButton.onClick = [this]
        {
            if (auto p = getParam())
            {
                track->edit.getUndoManager().beginNewTransaction ("Clear automation");
                p->getCurve().clear (&track->edit.getUndoManager());
            }
        };
        addAndMakeVisible (clearButton);
        refresh();
    }

    te::AutomatableParameter* getParam() const
    {
        auto it = viewState.automationLanes.find (track->itemID);
        return it != viewState.automationLanes.end() ? it->second.get() : nullptr;
    }

    void refresh()
    {
        if (auto p = getParam())
        {
            auto owner = p->getPlugin();
            auto label = p->getParameterName();

            if (owner != nullptr && owner != Mixing::findVolume (*track) && dynamic_cast<te::AuxSendPlugin*> (owner) == nullptr)
                label = owner->getName() + ": " + label;
            else if (auto send = dynamic_cast<te::AuxSendPlugin*> (owner))
                label = "Send " + Mixing::busLetter (send->getBusNumber());

            paramButton.setButtonText (label);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::lift (Colours::panelDark, 0.03f));
        g.setColour (Colours::outline);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 6).withTrimmedLeft (juce::jmax (0, track->getTrackDepth()) * 12 + 6);
        auto top = r.removeFromTop (20);
        clearButton.setBounds (top.removeFromRight (40));
        top.removeFromRight (4);
        paramButton.setBounds (top);
    }

private:
    EditViewState& viewState;
    te::Track::Ptr track;
    juce::TextButton paramButton, clearButton { "Clear" };
};

//==============================================================================
/** Draws and edits a parameter's automation curve along the timeline. */
class AutomationLane : public juce::Component, private juce::Timer
{
public:
    AutomationLane (EditViewState& evs, te::Track& t) : viewState (evs), track (&t)
    {
        startTimerHz (20);
    }

    te::AutomatableParameter* getParam() const
    {
        auto it = viewState.automationLanes.find (track->itemID);
        return it != viewState.automationLanes.end() ? it->second.get() : nullptr;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background.darker (0.15f));
        auto param = getParam();

        if (param == nullptr)
            return;

        auto colour = Theme::ofTrack (*track);
        auto& curve = param->getCurve();
        auto num = curve.getNumPoints();

        // Grid lines at 25% steps
        g.setColour (Colours::gridLine);
        for (int i = 1; i < 4; ++i)
            g.drawHorizontalLine (juce::roundToInt (valueArea().getY() + valueArea().getHeight() * (float) i / 4.0f), 0.0f, (float) getWidth());

        if (num == 0)
        {
            // Flat line at the current value
            auto y = valueToY (param->getCurrentValue());
            g.setColour (colour.withAlpha (0.5f));
            g.drawHorizontalLine (juce::roundToInt (y), 0.0f, (float) getWidth());
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("Click to add automation points", getLocalBounds().reduced (8, 4), juce::Justification::topLeft);
        }
        else
        {
            // Sample the curve across the visible width so curves/shapes draw correctly
            juce::Path p;
            for (int x = 0; x <= getWidth(); x += 2)
            {
                auto t = viewState.xToTime ((float) x);
                auto y = valueToY (curve.getValueAt (t, param->getCurrentValue()));
                if (x == 0) p.startNewSubPath ((float) x, y); else p.lineTo ((float) x, y);
            }

            juce::Path fill (p);
            fill.lineTo ((float) getWidth(), (float) getHeight());
            fill.lineTo (0.0f, (float) getHeight());
            fill.closeSubPath();
            g.setColour (colour.withAlpha (0.12f));
            g.fillPath (fill);

            g.setColour (colour);
            g.strokePath (p, juce::PathStrokeType (1.5f));

            for (int i = 0; i < num; ++i)
            {
                auto pt = getPointPosition (i);
                g.setColour (i == draggingIndex ? Colours::text : colour.brighter (0.3f));
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (pt));
                g.setColour (Colours::panelDark);
                g.drawEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (pt), 1.0f);
            }
        }

        // Value readout while dragging
        if (draggingIndex >= 0 && draggingIndex < num)
        {
            auto pt = getPointPosition (draggingIndex);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (param->valueToString (curve.getPointValue (draggingIndex)),
                        juce::Rectangle<float> (pt.x + 6.0f, juce::jlimit (0.0f, (float) getHeight() - 14.0f, pt.y - 16.0f), 90.0f, 14.0f).toNearestInt(),
                        juce::Justification::centredLeft);
        }

        g.setColour (Colours::outline);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto param = getParam();
        if (param == nullptr)
            return;

        auto& um = track->edit.getUndoManager();
        um.beginNewTransaction ("Edit automation");
        draggingIndex = findPointNear (e.position);

        if (draggingIndex < 0)
        {
            auto time = viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) e.x), e.mods.isCommandDown()));
            draggingIndex = param->getCurve().addPoint (time, yToValue ((float) e.y), 0.0f, &um);
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto param = getParam();
        if (param == nullptr || draggingIndex < 0)
            return;

        auto time = viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) e.x), e.mods.isCommandDown()));
        draggingIndex = param->getCurve().movePoint (*param, draggingIndex, time, yToValue ((float) e.y), false, &track->edit.getUndoManager());
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        draggingIndex = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto param = getParam();
        if (param == nullptr)
            return;

        auto index = findPointNear (e.position);

        if (index >= 0)
        {
            param->getCurve().removePoint (index, &track->edit.getUndoManager());
            draggingIndex = -1;
            repaint();
        }
    }

private:
    EditViewState& viewState;
    te::Track::Ptr track;
    int draggingIndex = -1;

    juce::Rectangle<float> valueArea() const    { return getLocalBounds().toFloat().reduced (0.0f, 5.0f); }

    float valueToY (float v) const
    {
        auto param = getParam();
        auto range = param->getValueRange();
        auto norm = range.getLength() > 0.0f ? (v - range.getStart()) / range.getLength() : 0.0f;
        auto a = valueArea();
        return a.getBottom() - juce::jlimit (0.0f, 1.0f, norm) * a.getHeight();
    }

    float yToValue (float y) const
    {
        auto param = getParam();
        auto range = param->getValueRange();
        auto a = valueArea();
        auto norm = juce::jlimit (0.0f, 1.0f, (a.getBottom() - y) / a.getHeight());
        return range.getStart() + norm * range.getLength();
    }

    juce::Point<float> getPointPosition (int index) const
    {
        auto& curve = getParam()->getCurve();
        return { viewState.timeToX (curve.getPointTime (index)), valueToY (curve.getPointValue (index)) };
    }

    int findPointNear (juce::Point<float> pos) const
    {
        auto param = getParam();
        if (param == nullptr)
            return -1;

        for (int i = param->getCurve().getNumPoints(); --i >= 0;)
            if (getPointPosition (i).getDistanceFrom (pos) < 6.0f)
                return i;

        return -1;
    }

    void timerCallback() override
    {
        // Repaint while automation might be recording or the value is moving
        if (track->edit.getTransport().isPlaying() || isMouseButtonDown())
            repaint();
    }
};

} // namespace elsewhere
