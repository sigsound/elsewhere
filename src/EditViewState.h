// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/** Shared UI state for one open edit: zoom, scroll, grid, selection. */
class EditViewState : public juce::ChangeBroadcaster
{
public:
    EditViewState (te::Edit& e, te::SelectionManager& sm) : edit (e), selectionManager (sm) {}

    te::Edit& edit;
    te::SelectionManager& selectionManager;

    //==============================================================================
    // Horizontal zoom/scroll, in beats (the grid follows the tempo map)
    double viewStartBeat = 0.0;
    double pixelsPerBeat = 24.0;

    // Snap grid in beats, 0 = off
    double gridBeats = 1.0;
    bool snapEnabled = true;

    /** How dragging clips behaves, like Pro Tools' edit modes:
            slip     clips move freely
            grid     clips snap to the grid
            shuffle  clips on a track close up: moving one slots it between the others, deleting
                     or trimming one moves the clips after it
            spot     clicking a clip asks for an exact start time
    */
    enum class EditMode { slip, grid, shuffle, spot };
    EditMode editMode = EditMode::grid;

    static juce::String getEditModeName (EditMode m)
    {
        switch (m)
        {
            case EditMode::slip:     return "Slip";
            case EditMode::grid:     return "Grid";
            case EditMode::shuffle:  return "Shuffle";
            case EditMode::spot:     return "Spot";
        }

        return {};
    }

    // Tracks hidden from the Arrangement and the Mixer (the track list shows them all).
    // Kept on the track, so it's saved with the project.
    static inline const juce::Identifier hiddenProperty { "elsewhereHidden" };

    static bool isTrackHidden (const te::Track& t)            { return t.state.getProperty (hiddenProperty, false); }

    static void setTrackHidden (te::Track& t, bool hidden)
    {
        if (hidden)
            t.state.setProperty (hiddenProperty, true, nullptr);
        else
            t.state.removeProperty (hiddenProperty, nullptr);
    }

    // Which bottom panel is showing
    enum class DetailMode { devices, clip, comments, live };

    // Collaborators currently in the project (from the collaboration session)
    struct Collaborator
    {
        juce::String name;
        juce::Colour colour;
        te::EditItemID focusedTrack;
    };

    std::vector<Collaborator> collaborators;

    /** Adds a comment on a stretch of a clip or track (set by MainComponent when comments are available). */
    std::function<void (te::EditItemID target, te::TimeRange range, const juce::String& targetName)> addComment;

    /** Comments: marking out a new one's region in the arrangement, the one open in the notes
        book, and whether resolved ones show (in the book and on the tracks). */
    bool addingComment = false;
    juce::String openCommentId;
    bool showResolvedComments = false;

    /** Number of open comments on an item (set by MainComponent). */
    std::function<int (te::EditItemID)> getCommentCount;
    std::function<juce::String (te::EditItemID)> getCommentText;
    DetailMode detailMode = DetailMode::devices;

    /** Scrolls the arrangement so a time is in view (set by the arrangement). */
    std::function<void (te::TimePosition)> revealTime;

    //==============================================================================
    double timeToBeat (te::TimePosition t) const    { return edit.tempoSequence.toBeats (t).inBeats(); }
    te::TimePosition beatToTime (double b) const    { return edit.tempoSequence.toTime (te::BeatPosition::fromBeats (b)); }

    float beatToX (double beat) const               { return (float) ((beat - viewStartBeat) * pixelsPerBeat); }
    double xToBeat (float x) const                  { return viewStartBeat + x / pixelsPerBeat; }

    float timeToX (te::TimePosition t) const        { return beatToX (timeToBeat (t)); }
    te::TimePosition xToTime (float x) const        { return beatToTime (juce::jmax (0.0, xToBeat (x))); }

    double getBeatsPerBar() const
    {
        return (double) edit.tempoSequence.getTimeSig (0)->numerator.get()
                 * 4.0 / (double) edit.tempoSequence.getTimeSig (0)->denominator.get();
    }

    /** Grid size that stays readable at the current zoom level. */
    double getEffectiveGrid() const
    {
        if (gridBeats > 0.0)
            return gridBeats;

        // "Adaptive" grid
        double g = 1.0 / 16.0;
        while (g * pixelsPerBeat < 12.0)
            g *= 2.0;
        return g;
    }

    double snapBeat (double beat, bool bypassSnap = false) const
    {
        if (! snapEnabled || bypassSnap || editMode == EditMode::slip)
            return juce::jmax (0.0, beat);

        auto g = getEffectiveGrid();
        return juce::jmax (0.0, std::round (beat / g) * g);
    }

    double snapBeatDown (double beat) const
    {
        auto g = getEffectiveGrid();
        return juce::jmax (0.0, std::floor (beat / g) * g);
    }

    void setZoom (double newPixelsPerBeat, float anchorX)
    {
        auto anchorBeat = xToBeat (anchorX);
        pixelsPerBeat = juce::jlimit (1.0, 800.0, newPixelsPerBeat);
        viewStartBeat = juce::jmax (0.0, anchorBeat - anchorX / pixelsPerBeat);
        sendChangeMessage();
    }

    void scrollBy (double beats)
    {
        viewStartBeat = juce::jmax (0.0, viewStartBeat + beats);
        sendChangeMessage();
    }

    //==============================================================================
    te::Track* getSelectedTrack() const
    {
        if (auto t = selectionManager.getFirstItemOfType<te::Track>())
            return t;

        if (auto c = selectionManager.getFirstItemOfType<te::Clip>())
            return c->getTrack();

        return nullptr;
    }

    te::Clip* getSelectedClip() const
    {
        return selectionManager.getFirstItemOfType<te::Clip>();
    }

    /** The track the device view shows: kept separately so selecting a clip or plugin
        doesn't lose track focus. */
    te::SafeSelectable<te::Track> focusedTrack;

    void focusTrack (te::Track* t)
    {
        if (focusedTrack.get() != t)
        {
            focusedTrack = t;
            sendChangeMessage();
        }
    }

    // Group tracks whose children are hidden
    std::set<te::EditItemID> collapsedGroups;

    // Tracks showing an automation lane, and which parameter it shows
    std::map<te::EditItemID, te::AutomatableParameter::Ptr> automationLanes;
    static constexpr int automationLaneHeight = 70;

    /** Tracks showing their take lanes (the takes kept from recording, for comping). */
    std::set<te::EditItemID> takeLanes;
    static constexpr int takeLaneHeight = 38;

    bool isHiddenByCollapsedGroup (te::Track& t) const
    {
        for (auto p = t.getParentTrack(); p != nullptr; p = p->getParentTrack())
            if (collapsedGroups.count (p->itemID))
                return true;

        return false;
    }

    // Track heights, keyed by track ID
    std::map<te::EditItemID, int> trackHeights;
    static constexpr int defaultTrackHeight = 90;    // room for the name, fader and input rows
    static constexpr int minTrackHeight = 26;

    int getTrackHeight (te::Track& t) const
    {
        auto it = trackHeights.find (t.itemID);
        return it != trackHeights.end() ? it->second : defaultTrackHeight;
    }
};

} // namespace elsewhere
