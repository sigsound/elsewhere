// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"
#include "SpaceDsp.h"
#include "OrganicEngine.h"
#include "../SoundLibrary.h"

namespace elsewhere
{
//==============================================================================
/**
    The device side of Elsewhere Organic's sound: its sources as SoundLibrary keeps them,
    and the names the panel shows. The sound itself (grain clouds, the tuned body, the
    built-in sources) is OrganicEngine, which knows nothing of JUCE.
*/
namespace OrganicDsp
{
    using CloudSettings = OrganicEngine::CloudSettings;

    /** The tuned body, with its names for the panel. */
    struct Body : OrganicEngine::Body
    {
        static juce::StringArray names()    { return { "String", "Tube", "Bowl", "Glass", "Wood" }; }
    };

    /**
        A SoundLibrary source as the engine plays it: the engine's view of the same audio
        (pointers into the source's buffers), kept alive together with the source.
    */
    struct Playable
    {
        SoundLibrary::SourcePtr lib;
        std::vector<OrganicEngine::Zone> zones;
        OrganicEngine::Source source;

        explicit Playable (SoundLibrary::SourcePtr s) : lib (std::move (s))
        {
            if (lib == nullptr)
                return;
            zones.reserve (lib->zones.size());
            for (auto& z : lib->zones)
            {
                OrganicEngine::Zone e;
                e.root = z.root; e.layer = z.layer; e.take = z.take;
                e.data[0] = z.audio.getReadPointer (0);
                e.data[1] = z.audio.getReadPointer (z.audio.getNumChannels() > 1 ? 1 : 0);
                e.length = z.audio.getNumSamples();
                zones.push_back (e);
            }
            source.zones = zones.data();
            source.numZones = (int) zones.size();
            source.rate = lib->rate;
            source.layers = lib->layers;
            source.takes = lib->takes;
            source.gain = lib->gain;
            source.sustains = lib->sustains();
            source.pitched = lib->pitched;
        }
    };

    //==============================================================================
    /** Sources built into Elsewhere Organic (so it plays without any pack installed). */
    namespace Builtins
    {
        using Info = OrganicEngine::BuiltinInfo;
        inline const std::vector<Info>& list()      { return OrganicEngine::builtins(); }

        /** A substitute for a pack source that isn't installed. */
        inline juce::String fallbackFor (const juce::String& family)
        {
            return juce::String ("builtin/") + OrganicEngine::fallbackFor (family.toRawUTF8());
        }

        /** A built-in source as a SoundLibrary source; its zones refer to the engine-built audio it keeps. */
        struct LibrarySource : SoundLibrary::Source
        {
            std::unique_ptr<OrganicEngine::BuiltSource> built;
        };

        inline SoundLibrary::SourcePtr make (const juce::String& id)
        {
            auto built = OrganicEngine::buildSource (id.toRawUTF8());
            if (built == nullptr)
                return {};

            auto src = std::make_shared<LibrarySource>();
            src->key = "builtin/" + id;
            src->name = juce::String (built->name);
            src->family = juce::String (built->family);
            src->rate = built->source.rate;
            src->pitched = built->source.pitched;
            for (size_t i = 0; i < built->zones.size(); ++i)
            {
                SoundLibrary::Zone z;
                z.root = built->zones[i].root;
                z.layer = built->zones[i].layer;
                z.take = built->zones[i].take;
                float* channels[2] = { built->audio[i][0].data(), built->audio[i][1].data() };
                z.audio.setDataToReferTo (channels, 2, built->zones[i].length);
                src->zones.push_back (std::move (z));
            }
            src->built = std::move (built);
            src->finish();
            return src;
        }
    }
}

} // namespace elsewhere
