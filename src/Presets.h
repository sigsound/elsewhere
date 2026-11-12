// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "LegacyNames.h"

namespace elsewhere
{
//==============================================================================
/** Device presets and project templates, stored under ~/Music/Elsewhere Projects. */
namespace Library
{
    inline juce::File getRoot()
    {
        auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        return Legacy::preferExisting (music.getChildFile ("Elsewhere Projects"), music.getChildFile ("Tandem Projects"));
    }

    inline juce::File getTemplatesFolder()      { return getRoot().getChildFile ("Templates"); }
    inline juce::File getDefaultTemplate()
    {
        auto f = getTemplatesFolder().getChildFile ("Default.elsewhere");
        auto legacy = f.withFileExtension (Legacy::projectExtension);
        return (! f.existsAsFile() && legacy.existsAsFile()) ? legacy : f;
    }

    //==============================================================================
    // Device presets
    inline constexpr const char* presetExtension = ".elsewherepreset";

    /** Presets are grouped per device, e.g. Presets/Reverb or Presets/Serum. */
    inline juce::File getPresetFolder (te::Plugin& p)
    {
        auto key = p.getName();

        if (auto ext = dynamic_cast<te::ExternalPlugin*> (&p))
            key = ext->desc.manufacturerName + " " + ext->desc.name;

        return getRoot().getChildFile ("Presets").getChildFile (juce::File::createLegalFileName (key.trim()));
    }

    inline juce::Array<juce::File> getPresets (te::Plugin& p)
    {
        auto files = getPresetFolder (p).findChildFiles (juce::File::findFiles, false,
                                                         juce::String ("*") + presetExtension + ";*" + Legacy::presetExtension);
        files.sort();
        return files;
    }

    inline bool savePreset (te::Plugin& p, const juce::String& name)
    {
        p.flushPluginStateToValueTree();
        auto folder = getPresetFolder (p);
        folder.createDirectory();

        auto v = p.state.createCopy();

        // Built-in devices don't store values that are at their default, so record every
        // parameter explicitly; otherwise loading the preset couldn't reset those knobs
        juce::ValueTree params ("ELSEWHERE_PARAMS");
        for (auto param : p.getAutomatableParameters())
        {
            juce::ValueTree item ("PARAM");
            item.setProperty ("id", param->paramID, nullptr);
            item.setProperty ("value", param->getCurrentValue(), nullptr);
            params.appendChild (item, nullptr);
        }
        v.appendChild (params, nullptr);

        if (auto xml = v.createXml())
            return xml->writeTo (folder.getChildFile (juce::File::createLegalFileName (name) + presetExtension));

        return false;
    }

    inline bool loadPreset (te::Plugin& p, const juce::File& f)
    {
        auto xml = juce::XmlDocument::parse (f);

        if (xml == nullptr)
            return false;

        auto v = Legacy::upgradeIdentifiers (juce::ValueTree::fromXml (*xml));

        if (! v.isValid() || v[te::IDs::type].toString() != p.state[te::IDs::type].toString())
            return false;

        auto pluginState = v.createCopy();
        pluginState.removeChild (pluginState.getChildWithName ("ELSEWHERE_PARAMS"), nullptr);

        p.edit.getUndoManager().beginNewTransaction ("Load preset");

        if (dynamic_cast<te::ExternalPlugin*> (&p) != nullptr
             || dynamic_cast<te::FourOscPlugin*> (&p) != nullptr
             || dynamic_cast<te::SamplerPlugin*> (&p) != nullptr)
        {
            p.restorePluginStateFromValueTree (pluginState);
        }
        else
        {
            // Built-in effects keep their settings as properties on the plugin state
            for (int i = 0; i < v.getNumProperties(); ++i)
            {
                auto id = v.getPropertyName (i);

                if (id != te::IDs::id && id != te::IDs::type && ! id.toString().startsWith ("window"))
                    p.state.setProperty (id, v[id], &p.edit.getUndoManager());
            }
        }

        if (dynamic_cast<te::ExternalPlugin*> (&p) == nullptr)
        {
            auto params = v.getChildWithName ("ELSEWHERE_PARAMS");

            for (auto param : p.getAutomatableParameters())
            {
                auto item = params.getChildWithProperty ("id", param->paramID);

                if (item.isValid())
                    param->setParameter ((float) item["value"], juce::sendNotification);
            }
        }

        return true;
    }

    //==============================================================================
    // Project templates

    /** Removes audio clips (whose files wouldn't exist in a new project) from an edit's state. */
    inline void stripAudioClips (juce::ValueTree v)
    {
        for (int i = v.getNumChildren(); --i >= 0;)
        {
            auto child = v.getChild (i);

            if (child.hasType (te::IDs::AUDIOCLIP))
                v.removeChild (i, nullptr);
            else
                stripAudioClips (child);
        }
    }

    inline bool saveTemplate (const juce::File& savedEditFile, const juce::File& destination)
    {
        auto xml = juce::XmlDocument::parse (savedEditFile);

        if (xml == nullptr)
            return false;

        auto v = juce::ValueTree::fromXml (*xml);
        stripAudioClips (v);
        destination.getParentDirectory().createDirectory();

        if (auto out = v.createXml())
            return out->writeTo (destination);

        return false;
    }

    inline juce::Array<juce::File> getTemplates()
    {
        auto files = getTemplatesFolder().findChildFiles (juce::File::findFiles, false, Legacy::projectWildcard (".elsewhere"));
        files.sort();
        return files;
    }
}

} // namespace elsewhere
