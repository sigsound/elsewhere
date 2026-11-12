// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Widgets.h"
#include "devices/DeviceBase.h"

namespace elsewhere
{
//==============================================================================
/**
    Modulators: an LFO that moves any knob on its track (Elsewhere devices, mixer controls,
    VST3/AU plug-in parameters). Built on the engine's modifiers, so modulation runs in
    the audio engine, is sample-accurate enough for filter sweeps, saves with the project
    and syncs to collaborators like everything else.
*/
namespace Modulators
{
    /** Elsewhere's wave choices, as indices into te::LFOModifier::Wave. */
    inline const std::vector<std::pair<juce::String, int>>& waves()
    {
        static const std::vector<std::pair<juce::String, int>> w = {
            { "Sine", te::LFOModifier::waveSine }, { "Triangle", te::LFOModifier::waveTriangle },
            { "Saw Up", te::LFOModifier::waveSawUp }, { "Saw Down", te::LFOModifier::waveSawDown },
            { "Square", te::LFOModifier::waveSquare }, { "Random", te::LFOModifier::random },
            { "Smooth", te::LFOModifier::noise } };
        return w;
    }

    /** Keeps a bipolar LFO centred on the knob whatever the depth (the engine's LFO adds
        its offset before converting to bipolar). */
    inline void centre (te::LFOModifier& lfo)
    {
        auto d = lfo.depthParam->getCurrentBaseValue();
        auto bipolar = lfo.bipolarParam->getCurrentBaseValue() > 0.5f;
        lfo.offsetParam->setParameter (bipolar ? (1.0f - d) * 0.5f : 0.0f, juce::sendNotification);
    }

    inline te::LFOModifier::Ptr addLFO (te::Track& track)
    {
        if (track.getModifierList() == nullptr)
            return {};

        auto m = track.getModifierList()->insertModifier (juce::ValueTree (te::IDs::LFO), -1, nullptr);
        te::LFOModifier::Ptr lfo (dynamic_cast<te::LFOModifier*> (m.get()));

        if (lfo != nullptr)
        {
            // Sensible start: a 1-bar sine synced to the song, swinging both ways around the knob
            lfo->rateTypeParam->setParameter ((float) te::ModifierCommon::bar, juce::sendNotification);
            lfo->rateParam->setParameter (1.0f, juce::sendNotification);
            lfo->syncTypeParam->setParameter ((float) te::ModifierCommon::transport, juce::sendNotification);
            lfo->depthParam->setParameter (1.0f, juce::sendNotification);
            lfo->bipolarParam->setParameter (1.0f, juce::sendNotification);
            lfo->phaseParam->setParameter (0.0f, juce::sendNotification);
            centre (*lfo);
        }

        return lfo;
    }

    struct Target
    {
        te::AutomatableParameter::Ptr param;
        te::AutomatableParameter::ModifierAssignment::Ptr assignment;
    };

    /** Everything on the track this LFO is moving. */
    inline std::vector<Target> getTargets (te::Track& track, te::Modifier& mod)
    {
        std::vector<Target> result;

        for (auto p : track.pluginList)
            for (auto param : p->getAutomatableParameters())
                for (auto a : param->getAssignments())
                    if (a->isForModifierSource (mod))
                        result.push_back ({ param, a });

        return result;
    }

    inline juce::String describe (te::AutomatableParameter& p)
    {
        auto plugin = p.getPlugin();
        return (plugin != nullptr ? plugin->getName().replace ("Elsewhere ", "") + ": " : juce::String()) + p.getParameterName();
    }

    /** True if the modifier is allowed to drive this parameter (same track, not itself). */
    inline bool canTarget (te::Track& track, te::Modifier& mod, te::AutomatableParameter& p)
    {
        auto plugin = p.getPlugin();
        if (plugin == nullptr || plugin->getOwnerTrack() != &track)
            return false;

        for (auto own : mod.getAutomatableParameters())
            if (own == &p)
                return false;

        return true;
    }
}

//==============================================================================
/** Wave display with the LFO's current position. */
class LfoDisplay : public juce::Component, private juce::Timer
{
public:
    LfoDisplay (te::LFOModifier& l) : lfo (&l)  { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 3.0f);
        r.reduce (4, 6);

        const auto colour = Colours::modulation;
        const int wave = juce::roundToInt (lfo->waveParam->getCurrentValue());
        const bool bipolar = lfo->bipolarParam->getCurrentValue() > 0.5f;
        const float depth = lfo->depthParam->getCurrentValue();

        auto shape = [&] (float ph) -> float
        {
            switch (wave)
            {
                case te::LFOModifier::waveSine:     return 0.5f + 0.5f * std::sin (ph * juce::MathConstants<float>::twoPi);
                case te::LFOModifier::waveTriangle: return 1.0f - std::abs (2.0f * ph - 1.0f);
                case te::LFOModifier::waveSawUp:    return ph;
                case te::LFOModifier::waveSawDown:  return 1.0f - ph;
                case te::LFOModifier::waveSquare:   return ph < 0.5f ? 1.0f : 0.0f;
                default:                            return 0.5f + 0.4f * std::sin (ph * 13.0f) * std::cos (ph * 7.0f);
            }
        };

        auto toY = [&] (float v)   // v: LFO output in its own units (-1..1 or 0..1)
        {
            auto norm = bipolar ? (v + 1.0f) * 0.5f : v;
            return r.getBottom() - r.getHeight() * juce::jlimit (0.0f, 1.0f, norm);
        };
        auto out = [&] (float s) { return bipolar ? depth * (2.0f * s - 1.0f) : depth * s; };

        g.setColour (Colours::gridBar);
        g.drawHorizontalLine ((int) toY (0.0f), r.getX(), r.getRight());

        juce::Path p;
        for (int i = 0; i <= 100; ++i)
        {
            auto ph = (float) i / 100.0f;
            auto pt = juce::Point<float> (r.getX() + r.getWidth() * ph, toY (out (shape (ph))));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (colour.withAlpha (0.8f));
        g.strokePath (p, juce::PathStrokeType (1.5f));

        // Live position
        auto phase = lfo->getCurrentPhase();
        auto pos = juce::Point<float> (r.getX() + r.getWidth() * phase, toY (lfo->getCurrentValue()));
        g.setColour (Colours::text);
        g.fillEllipse (juce::Rectangle<float> (7, 7).withCentre (pos));
    }

private:
    te::LFOModifier::Ptr lfo;
    void timerCallback() override   { repaint(); }
};

//==============================================================================
/** The LFO in the device chain. */
class LfoPanel : public juce::Component,
                 private juce::Timer
{
public:
    LfoPanel (te::Track& t, te::LFOModifier& l)
        : track (&t), lfo (&l), display (l),
          rate (*l.rateParam, "Hz", colour()), depth (*l.depthParam, "Depth", colour()), phase (*l.phaseParam, "Phase", colour()),
          bipolar (*l.bipolarParam, "Bipolar", colour())
    {
        for (auto& [name, w] : Modulators::waves())
        {
            auto* b = waveButtons.add (new juce::TextButton (name));
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonOnColourId, colour().withAlpha (0.5f));
            b->onClick = [this, value = w]
            {
                lfo->waveParam->setParameter ((float) value, juce::sendNotification);
                refresh();
            };
            addAndMakeVisible (b);
        }

        rateType.addItemList (te::ModifierCommon::getRateTypeChoices(), 1);
        rateType.changeItemText (1, "Hz (free)");
        rateType.onChange = [this]
        {
            lfo->rateTypeParam->setParameter ((float) (rateType.getSelectedId() - 1), juce::sendNotification);
            if (rateType.getSelectedId() > 1)
                lfo->rateParam->setParameter (1.0f, juce::sendNotification);
            refresh();
        };
        addAndMakeVisible (rateType);

        syncType.addItem ("Free running", 1);
        syncType.addItem ("Locked to song", 2);
        syncType.addItem ("Restart on note", 3);
        syncType.onChange = [this] { lfo->syncTypeParam->setParameter ((float) (syncType.getSelectedId() - 1), juce::sendNotification); };
        syncType.setTooltip ("Free: runs continuously. Locked to song: always in the same place at the same bar. "
                             "Restart on note: starts from the beginning with each MIDI note on this track.");
        addAndMakeVisible (syncType);

        for (juce::Component* c : { (juce::Component*) &display, (juce::Component*) &rate, (juce::Component*) &depth,
                                    (juce::Component*) &phase, (juce::Component*) &bipolar })
            addAndMakeVisible (c);

        mapButton.setClickingTogglesState (false);
        mapButton.setTooltip ("Click Map, then click any knob on this track to have the LFO move it");
        mapButton.onClick = [this] { toggleMapping(); };
        addAndMakeVisible (mapButton);

        power.setClickingTogglesState (true);
        power.setColour (juce::TextButton::buttonOnColourId, colour());
        power.onClick = [this] { lfo->enabledParam->setParameter (power.getToggleState() ? 1.0f : 0.0f, juce::sendNotification); };
        addAndMakeVisible (power);

        menuButton.onClick = [this]
        {
            juce::PopupMenu m;
            m.addItem ("Delete LFO", [this]
            {
                ModulationMap::cancel();
                juce::MessageManager::callAsync ([toRemove = lfo] { toRemove->remove(); });
            });
            m.showMenuAsync ({});
        };
        addAndMakeVisible (menuButton);

        rebuildTargets();
        refresh();
        startTimerHz (4);
    }

    ~LfoPanel() override
    {
        if (mapping)
            ModulationMap::cancel();
    }

    static juce::Colour colour()    { return Colours::modulation; }
    static constexpr int idealWidth = 620;

    std::function<void()> onChanged;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (r, 3.0f);

        auto title = r.removeFromTop (22.0f);
        g.setColour (Colours::panelLight);
        g.fillRoundedRectangle (title, 3.0f);
        g.setColour (colour());
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("LFO", title.toNearestInt().withTrimmedLeft (26), juce::Justification::centredLeft);

        // Targets section
        DeviceUI::paintSection (g, targetsArea, "Moves");

        if (rows.isEmpty())
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawFittedText (mapping ? "Now click a knob on this track..." : "Click Map, then click a knob to modulate it.",
                              targetsArea.reduced (8).withTrimmedTop (22), juce::Justification::centred, 3);
        }

        g.setColour (Colours::outline);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 3.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto title = r.removeFromTop (22).reduced (3);
        power.setBounds (title.removeFromLeft (16));
        menuButton.setBounds (title.removeFromRight (20));
        r.reduce (6, 4);

        targetsArea = r.removeFromRight (220);
        auto t = targetsArea.reduced (6, 2);
        mapButton.setBounds (t.getRight() - 56, t.getY(), 56, 16);
        t.removeFromTop (20);
        for (auto* row : rows)
            row->setBounds (t.removeFromTop (24));
        r.removeFromRight (8);

        auto left = r.removeFromLeft (180);
        auto waveRow = left.removeFromBottom (44);
        auto bw = waveRow.getWidth() / 4;
        for (int i = 0; i < waveButtons.size(); ++i)
            waveButtons[i]->setBounds (waveRow.getX() + (i % 4) * bw, waveRow.getY() + (i / 4) * 22, bw - 1, 21);
        left.removeFromBottom (4);
        display.setBounds (left);
        r.removeFromLeft (8);

        rateType.setBounds (r.removeFromTop (22));
        r.removeFromTop (3);
        syncType.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        auto knobs = r;
        auto kw = knobs.getWidth() / 4;
        rate.setBounds (knobs.removeFromLeft (kw));
        depth.setBounds (knobs.removeFromLeft (kw));
        phase.setBounds (knobs.removeFromLeft (kw));
        bipolar.setBounds (knobs.withSizeKeepingCentre (kw - 4, 22));
    }

private:
    te::Track::Ptr track;
    te::LFOModifier::Ptr lfo;
    LfoDisplay display;
    DeviceUI::Knob rate, depth, phase;
    DeviceUI::Toggle bipolar;
    juce::OwnedArray<juce::TextButton> waveButtons;
    juce::ComboBox rateType, syncType;
    juce::TextButton mapButton { "Map" }, power, menuButton { juce::String::fromUTF8 ("\xe2\x8b\xaf") };
    juce::Rectangle<int> targetsArea;
    bool mapping = false;
    size_t lastTargetCount = 0;
    float lastDepth = -1.0f, lastBipolar = -1.0f;

    //==============================================================================
    struct TargetRow : public juce::Component
    {
        TargetRow (Modulators::Target t) : target (std::move (t))
        {
            name.setText (Modulators::describe (*target.param), juce::dontSendNotification);
            name.setFont (juce::FontOptions (11.0f));
            name.setColour (juce::Label::textColourId, Colours::text);
            name.setMinimumHorizontalScale (0.7f);
            addAndMakeVisible (name);

            amount.setSliderStyle (juce::Slider::LinearBar);
            amount.setRange (-100.0, 100.0, 1.0);
            amount.setValue (target.assignment->value.get() * 100.0, juce::dontSendNotification);
            amount.setTextValueSuffix (" %");
            amount.setDoubleClickReturnValue (true, 50.0);
            amount.setColour (juce::Slider::trackColourId, LfoPanel::colour().withAlpha (0.5f));
            amount.onValueChange = [this] { target.assignment->value = (float) amount.getValue() / 100.0f; };
            amount.setTooltip ("How far the LFO moves this knob (negative = opposite direction)");
            addAndMakeVisible (amount);

            remove.onClick = [this]
            {
                juce::MessageManager::callAsync ([tgt = target] { tgt.param->removeModifier (*tgt.assignment); });
            };
            remove.setTooltip ("Stop modulating this");
            addAndMakeVisible (remove);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (0, 2);
            remove.setBounds (r.removeFromRight (18));
            r.removeFromRight (3);
            amount.setBounds (r.removeFromRight (70));
            r.removeFromRight (4);
            name.setBounds (r);
        }

        Modulators::Target target;
        juce::Label name;
        juce::Slider amount;
        juce::TextButton remove { "x" };
    };

    juce::OwnedArray<TargetRow> rows;

    void rebuildTargets()
    {
        rows.clear();
        auto targets = Modulators::getTargets (*track, *lfo);
        lastTargetCount = targets.size();

        for (auto& t : targets)
            addAndMakeVisible (rows.add (new TargetRow (t)));

        resized();
        repaint();
    }

    void toggleMapping()
    {
        if (mapping)
        {
            ModulationMap::cancel();
            mapping = false;
            refresh();
            return;
        }

        mapping = true;
        ModulationMap::begin ("LFO", [sp = SafePointer<LfoPanel> (this)] (te::AutomatableParameter& p)
        {
            if (sp == nullptr) return;
            sp->mapping = false;

            if (Modulators::canTarget (*sp->track, *sp->lfo, p))
            {
                if (Modulators::getTargets (*sp->track, *sp->lfo).size() < 16)
                {
                    bool already = false;
                    for (auto a : p.getAssignments())
                        already = already || a->isForModifierSource (*sp->lfo);
                    if (! already)
                        p.addModifier (*sp->lfo, 0.5f);
                }
            }
            else
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "LFO",
                    "An LFO can move knobs on its own track. Add an LFO to that track to modulate it.");
            }

            sp->rebuildTargets();
            sp->refresh();
        });

        refresh();
    }

    void refresh()
    {
        const int w = juce::roundToInt (lfo->waveParam->getCurrentBaseValue());
        const auto& ws = Modulators::waves();
        for (int i = 0; i < waveButtons.size(); ++i)
            waveButtons[i]->setToggleState (ws[(size_t) i].second == w, juce::dontSendNotification);

        rateType.setSelectedId (juce::roundToInt (lfo->rateTypeParam->getCurrentBaseValue()) + 1, juce::dontSendNotification);
        syncType.setSelectedId (juce::roundToInt (lfo->syncTypeParam->getCurrentBaseValue()) + 1, juce::dontSendNotification);
        rate.setEnabled (rateType.getSelectedId() == 1);
        rate.setAlpha (rate.isEnabled() ? 1.0f : 0.35f);
        power.setToggleState (lfo->enabledParam->getCurrentBaseValue() > 0.5f, juce::dontSendNotification);

        mapButton.setButtonText (mapping ? "Cancel" : "Map");
        mapButton.setColour (juce::TextButton::buttonColourId, mapping ? colour().withAlpha (0.6f) : Colours::panelLight);
        repaint();
    }

    void timerCallback() override
    {
        // Keep bipolar centred as depth changes
        auto d = lfo->depthParam->getCurrentBaseValue(), b = lfo->bipolarParam->getCurrentBaseValue();
        if (std::abs (d - lastDepth) > 1.0e-5f || std::abs (b - lastBipolar) > 1.0e-5f)
        {
            if (lastDepth >= 0.0f)
                Modulators::centre (*lfo);
            lastDepth = d; lastBipolar = b;
        }

        // Targets added/removed elsewhere (undo, a collaborator, deleting a device)
        if (Modulators::getTargets (*track, *lfo).size() != lastTargetCount)
            rebuildTargets();

        if (mapping != ModulationMap::isActive())
        {
            mapping = ModulationMap::isActive() && mapping;
            refresh();
        }
    }
};

} // namespace elsewhere
