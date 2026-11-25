// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "../Live.h"
#include "EqDevice.h"
#include "CompressorDevice.h"
#include "ReverbDevice.h"
#include "DrumsDevice.h"
#include "AmpDevice.h"
#include "MasterDevice.h"
#include "MonoDevice.h"
#include "PolyDevice.h"
#include "ModularDevice.h"
#include "OrganicDevice.h"
#include "HazeDevice.h"
#include "MidiEffects.h"
#include "EchoDevice.h"

namespace elsewhere
{
//==============================================================================
const std::vector<DeviceRegistry::Entry>& DeviceRegistry::getAll()
{
    static const std::vector<Entry> all = {
        { DrumsDevice::xmlTypeName,      "Drums",      "16-pad drum machine with a built-in kit; drop samples on pads", true,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new DrumsDevice (i); } },
        { PolyDevice::xmlTypeName,       "Poly",       "Polyphonic analog-style synth: Prism, Quad and Velvet models, unison, chords, chorus", true,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new PolyDevice (i); } },
        { OrganicDevice::xmlTypeName,    "Organic",    "Recorded strings, winds, harp, mallets and textures, reshaped: grain clouds, a resonant body, deep expression", true,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new OrganicDevice (i); } },
        { HazeDevice::xmlTypeName,       "Haze",       "Nostalgic tape and VHS synth: detuned voices, wow and flutter, age, tracking glitches, chroma, hiss and a slow, dark haze", true,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new HazeDevice (i); } },
        { ModularDevice::xmlTypeName,    "Modular",    "Patch-cable modular synth: VCOs, filters, envelopes, sequencer, delay and more", true,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new ModularDevice (i); } },
        { MonoDevice::xmlTypeName,       "Mono",       "Monophonic analog-style synth: three oscillators, ladder filter, glide (Seventy)", true,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new MonoDevice (i); } },
        { ArpDevice::xmlTypeName,        "Arpeggiator", "Plays held notes as a pattern, in time with the song", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new ArpDevice (i); }, true },
        { StepsDevice::xmlTypeName,      "Step Sequencer", "Note sequencer of up to 32 steps, with chance, repeats, slides and lanes that go round at their own length", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new StepsDevice (i); }, true },
        { EqDevice::xmlTypeName,         "EQ",         "8-band parametric EQ with a spectrum analyzer", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new EqDevice (i); } },
        { CompressorDevice::xmlTypeName, "Compressor", "Compressor with Clean, VCA, FET, Opto, Vintage and Limiter models", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new CompressorDevice (i); } },
        { AmpDevice::xmlTypeName,        "Amp",        "Guitar and bass amp with cabinet and mic simulation", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new AmpDevice (i); } },
        { MasterDevice::xmlTypeName,     "Master",     "Mastering chain with an assistant: loudness, tonal balance, width, limiter", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new MasterDevice (i); } },
        { ReverbDevice::xmlTypeName,     "Reverb",     "Algorithmic reverb with early reflections, modulation and freeze", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new ReverbDevice (i); } },
        { EchoDevice::xmlTypeName,       "Tape Echo",  "Tape loop echo with three playback heads, wow and flutter, and wear that makes every repeat decay differently", false,
          [] (te::PluginCreationInfo i) -> te::Plugin::Ptr { return new EchoDevice (i); } },
    };

    return all;
}

//==============================================================================
te::Plugin::Ptr ElsewhereEngineBehaviour::createCustomPlugin (te::PluginCreationInfo info)
{
    auto type = info.state[te::IDs::type].toString();

    if (type == Live::tapPluginType)
        return Live::createTapPlugin (info);

    for (auto& d : DeviceRegistry::getAll())
        if (d.type == type)
            return d.create (info);

    return {};
}

} // namespace elsewhere
