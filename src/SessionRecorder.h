// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Records a Session performance into the Arrangement.

    While the transport is recording, every clip you launch in the Session view is
    written onto the same track's timeline, at exactly the beats it played (from the
    engine's launch handles, so launch quantisation is honoured). A clip that was already
    playing when you pressed record is captured from the record point on. Whatever was
    on the timeline underneath is replaced, as in Ableton.

    The tracks keep playing their Session clips afterwards; press "Back to Arrangement"
    to hear the recorded version.
*/
class SessionRecorder : private juce::Timer
{
public:
    explicit SessionRecorder (te::Edit& e) : edit (e)   { startTimerHz (30); }
    ~SessionRecorder() override                         { stopTimer(); }

    /** Number of clips captured in the current/last recording (for the UI). */
    int getNumCaptured() const      { return numCaptured; }

    /** Writes one played clip onto its track's timeline.
        @param played        the Edit beat range the Session clip played for
        @param recordStart   anything before this isn't recorded (the record point) */
    static te::Clip* writeTake (te::AudioTrack& track, te::Clip& slotClip, te::BeatRange played, te::BeatPosition recordStart)
    {
        auto start = juce::jmax (played.getStart(), recordStart);
        auto end = played.getEnd();

        if (end - start < te::BeatDuration::fromBeats (1.0 / 64.0))
            return nullptr;

        auto& ts = track.edit.tempoSequence;
        const te::TimeRange time (ts.toTime (start), ts.toTime (end));

        // If the clip started before the record point, begin that far into it (wrapping
        // around its loop, since Session clips loop)
        double offsetBeats = (start - played.getStart()).inBeats();
        if (slotClip.isLooping() && slotClip.getLoopLengthBeats() > te::BeatDuration())
            offsetBeats = std::fmod (offsetBeats, slotClip.getLoopLengthBeats().inBeats());

        const auto offset = ts.toTime (played.getStart() + te::BeatDuration::fromBeats (offsetBeats)) - ts.toTime (played.getStart());

        // A deep copy: the engine re-IDs and repositions the state it's given
        return track.insertClipWithState (slotClip.state.createCopy(), slotClip.getName(), slotClip.type,
                                          { time, offset }, true, false);
    }

private:
    struct Take
    {
        te::AudioTrack::Ptr track;
        te::Clip::Ptr clip;
        te::BeatPosition start, end;
    };

    te::Edit& edit;
    std::map<te::EditItemID, Take> open;
    bool wasRecording = false;
    te::BeatPosition recordStart;
    int numCaptured = 0;

    void timerCallback() override   { update(); }

public:
    /** Checks the launch handles (the timer calls this; tests can call it directly). */
    void update()
    {
        auto& transport = edit.getTransport();
        const bool recording = transport.isRecording();

        if (recording && ! wasRecording)
        {
            open.clear();
            numCaptured = 0;
            recordStart = edit.tempoSequence.toBeats (transport.getTimeWhenStarted());
        }

        if (recording)
            poll();
        else if (wasRecording)
            finishAll();

        wasRecording = recording;
    }

private:
    void poll()
    {
        std::set<te::EditItemID> seen;

        for (auto track : te::getAudioTracks (edit))
        {
            for (auto slot : track->getClipSlotList().getClipSlots())
            {
                auto clip = slot->getClip();
                if (clip == nullptr) continue;
                auto lh = clip->getLaunchHandle();
                if (lh == nullptr) continue;

                const auto key = clip->itemID;
                seen.insert (key);
                auto it = open.find (key);
                const bool playing = lh->getPlayingStatus() == te::LaunchHandle::PlayState::playing;

                if (playing)
                {
                    if (auto range = lh->getPlayedRange())
                    {
                        // Relaunched (a new start): the previous run ends here
                        if (it != open.end() && std::abs ((it->second.start - range->getStart()).inBeats()) > 1.0e-3)
                        {
                            it->second.end = range->getStart();
                            finish (it->second);
                            open.erase (it);
                            it = open.end();
                        }

                        if (it == open.end())
                            open[key] = { track, clip, range->getStart(), range->getEnd() };
                        else
                            it->second.end = range->getEnd();
                    }
                }
                else if (it != open.end())
                {
                    if (auto last = lh->getLastPlayedRange(); last && std::abs ((last->getStart() - it->second.start).inBeats()) < 1.0e-3)
                        it->second.end = last->getEnd();

                    finish (it->second);
                    open.erase (it);
                }
            }
        }

        // Clips deleted while playing: keep what was heard
        for (auto it = open.begin(); it != open.end();)
        {
            if (seen.count (it->first) == 0) { finish (it->second); it = open.erase (it); }
            else ++it;
        }
    }

    void finishAll()
    {
        // Recording stopped: everything still playing ends at the stop point
        for (auto& [key, take] : open)
        {
            if (auto lh = take.clip->getLaunchHandle())
            {
                if (auto range = lh->getPlayedRange())
                    take.end = range->getEnd();
                else if (auto last = lh->getLastPlayedRange(); last && std::abs ((last->getStart() - take.start).inBeats()) < 1.0e-3)
                    take.end = last->getEnd();
            }

            finish (take);
        }

        open.clear();
    }

    void finish (const Take& take)
    {
        if (take.track == nullptr || take.clip == nullptr)
            return;

        if (writeTake (*take.track, *take.clip, { take.start, take.end }, recordStart) != nullptr)
            ++numCaptured;
    }
};

} // namespace elsewhere
