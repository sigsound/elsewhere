// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Project.h"
#include "TransportBar.h"
#include "Browser.h"
#include "SessionView.h"
#include "MixerView.h"
#include "ViewLayout.h"
#include "TrackList.h"
#include "ClipList.h"
#include "Transients.h"
#include "ProjectAudio.h"
#include "DetailView.h"
#include "Collab.h"
#include "SomewhereLink.h"
#include "Comments.h"
#include "LivePanel.h"
#include "SessionRecorder.h"
#include "Takes.h"
#include "ControlSurfaces.h"
#include "drums/DrumTrack.h"

namespace elsewhere
{
//==============================================================================
enum CommandIDs
{
    cmdNewProject = 0x2000,
    cmdOpenProject,
    cmdSave,
    cmdSaveAs,
    cmdExport,
    cmdUndo,
    cmdRedo,
    cmdPlayStop,
    cmdRecord,
    cmdToggleLoop,
    cmdNewAudioTrack,
    cmdNewMidiTrack,
    cmdDelete,
    cmdDuplicate,
    cmdSplit,
    cmdToggleView,
    cmdToggleDetail,
    cmdToggleBrowser,
    cmdZoomIn,
    cmdZoomOut,
    cmdZoomToFit,
    cmdAudioSettings,
    cmdPluginManager,
    cmdControlSurfaces,
    cmdToggleMetronome,
    cmdFollow,
    cmdImportAudio,
    cmdNewReturnTrack,
    cmdGroupTracks,
    cmdShowAutomation,
    cmdAutomationArm,
    cmdMidiLearn,
    cmdShareProject,
    cmdVersionHistory,
    cmdSetName,
    cmdGoLive,
    cmdNewDrumTrack,
    cmdImportSession,
    cmdExportDawProject,
    cmdToggleMixer,
    cmdShowDetail,
    cmdShowArrangement,
    cmdShowSession,
    cmdShowScenes,
    cmdShowTrackList,
    cmdEditShuffle,
    cmdEditSlip,
    cmdEditSpot,
    cmdEditGrid,
    cmdShowTrackIO,
    cmdShowClipList,
    cmdShowMarkers,
    cmdAddMarker,
    cmdPrevMarker,
    cmdNextMarker,
    cmdPrevTransient,
    cmdNextTransient,
    cmdGoToMarker1,     // ...to cmdGoToMarker1 + 8
    cmdGoToMarkerLast = cmdGoToMarker1 + 8,
    cmdSelectPrevTrack,
    cmdSelectNextTrack,
    cmdMuteTrack,
    cmdSoloTrack,
    cmdArmTrack,
    cmdNextArea,
    cmdPrevArea
};

//==============================================================================
class MainComponent : public juce::Component,
                      public juce::ApplicationCommandTarget,
                      public juce::DragAndDropContainer,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent (te::Engine& e, const juce::File& editFile, AppActions& actions);
    ~MainComponent() override;

    Project& getProject()           { return *project; }
    EditViewState& getViewState()   { return *viewState; }

    void paint (juce::Graphics&) override;
    void resized() override;

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    /** Adds a file to a track at a time, copying it into the project first. */
    void importFile (const juce::File&, te::Track&, te::TimePosition);
    void importFileToSlot (const juce::File&, te::ClipSlot&);

    /** Imports several files (e.g. stems) as one new track each, all starting at the same time.
        Audio is copied into the project on a background thread. */
    void importAsNewTracks (juce::Array<juce::File>, te::TimePosition start);
    void showImportDialog();

    /** Switches to the arrangement, zoomed to show everything (after importing a project). */
    void showWholeArrangement();

    Collab::Session* getCollab()    { return collab.get(); }
    Live::Session* getLive()        { return live.get(); }
    void toggleLive();

    void showShareDialog();
    void showVersionHistory();
    void showNameDialog();

    /** Closes any open file chooser (used by the dev self-checks). */
    void dismissFileChooser()       { chooser.reset(); }

    te::AudioTrack* addTrack (bool withInstrument);

    /** A drum track: Elsewhere Drums plus an 8-bar section to start from. */
    te::AudioTrack* addDrumTrack();
    te::Track* getLastNonReturnTrack();

    /** For the README screenshots (--showcase): collaborators shown as if they were in the
        project, and a view picked directly. */
    void showPresence (std::vector<EditViewState::Collaborator> people, const juce::String& status)
    {
        std::vector<std::pair<juce::String, juce::Colour>> chips;
        for (auto& p : people)
            chips.push_back ({ p.name, p.colour });

        viewState->collaborators = std::move (people);
        transportBar->setCollaborators (chips);
        transportBar->setStatusText (status);
        viewState->sendChangeMessage();
    }

    void showView (ViewLayout::MainView v, bool withDetail)
    {
        if (layout.detail != withDetail)
            togglePanel (layout.detail);
        setMainView (v);
    }

    std::function<void()> onTitleChanged;

    /** The parts of the window with controls, in the order F6 moves through them. */
    std::vector<juce::Component*> getFocusRegions() const;

    /** Ticks and names in the menu bar may have changed. */
    std::function<void()> onMenuStateChanged;

private:
    te::Engine& engine;
    AppActions& appActions;
    std::unique_ptr<Project> project;
    te::SelectionManager selectionManager { engine };
    std::unique_ptr<EditViewState> viewState;

    std::unique_ptr<TransportBar> transportBar;
    std::unique_ptr<Browser> browser;
    std::unique_ptr<ArrangementView> arrangement;
    std::unique_ptr<SessionView> session;
    std::unique_ptr<MixerView> mixer;
    std::unique_ptr<TrackList> trackList;
    std::unique_ptr<ClipList> clipList;
    std::unique_ptr<DetailView> detail;
    std::unique_ptr<Collab::Session> collab;
    std::unique_ptr<Live::Session> live;
    std::unique_ptr<SomewhereLink> somewhereLink;   // Somewhere (iPhone) on the same Wi-Fi
    std::unique_ptr<SessionRecorder> sessionRecorder;
    std::unique_ptr<Takes::Recorder> takeRecorder;      // keeps each recording (and loop pass) as a take
    std::unique_ptr<DrumTrack::Watcher> drumWatcher;
    juce::TooltipWindow tooltips { this, 500 };
    A11y::FocusRing focusRing;   // shows which control has focus while navigating by keyboard

    void startCollaboration();

    // Which view and panels are showing, remembered across projects
    ViewLayout::Layout layout;
    int autosaveCounter = 0;
    bool wasRecording = false;

    struct DetailResizer : public juce::Component
    {
        std::function<void (int)> onDrag;
        std::function<void()> onStart;
        void paint (juce::Graphics& g) override { g.fillAll (Colours::outline); }
        void mouseDown (const juce::MouseEvent&) override { if (onStart) onStart(); }
        void mouseDrag (const juce::MouseEvent& e) override { if (onDrag) onDrag (e.getDistanceFromDragStartY()); }
    } detailResizer;
    int detailHeightAtDragStart = 0;

    void setupInputs();
    void routeMidiToFocusedTrack();
    void deleteSelected();
    void duplicateSelected();
    void splitAtPlayhead();
    void updateViewVisibility();

    juce::PropertiesFile& props()   { return engine.getPropertyStorage().getPropertiesFile(); }
    void setMainView (ViewLayout::MainView);
    void togglePanel (bool& shown)  { shown = ! shown; saveLayout(); updateViewVisibility(); }
    void saveLayout()               { ViewLayout::save (props(), layout); }
    void setEditMode (EditViewState::EditMode);

public:
    /** After opening: copies in audio that lives outside the project, and asks about any that's
        missing. */
    void checkProjectAudio();

private:
    void showMissingAudio (const juce::String& intro);
    void moveToTransient (bool forwards, int attempt = 0);

    /** The tracks in the order the arrangement shows them (groups' hidden tracks left out). */
    juce::Array<te::Track*> getShownTracks() const;
    void selectTrackFromKeyboard (int delta);
    juce::StringArray lastPhones;   // Somewhere phones connected last time, to announce changes
    bool isViewShowing (ViewLayout::MainView v) const  { return layout.view == v; }

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    te::SafeSelectable<te::Track> lastMidiTarget;
    std::unique_ptr<juce::FileChooser> chooser;
    void createTracksForImportedFiles (const juce::Array<juce::File>& originals, const juce::Array<juce::File>& local, te::TimePosition start);
};

} // namespace elsewhere
