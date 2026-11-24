// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Return tracks, sends and group tracks.

    A return track is an audio track carrying an AuxReturnPlugin for one bus, tagged with
    the `elsewhereReturnBus` property. Every other track gets a post-fader AuxSendPlugin for
    each bus, starting silent, and the mixer shows those sends as knobs (A, B, ...).
*/
namespace Mixing
{
    inline const juce::Identifier returnBusProp { "elsewhereReturnBus" };

    inline int getReturnBus (const te::Track& t)
    {
        return t.state.hasProperty (returnBusProp) ? (int) t.state[returnBusProp] : -1;
    }

    inline bool isReturnTrack (const te::Track& t)      { return getReturnBus (t) >= 0; }

    inline juce::String busLetter (int bus)             { return juce::String::charToString ((juce::juce_wchar) ('A' + bus)); }

    inline juce::Array<te::AudioTrack*> getReturnTracks (te::Edit& edit)
    {
        juce::Array<te::AudioTrack*> result;

        for (auto t : te::getAudioTracks (edit))
            if (isReturnTrack (*t))
                result.add (t);

        return result;
    }

    inline juce::Array<int> getReturnBuses (te::Edit& edit)
    {
        juce::Array<int> buses;

        for (auto t : getReturnTracks (edit))
            buses.addIfNotAlreadyThere (getReturnBus (*t));

        buses.sort();
        return buses;
    }

    inline te::AuxSendPlugin* findSend (te::Track& t, int bus)
    {
        for (auto p : t.pluginList)
            if (auto s = dynamic_cast<te::AuxSendPlugin*> (p))
                if (s->getBusNumber() == bus)
                    return s;

        return nullptr;
    }

    /** Makes sure the track has a (silent, post-fader) send for every return bus. */
    inline void ensureSends (te::Track& t)
    {
        if (isReturnTrack (t) || t.isMasterTrack() || ! (t.isAudioTrack() || t.isFolderTrack()))
            return;

        // Tracks inside a group send via the group
        if (t.isPartOfSubmix())
            return;

        for (auto bus : getReturnBuses (t.edit))
        {
            if (findSend (t, bus) != nullptr)
                continue;

            if (auto p = t.edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {}))
            {
                if (auto send = dynamic_cast<te::AuxSendPlugin*> (p.get()))
                {
                    send->busNumber = bus;
                    send->setGainDb (-100.0f);
                }

                // After the fader (post-fader send), before the meter
                int index = t.pluginList.size();

                for (int i = 0; i < t.pluginList.size(); ++i)
                    if (dynamic_cast<te::LevelMeterPlugin*> (t.pluginList[i]) != nullptr)
                        index = i;

                t.pluginList.insertPlugin (p, index, nullptr);
            }
        }
    }

    inline void ensureSendsOnAllTracks (te::Edit& edit)
    {
        for (auto t : te::getAllTracks (edit))
            ensureSends (*t);
    }

    /** Creates return track A, B, C... with a reverb or delay already loaded. */
    inline te::AudioTrack* createReturnTrack (te::Edit& edit)
    {
        auto buses = getReturnBuses (edit);
        int bus = 0;
        while (buses.contains (bus))
            ++bus;

        if (bus >= 8)
            return nullptr;

        // Returns live at the end of the track list
        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, te::getAllTracks (edit).getLast()), nullptr);

        if (track == nullptr)
            return nullptr;

        bool isReverb = bus % 2 == 0;
        auto name = busLetter (bus) + " " + (isReverb ? "Reverb" : "Delay");
        track->state.setProperty (returnBusProp, bus, nullptr);
        track->setName (name);
        track->setColour (juce::Colour (0xff8b8e94));
        edit.setAuxBusName (bus, name);

        if (auto ret = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {}))
        {
            if (auto r = dynamic_cast<te::AuxReturnPlugin*> (ret.get()))
                r->busNumber = bus;

            track->pluginList.insertPlugin (ret, 0, nullptr);
        }

        if (auto fx = edit.getPluginCache().createNewPlugin (isReverb ? te::ReverbPlugin::xmlTypeName : te::DelayPlugin::xmlTypeName, {}))
        {
            track->pluginList.insertPlugin (fx, 1, nullptr);

            // Returns should be 100% wet
            for (auto param : fx->getAutomatableParameters())
            {
                auto n = param->getParameterName().toLowerCase();

                if (n.contains ("wet") || n.contains ("mix"))
                    param->setParameter (param->getValueRange().getEnd(), juce::sendNotification);
                else if (n.contains ("dry"))
                    param->setParameter (param->getValueRange().getStart(), juce::sendNotification);
            }
        }

        ensureSendsOnAllTracks (edit);
        return track.get();
    }

    //==============================================================================
    /** Wraps the given tracks in a new submix group track, inserted where the first one was. */
    inline te::FolderTrack* groupTracks (te::Edit& edit, juce::Array<te::Track*> tracks)
    {
        tracks.removeIf ([] (te::Track* t) { return t->isMasterTrack() || isReturnTrack (*t) || ! (t->isAudioTrack() || t->isFolderTrack()); });

        if (tracks.isEmpty())
            return nullptr;

        // Keep the on-screen order
        auto all = te::getAllTracks (edit);
        std::sort (tracks.begin(), tracks.end(), [&all] (auto* a, auto* b) { return all.indexOf (a) < all.indexOf (b); });

        auto first = tracks.getFirst();
        auto allIndex = all.indexOf (first);
        te::Track* preceding = nullptr;

        for (int i = allIndex; --i >= 0;)
            if (all[i]->getParentTrack() == first->getParentTrack())
            {
                preceding = all[i];
                break;
            }

        auto folder = edit.insertNewFolderTrack (te::TrackInsertPoint (first->getParentTrack(), preceding), nullptr, true);

        if (folder == nullptr)
            return nullptr;

        folder->setName ("Group");
        folder->setColour (first->getColour());

        te::Track* last = nullptr;

        for (auto t : tracks)
        {
            edit.moveTrack (t, te::TrackInsertPoint (folder.get(), last));
            last = t;

            // Children send through the group now
            for (auto p : t->pluginList.getPlugins())
                if (dynamic_cast<te::AuxSendPlugin*> (p) != nullptr)
                    p->deleteFromParent();
        }

        ensureSends (*folder);
        return folder.get();
    }

    /** Moves a group's tracks back out and deletes the group. */
    inline void ungroup (te::Edit& edit, te::FolderTrack& folder)
    {
        auto children = folder.getAllSubTracks (false);
        te::Track* preceding = &folder;

        for (auto t : children)
        {
            edit.moveTrack (t, te::TrackInsertPoint (folder.getParentTrack(), preceding));
            preceding = t;
            ensureSends (*t);
        }

        edit.deleteTrack (&folder);
    }

    //==============================================================================
    /** Where dragged tracks land: just before or after another track (alongside it, so in its
        group if it has one), or into a group, after the tracks already in it. */
    enum class TrackDrop { before, after, into };

    /** The tracks of a drag that can move: tracks and groups, not returns or the master, and not a
        track whose group is moving too (it goes with its group). In on-screen order. */
    inline juce::Array<te::Track*> movableTracks (te::Edit& edit, juce::Array<te::Track*> tracks)
    {
        const auto all = tracks;

        tracks.removeIf ([&all] (te::Track* t)
        {
            if (t == nullptr || t->isMasterTrack() || isReturnTrack (*t) || ! (t->isAudioTrack() || t->isFolderTrack()))
                return true;

            for (auto p = t->getParentTrack(); p != nullptr; p = p->getParentTrack())
                if (all.contains (p))
                    return true;

            return false;
        });

        auto order = te::getAllTracks (edit);
        std::sort (tracks.begin(), tracks.end(), [&order] (auto* a, auto* b) { return order.indexOf (a) < order.indexOf (b); });
        return tracks;
    }

    /** Can these tracks go there? Not onto or into themselves, and not among the returns (just
        before the first return is fine: that's the end of the other tracks). */
    inline bool canMoveTracks (te::Edit& edit, const juce::Array<te::Track*>& tracks, te::Track& target, TrackDrop where)
    {
        auto moving = movableTracks (edit, tracks);

        if (moving.isEmpty() || target.isMasterTrack() || ! (target.isAudioTrack() || target.isFolderTrack()))
            return false;

        if (isReturnTrack (target))
        {
            auto top = te::getTopLevelTracks (edit);
            auto before = top[top.indexOf (&target) - 1];

            if (where != TrackDrop::before || (before != nullptr && isReturnTrack (*before)))
                return false;
        }

        if (where == TrackDrop::into && ! target.isFolderTrack())
            return false;

        for (auto t = &target; t != nullptr; t = t->getParentTrack())
            if (moving.contains (t))
                return false;

        return true;
    }

    /** Moves tracks (keeping their order) and keeps their sends right: a track that joins a group
        sends through the group, and one that leaves a group gets its own sends back. */
    inline void moveTracks (te::Edit& edit, const juce::Array<te::Track*>& tracks, te::Track& target, TrackDrop where)
    {
        if (! canMoveTracks (edit, tracks, target, where))
            return;

        auto moving = movableTracks (edit, tracks);

        // A track leaving one list of tracks for another lives on only while something holds it
        juce::ReferenceCountedArray<te::Track> keep;
        for (auto t : moving)
            keep.add (t);

        auto parent = where == TrackDrop::into ? &target : target.getParentTrack();
        auto siblings = parent != nullptr ? parent->getAllSubTracks (false) : te::getTopLevelTracks (edit);

        // The track they go after, skipping any that are themselves moving
        int from = where == TrackDrop::into ? siblings.size() - 1
                 : siblings.indexOf (&target) - (where == TrackDrop::before ? 1 : 0);
        te::Track* preceding = nullptr;

        for (int i = from; i >= 0; --i)
            if (! moving.contains (siblings[i]))
            {
                preceding = siblings[i];
                break;
            }

        for (auto t : moving)
        {
            const bool wasGrouped = t->isPartOfSubmix();
            edit.moveTrack (t, te::TrackInsertPoint (parent, preceding));
            preceding = t;

            if (t->isPartOfSubmix())
            {
                if (! wasGrouped)
                    for (auto p : t->pluginList.getPlugins())
                        if (dynamic_cast<te::AuxSendPlugin*> (p) != nullptr)
                            p->deleteFromParent();
            }
            else
            {
                ensureSends (*t);
            }
        }
    }

    /** A click on a track's solo button. Solos add up, so soloing a
        guitar and then its return plays both. Cmd-click solos just this track (or, when it's
        already the only one, un-solos it). */
    inline void clickSolo (te::Track& track)
    {
        if (! juce::ModifierKeys::currentModifiers.isCommandDown())
        {
            track.setSolo (! track.isSolo (false));
            return;
        }

        bool onlyThisOne = track.isSolo (false);

        for (auto other : te::getAllTracks (track.edit))
        {
            if (other != &track && other->isSolo (false))
            {
                onlyThisOne = false;
                other->setSolo (false);
            }
        }

        track.setSolo (! onlyThisOne);
    }

    inline te::LevelMeterPlugin* findMeter (te::Track& t)
    {
        for (auto p : t.pluginList)
            if (auto m = dynamic_cast<te::LevelMeterPlugin*> (p))
                return m;

        return nullptr;
    }

    /** Every send on a track, in bus order. */
    inline juce::Array<te::AuxSendPlugin*> getSends (te::Track& t)
    {
        juce::Array<te::AuxSendPlugin*> sends;
        for (auto p : t.pluginList)
            if (auto s = dynamic_cast<te::AuxSendPlugin*> (p))
                sends.add (s);
        std::sort (sends.begin(), sends.end(), [] (auto* a, auto* b) { return a->getBusNumber() < b->getBusNumber(); });
        return sends;
    }

    inline te::VolumeAndPanPlugin* findVolume (te::Track& t);

    /** A pre-fader send sits before the track's fader, so the fader doesn't change how much is sent. */
    inline bool isPreFader (te::Track& t, te::AuxSendPlugin& send)
    {
        auto vol = findVolume (t);
        return vol != nullptr && t.pluginList.indexOf (&send) < t.pluginList.indexOf (vol);
    }

    inline void setPreFader (te::Track& t, te::AuxSendPlugin& send, bool pre)
    {
        auto vol = findVolume (t);
        if (vol == nullptr || isPreFader (t, send) == pre)
            return;

        te::Plugin::Ptr keep (&send);
        t.edit.getUndoManager().beginNewTransaction (pre ? "Pre-fader send" : "Post-fader send");
        send.removeFromParent();

        int index = t.pluginList.indexOf (vol);
        if (! pre)
        {
            // Straight after the fader (before the meter), like the sends ensureSends adds
            index = t.pluginList.size();
            for (int i = 0; i < t.pluginList.size(); ++i)
                if (dynamic_cast<te::LevelMeterPlugin*> (t.pluginList[i]) != nullptr)
                    index = i;
        }

        t.pluginList.insertPlugin (keep, index, nullptr);
    }

    inline te::VolumeAndPanPlugin* findVolume (te::Track& t)
    {
        if (auto at = dynamic_cast<te::AudioTrack*> (&t))
            return at->getVolumePlugin();

        if (auto ft = dynamic_cast<te::FolderTrack*> (&t))
            return ft->getVolumePlugin();

        return nullptr;
    }
}

} // namespace elsewhere
