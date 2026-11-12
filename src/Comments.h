// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Theme.h"
#include "Collab.h"

namespace elsewhere
{
//==============================================================================
/**
    Comments: notes about a stretch of a track or clip ("popping in the vocals here"). They live
    inside the project (an ELSEWHERE_COMMENTS node in the edit), so they're saved with it and
    sync to collaborators through the same operation log as everything else.

    Each one covers a range of the song (`position` to `end`, in seconds; older ones are a single
    point). They're read like a book, in song order: the notes book in the bottom panel, and the
    numbered marks along the foot of each track in the arrangement.
*/
namespace Comments
{
    inline const juce::Identifier rootType { "ELSEWHERE_COMMENTS" }, commentType { "COMMENT" };

    struct Comment
    {
        juce::ValueTree state;

        juce::String getId() const          { return state[Collab::cidProp].toString(); }
        juce::String getAuthor() const      { return state["author"].toString(); }
        juce::Colour getColour() const      { return juce::Colour::fromString (state["colour"].toString()); }
        juce::String getText() const        { return state["text"].toString(); }
        juce::Time getTime() const          { return juce::Time ((juce::int64) state["time"]); }
        te::EditItemID getTarget() const    { return te::EditItemID::fromVar (state["target"]); }
        bool isResolved() const             { return (bool) state["resolved"]; }
        double getStart() const             { return (double) state["position"]; }
        double getEnd() const               { return state.hasProperty ("end") ? juce::jmax (getStart(), (double) state["end"]) : getStart(); }

        te::TimeRange getRange() const      { return { te::TimePosition::fromSeconds (getStart()), te::TimePosition::fromSeconds (getEnd()) }; }
    };

    inline juce::ValueTree getRoot (te::Edit& edit)
    {
        auto root = edit.state.getChildWithName (rootType);

        if (! root.isValid())
        {
            root = juce::ValueTree (rootType);
            edit.state.appendChild (root, nullptr);
        }

        return root;
    }

    inline std::vector<Comment> getAll (te::Edit& edit)
    {
        std::vector<Comment> result;
        auto root = edit.state.getChildWithName (rootType);

        for (auto c : root)
            if (c.hasType (commentType))
                result.push_back ({ c });

        std::sort (result.begin(), result.end(), [] (auto& a, auto& b) { return a.getTime() > b.getTime(); });
        return result;
    }

    /** The comments as the book reads them: by where they start in the song, then by when they
        were written. */
    inline std::vector<Comment> inSongOrder (te::Edit& edit, bool includeResolved)
    {
        std::vector<Comment> result;

        for (auto& c : getAll (edit))
            if (includeResolved || ! c.isResolved())
                result.push_back (c);

        std::stable_sort (result.begin(), result.end(), [] (auto& a, auto& b)
        {
            if (a.getStart() < b.getStart() || b.getStart() < a.getStart())
                return a.getStart() < b.getStart();

            return a.getTime() < b.getTime();
        });
        return result;
    }

    inline std::vector<Comment> getFor (te::Edit& edit, te::EditItemID target, bool includeResolved = false)
    {
        std::vector<Comment> result;

        for (auto& c : getAll (edit))
            if (c.getTarget() == target && (includeResolved || ! c.isResolved()))
                result.push_back (c);

        return result;
    }

    /** Adds a comment and returns its ID. */
    inline juce::String add (te::Edit& edit, const Collab::Identity& who, te::EditItemID target, te::TimeRange range, const juce::String& text)
    {
        auto id = juce::Uuid().toDashedString();
        juce::ValueTree c (commentType);
        c.setProperty (Collab::cidProp, id, nullptr);
        c.setProperty ("author", who.name, nullptr);
        c.setProperty ("colour", who.colour.toString(), nullptr);
        c.setProperty ("text", text, nullptr);
        c.setProperty ("time", juce::Time::currentTimeMillis(), nullptr);
        c.setProperty ("target", target.toVar(), nullptr);
        c.setProperty ("position", range.getStart().inSeconds(), nullptr);
        c.setProperty ("end", range.getEnd().inSeconds(), nullptr);
        getRoot (edit).appendChild (c, &edit.getUndoManager());
        return id;
    }

    /** The track a comment is about: its target, or its target clip's track. */
    inline te::Track* trackFor (te::Edit& edit, te::EditItemID target)
    {
        for (auto t : te::getAllTracks (edit))
        {
            if (t->itemID == target)
                return t;

            if (auto ct = dynamic_cast<te::ClipTrack*> (t))
                for (auto c : ct->getClips())
                    if (c->itemID == target)
                        return t;
        }

        return nullptr;
    }

    /** "bars 12–14", or "bar 27" for a point or a stretch inside one bar. */
    inline juce::String describeRange (te::Edit& edit, te::TimeRange r)
    {
        auto barAt = [&] (te::TimePosition t) { return edit.tempoSequence.toBarsAndBeats (t).bars + 1; };
        auto first = barAt (r.getStart());
        auto last = r.getLength() > 0.001s ? barAt (r.getEnd() - 0.001s) : first;   // an end on a barline belongs to the bar before

        return first == last ? "bar " + juce::String (first)
                             : "bars " + juce::String (first) + juce::String::fromUTF8 ("\xe2\x80\x93") + juce::String (last);
    }

    inline juce::String describeTarget (te::Edit& edit, te::EditItemID id)
    {
        auto t = trackFor (edit, id);
        return t != nullptr ? t->getName() : juce::String ("Project");
    }

    /** Asks for the text, then adds a comment; `onAdded` gets its ID. */
    inline void promptAndAdd (te::Edit& edit, const Collab::Identity& who, te::EditItemID target, te::TimeRange range,
                              const juce::String& targetName, std::function<void (const juce::String&)> onAdded = {})
    {
        auto* aw = Theme::newAlertWindow ("Comment on " + targetName, describeRange (edit, range));
        aw->addTextEditor ("text", {});
        aw->addButton ("Add Comment", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([&edit, who, target, range, aw, onAdded] (int r)
        {
            auto text = aw->getTextEditorContents ("text").trim();

            if (r == 1 && text.isNotEmpty())
            {
                edit.getUndoManager().beginNewTransaction ("Add comment");
                auto id = add (edit, who, target, range, text);

                if (onAdded)
                    onAdded (id);
            }
        }), true);
    }

    inline juce::String timeAgo (juce::Time t)
    {
        auto secs = (juce::Time::getCurrentTime() - t).inSeconds();
        if (secs < 60)      return "just now";
        if (secs < 3600)    return juce::String ((int) (secs / 60)) + " min ago";
        if (secs < 86400)   return juce::String ((int) (secs / 3600)) + " h ago";
        return t.formatted ("%d %b");
    }

    /** A small disc with a number or letter in it: a note's number, or its author's initial. */
    inline void drawDisc (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour ink, const juce::String& text, bool outlineOnly = false)
    {
        g.setColour (fill);

        if (outlineOnly)
            g.drawEllipse (r.reduced (0.5f), 1.0f);
        else
            g.fillEllipse (r);

        g.setColour (outlineOnly ? fill : ink);
        g.setFont (juce::FontOptions (r.getHeight() * 0.6f, juce::Font::bold));
        g.drawText (text, r, juce::Justification::centred);
    }

    /** Opens a comment in the book: the panel switches to it, and (unless `movePlayhead` is
        false) the playhead goes to its start, with its track in focus. */
    inline void open (EditViewState& vs, const Comment& c, bool movePlayhead)
    {
        vs.openCommentId = c.getId();
        vs.detailMode = EditViewState::DetailMode::comments;

        if (movePlayhead)
        {
            vs.edit.getTransport().setPosition (c.getRange().getStart());

            if (vs.revealTime)
                vs.revealTime (c.getRange().getStart());

            if (auto t = trackFor (vs.edit, c.getTarget()))
                vs.focusTrack (t);
        }

        vs.sendChangeMessage();
    }

    //==============================================================================
    /**
        The notes book, in the bottom panel. The contents on the left list every note in song
        order; the open one is on the right as a page. Turning a page (the arrows, the arrow keys,
        or the contents) moves the playhead to that note's stretch, and while the song plays the
        book turns to the note under the playhead. Add Comment first asks you to drag across a
        track in the arrangement, to mark what the note is about.
    */
    class Book : public juce::Component, private juce::ValueTree::Listener, private juce::ChangeListener,
                 private juce::AsyncUpdater, private juce::Timer
    {
    public:
        Book (EditViewState& evs, const Collab::Identity& me) : viewState (evs), identity (me)
        {
            addButton.onClick = [this]
            {
                viewState.addingComment = ! viewState.addingComment;
                viewState.sendChangeMessage();
            };
            addAndMakeVisible (addButton);

            showResolved.setButtonText ("Show resolved");
            showResolved.onClick = [this]
            {
                viewState.showResolvedComments = showResolved.getToggleState();
                viewState.sendChangeMessage();
            };
            addAndMakeVisible (showResolved);

            prevButton.onClick = [this] { turn (-1); };
            nextButton.onClick = [this] { turn (1); };
            addAndMakeVisible (prevButton);
            addAndMakeVisible (nextButton);

            resolveButton.onClick = [this]
            {
                if (auto c = getOpen())
                {
                    auto& um = viewState.edit.getUndoManager();
                    um.beginNewTransaction ("Resolve comment");
                    c->state.setProperty ("resolved", ! c->isResolved(), &um);
                }
            };
            removeButton.onClick = [this]
            {
                if (auto c = getOpen())
                {
                    auto& um = viewState.edit.getUndoManager();
                    um.beginNewTransaction ("Delete comment");
                    c->state.getParent().removeChild (c->state, &um);
                }
            };
            addChildComponent (resolveButton);
            addChildComponent (removeButton);

            contents.book = this;
            contentsView.setViewedComponent (&contents, false);
            contentsView.setScrollBarsShown (true, false);
            contentsView.setScrollBarThickness (8);
            addAndMakeVisible (contentsView);

            setWantsKeyboardFocus (true);
            viewState.edit.state.addListener (this);
            viewState.addChangeListener (this);
            rebuild();
            startTimerHz (8);
        }

        ~Book() override
        {
            viewState.edit.state.removeListener (this);
            viewState.removeChangeListener (this);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::panelDark);
            auto& edit = viewState.edit;

            // While marking a new note's stretch, say how
            if (viewState.addingComment)
            {
                g.setColour (Colours::text);
                g.setFont (juce::FontOptions (12.5f));
                g.drawText ("Drag across a track to mark what the note is about. Esc cancels.",
                            hintArea, juce::Justification::centredLeft, true);
            }

            if (notes.empty())
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (13.0f));
                g.drawFittedText ("No notes yet. Click Add Comment, then drag across a track to mark what the note is about.",
                                  getLocalBounds().withTrimmedTop (topH).reduced (40, 10), juce::Justification::centred, 3);
                return;
            }

            if (openIndex >= 0)
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (12.0f));
                g.drawText ("page " + juce::String (openIndex + 1) + " of " + juce::String ((int) notes.size()),
                            pageNumberArea, juce::Justification::centredRight);
            }

            // The page
            g.setColour (Colours::panel);
            g.fillRoundedRectangle (pageArea.toFloat(), 14.0f);

            auto c = getOpen();
            if (! c)
                return;

            auto r = pageArea.reduced (22, 16);
            auto dim = c->isResolved() ? 0.5f : 1.0f;

            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (12.5f));
            g.drawText (describeTarget (edit, c->getTarget()) + juce::String::fromUTF8 (" \xc2\xb7 ") + describeRange (edit, c->getRange())
                          + (c->isResolved() ? juce::String::fromUTF8 (" \xc2\xb7 resolved") : juce::String()),
                        r.removeFromTop (20), juce::Justification::centredLeft, true);

            auto footer = r.removeFromBottom (24);
            r.removeFromTop (8);

            juce::AttributedString text;
            text.append (c->getText(), juce::FontOptions (17.0f), Colours::text.withMultipliedAlpha (dim));
            text.setWordWrap (juce::AttributedString::byWord);
            text.draw (g, r.toFloat());

            auto author = footer.removeFromLeft (footer.getWidth() - 190);
            drawDisc (g, author.removeFromLeft (20).withSizeKeepingCentre (18, 18).toFloat(), c->getColour(), juce::Colours::white,
                      c->getAuthor().substring (0, 1).toUpperCase());
            author.removeFromLeft (8);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (12.5f));
            g.drawText (c->getAuthor() + juce::String::fromUTF8 (" \xc2\xb7 ") + timeAgo (c->getTime()), author, juce::Justification::centredLeft, true);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8, 6);
            auto top = r.removeFromTop (topH - 6);

            addButton.setBounds (top.removeFromLeft (130).reduced (0, 2));
            top.removeFromLeft (10);
            showResolved.setBounds (top.removeFromLeft (130));
            nextButton.setBounds (top.removeFromRight (top.getHeight()).reduced (2));
            prevButton.setBounds (top.removeFromRight (top.getHeight()).reduced (2));
            top.removeFromRight (6);
            pageNumberArea = top.removeFromRight (110);
            hintArea = top.withTrimmedLeft (6);

            r.removeFromTop (4);
            contentsView.setBounds (r.removeFromLeft (juce::jlimit (220, 360, r.getWidth() * 2 / 5)));
            r.removeFromLeft (10);
            pageArea = r;

            auto buttons = pageArea.reduced (18, 14).removeFromBottom (24).removeFromRight (180);
            removeButton.setBounds (buttons.removeFromRight (80));
            buttons.removeFromRight (8);
            resolveButton.setBounds (buttons.removeFromRight (90));

            contents.setSize (contentsView.getMaximumVisibleWidth(), juce::jmax ((int) notes.size() * Contents::rowH, contentsView.getHeight()));
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::leftKey || k == juce::KeyPress::upKey)     { turn (-1); return true; }
            if (k == juce::KeyPress::rightKey || k == juce::KeyPress::downKey)  { turn (1); return true; }

            if (k == juce::KeyPress::escapeKey && viewState.addingComment)
            {
                viewState.addingComment = false;
                viewState.sendChangeMessage();
                return true;
            }

            return false;
        }

    private:
        /** The contents: one line per note. */
        struct Contents : public juce::Component
        {
            static constexpr int rowH = 30;
            Book* book = nullptr;

            void paint (juce::Graphics& g) override
            {
                auto& edit = book->viewState.edit;

                for (int i = 0; i < (int) book->notes.size(); ++i)
                {
                    auto& c = book->notes[(size_t) i];
                    auto r = juce::Rectangle<int> (0, i * rowH, getWidth(), rowH).reduced (0, 2);
                    auto isOpen = i == book->openIndex;
                    auto ink = Colours::text.withMultipliedAlpha (c.isResolved() ? 0.5f : 1.0f);

                    if (isOpen)
                    {
                        g.setColour (Colours::panel);
                        g.fillRoundedRectangle (r.toFloat(), (float) r.getHeight() * 0.5f);
                    }

                    r.removeFromLeft (6);
                    drawDisc (g, r.removeFromLeft (18).withSizeKeepingCentre (18, 18).toFloat(), ink, Colours::textInverse,
                              juce::String (i + 1), ! isOpen);
                    r.removeFromLeft (8);

                    g.setFont (juce::FontOptions (12.0f));
                    g.setColour (Colours::textDim);
                    g.drawText (describeRange (edit, c.getRange()), r.removeFromLeft (76), juce::Justification::centredLeft, true);
                    g.setColour (ink);
                    g.drawText (describeTarget (edit, c.getTarget()), r.removeFromLeft (72), juce::Justification::centredLeft, true);
                    g.setColour (Colours::textDim);
                    g.drawText (c.getText().upToFirstOccurrenceOf ("\n", false, false), r.reduced (4, 0), juce::Justification::centredLeft, true);
                }
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                auto i = e.y / rowH;

                if (i >= 0 && i < (int) book->notes.size())
                    book->openAt (i, true);

                book->grabKeyboardFocus();
            }
        };

        EditViewState& viewState;
        Collab::Identity identity;
        juce::TextButton addButton { "Add Comment" }, prevButton { juce::String::fromUTF8 ("\xe2\x80\xb9") }, nextButton { juce::String::fromUTF8 ("\xe2\x80\xba") };
        juce::TextButton resolveButton { "Resolve" }, removeButton { "Delete" };
        juce::ToggleButton showResolved;
        juce::Viewport contentsView;
        Contents contents;
        std::vector<Comment> notes;
        int openIndex = -1, ticks = 0;
        juce::Rectangle<int> pageArea, pageNumberArea, hintArea;
        static constexpr int topH = 36;

        std::optional<Comment> getOpen() const
        {
            if (openIndex >= 0 && openIndex < (int) notes.size())
                return notes[(size_t) openIndex];
            return std::nullopt;
        }

        void rebuild()
        {
            notes = inSongOrder (viewState.edit, viewState.showResolvedComments);
            openIndex = -1;

            for (int i = 0; i < (int) notes.size(); ++i)
                if (notes[(size_t) i].getId() == viewState.openCommentId)
                    openIndex = i;

            if (openIndex < 0 && ! notes.empty())
                openIndex = 0;

            showResolved.setToggleState (viewState.showResolvedComments, juce::dontSendNotification);
            addButton.setButtonText (viewState.addingComment ? "Cancel" : "Add Comment");
            addButton.setToggleState (viewState.addingComment, juce::dontSendNotification);

            auto c = getOpen();
            resolveButton.setVisible (c.has_value());
            removeButton.setVisible (c.has_value());
            resolveButton.setButtonText (c && c->isResolved() ? "Reopen" : "Resolve");
            prevButton.setEnabled (openIndex > 0);
            nextButton.setEnabled (openIndex >= 0 && openIndex + 1 < (int) notes.size());

            resized();
            repaint();
            contents.repaint();
        }

        void openAt (int index, bool movePlayhead)
        {
            if (index < 0 || index >= (int) notes.size())
                return;

            openIndex = index;
            open (viewState, notes[(size_t) index], movePlayhead);
        }

        void turn (int delta)       { openAt (openIndex + delta, true); }

        /** While playing, the book follows the song: it opens the note under the playhead. */
        void timerCallback() override
        {
            auto& transport = viewState.edit.getTransport();

            if (transport.isPlaying() && isShowing())
            {
                auto pos = transport.getPosition();

                for (int i = 0; i < (int) notes.size(); ++i)
                {
                    auto r = notes[(size_t) i].getRange();

                    if (pos >= r.getStart() && pos < juce::jmax (r.getEnd(), r.getStart() + 1.0s))
                    {
                        if (i != openIndex)
                            openAt (i, false);
                        break;
                    }
                }
            }

            if (++ticks % 240 == 0)     // "x min ago"
                repaint();
        }

        void handleAsyncUpdate() override                                   { rebuild(); }
        void changeListenerCallback (juce::ChangeBroadcaster*) override     { triggerAsyncUpdate(); }

        static bool isCommentTree (const juce::ValueTree& v)
        {
            return v.hasType (rootType) || v.hasType (commentType) || v.getParent().hasType (rootType);
        }

        void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree& c) override           { if (isCommentTree (p) || isCommentTree (c)) triggerAsyncUpdate(); }
        void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree& c, int) override    { if (isCommentTree (p) || isCommentTree (c)) triggerAsyncUpdate(); }
        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier&) override { if (v.hasType (commentType)) triggerAsyncUpdate(); }
    };

    //==============================================================================
    /**
        Over the arrangement's lanes: each note's stretch along the foot of its track, numbered as
        in the book, with its first words when there's room. Clicking one opens it. While a new
        comment is being added, dragging across a track marks its stretch (snapped to the grid).
    */
    class Overlay : public juce::Component, private juce::ValueTree::Listener, private juce::ChangeListener, private juce::AsyncUpdater
    {
    public:
        struct Lane { te::Track::Ptr track; int y = 0, h = 0; };

        explicit Overlay (EditViewState& evs) : viewState (evs)
        {
            setWantsKeyboardFocus (true);
            viewState.edit.state.addListener (this);
            viewState.addChangeListener (this);
        }

        ~Overlay() override
        {
            viewState.edit.state.removeListener (this);
            viewState.removeChangeListener (this);
        }

        void setLanes (std::vector<Lane> l)
        {
            marking = nullptr;   // it pointed into the old list
            markRange.reset();
            lanes = std::move (l);
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto notes = inSongOrder (viewState.edit, viewState.showResolvedComments);

            for (int i = 0; i < (int) notes.size(); ++i)
            {
                auto band = bandFor (notes[(size_t) i]);
                if (band.isEmpty())
                    continue;

                auto isOpen = notes[(size_t) i].getId() == viewState.openCommentId;
                auto alpha = notes[(size_t) i].isResolved() ? 0.5f : 1.0f;

                g.setColour (Colours::text.withAlpha ((isOpen ? 0.2f : 0.1f) * alpha));
                g.fillRoundedRectangle (band, band.getHeight() * 0.5f);

                if (isOpen)
                {
                    g.setColour (Colours::text.withAlpha (alpha));
                    g.drawRoundedRectangle (band.reduced (0.5f), band.getHeight() * 0.5f, 1.0f);
                }

                drawDisc (g, band.withWidth (band.getHeight()), Colours::text.withAlpha (alpha), Colours::textInverse, juce::String (i + 1));

                if (band.getWidth() > 60.0f)
                {
                    g.setColour (Colours::text.withAlpha (alpha));
                    g.setFont (juce::FontOptions (10.5f));
                    g.drawText (notes[(size_t) i].getText().upToFirstOccurrenceOf ("\n", false, false),
                                band.withTrimmedLeft (band.getHeight() + 5.0f).withTrimmedRight (8.0f), juce::Justification::centredLeft, true);
                }
            }

            // The stretch being marked for a new comment
            if (marking != nullptr && markRange.has_value())
            {
                auto x1 = viewState.timeToX (markRange->getStart()), x2 = viewState.timeToX (markRange->getEnd());
                auto r = juce::Rectangle<float> (x1, (float) marking->y + 2.0f, juce::jmax (2.0f, x2 - x1), (float) marking->h - 4.0f);
                g.setColour (Colours::text.withAlpha (0.1f));
                g.fillRoundedRectangle (r, 6.0f);
                g.setColour (Colours::text);
                g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
            }
        }

        bool hitTest (int x, int y) override
        {
            return viewState.addingComment || noteAt ({ (float) x, (float) y }).has_value();
        }

        void mouseMove (const juce::MouseEvent&) override
        {
            setMouseCursor (viewState.addingComment ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::PointingHandCursor);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! viewState.addingComment)
            {
                if (auto c = noteAt (e.position))
                    open (viewState, *c, true);
                return;
            }

            grabKeyboardFocus();
            marking = laneAt (e.y);
            markAnchor = snappedTime ((float) e.x);
            markRange = te::TimeRange (markAnchor, markAnchor);
            repaint();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (marking == nullptr)
                return;

            auto t = snappedTime ((float) e.x);
            markRange = te::TimeRange (std::min (markAnchor, t), std::max (markAnchor, t));
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (marking == nullptr || ! markRange.has_value())
                return;

            auto track = marking->track;
            auto range = *markRange;
            marking = nullptr;
            markRange.reset();
            viewState.addingComment = false;
            viewState.sendChangeMessage();
            repaint();

            if (track != nullptr && viewState.addComment)
                viewState.addComment (track->itemID, range, track->getName());
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::escapeKey && viewState.addingComment)
            {
                marking = nullptr;
                markRange.reset();
                viewState.addingComment = false;
                viewState.sendChangeMessage();
                return true;
            }

            return false;
        }

    private:
        EditViewState& viewState;
        std::vector<Lane> lanes;
        const Lane* marking = nullptr;
        te::TimePosition markAnchor;
        std::optional<te::TimeRange> markRange;

        const Lane* laneAt (int y) const
        {
            for (auto& l : lanes)
                if (y >= l.y && y < l.y + l.h && l.track != nullptr && ! l.track->isMasterTrack())
                    return &l;
            return nullptr;
        }

        te::TimePosition snappedTime (float x) const
        {
            return viewState.beatToTime (viewState.snapBeat (viewState.xToBeat (x)));
        }

        /** Where a note sits: along the foot of its track's lane. */
        juce::Rectangle<float> bandFor (const Comment& c) const
        {
            auto track = trackFor (viewState.edit, c.getTarget());

            for (auto& l : lanes)
            {
                if (l.track.get() != track || l.h < 24)
                    continue;

                auto x1 = viewState.timeToX (c.getRange().getStart()), x2 = viewState.timeToX (c.getRange().getEnd());
                const float h = 14.0f;
                return { x1, (float) (l.y + l.h) - h - 3.0f, juce::jmax (h, x2 - x1), h };
            }

            return {};
        }

        std::optional<Comment> noteAt (juce::Point<float> p) const
        {
            auto notes = inSongOrder (viewState.edit, viewState.showResolvedComments);

            for (auto it = notes.rbegin(); it != notes.rend(); ++it)
                if (bandFor (*it).contains (p))
                    return *it;

            return std::nullopt;
        }

        void handleAsyncUpdate() override                                   { repaint(); }
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            setMouseCursor (viewState.addingComment ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
            triggerAsyncUpdate();
        }

        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override            { if (c.hasType (commentType)) triggerAsyncUpdate(); }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override     { if (c.hasType (commentType)) triggerAsyncUpdate(); }
        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier&) override { if (v.hasType (commentType)) triggerAsyncUpdate(); }
    };
}

} // namespace elsewhere
