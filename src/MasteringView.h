// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Mastering.h"
#include "EditViewState.h"
#include "Theme.h"
#include "Widgets.h"

namespace elsewhere
{
//==============================================================================
/**
    What a mastering project shows in place of the arrangement: the album as one timeline, the
    song picked there (its gap and fades), every song in a list with its loudness, and the
    album's loudness song by song.

    Picking a song shows its mastering chain in the device panel; Album Chain shows the album's.
    Double-click a song to open its project.
*/
class MasteringView : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit MasteringView (EditViewState& evs) : viewState (evs), edit (evs.edit), album (evs.edit),
                                                  timeline (*this), strip (*this), list (*this), overview (*this)
    {
        setTitle ("Album");
        album.onChange = [this] { refresh(); };

        addSong.onClick = [this] { chooseSongs(); };
        addSong.setTooltip ("Add songs (Elsewhere projects) to the end of the album. You can also drag them in.");
        albumChain.onClick = [this] { showChainOf (edit.getMasterTrack()); };
        albumChain.setTooltip ("Show the album's mastering chain, which comes after every song's own");
        measure.onClick = [this] { album.isMeasuring() ? album.cancelMeasure() : album.measure (false); };
        measure.setTooltip ("Render each song through its chain and the album's, and measure its loudness and true peak");
        match.onClick = [this] { album.isMeasuring() ? album.cancelMeasure() : album.measure (true); };
        match.setTooltip ("Set each song's chain so every song reaches the album's loudness");
        exportButton.onClick = [this] { showExport(); };
        exportButton.setTooltip ("Export each song as its own file, and the whole album as one");

        target.accessibleName = "Album loudness";
        target.unit = "LUFS";
        target.setTooltip ("The loudness every song is matched to, in LUFS");
        target.setValue (Mastering::targetOf (edit), false);
        target.onValueChange = [this] (double v)
        {
            edit.getUndoManager().beginNewTransaction ("Album Loudness");
            Mastering::setTarget (edit, v);
            refresh();
        };

        targetLabel.setText ("Target", juce::dontSendNotification);
        targetLabel.setJustificationType (juce::Justification::centredRight);
        status.setJustificationType (juce::Justification::centredRight);

        for (auto* l : { &targetLabel, &status })
        {
            l->setFont (juce::FontOptions (12.5f));
            l->setColour (juce::Label::textColourId, Colours::textDim);
        }

        for (auto* c : std::initializer_list<juce::Component*> { &addSong, &albumChain, &measure, &match, &target, &targetLabel,
                                                                 &exportButton, &status, &timeline, &strip, &list, &overview })
            addAndMakeVisible (c);

        timeline.setTitle ("Album timeline");
        list.setTitle ("Songs");
        overview.setTitle ("Album loudness");

        viewState.addChangeListener (this);
        startTimerHz (15);
        refresh();
    }

    ~MasteringView() override
    {
        viewState.removeChangeListener (this);
        album.onChange = nullptr;
    }

    /** Asked to open a song's project (after the album has been saved). */
    std::function<void (const juce::File&)> onOpenSong;

    /** The device panel should be showing (a chain was picked). */
    std::function<void()> onShowChain;

    Mastering::Album& getAlbum()    { return album; }

    //==============================================================================
    void chooseSongs()
    {
        chooser = std::make_unique<juce::FileChooser> ("Add songs to " + Mastering::nameOf (edit),
                                                       Mastering::folderOf (edit).getParentDirectory(), juce::String ("*") + Project::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { addSongs (fc.getResults(), -1); });
    }

    void addSongs (const juce::Array<juce::File>& files, int index)
    {
        juce::StringArray skipped;
        te::AudioTrack* last = nullptr;
        edit.getUndoManager().beginNewTransaction ("Add Songs");

        for (auto& f : files)
        {
            if (! f.hasFileExtension (Project::fileExtension) || Mastering::isMasteringFile (f) || f == edit.editFileRetriever())
            {
                skipped.add (f.getFileName());
                continue;
            }

            if ((last = Mastering::addSong (edit, f, index)) != nullptr && index >= 0)
                ++index;
        }

        if (last != nullptr)
            select (*last);

        album.check();

        if (! skipped.isEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Only songs can be added",
                                                    "These aren't song projects, so they weren't added:\n" + skipped.joinIntoString ("\n"));
    }

    //==============================================================================
    void showExport();

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (juce::File (f).hasFileExtension (Project::fileExtension))
                return true;

        return false;
    }

    void fileDragMove (const juce::StringArray&, int x, int y) override
    {
        dropIndex = timeline.getBounds().contains (x, y) ? timeline.indexAt ((float) (x - timeline.getX())) : -1;
        timeline.repaint();
    }

    void fileDragExit (const juce::StringArray&) override     { dropIndex = -1; timeline.repaint(); }

    void filesDropped (const juce::StringArray& paths, int x, int y) override
    {
        auto index = timeline.getBounds().contains (x, y) ? timeline.indexAt ((float) (x - timeline.getX())) : -1;
        dropIndex = -1;
        juce::Array<juce::File> files;

        for (auto& p : paths)
            files.add (juce::File (p));

        addSongs (files, index >= Mastering::getSongs (edit).size() ? -1 : index);
    }

    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);
        g.setColour (Colours::outline);
        g.fillRect (0, toolbarH - 1, getWidth(), 1);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto bar = r.removeFromTop (toolbarH).reduced (8, 6);

        addSong.setBounds (bar.removeFromLeft (96));
        bar.removeFromLeft (6);
        albumChain.setBounds (bar.removeFromLeft (100));
        bar.removeFromLeft (18);
        targetLabel.setBounds (bar.removeFromLeft (48));
        bar.removeFromLeft (4);
        target.setBounds (bar.removeFromLeft (64));
        bar.removeFromLeft (6);
        measure.setBounds (bar.removeFromLeft (82));
        bar.removeFromLeft (6);
        match.setBounds (bar.removeFromLeft (124));
        exportButton.setBounds (bar.removeFromRight (110));
        bar.removeFromRight (8);
        status.setBounds (bar);

        timeline.setBounds (r.removeFromTop (juce::jlimit (130, 260, r.getHeight() * 2 / 5)));
        strip.setBounds (r.removeFromTop (36));
        overview.setBounds (r.removeFromRight (juce::jlimit (240, 520, r.getWidth() * 2 / 5)));
        list.setBounds (r);
    }

private:
    static constexpr int toolbarH = 40;

    EditViewState& viewState;
    te::Edit& edit;
    Mastering::Album album;
    std::unique_ptr<juce::FileChooser> chooser;
    int dropIndex = -1;

    juce::TextButton addSong { "Add Song..." }, albumChain { "Album Chain" }, measure { "Measure" },
                     match { "Match Loudness" }, exportButton { "Export Album..." };
    DragNumberBox target { -24.0, -6.0, 1 };
    juce::Label targetLabel, status;

    //==============================================================================
    te::AudioTrack* selected() const
    {
        auto t = dynamic_cast<te::AudioTrack*> (viewState.focusedTrack.get());
        return t != nullptr && Mastering::isSong (*t) ? t : nullptr;
    }

    void select (te::AudioTrack& t)
    {
        viewState.selectionManager.deselectAll();

        if (auto c = Mastering::clipOf (t))
            viewState.selectionManager.selectOnly (c);

        viewState.detailMode = EditViewState::DetailMode::devices;
        viewState.focusTrack (&t);
        viewState.sendChangeMessage();
    }

    void showChainOf (te::Track* t)
    {
        if (t == nullptr)
            return;

        viewState.detailMode = EditViewState::DetailMode::devices;
        viewState.focusTrack (t);
        viewState.sendChangeMessage();

        if (onShowChain)
            onShowChain();
    }

    void open (te::AudioTrack& t)
    {
        auto f = Mastering::songFileOf (t);

        if (! f.existsAsFile())
            return relink (t);

        if (onOpenSong)
            juce::MessageManager::callAsync ([cb = onOpenSong, f] { cb (f); });
    }

    void relink (te::AudioTrack& t)
    {
        auto expected = Mastering::songFileOf (t);
        chooser = std::make_unique<juce::FileChooser> ("Where is " + Mastering::songNameOf (t) + "?",
                                                       expected.getParentDirectory().exists() ? expected.getParentDirectory() : Mastering::folderOf (edit).getParentDirectory(),
                                                       juce::String ("*") + Project::fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, track = te::Track::Ptr (&t)] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();

            if (! f.existsAsFile() || track == nullptr)
                return;

            edit.getUndoManager().beginNewTransaction ("Find Song");
            Mastering::relink (*track, f);
            album.check();
        });
    }

    void remove (te::AudioTrack& t)
    {
        edit.getUndoManager().beginNewTransaction ("Remove Song");
        edit.deleteTrack (&t);
        Mastering::arrange (edit);
    }

    void hearTransition (te::AudioTrack& t)
    {
        auto start = Mastering::rangeOf (t).getStart();
        auto& transport = edit.getTransport();
        transport.stop (false, false);
        transport.setPosition (std::max (te::TimePosition(), start - te::TimeDuration::fromSeconds (6.0)));
        transport.play (false);
    }

    void showMenu (te::AudioTrack& t)
    {
        auto songs = Mastering::getSongs (edit);
        auto index = songs.indexOf (&t);
        auto missing = ! Mastering::songFileOf (t).existsAsFile();

        juce::PopupMenu m;
        m.addItem ("Open Song", ! missing, false, [this, &t] { open (t); });
        m.addItem ("Hear the Transition", index > 0 && Mastering::clipOf (t) != nullptr, false, [this, &t] { hearTransition (t); });
        m.addItem ("Show Its Chain", [this, &t] { select (t); showChainOf (&t); });
        m.addSeparator();
        m.addItem ("Move Earlier", index > 0, false, [this, &t, index]
        {
            edit.getUndoManager().beginNewTransaction ("Move Song");
            Mastering::moveSong (edit, t, index - 1);
        });
        m.addItem ("Move Later", index < songs.size() - 1, false, [this, &t, index]
        {
            edit.getUndoManager().beginNewTransaction ("Move Song");
            Mastering::moveSong (edit, t, index + 1);
        });
        m.addSeparator();
        m.addItem (missing ? "Find Song..." : "Point to Another Project...", [this, &t] { relink (t); });
        m.addItem ("Render Again", ! missing, false, [this, &t] { album.renderAgain (t); });
        m.addItem ("Show in " + juce::String (JUCE_MAC ? "Finder" : "Explorer"), ! missing, false, [&t] { Mastering::songFileOf (t).revealToUser(); });
        m.addSeparator();
        m.addItem ("Remove from Album", [this, &t] { remove (t); });
        m.showMenuAsync (juce::PopupMenu::Options());
    }

    juce::String describeStatus (te::AudioTrack& t) const
    {
        switch (album.getStatus (t))
        {
            case Mastering::Album::Status::missing:     return "Missing: right-click to find it";
            case Mastering::Album::Status::rendering:   return "Updating... " + juce::String (juce::roundToInt (album.getRenderProgress() * 100.0f)) + "%";
            case Mastering::Album::Status::waiting:     return "Waiting to update";
            case Mastering::Album::Status::failed:      return "Couldn't render: " + album.getError (t);
            case Mastering::Album::Status::upToDate:    break;
        }

        return {};
    }

    void refresh()
    {
        auto activity = album.getActivity();
        int updating = 0, missing = 0;

        for (auto t : Mastering::getSongs (edit))
        {
            auto s = album.getStatus (*t);
            updating += (s == Mastering::Album::Status::rendering || s == Mastering::Album::Status::waiting) ? 1 : 0;
            missing += s == Mastering::Album::Status::missing ? 1 : 0;
        }

        if (activity.isEmpty() && updating > 0)
            activity = "Updating " + juce::String (updating) + (updating == 1 ? " song" : " songs") + "...";

        if (missing > 0)
            activity = (activity.isNotEmpty() ? activity + "  " : juce::String()) + juce::String (missing) + " missing";

        status.setText (activity, juce::dontSendNotification);
        measure.setButtonText (album.isMeasuring() ? "Stop" : "Measure");
        match.setEnabled (! album.isMeasuring());

        if (! target.isBeingEdited())
            target.setValue (Mastering::targetOf (edit), false);

        albumChain.setToggleState (viewState.focusedTrack.get() == edit.getMasterTrack(), juce::dontSendNotification);
        timeline.repaint();
        strip.update();
        list.repaint();
        overview.repaint();
    }

    void timerCallback() override
    {
        timeline.repaint();

        if (album.isBusy())
            refresh();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override     { refresh(); }

    //==============================================================================
    /** The album, start to end: each song a block with its waveform and fades. */
    class Timeline : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        explicit Timeline (MasteringView& o) : owner (o)
        {
            setWantsKeyboardFocus (true);
        }

        /** Where a song dropped at x would go in the order. */
        int indexAt (float x)
        {
            auto songs = Mastering::getSongs (owner.edit);
            auto t = xToTime (x);

            for (int i = 0; i < songs.size(); ++i)
            {
                auto r = displayRange (*songs[i]);
                if (t < (r.first + r.second) / 2.0)
                    return i;
            }

            return songs.size();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::panelDark);
            auto songs = Mastering::getSongs (owner.edit);
            updateScale();

            // Ruler: minutes and seconds
            g.setColour (Colours::panel);
            g.fillRect (0, 0, getWidth(), rulerH);
            g.setFont (juce::FontOptions (11.0f));

            auto step = niceStep (70.0 / pxPerSec);
            for (double t = std::floor (scroll / step) * step; timeToX (t) < (float) getWidth(); t += step)
            {
                auto x = timeToX (t);
                g.setColour (Colours::gridBar);
                g.fillRect (x, (float) rulerH - 6.0f, 1.0f, 6.0f);
                g.setColour (Colours::gridLine);
                g.fillRect (x, (float) rulerH, 1.0f, (float) getHeight() - rulerH);
                g.setColour (Colours::textDim);
                g.drawText (Mastering::formatTime (t), (int) x + 3, 0, 60, rulerH - 4, juce::Justification::centredLeft);
            }

            if (songs.isEmpty())
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (14.0f));
                g.drawText ("Drag song projects here, or click Add Song", getLocalBounds().withTrimmedTop (rulerH), juce::Justification::centred);
            }

            auto selected = owner.selected();

            for (int i = 0; i < songs.size(); ++i)
                paintSong (g, *songs[i], i, songs[i] == selected);

            // Where a drop or a drag would put the song
            auto insertAt = owner.dropIndex >= 0 ? owner.dropIndex : (dragging != nullptr && dragTarget >= 0 ? dragTarget : -1);

            if (insertAt >= 0)
            {
                double t = insertAt < songs.size() ? displayRange (*songs[insertAt]).first : Mastering::albumEnd (owner.edit).inSeconds();
                g.setColour (Colours::accent);
                g.fillRect (timeToX (t) - 1.5f, (float) rulerH, 3.0f, (float) getHeight() - rulerH);
            }

            // Playhead
            auto x = timeToX (owner.edit.getTransport().getPosition().inSeconds());
            g.setColour (Colours::playhead);
            g.fillRect (x - 0.5f, 0.0f, 1.5f, (float) getHeight());
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            grabKeyboardFocus();
            auto* song = songAt (e.position);
            dragging = (te::AudioTrack*) nullptr;
            dragMode = DragMode::none;
            dragTarget = -1;

            if (song == nullptr)
            {
                // Click the ruler or an empty spot to move the playhead
                owner.edit.getTransport().setPosition (te::TimePosition::fromSeconds (std::max (0.0, xToTime (e.position.x))));
                return;
            }

            owner.select (*song);

            if (e.mods.isPopupMenu())
                return owner.showMenu (*song);

            dragging = song;
            dragMode = handleAt (*song, e.position);
            dragStartGap = Mastering::gapOf (*song);

            if (auto c = Mastering::clipOf (*song))
            {
                dragStartFadeIn = c->getFadeIn().inSeconds();
                dragStartFadeOut = c->getFadeOut().inSeconds();
            }

            if (dragMode == DragMode::none)
                dragMode = DragMode::move;

            owner.edit.getUndoManager().beginNewTransaction (dragMode == DragMode::move ? "Move Song" : "Fade");
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging == nullptr)
                return;

            auto dt = e.getDistanceFromDragStartX() / pxPerSec;
            auto clip = Mastering::clipOf (*dragging);
            auto length = Mastering::lengthOf (*dragging);

            if (dragMode == DragMode::fadeIn && clip != nullptr)
                clip->setFadeIn (te::TimeDuration::fromSeconds (juce::jlimit (0.0, length - clip->getFadeOut().inSeconds(), dragStartFadeIn + dt)));
            else if (dragMode == DragMode::fadeOut && clip != nullptr)
                clip->setFadeOut (te::TimeDuration::fromSeconds (juce::jlimit (0.0, length - clip->getFadeIn().inSeconds(), dragStartFadeOut - dt)));
            else if (dragMode == DragMode::move && e.getDistanceFromDragStart() > 3)
            {
                // Past the middle of another song: reorder. Otherwise the gap before it changes
                auto songs = Mastering::getSongs (owner.edit);
                auto from = songs.indexOf (dragging.get());
                auto to = indexAt (e.position.x);
                auto newIndex = to > from ? to - 1 : to;
                dragTarget = (newIndex != from && std::abs (e.getDistanceFromDragStartX()) > 12) ? to : -1;

                if (dragTarget < 0)
                    dragging->state.setProperty (Mastering::gapId, juce::jlimit (-30.0, 120.0, dragStartGap + dt), &owner.edit.getUndoManager());

                Mastering::arrange (owner.edit);
            }

            owner.strip.update();
            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging != nullptr && dragMode == DragMode::move && dragTarget >= 0)
            {
                auto songs = Mastering::getSongs (owner.edit);
                auto from = songs.indexOf (dragging.get());
                dragging->state.setProperty (Mastering::gapId, dragStartGap, &owner.edit.getUndoManager());
                Mastering::moveSong (owner.edit, *dragging, dragTarget > from ? dragTarget - 1 : dragTarget);
            }

            dragging = (te::AudioTrack*) nullptr;
            dragTarget = -1;
            repaint();
            owner.refresh();
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (auto* song = songAt (e.position))
                owner.open (*song);
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            auto* song = songAt (e.position);
            auto mode = song != nullptr ? handleAt (*song, e.position) : DragMode::none;
            setMouseCursor (mode == DragMode::fadeIn || mode == DragMode::fadeOut ? juce::MouseCursor::LeftRightResizeCursor
                                                                                  : juce::MouseCursor::NormalCursor);
            setTooltip (song == nullptr ? juce::String()
                        : mode == DragMode::none ? "Drag to change the gap before it, or past another song to reorder. Double-click to open it."
                                                 : "Drag to set the fade");
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            if (e.mods.isCommandDown())
            {
                auto t = xToTime (e.position.x);
                fit = false;
                pxPerSec = juce::jlimit (0.2, 400.0, pxPerSec * (w.deltaY > 0 ? 1.25 : 0.8));
                scroll = std::max (0.0, t - e.position.x / pxPerSec);
            }
            else if (! fit)
            {
                scroll = std::max (0.0, scroll - (w.deltaX != 0.0f ? w.deltaX : w.deltaY) * 200.0 / pxPerSec);
            }

            repaint();
        }

        void mouseMagnify (const juce::MouseEvent& e, float scale) override
        {
            auto t = xToTime (e.position.x);
            fit = false;
            pxPerSec = juce::jlimit (0.2, 400.0, pxPerSec * scale);
            scroll = std::max (0.0, t - e.position.x / pxPerSec);
            repaint();
        }

        /** Zoom to show the whole album again. */
        void zoomToFit()    { fit = true; repaint(); }

        bool keyPressed (const juce::KeyPress& k) override
        {
            auto* song = owner.selected();
            auto songs = Mastering::getSongs (owner.edit);
            auto i = songs.indexOf (song);

            if (k.isKeyCode (juce::KeyPress::returnKey) && song != nullptr)
                return owner.open (*song), true;

            if ((k.isKeyCode (juce::KeyPress::leftKey) || k.isKeyCode (juce::KeyPress::rightKey)) && ! songs.isEmpty())
            {
                auto next = juce::jlimit (0, songs.size() - 1, i + (k.isKeyCode (juce::KeyPress::leftKey) ? -1 : 1));
                owner.select (*songs[next]);
                return true;
            }

            return false;
        }

    private:
        enum class DragMode { none, move, fadeIn, fadeOut };
        static constexpr int rulerH = 20;

        MasteringView& owner;
        double pxPerSec = 10.0, scroll = 0.0;
        bool fit = true;
        te::SafeSelectable<te::AudioTrack> dragging;
        DragMode dragMode = DragMode::none;
        int dragTarget = -1;
        double dragStartGap = 0.0, dragStartFadeIn = 0.0, dragStartFadeOut = 0.0;
        std::map<juce::String, std::unique_ptr<te::SmartThumbnail>> thumbnails;

        void updateScale()
        {
            if (! fit)
                return;

            auto end = std::max (30.0, Mastering::albumEnd (owner.edit).inSeconds() + 4.0);

            for (auto t : Mastering::getSongs (owner.edit))
                end = std::max (end, displayRange (*t).second + 4.0);

            pxPerSec = std::max (0.2, (getWidth() - 16) / end);
            scroll = 0.0;
        }

        float timeToX (double t) const      { return (float) ((t - scroll) * pxPerSec) + 8.0f; }
        double xToTime (float x) const      { return (x - 8.0f) / pxPerSec + scroll; }

        static double niceStep (double seconds)
        {
            for (double s : { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 })
                if (s >= seconds)
                    return s;

            return 1200.0;
        }

        /** A song's place, and a width to show before it has been rendered. */
        std::pair<double, double> displayRange (te::AudioTrack& t) const
        {
            if (Mastering::clipOf (t) != nullptr)
            {
                auto r = Mastering::rangeOf (t);
                return { r.getStart().inSeconds(), r.getEnd().inSeconds() };
            }

            // Not rendered yet: after the song before it, as wide as its last render or a placeholder
            auto songs = Mastering::getSongs (owner.edit);
            double start = 0.0;
            auto i = songs.indexOf (&t);

            if (i > 0)
                start = displayRange (*songs[i - 1]).second + std::max (0.0, Mastering::gapOf (t));

            auto length = Mastering::lengthOf (t);
            return { start, start + (length > 0.0 ? length : 40.0 / std::max (0.2, pxPerSec) + 1.0) };
        }

        juce::Rectangle<float> blockOf (te::AudioTrack& t) const
        {
            auto [a, b] = displayRange (t);
            auto x1 = timeToX (a), x2 = std::max (x1 + 6.0f, timeToX (b));
            return { x1, (float) rulerH + 8.0f, x2 - x1, (float) getHeight() - rulerH - 16.0f };
        }

        te::AudioTrack* songAt (juce::Point<float> p)
        {
            auto songs = Mastering::getSongs (owner.edit);

            // Later songs are drawn on top where they overlap
            for (int i = songs.size(); --i >= 0;)
                if (blockOf (*songs[i]).contains (p))
                    return songs[i];

            return nullptr;
        }

        DragMode handleAt (te::AudioTrack& t, juce::Point<float> p) const
        {
            auto clip = Mastering::clipOf (t);

            if (clip == nullptr)
                return DragMode::none;

            auto b = blockOf (t);

            if (p.y > b.getY() + 16.0f)
                return DragMode::none;

            auto inX = b.getX() + (float) (clip->getFadeIn().inSeconds() * pxPerSec);
            auto outX = b.getRight() - (float) (clip->getFadeOut().inSeconds() * pxPerSec);

            if (std::abs (p.x - inX) < 7.0f)
                return DragMode::fadeIn;

            if (std::abs (p.x - outX) < 7.0f)
                return DragMode::fadeOut;

            return DragMode::none;
        }

        te::SmartThumbnail* thumbnailFor (te::WaveAudioClip& clip)
        {
            auto file = clip.getSourceFileReference().getFile();
            auto key = file.getFullPathName();
            auto& t = thumbnails[key];

            if (t == nullptr && file.existsAsFile())
                t = std::make_unique<te::SmartThumbnail> (clip.edit.engine, te::AudioFile (clip.edit.engine, file), *this, &clip.edit);

            return t.get();
        }

        void paintSong (juce::Graphics& g, te::AudioTrack& t, int index, bool isSelected)
        {
            auto b = blockOf (t);
            auto colour = Theme::ofTrack (t);
            auto clip = Mastering::clipOf (t);
            auto state = owner.album.getStatus (t);

            g.setColour (colour.withAlpha (clip != nullptr ? 0.22f : 0.10f));
            g.fillRoundedRectangle (b, 4.0f);

            if (clip != nullptr)
            {
                if (auto* thumb = thumbnailFor (*clip))
                {
                    g.setColour (colour);
                    auto r = b.reduced (2.0f, 16.0f).toNearestInt();
                    thumb->drawChannels (g, r, { te::TimePosition(), clip->getPosition().getLength() }, juce::Decibels::decibelsToGain (clip->getGainDB()));
                }

                // Fades: a line from the corner, with a handle to drag
                auto inX = b.getX() + (float) (clip->getFadeIn().inSeconds() * pxPerSec);
                auto outX = b.getRight() - (float) (clip->getFadeOut().inSeconds() * pxPerSec);
                g.setColour (Colours::text.withAlpha (0.7f));
                g.drawLine (b.getX(), b.getBottom(), inX, b.getY() + 6.0f, 1.2f);
                g.drawLine (outX, b.getY() + 6.0f, b.getRight(), b.getBottom(), 1.2f);
                g.fillEllipse (inX - 4.0f, b.getY() + 2.0f, 8.0f, 8.0f);
                g.fillEllipse (outX - 4.0f, b.getY() + 2.0f, 8.0f, 8.0f);
            }

            g.setColour (isSelected ? Colours::text : colour.withAlpha (0.6f));
            g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, isSelected ? 2.0f : 1.0f);

            // Number and name, then whatever's happening to it
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
            auto label = b.reduced (8.0f, 2.0f).toNearestInt();
            g.drawText (juce::String (index + 1) + "  " + Mastering::songNameOf (t), label.removeFromBottom (16), juce::Justification::bottomLeft, true);

            if (state != Mastering::Album::Status::upToDate)
            {
                g.setFont (juce::FontOptions (11.5f));
                g.setColour (state == Mastering::Album::Status::missing || state == Mastering::Album::Status::failed ? Colours::record : Colours::textDim);
                g.drawText (owner.describeStatus (t), label.withTrimmedTop (14), juce::Justification::topLeft, true);
            }
        }
    };

    //==============================================================================
    /** The song picked: its gap and fades, and a way to open it. */
    class SongStrip : public juce::Component
    {
    public:
        explicit SongStrip (MasteringView& o) : owner (o)
        {
            gap.accessibleName = "Gap before";
            gap.unit = "seconds";
            gap.setTooltip ("Seconds of silence before this song. Below zero, it starts under the end of the song before.");
            fadeIn.accessibleName = "Fade in";
            fadeIn.unit = "seconds";
            fadeOut.accessibleName = "Fade out";
            fadeOut.unit = "seconds";

            gap.onValueChange = [this] (double v)
            {
                if (auto* t = owner.selected())
                {
                    owner.edit.getUndoManager().beginNewTransaction ("Gap");
                    Mastering::setGap (*t, v);
                }
            };
            fadeIn.onValueChange = [this] (double v)
            {
                if (auto* t = owner.selected())
                    if (auto c = Mastering::clipOf (*t))
                        c->setFadeIn (te::TimeDuration::fromSeconds (v));
            };
            fadeOut.onValueChange = [this] (double v)
            {
                if (auto* t = owner.selected())
                    if (auto c = Mastering::clipOf (*t))
                        c->setFadeOut (te::TimeDuration::fromSeconds (v));
            };

            openButton.onClick = [this] { if (auto* t = owner.selected()) owner.open (*t); };
            transition.onClick = [this] { if (auto* t = owner.selected()) owner.hearTransition (*t); };
            transition.setTooltip ("Play from a few seconds before this song starts");

            for (auto* l : { &gapLabel, &inLabel, &outLabel, &name })
            {
                l->setFont (juce::FontOptions (12.5f));
                l->setColour (juce::Label::textColourId, Colours::textDim);
                l->setJustificationType (juce::Justification::centredRight);
                addAndMakeVisible (l);
            }

            name.setJustificationType (juce::Justification::centredLeft);
            name.setColour (juce::Label::textColourId, Colours::text);
            gapLabel.setText ("Gap before", juce::dontSendNotification);
            inLabel.setText ("Fade in", juce::dontSendNotification);
            outLabel.setText ("Fade out", juce::dontSendNotification);

            for (auto* c : std::initializer_list<juce::Component*> { &gap, &fadeIn, &fadeOut, &openButton, &transition })
                addAndMakeVisible (c);
        }

        void update()
        {
            auto* t = owner.selected();
            auto clip = t != nullptr ? Mastering::clipOf (*t) : nullptr;

            for (auto* c : std::initializer_list<juce::Component*> { &gap, &fadeIn, &fadeOut, &openButton, &transition, &gapLabel, &inLabel, &outLabel })
                c->setVisible (t != nullptr);

            name.setText (t != nullptr ? Mastering::songNameOf (*t) : juce::String ("Pick a song to set its gap and fades"), juce::dontSendNotification);

            if (t == nullptr)
                return;

            if (! gap.isBeingEdited())
                gap.setValue (Mastering::gapOf (*t), false);

            fadeIn.setEnabled (clip != nullptr);
            fadeOut.setEnabled (clip != nullptr);
            transition.setEnabled (clip != nullptr && Mastering::getSongs (owner.edit).indexOf (t) > 0);

            if (clip != nullptr)
            {
                if (! fadeIn.isBeingEdited())
                    fadeIn.setValue (clip->getFadeIn().inSeconds(), false);

                if (! fadeOut.isBeingEdited())
                    fadeOut.setValue (clip->getFadeOut().inSeconds(), false);
            }
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::panel);
            g.setColour (Colours::outline);
            g.fillRect (0, getHeight() - 1, getWidth(), 1);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8, 5);
            name.setBounds (r.removeFromLeft (juce::jmin (260, r.getWidth() / 4)));

            for (auto [label, box] : { std::pair<juce::Label*, DragNumberBox*> { &gapLabel, &gap }, { &inLabel, &fadeIn }, { &outLabel, &fadeOut } })
            {
                label->setBounds (r.removeFromLeft (76));
                r.removeFromLeft (4);
                box->setBounds (r.removeFromLeft (56));
                r.removeFromLeft (8);
            }

            openButton.setBounds (r.removeFromRight (96));
            r.removeFromRight (6);
            transition.setBounds (r.removeFromRight (140));
        }

    private:
        MasteringView& owner;
        DragNumberBox gap { -30.0, 120.0, 1 }, fadeIn { 0.0, 60.0, 1 }, fadeOut { 0.0, 60.0, 1 };
        juce::Label gapLabel, inLabel, outLabel, name;
        juce::TextButton openButton { "Open Song" }, transition { "Hear the Transition" };
    };

    //==============================================================================
    /** Every song in order, with how loud it is. */
    class SongList : public juce::Component
    {
    public:
        explicit SongList (MasteringView& o) : owner (o)    { setWantsKeyboardFocus (true); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::background);
            auto songs = Mastering::getSongs (owner.edit);
            auto selected = owner.selected();
            auto r = getLocalBounds().reduced (8, 4);

            g.setFont (juce::FontOptions (11.5f));
            drawRow (g, r.removeFromTop (rowH), { "#", "Song", "Length", "Gap", "Mix", "Mastered", "True peak", "" }, Colours::textDim);
            g.setColour (Colours::outline);
            g.fillRect (r.getX(), r.getY(), r.getWidth(), 1);

            g.setFont (juce::FontOptions (13.0f));

            for (int i = 0; i < songs.size(); ++i)
            {
                auto& t = *songs[i];
                auto row = r.removeFromTop (rowH);

                if (&t == selected)
                {
                    g.setColour (Colours::panelLight);
                    g.fillRoundedRectangle (row.toFloat(), 3.0f);
                }

                g.setColour (Theme::ofTrack (t));
                g.fillRoundedRectangle ((float) row.getX() + 2.0f, (float) row.getCentreY() - 5.0f, 3.0f, 10.0f, 1.5f);

                auto peak = Mastering::getProperty (t, Mastering::masterPeakId);
                auto state = owner.album.getStatus (t);
                drawRow (g, row, { juce::String (i + 1), Mastering::songNameOf (t),
                                   Mastering::clipOf (t) != nullptr ? Mastering::formatTime (Mastering::lengthOf (t)) : juce::String(),
                                   juce::String (Mastering::gapOf (t), 1) + " s",
                                   Mastering::formatDb (Mastering::getProperty (t, Mastering::mixLufsId)),
                                   Mastering::formatDb (Mastering::getProperty (t, Mastering::masterLufsId)),
                                   Mastering::formatDb (peak),
                                   owner.describeStatus (t) },
                         state == Mastering::Album::Status::missing ? Colours::record : Colours::text,
                         peak.has_value() && *peak > -1.0 ? Colours::meterRed : juce::Colour());
            }

            if (songs.isEmpty())
            {
                g.setColour (Colours::textDim);
                g.drawText ("No songs yet", r, juce::Justification::centredTop);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            grabKeyboardFocus();

            if (auto* t = songAt (e.y))
            {
                owner.select (*t);

                if (e.mods.isPopupMenu())
                    owner.showMenu (*t);
                else if (! owner.edit.getTransport().isPlaying())
                    owner.edit.getTransport().setPosition (Mastering::rangeOf (*t).getStart());
            }
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (auto* t = songAt (e.y))
                owner.open (*t);
        }

        bool keyPressed (const juce::KeyPress& k) override
        {
            auto songs = Mastering::getSongs (owner.edit);
            auto i = songs.indexOf (owner.selected());

            if (k.isKeyCode (juce::KeyPress::returnKey) && i >= 0)
                return owner.open (*songs[i]), true;

            if ((k.isKeyCode (juce::KeyPress::upKey) || k.isKeyCode (juce::KeyPress::downKey)) && ! songs.isEmpty())
            {
                auto delta = k.isKeyCode (juce::KeyPress::upKey) ? -1 : 1;

                // Alt moves the song itself
                if (k.getModifiers().isAltDown() && i >= 0)
                {
                    owner.edit.getUndoManager().beginNewTransaction ("Move Song");
                    Mastering::moveSong (owner.edit, *songs[i], i + delta);
                }
                else
                {
                    owner.select (*songs[juce::jlimit (0, songs.size() - 1, i + delta)]);
                }

                return true;
            }

            return false;
        }

    private:
        static constexpr int rowH = 24;
        MasteringView& owner;

        te::AudioTrack* songAt (int y)
        {
            auto i = (y - 4 - rowH) / rowH;
            auto songs = Mastering::getSongs (owner.edit);
            return y > 4 + rowH && i >= 0 && i < songs.size() ? songs[i] : nullptr;
        }

        void drawRow (juce::Graphics& g, juce::Rectangle<int> row, const juce::StringArray& cells, juce::Colour base, juce::Colour peakColour = {})
        {
            static const int widths[] = { 28, -1, 56, 56, 56, 70, 72 };
            auto r = row.withTrimmedLeft (10);
            auto fixed = 0;

            for (auto w : widths)
                fixed += std::max (0, w);

            auto statusW = juce::jlimit (0, 220, (r.getWidth() - fixed) / 2);
            auto nameW = std::max (60, r.getWidth() - fixed - statusW);

            for (int c = 0; c < cells.size(); ++c)
            {
                auto w = c < 7 ? (widths[c] < 0 ? nameW : widths[c]) : r.getWidth();
                auto cell = r.removeFromLeft (w).reduced (3, 0);
                g.setColour (c == 6 && ! peakColour.isTransparent() ? peakColour : c == 7 ? Colours::textDim : base);
                g.drawText (cells[c], cell, c >= 2 && c <= 6 ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
            }

            g.setColour (base);
        }
    };

    //==============================================================================
    /** Each song's loudness side by side, against the album's target. */
    class Overview : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        explicit Overview (MasteringView& o) : owner (o)
        {
            setTooltip ("Each song's loudness after mastering (or of its mix, faded, before it's been measured), and its true peak");
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (Colours::panelDark);
            auto r = getLocalBounds().reduced (12, 8);
            auto title = r.removeFromTop (18);
            auto targetLufs = Mastering::targetOf (owner.edit);

            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.setColour (Colours::textDim);
            g.drawText ("Album loudness", title, juce::Justification::centredLeft);
            g.setFont (juce::FontOptions (11.5f));
            g.drawText ("Target " + juce::String (targetLufs, 1) + " LUFS", title, juce::Justification::centredRight);

            auto songs = Mastering::getSongs (owner.edit);
            r.removeFromTop (6);
            auto labels = r.removeFromBottom (34);
            auto scaleArea = r.removeFromLeft (30);
            constexpr double lo = -30.0, hi = -4.0;
            auto yOf = [&] (double lufs) { return (float) r.getBottom() - (float) ((juce::jlimit (lo, hi, lufs) - lo) / (hi - lo)) * (float) r.getHeight(); };

            g.setFont (juce::FontOptions (10.5f));

            for (double v = -28.0; v <= hi; v += 4.0)
            {
                g.setColour (Colours::gridLine);
                g.fillRect ((float) r.getX(), yOf (v), (float) r.getWidth(), 1.0f);
                g.setColour (Colours::textDim);
                g.drawText (juce::String ((int) v), scaleArea.withY ((int) yOf (v) - 7).withHeight (14), juce::Justification::centredRight);
            }

            if (! songs.isEmpty())
            {
                auto slot = (float) r.getWidth() / (float) songs.size();
                auto barW = juce::jlimit (6.0f, 40.0f, slot * 0.55f);

                for (int i = 0; i < songs.size(); ++i)
                {
                    auto& t = *songs[i];
                    auto mastered = Mastering::getProperty (t, Mastering::masterLufsId);
                    auto value = mastered.has_value() ? mastered : Mastering::getProperty (t, Mastering::mixLufsId);
                    auto cx = (float) r.getX() + slot * ((float) i + 0.5f);
                    auto colour = Theme::ofTrack (t);

                    if (value.has_value() && std::isfinite (*value))
                    {
                        auto top = yOf (*value);
                        g.setColour (colour.withAlpha (mastered.has_value() ? 0.9f : 0.3f));
                        g.fillRoundedRectangle (cx - barW / 2.0f, top, barW, (float) r.getBottom() - top, 2.0f);
                        g.setColour (Colours::text);
                        g.drawText (Mastering::formatDb (value), juce::Rectangle<float> (cx - 30.0f, top - 15.0f, 60.0f, 14.0f), juce::Justification::centred);
                    }

                    auto peak = Mastering::getProperty (t, Mastering::masterPeakId);
                    auto l = juce::Rectangle<float> (cx - slot / 2.0f, (float) labels.getY() + 2.0f, slot, 14.0f);
                    g.setColour (Colours::textDim);
                    g.drawText (juce::String (i + 1), l, juce::Justification::centred);
                    g.setColour (peak.has_value() && *peak > -1.0 ? Colours::meterRed : Colours::textDim);
                    g.drawText (peak.has_value() ? Mastering::formatDb (peak) + " TP" : juce::String(), l.translated (0.0f, 15.0f), juce::Justification::centred);
                }
            }

            // The target, across them all
            g.setColour (Colours::accent);
            g.fillRect ((float) r.getX(), yOf (targetLufs) - 0.75f, (float) r.getWidth(), 1.5f);
        }

    private:
        MasteringView& owner;
    };

    Timeline timeline;
    SongStrip strip;
    SongList list;
    Overview overview;

    friend class Timeline;
    friend class SongStrip;
    friend class SongList;
    friend class Overview;
};

//==============================================================================
namespace MasteringExport
{
    enum class What { songsAndAlbum, songs, album };

    /** The export dialog's controls: what to write, and the format (shared with Export Audio). */
    class DialogContent : public juce::Component
    {
    public:
        DialogContent (te::Edit& e, Export::Settings s) : edit (e), settings (s)
        {
            what.addItemList ({ "Each song and the whole album", "Each song", "The whole album" }, 1);
            what.setSelectedItemIndex (0, juce::dontSendNotification);
            what.onChange = [this] { update(); };

            const bool mp3 = Export::canWriteMp3 (edit.engine);
            for (auto f : { te::RenderFormat::wav, te::RenderFormat::aiff, te::RenderFormat::flac, te::RenderFormat::mp3, te::RenderFormat::ogg })
                format.addItem (Export::formatName (f) + (f == te::RenderFormat::mp3 && ! mp3 ? " (install LAME to use)" : juce::String()), (int) f + 1);
            format.setItemEnabled ((int) te::RenderFormat::mp3 + 1, mp3);

            if (settings.format == te::RenderFormat::mp3 && ! mp3)
                settings.format = te::RenderFormat::wav;

            format.setSelectedId ((int) settings.format + 1, juce::dontSendNotification);
            format.onChange = [this]
            {
                settings.format = (te::RenderFormat) (format.getSelectedId() - 1);

                if (settings.format == te::RenderFormat::flac && settings.bitDepth == 32)
                    settings.bitDepth = 24;

                if (auto q = Export::qualityChoices (settings.format); ! q.empty())
                    settings.quality = q.front().value;

                updateDepth();
                update();
            };

            depth.onChange = [this]
            {
                (settings.hasBitDepth() ? settings.bitDepth : settings.quality) = depth.getSelectedId() - 1;
                update();
            };

            for (double r : { 44100.0, 48000.0, 88200.0, 96000.0 })
                rate.addItem (juce::String (r / 1000.0, r == 44100.0 || r == 88200.0 ? 1 : 0) + " kHz", (int) r);
            rate.setSelectedId ((int) settings.sampleRate, juce::dontSendNotification);
            rate.onChange = [this] { settings.sampleRate = rate.getSelectedId(); update(); };

            aiTraining.addItem ("Reserve rights", 1);
            aiTraining.addItem ("No notice", 2);
            aiTraining.setSelectedId (settings.reserveRights ? 1 : 2, juce::dontSendNotification);
            aiTraining.onChange = [this] { settings.reserveRights = aiTraining.getSelectedId() == 1; update(); };

            dither.setButtonText ("Dither");
            dither.setToggleState (settings.dither, juce::dontSendNotification);
            dither.onClick = [this] { settings.dither = dither.getToggleState(); update(); };

            for (auto* c : std::initializer_list<juce::Component*> { &what, &format, &depth, &rate, &aiTraining, &dither })
                addAndMakeVisible (c);

            preview.setJustificationType (juce::Justification::topLeft);
            preview.setFont (juce::FontOptions (12.5f));
            preview.setColour (juce::Label::textColourId, Colours::textDim);
            addAndMakeVisible (preview);

            setSize (480, 5 * rowH + 30 + 96);
            updateDepth();
            update();
        }

        const Export::Settings& getSettings() const     { return settings; }
        What getWhat() const                            { return (What) what.getSelectedItemIndex(); }

        Export::Plan makePlan (const juce::File& folder) const
        {
            return Mastering::planExport (edit, settings, folder, getWhat() != What::album, getWhat() != What::songs);
        }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (13.0f));
            int y = 0;

            for (auto* label : { "Export", "Format", settings.hasBitDepth() ? "Bit depth" : "Quality", "Sample rate", "AI training" })
            {
                g.drawText (label, 0, y, labelW - 10, rowH - 6, juce::Justification::centredRight);
                y += rowH;
            }
        }

        void resized() override
        {
            auto r = getLocalBounds();

            for (auto* c : std::initializer_list<juce::Component*> { &what, &format, &depth, &rate, &aiTraining })
                c->setBounds (r.removeFromTop (rowH).withTrimmedLeft (labelW).withTrimmedBottom (6));

            dither.setBounds (r.removeFromTop (30).withTrimmedLeft (labelW - 4).removeFromLeft (100));
            preview.setBounds (r.withTrimmedLeft (labelW - 4).withTrimmedTop (6));
        }

    private:
        static constexpr int rowH = 34, labelW = 100;
        te::Edit& edit;
        Export::Settings settings;
        juce::ComboBox what, format, depth, rate, aiTraining;
        juce::ToggleButton dither;
        juce::Label preview;

        void updateDepth()
        {
            depth.clear (juce::dontSendNotification);

            if (settings.hasBitDepth())
            {
                depth.addItem ("16-bit", 17);
                depth.addItem ("24-bit", 25);

                if (settings.format != te::RenderFormat::flac)
                    depth.addItem ("32-bit float", 33);

                depth.setSelectedId (settings.bitDepth + 1, juce::dontSendNotification);
            }
            else
            {
                for (auto& q : Export::qualityChoices (settings.format))
                    depth.addItem (q.name, q.value + 1);

                depth.setSelectedId (settings.quality + 1, juce::dontSendNotification);
            }

            repaint();
        }

        void update()
        {
            dither.setEnabled (settings.canDither());
            auto songs = Mastering::getSongs (edit).size();
            auto folder = Mastering::newExportFolder (Mastering::folderOf (edit).getChildFile ("Exports"), Mastering::nameOf (edit));
            auto ext = Export::extensionFor (settings.format);
            juce::String text;

            if (getWhat() != What::album)
                text << "Writes each song mastered, numbered in album order (" << Mastering::songFileNames ({ "First Song" }, ext)[0] << "...)";

            if (getWhat() == What::songsAndAlbum)
                text << ", and ";
            else if (getWhat() == What::album)
                text << "Writes ";

            if (getWhat() != What::songs)
                text << "the whole album as one file (" << Mastering::albumFileName (Mastering::nameOf (edit), ext) << ")";

            text << ", to Exports/" << folder.getFileName() << ", " << Export::describeFormat (settings) << ".";
            text << "\nEach song keeps the loudness its chain gives it; the songs end where their clips (and fades) do.";

            if (settings.reserveRights)
                text << "\nEach file is marked as not licensed for AI training in its metadata. The sound is unchanged.";

            if (songs == 0)
                text << "\nAdd some songs first.";

            preview.setText (text, juce::dontSendNotification);
        }
    };

    inline void showDialog (te::Edit& edit, Mastering::Album& album)
    {
        if (album.isBusy())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Songs are still updating",
                                                    "Wait until every song's render is up to date (and any measuring has finished), then export.");
            return;
        }

        auto settings = Export::lastSettings();

        if (auto rate = edit.engine.getDeviceManager().getSampleRate(); ! settings.chosen && (rate == 44100.0 || rate == 48000.0 || rate == 88200.0 || rate == 96000.0))
            settings.sampleRate = rate;

        auto name = Mastering::nameOf (edit);
        auto content = std::make_shared<DialogContent> (edit, settings);
        auto* aw = Theme::newAlertWindow ("Export Album", name);
        aw->addCustomComponent (content.get());
        aw->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create ([content, &edit, name] (int result)
        {
            if (result != 1)
                return;

            Export::lastSettings() = content->getSettings();
            Export::lastSettings().chosen = true;
            auto folder = Mastering::newExportFolder (Mastering::folderOf (edit).getChildFile ("Exports"), name);
            auto plan = content->makePlan (folder);

            if (plan.jobs.empty())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Nothing to export",
                                                        plan.problems.isEmpty() ? juce::String ("Add some songs first.") : plan.problems.joinIntoString ("\n"));
                return;
            }

            // Each song has a file of its own: show the folder when it's done
            plan.numStems = std::max (plan.numStems, 1);
            auto progress = std::make_shared<Export::Progress> (edit, std::move (plan));
            auto* pw = Theme::newAlertWindow ("Exporting", name);
            pw->addCustomComponent (progress.get());
            pw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            juce::Component::SafePointer<juce::AlertWindow> safe (pw);
            progress->onDone = [safe] (juce::String title, juce::String problems)
            {
                if (safe != nullptr)
                    safe->exitModalState (1);

                if (problems.isNotEmpty())
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, problems);
            };

            pw->enterModalState (true, juce::ModalCallbackFunction::create ([progress] (int r)
            {
                if (r == 0)
                    progress->cancel();
            }), true);
        }), true);
    }
}

inline void MasteringView::showExport()     { MasteringExport::showDialog (edit, album); }

} // namespace elsewhere
