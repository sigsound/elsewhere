// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Mixing.h"

namespace elsewhere
{
//==============================================================================
/**
    Where audio goes beyond the plain track-to-main path: tracks playing into a bus, tracks
    with no output (a kick that only keys a compressor) and sidechains, where a device hears
    another track to decide what to do.

    The engine builds one audio graph, so a route that would loop back on itself (a track
    keying a compressor on a track it plays into, say) is never offered. dependsOn() is the
    check, following every way one track's audio reaches another: outputs, groups, sends to
    returns and sidechains.
*/
namespace Routing
{
    inline const juce::Identifier busProp { "elsewhereBus" };

    /** The tracks whose audio this one takes in directly. */
    inline juce::Array<te::Track*> getInputs (te::Track& t)
    {
        juce::Array<te::Track*> inputs;
        auto& edit = t.edit;

        // Tracks playing into it, and a group's own tracks
        for (auto other : te::getAllTracks (edit))
        {
            if (other == &t)
                continue;

            if (auto at = dynamic_cast<te::AudioTrack*> (other))
                if (auto dest = dynamic_cast<te::AudioTrack*> (&t); dest != nullptr && at->getOutput().outputsToDestTrack (*dest))
                    inputs.addIfNotAlreadyThere (other);

            if (other->getParentTrack() == &t && other->isPartOfSubmix())
                inputs.addIfNotAlreadyThere (other);
        }

        // Sidechain keys and sends to the returns it carries
        for (auto p : t.pluginList)
        {
            if (auto id = p->getSidechainSourceID(); id.isValid())
                if (auto src = te::findTrackForID (edit, id))
                    inputs.addIfNotAlreadyThere (src);

            if (auto ret = dynamic_cast<te::AuxReturnPlugin*> (p))
                for (auto other : te::getAllTracks (edit))
                    if (other != &t && Mixing::findSend (*other, ret->busNumber) != nullptr)
                        inputs.addIfNotAlreadyThere (other);
        }

        return inputs;
    }

    /** True if any of b's audio reaches a, by any route. */
    inline bool dependsOn (te::Track& a, te::Track& b)
    {
        juce::Array<te::Track*> seen, todo { &a };

        while (! todo.isEmpty())
        {
            auto t = todo.removeAndReturn (todo.size() - 1);

            for (auto in : getInputs (*t))
            {
                if (in == &b)
                    return true;

                if (! seen.contains (in))
                {
                    seen.add (in);
                    todo.add (in);
                }
            }
        }

        return false;
    }

    //==============================================================================
    /** Tracks playing straight into this one. */
    inline juce::Array<te::AudioTrack*> getFeeders (te::AudioTrack& bus)
    {
        juce::Array<te::AudioTrack*> feeders;

        for (auto t : te::getAudioTracks (bus.edit))
            if (t != &bus && t->getOutput().outputsToDestTrack (bus))
                feeders.add (t);

        return feeders;
    }

    inline bool isBus (te::AudioTrack& t)       { return t.state[busProp] || ! getFeeders (t).isEmpty(); }

    /** A track can play into another unless it is the master, in a group (it plays through the
        group), or the other already reaches it. */
    inline bool canOutputTo (te::AudioTrack& t, te::AudioTrack& dest)
    {
        return &t != &dest && ! t.isPartOfSubmix() && ! dependsOn (t, dest);
    }

    inline void setOutput (te::AudioTrack& t, te::AudioTrack* dest)
    {
        if (dest != nullptr && ! canOutputTo (t, *dest))
            return;

        t.edit.getUndoManager().beginNewTransaction (dest != nullptr ? "Output to " + dest->getName() : "Output to Main");

        if (dest != nullptr)
            t.getOutput().setOutputToTrack (dest);
        else
            t.getOutput().setOutputToDefaultDevice (false);
    }

    inline void setNoOutput (te::AudioTrack& t)
    {
        t.edit.getUndoManager().beginNewTransaction ("No output");
        t.getOutput().setOutputToNone();
    }

    /** A new, empty track to gather others: "Bus", "Bus 2"... before the returns. */
    inline te::AudioTrack* createBus (te::Edit& edit)
    {
        juce::StringArray names;
        te::Track* preceding = nullptr;

        for (auto t : te::getAllTracks (edit))
        {
            names.add (t->getName());

            if (! Mixing::isReturnTrack (*t) && t->getParentTrack() == nullptr && ! t->isMasterTrack()
                 && (t->isAudioTrack() || t->isFolderTrack()))
                preceding = t;
        }

        auto name = juce::String ("Bus");
        for (int i = 2; names.contains (name); ++i)
            name = "Bus " + juce::String (i);

        auto bus = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr);

        if (bus == nullptr)
            return nullptr;

        bus->setName (name);
        bus->setColour (juce::Colour (0xff9a9da3));
        bus->state.setProperty (busProp, true, nullptr);
        Mixing::ensureSends (*bus);
        return bus.get();
    }

    /** "→ Bus", "No Output", or empty when it plays to Main. */
    inline juce::String describeOutput (te::AudioTrack& t)
    {
        auto& out = t.getOutput();

        if (auto dest = out.getDestinationTrack())
            return juce::String::fromUTF8 ("\xe2\x86\x92 ") + dest->getName();

        if (out.outputsToNone())
            return "No Output";

        return {};
    }

    //==============================================================================
    /** Devices with a key input, on a track (the master's chain plays after every track). */
    inline bool canSidechain (te::Plugin& p)
    {
        auto owner = p.getOwnerTrack();
        return owner != nullptr && ! owner->isMasterTrack() && p.canSidechain();
    }

    inline te::AudioTrack* getSidechainSource (te::Plugin& p)
    {
        if (auto id = p.getSidechainSourceID(); id.isValid())
            return dynamic_cast<te::AudioTrack*> (te::findTrackForID (p.edit, id));

        return nullptr;
    }

    /** Set to a track that has since been deleted (undo brings it back). */
    inline bool hasMissingSidechain (te::Plugin& p)
    {
        return p.getSidechainSourceID().isValid() && getSidechainSource (p) == nullptr;
    }

    inline bool canSidechainFrom (te::Plugin& p, te::AudioTrack& source)
    {
        auto owner = p.getOwnerTrack();
        return owner != nullptr && owner != &source && ! dependsOn (source, *owner);
    }

    /** Keys the device from a track's output (after its fader), or nothing. */
    inline void setSidechain (te::Plugin& p, te::AudioTrack* source)
    {
        if (source != nullptr && ! canSidechainFrom (p, *source))
            return;

        auto& um = p.edit.getUndoManager();
        um.beginNewTransaction (source != nullptr ? "Sidechain from " + source->getName() : "No sidechain");

        // Fresh wires each time: the track's own channels, then the key's
        if (auto wires = p.state.getChildWithName (te::IDs::SIDECHAINCONNECTIONS); wires.isValid())
            p.state.removeChild (wires, &um);

        if (source == nullptr)
        {
            p.setSidechainSourceID ({});
            return;
        }

        p.setSidechainSourceID (source->itemID);
        p.guessSidechainRouting();
    }

    /** Devices keyed from this track. */
    inline juce::Array<te::Plugin*> getSidechainTargets (te::Track& source)
    {
        juce::Array<te::Plugin*> targets;

        for (auto p : source.edit.getPluginCache().getPlugins())
            if (p->getSidechainSourceID() == source.itemID && p->getOwnerTrack() != nullptr)
                targets.add (p);

        return targets;
    }
}

} // namespace elsewhere
