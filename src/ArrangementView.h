// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "TrackHeader.h"
#include "AutomationLane.h"
#include "EditModes.h"
#include "Markers.h"
#include "Comments.h"
#include "drums/DrumTrack.h"

namespace elsewhere
{
//==============================================================================
/** Files dragged from the Browser panel carry this description; the file is the tree's selection. */
inline constexpr const char* browserDragDescription = "elsewhere-file";

/** A clip dragged from the clip list ("elsewhere-clip:<id>"), if that's what this is. */
inline te::Clip* getDraggedClip (te::Edit& edit, const juce::DragAndDropTarget::SourceDetails& d)
{
    auto desc = d.description.toString();

    if (! desc.startsWith ("elsewhere-clip:"))
        return nullptr;

    return te::findClipForID (edit, te::EditItemID::fromString (desc.fromFirstOccurrenceOf (":", false, false)));
}

inline juce::File getDraggedFile (const juce::DragAndDropTarget::SourceDetails& d)
{
    if (d.description.toString() == browserDragDescription)
        if (auto ft = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
            return ft->getSelectedFile();

    return {};
}

//==============================================================================
/** Clips show their track's colour: tracks (and groups) are the only things you colour. */
inline juce::Colour getClipColour (te::Clip& c)
{
    if (auto t = c.getTrack())
        return Theme::ofTrack (*t);

    return Colours::accent;
}

/** Draws the notes of a MIDI clip scaled into the given rectangle. */
inline void drawMidiNotes (juce::Graphics& g, te::MidiClip& mc, juce::Rectangle<float> r, double startBeat, double endBeat, juce::Colour colour)
{
    auto& seq = mc.getSequence();
    auto notes = seq.getNotes();

    if (notes.isEmpty() || endBeat <= startBeat)
        return;

    int lo = 127, hi = 0;
    for (auto n : notes) { lo = juce::jmin (lo, n->getNoteNumber()); hi = juce::jmax (hi, n->getNoteNumber()); }
    lo = juce::jmax (0, lo - 2); hi = juce::jmin (127, hi + 2);
    auto range = (float) juce::jmax (8, hi - lo + 1);
    auto noteH = juce::jmax (1.5f, r.getHeight() / range);

    auto loopLen = mc.isLooping() ? mc.getLoopLengthBeats().inBeats() : 0.0;
    auto loopStart = mc.isLooping() ? mc.getLoopStartBeats().inBeats() : 0.0;
    auto offset = mc.getOffsetInBeats().inBeats();
    auto clipLen = endBeat - startBeat;

    g.setColour (colour);

    auto drawNote = [&] (double s, double e, int noteNum)
    {
        if (e <= 0.0 || s >= clipLen) return;
        s = juce::jmax (0.0, s); e = juce::jmin (clipLen, e);
        auto x1 = r.getX() + (float) (s / clipLen) * r.getWidth();
        auto x2 = r.getX() + (float) (e / clipLen) * r.getWidth();
        auto y = r.getBottom() - ((float) (noteNum - lo) + 1.0f) * (r.getHeight() / range);
        g.fillRect (juce::Rectangle<float> (x1, y, juce::jmax (1.0f, x2 - x1 - 0.5f), noteH));
    };

    if (loopLen > 0.0)
    {
        for (double loopOffset = -offset; loopOffset < clipLen; loopOffset += loopLen)
            for (auto n : notes)
            {
                auto s = n->getStartBeat().inBeats() - loopStart;
                auto e = juce::jmin (n->getEndBeat().inBeats() - loopStart, loopLen);
                if (s < 0.0 || s >= loopLen) continue;
                drawNote (loopOffset + s, loopOffset + e, n->getNoteNumber());
            }
    }
    else
    {
        for (auto n : notes)
            drawNote (n->getStartBeat().inBeats() - offset, n->getEndBeat().inBeats() - offset, n->getNoteNumber());
    }
}

//==============================================================================
/** A clip on the timeline. Handles selection, move, trim and waveform drawing. */
class ArrangementClip : public juce::Component, public juce::SettableTooltipClient, private juce::ChangeListener
{
public:
    ArrangementClip (EditViewState& evs, te::Clip& c) : viewState (evs), clip (&c)
    {
        if (auto wc = dynamic_cast<te::WaveAudioClip*> (&c))
        {
            thumbnail = std::make_unique<te::SmartThumbnail> (c.edit.engine, wc->getPlaybackFile(), *this, &c.edit);
        }

        viewState.selectionManager.addChangeListener (this);
    }

    ~ArrangementClip() override
    {
        viewState.selectionManager.removeChangeListener (this);
    }

    te::Clip& getClip()     { return *clip; }

    /** Called by the lane when the clip's audio source may have changed. */
    void updateThumbnail()
    {
        if (auto wc = dynamic_cast<te::WaveAudioClip*> (clip.get()))
            if (thumbnail != nullptr)
                thumbnail->setNewFile (wc->getPlaybackFile());
    }

    void paint (juce::Graphics& g) override
    {
        if (DrumTrack::isSection (*clip))
            return paintDrumSection (g);

        auto colour = getClipColour (*clip);
        bool selected = viewState.selectionManager.isSelected (clip.get());
        bool muted = clip->disabled;

        if (muted)
            colour = colour.withSaturation (0.0f).withMultipliedBrightness (0.6f);

        // Orbit's tint: a quiet wash of the track's colour, with what's in it in the full colour
        auto r = paintTintBody (g, colour);
        auto title = r.removeFromTop (juce::jmin (16.0f, r.getHeight() * 0.35f));

        g.setColour (Theme::tintLabel (colour));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (clip->getName(), title.withTrimmedLeft (6.0f).withTrimmedRight (4.0f).toNearestInt(), juce::Justification::centredLeft, true);

        auto content = r.reduced (4.0f, 2.0f);

        if (auto mc = dynamic_cast<te::MidiClip*> (clip.get()))
        {
            auto startBeat = clip->getStartBeat().inBeats();
            auto endBeat = clip->getEndBeat().inBeats();
            drawMidiNotes (g, *mc, content, startBeat, endBeat, colour);
        }
        else if (auto ac = dynamic_cast<te::AudioClipBase*> (clip.get()))
        {
            drawWaveform (g, *ac, content, colour);
        }

        // A looped clip: the first pass in full, the repeats after it faded, and a handle where
        // the first repeat begins (drag it to change how long the loop is)
        if (auto loopX = getLoopHandleX())
        {
            auto body = getLocalBounds().toFloat().reduced (1.0f, 1.5f);
            juce::Path shape;
            shape.addRoundedRectangle (body, 6.0f);

            g.saveState();
            g.reduceClipRegion (shape);
            g.setColour (Colours::background.withAlpha (0.5f));
            g.fillRect (body.withLeft (*loopX));

            // Where each later repeat starts, quietly
            const auto loopLen = clip->getLoopLengthBeats().inBeats();
            g.setColour (Theme::tintLabel (colour).withAlpha (0.18f));

            for (double b = clip->getStartBeat().inBeats() + 2.0 * loopLen - clip->getOffsetInBeats().inBeats(); b < clip->getEndBeat().inBeats(); b += loopLen)
                g.fillRect (juce::Rectangle<float> (viewState.beatToX (b) - (float) getX(), body.getY(), 1.0f, body.getHeight()));

            g.restoreState();

            const bool active = dragZone == DragZone::loopEnd && dragged;
            g.setColour (Theme::tintLabel (colour).withAlpha (active || hoverLoopHandle ? 1.0f : 0.55f));
            g.fillRect (juce::Rectangle<float> (*loopX - 0.75f, body.getY() + 3.0f, 1.5f, body.getHeight() - 3.0f));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ *loopX, body.getY() + 3.5f }));
        }

        if (selected)
            paintSelection (g);

        // Fade handles
        if (auto ac = dynamic_cast<te::AudioClipBase*> (clip.get()))
            drawFades (g, *ac);

        // Comment badge
        if (viewState.getCommentCount)
        {
            if (auto n = viewState.getCommentCount (clip->itemID); n > 0)
            {
                auto badge = juce::Rectangle<float> ((float) getWidth() - 22.0f, 1.0f, 20.0f, 13.0f);
                g.setColour (Colours::text);
                g.fillRoundedRectangle (badge, 3.0f);
                g.setColour (Colours::background);
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                g.drawText (juce::String::fromUTF8 ("\xf0\x9f\x92\xac") + juce::String (n), badge.toNearestInt(), juce::Justification::centred);
            }
        }
    }

    juce::String getTooltip() override
    {
        return viewState.getCommentText ? viewState.getCommentText (clip->itemID) : juce::String();
    }

    /** Drum sections: named and coloured like song sections, with their hits in three rows. */
    void paintDrumSection (juce::Graphics& g)
    {
        const auto label = DrumTrack::getLabel (clip->state);
        auto colour = getClipColour (*clip);
        const bool selected = viewState.selectionManager.isSelected (clip.get());
        if (clip->disabled)
            colour = colour.withSaturation (0.0f).withMultipliedBrightness (0.6f);

        auto r = paintTintBody (g, colour);
        auto title = r.removeFromTop (juce::jmin (16.0f, r.getHeight() * 0.35f));

        const auto settings = DrumTrack::readSettings (clip->state);
        g.setColour (Theme::tintLabel (colour));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (label.toLowerCase() + juce::String::fromUTF8 (" \xc2\xb7 ") + juce::String (drums::styleName (settings.style)).toLowerCase(),
                    title.withTrimmedLeft (6.0f).withTrimmedRight (4.0f).toNearestInt(), juce::Justification::centredLeft, true);

        // The hits as dots in three rows, bigger and stronger the harder they're played
        if (auto mc = dynamic_cast<te::MidiClip*> (clip.get()); mc != nullptr && mc->getTrack() != nullptr && r.getHeight() > 12.0f)
        {
            const auto map = DrumTrack::noteMapFor (*mc->getTrack());
            const auto startBeat = clip->getStartBeat().inBeats();
            const float rowH = (r.getHeight() - 4.0f) / 3.0f;

            for (auto n : mc->getSequence().getNotes())
            {
                auto piece = DrumTrack::pieceForNote (map, n->getNoteNumber());
                if (! piece) continue;

                const int row = (int) drums::partOf (*piece);
                const float x = viewState.beatToX (startBeat + n->getStartBeat().inBeats()) - (float) getX();
                const float y = r.getY() + 2.0f + rowH * ((float) row + 0.5f);
                const float v = (float) n->getVelocity() / 127.0f;
                const float d = juce::jlimit (2.5f, 7.0f, rowH * (0.35f + 0.35f * v));
                g.setColour (colour.withAlpha (0.45f + 0.55f * v));
                g.fillEllipse (juce::Rectangle<float> (d, d).withCentre ({ x + d * 0.5f, y }));
            }
        }

        if (selected)
            paintSelection (g);
    }

    /** The clip's rounded body in its track's tint. Returns the area inside it. */
    juce::Rectangle<float> paintTintBody (juce::Graphics& g, juce::Colour colour)
    {
        auto body = getLocalBounds().toFloat().reduced (1.0f, 1.5f);
        g.setColour (Colours::background);
        g.fillRoundedRectangle (body, 6.0f);
        g.setColour (Theme::tint (colour));
        g.fillRoundedRectangle (body, 6.0f);
        return body;
    }

    /** Selected: thin ink lines at both ends with a dot on top, like Somewhere's trim handles. */
    void paintSelection (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f, 1.5f);
        g.setColour (Colours::text);

        for (auto x : { r.getX(), r.getRight() - 1.5f })
        {
            g.fillRect (juce::Rectangle<float> (x, r.getY() + 3.0f, 1.5f, r.getHeight() - 3.0f));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x + 0.75f, r.getY() + 3.5f }));
        }
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        auto zone = getDragZone (e.x);
        setMouseCursor (zone == DragZone::body ? juce::MouseCursor::NormalCursor : juce::MouseCursor::LeftRightResizeCursor);
        setHoverLoopHandle (zone == DragZone::loopEnd);
    }

    void mouseExit (const juce::MouseEvent&) override  { setHoverLoopHandle (false); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& sm = viewState.selectionManager;

        if (e.mods.isShiftDown() || e.mods.isCommandDown())
            sm.select (clip.get(), true);
        else if (! sm.isSelected (clip.get()))
            sm.selectOnly (clip.get());

        viewState.focusTrack (clip->getTrack());

        if (e.mods.isPopupMenu())
        {
            showClipMenu();
            return;
        }

        clip->edit.getUndoManager().beginNewTransaction ("Edit clip");

        dragZone = getDragZone (e.x);
        dragStartPos = clip->getPosition();
        dragStartBeat = clip->getStartBeat().inBeats();
        dragStartEndBeat = clip->getEndBeat().inBeats();
        dragStartTrack = dynamic_cast<te::ClipTrack*> (clip->getTrack());
        dragStartLoop = clip->getLoopRangeBeats();
        dragged = false;

        // Alt-drag duplicates, leaving the original in place
        if (e.mods.isAltDown() && dragZone == DragZone::body)
            if (auto dup = te::duplicateClip (*clip))
                dup->setPosition (dragStartPos);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;

        auto deltaBeats = e.getDistanceFromDragStartX() / viewState.pixelsPerBeat;

        if (! dragged && std::abs (e.getDistanceFromDragStartX()) < 3 && std::abs (e.getDistanceFromDragStartY()) < 3)
            return;

        dragged = true;
        auto bypassSnap = e.mods.isCommandDown();
        auto& ts = clip->edit.tempoSequence;

        switch (dragZone)
        {
            case DragZone::body:
            {
                auto newStartBeat = viewState.snapBeat (dragStartBeat + deltaBeats, bypassSnap);
                clip->setStart (ts.toTime (te::BeatPosition::fromBeats (newStartBeat)), false, true);

                // Show vertical drag to another track by nudging the component; applied on mouse up
                setTopLeftPosition (getX(), e.getDistanceFromDragStartY());
                break;
            }
            case DragZone::leftEdge:
            {
                auto newStartBeat = viewState.snapBeat (dragStartBeat + deltaBeats, bypassSnap);
                newStartBeat = juce::jmin (newStartBeat, dragStartEndBeat - 1.0 / 16.0);
                clip->setStart (ts.toTime (te::BeatPosition::fromBeats (newStartBeat)), true, false);
                break;
            }
            case DragZone::rightEdge:
            {
                auto newEndBeat = viewState.snapBeat (dragStartEndBeat + deltaBeats, bypassSnap);
                newEndBeat = juce::jmax (newEndBeat, clip->getStartBeat().inBeats() + 1.0 / 16.0);
                auto newEnd = ts.toTime (te::BeatPosition::fromBeats (newEndBeat));
                clip->setLength (newEnd - clip->getPosition().getStart(), true);
                break;
            }
            case DragZone::loopEnd:
            {
                // The loop keeps its start; the handle sets its length, on the grid unless Cmd is
                // held. Past the clip's end it lengthens the loop and the clip shows one pass
                const auto start = dragStartLoop.getStart();
                const auto offset = clip->getOffsetInBeats().inBeats();
                const auto handleStart = dragStartBeat + dragStartLoop.getLength().inBeats() - offset;
                auto beat = viewState.snapBeat (handleStart + deltaBeats, bypassSnap);
                auto length = juce::jmax (1.0 / 16.0, beat - dragStartBeat + offset);
                clip->setLoopRangeBeats ({ start, start + te::BeatDuration::fromBeats (length) });
                repaint();
                break;
            }
            case DragZone::fadeIn:
            case DragZone::fadeOut:
            {
                if (auto ac = dynamic_cast<te::AudioClipBase*> (clip.get()))
                {
                    auto len = clip->getPosition().getLength().inSeconds();
                    auto x = (float) e.getPosition().x;
                    auto secs = juce::jlimit (0.0, len, (double) x / getWidth() * len);

                    if (dragZone == DragZone::fadeIn)
                        ac->setFadeIn (te::TimeDuration::fromSeconds (secs));
                    else
                        ac->setFadeOut (te::TimeDuration::fromSeconds (len - secs));
                }
                break;
            }
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragZone == DragZone::body && dragged && onDropOnTrackAt != nullptr)
        {
            auto posInParent = e.getEventRelativeTo (getParentComponent()).getPosition();
            onDropOnTrackAt (*this, posInParent.y);
        }

        if (dragged && viewState.editMode == EditViewState::EditMode::shuffle)
            applyShuffle();

        // Spot: a click (not a drag) asks where the clip should start
        if (! dragged && dragZone == DragZone::body && ! e.mods.isPopupMenu() && ! e.mods.isAnyModifierKeyDown()
             && viewState.editMode == EditViewState::EditMode::spot && e.getNumberOfClicks() == 1)
            EditModes::showSpotDialog (*clip);

        dragged = false;

        if (auto p = getParentComponent())
            p->resized();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        viewState.detailMode = EditViewState::DetailMode::clip;
        viewState.sendChangeMessage();
    }

    /** Set by the lane: moves the clip to the track under the given y (relative to the lane). */
    std::function<void (ArrangementClip&, int)> onDropOnTrackAt;

private:
    enum class DragZone { body, leftEdge, rightEdge, fadeIn, fadeOut, loopEnd };

    EditViewState& viewState;
    te::Clip::Ptr clip;
    std::unique_ptr<te::SmartThumbnail> thumbnail;

    DragZone dragZone = DragZone::body;
    te::ClipPosition dragStartPos;
    te::ClipTrack* dragStartTrack = nullptr;
    double dragStartBeat = 0.0, dragStartEndBeat = 0.0;
    te::BeatRange dragStartLoop;
    bool dragged = false, hoverLoopHandle = false;

    /** Where the first repeat of a looped clip starts, if it starts inside the clip. */
    std::optional<float> getLoopHandleX() const
    {
        if (! clip->isLooping())
            return {};

        const auto loopLen = clip->getLoopLengthBeats().inBeats();

        if (loopLen <= 0.0)
            return {};

        const auto b = clip->getStartBeat().inBeats() + loopLen - clip->getOffsetInBeats().inBeats();

        if (b <= clip->getStartBeat().inBeats() || b >= clip->getEndBeat().inBeats())
            return {};

        return viewState.beatToX (b) - (float) getX();
    }

    void setHoverLoopHandle (bool h)
    {
        if (h != hoverLoopHandle)
        {
            hoverLoopHandle = h;
            repaint();
        }
    }

    /** Shuffle, after a drag: the track closes up around the change. */
    void applyShuffle()
    {
        auto& edit = clip->edit;
        auto id = clip->itemID;
        auto from = dragStartPos.time;
        auto fromTrack = te::Track::Ptr (dragStartTrack);

        if (dragZone == DragZone::body)
        {
            // After any move to another track, which the lane does asynchronously
            juce::MessageManager::callAsync ([&edit, id, from, fromTrack]
            {
                if (auto c = te::findClipForID (edit, id))
                    EditModes::shuffleDrop (*c, dynamic_cast<te::ClipTrack*> (fromTrack.get()), from);
            });
        }
        else if (dragZone == DragZone::rightEdge && dragStartTrack != nullptr)
        {
            EditModes::shiftClipsFrom (*dragStartTrack, from.getEnd(), clip->getPosition().getEnd() - from.getEnd(), clip.get());
        }
        else if (dragZone == DragZone::leftEdge && dragStartTrack != nullptr)
        {
            // The clip keeps its start; what was trimmed off comes out of the clips after it
            auto trimmed = clip->getPosition().getStart() - from.getStart();
            clip->setStart (from.getStart(), false, true);
            EditModes::shiftClipsFrom (*dragStartTrack, from.getEnd(), te::TimeDuration() - trimmed, clip.get());
        }
    }

    DragZone getDragZone (int x) const
    {
        const int edge = juce::jmin (6, getWidth() / 4);

        if (dynamic_cast<te::AudioClipBase*> (clip.get()) != nullptr && getHeight() > 30)
        {
            // Small squares at the top corners set the fades
            if (x < 8 && juce::Desktop::getMousePosition().y - getScreenY() < 14) return DragZone::fadeIn;
            if (x > getWidth() - 8 && juce::Desktop::getMousePosition().y - getScreenY() < 14) return DragZone::fadeOut;
        }

        if (x < edge)                return DragZone::leftEdge;
        if (x > getWidth() - edge)   return DragZone::rightEdge;

        if (auto loopX = getLoopHandleX(); loopX && std::abs ((float) x - *loopX) <= 4.0f)
            return DragZone::loopEnd;

        return DragZone::body;
    }

    void drawWaveform (juce::Graphics& g, te::AudioClipBase& ac, juce::Rectangle<float> r, juce::Colour colour)
    {
        if (thumbnail == nullptr)
            return;

        if (thumbnail->isGeneratingProxy())
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("Analysing... " + juce::String (juce::roundToInt (thumbnail->getProxyProgress() * 100.0f)) + "%",
                        r.toNearestInt(), juce::Justification::centred);
            return;
        }

        g.setColour (colour);
        auto gain = ac.getGain();

        // The playback file already includes warping/looping for auto-tempo clips; for plain
        // clips we map the visible part of the clip onto the source file.
        auto clipLen = ac.getPosition().getLength();
        auto offset = ac.getPosition().getOffset();
        auto speed = ac.getSpeedRatio();

        if (ac.usesTimeStretchedProxy() || ac.isLooping())
        {
            auto t2 = te::TimePosition::fromSeconds (thumbnail->getTotalLength());
            auto len = juce::jmin (t2, te::toPosition (clipLen + offset));
            drawChannels (g, r, { te::toPosition (offset), len }, gain);
        }
        else
        {
            auto start = te::TimePosition::fromSeconds (offset.inSeconds() * speed);
            auto end = te::TimePosition::fromSeconds ((offset + clipLen).inSeconds() * speed);
            drawChannels (g, r, { start, end }, gain);
        }
    }

    void drawChannels (juce::Graphics& g, juce::Rectangle<float> r, te::TimeRange range, float gain)
    {
        auto area = r.toNearestInt();

        if (thumbnail->getNumChannels() > 1 && area.getHeight() > 30)
        {
            thumbnail->drawChannel (g, area.removeFromTop (area.getHeight() / 2), range, 0, gain);
            thumbnail->drawChannel (g, area, range, 1, gain);
        }
        else
        {
            thumbnail->drawChannels (g, area, range, gain);
        }
    }

    void drawFades (juce::Graphics& g, te::AudioClipBase& ac)
    {
        auto len = ac.getPosition().getLength().inSeconds();
        if (len <= 0.0) return;

        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::text.withAlpha (0.8f));

        auto fadeIn = ac.getFadeIn().inSeconds();
        auto fadeOut = ac.getFadeOut().inSeconds();

        if (fadeIn > 0.0)
        {
            auto x = (float) (fadeIn / len) * r.getWidth();
            g.drawLine (r.getX(), r.getBottom(), r.getX() + x, r.getY() + 14.0f, 1.0f);
        }

        if (fadeOut > 0.0)
        {
            auto x = r.getRight() - (float) (fadeOut / len) * r.getWidth();
            g.drawLine (x, r.getY() + 14.0f, r.getRight(), r.getBottom(), 1.0f);
        }
    }

    void showClipMenu()
    {
        juce::PopupMenu m;
        auto clipRef = clip;
        auto& edit = clip->edit;
        auto* evs = &viewState;

        m.addItem ("Split at Playhead", [clipRef, &edit]
        {
            if (auto ct = dynamic_cast<te::ClipTrack*> (clipRef->getTrack()))
                ct->splitClip (*clipRef, edit.getTransport().getPosition());
        });
        m.addItem ("Duplicate", [clipRef]
        {
            if (auto dup = te::duplicateClip (*clipRef))
                dup->setStart (clipRef->getPosition().getEnd(), false, true);
        });
        m.addItem (clip->disabled ? "Unmute Clip" : "Mute Clip", [clipRef] { clipRef->disabled = ! clipRef->disabled.get(); });

        if (viewState.addComment)
            m.addItem ("Add Comment...", [clipRef, evs]
            {
                evs->addComment (clipRef->itemID, clipRef->getEditTimeRange(), clipRef->getName());
            });

        if (clip->canLoop())
            m.addItem ("Loop", true, clip->isLooping(), [clipRef]
            {
                if (clipRef->isLooping())
                    clipRef->disableLooping();
                else
                    clipRef->setLoopRangeBeats ({ te::BeatPosition() + clipRef->getOffsetInBeats(), clipRef->getLengthInBeats() });
            });

        if (auto ac = dynamic_cast<te::AudioClipBase*> (clip.get()))
        {
            auto acRef = te::AudioClipBase::Ptr (ac);
            m.addItem ("Warp (follow tempo)", true, ac->getAutoTempo(), [acRef] { acRef->setAutoTempo (! acRef->getAutoTempo()); });
            m.addItem ("Reverse", true, ac->getIsReversed(), [acRef] { acRef->setIsReversed (! acRef->getIsReversed()); });

            // This clip, plus the others selected with it
            juce::Array<te::AudioClipBase::Ptr> toNormalise { acRef };

            if (viewState.selectionManager.isSelected (ac))
                for (auto other : viewState.selectionManager.getItemsOfType<te::AudioClipBase>())
                    if (other != ac)
                        toNormalise.add (other);

            m.addItem (toNormalise.size() > 1 ? "Normalize " + juce::String (toNormalise.size()) + " Clips..." : "Normalize...", [toNormalise]
            {
                juce::Array<te::AudioClipBase*> clips;
                for (auto& c : toNormalise)
                    clips.add (c.get());
                Normalise::showForClips (clips);
            });
        }

        // Clips take their track's colour; this colours the track (or its whole group)
        if (auto t = clip->getTrack())
            m.addSubMenu (t->getParentFolderTrack() != nullptr ? "Group Colour" : "Track Colour", colourMenu (*t));

        m.addSeparator();
        m.addItem ("Delete", [clipRef, evs]
        {
            evs->selectionManager.deselectAll();
            EditModes::deleteClips ({ clipRef.get() }, evs->editMode == EditViewState::EditMode::shuffle);
        });

        m.showMenuAsync ({}, [clipRef] (int result)
        {
            if (auto t = clipRef->getTrack())
                applyColourChoice (*t, result);
        });
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override    { repaint(); }
};

//==============================================================================
/** A group's lane: everything the group plays as one combined waveform in the group's colour, so
    you can still see what's in a group when it's closed. Audio clips add their waveforms and MIDI
    clips the notes they play (by velocity), as powers, the way signals mix. When the group is open
    it's drawn quietly, as its tracks are right below it. */
class GroupOverview
{
public:
    explicit GroupOverview (juce::Component& c) : owner (c) {}

    void paint (juce::Graphics& g, te::FolderTrack& group, EditViewState& vs, juce::Rectangle<float> area, bool closed)
    {
        juce::Array<te::Clip*> clips;

        for (auto t : group.getAllSubTracks (true))
            if (auto ct = dynamic_cast<te::ClipTrack*> (t); ct != nullptr && ! t->isMuted (true))
                for (auto c : ct->getClips())
                    if (! c->disabled)
                        clips.add (c);

        forgetThumbnailsExcept (clips);

        if (clips.isEmpty())
            return;

        constexpr float step = 2.0f;
        const auto colour = Theme::ofTrack (group);
        const int n = juce::jmax (1, (int) std::ceil (area.getWidth() / step));
        std::vector<float> power ((size_t) n, 0.0f);
        juce::Array<juce::Range<float>> spans;

        for (auto c : clips)
        {
            const auto x1 = vs.timeToX (c->getPosition().getStart()), x2 = vs.timeToX (c->getPosition().getEnd());

            if (x2 < area.getX() || x1 > area.getRight())
                continue;

            spans.add ({ x1, x2 });
            const int i1 = juce::jlimit (0, n, (int) std::floor ((x1 - area.getX()) / step));
            const int i2 = juce::jlimit (0, n, (int) std::ceil ((x2 - area.getX()) / step));

            if (auto ac = dynamic_cast<te::WaveAudioClip*> (c))
                addAudio (power, *ac, vs, area.getX(), step, i1, i2);
            else if (auto mc = dynamic_cast<te::MidiClip*> (c))
                addNotes (power, *mc, vs, area.getX(), step, i1, i2);
        }

        // The clips' extent, merged, as rounded bodies
        std::sort (spans.begin(), spans.end(), [] (auto a, auto b) { return a.getStart() < b.getStart(); });
        juce::Array<juce::Range<float>> merged;
        for (auto s : spans)
            if (! merged.isEmpty() && s.getStart() <= merged.getLast().getEnd() + 1.0f)
                merged.getReference (merged.size() - 1).setEnd (juce::jmax (merged.getLast().getEnd(), s.getEnd()));
            else
                merged.add (s);

        auto bodyArea = area.reduced (0.0f, 1.5f);
        for (auto s : merged)
        {
            auto body = juce::Rectangle<float> (s.getStart() + 1.0f, bodyArea.getY(), s.getLength() - 2.0f, bodyArea.getHeight());
            g.setColour (closed ? Theme::tint (colour) : Colours::panel);
            g.fillRoundedRectangle (body, 6.0f);
        }

        // The combined waveform, mirrored about the middle
        auto wave = bodyArea.reduced (0.0f, 5.0f);
        const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
        juce::Path columns;

        for (int i = 0; i < n; ++i)
            if (const float level = juce::jmin (1.0f, std::sqrt (power[(size_t) i])); level > 0.004f)
                columns.addRectangle (area.getX() + (float) i * step, mid - level * half, step, juce::jmax (1.0f, level * half * 2.0f));

        g.setColour (closed ? colour : colour.withAlpha (0.75f));
        g.fillPath (columns);
    }

private:
    juce::Component& owner;
    std::map<juce::String, std::unique_ptr<te::SmartThumbnail>> thumbnails;

    te::SmartThumbnail* thumbnailFor (te::WaveAudioClip& clip)
    {
        auto& t = thumbnails[clip.itemID.toString()];

        if (t == nullptr)
            t = std::make_unique<te::SmartThumbnail> (clip.edit.engine, clip.getPlaybackFile(), owner, &clip.edit);

        return t.get();
    }

    void forgetThumbnailsExcept (const juce::Array<te::Clip*>& clips)
    {
        for (auto it = thumbnails.begin(); it != thumbnails.end();)
        {
            bool used = false;
            for (auto c : clips)
                used = used || c->itemID.toString() == it->first;

            it = used ? std::next (it) : thumbnails.erase (it);
        }
    }

    void addAudio (std::vector<float>& power, te::WaveAudioClip& ac, EditViewState& vs, float x0, float step, int i1, int i2)
    {
        auto* th = thumbnailFor (ac);

        if (th == nullptr || th->getNumChannels() == 0 || th->isGeneratingProxy())
            return;

        // The same mapping from the timeline onto the file as the clip's own waveform
        const auto clipStart = ac.getPosition().getStart().inSeconds();
        const auto offset = ac.getPosition().getOffset().inSeconds();
        const auto speed = (ac.usesTimeStretchedProxy() || ac.isLooping()) ? 1.0 : ac.getSpeedRatio();
        const auto gain = ac.getGain();

        for (int i = i1; i < i2; ++i)
        {
            const auto t0 = vs.xToTime (x0 + (float) i * step).inSeconds(), t1 = vs.xToTime (x0 + (float) (i + 1) * step).inSeconds();
            const auto s0 = (offset + (t0 - clipStart)) * speed, s1 = (offset + (t1 - clipStart)) * speed;
            float level = 0.0f;

            for (int ch = 0; ch < th->getNumChannels(); ++ch)
            {
                float lo = 0.0f, hi = 0.0f;
                th->getApproximateMinMax (s0, s1, ch, lo, hi);
                level = juce::jmax (level, std::abs (lo), std::abs (hi));
            }

            level *= gain;
            power[(size_t) i] += level * level;
        }
    }

    static void addNotes (std::vector<float>& power, te::MidiClip& mc, EditViewState& vs, float x0, float step, int i1, int i2)
    {
        const auto clipStart = mc.getStartBeat().inBeats(), clipEnd = mc.getEndBeat().inBeats();
        const auto offset = mc.getOffsetInBeats().inBeats();
        const auto loopLen = mc.isLooping() ? mc.getLoopLengthBeats().inBeats() : 0.0;
        const auto loopStart = mc.isLooping() ? mc.getLoopStartBeats().inBeats() : 0.0;
        const auto decayPx = (float) vs.pixelsPerBeat * 0.6f;

        auto addNote = [&] (double startBeat, double endBeat, float velocity)
        {
            startBeat = juce::jmax (startBeat, clipStart);
            endBeat = juce::jmin (endBeat, clipEnd);
            if (endBeat <= startBeat) return;

            const auto xs = vs.beatToX (startBeat), xe = vs.beatToX (endBeat);
            const int a = juce::jmax (i1, (int) std::floor ((xs - x0) / step)), b = juce::jmin (i2, (int) std::ceil ((xe - x0) / step));

            for (int i = a; i < b; ++i)
            {
                const float since = x0 + (float) i * step - xs;
                const float level = 0.7f * velocity * (0.35f + 0.65f * std::exp (-juce::jmax (0.0f, since) / decayPx));
                power[(size_t) i] += level * level * 0.5f;
            }
        };

        for (auto n : mc.getSequence().getNotes())
        {
            const float v = (float) n->getVelocity() / 127.0f;

            if (loopLen > 0.0)
            {
                const auto s = n->getStartBeat().inBeats() - loopStart, e = juce::jmin (n->getEndBeat().inBeats() - loopStart, loopLen);
                if (s < 0.0 || s >= loopLen) continue;

                for (double rep = clipStart - offset; rep < clipEnd; rep += loopLen)
                    addNote (rep + s, rep + e, v);
            }
            else
            {
                addNote (clipStart + n->getStartBeat().inBeats() - offset, clipStart + n->getEndBeat().inBeats() - offset, v);
            }
        }
    }
};

//==============================================================================
/** The horizontal strip where a track's clips live. */
class TrackLane : public juce::Component,
                  public juce::FileDragAndDropTarget,
                  public juce::DragAndDropTarget,
                  private juce::ValueTree::Listener,
                  private juce::AsyncUpdater
{
public:
    TrackLane (EditViewState& evs, te::Track& t) : viewState (evs), track (&t)
    {
        track->state.addListener (this);
        rebuildClips();
    }

    ~TrackLane() override
    {
        track->state.removeListener (this);
    }

    te::Track* getTrack() const    { return track.get(); }

    /** Set by the parent: moves a clip onto whichever track is at a y position relative to this lane. */
    std::function<void (te::Clip&, int yInLane)> onMoveClipToY;
    std::function<void (const juce::File&, te::Track&, te::TimePosition)> onFileDropped;
    std::function<void (const juce::StringArray&, te::TimePosition)> onFilesDroppedAsNewTracks;

    void paint (juce::Graphics& g) override
    {
        auto isSelected = viewState.focusedTrack == track.get();
        g.fillAll (isSelected ? Theme::lift (Colours::background, 0.04f) : Colours::background);
        paintGrid (g);

        if (auto group = dynamic_cast<te::FolderTrack*> (track.get()))
            overview.paint (g, *group, viewState, getLocalBounds().toFloat(), viewState.collapsedGroups.count (track->itemID) > 0);

        if (isDragOver)
        {
            g.setColour (Colours::accent.withAlpha (0.15f));
            g.fillRect (getLocalBounds());
        }

        // Recording in progress on this track
        if (auto at = dynamic_cast<te::AudioTrack*> (track.get()))
        {
            auto& transport = track->edit.getTransport();

            if (transport.isRecording() && EngineHelpers::isTrackArmed (*at))
            {
                auto x1 = viewState.timeToX (transport.getTimeWhenStarted());
                auto x2 = viewState.timeToX (transport.getPosition());
                g.setColour (Colours::record.withAlpha (0.35f));
                g.fillRoundedRectangle (juce::Rectangle<float> (x1, 1.5f, juce::jmax (1.0f, x2 - x1), (float) getHeight() - 3.0f), 6.0f);
            }
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        // A drum section being dragged out
        if (newSection)
        {
            auto x1 = viewState.beatToX (newSection->first), x2 = viewState.beatToX (newSection->second);
            auto box = juce::Rectangle<float> (x1, 1.0f, x2 - x1, (float) getHeight() - 2.0f);
            g.setColour (Colours::accent.withAlpha (0.25f));
            g.fillRect (box);
            g.setColour (Theme::readable (Colours::accent));
            g.drawRect (box, 1.5f);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (juce::String (juce::roundToInt ((newSection->second - newSection->first) / viewState.getBeatsPerBar())) + " bars",
                        box.reduced (4).toNearestInt(), juce::Justification::topLeft);
        }

        // Playing Session clips: the Arrangement clips on this track are silent (like Ableton's greyed-out clips)
        if (auto at = dynamic_cast<te::AudioTrack*> (track.get()); at != nullptr && EngineHelpers::isPlayingSessionClips (*at))
        {
            g.setColour (Colours::background.withAlpha (0.6f));
            g.fillRect (getLocalBounds());
            g.setColour (Theme::readable (Colours::accent));
            g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
            g.drawText (juce::String::fromUTF8 ("Playing Session clips \xe2\x80\x94 click \xe2\x86\xa9 on the track header (or Back to Arrangement) to hear these clips"),
                        getLocalBounds().reduced (8, 4), juce::Justification::topLeft, true);
        }

        // Frozen tracks: tint and label, clips stay visible underneath
        if (auto at = dynamic_cast<te::AudioTrack*> (track.get()); at != nullptr && at->isFrozen (te::Track::individualFreeze))
        {
            g.setColour (Colours::solo.withAlpha (0.18f));
            g.fillRect (getLocalBounds());
            g.setColour (Theme::readable (Colours::solo));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("FROZEN", getLocalBounds().reduced (6, 2), juce::Justification::topRight);
        }
    }

    void paintGrid (juce::Graphics& g)
    {
        auto beatsPerBar = viewState.getBeatsPerBar();
        auto grid = viewState.getEffectiveGrid();
        auto start = std::floor (viewState.viewStartBeat / grid) * grid;
        auto end = viewState.xToBeat ((float) getWidth());

        // Orbit: barely-there lines, a little firmer on bars
        for (double b = start; b < end; b += grid)
        {
            bool isBar = std::abs (std::fmod (b, beatsPerBar)) < 1e-6;
            g.setColour (isBar ? Colours::panel : Colours::gridLine);
            g.drawVerticalLine (juce::roundToInt (viewState.beatToX (b)), 0.0f, (float) getHeight());
        }
    }

    void resized() override
    {
        for (auto* c : clips)
        {
            auto& clip = c->getClip();
            auto x1 = viewState.timeToX (clip.getPosition().getStart());
            auto x2 = viewState.timeToX (clip.getPosition().getEnd());
            c->setBounds (juce::roundToInt (x1), 1, juce::jmax (4, juce::roundToInt (x2 - x1)), getHeight() - 2);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        viewState.focusTrack (track.get());

        if (! e.mods.isShiftDown())
            viewState.selectionManager.selectOnly (track.get());

        auto t = viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) e.x)));
        track->edit.getTransport().setPosition (t);
    }

    // Drum tracks: drag across empty bars to create a section
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! DrumTrack::isDrumTrack (*track) || e.mods.isPopupMenu())
            return;

        auto bar = viewState.getBeatsPerBar();
        auto a = std::floor (viewState.xToBeat ((float) e.getMouseDownX()) / bar) * bar;
        auto b = std::ceil (viewState.xToBeat ((float) e.x) / bar) * bar;
        newSection = { juce::jmin (a, b - bar), juce::jmax (a + bar, b) };
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! newSection)
            return;

        auto range = *newSection;
        newSection.reset();
        repaint();

        if (auto at = dynamic_cast<te::AudioTrack*> (track.get()))
            createDrumSection (*at, range.first, range.second);
    }

    void createDrumSection (te::AudioTrack& at, double startBeat, double endBeat)
    {
        at.edit.getUndoManager().beginNewTransaction ("Create drum section");

        // Carry on in the style of the previous section, if there is one
        auto style = drums::Style::rock;
        for (auto c : at.getClips())
            if (DrumTrack::isSection (*c) && c->getStartBeat().inBeats() < startBeat)
                style = DrumTrack::readSettings (c->state).style;

        if (auto mc = DrumTrack::createSection (at, startBeat, endBeat, "Verse", style))
        {
            viewState.selectionManager.selectOnly (mc);
            viewState.detailMode = EditViewState::DetailMode::clip;
            viewState.sendChangeMessage();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Drum tracks: double-click makes a 4-bar section
        if (auto at = dynamic_cast<te::AudioTrack*> (track.get()); at != nullptr && DrumTrack::isDrumTrack (*at))
        {
            auto bar = viewState.getBeatsPerBar();
            auto startBeat = std::floor (viewState.xToBeat ((float) e.x) / bar) * bar;
            createDrumSection (*at, startBeat, startBeat + bar * 4);
            return;
        }

        // Double-click on an empty lane creates a one-bar MIDI clip (like drawing in Ableton)
        if (auto at = dynamic_cast<te::AudioTrack*> (track.get()))
        {
            auto bar = viewState.getBeatsPerBar();
            auto startBeat = std::floor (viewState.xToBeat ((float) e.x) / bar) * bar;
            auto& ts = track->edit.tempoSequence;
            auto range = te::TimeRange (ts.toTime (te::BeatPosition::fromBeats (startBeat)),
                                        ts.toTime (te::BeatPosition::fromBeats (startBeat + bar)));

            track->edit.getUndoManager().beginNewTransaction ("Create MIDI clip");

            if (auto mc = at->insertMIDIClip ("MIDI Clip", range, nullptr))
            {
                viewState.selectionManager.selectOnly (mc.get());
                viewState.detailMode = EditViewState::DetailMode::clip;
                viewState.sendChangeMessage();
            }
        }
    }

    // External files (Finder)
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        if (dynamic_cast<te::AudioTrack*> (track.get()) == nullptr)
            return false;

        for (auto& f : files)
            if (juce::File (f).hasFileExtension (te::soundFileAndMidiExtensions))
                return true;

        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override   { isDragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override              { isDragOver = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int x, int) override
    {
        isDragOver = false;
        auto t = viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) x)));

        // Several files at once (e.g. stems) -> one new track each, all lined up
        if (files.size() > 1 && onFilesDroppedAsNewTracks)
            onFilesDroppedAsNewTracks (files, t);
        else if (onFileDropped && ! files.isEmpty())
            onFileDropped (juce::File (files[0]), *track, t);

        repaint();
    }

    // Internal drags (from the browser panel)
    bool isInterestedInDragSource (const SourceDetails& d) override
    {
        if (dynamic_cast<te::AudioTrack*> (track.get()) == nullptr)
            return false;

        return getDraggedFile (d).existsAsFile() || getDraggedClip (track->edit, d) != nullptr;
    }

    void itemDragEnter (const SourceDetails&) override { isDragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override  { isDragOver = false; repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        isDragOver = false;
        auto t = viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) d.localPosition.x)));

        // A clip from the clip list: a copy of it goes here
        if (auto source = getDraggedClip (track->edit, d))
        {
            auto& edit = track->edit;
            edit.getUndoManager().beginNewTransaction ("Place " + source->getName());

            if (auto dup = te::duplicateClip (*source))
            {
                dup->setStart (t, false, true);

                if (auto target = dynamic_cast<te::ClipTrack*> (track.get()); target != nullptr && target != source->getTrack())
                    dup->moveTo (*target);
            }

            repaint();
            return;
        }

        if (onFileDropped)
            onFileDropped (getDraggedFile (d), *track, t);

        repaint();
    }

    void updateLayout()
    {
        resized();
        repaint();
    }

private:
    EditViewState& viewState;
    te::Track::Ptr track;
    juce::OwnedArray<ArrangementClip> clips;
    GroupOverview overview { *this };
    bool isDragOver = false;
    std::optional<std::pair<double, double>> newSection;     // drum section being dragged out (beats)

    void rebuildClips()
    {
        clips.clear();

        if (auto ct = dynamic_cast<te::ClipTrack*> (track.get()))
        {
            for (auto c : ct->getClips())
            {
                auto* cc = clips.add (new ArrangementClip (viewState, *c));
                cc->onDropOnTrackAt = [this] (ArrangementClip& comp, int y)
                {
                    if (onMoveClipToY && (y < 0 || y >= getHeight()))
                        onMoveClipToY (comp.getClip(), y);
                };
                addAndMakeVisible (cc);
            }
        }

        resized();
    }

    void handleAsyncUpdate() override
    {
        rebuildClips();
        repaint();
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override
    {
        if (te::Clip::isClipState (c)) triggerAsyncUpdate();
    }

    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override
    {
        if (te::Clip::isClipState (c)) triggerAsyncUpdate();
    }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (v == track->state)
            juce::MessageManager::callAsync ([sp = SafePointer<TrackLane> (this)] { if (sp) sp->repaint(); });

        if (te::Clip::isClipState (v))
        {
            if (id == te::IDs::start || id == te::IDs::length || id == te::IDs::offset)
                juce::MessageManager::callAsync ([sp = SafePointer<TrackLane> (this)] { if (sp) sp->resized(); });

            juce::MessageManager::callAsync ([sp = SafePointer<TrackLane> (this)]
            {
                if (sp)
                {
                    for (auto* c : sp->clips)
                    {
                        c->updateThumbnail();
                        c->repaint();
                    }

                    if (sp->track->isFolderTrack())
                        sp->repaint();
                }
            });
        }
    }
};

//==============================================================================
/** Bar numbers + loop region at the top of the arrangement. Click to move the playhead,
    drag in the lower half to set the loop. */
class TimelineRuler : public juce::Component, private juce::Timer
{
public:
    TimelineRuler (EditViewState& evs) : viewState (evs) { startTimerHz (15); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);
        auto& transport = viewState.edit.getTransport();

        // Loop region: a capsule behind the bar numbers, raised with a dot at each end when looping.
        // The dot under the mouse grows, as it's the handle for that end.
        // With punch on the same brace marks where recording happens, so it turns record red.
        auto brace = braceBounds();
        const bool punch = EngineHelpers::isPunching (viewState.edit);
        g.setColour (punch ? Colours::record.withAlpha (0.28f) : transport.looping ? Colours::panelLight : Colours::panel);
        g.fillRoundedRectangle (brace, brace.getHeight() * 0.5f);

        if (isBraceActive())
        {
            g.setColour (punch ? Colours::record : Colours::text);
            for (auto end : { -1, 1 })
            {
                auto size = end == activeEnd ? 8.0f : 4.0f;
                g.fillEllipse (juce::Rectangle<float> (size, size).withCentre ({ endDotX (end), brace.getCentreY() }));
            }
        }

        auto beatsPerBar = viewState.getBeatsPerBar();
        auto end = viewState.xToBeat ((float) getWidth());

        // Pick a bar label spacing that doesn't overlap
        int barStep = 1;
        while (barStep * beatsPerBar * viewState.pixelsPerBeat < 40.0)
            barStep *= 2;

        g.setFont (juce::FontOptions (10.0f));

        auto firstBar = (int) std::floor (viewState.viewStartBeat / beatsPerBar);

        for (int bar = firstBar; bar * beatsPerBar < end; ++bar)
        {
            if (bar % barStep != 0)
                continue;

            auto x = viewState.beatToX (bar * beatsPerBar);
            g.setColour (bar % (barStep * 4) == 0 ? Colours::textDim : Theme::indicator (Colours::gridBar));
            g.drawText (juce::String (bar + 1), juce::Rectangle<float> (x + 4.0f, 0.0f, 40.0f, (float) getHeight()).toNearestInt(),
                        juce::Justification::centredLeft);
        }

        // Playhead: an ink dot at the foot of the line
        auto px = viewState.timeToX (transport.getPosition());
        g.setColour (Colours::playhead);
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ px + 0.5f, (float) getHeight() - 4.5f }));
    }

    /*  Mouse: the top half moves the playhead (click or scrub). The bottom half is the loop's:
        drag inside it to move it, click inside it to turn looping on or off, drag outside it to
        draw a new one. While looping, the dot at either end can be dragged from anywhere up or
        down the ruler to move that end. */
    void mouseMove (const juce::MouseEvent& e) override
    {
        auto end = loopEndAt ((float) e.x);
        auto overLoop = e.y > getHeight() / 2 && braceBounds().getHorizontalRange().contains ((float) e.x);

        setMouseCursor (end != 0   ? juce::MouseCursor::LeftRightResizeCursor
                        : overLoop ? juce::MouseCursor::DraggingHandCursor
                                   : juce::MouseCursor::NormalCursor);
        setActiveEnd (end);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (! isMouseButtonDown())
            setActiveEnd (0);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto& transport = viewState.edit.getTransport();
        auto beat = viewState.snapBeat (viewState.xToBeat ((float) e.x));
        dragStartBeat = beat;
        loopDragStart = transport.getLoopRange();

        if (auto end = loopEndAt ((float) e.x); end != 0)
        {
            drag = end < 0 ? Drag::loopStart : Drag::loopEnd;
            grabOffset = (float) e.x - viewState.timeToX (end < 0 ? loopDragStart.getStart() : loopDragStart.getEnd());
        }
        else if (e.y > getHeight() / 2)
        {
            auto inside = (float) e.x >= viewState.timeToX (loopDragStart.getStart())
                       && (float) e.x <= viewState.timeToX (loopDragStart.getEnd());
            drag = inside && e.getNumberOfClicks() == 1 ? Drag::moveLoop : Drag::newLoop;
        }
        else
        {
            drag = Drag::playhead;
            transport.setPosition (viewState.beatToTime (beat));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& transport = viewState.edit.getTransport();
        auto beat = viewState.snapBeat (viewState.xToBeat ((float) e.x));
        auto startB = viewState.timeToBeat (loopDragStart.getStart());
        auto endB = viewState.timeToBeat (loopDragStart.getEnd());

        switch (drag)
        {
            case Drag::playhead:
                transport.setPosition (viewState.beatToTime (beat));
                break;

            case Drag::moveLoop:
            {
                auto delta = viewState.snapBeat (e.getDistanceFromDragStartX() / viewState.pixelsPerBeat);
                auto newStart = juce::jmax (0.0, startB + delta);
                transport.setLoopRange ({ viewState.beatToTime (newStart), viewState.beatToTime (newStart + endB - startB) });
                break;
            }

            case Drag::newLoop:
                if (std::abs (beat - dragStartBeat) > 0.0)
                {
                    transport.setLoopRange ({ viewState.beatToTime (juce::jmin (beat, dragStartBeat)),
                                              viewState.beatToTime (juce::jmax (beat, dragStartBeat)) });
                    if (! EngineHelpers::isPunching (viewState.edit))
                        transport.looping = true;
                }
                break;

            case Drag::loopStart:
            case Drag::loopEnd:
            {
                // The end follows the mouse, keeping where on the dot it was grabbed; dragged past
                // the other end, the two swap rather than the loop vanishing
                auto moved = juce::jmax (0.0, viewState.snapBeat (viewState.xToBeat ((float) e.x - grabOffset)));
                auto fixed = drag == Drag::loopStart ? endB : startB;

                if (std::abs (moved - fixed) > 1.0e-6)
                    transport.setLoopRange ({ viewState.beatToTime (juce::jmin (moved, fixed)),
                                              viewState.beatToTime (juce::jmax (moved, fixed)) });
                break;
            }
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        // A click on the brace turns it off when it's on (looping or punching) and loops when it's off
        if (drag == Drag::moveLoop && ! e.mouseWasDraggedSinceMouseDown())
        {
            auto& edit = viewState.edit;

            if (EngineHelpers::isPunching (edit))   EngineHelpers::setPunching (edit, false);
            else                                    EngineHelpers::setLooping (edit, ! edit.getTransport().looping.get());
        }

        drag = Drag::playhead;
        setActiveEnd (isMouseOver() ? loopEndAt ((float) e.x) : 0);
        repaint();
    }

private:
    EditViewState& viewState;
    enum class Drag { playhead, newLoop, moveLoop, loopStart, loopEnd };
    Drag drag = Drag::playhead;
    double dragStartBeat = 0.0;
    te::TimeRange loopDragStart;
    float grabOffset = 0.0f;
    int activeEnd = 0;                              // the loop end under the mouse or being dragged: -1 start, 1 end
    static constexpr float endGrabRadius = 7.0f;

    juce::Rectangle<float> braceBounds() const
    {
        auto loop = viewState.edit.getTransport().getLoopRange();
        auto lx1 = viewState.timeToX (loop.getStart());
        auto lx2 = viewState.timeToX (loop.getEnd());
        return { lx1 + 1.0f, 3.0f, juce::jmax (2.0f, lx2 - lx1 - 2.0f), (float) getHeight() - 6.0f };
    }

    float endDotX (int end) const
    {
        auto brace = braceBounds();
        return end < 0 ? brace.getX() + brace.getHeight() * 0.5f : brace.getRight() - brace.getHeight() * 0.5f;
    }

    /** The brace end whose dot is at x while looping or punching: -1 the start, 1 the end, 0 neither. */
    bool isBraceActive() const      { return viewState.edit.getTransport().looping || EngineHelpers::isPunching (viewState.edit); }

    int loopEndAt (float x) const
    {
        if (! isBraceActive())
            return 0;

        auto toStart = std::abs (x - endDotX (-1)), toEnd = std::abs (x - endDotX (1));

        if (juce::jmin (toStart, toEnd) > endGrabRadius)
            return 0;

        return toStart <= toEnd ? -1 : 1;
    }

    void setActiveEnd (int end)
    {
        if (drag == Drag::loopStart || drag == Drag::loopEnd || end == activeEnd)
            return;

        activeEnd = end;
        repaint();
    }

    void timerCallback() override { repaint(); }
};

//==============================================================================
/** Thin vertical line drawn over all lanes. */
class PlayheadOverlay : public juce::Component, private juce::Timer
{
public:
    PlayheadOverlay (EditViewState& evs) : viewState (evs)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colours::playhead.withAlpha (0.9f));
        g.fillRect ((float) x, 0.0f, 1.0f, (float) getHeight());
    }

    std::function<void (float)> onPlayheadMoved;

private:
    EditViewState& viewState;
    int x = -10;

    void timerCallback() override
    {
        auto newX = juce::roundToInt (viewState.timeToX (viewState.edit.getTransport().getPosition()));

        if (newX != x)
        {
            repaint (x - 1, 0, 3, getHeight());
            x = newX;
            repaint (x - 1, 0, 3, getHeight());

            if (onPlayheadMoved)
                onPlayheadMoved ((float) x);
        }
    }
};

//==============================================================================
class ArrangementView : public juce::Component,
                        private juce::ChangeListener,
                        private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    static constexpr int normalHeaderWidth = 200, wideHeaderWidth = 300;
    static constexpr int rulerHeight = 26;

    /** Record & Mix's Edit view has wider headers, with room for the input and output. */
    void setHeaderWidth (int w)
    {
        if (w == headerWidth)
            return;

        headerWidth = w;
        content.headerWidth = w;
        resized();
        repaint();
    }

    int getHeaderWidth() const      { return headerWidth; }

    void setTitle (const juce::String& t)   { title = t; repaint(); }

    ArrangementView (EditViewState& evs) : viewState (evs), markers (evs), ruler (evs), playhead (evs), commentMarks (evs)
    {
        addAndMakeVisible (markers);
        addAndMakeVisible (ruler);
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (10);

        addAndMakeVisible (hScroll);
        hScroll.setAutoHide (false);
        hScroll.addListener (&scrollListener);
        scrollListener.onScroll = [this] (double start)
        {
            viewState.viewStartBeat = start;
            viewState.sendChangeMessage();
        };

        content.addAndMakeVisible (commentMarks);
        content.addAndMakeVisible (playhead);

        playhead.onPlayheadMoved = [this] (float x) { followPlayhead (x); };

        viewState.revealTime = [this] (te::TimePosition t)
        {
            auto x = viewState.timeToX (t);
            auto laneWidth = (float) (getWidth() - headerWidth - viewport.getScrollBarThickness());

            if (x < 0.0f || x > laneWidth * 0.9f)
            {
                viewState.viewStartBeat = juce::jmax (0.0, viewState.timeToBeat (t) - (laneWidth * 0.15f) / viewState.pixelsPerBeat);
                viewState.sendChangeMessage();
            }
        };

        viewState.addChangeListener (this);
        viewState.edit.state.addListener (this);
        rebuildTracks();
    }

    ~ArrangementView() override
    {
        viewState.revealTime = nullptr;
        viewState.edit.state.removeListener (this);
        viewState.removeChangeListener (this);
    }

    std::function<void (const juce::File&, te::Track&, te::TimePosition)> onFileDropped;
    std::function<void (const juce::StringArray&, te::TimePosition)> onFilesDroppedAsNewTracks;
    bool followPlayback = true;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f).withKerningFactor (0.2f));
        g.drawText (title.toLowerCase(), juce::Rectangle<int> (16, MarkerStrip::height, headerWidth - 16, rulerHeight), juce::Justification::centredLeft);
        g.drawText ("markers", juce::Rectangle<int> (16, 0, headerWidth - 16, MarkerStrip::height), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto markerRow = r.removeFromTop (MarkerStrip::height);
        markers.setBounds (markerRow.withTrimmedLeft (headerWidth).withTrimmedRight (viewport.getScrollBarThickness()));
        auto top = r.removeFromTop (rulerHeight);
        top.removeFromLeft (headerWidth);
        ruler.setBounds (top.withTrimmedRight (viewport.getScrollBarThickness()));

        auto bottom = r.removeFromBottom (12);
        hScroll.setBounds (bottom.withTrimmedLeft (headerWidth).withTrimmedRight (viewport.getScrollBarThickness()));
        viewport.setBounds (r);
        layoutContent();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        handleWheel (e, w);
    }

    void handleWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
    {
        auto laneX = (float) (e.getEventRelativeTo (this).x - headerWidth);

        if (e.mods.isCommandDown() || e.mods.isAltDown())
        {
            auto factor = std::pow (1.15, (double) (w.deltaY * 8.0f));
            viewState.setZoom (viewState.pixelsPerBeat * factor, laneX);
        }
        else if (std::abs (w.deltaX) > std::abs (w.deltaY) || e.mods.isShiftDown())
        {
            auto d = std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY;
            viewState.scrollBy (-d * 400.0 / viewState.pixelsPerBeat);
        }
        else
        {
            viewport.setViewPosition (viewport.getViewPositionX(),
                                      juce::jmax (0, viewport.getViewPositionY() - juce::roundToInt (w.deltaY * 300.0f)));
        }
    }

    void mouseMagnify (const juce::MouseEvent& e, float scale) override
    {
        viewState.setZoom (viewState.pixelsPerBeat * scale, (float) (e.x - headerWidth));
    }

    /** Zoom so the whole song fits. */
    void zoomToFit()
    {
        auto lenBeats = juce::jmax (16.0, viewState.timeToBeat (te::toPosition (viewState.edit.getLength())) + 4.0);
        viewState.viewStartBeat = 0.0;
        viewState.pixelsPerBeat = juce::jmax (1.0, (getWidth() - headerWidth - 20) / lenBeats);
        viewState.sendChangeMessage();
    }

private:
    //==============================================================================
    /** Holds the rows; the empty space below the tracks accepts files to create new tracks. */
    struct Content : public juce::Component, public juce::FileDragAndDropTarget
    {
        std::function<void (const juce::MouseEvent&, const juce::MouseWheelDetails&)> onWheel;
        std::function<void (const juce::StringArray&, int x)> onFilesDropped;
        int tracksBottom = 0, headerWidth = normalHeaderWidth;
        bool dragOver = false;

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { if (onWheel) onWheel (e, w); }

        void paint (juce::Graphics& g) override
        {
            auto empty = getLocalBounds().withTop (tracksBottom);

            if (dragOver)
            {
                g.setColour (Colours::accent.withAlpha (0.12f));
                g.fillRect (empty);
            }

            g.setColour (dragOver ? Colours::text : Colours::textDim);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText ("Drop audio or MIDI files here to create new tracks (select several to import stems)",
                        empty.withHeight (60).withTrimmedLeft (headerWidth), juce::Justification::centred);
        }

        bool isInterestedInFileDrag (const juce::StringArray& files) override
        {
            for (auto& f : files)
                if (juce::File (f).hasFileExtension (te::soundFileAndMidiExtensions))
                    return true;
            return false;
        }

        void fileDragMove (const juce::StringArray&, int, int y) override       { setDragOver (y >= tracksBottom); }
        void fileDragExit (const juce::StringArray&) override                   { setDragOver (false); }

        void filesDropped (const juce::StringArray& files, int x, int y) override
        {
            setDragOver (false);
            if (y >= tracksBottom && onFilesDropped)
                onFilesDropped (files, x - headerWidth);
        }

        void setDragOver (bool b) { if (b != dragOver) { dragOver = b; repaint(); } }
    };

    struct ScrollListener : public juce::ScrollBar::Listener
    {
        std::function<void (double)> onScroll;
        void scrollBarMoved (juce::ScrollBar*, double newStart) override { if (onScroll) onScroll (newStart); }
    };

    struct Row
    {
        std::unique_ptr<TrackHeader> header;
        std::unique_ptr<TrackLane> lane;
        std::unique_ptr<AutomationLaneHeader> automationHeader;
        std::unique_ptr<AutomationLane> automationLane;
        std::vector<std::unique_ptr<Takes::LaneHeader>> takeHeaders;
        std::vector<std::unique_ptr<Takes::Lane>> takeLanes;
    };

    /** Rows need rebuilding when lanes are shown/hidden or groups collapse. */
    juce::String getStructureSignature() const
    {
        juce::String sig;
        for (auto& [id, param] : viewState.automationLanes) sig << id.toString() << ":" << juce::String::toHexString ((juce::pointer_sized_int) param.get()) << ",";
        sig << "|";
        for (auto& id : viewState.collapsedGroups) sig << id.toString() << ",";
        sig << "|";
        for (auto& id : viewState.takeLanes)
            if (auto t = te::findAudioTrackForID (viewState.edit, id))
                sig << id.toString() << ":" << Takes::getNumLanes (*t) << ",";
        return sig;
    }

    juce::String lastStructure;

    EditViewState& viewState;
    int headerWidth = normalHeaderWidth;
    juce::String title { "Arrangement" };
    MarkerStrip markers;
    TimelineRuler ruler;
    juce::Viewport viewport;
    Content content;
    PlayheadOverlay playhead;
    Comments::Overlay commentMarks;     // the notes along each track, and marking out a new one
    juce::ScrollBar hScroll { false };
    ScrollListener scrollListener;
    std::vector<Row> rows;

    void followPlayhead (float x)
    {
        auto& transport = viewState.edit.getTransport();

        if (! followPlayback || ! transport.isPlaying() || transport.isUserDragging())
            return;

        auto laneWidth = (float) (getWidth() - headerWidth - viewport.getScrollBarThickness());

        if (x > laneWidth * 0.9f || x < 0.0f)
        {
            viewState.viewStartBeat = juce::jmax (0.0, viewState.timeToBeat (transport.getPosition()) - (laneWidth * 0.1f) / viewState.pixelsPerBeat);
            viewState.sendChangeMessage();
        }
    }

    void rebuildTracks()
    {
        rows.clear();
        content.onWheel = [this] (auto& e, auto& w) { handleWheel (e, w); };
        content.onFilesDropped = [this] (const juce::StringArray& files, int x)
        {
            if (onFilesDroppedAsNewTracks)
                onFilesDroppedAsNewTracks (files, viewState.beatToTime (viewState.snapBeat (viewState.xToBeat ((float) juce::jmax (0, x)))));
        };

        auto tracks = te::getAllTracks (viewState.edit);

        for (auto t : tracks)
        {
            if (! (t->isAudioTrack() || t->isFolderTrack()) || viewState.isHiddenByCollapsedGroup (*t) || EditViewState::isTrackHidden (*t))
                continue;

            addRow (*t);
        }

        lastStructure = getStructureSignature();

        if (auto master = viewState.edit.getMasterTrack())
            addRow (*master);

        commentMarks.toFront (false);
        playhead.toFront (false);
        layoutContent();
    }

    void addRow (te::Track& t)
    {
        Row row;
        row.header = std::make_unique<TrackHeader> (viewState, t);
        row.lane = std::make_unique<TrackLane> (viewState, t);
        row.lane->onFileDropped = [this] (const juce::File& f, te::Track& track, te::TimePosition time)
        {
            if (onFileDropped)
                onFileDropped (f, track, time);
        };
        row.lane->onFilesDroppedAsNewTracks = [this] (const juce::StringArray& files, te::TimePosition time)
        {
            if (onFilesDroppedAsNewTracks)
                onFilesDroppedAsNewTracks (files, time);
        };

        auto* lanePtr = row.lane.get();
        row.lane->onMoveClipToY = [this, lanePtr] (te::Clip& clip, int yInLane)
        {
            auto yInContent = lanePtr->getY() + yInLane;

            for (auto& r : rows)
            {
                if (r.lane->getY() <= yInContent && yInContent < r.lane->getBottom())
                {
                    if (auto ct = dynamic_cast<te::ClipTrack*> (r.lane->getTrack()); ct != nullptr && ! ct->isMasterTrack())
                    {
                        auto clipRef = te::Clip::Ptr (&clip);
                        juce::MessageManager::callAsync ([clipRef, ct] { clipRef->moveTo (*ct); });
                    }
                    return;
                }
            }
        };

        auto trackRef = te::Track::Ptr (&t);
        row.header->onToggleAutomation = [this, trackRef]
        {
            auto& lanes = viewState.automationLanes;

            if (lanes.count (trackRef->itemID))
                lanes.erase (trackRef->itemID);
            else if (auto p = Automation::getDefaultParameter (*trackRef))
                lanes[trackRef->itemID] = p;

            viewState.sendChangeMessage();
        };

        content.addAndMakeVisible (*row.header);
        content.addAndMakeVisible (*row.lane);

        if (auto at = dynamic_cast<te::AudioTrack*> (&t); at != nullptr && viewState.takeLanes.count (t.itemID))
        {
            for (int i = 0, n = Takes::getNumLanes (*at); i < n; ++i)
            {
                content.addAndMakeVisible (*row.takeHeaders.emplace_back (std::make_unique<Takes::LaneHeader> (t, i)));
                content.addAndMakeVisible (*row.takeLanes.emplace_back (std::make_unique<Takes::Lane> (viewState, *at, i)));
            }
        }

        if (viewState.automationLanes.count (t.itemID))
        {
            row.automationHeader = std::make_unique<AutomationLaneHeader> (viewState, t);
            row.automationLane = std::make_unique<AutomationLane> (viewState, t);
            content.addAndMakeVisible (*row.automationHeader);
            content.addAndMakeVisible (*row.automationLane);
        }

        rows.push_back (std::move (row));
    }

    void layoutContent()
    {
        int y = 0;
        auto width = juce::jmax (headerWidth + 1, viewport.getMaximumVisibleWidth());
        std::vector<Comments::Overlay::Lane> lanes;

        for (auto& r : rows)
        {
            auto h = viewState.getTrackHeight (*r.header->getTrack());

            if (r.header->getTrack()->isMasterTrack())
                y += 6; // small gap above master

            r.header->setBounds (0, y, headerWidth, h);
            r.lane->setBounds (headerWidth, y, width - headerWidth, h);
            r.lane->updateLayout();
            lanes.push_back ({ r.header->getTrack(), y, h });
            y += h;

            for (size_t i = 0; i < r.takeLanes.size(); ++i)
            {
                auto th = EditViewState::takeLaneHeight;
                r.takeHeaders[i]->setBounds (0, y, headerWidth, th);
                r.takeLanes[i]->setBounds (headerWidth, y, width - headerWidth, th);
                r.takeLanes[i]->repaint();
                y += th;
            }

            if (r.automationLane != nullptr)
            {
                auto ah = EditViewState::automationLaneHeight;
                r.automationHeader->setBounds (0, y, headerWidth, ah);
                r.automationHeader->refresh();
                r.automationLane->setBounds (headerWidth, y, width - headerWidth, ah);
                r.automationLane->repaint();
                y += ah;
            }
        }

        auto totalH = juce::jmax (y + 80, viewport.getMaximumVisibleHeight());
        content.tracksBottom = y;
        content.setSize (width, totalH);
        content.repaint();
        playhead.setBounds (headerWidth, 0, width - headerWidth, totalH);
        commentMarks.setBounds (playhead.getBounds());
        commentMarks.setLanes (std::move (lanes));
        updateScrollBar();
        ruler.repaint();
    }

    void updateScrollBar()
    {
        // Before the first real layout the view can be narrower than the headers
        auto visibleBeats = juce::jmax (1.0, (getWidth() - headerWidth) / viewState.pixelsPerBeat);
        auto songBeats = viewState.timeToBeat (te::toPosition (viewState.edit.getLength()));
        auto total = juce::jmax (songBeats + visibleBeats, viewState.viewStartBeat + visibleBeats * 1.5);
        hScroll.setRangeLimits (0.0, total, juce::dontSendNotification);
        hScroll.setCurrentRange (viewState.viewStartBeat, visibleBeats, juce::dontSendNotification);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (getStructureSignature() != lastStructure)
            return rebuildTracks();

        layoutContent();
        repaint();
    }

    void handleAsyncUpdate() override { rebuildTracks(); }

    // New takes can need another lane
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override
    {
        if (te::TrackList::isTrack (c) || c.hasType (Takes::takeType)) triggerAsyncUpdate();
    }

    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override
    {
        if (te::TrackList::isTrack (c) || c.hasType (Takes::takeType)) triggerAsyncUpdate();
    }

    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (id == EditViewState::hiddenProperty && te::TrackList::isTrack (v)) triggerAsyncUpdate();

        // A group's colour shows on all of its tracks and their clips
        if (id == te::IDs::colour && te::TrackList::isTrack (v)) repaint();
    }
};

} // namespace elsewhere
