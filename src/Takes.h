// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/**
    Takes and comping.

    Every recording on an audio track is kept as a take, and so is every pass of a loop
    recording. Takes live with the track, in its state:

        TRACK
          ELSEWHERE_TAKES  cid="takes" next="4"
            TAKE  cid="..." number="1" source="Samples/Recorded/Vox_Take_1.wav" start="8.0" length="4.0" offset="0"
            TAKE  ...

    so they're saved, undone and synced to collaborators like everything else. The newest take
    goes on the track. Comping copies part of a take onto the track as an ordinary clip,
    replacing what was there, so a comp is just clips: it plays, edits, exports and syncs
    exactly as any other audio does, and the takes stay on hand to change your mind.

    A take's `start` and `length` are where it was recorded on the timeline; `offset` is how far
    into its file that is.
*/
namespace Takes
{
    inline const juce::Identifier takesType { "ELSEWHERE_TAKES" }, takeType { "TAKE" },
                                  numberProp { "number" }, nextProp { "next" }, sourceProp { "source" },
                                  startProp { "start" }, lengthProp { "length" }, offsetProp { "offset" };
    inline const juce::Identifier cidProp { "cid" };     // Collab's string IDs for Elsewhere's own items

    struct Take
    {
        juce::ValueTree state;
        int number = 0;
        juce::File file;
        te::TimeRange range;
        te::TimeDuration offset;

        /** Where the start of the file sits on the timeline. */
        te::TimePosition origin() const     { return range.getStart() - offset; }
    };

    inline juce::ValueTree getTree (te::Track& track, bool create)
    {
        auto tree = track.state.getChildWithName (takesType);

        if (! tree.isValid() && create)
        {
            tree = juce::ValueTree (takesType, { { cidProp, "takes" }, { nextProp, 1 } });
            track.state.appendChild (tree, &track.edit.getUndoManager());
        }

        return tree;
    }

    inline std::vector<Take> getTakes (te::Track& track)
    {
        std::vector<Take> takes;

        for (auto t : getTree (track, false))
        {
            if (! t.hasType (takeType))
                continue;

            Take take;
            take.state = t;
            take.number = t[numberProp];
            take.file = te::SourceFileReference::findFileFromString (track.edit, t[sourceProp].toString());
            take.range = { te::TimePosition::fromSeconds ((double) t[startProp]),
                           te::TimeDuration::fromSeconds ((double) t[lengthProp]) };
            take.offset = te::TimeDuration::fromSeconds ((double) t[offsetProp]);
            takes.push_back (take);
        }

        return takes;
    }

    inline int getNumTakes (te::Track& track)
    {
        int n = 0;
        for (auto t : getTree (track, false))
            n += t.hasType (takeType) ? 1 : 0;
        return n;
    }

    inline Take addTake (te::Track& track, const juce::File& file, te::TimeRange range, te::TimeDuration offset)
    {
        auto um = &track.edit.getUndoManager();
        auto tree = getTree (track, true);
        int number = juce::jmax (1, (int) tree[nextProp]);
        tree.setProperty (nextProp, number + 1, um);

        juce::ValueTree t (takeType, { { cidProp, juce::Uuid().toDashedString() },
                                       { numberProp, number },
                                       { sourceProp, te::SourceFileReference::findPathFromFile (track.edit, file, true) },
                                       { startProp, range.getStart().inSeconds() },
                                       { lengthProp, range.getLength().inSeconds() },
                                       { offsetProp, offset.inSeconds() } });
        tree.appendChild (t, um);
        return { t, number, file, range, offset };
    }

    /** Removes the take from the list. Its audio file is left alone, as a comp may still use it. */
    inline void removeTake (te::Track& track, const Take& take)
    {
        getTree (track, false).removeChild (take.state, &track.edit.getUndoManager());
    }

    //==============================================================================
    /** Lays takes out in lanes: each goes in the first lane where it doesn't overlap another, so
        the passes of one loop recording stack up while takes in different places share a lane.
        Returns the lane of each take (in the order given) and sets numLanes. */
    inline std::vector<int> packLanes (const std::vector<Take>& takes, int& numLanes)
    {
        std::vector<std::vector<te::TimeRange>> lanes;
        std::vector<int> laneOf;

        for (auto& take : takes)
        {
            size_t lane = 0;

            for (; lane < lanes.size(); ++lane)
                if (std::none_of (lanes[lane].begin(), lanes[lane].end(), [&] (auto r) { return r.overlaps (take.range); }))
                    break;

            if (lane == lanes.size())
                lanes.emplace_back();

            lanes[lane].push_back (take.range);
            laneOf.push_back ((int) lane);
        }

        numLanes = (int) lanes.size();
        return laneOf;
    }

    inline int getNumLanes (te::Track& track)
    {
        int n = 0;
        packLanes (getTakes (track), n);
        return n;
    }

    /** The parts of a take that are on the track now (clips playing the same file, lined up with it). */
    inline juce::Array<te::TimeRange> getUsedRanges (te::ClipTrack& track, const Take& take)
    {
        juce::Array<te::TimeRange> used;

        for (auto c : track.getClips())
        {
            auto wc = dynamic_cast<te::WaveAudioClip*> (c);

            if (wc == nullptr || wc->getOriginalFile() != take.file || wc->getSpeedRatio() != 1.0)
                continue;

            auto pos = wc->getPosition();

            if (std::abs ((pos.getStart() - pos.getOffset() - take.origin()).inSeconds()) > 0.001)
                continue;

            if (auto r = pos.time.getIntersectionWith (take.range); r.getLength() > 0.001s)
                used.add (r);
        }

        return used;
    }

    //==============================================================================
    /** Puts part of a take on the track, replacing whatever was there. */
    inline te::Clip* comp (te::AudioTrack& track, const Take& take, te::TimeRange range, te::SelectionManager* sm = nullptr)
    {
        range = range.getIntersectionWith (take.range);

        if (range.getLength() < 0.01s || ! take.file.existsAsFile())
            return nullptr;

        track.deleteRegion (range, sm);

        auto clip = track.insertWaveClip ("Take " + juce::String (take.number), take.file,
                                          { range, take.offset + (range.getStart() - take.range.getStart()) }, false);
        return clip.get();
    }

    inline te::Clip* useWholeTake (te::AudioTrack& track, const Take& take, te::SelectionManager* sm = nullptr)
    {
        return comp (track, take, take.range, sm);
    }

    /** Takes no part of which is on the track. */
    inline std::vector<Take> getUnusedTakes (te::AudioTrack& track)
    {
        std::vector<Take> unused;

        for (auto& take : getTakes (track))
            if (getUsedRanges (track, take).isEmpty())
                unused.push_back (take);

        return unused;
    }

    //==============================================================================
    /** Turns what the engine just recorded onto a track into takes: one per loop pass, or one for
        an ordinary recording. The newest goes on the track, replacing what was under it (the
        earlier passes and recordings stay in the take lanes). Returns the number of takes made. */
    inline int absorbRecording (te::AudioTrack& track, te::WaveAudioClip& clip)
    {
        auto& edit = track.edit;
        auto pos = clip.getPosition();
        juce::Array<juce::File> files;

        // A loop recording comes back as one clip holding a take per pass
        for (auto t : clip.state.getChildWithName (te::IDs::TAKES))
            if (auto f = te::SourceFileReference::findFileFromString (edit, t[te::IDs::source].toString()); f.existsAsFile())
                files.addIfNotAlreadyThere (f);

        if (files.isEmpty())
            files.add (clip.getOriginalFile());

        Take newest;

        for (auto& f : files)
        {
            // The last pass can be cut short when recording stops part way round
            auto fileLength = te::TimeDuration::fromSeconds (te::AudioFile (edit.engine, f).getLength());
            auto length = juce::jmin (pos.getLength(), fileLength - pos.getOffset());

            if (length > 0.1s)
                newest = addTake (track, f, { pos.getStart(), length }, pos.getOffset());
        }

        if (! newest.state.isValid())
            return 0;

        clip.removeFromParent();
        useWholeTake (track, newest);
        return (int) files.size();
    }

    //==============================================================================
    /** Listens for recordings finishing and makes them into takes. */
    class Recorder : private te::TransportControl::Listener
    {
    public:
        Recorder (EditViewState& evs) : viewState (evs)     { viewState.edit.getTransport().addListener (this); }
        ~Recorder() override                                { viewState.edit.getTransport().removeListener (this); }

    private:
        EditViewState& viewState;

        void recordingFinished (te::InputDeviceInstance&, te::EditItemID targetID, const juce::ReferenceCountedArray<te::Clip>& clips) override
        {
            auto& edit = viewState.edit;
            auto track = te::findAudioTrackForID (edit, targetID);

            if (track == nullptr)
                return;     // recorded into a Session slot: those stay as they are

            // After the engine has finished with its new clips
            juce::MessageManager::callAsync ([this, alive = std::weak_ptr<bool> (alive), trackRef = te::AudioTrack::Ptr (track), clips]
            {
                if (alive.expired())
                    return;

                for (auto c : clips)
                {
                    auto wc = dynamic_cast<te::WaveAudioClip*> (c);

                    if (wc == nullptr || wc->getClipSlot() != nullptr || wc->getClipTrack() != trackRef.get())
                        continue;

                    // Several passes: open the lanes so they're there to comp
                    if (absorbRecording (*trackRef, *wc) > 1)
                    {
                        viewState.takeLanes.insert (trackRef->itemID);
                        viewState.sendChangeMessage();
                    }
                }
            });
        }

        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    };

    //==============================================================================
    /** The left-hand header beside a track's take lanes. */
    class LaneHeader : public juce::Component
    {
    public:
        LaneHeader (te::Track& t, int laneIndex) : track (&t), lane (laneIndex) {}

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::lift (Colours::panelDark, 0.015f));
            g.setColour (Colours::outline);
            g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

            if (lane == 0)
            {
                auto n = getNumTakes (*track);
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (10.0f).withKerningFactor (0.2f));
                g.drawText ("takes  " + juce::String (n), getLocalBounds().withTrimmedLeft (34), juce::Justification::centredLeft);
            }

            // A thread down the left edge ties the lanes to their track
            g.setColour (Theme::ofTrack (*track).withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> (20.0f, 0.0f, 1.5f, (float) getHeight()));
        }

    private:
        te::Track::Ptr track;
        int lane;
    };

    //==============================================================================
    /**
        One take lane. Each take shows as a quiet waveform with its number in a circle; the parts
        on the track now are drawn in the track's colour.

        Drag across a take to comp that part onto the track (snapping to the grid when it's on).
        Click a take to use all of it. Right-click for the rest.
    */
    class Lane : public juce::Component,
                 private juce::ValueTree::Listener,
                 private juce::AsyncUpdater
    {
    public:
        Lane (EditViewState& evs, te::AudioTrack& t, int laneIndex) : viewState (evs), track (&t), lane (laneIndex)
        {
            track->state.addListener (this);
            refreshTakes();
        }

        ~Lane() override    { track->state.removeListener (this); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::lift (Colours::background, 0.015f));
            g.setColour (Colours::outline);
            g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());

            auto colour = Theme::ofTrack (*track);

            for (auto& item : items)
            {
                auto body = rangeToRect (item.take.range);

                if (body.getRight() < 0.0f || body.getX() > (float) getWidth())
                    continue;

                // The take, quietly
                g.setColour (Theme::tint (colour).withMultipliedAlpha (0.55f));
                g.fillRoundedRectangle (body, 5.0f);

                auto wave = body.reduced (0.0f, 4.0f);
                drawWave (g, item, wave, item.take.range, Colours::textDim.withAlpha (0.5f));

                // ...and the parts in the comp, in the track's colour
                for (auto used : getUsedRanges (*track, item.take))
                {
                    auto u = rangeToRect (used).getIntersection (body);
                    g.setColour (Theme::tint (colour));
                    g.fillRoundedRectangle (u, 5.0f);
                    g.saveState();
                    g.reduceClipRegion (u.toNearestInt());
                    drawWave (g, item, wave, item.take.range, Theme::contentOn (colour));
                    g.restoreState();
                }

                // The take's number, in an Orbit circle
                auto disc = juce::Rectangle<float> (13.0f, 13.0f).withCentre ({ body.getX() + 10.0f, body.getCentreY() });
                g.setColour (Colours::background);
                g.fillEllipse (disc);
                g.setColour (Colours::text);
                g.drawEllipse (disc, 1.0f);
                g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
                g.drawText (juce::String (item.take.number), disc, juce::Justification::centred);
            }

            // The swipe, as it's being made
            if (swipe)
            {
                auto r = rangeToRect (*swipe);
                g.setColour (Colours::text.withAlpha (0.12f));
                g.fillRoundedRectangle (r, 5.0f);
                g.setColour (Colours::text);
                g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            swipeTake = findItemAt ((float) e.x);
            swipe.reset();

            if (swipeTake == nullptr)
                return;

            if (e.mods.isPopupMenu())
            {
                showMenu (swipeTake->take);
                swipeTake = nullptr;
                return;
            }

            swipeStart = timeAt ((float) e.x);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (swipeTake == nullptr || e.getDistanceFromDragStartX() == 0)
                return;

            auto now = timeAt ((float) e.x);
            swipe = te::TimeRange (std::min (swipeStart, now), std::max (swipeStart, now))
                        .getIntersectionWith (swipeTake->take.range);
            repaint();
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (swipeTake == nullptr)
                return;

            auto take = swipeTake->take;
            auto& um = track->edit.getUndoManager();

            if (! e.mouseWasDraggedSinceMouseDown())
            {
                um.beginNewTransaction ("Use Take " + juce::String (take.number));
                useWholeTake (*track, take, &viewState.selectionManager);
            }
            else if (swipe && swipe->getLength() > 0.01s)
            {
                um.beginNewTransaction ("Comp Take " + juce::String (take.number));
                comp (*track, take, *swipe, &viewState.selectionManager);
            }

            swipe.reset();
            swipeTake = nullptr;
            repaint();
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            setMouseCursor (findItemAt ((float) e.x) != nullptr ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::NormalCursor);
        }

    private:
        struct Item
        {
            Take take;
            std::unique_ptr<te::SmartThumbnail> thumbnail;
        };

        EditViewState& viewState;
        te::AudioTrack::Ptr track;
        int lane;
        std::vector<Item> items;
        Item* swipeTake = nullptr;
        te::TimePosition swipeStart;
        std::optional<te::TimeRange> swipe;

        juce::Rectangle<float> rangeToRect (te::TimeRange r) const
        {
            auto x1 = viewState.timeToX (r.getStart()), x2 = viewState.timeToX (r.getEnd());
            return { x1, 2.0f, juce::jmax (2.0f, x2 - x1), (float) getHeight() - 4.0f };
        }

        te::TimePosition timeAt (float x) const
        {
            return viewState.beatToTime (viewState.snapBeat (viewState.xToBeat (x)));
        }

        Item* findItemAt (float x)
        {
            for (auto& item : items)
                if (rangeToRect (item.take.range).getHorizontalRange().contains (x))
                    return &item;

            return nullptr;
        }

        void drawWave (juce::Graphics& g, Item& item, juce::Rectangle<float> area, te::TimeRange range, juce::Colour colour)
        {
            if (item.thumbnail == nullptr || item.thumbnail->isGeneratingProxy())
                return;

            // The file's own time for the visible part of the take
            auto start = te::TimePosition() + item.take.offset;
            g.setColour (colour);
            item.thumbnail->drawChannels (g, area.toNearestInt(), { start, start + range.getLength() }, 1.0f);
        }

        void showMenu (const Take& take)
        {
            juce::PopupMenu m;
            auto t = track;

            m.addItem ("Use Take " + juce::String (take.number), [this, take, t]
            {
                t->edit.getUndoManager().beginNewTransaction ("Use Take " + juce::String (take.number));
                useWholeTake (*t, take, &viewState.selectionManager);
            });

            m.addSeparator();
            m.addItem ("Delete Take " + juce::String (take.number), [take, t]
            {
                t->edit.getUndoManager().beginNewTransaction ("Delete take");
                removeTake (*t, take);
            });

            auto unused = getUnusedTakes (*t);
            m.addItem ("Delete Unused Takes", ! unused.empty(), false, [unused, t]
            {
                t->edit.getUndoManager().beginNewTransaction ("Delete unused takes");
                for (auto& u : unused)
                    removeTake (*t, u);
            });

            m.showMenuAsync ({});
        }

        void refreshTakes()
        {
            auto takes = getTakes (*track);
            int numLanes = 0;
            auto laneOf = packLanes (takes, numLanes);

            std::vector<Item> next;

            for (size_t i = 0; i < takes.size(); ++i)
            {
                if (laneOf[i] != lane)
                    continue;

                Item item { takes[i], nullptr };

                // Keep the thumbnail when it's the same file
                for (auto& old : items)
                    if (old.take.file == item.take.file && old.thumbnail != nullptr)
                        item.thumbnail = std::move (old.thumbnail);

                if (item.thumbnail == nullptr && item.take.file.existsAsFile())
                    item.thumbnail = std::make_unique<te::SmartThumbnail> (track->edit.engine, te::AudioFile (track->edit.engine, item.take.file),
                                                                           *this, &track->edit);
                next.push_back (std::move (item));
            }

            swipeTake = nullptr;
            items = std::move (next);
            repaint();
        }

        void handleAsyncUpdate() override   { refreshTakes(); }

        // Takes coming and going, and clips changing on the track (what's used), all need a repaint
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override          { triggerAsyncUpdate(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override   { triggerAsyncUpdate(); }
        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
    };
}

} // namespace elsewhere
