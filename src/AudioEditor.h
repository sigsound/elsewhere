// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "EditModes.h"
#include "Normalise.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/** Editing part of an audio track, as Logic does with a selection in its audio editor: what's
    inside a time range is split off into clips of its own, which are then normalized, reversed,
    deleted or kept, leaving the rest as it was. */
namespace AudioEdits
{
    /** Splits the track's audio clips at the range's edges; returns the clips now inside it. */
    inline juce::Array<te::AudioClipBase*> isolate (te::ClipTrack& track, te::TimeRange range)
    {
        juce::Array<te::Clip*> overlapping;

        for (auto c : track.getClips())
            if (dynamic_cast<te::AudioClipBase*> (c) != nullptr && c->getEditTimeRange().overlaps (range))
                overlapping.add (c);

        juce::Array<te::AudioClipBase*> inside;

        for (auto c : overlapping)
        {
            te::Clip* piece = c;

            if (range.getStart() > piece->getPosition().getStart())
                if (auto right = track.splitClip (*piece, range.getStart()))
                    piece = right;

            if (range.getEnd() < piece->getPosition().getEnd())
                track.splitClip (*piece, range.getEnd());

            // A split within a millisecond of a clip's edge doesn't happen; that clip counts as inside
            auto r = piece->getEditTimeRange();

            if (r.getStart() >= range.getStart() - 0.002s && r.getEnd() <= range.getEnd() + 0.002s)
                if (auto ac = dynamic_cast<te::AudioClipBase*> (piece))
                    inside.add (ac);
        }

        return inside;
    }

    inline juce::Array<te::Clip*> asClips (const juce::Array<te::AudioClipBase*>& clips)
    {
        juce::Array<te::Clip*> result;
        for (auto c : clips)
            result.add (c);
        return result;
    }

    inline void reverse (te::ClipTrack& track, te::TimeRange range)
    {
        for (auto c : isolate (track, range))
            c->setIsReversed (! c->getIsReversed());
    }

    /** Removes the audio in the range, leaving a gap (or, in Shuffle mode, closing it up). */
    inline void remove (te::ClipTrack& track, te::TimeRange range, bool shuffle)
    {
        EditModes::deleteClips (asClips (isolate (track, range)), shuffle);
    }

    /** Keeps only the range of what's in `span`. */
    inline void trim (te::ClipTrack& track, te::TimeRange span, te::TimeRange keep)
    {
        for (auto outside : { te::TimeRange (span.getStart(), keep.getStart()), te::TimeRange (keep.getEnd(), span.getEnd()) })
            if (outside.getLength() > 0.001s)
                remove (track, outside, false);
    }
}

//==============================================================================
/**
    The clip editor's waveform (when warp markers are off): the clip's stretch of its track. Drag
    to select part of it, then right-click to normalize, reverse, delete or trim it, or comment on
    it; Delete removes it too. It starts with the whole clip selected. Edits split the clip at the
    selection's edges, and the pieces stay in view here.
*/
class AudioSelectionEditor : public juce::Component, private juce::Timer
{
public:
    AudioSelectionEditor (EditViewState& evs, te::AudioClipBase& c)
        : viewState (evs), track (c.getClipTrack()), span (c.getEditTimeRange()), selection (span)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (15);
    }

    bool hasSelection() const       { return selection.has_value() && selection->getLength() > 0.001s; }
    void selectAll()                { selection = span; repaint(); }
    void setSelection (std::optional<te::TimeRange> r)  { selection = r; repaint(); }

    //==============================================================================
    void normalizeSelection()
    {
        auto ct = getTrack();
        if (ct == nullptr || ! hasSelection())
            return;

        auto range = *selection;
        std::vector<Normalise::Part> parts;

        for (auto c : ct->getClips())
            if (auto ac = dynamic_cast<te::AudioClipBase*> (c); ac != nullptr && ! c->disabled && c->getEditTimeRange().overlaps (range))
                parts.push_back ({ ac, range.getIntersectionWith (c->getEditTimeRange()) });

        if (parts.empty())
            return;

        auto what = range == span ? juce::String ("The whole clip") : "The selection, " + describe (range);
        auto trackRef = te::Track::Ptr (ct);
        juce::Component::SafePointer<AudioSelectionEditor> sp (this);

        Normalise::showDialog (parts, what, [trackRef, range, sp] (const std::vector<Normalise::Part>& ps, const std::vector<float>& changes)
        {
            auto clipTrack = dynamic_cast<te::ClipTrack*> (trackRef.get());
            if (clipTrack == nullptr)
                return;

            clipTrack->edit.getUndoManager().beginNewTransaction ("Normalize selection");

            for (auto piece : AudioEdits::isolate (*clipTrack, range))
            {
                // The part this piece came from is the one around its middle
                auto middle = piece->getEditTimeRange().getCentre();

                for (size_t i = 0; i < ps.size(); ++i)
                    if (ps[i].range.contains (middle) && changes[i] != 0.0f)
                        piece->setGainDB (piece->getGainDB() + changes[i]);
            }

            if (sp != nullptr)
                sp->repaint();
        }, Normalise::Process::together);
    }

    void reverseSelection()     { edit ("Reverse selection", [this] (te::ClipTrack& t) { AudioEdits::reverse (t, *selection); }); }
    void splitSelection()       { edit ("Split at selection", [this] (te::ClipTrack& t) { AudioEdits::isolate (t, *selection); }); }

    void deleteSelection()
    {
        edit ("Delete selection", [this] (te::ClipTrack& t)
        {
            AudioEdits::remove (t, *selection, viewState.editMode == EditViewState::EditMode::shuffle);
            selection.reset();
        });
    }

    void trimToSelection()
    {
        edit ("Trim to selection", [this] (te::ClipTrack& t)
        {
            AudioEdits::trim (t, span, *selection);
            span = *selection;
        });
    }

    void commentOnSelection()
    {
        if (auto ct = getTrack(); ct != nullptr && hasSelection() && viewState.addComment)
            viewState.addComment (ct->itemID, *selection, ct->getName());
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);
        auto ct = getTrack();

        if (ct == nullptr)
            return;

        auto r = getLocalBounds().toFloat();
        paintRuler (g, r.removeFromTop ((float) rulerH));
        auto hint = r.removeFromBottom (18.0f);
        auto lane = r.reduced (0.0f, 4.0f);
        laneArea = lane;

        juce::StringArray stillUsed;
        auto colour = Theme::ofTrack (*ct);

        for (auto c : ct->getClips())
        {
            auto ac = dynamic_cast<te::AudioClipBase*> (c);
            auto clipRange = c->getEditTimeRange();

            if (ac == nullptr || ! clipRange.overlaps (span))
                continue;

            auto visible = clipRange.getIntersectionWith (span);
            auto x1 = timeToX (clipRange.getStart()), x2 = timeToX (clipRange.getEnd());
            auto body = juce::Rectangle<float> (x1, lane.getY(), x2 - x1, lane.getHeight()).reduced (0.5f, 0.0f);

            g.saveState();
            g.reduceClipRegion (juce::Rectangle<float> (timeToX (visible.getStart()), lane.getY(),
                                                        timeToX (visible.getEnd()) - timeToX (visible.getStart()), lane.getHeight()).toNearestInt());
            g.setColour (Theme::tint (colour).withMultipliedAlpha (c->disabled ? 0.4f : 1.0f));
            g.fillRoundedRectangle (body, 8.0f);
            g.setColour (colour.withMultipliedAlpha (c->disabled ? 0.4f : 1.0f));
            drawWaveform (g, *ac, visible, body.reduced (0.0f, 8.0f), stillUsed);
            g.restoreState();
        }

        forgetThumbnailsExcept (stillUsed);

        // The selection: lighter, with an ink edge and a dot at each end
        if (hasSelection())
        {
            auto sx1 = timeToX (selection->getStart()), sx2 = timeToX (selection->getEnd());
            g.setColour (Colours::text.withAlpha (0.09f));
            g.fillRect (juce::Rectangle<float> (sx1, lane.getY(), sx2 - sx1, lane.getHeight()));
            g.setColour (Colours::text);

            for (auto x : { sx1, sx2 })
            {
                g.fillRect (x - 0.5f, lane.getY(), 1.0f, lane.getHeight());
                g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, lane.getY() + 3.5f }));
            }
        }

        // Playhead
        auto pos = viewState.edit.getTransport().getPosition();

        if (span.contains (pos))
        {
            g.setColour (Colours::playhead);
            g.fillRect (timeToX (pos) - 0.5f, lane.getY(), 1.0f, lane.getHeight());
        }

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (hasSelection() ? describe (*selection) + " selected. Right-click to normalize, reverse, delete or trim it, or comment on it."
                                   : "Drag to select part of the audio. Double-click selects all of it.",
                    hint.reduced (pad, 0.0f), juce::Justification::centredLeft, true);
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (edgeAt ((float) e.x) != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::IBeamCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (e.mods.isPopupMenu())
            return showMenu();

        auto t = xToTime ((float) e.x);

        if (auto edge = edgeAt ((float) e.x); edge != 0)
            anchor = edge < 0 ? selection->getEnd() : selection->getStart();   // drag the other way from the fixed end
        else
            anchor = t;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || ! anchor)
            return;

        auto t = xToTime ((float) e.x);
        selection = te::TimeRange (std::min (*anchor, t), std::max (*anchor, t));
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A click puts the playhead there
        if (! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown() && edgeAt ((float) e.x) == 0)
        {
            selection.reset();
            viewState.edit.getTransport().setPosition (xToTime ((float) e.x));
            repaint();
        }

        anchor.reset();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override    { selectAll(); }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && hasSelection())
        {
            deleteSelection();
            return true;
        }

        if (k == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
        {
            selectAll();
            return true;
        }

        return false;
    }

private:
    static constexpr int rulerH = 16;
    static constexpr float pad = 10.0f;
    EditViewState& viewState;
    te::Track::Ptr track;
    te::TimeRange span;
    std::optional<te::TimeRange> selection;
    std::optional<te::TimePosition> anchor;
    juce::Rectangle<float> laneArea;
    std::map<juce::String, std::unique_ptr<te::SmartThumbnail>> thumbnails;

    te::ClipTrack* getTrack() const     { return dynamic_cast<te::ClipTrack*> (track.get()); }

    float timeToX (te::TimePosition t) const
    {
        auto len = juce::jmax (0.001, span.getLength().inSeconds());
        return pad + (float) ((t - span.getStart()).inSeconds() / len) * ((float) getWidth() - 2.0f * pad);
    }

    te::TimePosition xToTime (float x) const
    {
        // Near either end counts as the end, so selecting to the edge is easy
        if (x < pad + 4.0f)                     return span.getStart();
        if (x > (float) getWidth() - pad - 4.0f) return span.getEnd();

        auto len = span.getLength().inSeconds();
        auto t = span.getStart().inSeconds() + (double) (x - pad) / juce::jmax (1.0f, (float) getWidth() - 2.0f * pad) * len;
        return te::TimePosition::fromSeconds (juce::jlimit (span.getStart().inSeconds(), span.getEnd().inSeconds(), t));
    }

    /** -1 or 1 when x is on the selection's start or end, else 0. */
    int edgeAt (float x) const
    {
        if (! hasSelection())
            return 0;

        if (std::abs (x - timeToX (selection->getStart())) < 5.0f)  return -1;
        if (std::abs (x - timeToX (selection->getEnd())) < 5.0f)    return 1;
        return 0;
    }

    juce::String describe (te::TimeRange r) const
    {
        auto& e = viewState.edit;
        return EngineHelpers::formatBarsBeats (e, r.getStart()) + juce::String::fromUTF8 (" \xe2\x80\x93 ")
             + EngineHelpers::formatBarsBeats (e, r.getEnd()) + " (" + juce::String (r.getLength().inSeconds(), 2) + " s)";
    }

    /** Runs an edit on the selection as one undoable step; if it removed the clip the details
        were showing, shows one of what's left. */
    void edit (const juce::String& name, std::function<void (te::ClipTrack&)> fn)
    {
        auto ct = getTrack();
        if (ct == nullptr || ! hasSelection())
            return;

        ct->edit.getUndoManager().beginNewTransaction (name);
        fn (*ct);

        if (viewState.getSelectedClip() == nullptr)
            for (auto c : ct->getClips())
                if (c->getEditTimeRange().overlaps (span))
                {
                    viewState.selectionManager.selectOnly (c);
                    break;
                }

        repaint();
    }

    void showMenu()
    {
        auto has = hasSelection();
        juce::Component::SafePointer<AudioSelectionEditor> sp (this);
        auto run = [sp] (void (AudioSelectionEditor::*fn)()) { return [sp, fn] { if (sp != nullptr) (sp.getComponent()->*fn)(); }; };

        juce::PopupMenu m;
        m.addItem ("Normalize...", has, false, run (&AudioSelectionEditor::normalizeSelection));
        m.addItem ("Reverse", has, false, run (&AudioSelectionEditor::reverseSelection));

        juce::PopupMenu::Item del ("Delete");
        del.setEnabled (has).setAction (run (&AudioSelectionEditor::deleteSelection));
        del.shortcutKeyDescription = juce::String::fromUTF8 ("\xe2\x8c\xab");
        m.addItem (del);

        m.addItem ("Trim to Selection", has, false, run (&AudioSelectionEditor::trimToSelection));
        m.addItem ("Split at Selection", has, false, run (&AudioSelectionEditor::splitSelection));

        if (viewState.addComment)
        {
            m.addSeparator();
            m.addItem ("Comment on Selection...", has, false, run (&AudioSelectionEditor::commentOnSelection));
        }

        m.addSeparator();
        m.addItem ("Select All", run (&AudioSelectionEditor::selectAll));
        m.showMenuAsync ({});
    }

    void paintRuler (juce::Graphics& g, juce::Rectangle<float> r)
    {
        auto beatsPerBar = juce::jmax (1, juce::roundToInt (viewState.getBeatsPerBar()));
        auto firstBar = (int) std::floor (viewState.timeToBeat (span.getStart()) / beatsPerBar);
        auto lastBar = (int) std::ceil (viewState.timeToBeat (span.getEnd()) / beatsPerBar);
        float lastX = -1000.0f;

        g.setFont (juce::FontOptions (10.0f));

        for (int bar = firstBar; bar <= lastBar; ++bar)
        {
            auto t = viewState.beatToTime ((double) bar * beatsPerBar);
            if (! span.contains (t) || timeToX (t) - lastX < 36.0f)
                continue;

            auto x = timeToX (t);
            g.setColour (Theme::control());
            g.fillRect (x, r.getBottom() - 4.0f, 1.0f, 4.0f);
            g.setColour (Colours::textDim);
            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 3.0f, r.getY(), 40.0f, r.getHeight() - 2.0f).toNearestInt(),
                        juce::Justification::centredLeft);
            lastX = x;
        }
    }

    /** The visible part of a clip's waveform, the way the arrangement draws it (with its gain). */
    void drawWaveform (juce::Graphics& g, te::AudioClipBase& ac, te::TimeRange visible, juce::Rectangle<float> area, juce::StringArray& used)
    {
        auto file = ac.getPlaybackFile();
        auto key = ac.itemID.toString() + file.getFile().getFullPathName();
        used.add (key);

        auto& th = thumbnails[key];
        if (th == nullptr)
            th = std::make_unique<te::SmartThumbnail> (ac.edit.engine, file, *this, &ac.edit);

        if (th->isGeneratingProxy() || th->getTotalLength() <= 0.0)
            return;

        auto offset = ac.getPosition().getOffset().inSeconds();
        auto speed = (ac.usesTimeStretchedProxy() || ac.isLooping()) ? 1.0 : ac.getSpeedRatio();
        auto from = (visible.getStart() - ac.getPosition().getStart()).inSeconds();
        auto to = (visible.getEnd() - ac.getPosition().getStart()).inSeconds();
        te::TimeRange source (te::TimePosition::fromSeconds ((offset + from) * speed), te::TimePosition::fromSeconds ((offset + to) * speed));

        auto x1 = timeToX (visible.getStart()), x2 = timeToX (visible.getEnd());
        auto rect = juce::Rectangle<float> (x1, area.getY(), x2 - x1, area.getHeight()).toNearestInt();

        if (th->getNumChannels() > 1 && rect.getHeight() > 60)
        {
            th->drawChannel (g, rect.removeFromTop (rect.getHeight() / 2), source, 0, ac.getGain());
            th->drawChannel (g, rect, source, 1, ac.getGain());
        }
        else
        {
            th->drawChannels (g, rect, source, ac.getGain());
        }
    }

    void forgetThumbnailsExcept (const juce::StringArray& keep)
    {
        for (auto it = thumbnails.begin(); it != thumbnails.end();)
            it = keep.contains (it->first) ? std::next (it) : thumbnails.erase (it);
    }

    void timerCallback() override
    {
        if (isShowing())
            repaint();
    }
};

} // namespace elsewhere
