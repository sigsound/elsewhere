// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"
#include "Mixing.h"
#include "PluginWindow.h"
#include "MasteringView.h"
#include "SomewhereBeat.h"

namespace elsewhere
{
//==============================================================================
MainComponent::MainComponent (te::Engine& e, const juce::File& editFile, AppActions& actions, bool recover)
    : engine (e), appActions (actions)
{
    tooltips.setOpaque (false);   // so its rounded corners show what's behind

    const bool isNewProject = ! editFile.existsAsFile();
    project = std::make_unique<Project> (engine, editFile, recover);
    auto& edit = project->getEdit();

    viewState = std::make_unique<EditViewState> (edit, selectionManager);
    sessionRecorder = std::make_unique<SessionRecorder> (edit);
    takeRecorder = std::make_unique<Takes::Recorder> (*viewState);
    ControlSurfaces::attach (*viewState);
    ElsewhereUIBehaviour::currentEdit() = &edit;
    ElsewhereDevice::importFileHook = [sp = juce::Component::SafePointer<MainComponent> (this)] (const juce::File& f)
    {
        return sp != nullptr ? sp->project->importAudioFile (f) : f;
    };
    setupInputs();

    // Collaboration (only active if this project has sharing turned on)
    collab = std::make_unique<Collab::Session> (*project, Collab::Identity::load (engine.getPropertyStorage().getPropertiesFile()));
    drumWatcher = std::make_unique<DrumTrack::Watcher> (edit, [c = collab.get()] { return c->isApplyingRemote(); });
    project->beforeSave = [this] { collab->prepareForSave(); };
    project->afterSave = [this] { collab->writeSnapshot(); };
    project->savedByName = collab->getIdentity().name;

    if (collab->isActive())
        startCollaboration();

    viewState->addComment = [this] (te::EditItemID target, te::TimeRange range, const juce::String& name)
    {
        // Once it's written, open it in the notes book
        Comments::promptAndAdd (project->getEdit(), collab->getIdentity(), target, range, name, [this] (const juce::String& id)
        {
            viewState->openCommentId = id;
            viewState->detailMode = EditViewState::DetailMode::comments;
            viewState->sendChangeMessage();
        });
    };
    viewState->getCommentCount = [this] (te::EditItemID id) { return (int) Comments::getFor (project->getEdit(), id).size(); };
    viewState->getCommentText = [this] (te::EditItemID id)
    {
        juce::StringArray lines;
        for (auto& c : Comments::getFor (project->getEdit(), id))
            lines.add (c.getAuthor() + ": " + c.getText());
        return lines.joinIntoString ("\n");
    };

    transportBar = std::make_unique<TransportBar> (*viewState);
    browser = std::make_unique<Browser> (engine, project->getFolder());
    arrangement = std::make_unique<ArrangementView> (*viewState);
    session = std::make_unique<SessionView> (*viewState);
    mixer = std::make_unique<MixerView> (*viewState);
    trackList = std::make_unique<TrackList> (*viewState);
    clipList = std::make_unique<ClipList> (*viewState);
    detail = std::make_unique<DetailView> (*viewState);

    // F6 moves between these; Tab moves between the controls inside one and wraps round
    for (auto [region, title] : std::initializer_list<std::pair<juce::Component*, const char*>> {
             { transportBar.get(), "Transport" }, { browser.get(), "Browser" }, { trackList.get(), "Track List" },
             { arrangement.get(), "Arrangement" }, { session.get(), "Session" }, { mixer.get(), "Mixer" },
             { clipList.get(), "Clips and Markers" }, { detail.get(), "Device Panel" } })
    {
        region->setTitle (title);
        region->setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
    }
    detail->setCommentsPanel (std::make_unique<Comments::Book> (*viewState, collab->getIdentity()));

    live = std::make_unique<Live::Session> (*project, *collab);

    somewhereLink = std::make_unique<SomewhereLink> (*project, *viewState, engine.getPropertyStorage().getPropertiesFile());
    somewhereLink->onSampleDropped = [this] (const juce::File& f)
    {
        auto target = viewState->focusedTrack.get();

        if (target == nullptr || target->isMasterTrack())
            target = addTrack (false);

        if (target != nullptr)
            importFile (f, *target, project->getEdit().getTransport().getPosition());
    };

    // A beat sent from the phone: it arrives at bar 1, as MIDI on the same sounds
    somewhereLink->onBeat = [this] (const juce::File& folder, const juce::String& songId)
    {
        importSomewhereBeat (folder, te::TimePosition(), songId);
    };

    // A take recorded on the phone: onto that phone's own track, at the beat it started
    somewhereLink->onTake = [this] (const juce::File& f, double beat, const juce::String& phoneName) -> juce::String
    {
        auto& edit = project->getEdit();
        auto name = "Somewhere (" + phoneName + ")";
        te::AudioTrack* track = nullptr;

        for (auto* t : te::getAudioTracks (edit))
            if (t->getName() == name)
                track = t;

        if (track == nullptr)
            if ((track = addTrack (false)) != nullptr)
                track->setName (name);

        if (track == nullptr)
            return {};

        edit.getUndoManager().beginNewTransaction ("Somewhere take");
        te::AudioFile af (engine, f);
        auto start = edit.tempoSequence.toTime (te::BeatPosition::fromBeats (beat));

        if (auto clip = track->insertWaveClip (f.getFileNameWithoutExtension(), f,
                                               { { start, te::TimeDuration::fromSeconds (af.getLength()) }, {} }, false))
            clip->setAutoTempo (false);   // it was played at this tempo: no warping

        return name;
    };

    // Shared Session view: what you launch launches for the other players too
    Launcher::broadcaster() = [sp = juce::Component::SafePointer<MainComponent> (this)] (const juce::var& v)
    {
        if (sp != nullptr && sp->live != nullptr)
            sp->live->sendLauncherAction (v);
    };

    live->onLauncherAction = [this] (const juce::String& from, const juce::var& v)
    {
        if (! live->isSharingLaunches())
            return;

        auto what = Launcher::applyShared (project->getEdit(), v);
        if (what.isNotEmpty())
            std::cout << "LIVE-LAUNCH " << from << ": " << what << std::endl;
    };
    auto livePanel = std::make_unique<LivePanel> (*viewState, *live, *collab);
    livePanel->onNeedsSharing = [this] { showShareDialog(); };
    detail->setLivePanel (std::move (livePanel));

    arrangement->onFileDropped = [this] (const juce::File& f, te::Track& t, te::TimePosition time) { importFile (f, t, time); };
    arrangement->onFilesDroppedAsNewTracks = [this] (const juce::StringArray& paths, te::TimePosition time)
    {
        juce::Array<juce::File> files;
        for (auto& p : paths)
            files.add (juce::File (p));
        importAsNewTracks (files, time);
    };
    session->onFileDropped = [this] (const juce::File& f, te::ClipSlot& s) { importFileToSlot (f, s); };
    browser->onFileChosen = [this] (const juce::File& f)
    {
        if (f.hasFileExtension ("elsewhere;tandem"))
            return appActions.openProject (f);

        auto target = viewState->focusedTrack.get();

        if (target == nullptr || target->isMasterTrack())
            target = addTrack (f.hasFileExtension (te::midiFileExtensions));

        if (target != nullptr)
            importFile (f, *target, project->getEdit().getTransport().getPosition());
    };

    transportBar->onToggleView = [this] { perform (cmdToggleMixer); };
    transportBar->onBackToAlbum = [this] { perform (cmdBackToAlbum); };

    // An insert that opens in the device panel: make sure the panel is showing
    mixer->onShowDevices = [this]
    {
        if (! layout.detail)
        {
            layout.detail = true;
            saveLayout();
            updateViewVisibility();
        }
    };

    transportBar->onEditMode = [this] (EditViewState::EditMode m) { setEditMode (m); };
    viewState->editMode = (EditViewState::EditMode) juce::jlimit (0, 3, props().getIntValue ("editMode", (int) EditViewState::EditMode::grid));

    layout = ViewLayout::load (props());
    transportBar->onLiveClicked = [this] { toggleLive(); };

    for (auto* c : std::initializer_list<juce::Component*> { transportBar.get(), browser.get(), arrangement.get(),
                                                             session.get(), mixer.get(), trackList.get(), clipList.get(), detail.get(), &detailResizer })
        addAndMakeVisible (c);

    // A mastering project: the album in place of the arrangement and session
    if (Mastering::isMastering (edit))
    {
        mastering = std::make_unique<MasteringView> (*viewState);
        mastering->setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
        mastering->onShowChain = [this] { mixer->onShowDevices(); };
        mastering->onOpenSong = [this] (const juce::File& song)
        {
            // Saved first, so the song opens without asking, and comes back to the album as it was
            if (project->hasChangedSinceSaved())
                project->save();

            Mastering::setReturn (props(), project->getEditFile(), song);
            appActions.openProject (song);
        };
        addAndMakeVisible (*mastering);
    }

    detailResizer.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    detailResizer.onStart = [this] { detailHeightAtDragStart = layout.detailHeight; };
    detailResizer.onDrag = [this] (int dy)
    {
        layout.detailHeight = juce::jlimit (80, juce::jmax (80, getHeight() - 200), detailHeightAtDragStart - dy);
        saveLayout();
        resized();
    };

    // New projects start with one MIDI and one audio track
    // (replacing the empty track the engine creates)
    if (isNewProject && mastering == nullptr)
    {
        for (auto t : te::getAudioTracks (edit))
            edit.deleteTrack (t);

        addTrack (true);
        addTrack (false);
        edit.getUndoManager().clearUndoHistory();
        project->save();
    }

    if (auto first = te::getAudioTracks (edit).getFirst())
        viewState->focusTrack (first);

    viewState->addChangeListener (this);
    selectionManager.addChangeListener (this);
    edit.getTransport().addChangeListener (this);

    updateViewVisibility();
    routeMidiToFocusedTrack();
    setWantsKeyboardFocus (true);
    addAndMakeVisible (focusRing);
    startTimer (1000);

    // Warn about Dropbox conflicts (shared projects don't need to: every change is in the op logs)
    auto conflicts = project->findConflictedCopies();

    if (! conflicts.isEmpty() && ! collab->isActive())
        juce::MessageManager::callAsync ([n = conflicts.size()]
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Conflicted copies found",
                juce::String (n) + " conflicted copy file(s) exist in this project folder. This happens when two people save "
                "the same project at the same time. Open the conflicted copy separately to recover anything you need, then delete it.");
        });
}

MainComponent::~MainComponent()
{
    stopTimer();

    // Closed mid-take (quitting, opening another project): the take is kept, and saved, as it
    // is when recording stops any other way
    if (auto& transport = project->getEdit().getTransport(); transport.isRecording())
    {
        transport.stop (false, false);
        EngineHelpers::keepInputWhenOverloaded (engine, false);
        project->save();
    }
    engine.getMidiLearnState().setActive (false);

    if (ElsewhereUIBehaviour::currentEdit() == &project->getEdit())
        ElsewhereUIBehaviour::currentEdit() = nullptr;

    project->getEdit().getTransport().removeChangeListener (this);
    selectionManager.removeChangeListener (this);
    viewState->removeChangeListener (this);
    project->getEdit().getTransport().stop (false, true);

    // Components must go before the edit they reference
    project->beforeSave = nullptr;
    project->afterSave = nullptr;
    mastering.reset();
    detail.reset();   // the live panel refers to the live session
    somewhereLink.reset();
    live.reset();
    collab.reset();
    detail.reset();
    session.reset();
    arrangement.reset();
    mixer.reset();
    trackList.reset();
    clipList.reset();
    browser.reset();
    transportBar.reset();
    ControlSurfaces::detach (*viewState);
    takeRecorder.reset();
    selectionManager.deselectAll();
    viewState.reset();
}

//==============================================================================
void MainComponent::setupInputs()
{
    auto& dm = engine.getDeviceManager();

    for (auto& midiIn : dm.getMidiInDevices())
    {
        midiIn->setMonitorMode (te::InputDevice::MonitorMode::automatic);
        midiIn->setEnabled (true);
    }

    for (auto wip : dm.getWaveInputDevices())
        wip->setMonitorMode (te::InputDevice::MonitorMode::automatic);

    project->getEdit().getTransport().ensureContextAllocated();
}

/** "Auto arm": whichever instrument track is selected receives the MIDI keyboard. */
void MainComponent::routeMidiToFocusedTrack()
{
    auto at = dynamic_cast<te::AudioTrack*> (viewState->focusedTrack.get());

    if (at == nullptr || ! EngineHelpers::isInstrumentTrack (*at) || lastMidiTarget == at)
        return;

    auto& edit = project->getEdit();

    for (auto instance : edit.getAllInputDevices())
    {
        auto type = instance->getInputDevice().getDeviceType();

        if (type == te::InputDevice::physicalMidiDevice || type == te::InputDevice::virtualMidiDevice)
        {
            // Move MIDI inputs from the previous instrument track to the new one
            if (lastMidiTarget != nullptr)
            {
                if (EngineHelpers::isTrackArmed (*dynamic_cast<te::AudioTrack*> (lastMidiTarget.get())))
                    continue; // leave armed tracks alone

                [[maybe_unused]] auto r = instance->removeTarget (lastMidiTarget->itemID, nullptr);
            }

            if (instance->getTargets().isEmpty())
            {
                [[maybe_unused]] auto res = instance->setTarget (at->itemID, false, nullptr);
                instance->setRecordingEnabled (at->itemID, true);
            }
        }
    }

    lastMidiTarget = at;
}

te::Track* MainComponent::getLastNonReturnTrack()
{
    te::Track* last = nullptr;

    for (auto t : te::getTopLevelTracks (project->getEdit()))
        if ((t->isAudioTrack() || t->isFolderTrack()) && ! Mixing::isReturnTrack (*t))
            last = t;

    return last;
}

te::AudioTrack* MainComponent::addDrumTrack()
{
    auto& edit = project->getEdit();
    auto track = addTrack (true);
    if (track == nullptr)
        return nullptr;

    // Swap the default synth for Elsewhere Drums, and mark the track as a drum track
    for (auto p : juce::Array<te::Plugin*> (track->pluginList.getPlugins().begin(), track->pluginList.getPlugins().size()))
        if (p->isSynth())
            p->deleteFromParent();

    if (auto drums = DeviceRegistry::create (edit, "elsewhereDrums"))
    {
        track->pluginList.insertPlugin (drums, 0, nullptr);

        // Kick + crash together can reach full scale: start with some headroom
        if (auto vol = drums->getAutomatableParameterByID ("volume"))
            vol->setParameter (-6.0f, juce::sendNotification);
    }

    track->state.setProperty (DrumTrack::trackProp, true, &edit.getUndoManager());
    track->setName ("Drums");
    DrumTrack::createSection (*track, 0.0, 8.0 * juce::jmax (1, edit.tempoSequence.getTimeSig (0)->numerator.get()), "Verse");
    viewState->focusTrack (track);
    return track;
}

te::AudioTrack* MainComponent::addTrack (bool withInstrument)
{
    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction (withInstrument ? "New MIDI Track" : "New Audio Track");

    te::Track* preceding = viewState->focusedTrack.get();

    if (preceding == nullptr || preceding->isMasterTrack() || Mixing::isReturnTrack (*preceding))
        preceding = getLastNonReturnTrack();

    auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr);

    if (track == nullptr)
        return nullptr;

    auto index = te::getAudioTracks (edit).indexOf (track.get());
    track->setColour (EngineHelpers::trackColourFor (index));

    if (withInstrument)
    {
        track->setName ("MIDI " + juce::String (index + 1));
        EngineHelpers::insertBuiltInPlugin (*track, te::FourOscPlugin::xmlTypeName, 0);
    }
    else
    {
        track->setName ("Audio " + juce::String (index + 1));

        // Give each new audio track the next free mono input
        for (auto instance : edit.getAllInputDevices())
        {
            if (instance->getInputDevice().getDeviceType() != te::InputDevice::waveDevice)
                continue;

            if (instance->getTargets().isEmpty() && instance->getInputDevice().getName().containsChar ('/') == false)
            {
                EngineHelpers::assignInputToTrack (*instance, *track);
                break;
            }
        }
    }

    auto numScenes = edit.getSceneList().getScenes().size();
    track->getClipSlotList().ensureNumberOfSlots (numScenes);
    Mixing::ensureSends (*track);

    viewState->focusTrack (track.get());
    selectionManager.selectOnly (track.get());
    return track.get();
}

//==============================================================================
void MainComponent::importFile (const juce::File& f, te::Track& t, te::TimePosition time)
{
    // A beat from Somewhere: all of it, on new tracks
    if (SomewhereBeat::isBeatFolder (f))
        return importSomewhereBeat (f, time);

    auto* at = dynamic_cast<te::AudioTrack*> (&t);

    if (at == nullptr || ! f.existsAsFile())
        return;

    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction ("Import " + f.getFileName());

    if (f.hasFileExtension (te::soundFileExtensions))
    {
        auto local = project->importAudioFile (f);
        te::AudioFile af (engine, local);

        if (! af.isValid())
            return;

        auto len = te::TimeDuration::fromSeconds (af.getLength());

        if (auto clip = at->insertWaveClip (f.getFileNameWithoutExtension(), local, { { time, len }, {} }, false))
            selectionManager.selectOnly (clip.get());
    }
    else if (f.hasFileExtension (te::midiFileExtensions))
    {
        if (auto clip = te::createClipFromFile (f, *at, te::MidiList::looksLikeMPEData (f)))
        {
            clip->setStart (time, false, true);
            selectionManager.selectOnly (clip.get());
        }
    }
}

//==============================================================================
namespace
{
    /** Copies files into the project with a progress bar, then calls back on the message thread. */
    struct ImportJob : public juce::ThreadWithProgressWindow
    {
        ImportJob (Project& p, juce::Array<juce::File> f, std::function<void (juce::Array<juce::File>)> done)
            : juce::ThreadWithProgressWindow ("Importing audio...", true, true), project (p), files (std::move (f)), onDone (std::move (done)) {}

        void run() override
        {
            for (int i = 0; i < files.size() && ! threadShouldExit(); ++i)
            {
                setStatusMessage ("Copying " + files[i].getFileName() + " into the project");
                setProgress ((double) i / files.size());
                auto f = files[i];
                results.add (f.hasFileExtension (te::soundFileExtensions) ? project.importAudioFile (f) : f);
            }

            setProgress (1.0);
        }

        void threadComplete (bool cancelled) override
        {
            if (! cancelled)
                onDone (results);

            delete this;
        }

        Project& project;
        juce::Array<juce::File> files, results;
        std::function<void (juce::Array<juce::File>)> onDone;
    };
}

void MainComponent::showImportDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import audio or MIDI (select several files to import stems)",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   EngineHelpers::toWildcards (te::soundFileAndMidiExtensions));

    // Folders too: a beat folder from Somewhere imports in one go
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                            | juce::FileBrowserComponent::canSelectDirectories | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        auto files = fc.getResults();

        if (files.isEmpty())
            return;

        // A single file goes onto the selected track at the playhead; several become new tracks at bar 1
        auto target = viewState->focusedTrack.get();

        if (files.size() == 1 && ! files[0].isDirectory() && target != nullptr && ! target->isMasterTrack() && ! te::getAudioTracks (project->getEdit()).isEmpty())
            importFile (files[0], *target, project->getEdit().getTransport().getPosition());
        else
            importAsNewTracks (files, 0s);
    });
}

void MainComponent::importAsNewTracks (juce::Array<juce::File> files, te::TimePosition start)
{
    if (auto folder = SomewhereBeat::findBeatFolder (files); folder != juce::File())
        return importSomewhereBeat (folder, start);

    files.removeIf ([] (const juce::File& f) { return ! f.hasFileExtension (te::soundFileAndMidiExtensions); });

    if (files.isEmpty())
        return;

    // Sort so stems keep a sensible order ("01 Kick", "02 Snare"...)
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareNatural (b.getFileName()) < 0;
    });

    auto safeThis = SafePointer<MainComponent> (this);
    auto originals = files;
    auto* job = new ImportJob (*project, files, [safeThis, originals, start] (juce::Array<juce::File> local)
    {
        if (safeThis != nullptr)
            safeThis->createTracksForImportedFiles (originals, local, start);
    });
    job->launchThread();
}

void MainComponent::importSomewhereBeat (const juce::File& folder, te::TimePosition start, const juce::String& replacing)
{
    auto beat = SomewhereBeat::read (folder);

    if (! beat)
        return;

    juce::Array<juce::File> stems;
    for (auto& s : beat->stems)
        stems.add (s.file);

    auto safeThis = SafePointer<MainComponent> (this);
    auto* job = new ImportJob (*project, stems, [safeThis, b = *beat, start, replacing] (juce::Array<juce::File> local)
    {
        if (safeThis == nullptr)
            return;

        auto& edit = safeThis->project->getEdit();
        edit.getUndoManager().beginNewTransaction ("Import " + b.name);
        auto* proj = safeThis->project.get();

        // Sent again from the phone: its new tracks take the old ones' place (one undo step brings them back)
        static const juce::Identifier sentAs ("somewhereSong");
        te::Track* after = safeThis->getLastNonReturnTrack();
        if (replacing.isNotEmpty())
        {
            juce::Array<te::Track*> old;
            for (auto* t : te::getAudioTracks (edit))
                if (t->state[sentAs].toString() == replacing)
                    old.add (t);

            if (! old.isEmpty())
            {
                after = old.getFirst()->getSiblingTrack (-1, false);
                for (auto* t : old)
                    edit.deleteTrack (t);
            }
        }

        auto r = SomewhereBeat::build (edit, b, local, start, after,
                                       [proj, sounds = b.name + " Sounds"] (const juce::File& f) { return proj->importAudioFile (f, sounds); });

        juce::Array<te::AudioTrack*> made (r.stems);
        made.addArray (r.keysBySound);
        made.add (r.drums, r.keys);

        for (auto* t : made)
        {
            if (t == nullptr)
                continue;

            if (replacing.isNotEmpty())
                t->state.setProperty (sentAs, replacing, &edit.getUndoManager());

            // Ready for the phone's FX pad: each of the beat's tracks gets its Somewhere Control
            SomewhereControlDevice::findOrAddTo (*t);
        }

        if (auto* focus = r.drums != nullptr ? r.drums : r.keys != nullptr ? r.keys : r.stems.getFirst())
            safeThis->viewState->focusTrack (focus);

        if (! safeThis->isViewShowing (ViewLayout::MainView::arrangement))
            safeThis->setMainView (ViewLayout::MainView::arrangement);

        juce::MessageManager::callAsync ([safeThis] { if (safeThis) safeThis->arrangement->zoomToFit(); });

        juce::String message;
        auto bpmText = [] (double bpm) { return juce::String (bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); };

        if (r.songBpm > 0.0)
            message << "The beat is " << bpmText (b.bpm) << " bpm and this song " << bpmText (r.songBpm) << " bpm. Its MIDI tracks follow the song's tempo, "
                       "but its stems play as recorded, so they only line up once the song is at " << bpmText (b.bpm) << " bpm.\n\n";

        if (r.mixedKeys)
            message << "The keys play more than one sound, so they play from their stem. Their notes are on the muted Keys track; "
                       "export the beat again from an up-to-date Somewhere to get a track for each sound.\n\n";

        if (! r.missing.isEmpty())
            message << "These sounds didn't come with the beat, so it plays from its stems for now:\n" << r.missing.joinIntoString ("\n")
                    << "\n\nExport the beat again from an up-to-date Somewhere to edit it as MIDI.";

        if (message.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Import " + b.name, message.trim());
    });
    job->launchThread();
}

void MainComponent::createTracksForImportedFiles (const juce::Array<juce::File>& originals, const juce::Array<juce::File>& local,
                                                   te::TimePosition start)
{
    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction ("Import " + juce::String (local.size()) + " files");

    juce::StringArray failed;
    double firstRate = 0.0;
    bool mixedRates = false;
    te::Track* lastTrack = nullptr;

    for (int i = 0; i < local.size(); ++i)
    {
        auto f = local[i];
        bool isMidi = f.hasFileExtension (te::midiFileExtensions);

        // Build the track without the default input so importing 20 stems doesn't grab 20 inputs
        auto preceding = lastTrack != nullptr ? lastTrack : getLastNonReturnTrack();
        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr);

        if (track == nullptr)
            continue;

        auto index = te::getAudioTracks (edit).indexOf (track.get());
        track->setName (originals[i].getFileNameWithoutExtension());
        track->setColour (EngineHelpers::trackColourFor (index));
        track->getClipSlotList().ensureNumberOfSlots (edit.getSceneList().getScenes().size());
        Mixing::ensureSends (*track);
        lastTrack = track.get();

        if (isMidi)
        {
            EngineHelpers::insertBuiltInPlugin (*track, te::FourOscPlugin::xmlTypeName, 0);

            if (auto clip = te::createClipFromFile (f, *track, te::MidiList::looksLikeMPEData (f)))
                clip->setStart (start, false, true);

            continue;
        }

        te::AudioFile af (engine, f);

        if (! af.isValid())
        {
            failed.add (originals[i].getFileName());
            edit.deleteTrack (track.get());
            lastTrack = nullptr;
            continue;
        }

        auto rate = af.getSampleRate();
        if (firstRate == 0.0) firstRate = rate;
        else if (! juce::approximatelyEqual (rate, firstRate)) mixedRates = true;

        // Stems play exactly as exported: no warping, no fades
        if (auto clip = track->insertWaveClip (originals[i].getFileNameWithoutExtension(), f,
                                               { { start, te::TimeDuration::fromSeconds (af.getLength()) }, {} }, false))
        {
            clip->setAutoTempo (false);
            clip->setFadeIn ({});
            clip->setFadeOut ({});
        }
    }

    if (lastTrack != nullptr)
        viewState->focusTrack (lastTrack);

    // Switch to the arrangement so the imported stems are visible
    if (! isViewShowing (ViewLayout::MainView::arrangement) && local.size() > 1)
    {
        setMainView (ViewLayout::MainView::arrangement);
        juce::MessageManager::callAsync ([sp = SafePointer<MainComponent> (this)] { if (sp) sp->arrangement->zoomToFit(); });
    }

    juce::String message;

    if (! failed.isEmpty())
        message << "These files couldn't be read:\n" << failed.joinIntoString ("\n") << "\n\n";

    if (mixedRates)
        message << "The imported files have different sample rates. They'll play at the right speed, "
                   "but converting them to match your project gives the best quality.\n\n";

    auto deviceRate = engine.getDeviceManager().getSampleRate();

    if (firstRate > 0.0 && deviceRate > 0.0 && ! juce::approximatelyEqual (firstRate, deviceRate))
        message << "The stems are " << juce::String (firstRate / 1000.0, 1) << " kHz but your interface is running at "
                << juce::String (deviceRate / 1000.0, 1) << " kHz. They'll be resampled on playback; for mixing, "
                   "consider setting your interface to " << juce::String (firstRate / 1000.0, 1) << " kHz in Audio Settings.";

    if (message.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Import", message.trim());
}

void MainComponent::showWholeArrangement()
{
    if (! isViewShowing (ViewLayout::MainView::arrangement))
        setMainView (ViewLayout::MainView::arrangement);

    juce::MessageManager::callAsync ([sp = SafePointer<MainComponent> (this)] { if (sp) sp->arrangement->zoomToFit(); });
}

void MainComponent::importFileToSlot (const juce::File& f, te::ClipSlot& slot)
{
    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction ("Import " + f.getFileName());

    if (auto existing = slot.getClip())
        existing->removeFromParent();

    if (f.hasFileExtension (te::soundFileExtensions))
    {
        auto local = project->importAudioFile (f);
        te::AudioFile af (engine, local);

        if (! af.isValid())
            return;

        if (auto wc = te::insertWaveClip (slot, f.getFileNameWithoutExtension(), local,
                                          { { 0_tp, te::TimeDuration::fromSeconds (af.getLength()) } },
                                          te::DeleteExistingClips::yes))
        {
            // Loops follow the song tempo in Session view
            wc->setAutoTempo (true);
            wc->setLoopRangeBeats ({ 0_bp, wc->getLengthInBeats() });
        }
    }
    else if (f.hasFileExtension (te::midiFileExtensions))
    {
        if (auto mc = te::createClipFromFile (f, slot, te::MidiList::looksLikeMPEData (f)))
            mc->setLoopRangeBeats (mc->getEditBeatRange());
    }
}

//==============================================================================
void MainComponent::startCollaboration()
{
    project->nameImportsByContent = true;
    ElsewhereEngineBehaviour::recordingTag() = collab->getIdentity().getShortTag();
}

namespace
{
    juce::File findCloudFolder()
    {
        auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

        for (auto f : { home.getChildFile ("Dropbox"), home.getChildFile ("Library").getChildFile ("CloudStorage").getChildFile ("Dropbox") })
            if (f.isDirectory())
                return f;

        return {};
    }

    bool isInCloudFolder (const juce::File& f)
    {
        // Mac: ~/Dropbox, iCloud Drive, ~/Library/CloudStorage/...; Windows: Dropbox, OneDrive,
        // and Google Drive for desktop's virtual drive ("G:\My Drive", "G:\Shared drives")
        auto path = f.getFullPathName().replaceCharacter ('\\', '/');
        return path.contains ("/Dropbox") || path.contains ("/Library/Mobile Documents/")
                || path.contains ("/Library/CloudStorage/") || path.contains ("/Google Drive")
                || path.contains ("/My Drive/") || path.contains ("/Shared drives/") || path.contains ("/OneDrive");
    }

    //==============================================================================
    struct ShareDialog : public juce::Component
    {
        ShareDialog (MainComponent& m, Project& p, Collab::Session& s, AppActions& a) : main (m), project (p), session (s), actions (a)
        {
            nameEditor.setText (session.getIdentity().name, false);
            nameEditor.setTooltip ("The name your collaborators see");
            addAndMakeVisible (nameEditor);
            addAndMakeVisible (nameLabel);
            nameLabel.attachToComponent (&nameEditor, true);

            enableButton.onClick = [this]
            {
                session.enable();
                main.getProject().nameImportsByContent = true;
                refresh();
            };
            addAndMakeVisible (enableButton);

            revealButton.onClick = [this] { project.getEditFile().revealToUser(); };
            addAndMakeVisible (revealButton);

            moveButton.onClick = [this] { moveToCloud(); };
            addAndMakeVisible (moveButton);

            setSize (560, 360);
            refresh();
        }

        ~ShareDialog() override
        {
            auto name = nameEditor.getText().trim();

            if (name.isNotEmpty() && name != session.getIdentity().name)
                main.getViewState().edit.engine.getPropertyStorage().getPropertiesFile().setValue ("collabName", name);
        }

        void refresh()
        {
            auto shared = session.isActive();
            auto inCloud = isInCloudFolder (project.getFolder());

            enableButton.setVisible (! shared);
            moveButton.setVisible (! inCloud && findCloudFolder().isDirectory());

            juce::String text;
            text << "Project folder:\n" << project.getFolder().getFullPathName() << "\n\n";

            if (inCloud)
                text << juce::String::fromUTF8 ("\xe2\x9c\x93 ") << "This folder is in a cloud-synced location.\n\n";
            else if (findCloudFolder().isDirectory())
                text << "This folder isn't in Dropbox yet. Move it there so your collaborator can get it.\n\n";
            else
                text << "Put this folder in Dropbox, iCloud Drive or Google Drive so it syncs.\n\n";

            if (shared)
                text << "Sharing is ON. Share the project folder with your collaborator (in Dropbox: right-click it, "
                        "then Share). When they open the .elsewhere file in Elsewhere, you'll see each other's changes "
                        "within a few seconds, and their name will appear in the top bar.";
            else
                text << "Turn on sharing so both of you can work on this project at the same time. Every change "
                        "is saved as a small update, so the cloud service never has to merge two versions of the file.";

            info.setText (text, juce::dontSendNotification);
            resized();
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::panel); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16);
            auto nameRow = r.removeFromTop (26);
            nameEditor.setBounds (nameRow.withTrimmedLeft (90).withWidth (220));
            r.removeFromTop (12);
            auto buttons = r.removeFromBottom (30);
            revealButton.setBounds (buttons.removeFromLeft (140));
            buttons.removeFromLeft (8);

            if (moveButton.isVisible())
            {
                moveButton.setBounds (buttons.removeFromLeft (170));
                buttons.removeFromLeft (8);
            }

            if (enableButton.isVisible())
                enableButton.setBounds (buttons.removeFromRight (150));

            info.setBounds (r);
        }

        void moveToCloud()
        {
            auto cloud = Legacy::preferExisting (findCloudFolder().getChildFile ("Elsewhere Projects"),
                                                 findCloudFolder().getChildFile ("Tandem Projects"));
            cloud.createDirectory();
            session.enable();

            ProjectAudio::collectExternal (project);

            if (auto copied = project.saveCopyAs (cloud, project.getName()))
            {
                auto target = *copied;
                auto* a = &actions;

                if (auto dw = findParentComponentOfClass<juce::DialogWindow>())
                    dw->exitModalState (0);

                juce::MessageManager::callAsync ([a, target] { a->openProject (target); });
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Moved to Dropbox",
                    "The project was copied to " + target.getParentDirectory().getFullPathName()
                      + " and opened from there. The original folder is untouched; you can delete it once you're happy.");
            }
        }

        MainComponent& main;
        Project& project;
        Collab::Session& session;
        AppActions& actions;

        juce::Label nameLabel { {}, "Your name" }, info;
        juce::TextEditor nameEditor;
        juce::TextButton enableButton { "Turn On Sharing" }, revealButton { "Show in " + fileManagerName() }, moveButton { "Move to Dropbox" };
    };

    //==============================================================================
    struct HistoryDialog : public juce::Component, private juce::ListBoxModel
    {
        HistoryDialog (Project& p, AppActions& a) : project (p), actions (a)
        {
            for (auto& c : History::read (project.getEditFile()))
                if (c.kind != History::Kind::damaged)
                    checkpoints.insert (0, c);     // newest first

            list.setModel (this);
            list.setRowHeight (26);
            addAndMakeVisible (list);

            empty.setText ("Every time you save, the version you saved is kept here.", juce::dontSendNotification);
            empty.setJustificationType (juce::Justification::centred);
            empty.setColour (juce::Label::textColourId, Colours::textDim);
            addChildComponent (empty);
            empty.setVisible (checkpoints.isEmpty());

            // A shared project is put back together from everyone's changes, so it isn't wound back here
            shared = Project::isSharedProjectFile (project.getEditFile());
            openButton.onClick = [this] { openSelected(); };
            backButton.onClick = [this] { goBackToSelected(); };
            addAndMakeVisible (openButton);

            if (! shared)
                addAndMakeVisible (backButton);

            if (! checkpoints.isEmpty())
                list.selectRow (0);

            setSize (560, 420);
        }

        void paint (juce::Graphics& g) override { g.fillAll (Colours::panel); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            auto bottom = r.removeFromBottom (30);
            openButton.setBounds (bottom.removeFromRight (150));
            bottom.removeFromRight (8);
            backButton.setBounds (bottom.removeFromRight (190));
            r.removeFromBottom (8);
            list.setBounds (r);
            empty.setBounds (r);
        }

        int getNumRows() override { return checkpoints.size(); }

        static juce::String describeTime (juce::Time t)
        {
            auto now = juce::Time::getCurrentTime();
            auto sameDay = [] (juce::Time a, juce::Time b) { return a.getYear() == b.getYear() && a.getDayOfYear() == b.getDayOfYear(); };
            auto clock = t.formatted ("%H:%M");

            if (sameDay (t, now))
                return "Today " + clock;

            if (sameDay (t, now - juce::RelativeTime::days (1)))
                return "Yesterday " + clock;

            return t.formatted (t.getYear() == now.getYear() ? "%e %b" : "%e %b %Y").trim() + ", " + clock;
        }

        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (selected)
                g.fillAll (Colours::accent.withAlpha (0.3f));

            auto& c = checkpoints.getReference (row);
            auto what = describeTime (c.time);

            if (row == 0 && c.kind == History::Kind::saved)
                what << "  (as it is now)";

            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (what, 8, 0, w / 2, h, juce::Justification::centredLeft);

            juce::String note = c.label.isNotEmpty() ? c.label
                              : c.kind == History::Kind::unsaved ? juce::String ("unsaved changes, set aside")
                              : c.savedBy.isNotEmpty() ? "saved by " + c.savedBy : juce::String();
            g.setColour (Colours::textDim);
            g.drawText (note, w / 2, 0, w / 2 - 8, h, juce::Justification::centredRight);
        }

        void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { openSelected(); }

        const History::Checkpoint* getSelected() const
        {
            auto row = list.getSelectedRow();
            return juce::isPositiveAndBelow (row, checkpoints.size()) ? &checkpoints.getReference (row) : nullptr;
        }

        std::unique_ptr<juce::XmlElement> loadSelected()
        {
            auto* c = getSelected();

            if (c == nullptr)
                return {};

            auto content = History::load (*c);
            auto xml = content.isEmpty() ? nullptr : juce::parseXML (content.toString());

            if (xml == nullptr || ! xml->hasTagName ("EDIT"))
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "This version can't be read",
                                                        "Its copy in the project's history is damaged. Try the one before it.");
                return {};
            }

            return xml;
        }

        void close()
        {
            if (auto dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (0);
        }

        /** Opens the old version as a separate copy, so nothing current is overwritten. */
        void openSelected()
        {
            auto xml = loadSelected();

            if (xml == nullptr)
                return;

            auto stamp = getSelected()->time.formatted ("%Y-%m-%d %H-%M");
            auto newEdit = Project::createProjectFolder (project.getFolder().getParentDirectory(), project.getName() + " (" + stamp + ")");
            auto newFolder = newEdit.getParentDirectory();

            for (auto sub : { "Samples/Imported", "Samples/Recorded", "Samples/Bounced", "Samples/Somewhere" })
                project.getFolder().getChildFile (sub).copyDirectoryTo (newFolder.getChildFile (sub));

            // The copy is a separate, unshared project
            for (auto name : { "elsewhereCollab", "elsewhereCollabClock", "tandemCollab", "tandemCollabClock" })
                xml->removeAttribute (name);

            xml->writeTo (newEdit);
            close();
            juce::MessageManager::callAsync ([a = &actions, newEdit] { a->openProject (newEdit); });
        }

        /** Makes the project what it was then. What it is now stays in the history, so this can be undone the same way. */
        void goBackToSelected()
        {
            auto xml = loadSelected();

            if (xml == nullptr)
                return;

            auto when = describeTime (getSelected()->time);
            juce::Component::SafePointer<HistoryDialog> safe (this);
            auto options = juce::MessageBoxOptions()
                             .withIconType (juce::MessageBoxIconType::QuestionIcon)
                             .withTitle ("Go back to " + when + "?")
                             .withMessage ("The project becomes what it was then. What it is now stays in Version History, so you can come back to it.")
                             .withButton ("Go Back")
                             .withButton ("Cancel");

            juce::AlertWindow::showAsync (options, [safe, x = std::shared_ptr<juce::XmlElement> (std::move (xml))] (int result)
            {
                if (result != 1 || safe == nullptr)
                    return;

                auto& p = safe->project;

                // Saved first, so what's there now is in the history
                if (! p.save())
                    return;

                auto file = p.getEditFile();
                juce::TemporaryFile temp (file);

                if (! x->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't go back", "The project file couldn't be written.");
                    return;
                }

                auto* acts = &safe->actions;
                safe->close();
                juce::MessageManager::callAsync ([acts, file] { acts->openProject (file); });
            });
        }

        Project& project;
        AppActions& actions;
        juce::Array<History::Checkpoint> checkpoints;
        bool shared = false;
        juce::ListBox list;
        juce::Label empty;
        juce::TextButton openButton { "Open as a Copy" }, backButton { "Go Back to This Version" };
    };

    void launchDialog (const juce::String& title, juce::Component* content)
    {
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = title;
        o.dialogBackgroundColour = Colours::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.content.setOwned (content);
        o.launchAsync();
    }
}

void MainComponent::toggleLive()
{
    viewState->detailMode = EditViewState::DetailMode::live;
    viewState->sendChangeMessage();

    if (live->isActive())
        live->stop();
    else if (collab->isActive())
        live->start();
    else
        showShareDialog();

    transportBar->setLiveActive (live->isActive());
}

void MainComponent::showShareDialog()
{
    launchDialog ("Share Project", new ShareDialog (*this, *project, *collab, appActions));
}

void MainComponent::showVersionHistory()
{
    project->save();
    launchDialog ("Version History", new HistoryDialog (*project, appActions));
}

//==============================================================================
ReferenceDevice* MainComponent::findReference() const
{
    for (auto* p : project->getEdit().getMasterPluginList().getPlugins())
        if (auto* ref = dynamic_cast<ReferenceDevice*> (p))
            return ref;

    return nullptr;
}

void MainComponent::chooseReference()
{
    referenceChooser = std::make_unique<juce::FileChooser> ("Choose a song to compare with",
                                                            findReference() != nullptr ? findReference()->getFile() : juce::File(),
                                                            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    referenceChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                   [this] (const juce::FileChooser& fc)
    {
        auto file = fc.getResult();

        if (file == juce::File())
            return;

        auto& edit = project->getEdit();
        edit.getUndoManager().beginNewTransaction ("Compare with Reference");
        auto* ref = findReference();

        // At the end of the master track, after its devices, so the reference skips them (the
        // meter stays last, so it shows what you hear)
        if (ref == nullptr)
        {
            auto& list = edit.getMasterPluginList();
            int index = list.size();

            for (int i = 0; i < list.size(); ++i)
                if (dynamic_cast<te::LevelMeterPlugin*> (list[i]) != nullptr)
                {
                    index = i;
                    break;
                }

            auto p = DeviceRegistry::create (edit, ReferenceDevice::xmlTypeName);
            list.insertPlugin (p, index, nullptr);
            ref = dynamic_cast<ReferenceDevice*> (p.get());
        }

        if (ref != nullptr)
        {
            ref->setFile (file);
            ref->setListeningToReference (true);
        }
    });
}

void MainComponent::showNameDialog()
{
    auto* aw = Theme::newAlertWindow ("Your Name", "Shown to collaborators and on your comments.");
    Theme::addTextField (*aw, "name", collab->getIdentity().name);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int r)
    {
        auto name = aw->getTextEditorContents ("name").trim();

        if (r == 1 && name.isNotEmpty())
        {
            engine.getPropertyStorage().getPropertiesFile().setValue ("collabName", name);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Your Name",
                                                    "Saved. It's used from the next time you open a project.");
        }
    }), true);
}

//==============================================================================
void MainComponent::deleteSelected()
{
    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction ("Delete");

    auto clips = selectionManager.getItemsOfType<te::Clip>();

    if (! clips.isEmpty())
    {
        selectionManager.deselectAll();
        EditModes::deleteClips (clips, viewState->editMode == EditViewState::EditMode::shuffle);
        return;
    }

    if (auto t = selectionManager.getFirstItemOfType<te::Track>())
    {
        if (! t->isMasterTrack())
        {
            selectionManager.deselectAll();
            viewState->focusTrack (nullptr);
            edit.deleteTrack (t);
        }
    }
}

void MainComponent::duplicateSelected()
{
    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction ("Duplicate");

    auto clips = selectionManager.getItemsOfType<te::Clip>();

    if (! clips.isEmpty())
    {
        te::SelectableList newSelection;

        for (auto c : clips)
        {
            if (c->getClipSlot() != nullptr)
                continue;

            if (auto dup = te::duplicateClip (*c))
            {
                dup->setStart (c->getPosition().getEnd(), false, true);
                newSelection.add (dup.get());
            }
        }

        if (! newSelection.isEmpty())
            selectionManager.select (newSelection);

        return;
    }

    if (auto t = dynamic_cast<te::AudioTrack*> (selectionManager.getFirstItemOfType<te::Track>()))
    {
        auto state = t->state.createCopy();
        te::EditItemID::remapIDs (state, nullptr, edit);
        edit.insertTrack (state, t->state.getParent(), t->state, nullptr);
    }
}

void MainComponent::splitAtPlayhead()
{
    auto& edit = project->getEdit();
    edit.getUndoManager().beginNewTransaction ("Split");
    auto pos = edit.getTransport().getPosition();

    for (auto c : selectionManager.getItemsOfType<te::Clip>())
        if (c->getEditTimeRange().containsInclusive (pos))
            if (auto ct = dynamic_cast<te::ClipTrack*> (c->getTrack()))
                ct->splitClip (*c, pos);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void MainComponent::resized()
{
    focusRing.setBounds (getLocalBounds());
    auto r = getLocalBounds();
    transportBar->setBounds (r.removeFromTop (52));

    if (layout.trackList)
        trackList->setBounds (r.removeFromLeft (TrackList::width));

    if (layout.browser)
        browser->setBounds (r.removeFromLeft (220));

    if (layout.clipList)
        clipList->setBounds (r.removeFromRight (ClipList::width));

    if (layout.detail)
    {
        detail->setBounds (r.removeFromBottom (juce::jmin (layout.detailHeight, juce::jmax (80, r.getHeight() - 200))));
        detailResizer.setBounds (r.removeFromBottom (4));
    }

    arrangement->setBounds (r);
    session->setBounds (r);
    mixer->setBounds (r);

    if (mastering != nullptr)
        mastering->setBounds (r);
}

void MainComponent::updateViewVisibility()
{
    using MV = ViewLayout::MainView;
    session->setVisible (isViewShowing (MV::session));
    arrangement->setVisible (isViewShowing (MV::arrangement));
    mixer->setVisible (isViewShowing (MV::mixer));

    if (mastering != nullptr)
    {
        session->setVisible (false);
        arrangement->setVisible (false);
        mastering->setVisible (! isViewShowing (MV::mixer));
    }

    browser->setVisible (layout.browser);
    trackList->setVisible (layout.trackList);
    clipList->setVisible (layout.clipList);
    arrangement->setHeaderWidth (layout.trackIO ? ArrangementView::wideHeaderWidth : ArrangementView::normalHeaderWidth);
    session->setShowScenes (layout.scenes);
    transportBar->setHasSession (isViewShowing (MV::session));
    detail->setVisible (layout.detail);
    detailResizer.setVisible (layout.detail);
    transportBar->setMixerShowing (isViewShowing (MV::mixer));
    resized();

    if (onMenuStateChanged)
        onMenuStateChanged();
}

void MainComponent::setEditMode (EditViewState::EditMode m)
{
    viewState->editMode = m;
    props().setValue ("editMode", (int) m);
    viewState->sendChangeMessage();

    if (onMenuStateChanged)
        onMenuStateChanged();
}

void MainComponent::checkProjectAudio()
{
    // An album's audio is its renders, which it makes again itself when they're missing
    if (mastering != nullptr)
        return;

    auto copied = ProjectAudio::collectExternal (*project);

    // Moved within the project (e.g. a folder renamed)? Look there first
    if (! ProjectAudio::findMissing (*project).isEmpty())
        ProjectAudio::relinkFrom (*project, project->getFolder());

    juce::String intro;

    if (copied > 0)
        intro << "Copied " << copied << (copied == 1 ? " audio file" : " audio files")
              << " this project uses from other folders into its Samples folder, so it keeps working if the project is moved.\n\n";

    if (ProjectAudio::findMissing (*project).isEmpty())
    {
        if (copied > 0)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Project Audio", intro.trim());

        return;
    }

    showMissingAudio (intro);
}

void MainComponent::showMissingAudio (const juce::String& intro)
{
    auto missing = ProjectAudio::findMissing (*project);

    juce::String list;
    for (int i = 0; i < juce::jmin (8, missing.size()); ++i)
        list << "    " << missing[i] << "\n";
    if (missing.size() > 8)
        list << "    ...and " << (missing.size() - 8) << " more\n";

    auto message = intro + juce::String (missing.size() == 1 ? "This audio file can't be found" : "These audio files can't be found")
                     + ", so " + (missing.size() == 1 ? "its clips are" : "their clips are") + " silent:\n\n" + list
                     + "\nChoose a folder to look in (subfolders too). Files found outside the project are copied into it.";

    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Missing Audio", message, "Locate...", "Not Now", this,
        juce::ModalCallbackFunction::create ([sp = SafePointer<MainComponent> (this), count = missing.size()] (int result)
        {
            if (sp == nullptr || result != 1)
                return;

            sp->chooser = std::make_unique<juce::FileChooser> ("Find the missing audio in...", sp->project->getFolder().getParentDirectory());
            sp->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                      [sp, count] (const juce::FileChooser& fc)
            {
                if (sp == nullptr || fc.getResult() == juce::File())
                    return;

                auto& edit = sp->project->getEdit();
                edit.getUndoManager().beginNewTransaction ("Relink audio");
                auto found = ProjectAudio::relinkFrom (*sp->project, fc.getResult());
                auto stillMissing = ProjectAudio::findMissing (*sp->project).size();

                if (stillMissing == 0)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Missing Audio",
                                                            "Found " + juce::String (found) + (found == 1 ? " file" : " files") + ". Everything plays again.");
                else
                    sp->showMissingAudio ("Found " + juce::String (found) + " of " + juce::String (count) + ".\n\n");
            });
        }));
}

void MainComponent::moveToTransient (bool forwards, int attempt)
{
    // The first time a file is asked about, its transients take a moment to find
    if (! Transients::moveToNext (*viewState, forwards) && attempt < 50)
        juce::Timer::callAfterDelay (100, [sp = SafePointer<MainComponent> (this), forwards, attempt]
        {
            if (sp != nullptr)
                sp->moveToTransient (forwards, attempt + 1);
        });
}

void MainComponent::setMainView (ViewLayout::MainView v)
{
    if (v != ViewLayout::MainView::mixer)
        layout.lastNonMixerView = v;

    layout.view = v;
    saveLayout();
    updateViewVisibility();
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == viewState.get() || source == &selectionManager)
    {
        routeMidiToFocusedTrack();

        if (viewState->detailMode == EditViewState::DetailMode::clip && viewState->getSelectedClip() != nullptr)
            detail->focusPianoRoll();
    }

    if (source == &project->getEdit().getTransport())
    {
        auto& transport = project->getEdit().getTransport();

        // However recording started or stopped (a control surface, the space bar, the end of
        // a punch), nothing is cut out of a take while it runs
        if (wasRecording != transport.isRecording())
            EngineHelpers::keepInputWhenOverloaded (engine, transport.isRecording());

        // Save straight after a recording, so a crash can't lose a take
        if (wasRecording && ! transport.isRecording())
            project->save();

        wasRecording = transport.isRecording();
    }

    if (onTitleChanged)
        onTitleChanged();
}

void MainComponent::timerCallback()
{
    // Heartbeat for the collaboration lock
    if (++autosaveCounter % 30 == 0)
        project->refreshLock();

    transportBar->setLiveActive (live->isActive());

    if (collab->isActive())
    {
        // Publish what we're doing, show what the others are doing
        collab->focusedTrack = viewState->focusedTrack != nullptr ? viewState->focusedTrack->itemID : te::EditItemID();

        std::vector<EditViewState::Collaborator> people;
        std::vector<std::pair<juce::String, juce::Colour>> chips;

        for (auto& p : collab->getOthersOnline())
        {
            people.push_back ({ p.name, p.colour, p.focusedTrack });
            chips.push_back ({ p.name, p.colour });
        }

        auto changed = people.size() != viewState->collaborators.size();
        for (size_t i = 0; ! changed && i < people.size(); ++i)
            changed = people[i].focusedTrack != viewState->collaborators[i].focusedTrack || people[i].name != viewState->collaborators[i].name;

        viewState->collaborators = people;
        transportBar->setCollaborators (chips);

        if (changed)
            viewState->sendChangeMessage();

        transportBar->setStatusText ("Shared");

        // Shared projects save themselves regularly (the op logs already hold every change)
        if (autosaveCounter % 60 == 0 && project->hasChangedSinceSaved())
            project->save();
    }
    else
    {
        // Keep unsaved changes somewhere a crash can't take them (not mid-take: recording
        // saves the project itself when it stops)
        if (autosaveCounter % 20 == 0 && ! project->getEdit().getTransport().isRecording())
            project->updateRecovery();

        // The project's name is the window's title, so the status only says what needs saying
        auto foreign = project->getForeignLock();
        auto album = Mastering::getReturn (props(), project->getEditFile());
        transportBar->setStatusText (foreign ? "Also open by " + foreign->user + " (" + foreign->machine + ")"
                                     : juce::String());
        transportBar->setBackToAlbum (album.existsAsFile() ? album.getFileNameWithoutExtension() : juce::String());
    }

    auto phones = somewhereLink->getConnectedNames();

    for (auto& p : phones)
        if (! lastPhones.contains (p))
            A11y::announce ("Somewhere connected: " + p);

    for (auto& p : lastPhones)
        if (! phones.contains (p))
            A11y::announce ("Somewhere disconnected: " + p);

    lastPhones = phones;

    if (onTitleChanged)
        onTitleChanged();
}

//==============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& commands)
{
    commands.addArray ({ cmdNewProject, cmdOpenProject, cmdSave, cmdSaveAs, cmdExport, cmdUndo, cmdRedo,
                         cmdPlayStop, cmdRecord, cmdToggleLoop, cmdNewAudioTrack, cmdNewMidiTrack, cmdNewDrumTrack, cmdDelete,
                         cmdDuplicate, cmdSplit, cmdToggleView, cmdToggleDetail, cmdToggleBrowser, cmdZoomIn,
                         cmdZoomOut, cmdZoomToFit, cmdAudioSettings, cmdPluginManager, cmdControlSurfaces, cmdToggleMetronome, cmdFollow,
                         cmdImportAudio, cmdNewReturnTrack, cmdGroupTracks, cmdShowAutomation, cmdAutomationArm,
                         cmdMidiLearn, cmdShareProject, cmdVersionHistory, cmdSetName, cmdGoLive,
                         cmdImportSession, cmdExportDawProject, cmdToggleMixer, cmdShowDetail,
                         cmdShowArrangement, cmdShowSession, cmdShowScenes, cmdShowTrackIO, cmdShowTrackList,
                         cmdShowClipList, cmdShowMarkers, cmdAddMarker, cmdPrevMarker, cmdNextMarker,
                         cmdPrevTransient, cmdNextTransient,
                         cmdEditShuffle, cmdEditSlip, cmdEditSpot, cmdEditGrid,
                         cmdSelectPrevTrack, cmdSelectNextTrack, cmdMuteTrack, cmdSoloTrack, cmdArmTrack, cmdNextArea, cmdPrevArea,
                         cmdListenReference, cmdChooseReference, cmdCaptureMidi, cmdExportSeveral,
                         cmdNewMasteringProject, cmdAddSong, cmdBackToAlbum });

    for (int i = cmdGoToMarker1; i <= cmdGoToMarkerLast; ++i)
        commands.add (i);
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using KP = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;
    auto& edit = project->getEdit();

    if (id >= cmdGoToMarker1 && id <= cmdGoToMarkerLast)
    {
        auto n = (int) (id - cmdGoToMarker1) + 1;
        auto m = edit.getMarkerManager().getMarkerByID (n);
        info.setInfo ("Marker " + juce::String (n) + (m != nullptr ? ": " + m->getName() : juce::String()), "Go to marker " + juce::String (n), "Markers", 0);
        info.setActive (m != nullptr);
        info.addDefaultKeypress ('0' + n, juce::ModifierKeys::altModifier);
        return;
    }

    switch (id)
    {
        case cmdNewProject:     info.setInfo ("New Project", "Create a new project", "File", 0); info.addDefaultKeypress ('n', cmd); break;
        case cmdOpenProject:    info.setInfo ("Open Project...", "Open a project", "File", 0); info.addDefaultKeypress ('o', cmd); break;
        case cmdSave:           info.setInfo ("Save", "Save the project", "File", 0); info.addDefaultKeypress ('s', cmd); break;
        case cmdSaveAs:         info.setInfo ("Save As...", "Save a copy of the project", "File", 0); info.addDefaultKeypress ('s', cmd | shift); break;
        case cmdExport:         info.setInfo ("Export Audio...", "Render the arrangement to a WAV file", "File", 0); info.addDefaultKeypress ('r', cmd | shift); break;
        case cmdExportSeveral:  info.setInfo ("Export Several Projects...", "Export each of several projects with the same settings", "File", 0); break;
        case cmdNewMasteringProject:
            info.setInfo ("New Mastering Project...", "Create a project for mastering the songs of an EP or album together", "File", 0);
            break;
        case cmdAddSong:
            info.setInfo ("Add Song...", "Add songs (Elsewhere projects) to the album", "File", 0);
            info.setActive (mastering != nullptr);
            break;
        case cmdBackToAlbum:
        {
            auto album = Mastering::getReturn (props(), project->getEditFile());
            info.setInfo (album.existsAsFile() ? "Back to " + album.getFileNameWithoutExtension() : juce::String ("Back to Album"),
                          "Go back to the mastering project this song was opened from", "File", 0);
            info.setActive (album.existsAsFile() && mastering == nullptr);
            info.addDefaultKeypress (juce::KeyPress::upKey, cmd | shift);
            break;
        }
        case cmdUndo:
            info.setInfo ("Undo " + edit.getUndoManager().getUndoDescription(), "Undo", "Edit", 0);
            info.setActive (edit.getUndoManager().canUndo());
            info.addDefaultKeypress ('z', cmd);
            break;
        case cmdRedo:
            info.setInfo ("Redo " + edit.getUndoManager().getRedoDescription(), "Redo", "Edit", 0);
            info.setActive (edit.getUndoManager().canRedo());
            info.addDefaultKeypress ('z', cmd | shift);
            break;
        case cmdDelete:         info.setInfo ("Delete", "Delete selection", "Edit", 0); info.addDefaultKeypress (KP::deleteKey, 0); info.addDefaultKeypress (KP::backspaceKey, 0); break;
        case cmdDuplicate:      info.setInfo ("Duplicate", "Duplicate selection", "Edit", 0); info.addDefaultKeypress ('d', cmd); break;
        case cmdSplit:          info.setInfo ("Split at Playhead", "Split selected clips", "Edit", 0); info.addDefaultKeypress ('e', cmd); break;
        case cmdNewAudioTrack:  info.setInfo ("Insert Audio Track", "Insert an audio track", "Create", 0); info.addDefaultKeypress ('t', cmd); break;
        case cmdNewMidiTrack:   info.setInfo ("Insert MIDI Track", "Insert a MIDI/instrument track", "Create", 0); info.addDefaultKeypress ('t', cmd | shift); break;
        case cmdNewDrumTrack:   info.setInfo ("Insert Drum Track", "Insert a drum track: a kit plus sections you drag out on the timeline", "Create", 0); info.addDefaultKeypress ('d', cmd | shift); break;
        case cmdPlayStop:       info.setInfo ("Play / Stop", "Start or stop playback", "Transport", 0); info.addDefaultKeypress (KP::spaceKey, 0); break;
        case cmdRecord:         info.setInfo ("Record", "Start recording", "Transport", 0); info.addDefaultKeypress (KP::F9Key, 0); break;
        case cmdToggleLoop:
            info.setInfo ("Loop", "Toggle looping", "Transport", 0);
            info.setTicked (edit.getTransport().looping.get());
            info.addDefaultKeypress ('l', cmd);
            break;
        case cmdToggleMetronome:
            info.setInfo ("Metronome", "Toggle the metronome", "Transport", 0);
            info.setTicked (edit.clickTrackEnabled.get());
            info.addDefaultKeypress ('m', cmd | shift);
            break;
        case cmdToggleView:
            info.setInfo (isViewShowing (ViewLayout::MainView::session) ? "Show Arrangement" : "Show Session",
                          "Switch between the Arrangement and the Session grid", "View", 0);
            info.addDefaultKeypress (KP::tabKey, 0);
            break;
        case cmdToggleMixer:
            info.setInfo ("Mixer", "Show the mixer (again to go back)", "View", 0);
            info.setTicked (isViewShowing (ViewLayout::MainView::mixer));
            info.addDefaultKeypress ('=', cmd);
            break;
        case cmdShowDetail:
            info.setInfo ("Show Device Panel", "Show or hide the clip and device panel", "View", 0);
            info.setTicked (layout.detail);
            info.addDefaultKeypress ('l', cmd | juce::ModifierKeys::altModifier);
            break;
        case cmdShowTrackList:
            info.setInfo ("Show Track List", "Show or hide the list of tracks", "View", 0);
            info.setTicked (layout.trackList);
            info.addDefaultKeypress ('k', cmd | juce::ModifierKeys::altModifier);
            break;
        case cmdEditShuffle:
        case cmdEditSlip:
        case cmdEditSpot:
        case cmdEditGrid:
        {
            // F1 to F4: Shuffle, Slip, Spot, Grid
            static const EditViewState::EditMode modes[] = { EditViewState::EditMode::shuffle, EditViewState::EditMode::slip,
                                                             EditViewState::EditMode::spot, EditViewState::EditMode::grid };
            auto index = (int) (id - cmdEditShuffle);
            auto mode = modes[index];
            info.setInfo (EditViewState::getEditModeName (mode) + " Mode", "Edit mode", "Edit", 0);
            info.setTicked (viewState->editMode == mode);
            info.addDefaultKeypress (KP::F1Key + index, 0);
            break;
        }
        case cmdShowArrangement:
            info.setInfo ("Arrangement", "Show the Arrangement", "View", 0);
            info.setTicked (isViewShowing (ViewLayout::MainView::arrangement));
            info.addDefaultKeypress ('1', cmd);
            break;
        case cmdShowSession:
            info.setInfo ("Session", "Show the Session grid", "View", 0);
            info.setTicked (isViewShowing (ViewLayout::MainView::session));
            info.addDefaultKeypress ('2', cmd);
            break;
        case cmdShowScenes:
            info.setInfo ("Show Scenes", "Show the scene launch column beside the Session grid", "View", 0);
            info.setTicked (layout.scenes);
            break;
        case cmdShowTrackIO:
            info.setInfo ("Show Track Inputs / Outputs", "Wider track headers with the input and output", "View", 0);
            info.setTicked (layout.trackIO);
            break;
        case cmdShowClipList:
            info.setInfo ("Show Clips & Markers", "The list of clips and markers, on the right", "View", 0);
            info.setTicked (layout.clipList);
            info.addDefaultKeypress ('c', cmd | juce::ModifierKeys::altModifier);
            break;
        case cmdShowMarkers:
            info.setInfo ("Markers...", "Show the list of markers", "Markers", 0);
            info.addDefaultKeypress ('5', cmd);
            break;
        case cmdAddMarker:
            info.setInfo ("Add Marker", "Add a marker at the playhead", "Markers", 0);
            info.addDefaultKeypress (KP::returnKey, 0);
            break;
        case cmdPrevMarker:
            info.setInfo ("Previous Marker", "Move the playhead to the previous marker", "Markers", 0);
            info.addDefaultKeypress (KP::leftKey, cmd);
            break;
        case cmdNextMarker:
            info.setInfo ("Next Marker", "Move the playhead to the next marker", "Markers", 0);
            info.addDefaultKeypress (KP::rightKey, cmd);
            break;
        case cmdPrevTransient:
            info.setInfo ("Previous Transient", "Move the playhead to the previous hit on the selected track", "Markers", 0);
            info.addDefaultKeypress (KP::leftKey, juce::ModifierKeys::altModifier);
            break;
        case cmdNextTransient:
            info.setInfo ("Next Transient", "Move the playhead to the next hit on the selected track", "Markers", 0);
            info.addDefaultKeypress (KP::rightKey, juce::ModifierKeys::altModifier);
            break;
        case cmdToggleDetail:
            info.setInfo ("Toggle Clip / Device View", "Switch the bottom panel", "View", 0);
            info.addDefaultKeypress (KP::tabKey, shift);
            break;
        case cmdToggleBrowser:
            info.setInfo ("Show Browser", "Show or hide the browser", "View", 0);
            info.setTicked (layout.browser);
            info.addDefaultKeypress ('b', cmd | juce::ModifierKeys::altModifier);
            break;
        case cmdFollow:
            info.setInfo ("Follow Playhead", "Scroll the arrangement with playback", "View", 0);
            info.setTicked (arrangement->followPlayback);
            info.addDefaultKeypress ('f', cmd | shift);
            break;
        case cmdZoomIn:         info.setInfo ("Zoom In", "Zoom in", "View", 0); info.addDefaultKeypress ('=', 0); info.addDefaultKeypress ('+', 0); break;
        case cmdZoomOut:        info.setInfo ("Zoom Out", "Zoom out", "View", 0); info.addDefaultKeypress ('-', 0); break;
        case cmdZoomToFit:      info.setInfo ("Zoom to Fit", "Show the whole song", "View", 0); info.addDefaultKeypress ('0', cmd); break;
        case cmdImportAudio:    info.setInfo ("Import Audio / Stems...", "Import audio or MIDI files; several files become one track each", "File", 0); info.addDefaultKeypress ('i', cmd); break;
        case cmdNewReturnTrack: info.setInfo ("Insert Return Track", "Add a reverb/delay return with sends on every track", "Create", 0); info.addDefaultKeypress ('t', cmd | juce::ModifierKeys::altModifier); break;
        case cmdGroupTracks:    info.setInfo ("Group Tracks", "Put the selected tracks in a group (submix)", "Create", 0); info.addDefaultKeypress ('g', cmd); break;
        case cmdShowAutomation: info.setInfo ("Show Automation", "Show or hide the selected track's automation lane", "View", 0); info.addDefaultKeypress ('a', 0); break;
        case cmdAutomationArm:
            info.setInfo ("Automation Arm", "Record knob and fader moves as automation while playing", "Transport", 0);
            info.setTicked (edit.getAutomationRecordManager().isWritingAutomation());
            break;
        case cmdImportSession: info.setInfo ("Import Session (Pro Tools, Ableton, Cubase, AAF, DAWproject)...", "Make an Elsewhere project from another DAW's session: a Pro Tools session, an Ableton Live Set, a Cubase project, AAF from Pro Tools or Logic, or DAWproject from Bitwig, Studio One or Cubase", "File", 0); break;
        case cmdExportDawProject: info.setInfo ("Export DAWproject...", "Save the arrangement as a .dawproject file for Bitwig, Studio One or Cubase", "File", 0); break;
        case cmdShareProject:   info.setInfo ("Share Project...", "Work on this project with someone else via Dropbox", "File", 0); break;
        case cmdVersionHistory: info.setInfo ("Version History...", "Go back to an earlier saved version, or open one as a copy", "File", 0); break;
        case cmdListenReference:
        {
            auto* ref = findReference();
            info.setInfo ("Listen to Reference", "Switch between your mix and the reference song", "Transport", 0);
            info.setActive (ref != nullptr);
            info.setTicked (ref != nullptr && ref->isListeningToReference());
            info.addDefaultKeypress ('r', 0);
            break;
        }
        case cmdCaptureMidi:
            info.setInfo ("Capture MIDI", "Turn what you just played into a clip, even if you weren't recording", "Transport", 0);
            info.addDefaultKeypress ('c', cmd | shift);
            break;
        case cmdChooseReference: info.setInfo ("Compare with Reference...", "Choose a finished song to compare your mix with, at matched loudness", "Transport", 0); break;
        case cmdGoLive:
            info.setInfo ("Go Live", "Play together over the internet", "Transport", 0);
            info.setTicked (live != nullptr && live->isActive());
            info.addDefaultKeypress ('j', cmd);
            break;
        case cmdSetName:        info.setInfo ("Your Name...", "The name collaborators see", "Options", 0); break;
        case cmdSelectPrevTrack:
            info.setInfo ("Select Previous Track", "Select the track above", "Track", 0);
            info.addDefaultKeypress (KP::upKey, 0);
            break;
        case cmdSelectNextTrack:
            info.setInfo ("Select Next Track", "Select the track below", "Track", 0);
            info.addDefaultKeypress (KP::downKey, 0);
            break;
        case cmdMuteTrack:
        case cmdSoloTrack:
        case cmdArmTrack:
        {
            auto* t = viewState->focusedTrack.get();
            auto* at = dynamic_cast<te::AudioTrack*> (t);
            const bool mixes = t != nullptr && ! t->isMasterTrack() && (at != nullptr || t->isFolderTrack());

            if (id == cmdMuteTrack)
            {
                info.setInfo ("Mute Track", "Mute or unmute the selected track", "Track", 0);
                info.setActive (mixes);
                info.setTicked (mixes && t->isMuted (false));
                info.addDefaultKeypress ('m', juce::ModifierKeys::altModifier);
            }
            else if (id == cmdSoloTrack)
            {
                info.setInfo ("Solo Track", "Solo the selected track, or stop soloing it", "Track", 0);
                info.setActive (mixes);
                info.setTicked (mixes && t->isSolo (false));
                info.addDefaultKeypress ('s', juce::ModifierKeys::altModifier);
            }
            else
            {
                const bool armable = at != nullptr && ! Mixing::isReturnTrack (*at);
                info.setInfo ("Arm Track for Recording", "Arm or disarm the selected track", "Track", 0);
                info.setActive (armable);
                info.setTicked (armable && EngineHelpers::isTrackArmed (*at));
                info.addDefaultKeypress ('r', juce::ModifierKeys::altModifier);
            }
            break;
        }
        case cmdNextArea:
            info.setInfo ("Move to Next Area", "Move keyboard focus to the controls in the next part of the window", "View", 0);
            info.addDefaultKeypress (KP::F6Key, 0);
            break;
        case cmdPrevArea:
            info.setInfo ("Move to Previous Area", "Move keyboard focus to the controls in the previous part of the window", "View", 0);
            info.addDefaultKeypress (KP::F6Key, shift);
            break;
        case cmdMidiLearn:
            info.setInfo ("MIDI Map Mode", "Map knobs on your MIDI controller: click a control, then move a knob", "Options", 0);
            info.setTicked (engine.getMidiLearnState().isActive());
            info.addDefaultKeypress ('m', cmd);
            break;
        case cmdAudioSettings:  info.setInfo ("Audio / MIDI Settings...", "Choose your audio interface", "Options", 0); info.addDefaultKeypress (',', cmd); break;
        case cmdPluginManager:  info.setInfo ("Plug-in Manager...", "Scan for VST3 / AU plugins", "Options", 0); break;
        case cmdControlSurfaces: info.setInfo ("Control Surfaces...", "Mackie Control faders and transport", "Options", 0); break;
        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    auto& edit = project->getEdit();

    if (info.commandID >= cmdGoToMarker1 && info.commandID <= cmdGoToMarkerLast)
        return Markers::goTo (edit, (int) (info.commandID - cmdGoToMarker1) + 1);

    switch (info.commandID)
    {
        case cmdNewProject:     appActions.newProject(); break;
        case cmdOpenProject:    appActions.openProject(); break;
        case cmdSave:           appActions.saveProject(); break;
        case cmdSaveAs:         appActions.saveProjectAs(); break;
        case cmdExport:         appActions.exportAudio(); break;
        case cmdExportSeveral:  appActions.exportSeveralProjects(); break;
        case cmdNewMasteringProject:    appActions.newMasteringProject(); break;
        case cmdAddSong:                if (mastering != nullptr) mastering->chooseSongs(); break;
        case cmdBackToAlbum:
            if (auto album = Mastering::getReturn (props(), project->getEditFile()); album.existsAsFile())
                juce::MessageManager::callAsync ([a = &appActions, album] { a->openProject (album); });
            break;
        case cmdUndo:           edit.getUndoManager().undo(); break;
        case cmdRedo:           edit.getUndoManager().redo(); break;
        case cmdDelete:         deleteSelected(); break;
        case cmdDuplicate:      duplicateSelected(); break;
        case cmdSplit:          splitAtPlayhead(); break;
        case cmdNewAudioTrack:  addTrack (false); break;
        case cmdNewMidiTrack:   addTrack (true); break;
        case cmdNewDrumTrack:   addDrumTrack(); break;
        case cmdPlayStop:       EngineHelpers::togglePlay (edit); break;
        case cmdRecord:         EngineHelpers::toggleRecord (edit); break;
        case cmdToggleLoop:     EngineHelpers::setLooping (edit, ! edit.getTransport().looping.get()); break;
        case cmdToggleMetronome: edit.clickTrackEnabled = ! edit.clickTrackEnabled.get(); break;
        case cmdToggleView:     setMainView (isViewShowing (ViewLayout::MainView::session) ? ViewLayout::MainView::arrangement : ViewLayout::MainView::session); break;
        case cmdToggleMixer:    setMainView (isViewShowing (ViewLayout::MainView::mixer) ? layout.lastNonMixerView : ViewLayout::MainView::mixer); break;
        case cmdShowDetail:     togglePanel (layout.detail); break;
        case cmdShowTrackList:  togglePanel (layout.trackList); break;
        case cmdShowScenes:     togglePanel (layout.scenes); break;
        case cmdShowTrackIO:    togglePanel (layout.trackIO); break;
        case cmdShowClipList:   togglePanel (layout.clipList); break;
        case cmdShowMarkers:
            if (layout.clipList && clipList->getTab() == ClipList::Tab::markers)
                togglePanel (layout.clipList);
            else
            {
                clipList->setTab (ClipList::Tab::markers);
                layout.clipList = true;
                saveLayout();
                updateViewVisibility();
            }
            break;
        case cmdAddMarker:      Markers::add (edit, edit.getTransport().getPosition()); break;
        case cmdPrevMarker:     Markers::goToNext (edit, false); break;
        case cmdNextMarker:     Markers::goToNext (edit, true); break;
        case cmdPrevTransient:  moveToTransient (false); break;
        case cmdNextTransient:  moveToTransient (true); break;
        case cmdShowArrangement: setMainView (ViewLayout::MainView::arrangement); break;
        case cmdShowSession:    setMainView (ViewLayout::MainView::session); break;
        case cmdEditShuffle:    setEditMode (EditViewState::EditMode::shuffle); break;
        case cmdEditSlip:       setEditMode (EditViewState::EditMode::slip); break;
        case cmdEditSpot:       setEditMode (EditViewState::EditMode::spot); break;
        case cmdEditGrid:       setEditMode (EditViewState::EditMode::grid); break;
        case cmdToggleDetail:
            viewState->detailMode = viewState->detailMode == EditViewState::DetailMode::clip ? EditViewState::DetailMode::devices
                                                                                             : EditViewState::DetailMode::clip;
            viewState->sendChangeMessage();
            break;
        case cmdToggleBrowser:  togglePanel (layout.browser); break;
        case cmdFollow:         arrangement->followPlayback = ! arrangement->followPlayback; break;
        case cmdZoomIn:         viewState->setZoom (viewState->pixelsPerBeat * 1.5, 0.0f); break;
        case cmdZoomOut:        viewState->setZoom (viewState->pixelsPerBeat / 1.5, 0.0f); break;
        case cmdZoomToFit:      arrangement->zoomToFit(); break;
        case cmdAudioSettings:  appActions.showAudioSettings(); break;
        case cmdImportAudio:    showImportDialog(); break;
        case cmdImportSession: appActions.importSession(); break;
        case cmdExportDawProject: appActions.exportDawProject(); break;
        case cmdShareProject:   showShareDialog(); break;
        case cmdVersionHistory: showVersionHistory(); break;
        case cmdListenReference:
            if (auto* ref = findReference())
            {
                ref->setListeningToReference (! ref->isListeningToReference());
                A11y::announce (ref->isListeningToReference() ? "Reference" : "Mix");
            }
            break;
        case cmdChooseReference: chooseReference(); break;
        case cmdCaptureMidi:
        {
            auto& edit = project->getEdit();
            edit.getUndoManager().beginNewTransaction ("Capture MIDI");
            auto clips = MidiCapture::capture (edit);

            if (clips.isEmpty())
            {
                A11y::announce ("Nothing played to capture");
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Capture MIDI",
                                                        "Nothing was played in the last 30 seconds. Play something on a MIDI keyboard "
                                                        "with an instrument track selected, then capture it.");
                break;
            }

            selectionManager.deselectAll();
            for (auto* c : clips)
                selectionManager.addToSelection (c);

            A11y::announce ("Captured " + juce::String (clips.size()) + (clips.size() == 1 ? " clip" : " clips"));
            break;
        }
        case cmdSetName:        showNameDialog(); break;
        case cmdSelectPrevTrack: selectTrackFromKeyboard (-1); break;
        case cmdSelectNextTrack: selectTrackFromKeyboard (1); break;
        case cmdNextArea:       A11y::moveToNextRegion (getFocusRegions(), true); break;
        case cmdPrevArea:       A11y::moveToNextRegion (getFocusRegions(), false); break;
        case cmdMuteTrack:
        case cmdSoloTrack:
        case cmdArmTrack:
        {
            auto* t = viewState->focusedTrack.get();

            if (t == nullptr || t->isMasterTrack())
                break;

            const auto name = TrackSpeech::name (*t);

            if (info.commandID == cmdMuteTrack)
            {
                t->setMute (! t->isMuted (false));
                A11y::announce (name + (t->isMuted (false) ? " muted" : " unmuted"));
            }
            else if (info.commandID == cmdSoloTrack)
            {
                Mixing::clickSolo (*t);
                A11y::announce (name + (t->isSolo (false) ? " soloed" : " not soloed"));
            }
            else if (auto* at = dynamic_cast<te::AudioTrack*> (t); at != nullptr && ! Mixing::isReturnTrack (*at))
            {
                if (! EngineHelpers::trackHasInput (*at))
                {
                    A11y::announce (name + " has no input. Choose one with its In button first", juce::AccessibilityHandler::AnnouncementPriority::high);
                    break;
                }

                EngineHelpers::armTrack (*at, ! EngineHelpers::isTrackArmed (*at));
                A11y::announce (name + (EngineHelpers::isTrackArmed (*at) ? " armed" : " disarmed"));
            }

            if (onMenuStateChanged)
                onMenuStateChanged();
            break;
        }
        case cmdGoLive:         toggleLive(); break;
        case cmdMidiLearn:      engine.getMidiLearnState().setActive (! engine.getMidiLearnState().isActive()); break;
        case cmdNewReturnTrack:
            edit.getUndoManager().beginNewTransaction ("Insert Return Track");
            if (auto rt = Mixing::createReturnTrack (edit))
                viewState->focusTrack (rt);
            break;
        case cmdGroupTracks:
        {
            auto tracks = selectionManager.getItemsOfType<te::Track>();
            if (tracks.isEmpty())
                if (auto t = viewState->focusedTrack.get())
                    tracks.add (t);

            edit.getUndoManager().beginNewTransaction ("Group tracks");
            if (auto g = Mixing::groupTracks (edit, tracks))
            {
                viewState->focusTrack (g);
                selectionManager.selectOnly (g);
            }
            break;
        }
        case cmdShowAutomation:
            if (auto t = viewState->focusedTrack.get())
            {
                auto& lanes = viewState->automationLanes;
                if (lanes.count (t->itemID)) lanes.erase (t->itemID);
                else if (auto p = Automation::getDefaultParameter (*t)) lanes[t->itemID] = p;
                viewState->sendChangeMessage();
            }
            break;
        case cmdAutomationArm:
        {
            auto& arm = edit.getAutomationRecordManager();
            arm.setWritingAutomation (! arm.isWritingAutomation());
            break;
        }
        case cmdPluginManager:  appActions.showPluginManager(); break;
        case cmdControlSurfaces: appActions.showControlSurfaces(); break;
        default: return false;
    }

    // Menu ticks follow (later: a command can replace this component, e.g. New Project)
    juce::MessageManager::callAsync ([sp = SafePointer<MainComponent> (this)]
    {
        if (sp != nullptr && sp->onMenuStateChanged)
            sp->onMenuStateChanged();
    });

    return true;
}

//==============================================================================
std::vector<juce::Component*> MainComponent::getFocusRegions() const
{
    return { transportBar.get(), browser.get(), trackList.get(), arrangement.get(), session.get(), mixer.get(),
             clipList.get(), detail.get() };
}

juce::Array<te::Track*> MainComponent::getShownTracks() const
{
    juce::Array<te::Track*> shown;

    for (auto t : te::getAllTracks (project->getEdit()))
        if ((t->isAudioTrack() || t->isFolderTrack()) && ! viewState->isHiddenByCollapsedGroup (*t) && ! EditViewState::isTrackHidden (*t))
            shown.add (t);

    if (auto master = project->getEdit().getMasterTrack())
        shown.add (master);

    return shown;
}

/** Up and Down: select the track above or below the focused one, and say which it is. */
void MainComponent::selectTrackFromKeyboard (int delta)
{
    auto shown = getShownTracks();

    if (shown.isEmpty())
        return;

    const int current = shown.indexOf (viewState->focusedTrack.get());
    const int next = current < 0 ? (delta > 0 ? 0 : shown.size() - 1) : juce::jlimit (0, shown.size() - 1, current + delta);
    auto* t = shown[next];

    selectionManager.selectOnly (t);
    viewState->focusTrack (t);
    A11y::announce (TrackSpeech::describe (*t));

    if (onMenuStateChanged)
        onMenuStateChanged();
}

bool MainComponent::exportMastering()
{
    if (mastering == nullptr)
        return false;

    mastering->showExport();
    return true;
}

} // namespace elsewhere
