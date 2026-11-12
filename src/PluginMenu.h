// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "devices/DeviceBase.h"
#include "Modulators.h"

namespace elsewhere
{
//==============================================================================
/** Builds the "add device" popup: built-in instruments/effects plus every
    scanned VST3/AU plugin, grouped by manufacturer. */
namespace PluginMenu
{
    struct BuiltIn
    {
        juce::String name, xmlType;
        bool isInstrument;
    };

    inline std::vector<BuiltIn> getBuiltIns()
    {
        return {
            { "4OSC Synth",   te::FourOscPlugin::xmlTypeName,     true  },
            { "Sampler",      te::SamplerPlugin::xmlTypeName,     true  },
            { "Simple EQ (4 band)",   te::EqualiserPlugin::xmlTypeName,   false },
            { "Simple Compressor",    te::CompressorPlugin::xmlTypeName,  false },
            { "Simple Reverb",        te::ReverbPlugin::xmlTypeName,      false },
            { "Delay",        te::DelayPlugin::xmlTypeName,       false },
            { "Chorus",       te::ChorusPlugin::xmlTypeName,      false },
            { "Phaser",       te::PhaserPlugin::xmlTypeName,      false },
            { "Low/High Pass",te::LowPassPlugin::xmlTypeName,     false },
            { "Pitch Shift",  te::PitchShiftPlugin::xmlTypeName,  false },
            { "Utility (Vol/Pan)", te::VolumeAndPanPlugin::xmlTypeName, false },
        };
    }

    using Callback = std::function<void (te::Plugin::Ptr)>;

    inline te::Plugin::Ptr createExternal (te::Edit& edit, const juce::PluginDescription& d)
    {
        return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, d);
    }

    inline void addExternalGroup (juce::PopupMenu& menu, te::Edit& edit, bool instruments, const Callback& cb)
    {
        auto& list = edit.engine.getPluginManager().knownPluginList;
        std::map<juce::String, juce::PopupMenu> byVendor;

        auto types = list.getTypes();
        std::sort (types.begin(), types.end(), [] (auto& a, auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });

        for (const auto& d : types)
        {
            if (d.isInstrument != instruments)
                continue;

            auto vendor = d.manufacturerName.isNotEmpty() ? d.manufacturerName : juce::String ("Unknown");
            auto label = d.name + "  (" + d.pluginFormatName + ")";
            byVendor[vendor].addItem (label, [&edit, d, cb] { if (auto p = createExternal (edit, d)) cb (p); });
        }

        if (byVendor.empty())
            menu.addItem ("No plugins found. Use Preferences > Plugins to scan.", false, false, [] {});

        for (auto& [vendor, sub] : byVendor)
            menu.addSubMenu (vendor, sub);
    }

    inline juce::PopupMenu build (te::Edit& edit, Callback cb)
    {
        juce::PopupMenu m, instruments, midiEffects, effects, vstInstruments, vstEffects;

        // Elsewhere's own devices first, then the engine's basic ones
        for (auto& d : DeviceRegistry::getAll())
        {
            if (d.hidden)
                continue;

            auto type = d.type;
            juce::PopupMenu::Item item (d.name);
            item.action = [&edit, type, cb] { if (auto p = DeviceRegistry::create (edit, type)) cb (p); };
            item.setColour (Colours::accent);
            (d.isMidiEffect ? midiEffects : d.isInstrument ? instruments : effects).addItem (item);
        }

        instruments.addSeparator();
        effects.addSeparator();

        for (auto& b : getBuiltIns())
        {
            auto item = [&edit, b, cb]
            {
                if (auto p = edit.getPluginCache().createNewPlugin (b.xmlType, {}))
                    cb (p);
            };

            (b.isInstrument ? instruments : effects).addItem (b.name, item);
        }

        addExternalGroup (vstInstruments, edit, true, cb);
        addExternalGroup (vstEffects, edit, false, cb);

        m.addSectionHeader ("Elsewhere Devices");
        m.addSubMenu ("Instruments", instruments);
        m.addSubMenu ("MIDI Effects", midiEffects);
        m.addSubMenu ("Audio Effects", effects);
        m.addSectionHeader ("Plug-ins (VST3 / AU)");
        m.addSubMenu ("Instruments", vstInstruments);
        m.addSubMenu ("Audio Effects", vstEffects);
        return m;
    }

    /** Shows the menu and inserts the chosen plugin into the track's chain at the given index (-1 = end, before the mixer). */
    inline void showForTrack (te::Track& track, int index, juce::Component* target = nullptr, std::function<void (te::Plugin&)> onAdded = {})
    {
        auto trackRef = te::Track::Ptr (&track);

        auto menu = build (track.edit, [trackRef, index, onAdded] (te::Plugin::Ptr p)
        {
            auto& list = trackRef->pluginList;
            int insertIndex = index;

            if (insertIndex < 0)
            {
                // Insert before the track's own volume/meter plugins (and the master's meter and
                // live-session tap), so they stay at the end of the chain
                insertIndex = list.size();

                for (int i = 0; i < list.size(); ++i)
                {
                    if (EngineHelpers::isMixerPlugin (*list[i]))
                    {
                        insertIndex = i;
                        break;
                    }
                }
            }

            // Instruments go first in the chain (after a return's input, if any)
            if (p->isSynth())
                insertIndex = 0;

            // MIDI effects go just before the instrument they feed
            if (DeviceRegistry::isMidiEffect (p->getPluginType()))
            {
                insertIndex = 0;
                for (int i = 0; i < list.size(); ++i)
                    if (list[i]->isSynth()) { insertIndex = i; break; }
            }

            if (insertIndex == 0 && list.size() > 0 && dynamic_cast<te::AuxReturnPlugin*> (list[0]) != nullptr)
                insertIndex = 1;

            list.insertPlugin (p, insertIndex, nullptr);

            if (onAdded)
                onAdded (*p);
        });

        // Modulators live on the track rather than in the plugin chain
        if (track.getModifierList() != nullptr)
        {
            menu.addSectionHeader ("Modulators");
            juce::PopupMenu::Item lfo ("LFO (move any knob on this track)");
            lfo.action = [trackRef] { Modulators::addLFO (*trackRef); };
            lfo.setColour (Colours::modulation);
            menu.addItem (lfo);
        }

        menu.showMenuAsync (target != nullptr ? juce::PopupMenu::Options().withTargetComponent (target)
                                              : juce::PopupMenu::Options());
    }
}

} // namespace elsewhere
