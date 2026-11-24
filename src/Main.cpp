// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MainComponent.h"
#include "ProjectAudioTests.h"
#include "TakesTests.h"
#include "MidiLanesTests.h"
#include "RoutingTests.h"
#include "ControlSurfaceTests.h"
#include "Routing.h"
#include "devices/CompressorDevice.h"
#include "MidiLanes.h"
#include "ExportTests.h"
#include "Export.h"
#include "PluginWindow.h"
#include "Mixing.h"
#include "PluginScanner.h"
#include "Presets.h"
#include "Comments.h"
#include "devices/DeviceTests.h"
#include "devices/DrumKits.h"
#include "PluginTests.h"
#include "platform/MacDockIcon.h"
#include "AboutWindow.h"
#include "DawProject.h"
#include "DawProjectTests.h"
#include "AafTests.h"
#include "ImportInspector.h"
#include "AbletonTests.h"
#include "CubaseTests.h"
#include "PerfTest.h"
#include "RecordTests.h"
#include "Showcase.h"

namespace elsewhere
{
//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow() : DocumentWindow ("Elsewhere", Colours::background, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (900, 600, 10000, 10000);
    }

    std::function<void()> onClose;
    void closeButtonPressed() override { if (onClose) onClose(); }
};

//==============================================================================
class ElsewhereApplication : public juce::JUCEApplication,
                          public AppActions,
                          private juce::MenuBarModel,
                          private juce::ChangeListener,
                          private juce::DarkModeSettingListener
{
public:
    const juce::String getApplicationName() override       { return "Elsewhere"; }
    const juce::String getApplicationVersion() override    { return ELSEWHERE_VERSION; }
    bool moreThanOneInstanceAllowed() override
    {
        // Plug-in scan helpers run alongside the main app
        // (dev snapshot runs too, so they work while you have Elsewhere open)
        auto args = getCommandLineParameterArray();
        return args.contains (PluginScanner::scanArg) || args.contains ("--snapshot") || args.contains ("--collab-test")
                || args.contains ("--live-test") || args.contains ("--device-test") || args.contains ("--plugin-test")
                || args.contains ("--somewhere-test") || args.contains ("--render-kits")
                || args.contains ("--dawproject-test") || args.contains ("--aaf-test") || args.contains ("--import-test")
                || args.contains ("--ableton-test") || args.contains ("--cubase-test") || args.contains ("--perf-test") || args.contains ("--record-test")
                || args.contains ("--project-audio-test") || args.contains ("--takes-test") || args.contains ("--midi-lanes-test") || args.contains ("--routing-test") || args.contains ("--surfaces-test") || args.contains ("--export-test")
                || args.contains ("--accessibility-test");
    }

    void initialise (const juce::String& commandLine) override
    {
        if (getCommandLineParameterArray().contains (PluginScanner::scanArg))
        {
           #if JUCE_MAC
            juce::Process::setDockIconVisible (false);
           #endif
            setApplicationReturnValue (PluginScanner::runChildScan (getCommandLineParameterArray()));
            quit();
            return;
        }

        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        setDockIconFromBundle();

        // Automated test runs get their own settings (~/Library/Elsewhere Test), so they can never
        // touch your recent projects, audio setup or plug-in list
        isTestRun = commandLine.contains ("--snapshot") || commandLine.contains ("--showcase") || commandLine.contains ("--collab-test")
                     || commandLine.contains ("--live-test") || commandLine.contains ("--device-test") || commandLine.contains ("--plugin-test")
                     || commandLine.contains ("--somewhere-test") || commandLine.contains ("--render-kits")
                     || commandLine.contains ("--dawproject-test") || commandLine.contains ("--aaf-test")
                     || commandLine.contains ("--import-test") || commandLine.contains ("--ableton-test")
                     || commandLine.contains ("--cubase-test") || commandLine.contains ("--project-audio-test")
                     || commandLine.contains ("--takes-test") || commandLine.contains ("--midi-lanes-test") || commandLine.contains ("--routing-test") || commandLine.contains ("--surfaces-test") || commandLine.contains ("--export-test")
                     || commandLine.contains ("--accessibility-test");   // a normal window for Somewhere tests: use with --open <copy>

        // Dev aid: --perf-test <project.elsewhere> <outDir> [buffer] [seconds] times audio callbacks
        // on its own engine (no audio device) and quits
        if (commandLine.contains ("--perf-test"))
        {
            setApplicationReturnValue (PerfTest::run (getCommandLineParameterArray()));
            quit();
            return;
        }

        // Dev aid: --record-test <outDir> records a known signal through its own engine (no audio
        // device), checks what landed on disk and on the timeline, and quits
        if (commandLine.contains ("--record-test"))
        {
            setApplicationReturnValue (RecordTests::run (getCommandLineParameterArray()) == 0 ? 0 : 1);
            quit();
            return;
        }

        // The engine's audio worker threads join the interface's audio workgroup, so macOS
        // schedules them as part of the real-time audio work (on Apple Silicon, on performance
        // cores, by its deadline). Without it, a worker woken late made the whole buffer late:
        // pops across the mix in big projects
        te::EditPlaybackContext::enableAudioWorkgroup (true);

        // Settings and sound packs saved while the app was called Tandem move to the new folders
        if (! isTestRun)
            Legacy::moveAppFolders();

        engine = std::make_unique<te::Engine> (isTestRun ? "Elsewhere Test" : "Elsewhere", std::make_unique<ElsewhereUIBehaviour>(), std::make_unique<ElsewhereEngineBehaviour> (! commandLine.contains ("--no-audio")));
        Export::registerMp3Encoder (*engine);

        // Test runs start from the default layout, but on the Session grid with its scenes
        // (the snapshot steps were written for that)
        if (isTestRun)
        {
            auto& props = engine->getPropertyStorage().getPropertiesFile();

            auto keys = props.getAllProperties().getAllKeys();   // a copy: removing changes the original

            for (auto& k : keys)
                if (k.startsWith ("layout.") || k.startsWith ("workspace") || k == "editMode")
                    props.removeValue (k);

            props.setValue (ViewLayout::key ("view"), (int) ViewLayout::MainView::session);
            props.setValue (ViewLayout::key ("scenes"), true);
        }

        // Test runs keep the dark theme so their snapshots stay comparable
        if (! isTestRun)
        {
            A11y::refresh();
            applyAppearance (false);
            juce::Desktop::getInstance().addDarkModeSettingListener (this);
            systemSettingsWatcher.onChange = [this] { systemAccessibilityChanged(); };
            systemSettingsWatcher.startTimer (1000);
        }

        // Dev aid: --plugin-test <outDir> checks the installed test plug-ins and quits
        if (commandLine.contains ("--plugin-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (PluginTests::run (*engine, juce::File (args[args.indexOf ("--plugin-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        // Dev aid: --render-kits <outDir> writes the synthesised drum kits as FLAC files (for the
        // shared sound library, sounds/catalog.json) and quits
        if (commandLine.contains ("--render-kits"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (renderKits (juce::File (args[args.indexOf ("--render-kits") + 1])) ? 0 : 1);
            quit();
            return;
        }

        // Dev aid: --device-test <outDir> checks the DSP of Elsewhere's built-in devices and quits
        if (commandLine.contains ("--device-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (DeviceTests::run (*engine, juce::File (args[args.indexOf ("--device-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        // Dev aid: --dawproject-test <outDir> checks DAWproject import/export and quits
        // Dev aid: --import-test <session file> <outDir> imports a Pro Tools, AAF or DAWproject file
        // and lists what came across
        if (commandLine.contains ("--import-test"))
        {
            auto args = getCommandLineParameterArray();
            auto i = args.indexOf ("--import-test");
            setApplicationReturnValue (ImportInspector::run (*engine, juce::File (args[i + 1]), juce::File (args[i + 2])));
            quit();
            return;
        }

        if (commandLine.contains ("--cubase-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (CubaseTests::run (*engine, juce::File (args[args.indexOf ("--cubase-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--ableton-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (AbletonTests::run (*engine, juce::File (args[args.indexOf ("--ableton-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--export-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (ExportTests::run (*engine, juce::File (args[args.indexOf ("--export-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--surfaces-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (ControlSurfaceTests::run (*engine, juce::File (args[args.indexOf ("--surfaces-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--routing-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (RoutingTests::run (*engine, juce::File (args[args.indexOf ("--routing-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--midi-lanes-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (MidiLanesTests::run (*engine, juce::File (args[args.indexOf ("--midi-lanes-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--takes-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (TakesTests::run (*engine, juce::File (args[args.indexOf ("--takes-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--project-audio-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (ProjectAudioTests::run (*engine, juce::File (args[args.indexOf ("--project-audio-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--aaf-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (AafTests::run (*engine, juce::File (args[args.indexOf ("--aaf-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        if (commandLine.contains ("--dawproject-test"))
        {
            auto args = getCommandLineParameterArray();
            setApplicationReturnValue (DawProjectTests::run (*engine, juce::File (args[args.indexOf ("--dawproject-test") + 1])) == 0 ? 0 : 1);
            quit();
            return;
        }

        engine->getPluginManager().knownPluginList.setCustomScanner (std::make_unique<PluginScanner::OutOfProcessScanner>());

        // The engine's first MIDI device scan rebuilds all devices, which stops playback. Left
        // alone it happens ~4 s after launch (so pressing play straight away got cut off);
        // do it now, before any project is open.
        engine->getDeviceManager().rescanMidiDeviceList();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

        // Tracktion expects every channel of the interface to be open (tracks choose their own
        // inputs), so keep all channels enabled whenever the device changes
        engine->getDeviceManager().deviceManager.addChangeListener (this);
        enableAllChannels();

        window = std::make_unique<MainWindow>();
        window->onClose = [this] { systemRequestedQuit(); };
        window->addKeyListener (&escapeCancelsMapping);
        window->addKeyListener (commandManager.getKeyMappings());

        // Asked before the shortcuts: Tab moves between controls while navigating by keyboard
        keyboardNavigation.getHome = [this] () -> juce::Component* { return mainComponent; };
        keyboardNavigation.isEscapeTaken = [] { return ModulationMap::isActive(); };
        window->addKeyListener (&keyboardNavigation);

       #if JUCE_MAC
        juce::PopupMenu appMenu;
        appMenu.addItem (6200, "About Elsewhere");
        juce::MenuBarModel::setMacMainMenu (this, &appMenu);
       #else
        window->setMenuBar (this);
       #endif

        // Open the file we were launched with, or the most recent project, or a fresh one
        auto toOpen = juce::File::createFileWithoutCheckingPath (commandLine.fromLastOccurrenceOf ("--open ", false, false).upToFirstOccurrenceOf (" --", false, false).unquoted().trim());

        if (! toOpen.existsAsFile())
            toOpen = getRecentProject();

        if (commandLine.contains ("--showcase"))
            loadProject (Project::createProjectFolder (showcaseDir().getChildFile ("project"), Showcase::projectName));
        else if (toOpen.existsAsFile())
            loadProject (toOpen);
        else
            loadProject (Project::createProjectFolder (Project::getDefaultProjectsFolder(), "Untitled"));

        window->centreWithSize (1400, 860);
        window->setVisible (true);

        if (mainComponent != nullptr)
            mainComponent->grabKeyboardFocus();

        // Dev aid: --live-test <A|B> <outDir> runs a scripted two-person live session
        if (commandLine.contains ("--live-test"))
        {
            auto args = getCommandLineParameterArray();
            auto i = args.indexOf ("--live-test");
            runLiveTest (args[i + 1], juce::File (args[i + 2]));
            return;
        }

        // Dev aid: --collab-test <A|B> <outDir> runs a scripted two-person editing session
        if (commandLine.contains ("--collab-test"))
        {
            auto args = getCommandLineParameterArray();
            auto i = args.indexOf ("--collab-test");
            runCollabTest (args[i + 1], juce::File (args[i + 2]));
            return;
        }

        // --showcase <dir>: the README screenshots, from a demo project (src/Showcase.h)
        // Add --device-shots for a picture of each built-in device's panel instead (for design work)
        if (commandLine.contains ("--showcase"))
        {
            if (commandLine.contains ("--device-shots"))
                juce::Timer::callAfterDelay (2500, [this] { takeDeviceShots (showcaseDir()); });
            else
                juce::Timer::callAfterDelay (2500, [this] { takeShowcase (showcaseDir()); });
            return;
        }

        // Dev aid: --accessibility-test <file> writes what screen readers are given (the window's
        // accessibility tree) and checks the keyboard navigation, then quits
        if (commandLine.contains ("--accessibility-test"))
        {
            auto file = juce::File (commandLine.fromFirstOccurrenceOf ("--accessibility-test ", false, false).upToFirstOccurrenceOf (" --", false, false).unquoted().trim());
            // (Started outside the timer: the test runs message loops of its own, which would fire it again)
            juce::Timer::callAfterDelay (2500, [this, file] { juce::MessageManager::callAsync ([this, file] { runAccessibilityTest (file); }); });
            return;
        }

        // Dev aid: --snapshot <dir> renders both main views to PNGs and quits
        if (commandLine.contains ("--snapshot"))
        {
            auto dir = juce::File (commandLine.fromFirstOccurrenceOf ("--snapshot ", false, false).upToFirstOccurrenceOf (" ", false, false));
            juce::Timer::callAfterDelay (2500, [this, dir] { takeSnapshots (dir); });
            return;
        }

        // First launch: help the user pick their interface
        if (! isTestRun && ! engine->getPropertyStorage().getPropertiesFile().containsKey ("hasShownAudioSetup"))
        {
            engine->getPropertyStorage().getPropertiesFile().setValue ("hasShownAudioSetup", true);
            juce::MessageManager::callAsync ([this] { showAudioSettings(); });
        }
    }

    void shutdown() override
    {
        if (engine != nullptr)
            engine->getDeviceManager().deviceManager.removeChangeListener (this);

       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu (nullptr);
       #else
        if (window != nullptr)
            window->setMenuBar (nullptr);
       #endif
        systemSettingsWatcher.stopTimer();
        juce::Desktop::getInstance().removeDarkModeSettingListener (this);

        if (window != nullptr)
            window->clearContentComponent();

        mainComponent = nullptr;
        window = nullptr;
        engine = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        confirmDiscardChanges ([] { quit(); });
    }

    /** <outDir>/<kit id>_<pad 00-15>.flac for every synthesised kit (the centre variation). */
    static bool renderKits (const juce::File& outDir)
    {
        outDir.createDirectory();
        juce::FlacAudioFormat flac;
        auto& kits = DrumKits::kits();

        for (int k = 1; k < (int) kits.size(); ++k)
        {
            if (kits[(size_t) k].recorded)
                continue;

            for (int pad = 0; pad < 16; ++pad)
            {
                auto audio = DrumKits::render (k, pad, 1);
                auto file = outDir.getChildFile (juce::String (kits[(size_t) k].id) + "_" + juce::String (pad).paddedLeft ('0', 2) + ".flac");
                file.deleteFile();
                std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
                auto writer = flac.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (DrumKits::rate)
                                                             .withNumChannels (audio.getNumChannels()).withBitsPerSample (24));
                if (writer == nullptr)
                    return false;
                writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
            }
        }
        return true;
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        auto f = juce::File::createFileWithoutCheckingPath (commandLine.unquoted().trim());

        if (f.existsAsFile())
            openProject (f);
    }

    //==============================================================================
    // AppActions
    void newProject() override
    {
        // Start from the default template if there is one
        auto def = Library::getDefaultTemplate();
        newProjectFrom (def.existsAsFile() ? def : juce::File());
    }

    void newProjectFrom (juce::File templateFile)
    {
        confirmDiscardChanges ([this, templateFile]
        {
            chooser = std::make_unique<juce::FileChooser> ("Name your new project", Project::getDefaultProjectsFolder().getChildFile ("New Song"), "");
            Project::getDefaultProjectsFolder().createDirectory();

            chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, templateFile] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();

                if (f == juce::File())
                    return;

                auto editFile = Project::createProjectFolder (f.getParentDirectory(), f.getFileNameWithoutExtension());

                if (templateFile.existsAsFile())
                    templateFile.copyFileTo (editFile);

                loadProject (editFile);
                saveProject();
            });
        });
    }

    void openProject() override
    {
        confirmDiscardChanges ([this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Open an Elsewhere project", Project::getDefaultProjectsFolder(), Legacy::projectWildcard (Project::fileExtension) + ";" + SessionImport::wildcard);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();

                if (SessionImport::canImport (f))
                    convertSession (f);
                else if (f.existsAsFile())
                    openProjectChecked (f);
            });
        });
    }

    void openProject (const juce::File& f) override
    {
        confirmDiscardChanges ([this, f] { openProjectChecked (f); });
    }

    void saveProject() override     { saveProjectThen ({}); }

    /** Saves, then says whether the project is on disk. Quitting and opening another project
        wait for this, so "Save" can't go on to close a project that wasn't saved: the answer
        may come later (the "open elsewhere" question) or be no (a full disk, a read-only folder). */
    void saveProjectThen (std::function<void (bool)> done)
    {
        if (mainComponent == nullptr)
        {
            if (done)
                done (false);

            return;
        }

        auto& p = mainComponent->getProject();

        if (auto lock = p.getForeignLock(); lock && ! Collab::isEnabled (p.getEdit()))
        {
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Project is open elsewhere",
                lock->user + " has this project open on " + lock->machine + ". If you both save, Dropbox will create a "
                "conflicted copy. Save anyway?", "Save", "Cancel", nullptr,
                juce::ModalCallbackFunction::create ([this, done] (int r)
                {
                    auto saved = r == 1 && saveNow();

                    if (done)
                        done (saved);
                }));
            return;
        }

        auto saved = saveNow();

        if (done)
            done (saved);
    }

    /** Writes the project, and says so if it couldn't. */
    bool saveNow()
    {
        if (mainComponent == nullptr)
            return false;

        auto& p = mainComponent->getProject();

        if (p.save (false))
        {
            addRecentProject (p.getEditFile());
            return true;
        }

        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't save",
            "\"" + p.getName() + "\" couldn't be saved to " + p.getFolder().getFullPathName() + ". Check that the disk isn't full "
            "and that you can write to that folder. The project is still open with your changes.");
        return false;
    }

    void saveProjectAs() override
    {
        if (mainComponent == nullptr)
            return;

        chooser = std::make_unique<juce::FileChooser> ("Save a copy of this project as...", Project::getDefaultProjectsFolder().getChildFile (mainComponent->getProject().getName() + " copy"), "");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();

            if (f == juce::File())
                return;

            auto& project = mainComponent->getProject();
            ProjectAudio::collectExternal (project);

            if (auto newFile = project.saveCopyAs (f.getParentDirectory(), f.getFileNameWithoutExtension()))
                loadProject (*newFile);
        });
    }

    void exportAudio() override
    {
        if (mainComponent == nullptr)
            return;

        auto& project = mainComponent->getProject();
        Export::showDialog (project.getEdit(), project.getFolder(), project.getName());
    }

    //==============================================================================
    // Sessions from other DAWs: AAF (Pro Tools, Logic...) and DAWproject (Bitwig, Studio One, Cubase)
    void importSession() override
    {
        confirmDiscardChanges ([this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Import a session from another DAW (Pro Tools, Ableton, Cubase, AAF or DAWproject)",
                                                           juce::File::getSpecialLocation (juce::File::userHomeDirectory), SessionImport::wildcard);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
            {
                if (fc.getResult().existsAsFile())
                    convertSession (fc.getResult());
            });
        });
    }

    /** Makes a new Elsewhere project from another DAW's session: asks where to put it, copies the
        audio in, then builds the arrangement. */
    void convertSession (const juce::File& source)
    {
        std::shared_ptr<SessionImport::Importer> importer = SessionImport::open (source);

        if (importer->getError().isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Can't import " + source.getFileName(), importer->getError());
            return;
        }

        Project::getDefaultProjectsFolder().createDirectory();
        chooser = std::make_unique<juce::FileChooser> ("Name the new Elsewhere project",
                                                       Project::getDefaultProjectsFolder().getChildFile (source.getFileNameWithoutExtension()), "");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, importer, source] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();

            if (f == juce::File())
                return;

            auto editFile = Project::createProjectFolder (f.getParentDirectory(), f.getFileNameWithoutExtension());
            auto folder = editFile.getParentDirectory();

            struct CopyJob : public juce::ThreadWithProgressWindow
            {
                CopyJob (std::shared_ptr<SessionImport::Importer> i, juce::File d, std::function<void (bool)> done)
                    : juce::ThreadWithProgressWindow ("Copying the audio into the project...", true, true),
                      importer (std::move (i)), dest (std::move (d)), onDone (std::move (done)) {}

                void run() override
                {
                    ok = importer->extractAudio (dest, [this] (double p) { setProgress (p); return ! threadShouldExit(); });
                }

                void threadComplete (bool cancelled) override
                {
                    onDone (ok && ! cancelled);
                    delete this;
                }

                std::shared_ptr<SessionImport::Importer> importer;
                juce::File dest;
                std::function<void (bool)> onDone;
                bool ok = false;
            };

            auto* job = new CopyJob (importer, folder.getChildFile ("Samples/Imported"), [this, importer, editFile, folder] (bool ok)
            {
                if (! ok)
                {
                    folder.deleteRecursively();
                    return;
                }

                loadProject (editFile);

                if (mainComponent == nullptr)
                    return;

                auto& project = mainComponent->getProject();
                auto& edit = project.getEdit();
                auto summary = importer->build (edit);
                summary.headroomDb = SessionImport::addHeadroom (edit);
                edit.getUndoManager().clearUndoHistory();
                saveProject();
                mainComponent->showWholeArrangement();

                auto message = summary.describe();
                auto deviceRate = engine->getDeviceManager().getSampleRate();

                if (summary.firstSampleRate > 0.0 && deviceRate > 0.0 && ! juce::approximatelyEqual (summary.firstSampleRate, deviceRate))
                    message << "\n\nThe audio is " << juce::String (summary.firstSampleRate / 1000.0, 1) << " kHz but your interface is running at "
                            << juce::String (deviceRate / 1000.0, 1) << " kHz, so it's resampled on playback. Set the interface to "
                            << juce::String (summary.firstSampleRate / 1000.0, 1) << " kHz in Audio Settings for the best quality.";

                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Session imported", message);
            });

            job->launchThread();
        });
    }

    void exportDawProject() override
    {
        if (mainComponent == nullptr)
            return;

        auto& project = mainComponent->getProject();
        auto defaultFile = project.getFolder().getChildFile ("Exports").getChildFile (project.getName() + DawProject::fileExtension);
        defaultFile.getParentDirectory().createDirectory();

        chooser = std::make_unique<juce::FileChooser> ("Export DAWproject", defaultFile, "*.dawproject");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();

            if (f == juce::File() || mainComponent == nullptr)
                return;

            f = f.withFileExtension (DawProject::fileExtension);
            auto& p = mainComponent->getProject();
            p.getEdit().getTransport().stop (false, true);

            struct WriteJob : public juce::ThreadWithProgressWindow
            {
                WriteJob (DawProject::ExportPlan p, juce::File d)
                    : juce::ThreadWithProgressWindow ("Exporting DAWproject...", true, false), plan (std::move (p)), dest (std::move (d)) {}

                void run() override     { setProgress (-1.0); result = DawProject::writeExport (plan, dest); }

                void threadComplete (bool) override
                {
                    if (result.wasOk())
                    {
                        dest.revealToUser();
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "DAWproject exported",
                            "Saved " + dest.getFileName() + ". Open it in Bitwig, Studio One or Cubase.\n\n" + plan.warnings.joinIntoString ("\n"));
                    }
                    else
                    {
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
                    }

                    delete this;
                }

                DawProject::ExportPlan plan;
                juce::File dest;
                juce::Result result = juce::Result::ok();
            };

            (new WriteJob (DawProject::planExport (p.getEdit(), p.getName()), f))->launchThread();
        });
    }

    void saveAsTemplate (bool asDefault)
    {
        if (mainComponent == nullptr)
            return;

        auto& project = mainComponent->getProject();
        project.save();

        if (asDefault)
        {
            if (Library::saveTemplate (project.getEditFile(), Library::getDefaultTemplate()))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Default Template",
                    "New projects will now start with these tracks, devices and settings.");
            return;
        }

        auto* aw = Theme::newAlertWindow ("Save as Template", "Tracks, devices, sends and routing are saved. Audio clips aren't.");
        Theme::addTextField (*aw, "name", project.getName());
        aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw] (int r)
        {
            auto name = juce::File::createLegalFileName (aw->getTextEditorContents ("name").trim());

            if (r == 1 && name.isNotEmpty() && mainComponent != nullptr)
                Library::saveTemplate (mainComponent->getProject().getEditFile(),
                                       Library::getTemplatesFolder().getChildFile (name + ".elsewhere"));
        }), true);
    }

    void showAudioSettings() override
    {
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Audio / MIDI Settings";
        o.dialogBackgroundColour = Colours::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;

        // min == max hides the per-channel checkboxes: all channels are always open, and each
        // track picks its own input
        auto* selector = new juce::AudioDeviceSelectorComponent (engine->getDeviceManager().deviceManager,
                                                                 256, 256, 256, 256, true, true, false, false);
        selector->setSize (560, 620);
        o.content.setOwned (selector);
        o.launchAsync();
    }

    void showControlSurfaces() override
    {
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Control Surfaces";
        o.dialogBackgroundColour = Colours::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.content.setOwned (new ControlSurfaceSettings (*engine));
        o.launchAsync();
    }

    void showPluginManager() override
    {
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Plug-in Manager";
        o.dialogBackgroundColour = Colours::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;

        auto& pm = engine->getPluginManager();
        auto* list = new juce::PluginListComponent (pm.pluginFormatManager, pm.knownPluginList,
                                                    engine->getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
                                                    std::addressof (engine->getPropertyStorage().getPropertiesFile()), true);
        list->setSize (820, 560);
        o.content.setOwned (list);
        o.launchAsync();
    }

private:
    //==============================================================================
    // Appearance: Auto follows macOS light / dark mode
    juce::String getAppearance()
    {
        auto a = engine->getPropertyStorage().getPropertiesFile().getValue ("appearance", "auto");
        return a == "light" || a == "dark" ? a : juce::String ("auto");
    }

    void darkModeSettingChanged() override
    {
        if (getAppearance() == "auto")
            applyAppearance (true);
    }

    /** Increase Contrast: on when the system asks for it (Increase Contrast on macOS, a high
        contrast theme on Windows) or when it's switched on in Options > Appearance. */
    bool wantsMoreContrast()
    {
        return A11y::settings().moreContrast || engine->getPropertyStorage().getPropertiesFile().getBoolValue ("moreContrast", false);
    }

    static juce::String systemSettingsName()
    {
       #if JUCE_MAC
        return "System Settings";
       #else
        return "Windows Settings";
       #endif
    }

    /** Checks the system's accessibility settings once a second and follows any change. */
    struct SystemSettingsWatcher : public juce::Timer
    {
        std::function<void()> onChange;
        void timerCallback() override   { if (A11y::refresh() && onChange) onChange(); }
    } systemSettingsWatcher;

    void systemAccessibilityChanged()
    {
        applyAppearance (true);

        // Reduce Motion: views that flash read the setting as they paint
        auto& desktop = juce::Desktop::getInstance();
        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* c = desktop.getComponent (i))
                c->repaint();

        menuItemsChanged();
    }

    void applyAppearance (bool updateOpenWindows)
    {
        auto appearance = getAppearance();
        auto dark = appearance == "dark" || (appearance == "auto" && juce::Desktop::getInstance().isDarkModeActive());
        auto more = wantsMoreContrast();
        auto& palette = dark ? (more ? Themes::darkMoreContrast : Themes::dark)
                             : (more ? Themes::lightMoreContrast : Themes::light);

        setAppAppearance (appearance == "light" ? 1 : appearance == "dark" ? 2 : 0);
        switchTheme (palette, updateOpenWindows);
    }

    void switchTheme (const Palette& palette, bool updateOpenWindows)
    {
        if (Theme::current == &palette)
            return;

        // Colours already set on components are swapped for their equivalents in the new theme
        std::map<juce::uint32, juce::uint32> remap;
        auto colours = Theme::allColours();
        std::vector<juce::uint32> before;
        for (auto* c : colours)
            before.push_back (c->getARGB() & 0xffffff);

        Theme::apply (palette);
        lookAndFeel.refreshColours();

        for (size_t i = 0; i < colours.size(); ++i)
            remap.emplace (before[i], colours[i]->getARGB() & 0xffffff);

        if (! updateOpenWindows)
            return;

        auto& desktop = juce::Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            if (auto* c = desktop.getComponent (i))
            {
                remapColours (*c, remap);
                c->sendLookAndFeelChange();
                c->repaint();
            }
    }

    static void remapColours (juce::Component& c, const std::map<juce::uint32, juce::uint32>& remap)
    {
        static const juce::String prefix ("jcclr_");
        auto& props = c.getProperties();

        for (int i = props.size(); --i >= 0;)
        {
            auto name = props.getName (i).toString();

            if (! name.startsWith (prefix))
                continue;

            auto argb = (juce::uint32) (int) props.getValueAt (i);

            if (auto it = remap.find (argb & 0xffffff); it != remap.end())
                c.setColour (name.substring (prefix.length()).getHexValue32(), juce::Colour ((argb & 0xff000000) | it->second));
        }

        for (auto* child : c.getChildren())
            remapColours (*child, remap);
    }

    ElsewhereLookAndFeel lookAndFeel;
    std::unique_ptr<te::Engine> engine;
    /** Esc leaves a modulator's Map mode. */
    struct EscapeCancelsMapping : public juce::KeyListener
    {
        bool keyPressed (const juce::KeyPress& k, juce::Component*) override
        {
            if (k != juce::KeyPress::escapeKey || ! ModulationMap::isActive())
                return false;
            ModulationMap::cancel();
            return true;
        }
    } escapeCancelsMapping;

    A11y::KeyboardNavigation keyboardNavigation;

    std::unique_ptr<MainWindow> window;
    MainComponent* mainComponent = nullptr;   // owned by the window
    juce::ApplicationCommandManager commandManager;
    std::unique_ptr<juce::FileChooser> chooser;
    bool isTestRun = false;

    //==============================================================================
    void enableAllChannels()
    {
        auto& dm = engine->getDeviceManager().deviceManager;
        auto* device = dm.getCurrentAudioDevice();

        if (device == nullptr)
            return;

        auto numIns = device->getInputChannelNames().size();
        auto numOuts = device->getOutputChannelNames().size();
        auto setup = dm.getAudioDeviceSetup();

        if (setup.inputChannels.countNumberOfSetBits() == numIns && setup.outputChannels.countNumberOfSetBits() == numOuts)
            return;

        setup.useDefaultInputChannels = false;
        setup.useDefaultOutputChannels = false;
        setup.inputChannels.clear();
        setup.inputChannels.setRange (0, numIns, true);
        setup.outputChannels.clear();
        setup.outputChannels.setRange (0, numOuts, true);
        dm.setAudioDeviceSetup (setup, true);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (auto* d = engine->getDeviceManager().deviceManager.getCurrentAudioDevice())
            std::cout << "DEVICECHANGE t=" << juce::Time::getMillisecondCounter() << " ins=" << d->getActiveInputChannels().countNumberOfSetBits()
                      << "/" << d->getInputChannelNames().size() << " outs=" << d->getActiveOutputChannels().countNumberOfSetBits()
                      << "/" << d->getOutputChannelNames().size() << " sr=" << d->getCurrentSampleRate() << std::endl;

        // Defer: we're inside the device manager's own notification
        juce::MessageManager::callAsync ([this] { if (engine != nullptr) enableAllChannels(); });
    }

    /** Shuffle and Spot, on a scratch track: prints CHECK lines. */
    void runEditModeChecks()
    {
        auto& edit = mainComponent->getProject().getEdit();
        auto check = [] (const juce::String& what, bool ok) { std::cout << "CHECK " << what << ": " << (ok ? "OK" : "FAIL") << std::endl; };
        auto near = [] (te::TimePosition a, double b) { return std::abs (a.inSeconds() - b) < 1.0e-3; };

        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, te::getAllTracks (edit).getLast()), nullptr);
        track->setName ("Shuffle test");
        juce::Array<te::Clip*> clips;

        for (int i = 0; i < 3; ++i)
            clips.add (track->insertMIDIClip ("c" + juce::String (i), { te::TimePosition::fromSeconds (i * 2.0), te::TimePosition::fromSeconds (i * 2.0 + 2.0) }, nullptr).get());

        auto c0 = clips[0], c2 = clips[2];

        // Delete the middle one: the last closes up
        EditModes::deleteClips ({ clips[1] }, true);
        check ("shuffle delete closes the gap", near (c2->getPosition().getStart(), 2.0) && near (c0->getPosition().getStart(), 0.0));

        // Drag the first to just past the second's middle: it slots in after it
        auto from = c0->getPosition().time;
        c0->setStart (te::TimePosition::fromSeconds (3.4), false, true);
        EditModes::shuffleDrop (*c0, track.get(), from);
        check ("shuffle drop reorders", near (c2->getPosition().getStart(), 0.0) && near (c0->getPosition().getStart(), 2.0));

        // Delete without shuffle leaves the gap
        EditModes::deleteClips ({ c2 }, false);
        check ("slip delete leaves the gap", near (c0->getPosition().getStart(), 2.0));

        auto bar2 = edit.tempoSequence.toTime (te::tempo::BarsAndBeats { 1, {} });
        check ("spot parses bars", EditModes::parseSpotBars (edit, "2.1.1") == bar2 && EditModes::parseSpotBars (edit, "2") == bar2);
        check ("spot parses time", near (*EditModes::parseSpotTime ("1:02.5"), 62.5) && near (*EditModes::parseSpotTime ("7.25"), 7.25));
        check ("spot rejects junk", ! EditModes::parseSpotBars (edit, "abc") && ! EditModes::parseSpotTime ("x1"));
        check ("spot round trip", EditModes::parseSpotBars (edit, EditModes::formatSpotBars (edit, bar2)) == bar2);

        edit.deleteTrack (track.get());

        // Markers
        auto m1 = Markers::add (edit, te::TimePosition::fromSeconds (2.0), "Verse");
        auto m2 = Markers::add (edit, te::TimePosition::fromSeconds (5.0), "Chorus");
        check ("markers are numbered", m1 != nullptr && m2 != nullptr && m1->getMarkerID() == 1 && m2->getMarkerID() == 2);
        Markers::goTo (edit, 2);
        check ("go to marker 2", near (edit.getTransport().getPosition(), 5.0));
        Markers::goToNext (edit, false);
        check ("previous marker", near (edit.getTransport().getPosition(), 2.0));
        Markers::goToNext (edit, true);
        check ("next marker", near (edit.getTransport().getPosition(), 5.0));
        edit.getTransport().setPosition (te::TimePosition());

        // Hidden tracks leave the arrangement and mixer
        if (auto first = te::getAudioTracks (edit).getFirst())
            EditViewState::setTrackHidden (*first, true);
    }

    juce::File showcaseDir() const
    {
        auto cl = getCommandLineParameters();
        return juce::File (cl.fromFirstOccurrenceOf ("--showcase ", false, false).upToFirstOccurrenceOf (" --", false, false).unquoted().trim());
    }

    /** Builds the demo song and saves each main view, dark then light, at Retina resolution:
        the README's screenshots. Run with --no-audio. */
    void takeShowcase (juce::File dir)
    {
        dir.createDirectory();
        auto* mc = mainComponent;
        if (mc == nullptr)
            return quit();

        auto tracks = std::make_shared<Showcase::Tracks> (Showcase::build (*mc));
        auto& vs = mc->getViewState();

        struct Step { juce::String name; std::function<void()> setUp; };
        auto views = [mc, tracks, &vs]
        {
            using V = ViewLayout::MainView;
            using D = EditViewState::DetailMode;
            auto select = [&vs] (te::Selectable* s, te::Track* focus, D mode)
            {
                vs.selectionManager.deselectAll();
                if (s != nullptr)
                    vs.selectionManager.selectOnly (s);
                if (focus != nullptr)
                    vs.focusTrack (focus);
                vs.detailMode = mode;
                vs.sendChangeMessage();
            };

            return std::vector<Step> {
                { "arrangement", [=] { mc->showView (V::arrangement, true); mc->showWholeArrangement(); select (tracks->keysClip, tracks->keys, D::clip); } },
                { "devices",     [=] { mc->showView (V::arrangement, true); select (nullptr, tracks->keys, D::devices); } },
                { "comments",    [=] { mc->showView (V::arrangement, true); select (nullptr, tracks->vocal, D::comments); } },
                { "drums",       [=] { mc->showView (V::arrangement, true); select (tracks->chorusDrums, tracks->drums, D::clip); } },
                { "session",     [=] { mc->showView (V::session, false); select (nullptr, tracks->keys, D::clip); } },
                { "mixer",       [=] { mc->showView (V::mixer, false); select (nullptr, tracks->vocal, D::clip); } },
            };
        }();

        auto save = [this, dir] (const juce::String& name)
        {
            auto* content = window->getContentComponent();
            auto img = content->createComponentSnapshot (content->getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out (dir.getChildFile (name + ".png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
        };

        // Each step: set up, give the views a moment to lay out and draw, save. All of them in
        // the dark theme, then all again in light.
        auto steps = std::make_shared<std::vector<std::function<void()>>>();
        for (auto theme : { &Themes::dark, &Themes::light })
        {
            const auto suffix = theme == &Themes::light ? juce::String ("_light") : juce::String();
            steps->push_back ([this, theme] { switchTheme (*theme, true); });
            for (auto& v : views)
            {
                steps->push_back (v.setUp);
                steps->push_back ([mc] { Showcase::clearInputs (mc->getProject().getEdit()); mc->getViewState().sendChangeMessage(); });
                steps->push_back ([save, name = v.name + suffix] { save (name); });
            }
        }

        auto run = std::make_shared<std::function<void (size_t)>>();
        *run = [steps, run] (size_t i)
        {
            if (i >= steps->size())
                return quit();
            (*steps)[i]();
            juce::Timer::callAfterDelay (900, [run, i] { (*run) (i + 1); });
        };
        (*run) (0);
    }

    /** <dir>/device_<type>.png (and _light) for each built-in device as the chain shows it, at the
        default detail height, plus an LFO. On the demo song's keys track, one device at a time. */
    void takeDeviceShots (juce::File dir)
    {
        dir.createDirectory();
        auto* mc = mainComponent;
        if (mc == nullptr)
            return quit();

        auto tracks = Showcase::build (*mc);
        auto& track = *tracks.keys;
        constexpr int height = 200;

        auto save = [dir] (juce::Component& c, const juce::String& name)
        {
            auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out (dir.getChildFile (name + ".png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
        };

        for (auto theme : { &Themes::dark, &Themes::light })
        {
            switchTheme (*theme, true);
            const auto suffix = theme == &Themes::light ? juce::String ("_light") : juce::String();

            for (auto& d : DeviceRegistry::getAll())
            {
                if (d.hidden)
                    continue;

                if (auto p = DeviceRegistry::create (track.edit, d.type))
                {
                    track.pluginList.insertPlugin (p, 0, nullptr);
                    {
                        DevicePanel panel (*p);
                        panel.setSize (panel.getIdealWidth(), height);
                        save (panel, "device_" + d.type + suffix);
                    }
                    p->deleteFromParent();
                }
            }

            if (auto lfo = Modulators::addLFO (track))
            {
                {
                    LfoPanel panel (track, *lfo);
                    panel.setSize (LfoPanel::idealWidth, height);
                    save (panel, "device_lfo" + suffix);
                }
                lfo->remove();
            }
        }

        quit();
    }

    void runAccessibilityTest (juce::File file)
    {
        juce::String out;

        auto roleName = [] (juce::AccessibilityRole r) -> juce::String
        {
            using R = juce::AccessibilityRole;
            static const std::pair<R, const char*> names[] {
                { R::button, "button" }, { R::toggleButton, "toggle" }, { R::radioButton, "radio" }, { R::comboBox, "combo" },
                { R::image, "image" }, { R::slider, "slider" }, { R::label, "label" }, { R::staticText, "text" },
                { R::editableText, "editable" }, { R::group, "group" }, { R::window, "window" }, { R::dialogWindow, "dialog" },
                { R::scrollBar, "scrollbar" }, { R::list, "list" }, { R::listItem, "listItem" }, { R::tree, "tree" },
                { R::treeItem, "treeItem" }, { R::ignored, "ignored" }, { R::unspecified, "unspecified" } };

            for (auto& [role, name] : names)
                if (role == r)
                    return name;

            return "other";
        };

        std::function<void (juce::AccessibilityHandler&, int)> walk = [&] (juce::AccessibilityHandler& h, int depth)
        {
            juce::String line = juce::String::repeatedString ("  ", depth) + roleName (h.getRole());
            if (h.getTitle().isNotEmpty())          line << " \"" << h.getTitle() << "\"";
            if (h.getDescription().isNotEmpty())    line << " (" << h.getDescription() << ")";
            if (auto* v = h.getValueInterface())    line << " = " << v->getCurrentValueAsString();
            auto st = h.getCurrentState();
            if (st.isCheckable())                   line << (st.isChecked() ? " [on]" : " [off]");
            if (st.isSelected())                    line << " [selected]";
            if (h.getActions().contains (juce::AccessibilityActionType::press)) line << " [press]";
            out << line << "\n";

            for (auto* child : h.getChildren())
                walk (*child, depth + 1);
        };

        auto describeFocus = [] () -> juce::String
        {
            auto* f = juce::Component::getCurrentlyFocusedComponent();
            if (f == nullptr)
                return "nothing";

            juce::String region;
            for (auto* p = f->getParentComponent(); p != nullptr; p = p->getParentComponent())
                if (p->isKeyboardFocusContainer())
                {
                    region = p->getTitle();
                    break;
                }

            auto* h = f->getAccessibilityHandler();
            return (h != nullptr ? h->getTitle() : f->getName()) + " in " + (region.isEmpty() ? juce::String ("the window") : region);
        };

        auto* peer = window->getPeer();
        auto press = [&] (const juce::KeyPress& k, const juce::String& name)
        {
            const bool used = peer->handleKeyPress (k);
            out << "  " << name << (used ? "" : " (not used)") << ": focus on " << describeFocus();
            if (auto* t = mainComponent->getViewState().focusedTrack.get())
                out << "; track " << TrackSpeech::describe (*t);
            out << "\n";
        };

        // A dialog left open (e.g. about missing audio) would block the window
        while (auto* modal = juce::Component::getCurrentlyModalComponent())
        {
            out << "Closed a dialog: " << modal->getName() << " " << modal->getTitle() << "\n";
            modal->exitModalState (0);
            delete modal;
        }

        juce::Process::makeForegroundProcess();
        window->toFront (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (500);

        out << "== Accessibility tree\n";
        if (auto* h = window->getAccessibilityHandler())
            walk (*h, 0);

        for (auto [command, name] : { std::pair { (int) cmdShowArrangement, "Arrangement" }, { (int) cmdToggleMixer, "Mixer" } })
        {
            commandManager.invokeDirectly (command, false);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (500);
            out << "\n== Accessibility tree, " << name << "\n";
            walk (*window->getAccessibilityHandler(), 0);
        }

        commandManager.invokeDirectly (cmdShowArrangement, false);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);

        // The Live tab, pressed as a screen reader would
        std::function<juce::AccessibilityHandler* (juce::AccessibilityHandler&, const juce::String&)> find
            = [&] (juce::AccessibilityHandler& h, const juce::String& title) -> juce::AccessibilityHandler*
        {
            if (h.getTitle() == title && h.getRole() == juce::AccessibilityRole::button)
                return &h;
            for (auto* c : h.getChildren())
                if (auto* found = find (*c, title))
                    return found;
            return nullptr;
        };

        if (auto* liveTab = find (*window->getAccessibilityHandler(), "Live"))
        {
            liveTab->getActions().invoke (juce::AccessibilityActionType::press);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (500);

            if (auto* panel = find (*window->getAccessibilityHandler(), "Go Live"))
            {
                out << "\n== Live panel\n";
                for (auto* c : panel->getParent()->getChildren())
                    walk (*c, 1);
            }
        }

        out << "\n== Keyboard\n";
        mainComponent->grabKeyboardFocus();
        out << "  start: focus on " << describeFocus() << "\n";
        press (juce::KeyPress (juce::KeyPress::F6Key), "F6");
        for (int i = 0; i < 4; ++i)
            press (juce::KeyPress (juce::KeyPress::tabKey), "Tab");
        press (juce::KeyPress (juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0), "Shift-Tab");
        press (juce::KeyPress (juce::KeyPress::F6Key), "F6");
        press (juce::KeyPress (juce::KeyPress::F6Key), "F6");
        press (juce::KeyPress (juce::KeyPress::F6Key, juce::ModifierKeys::shiftModifier, 0), "Shift-F6");
        press (juce::KeyPress (juce::KeyPress::escapeKey), "Escape");
        press (juce::KeyPress (juce::KeyPress::downKey), "Down");
        press (juce::KeyPress (juce::KeyPress::downKey), "Down");
        press (juce::KeyPress (juce::KeyPress::upKey), "Up");
        press (juce::KeyPress ('m', juce::ModifierKeys::altModifier, 0), "Alt-M");
        press (juce::KeyPress ('s', juce::ModifierKeys::altModifier, 0), "Alt-S");
        press (juce::KeyPress ('m', juce::ModifierKeys::altModifier, 0), "Alt-M");
        press (juce::KeyPress ('s', juce::ModifierKeys::altModifier, 0), "Alt-S");

        // A fader, through the value interface a screen reader uses
        out << "\n== Volume fader\n";
        std::function<juce::AccessibilityHandler* (juce::AccessibilityHandler&)> findVolume = [&] (juce::AccessibilityHandler& h) -> juce::AccessibilityHandler*
        {
            if (h.getRole() == juce::AccessibilityRole::slider && h.getTitle() == "Volume")
                return &h;
            for (auto* c : h.getChildren())
                if (auto* found = findVolume (*c))
                    return found;
            return nullptr;
        };

        if (auto* vol = findVolume (*window->getAccessibilityHandler()))
        {
            auto* v = vol->getValueInterface();
            const auto before = v->getCurrentValue();
            out << "  " << v->getCurrentValueAsString() << " at " << before << "\n";
            v->setValue (before + v->getRange().getInterval());
            out << "  increment: " << v->getCurrentValueAsString() << "\n";
            v->setValue (v->getCurrentValue() - v->getRange().getInterval());
            out << "  decrement: " << v->getCurrentValueAsString() << "\n";
        }
        else
        {
            out << "  no Volume slider found\n";
        }

        // What the focus ring and Increase Contrast look like
        auto snapshot = [this, dir = file.getParentDirectory()] (const juce::String& name)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
            auto* content = window->getContentComponent();
            auto img = content->createComponentSnapshot (content->getLocalBounds(), true, 1.0f);
            juce::FileOutputStream stream (dir.getChildFile (name + ".png"));
            stream.setPosition (0);
            stream.truncate();
            juce::PNGImageFormat().writeImageToStream (img, stream);
        };

        mainComponent->grabKeyboardFocus();
        press (juce::KeyPress (juce::KeyPress::F6Key), "F6");
        press (juce::KeyPress (juce::KeyPress::tabKey), "Tab");
        press (juce::KeyPress (juce::KeyPress::tabKey), "Tab");
        press (juce::KeyPress (juce::KeyPress::tabKey), "Tab");
        snapshot ("focus-ring");

        switchTheme (Themes::darkMoreContrast, true);
        snapshot ("more-contrast-dark");
        switchTheme (Themes::lightMoreContrast, true);
        snapshot ("more-contrast-light");
        switchTheme (Themes::light, true);
        snapshot ("light");

        file.replaceWithText (out);
        quit();
    }

    void takeSnapshots (juce::File dir)
    {
        dir.createDirectory();

        // ELSEWHERE_TEST_SCALE=2: render at Retina resolution (for README screenshots)
        const auto scale = (float) juce::jlimit (1.0, 3.0, juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_SCALE", "1").getDoubleValue());

        auto save = [this, dir, scale] (const juce::String& name)
        {
            auto img = window->getContentComponent()->createComponentSnapshot (window->getContentComponent()->getLocalBounds(), true, scale);
            juce::FileOutputStream out (dir.getChildFile (name + ".png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
        };

        auto saveAbout = [dir] (const juce::String& name)
        {
            AboutComponent about;
            auto img = about.createComponentSnapshot (about.getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out (dir.getChildFile (name + ".png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
        };

        // A dialog with a text field, as Add Comment shows it (the field must be easy to find)
        auto saveTextDialog = [dir] (const juce::String& name)
        {
            std::unique_ptr<juce::AlertWindow> aw (Theme::newAlertWindow ("Comment on Guitars", "bars 2-5"));
            Theme::addTextField (*aw, "text", {})->setTextToShowWhenEmpty ("Write a comment", Colours::textDim);
            aw->addButton ("Add Comment", 1);
            aw->addButton ("Cancel", 0);
            aw->setVisible (true);
            auto img = aw->createComponentSnapshot (aw->getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out (dir.getChildFile (name + ".png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
        };
        saveTextDialog ("text_dialog");

        // Optional: ELSEWHERE_TEST_EXPORT=<file.wav> renders the project, to check the audio really plays
        auto exportPath = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_EXPORT", {});

        if (exportPath.isNotEmpty() && mainComponent != nullptr)
        {
            auto& edit = mainComponent->getProject().getEdit();
            juce::BigInteger all;
            all.setRange (0, te::getAllTracks (edit).size(), true);
            auto ok = te::Renderer::renderToFile ("Export", juce::File (exportPath), edit, { 0s, edit.getLength() }, all, true, false, {}, true);
            std::cout << "CHECK export: " << (ok ? "OK" : "FAIL") << std::endl;
        }

        // Put some content in so the clip views get exercised
        if (auto mc = mainComponent)
        {
            auto& edit = mc->getProject().getEdit();
            if (auto at = te::getAudioTracks (edit).getFirst())
            {
                auto& ts = edit.tempoSequence;
                if (auto clip = at->insertMIDIClip ("Chords", { ts.toTime (4_bp), ts.toTime (20_bp) }, nullptr))
                {
                    for (int i = 0; i < 4; ++i)
                        for (int n : { 48, 52, 55 })
                            clip->getSequence().addNote (n + i * 2, te::BeatPosition::fromBeats (i * 4.0), 3.5_bd, 100, 0, nullptr);
                    mc->getViewState().selectionManager.selectOnly (clip.get());

                    // ELSEWHERE_TEST_MIDI_LANES=velocity|<cc number>: shaped velocities, a mod wheel swell and
                    // some pitch bend, with that lane showing under the piano roll
                    if (auto lane = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_MIDI_LANES", {}); lane.isNotEmpty())
                    {
                        auto& seq = clip->getSequence();
                        auto notes = seq.getNotes();
                        for (int i = 0; i < notes.size(); ++i)
                            notes[i]->setVelocity (40 + (i * 37) % 88, nullptr);

                        MidiLanes::drawControllers (seq, 1, 0.0, 0, 6.0, 110 << 7, 0.125, nullptr);
                        MidiLanes::drawControllers (seq, 1, 6.0, 110 << 7, 12.0, 20 << 7, 0.125, nullptr);
                        MidiLanes::drawControllers (seq, MidiLanes::pitchType, 4.0, 8192, 5.0, 12000, 0.0625, nullptr);
                        MidiLanes::drawControllers (seq, MidiLanes::pitchType, 5.0, 12000, 6.0, 8192, 0.0625, nullptr);
                        MidiLanes::Lane::lastType() = lane == "velocity" ? MidiLanes::velocityType
                                                    : lane == "pitch" ? MidiLanes::pitchType : lane.getIntValue();
                    }
                }
                // ELSEWHERE_TEST_LOOP=1: Chords looping every 6 beats, with its track selected (not the
                // count-in: that's an app setting, and would stay set after the snapshot)
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_LOOP", {}).isNotEmpty())
                {
                    if (auto clip = at->getClips().getFirst())
                        clip->setLoopRangeBeats ({ 0_bp, 6_bp });

                    mc->getViewState().selectionManager.selectOnly (at);
                    mc->getViewState().focusTrack (at);
                }

                if (auto slot = at->getClipSlotList().getClipSlots()[0])
                    if (auto sc = te::insertMIDIClip (*slot, "Loop A", ts.toTime ({ 0_bp, 4_bd })))
                        sc->getSequence().addNote (60, 0_bp, 1_bd, 100, 0, nullptr);

                // ELSEWHERE_TEST_SENDS=1: two returns, sends turned up, one pre-fader
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_SENDS", {}).isNotEmpty())
                {
                    Mixing::createReturnTrack (edit);
                    Mixing::createReturnTrack (edit);
                    for (auto t : te::getAudioTracks (edit))
                    {
                        auto sends = Mixing::getSends (*t);
                        if (sends.size() >= 2)
                        {
                            sends[0]->setGainDb (-6.0f);
                            sends[1]->setGainDb (-12.0f);
                            Mixing::setPreFader (*t, *sends[1], true);
                        }
                    }
                }

                // ELSEWHERE_TEST_DRUMS=1: a drum track with a Verse and a Chorus
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_DRUMS", {}).isNotEmpty())
                {
                    if (auto dt = mc->addDrumTrack())
                        if (auto chorus = DrumTrack::createSection (*dt, 32.0, 64.0, "Chorus", drums::Style::rock))
                        {
                            chorus->state.setProperty (DrumTrack::Props::loudness, 0.9, nullptr);
                            chorus->state.setProperty (DrumTrack::Props::complexity, 0.7, nullptr);
                            DrumTrack::regenerate (*chorus);
                            mc->getViewState().selectionManager.selectOnly (chorus);
                            mc->getViewState().detailMode = EditViewState::DetailMode::clip;
                            mc->getViewState().sendChangeMessage();
                        }
                }

                // ELSEWHERE_TEST_TAKES=<folder of audio files>: a take of bars 5-9 from each file on the
                // first track, comped from two of them, with the take lanes open and punch on
                if (auto takesDir = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_TAKES", {}); takesDir.isNotEmpty())
                {
                    auto files = juce::File (takesDir).findChildFiles (juce::File::findFiles, false, "*.wav");
                    files.sort();
                    const te::TimeRange bars { ts.toTime (16_bp), ts.toTime (32_bp) };

                    for (auto& f : files)
                        Takes::addTake (*at, f, bars, {});

                    auto takes = Takes::getTakes (*at);

                    if (takes.size() >= 2)
                    {
                        Takes::useWholeTake (*at, takes.back());
                        Takes::comp (*at, takes.front(), { ts.toTime (20_bp), ts.toTime (24_bp) });
                    }

                    mc->getViewState().takeLanes.insert (at->itemID);
                    EngineHelpers::setPunching (edit, true);
                    edit.getTransport().setLoopRange ({ ts.toTime (20_bp), ts.toTime (28_bp) });
                    mc->getViewState().sendChangeMessage();
                }

                // ELSEWHERE_TEST_GROUP=open|closed: a "Band" group of three MIDI tracks, to see its
                // tracks in tones of the group's colour, or its combined waveform when it's closed
                if (auto groupTest = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_GROUP", {}); groupTest.isNotEmpty())
                {
                    juce::Array<te::Track*> members;
                    te::Track* previous = at;
                    const char* names[] = { "Bass", "Guitar", "Keys" };

                    for (int i = 0; i < 3; ++i)
                        if (auto t = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, previous), nullptr))
                        {
                            t->setName (names[i]);
                            if (auto clip = t->insertMIDIClip (names[i], { ts.toTime (te::BeatPosition::fromBeats (4.0 * i)), ts.toTime (te::BeatPosition::fromBeats (32.0 + 8.0 * i)) }, nullptr))
                                for (int b = 0; b < 32; ++b)
                                    clip->getSequence().addNote (36 + i * 12 + (b * 5) % 12, te::BeatPosition::fromBeats (b * (i == 0 ? 1.0 : 0.75)),
                                                                 te::BeatDuration::fromBeats (i == 2 ? 2.0 : 0.5), 50 + (b * 37) % 77, 0, nullptr);
                            members.add (t.get());
                            previous = t.get();
                        }

                    if (auto group = Mixing::groupTracks (edit, members))
                    {
                        group->setName ("Band");
                        group->setColour (EngineHelpers::trackColourFor (4));

                        if (groupTest == "closed")
                            mc->getViewState().collapsedGroups.insert (group->itemID);

                        mc->getViewState().sendChangeMessage();
                    }
                }

                // ELSEWHERE_TEST_COMMENTS=1: two notes on the first track, with the book open at the second
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_COMMENTS", {}).isNotEmpty())
                {
                    auto& who = mc->getCollab()->getIdentity();
                    Comments::add (edit, who, at->itemID, { ts.toTime (4_bp), ts.toTime (12_bp) }, "Chords could be wider here");
                    auto id = Comments::add (edit, who, at->itemID, { ts.toTime (14_bp), ts.toTime (20_bp) }, "Hearing a bit of popping at the end");
                    mc->getViewState().openCommentId = id;
                    mc->getViewState().detailMode = EditViewState::DetailMode::comments;
                    mc->getViewState().sendChangeMessage();
                }

                // ELSEWHERE_TEST_ROUTING=1: a compressor on the first track keyed from the second, which
                // plays into a new bus with no other output of its own
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_ROUTING", {}).isNotEmpty())
                {
                    auto tracks = te::getAudioTracks (edit);

                    if (tracks.size() > 1)
                    {
                        tracks[1]->setName ("Kick");
                        if (auto p = DeviceRegistry::create (edit, CompressorDevice::xmlTypeName))
                        {
                            at->pluginList.insertPlugin (p, 0, nullptr);
                            Routing::setSidechain (*p, tracks[1]);
                        }

                        if (auto bus = Routing::createBus (edit))
                        {
                            bus->setName ("Drum Bus");
                            Routing::setOutput (*tracks[1], bus);
                        }
                    }

                    mc->getViewState().selectionManager.deselectAll();
                    mc->getViewState().focusTrack (at);
                    mc->getViewState().detailMode = EditViewState::DetailMode::devices;
                    mc->getViewState().sendChangeMessage();
                }

                // ELSEWHERE_TEST_DEVICES=1: load Elsewhere's own devices into the chain
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_DEVICES", {}).isNotEmpty())
                {
                    int index = 0;
                    for (auto& d : DeviceRegistry::getAll())
                        if (auto p = DeviceRegistry::create (edit, d.type))
                            at->pluginList.insertPlugin (p, index++, nullptr);

                    // ...and an LFO moving the first device's first knob
                    if (auto lfo = Modulators::addLFO (*at))
                        if (auto first = at->pluginList[0])
                            if (auto p = first->getAutomatableParameters()[1])
                                p->addModifier (*lfo, 0.4f);

                    mc->getViewState().selectionManager.deselectAll();
                    mc->getViewState().detailMode = EditViewState::DetailMode::devices;
                    mc->getViewState().focusTrack (at);
                    mc->getViewState().sendChangeMessage();
                }
            }
        }

        // Optional stems scenario: ELSEWHERE_TEST_STEMS=<folder of audio files>
        auto stemsDir = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_STEMS", {});

        if (stemsDir.isNotEmpty())
        {
            auto files = juce::File (stemsDir).findChildFiles (juce::File::findFiles, false, "*.wav");
            mainComponent->importAsNewTracks (files, 0s);

            juce::Timer::callAfterDelay (3000, [this, save]
            {
                auto& edit = mainComponent->getProject().getEdit();
                auto& vs = mainComponent->getViewState();
                commandManager.invokeDirectly (cmdNewReturnTrack, false);

                // Group the first two stems
                auto tracks = te::getAudioTracks (edit);
                juce::Array<te::Track*> toGroup;
                for (auto t : tracks)
                    if (t->getName().startsWith ("01") || t->getName().startsWith ("02"))
                        toGroup.add (t);
                vs.selectionManager.deselectAll();
                for (auto t : toGroup)
                    vs.selectionManager.addToSelection (t);
                commandManager.invokeDirectly (cmdGroupTracks, false);

                // Automate the keys' volume and send some to the reverb
                for (auto t : te::getAudioTracks (edit))
                {
                    if (t->getName().startsWith ("03"))
                    {
                        auto vol = t->getVolumePlugin()->volParam;
                        auto& um = edit.getUndoManager();
                        vol->getCurve().addPoint (0s, 0.2f, 0.0f, &um);
                        vol->getCurve().addPoint (4s, 0.9f, 0.0f, &um);
                        vol->getCurve().addPoint (8s, 0.5f, 0.0f, &um);
                        vs.automationLanes[t->itemID] = vol.get();

                        if (auto send = Mixing::findSend (*t, 0))
                            send->setGainDb (-6.0f);
                    }
                }

                vs.sendChangeMessage();
                juce::Timer::callAfterDelay (6000, [this, save]
                {
                    save ("stems_arrangement");
                    commandManager.invokeDirectly (cmdToggleView, false);
                    juce::Timer::callAfterDelay (800, [this, save]
                    {
                        save ("stems_session");
                        juce::MessageManager::callAsync ([this, save] { runPhase1Checks (save); });
                    });
                });
            });
            return;
        }

        // ELSEWHERE_TEST_LAYOUT=1: the default layout, the mixer, the Session grid with its scenes
        // pinned, and the track inputs / outputs; then quit
        if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_LAYOUT", {}) == "1")
        {
            runEditModeChecks();
            commandManager.invokeDirectly (cmdShowArrangement, false);
            juce::Timer::callAfterDelay (800, [this, save]
            {
                save ("layout_arrangement");
                commandManager.invokeDirectly (cmdNewReturnTrack, false);
                commandManager.invokeDirectly (cmdToggleMixer, false);
                juce::Timer::callAfterDelay (1500, [this, save]
                {
                    save ("layout_mixer");
                    commandManager.invokeDirectly (cmdShowSession, false);
                    juce::Timer::callAfterDelay (800, [this, save]
                    {
                        save ("layout_session_scenes");
                        commandManager.invokeDirectly (cmdShowArrangement, false);
                        commandManager.invokeDirectly (cmdShowTrackIO, false);
                        commandManager.invokeDirectly (cmdShowClipList, false);

                        // Tab to transient on the drums (detection runs in the background first)
                        auto& edit = mainComponent->getProject().getEdit();
                        auto tracks = te::getAudioTracks (edit);
                        mainComponent->getViewState().focusTrack (tracks[1]);
                        edit.getTransport().setPosition (te::TimePosition());
                        commandManager.invokeDirectly (cmdNextTransient, false);

                        juce::Timer::callAfterDelay (4000, [this, save]
                        {
                            auto& e = mainComponent->getProject().getEdit();
                            auto first = e.getTransport().getPosition();
                            std::cout << "CHECK next transient moves the playhead: " << (first > te::TimePosition() ? "OK" : "FAIL")
                                      << " (" << first.inSeconds() << " s)" << std::endl;
                            commandManager.invokeDirectly (cmdNextTransient, false);
                            auto second = e.getTransport().getPosition();
                            commandManager.invokeDirectly (cmdPrevTransient, false);
                            auto back = e.getTransport().getPosition();
                            std::cout << "CHECK transients step forward and back: "
                                      << (second > first && std::abs (back.inSeconds() - first.inSeconds()) < 0.001 ? "OK" : "FAIL")
                                      << " (" << second.inSeconds() << " s)" << std::endl;
                            save ("layout_track_io_lists");
                            quit();
                        });
                    });
                });
            });
            return;
        }

        save ("session");
        saveAbout ("about");

        // ELSEWHERE_TEST_SURFACES=1: the Control Surfaces window
        if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_SURFACES", {}).isNotEmpty() && mainComponent != nullptr)
        {
            ControlSurfaceSettings settings (*engine);
            auto img = settings.createComponentSnapshot (settings.getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out (dir.getChildFile ("control_surfaces.png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
        }

        // ELSEWHERE_TEST_ROUTING: the keyed compressor's panel on its own
        if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_ROUTING", {}).isNotEmpty() && mainComponent != nullptr)
        {
            if (auto first = te::getAudioTracks (mainComponent->getProject().getEdit()).getFirst())
                if (auto comp = first->pluginList.findFirstPluginOfType<CompressorDevice>())
                {
                    DevicePanel panel (*comp);
                    panel.setSize (panel.getIdealWidth(), 230);
                    auto img = panel.createComponentSnapshot (panel.getLocalBounds(), true, 2.0f);
                    juce::FileOutputStream out (dir.getChildFile ("routing_device.png"));
                    out.setPosition (0); out.truncate();
                    juce::PNGImageFormat().writeImageToStream (img, out);
                }
        }

        // ELSEWHERE_TEST_EXPORT_DIALOG=1: the Export dialog, set for the mix and stems
        if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_EXPORT_DIALOG", {}).isNotEmpty() && mainComponent != nullptr)
        {
            auto& project = mainComponent->getProject();
            auto settings = Export::lastSettings();
            settings.what = Export::What::mixAndStems;
            settings.loudness = Export::Loudness::streaming;
            Export::DialogContent content (project.getEdit(), settings, project.getFolder().getChildFile ("Exports"), project.getFolder(), project.getName());
            std::unique_ptr<juce::AlertWindow> aw (Theme::newAlertWindow ("Export", project.getName()));
            aw->addCustomComponent (&content);
            aw->addButton ("Export", 1);
            aw->addButton ("Cancel", 0);
            aw->setVisible (true);
            auto img = aw->createComponentSnapshot (aw->getLocalBounds(), true, 2.0f);
            juce::FileOutputStream out (dir.getChildFile ("export_dialog.png"));
            out.setPosition (0); out.truncate();
            juce::PNGImageFormat().writeImageToStream (img, out);
            aw->removeCustomComponent (0);
        }

        commandManager.invokeDirectly (cmdToggleView, false);
        juce::Timer::callAfterDelay (800, [this, save, saveAbout, saveTextDialog]
        {
            mainComponent->getProject().getEdit().getTransport().setPosition (6s);
            save ("arrangement");
            commandManager.invokeDirectly (cmdToggleDetail, false);
            juce::Timer::callAfterDelay (800, [this, save, saveAbout, saveTextDialog]
            {
                save ("arrangement_clip");

                // ELSEWHERE_TEST_THEME=light: switch theme live, as the Appearance menu does, and snapshot again
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_THEME", {}) != "light")
                    return quit();

                switchTheme (Themes::light, true);
                juce::Timer::callAfterDelay (800, [this, save, saveAbout, saveTextDialog]
                {
                    save ("arrangement_clip_light");
                    saveAbout ("about_light");
                    saveTextDialog ("text_dialog_light");
                    commandManager.invokeDirectly (cmdToggleView, false);
                    juce::Timer::callAfterDelay (800, [this, save]
                    {
                        save ("session_light");

                        // ELSEWHERE_TEST_MIXER=1: the mixer too, light then dark
                        if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_MIXER", {}).isEmpty())
                            return quit();

                        commandManager.invokeDirectly (cmdToggleMixer, false);
                        juce::Timer::callAfterDelay (1200, [this, save]
                        {
                            save ("mixer_light");
                            switchTheme (Themes::dark, true);
                            juce::Timer::callAfterDelay (800, [save] { save ("mixer"); quit(); });
                        });
                    });
                });
            });
        });
    }

    /** The edit's state minus per-machine and bookkeeping bits, for comparing collaborators. */
    static juce::String canonicalState (te::Edit& edit)
    {
        auto v = edit.state.createCopy();

        for (auto type : { te::IDs::INPUTDEVICES, te::IDs::TRANSPORT })
            v.removeChild (v.getChildWithName (type), nullptr);

        std::function<void (juce::ValueTree)> strip = [&] (juce::ValueTree n)
        {
            for (auto prop : { "lastSignificantChange", "modifiedBy", "windowX", "windowY", "windowLocked",
                               "elsewhereCollabClock", "elsewhereSavedBy", "creationTime", "appVersion" })
                n.removeProperty (prop, nullptr);

            // Empty placeholders the engine adds when it loads a file
            for (int i = n.getNumChildren(); --i >= 0;)
                if (auto c = n.getChild (i); c.hasType ("MODIFIERASSIGNMENTS") && c.getNumChildren() == 0 && c.getNumProperties() == 0)
                    n.removeChild (i, nullptr);

            for (auto c : n)
                strip (c);

            // Inside a track, only the order of same-type children matters (e.g. plugin chain);
            // the engine doesn't care whether OUTPUTDEVICES comes before CLIPSLOTS
            if (n.hasType (te::IDs::TRACK))
            {
                juce::Array<juce::ValueTree> kids;
                for (auto c : n) kids.add (c);
                std::stable_sort (kids.begin(), kids.end(), [] (auto& a, auto& b) { return a.getType().toString() < b.getType().toString(); });
                n.removeAllChildren (nullptr);
                for (auto& c : kids) n.appendChild (c, nullptr);
            }
        };

        strip (v);
        return v.toXmlString();
    }

    /** Dev-only: two instances (ELSEWHERE_USER_ID=A / B) edit the same shared project at once. */
    void runCollabTest (const juce::String& role, juce::File outDir)
    {
        outDir.createDirectory();
        auto step = [] (int ms, std::function<void()> fn) { juce::Timer::callAfterDelay (ms, std::move (fn)); };

        // C: someone opening the project afterwards: just load, catch up, and report
        if (role == "C")
        {
            step (6000, [this, outDir]
            {
                auto& edit = mainComponent->getProject().getEdit();
                outDir.getChildFile ("C.xml").replaceWithText (canonicalState (edit));
                std::cout << "COLLAB C pending=" << mainComponent->getCollab()->getNumPendingRemote()
                          << " tracks=" << te::getAudioTracks (edit).size() << std::endl;
                quit();
            });
            return;
        }

        step (1500, [this, role]
        {
            auto& edit = mainComponent->getProject().getEdit();
            auto& um = edit.getUndoManager();
            auto findTrack = [&edit] (const juce::String& name) -> te::AudioTrack*
            {
                for (auto t : te::getAudioTracks (edit))
                    if (t->getName().startsWith (name))
                        return t;
                return nullptr;
            };

            if (role == "A")
            {
                auto t = mainComponent->addTrack (true);
                t->setName ("A track");
                t->getVolumePlugin()->setVolumeDb (-6.0f);
                if (auto b = findTrack ("Bass")) b->setName ("Bass renamed by A");
                if (auto d = findTrack ("drums"))
                    Comments::add (edit, mainComponent->getCollab()->getIdentity(), d->itemID, te::TimeRange (4s, te::TimePosition (8s)), "Kick is too loud in the intro");

                if (auto mc = t->insertMIDIClip ("A riff", te::TimeRange (0s, te::TimePosition (4s)), nullptr))
                    for (int n : { 60, 64, 67 })
                        mc->getSequence().addNote (n, te::BeatPosition::fromBeats (n - 60), 1_bd, 100, 0, &um);
            }
            else
            {
                auto t = mainComponent->addTrack (false);
                t->setName ("B track");
                edit.tempoSequence.getTempo (0)->setBpm (128.0);
                if (auto b = findTrack ("Bass")) b->setName ("Bass renamed by B");
                if (auto g = findTrack ("Guitar")) g->setMute (true);
            }
        });

        step (3500, [this, role]
        {
            auto& edit = mainComponent->getProject().getEdit();

            if (role == "A")
            {
                for (auto t : te::getAudioTracks (edit))
                    if (t->getName() == "drums")
                        t->getVolumePlugin()->setVolumeDb (-3.0f);

                // A drum track with a chorus section (its notes aren't synced: Bob generates his own)
                if (auto dt = mainComponent->addDrumTrack())
                {
                    dt->setName ("Drums by A");
                    DrumTrack::createSection (*dt, 32.0, 48.0, "Chorus", drums::Style::rock);
                }

                // New audio arriving mid-session (tests audio that syncs after the edit that uses it)
                auto newAudio = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_NEW_AUDIO", {});
                if (newAudio.isNotEmpty())
                    mainComponent->importAsNewTracks ({ juce::File (newAudio) }, 0s);
            }
            else
            {
                Mixing::createReturnTrack (edit);

                for (auto t : te::getAudioTracks (edit))
                    if (t->getName().startsWith ("hydra"))
                        edit.deleteTrack (t);
            }
        });

        // Both change the same drum section at the same moment: Alice the complexity, Bob the style
        step (9000, [this, role]
        {
            auto& edit = mainComponent->getProject().getEdit();
            for (auto t : te::getAudioTracks (edit))
                for (auto c : t->getClips())
                    if (DrumTrack::isSection (*c) && DrumTrack::getLabel (c->state) == "Chorus")
                    {
                        edit.getUndoManager().beginNewTransaction ("test");
                        if (role == "A") c->state.setProperty (DrumTrack::Props::complexity, 0.9, &edit.getUndoManager());
                        else             c->state.setProperty (DrumTrack::Props::style, (int) drums::Style::funk, &edit.getUndoManager());
                    }
        });

        // Screenshot of the collaboration UI (presence chips, comments) while both are connected
        step (13000, [this, role, outDir]
        {
            if (role != "A")
                return;

            commandManager.invokeDirectly (cmdToggleView, false);
            auto& vs = mainComponent->getViewState();
            vs.detailMode = EditViewState::DetailMode::comments;
            vs.sendChangeMessage();

            juce::Timer::callAfterDelay (1200, [this, outDir]
            {
                auto* c = window->getContentComponent();
                auto img = c->createComponentSnapshot (c->getLocalBounds(), true, 1.0f);
                juce::FileOutputStream out (outDir.getChildFile ("A_ui.png"));
                juce::PNGImageFormat().writeImageToStream (img, out);
            });
        });

        // Let both sides exchange everything, save, then write out what each ended up with
        step (16000, [this] { mainComponent->getProject().save(); });

        step (20000, [this, role, outDir]
        {
            auto& edit = mainComponent->getProject().getEdit();
            outDir.getChildFile (role + ".xml").replaceWithText (canonicalState (edit));
            int missing = 0;
            for (auto t : te::getAudioTracks (edit))
                for (auto c : t->getClips())
                    if (auto ac = dynamic_cast<te::AudioClipBase*> (c); ac != nullptr && ! ac->getOriginalFile().existsAsFile())
                        ++missing;

            for (auto t : te::getAudioTracks (edit))
                for (auto c : t->getClips())
                    if (auto mc = dynamic_cast<te::MidiClip*> (c); mc != nullptr && DrumTrack::isSection (*mc))
                    {
                        auto s = DrumTrack::readSettings (mc->state);
                        std::cout << "COLLAB-DRUMS " << role << " " << DrumTrack::getLabel (mc->state) << " style=" << drums::styleName (s.style)
                                  << " complexity=" << s.complexity << " notes=" << mc->getSequence().getNumNotes() << std::endl;
                    }

            std::cout << "COLLAB " << role << " pending=" << mainComponent->getCollab()->getNumPendingRemote()
                      << " tracks=" << te::getAudioTracks (edit).size() << " missingAudio=" << missing << std::endl;
            quit();
        });
    }

    /** Dev-only: both instances go live, play (muted), and report what they exchanged. */
    void runLiveTest (const juce::String& role, juce::File outDir)
    {
        outDir.createDirectory();

        juce::Timer::callAfterDelay (1500, [this]
        {
            auto& edit = mainComponent->getProject().getEdit();
            if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_NO_MUTE", {}).isEmpty())
                edit.getMasterVolumePlugin()->setVolumeDb (-100.0f);   // silent speakers; capture happens before the master fader
            if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_REALTIME", {}).isNotEmpty())
                mainComponent->getLive()->setRealtime (true);

            if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_NO_LIVE", {}).isEmpty())
                mainComponent->toggleLive();

            edit.getTransport().setPosition (0s);
            edit.getTransport().play (false);
        });

        // Shared Session view: Alice makes a Session clip (collab sync delivers it to Bob),
        // launches it, then stops everything; Bob reports what his copy is doing
        auto firstSlotClip = [this]() -> te::Clip*
        {
            if (auto t = te::getAudioTracks (mainComponent->getProject().getEdit()).getFirst())
                if (auto slot = t->getClipSlotList().getClipSlots()[0])
                    return slot->getClip();
            return nullptr;
        };

        if (role == "A")
        {
            juce::Timer::callAfterDelay (2000, [this]
            {
                auto& edit = mainComponent->getProject().getEdit();
                edit.getSceneList().ensureNumberOfScenes (1);
                if (auto t = te::getAudioTracks (edit).getFirst())
                {
                    t->getClipSlotList().ensureNumberOfSlots (1);
                    if (auto slot = t->getClipSlotList().getClipSlots()[0]; slot != nullptr && slot->getClip() == nullptr)
                        if (auto c = te::insertMIDIClip (*slot, "Shared", { 0s, edit.tempoSequence.toTime (te::BeatPosition::fromBeats (4.0)) }))
                            c->getSequence().addNote (60, te::BeatPosition(), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);
                }
            });
            juce::Timer::callAfterDelay (20000, [firstSlotClip] { if (auto c = firstSlotClip()) Launcher::userLaunchClip (*c); });
            juce::Timer::callAfterDelay (30000, [this] { Launcher::userStopAll (mainComponent->getProject().getEdit()); });
        }
        else
        {
            // Bob records the session from 10 s to 40 s: Alice should end up on her own track
            juce::Timer::callAfterDelay (10000, [this] { mainComponent->getProject().getEdit().getTransport().record (false, true); });
            juce::Timer::callAfterDelay (40000, [this] { mainComponent->getProject().getEdit().getTransport().stopRecording (false); });
            juce::Timer::callAfterDelay (43000, [this]
            {
                for (auto t : te::getAudioTracks (mainComponent->getProject().getEdit()))
                {
                    if (! t->getName().endsWith ("(live)"))
                        continue;

                    for (auto c : t->getClips())
                    {
                        auto wc = dynamic_cast<te::WaveAudioClip*> (c);
                        float peak = 0.0f;
                        if (wc != nullptr)
                        {
                            juce::AudioFormatManager fm;
                            fm.registerBasicFormats();
                            if (std::unique_ptr<juce::AudioFormatReader> rd { fm.createReaderFor (wc->getOriginalFile()) })
                            {
                                juce::AudioBuffer<float> b ((int) rd->numChannels, (int) rd->lengthInSamples);
                                rd->read (&b, 0, b.getNumSamples(), 0, true, true);
                                peak = b.getMagnitude (0, b.getNumSamples());
                            }
                        }
                        std::cout << "LIVE-REC-CHECK B track=\"" << t->getName() << "\" start=" << juce::String (c->getPosition().getStart().inSeconds(), 2)
                                  << "s length=" << juce::String (c->getPosition().getLength().inSeconds(), 1) << "s peak=" << juce::String (peak, 3) << std::endl;
                    }
                }
            });

            for (int t : { 26000, 36000 })
                juce::Timer::callAfterDelay (t, [firstSlotClip, t]
                {
                    auto c = firstSlotClip();
                    bool queued = false;
                    std::cout << "LIVE-LAUNCH-CHECK B T+" << t / 1000 << "s hasClip=" << (c != nullptr)
                              << " playing=" << (c != nullptr && Launcher::isPlayingOrQueued (*c, queued)) << std::endl;
                });
        }

        // Optional: per-track meters while playing (ELSEWHERE_TEST_METERS=1), to find silent tracks
        if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_METERS", {}).isNotEmpty())
        {
            static std::map<juce::String, std::unique_ptr<te::LevelMeasurer::Client>> meters;

            juce::Timer::callAfterDelay (2500, [this]
            {
                if (juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_TEST_BACK", {}).isNotEmpty())
                    for (auto t : te::getAudioTracks (mainComponent->getProject().getEdit()))
                        EngineHelpers::backToArrangement (*t);

                for (auto t : te::getAudioTracks (mainComponent->getProject().getEdit()))
                {
                    auto& c = meters[t->getName()];
                    c = std::make_unique<te::LevelMeasurer::Client>();
                    t->getLevelMeterPlugin()->measurer.addClient (*c);
                }
            });

            for (int t = 5000; t < 20000; t += 5000)
                juce::Timer::callAfterDelay (t, [t]
                {
                    std::cout << "METERS T+" << t / 1000 << "s";
                    for (auto& [name, c] : meters)
                        std::cout << " | " << name << "=" << juce::roundToInt (juce::jmax (c->getAndClearAudioLevel (0).dB, c->getAndClearAudioLevel (1).dB));
                    std::cout << std::endl;
                });
        }

        for (int t = 3000; t < 47000; t += 3000)
            juce::Timer::callAfterDelay (t, [this, t]
            {
                auto& tr = mainComponent->getProject().getEdit().getTransport();
                std::cout << "t=" << juce::Time::getMillisecondCounter() << " T+" << t / 1000 << "s playing=" << tr.isPlaying() << " pos=" << tr.getPosition().inSeconds()
                          << " peers=" << mainComponent->getLive()->getPeers().size();

                for (auto& p : mainComponent->getLive()->getPeers())
                {
                    std::cout << " hearing " << p.name << " at level " << p.level;

                    if (mainComponent->getLive()->isRealtime())
                        std::cout << " latency~" << juce::roundToInt (p.estimatedLatencyMs) << "ms buffer=" << juce::roundToInt (p.jitterBufferMs)
                                  << "ms underruns=" << p.underruns << " rtt=" << juce::roundToInt (p.roundTripMs) << "ms"
                                  << " buffered=" << juce::roundToInt (p.bufferedMs) << "ms drift=" << juce::roundToInt (p.clockDriftPpm) << "ppm";
                }

                std::cout << std::endl;
            });

        juce::Timer::callAfterDelay (47000, [this, role, outDir]
        {
            commandManager.invokeDirectly (cmdToggleView, false);
            mainComponent->getViewState().detailMode = EditViewState::DetailMode::live;
            mainComponent->getViewState().sendChangeMessage();
            auto* c = window->getContentComponent();
            auto img = c->createComponentSnapshot (c->getLocalBounds(), true, 1.0f);
            juce::FileOutputStream out (outDir.getChildFile (role + "_live.png"));
            juce::PNGImageFormat().writeImageToStream (img, out);
        });

        juce::Timer::callAfterDelay (48000, [this, role]
        {
            auto* live = mainComponent->getLive();
            auto stats = live->getStats();
            std::cout << "LIVE " << role << " sent=" << stats.intervalsSent << " received=" << stats.intervalsReceived
                      << " interval=" << stats.currentInterval;

            for (auto& p : live->getPeers())
                std::cout << " | peer=" << p.name << " connected=" << p.connected << " rtt=" << juce::roundToInt (p.roundTripMs)
                          << "ms lastInterval=" << p.lastIntervalReceived << " state=" << p.state;

            std::cout << std::endl;
            live->stop();
            mainComponent->getProject().getEdit().getTransport().stop (false, true);
            quit();
        });
    }

    /** Dev-only: exercises freeze, bounce, warp markers, presets and templates, writing results to stdout. */
    void runPhase1Checks (std::function<void (const juce::String&)> save)
    {
        static bool hasRun = false;
        if (hasRun) return;
        hasRun = true;


        auto& project = mainComponent->getProject();
        auto& edit = project.getEdit();
        auto& vs = mainComponent->getViewState();
        auto report = [] (const juce::String& what, bool ok) { std::cout << "CHECK " << what << ": " << (ok ? "OK" : "FAIL") << std::endl; };

        te::AudioTrack* keys = nullptr, *vocals = nullptr, *bass = nullptr, *ret = nullptr;
        for (auto t : te::getAudioTracks (edit))
        {
            if (t->getName().startsWith ("03")) keys = t;
            if (t->getName().startsWith ("04")) vocals = t;
            if (t->getName().startsWith ("02")) bass = t;
            if (Mixing::isReturnTrack (*t)) ret = t;
        }

        // Presets: save the return's reverb, change it, load it back
        if (ret != nullptr)
        {
            auto userPlugins = EngineHelpers::getUserPlugins (*ret);
            if (auto reverb = userPlugins.getFirst())
            {
                auto param = reverb->getAutomatableParameters().getFirst();
                auto original = param->getCurrentValue();
                report ("preset save", Library::savePreset (*reverb, "Test Preset"));
                param->setParameter (param->getValueRange().getStart(), juce::sendNotification);
                auto presetFile = Library::getPresetFolder (*reverb).getChildFile ("Test Preset.elsewherepreset");
                report ("preset load", Library::loadPreset (*reverb, presetFile));
                report ("preset restores value", std::abs (param->getCurrentValue() - original) < 0.001f);
                presetFile.deleteFile();
            }
        }

        // Template: should contain tracks but no audio clips
        auto templateFile = Library::getTemplatesFolder().getChildFile ("Test Template.elsewhere");
        project.save();
        report ("template save", Library::saveTemplate (project.getEditFile(), templateFile));
        auto templateText = templateFile.loadFileAsString();
        report ("template has no audio clips", templateText.isNotEmpty() && ! templateText.contains ("<AUDIOCLIP"));
        report ("template keeps tracks", templateText.contains ("03 Keys"));
        templateFile.deleteFile();

        // Freeze
        if (vocals != nullptr)
        {
            vocals->setFrozen (true, te::Track::individualFreeze);
            report ("freeze", vocals->isFrozen (te::Track::individualFreeze));
        }

        // Bounce
        if (keys != nullptr)
        {
            auto before = te::getAudioTracks (edit).size();
            bounceTrack (*keys, edit, vs);
            report ("bounce adds track", te::getAudioTracks (edit).size() == before + 1);
            report ("bounce file written", project.getFolder().getChildFile ("Samples/Bounced").getNumberOfChildFiles (juce::File::findFiles) > 0);
        }

        // Warp markers + sample editor
        if (bass != nullptr)
        {
            if (auto clip = dynamic_cast<te::WaveAudioClip*> (bass->getClips().getFirst()))
            {
                clip->setWarpTime (true);
                auto& wtm = clip->getWarpTimeManager();
                auto n = wtm.getMarkers().size();
                wtm.insertMarker (te::WarpMarker (4s, 5s));
                report ("warp marker added", wtm.getMarkers().size() == n + 1);
                vs.selectionManager.selectOnly (clip);
                vs.detailMode = EditViewState::DetailMode::clip;
                vs.sendChangeMessage();
            }
        }

        commandManager.invokeDirectly (cmdToggleView, false);

        juce::Timer::callAfterDelay (2500, [this, save]
        {
            save ("phase1_arrangement");
            mainComponent->getProject().save();

            // Open (then close) every dialog, so assertion failures in them show up in the log
            std::cout << "CHECK-DIALOGS begin" << std::endl;
            mainComponent->showImportDialog();
            showAudioSettings();
            showPluginManager();
            mainComponent->showShareDialog();
            mainComponent->showVersionHistory();

            juce::Timer::callAfterDelay (1500, [this]
            {
                mainComponent->dismissFileChooser();
                juce::ModalComponentManager::getInstance()->cancelAllModalComponents();

                for (int i = juce::TopLevelWindow::getNumTopLevelWindows(); --i >= 0;)
                    if (auto dw = dynamic_cast<juce::DialogWindow*> (juce::TopLevelWindow::getTopLevelWindow (i)))
                        delete dw;

                std::cout << "CHECK-DIALOGS end" << std::endl;
                quit();
            });
        });
    }

    void loadProject (const juce::File& editFile)
    {
        if (window == nullptr)
            return;

        window->clearContentComponent();
        mainComponent = nullptr;

        auto* mc = new MainComponent (*engine, editFile, *this);
        mainComponent = mc;
        mc->setSize (1400, 860);
        mc->onTitleChanged = [this] { updateTitle(); };
        mc->onMenuStateChanged = [this] { menuItemsChanged(); };
        window->setContentOwned (mc, false);

        commandManager.registerAllCommandsForTarget (mc);
        commandManager.setFirstCommandTarget (mc);
        menuItemsChanged();

        addRecentProject (editFile);
        updateTitle();
        if (mc->isShowing())
            mc->grabKeyboardFocus();

        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<MainComponent> (mc)]
        {
            if (sp != nullptr)
                sp->checkProjectAudio();
        });
    }

    void openProjectChecked (const juce::File& f)
    {
        // Is a friend editing this right now (via Dropbox)?
        auto lock = Project::readForeignLock (f.getParentDirectory());

        // Shared projects are meant to be open in several places at once
        if (! lock || Project::isSharedProjectFile (f))
            return loadProject (f);

        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, "Project in use",
            lock->user + " is currently working on this project (" + lock->machine + "). "
            "If you both make changes, Dropbox will create a conflicted copy. Open it anyway?",
            "Open Anyway", "Cancel", nullptr,
            juce::ModalCallbackFunction::create ([this, f] (int r) { if (r == 1) loadProject (f); }));
    }

    void confirmDiscardChanges (std::function<void()> then)
    {
        if (mainComponent == nullptr || ! mainComponent->getProject().hasChangedSinceSaved())
            return then();

        auto options = juce::MessageBoxOptions()
                         .withIconType (juce::MessageBoxIconType::QuestionIcon)
                         .withTitle ("Save changes?")
                         .withMessage ("Do you want to save the changes to \"" + mainComponent->getProject().getName() + "\"?")
                         .withButton ("Save")
                         .withButton ("Don't Save")
                         .withButton ("Cancel");

        juce::AlertWindow::showAsync (options, [this, then] (int result)
        {
            // Only carry on once the project really is saved
            if (result == 1)       { saveProjectThen ([then] (bool saved) { if (saved) then(); }); }
            else if (result == 2)  { then(); }
        });
    }

    void updateTitle()
    {
        if (window != nullptr && mainComponent != nullptr)
        {
            auto& p = mainComponent->getProject();
            // Just the project: the menu bar and the wordmark already say it's Elsewhere
            auto name = p.getName().isNotEmpty() ? p.getName() : juce::String ("Elsewhere");
            window->setName (name + (p.hasChangedSinceSaved() ? " *" : ""));
        }
    }

    static bool isTemporaryLocation (const juce::File& f)
    {
        auto path = f.getFullPathName();
        return f.isAChildOf (juce::File::getSpecialLocation (juce::File::tempDirectory))
                || path.startsWith ("/private/var/folders/") || path.startsWith ("/var/folders/")
                || path.startsWith ("/tmp/") || path.startsWith ("/private/tmp/");
    }

    juce::File getRecentProject()
    {
        auto recent = engine->getPropertyStorage().getPropertiesFile().getValue ("recentProjects");
        juce::RecentlyOpenedFilesList list;
        list.restoreFromString (recent);
        list.removeNonExistentFiles();

        // Never reopen something in a temporary folder (e.g. a copy made by a test run)
        for (int i = 0; i < list.getNumFiles(); ++i)
            if (! isTemporaryLocation (list.getFile (i)))
                return list.getFile (i);

        return {};
    }

    void addRecentProject (const juce::File& f)
    {
        if (isTestRun)
            return;

        auto& props = engine->getPropertyStorage().getPropertiesFile();
        juce::RecentlyOpenedFilesList list;
        list.restoreFromString (props.getValue ("recentProjects"));
        list.addFile (f);

        for (int i = list.getNumFiles(); --i >= 0;)
            if (isTemporaryLocation (list.getFile (i)))
                list.removeFile (list.getFile (i));

        props.setValue ("recentProjects", list.toString());

        // macOS can only add files that exist on disk (new projects aren't saved yet)
        if (f.existsAsFile())
            list.registerRecentFileNatively (f);
    }

    //==============================================================================
    // MenuBarModel
    juce::StringArray getMenuBarNames() override
    {
       #if JUCE_MAC
        return { "File", "Edit", "Create", "View", "Options" };
       #else
        return { "File", "Edit", "Create", "View", "Options", "Help" };   // the Mac has About in the Elsewhere menu
       #endif
    }

    juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
    {
        juce::PopupMenu m;
        auto* cm = &commandManager;

        if (index == 0)
        {
            m.addCommandItem (cm, cmdNewProject);

            juce::PopupMenu templates;
            auto templateFiles = Library::getTemplates();
            for (int i = 0; i < templateFiles.size(); ++i)
                templates.addItem (7000 + i, templateFiles[i].getFileNameWithoutExtension());
            if (templateFiles.isEmpty())
                templates.addItem (-1, "No templates yet", false);
            m.addSubMenu ("New Project from Template", templates);

            m.addCommandItem (cm, cmdOpenProject);

            juce::PopupMenu recent;
            juce::RecentlyOpenedFilesList list;
            list.restoreFromString (engine->getPropertyStorage().getPropertiesFile().getValue ("recentProjects"));
            list.createPopupMenuItems (recent, 5000, true, true);
            m.addSubMenu ("Open Recent", recent);

            m.addSeparator();
            m.addCommandItem (cm, cmdSave);
            m.addCommandItem (cm, cmdSaveAs);
            m.addSeparator();
            m.addCommandItem (cm, cmdImportAudio);
            m.addCommandItem (cm, cmdExport);
            m.addSeparator();
            m.addCommandItem (cm, cmdImportSession);
            m.addCommandItem (cm, cmdExportDawProject);
            m.addSeparator();
            m.addCommandItem (cm, cmdShareProject);
            m.addCommandItem (cm, cmdVersionHistory);
            m.addSeparator();
            m.addItem (6002, "Save as Template...");
            m.addItem (6003, "Save as Default Template");
            m.addSeparator();
            m.addItem (6001, "Show Project in " + fileManagerName());
        }
        else if (index == 1)
        {
            m.addCommandItem (cm, cmdUndo);
            m.addCommandItem (cm, cmdRedo);
            m.addSeparator();
            m.addCommandItem (cm, cmdDuplicate);
            m.addCommandItem (cm, cmdSplit);
            m.addCommandItem (cm, cmdDelete);
            m.addSeparator();
            m.addCommandItem (cm, cmdEditShuffle);
            m.addCommandItem (cm, cmdEditSlip);
            m.addCommandItem (cm, cmdEditSpot);
            m.addCommandItem (cm, cmdEditGrid);
        }
        else if (index == 2)
        {
            m.addCommandItem (cm, cmdNewAudioTrack);
            m.addCommandItem (cm, cmdNewMidiTrack);
            m.addCommandItem (cm, cmdNewDrumTrack);
            m.addCommandItem (cm, cmdNewReturnTrack);
            m.addSeparator();
            m.addCommandItem (cm, cmdGroupTracks);
        }
        else if (index == 3)
        {
            m.addCommandItem (cm, cmdShowArrangement);
            m.addCommandItem (cm, cmdShowSession);
            m.addCommandItem (cm, cmdToggleMixer);
            m.addCommandItem (cm, cmdToggleView);
            m.addSeparator();
            m.addCommandItem (cm, cmdShowTrackList);
            m.addCommandItem (cm, cmdToggleBrowser);
            m.addCommandItem (cm, cmdShowDetail);
            m.addCommandItem (cm, cmdToggleDetail);
            m.addCommandItem (cm, cmdShowClipList);
            m.addCommandItem (cm, cmdShowScenes);
            m.addCommandItem (cm, cmdShowTrackIO);
            m.addSeparator();
            m.addCommandItem (cm, cmdShowAutomation);
            m.addCommandItem (cm, cmdFollow);
            m.addSeparator();
            m.addCommandItem (cm, cmdZoomIn);
            m.addCommandItem (cm, cmdZoomOut);
            m.addCommandItem (cm, cmdZoomToFit);
            m.addSeparator();
            m.addCommandItem (cm, cmdPlayStop);
            m.addCommandItem (cm, cmdRecord);
            m.addCommandItem (cm, cmdToggleLoop);
            m.addCommandItem (cm, cmdToggleMetronome);
            m.addCommandItem (cm, cmdAutomationArm);
            m.addSeparator();
            m.addCommandItem (cm, cmdAddMarker);
            m.addCommandItem (cm, cmdShowMarkers);
            m.addCommandItem (cm, cmdPrevMarker);
            m.addCommandItem (cm, cmdNextMarker);

            juce::PopupMenu goTo;
            for (int i = cmdGoToMarker1; i <= cmdGoToMarkerLast; ++i)
                goTo.addCommandItem (cm, i);
            m.addSubMenu ("Go to Marker", goTo);

            m.addCommandItem (cm, cmdPrevTransient);
            m.addCommandItem (cm, cmdNextTransient);
            m.addSeparator();
            m.addCommandItem (cm, cmdGoLive);
        }
        else if (index == 4)
        {
            m.addCommandItem (cm, cmdAudioSettings);
            m.addCommandItem (cm, cmdPluginManager);
            m.addCommandItem (cm, cmdControlSurfaces);
            m.addSeparator();

            juce::PopupMenu appearance;
            auto current = getAppearance();
            appearance.addItem (6100, "Auto (match macOS)", true, current == "auto");
            appearance.addItem (6101, "Light", true, current == "light");
            appearance.addItem (6102, "Dark", true, current == "dark");
            appearance.addSeparator();
            appearance.addItem (6103, A11y::settings().moreContrast ? "Increase Contrast (on in " + systemSettingsName() + ")" : juce::String ("Increase Contrast"),
                                ! A11y::settings().moreContrast, wantsMoreContrast());
            m.addSubMenu ("Appearance", appearance);
            m.addSeparator();
            m.addCommandItem (cm, cmdMidiLearn);
            m.addCommandItem (cm, cmdSetName);
        }
        else if (index == 5)
        {
            m.addItem (6200, "About Elsewhere");
        }

        return m;
    }

    void menuItemSelected (int itemID, int) override
    {
        if (itemID >= 5000 && itemID < 5100)
        {
            juce::RecentlyOpenedFilesList list;
            list.restoreFromString (engine->getPropertyStorage().getPropertiesFile().getValue ("recentProjects"));
            openProject (list.getFile (itemID - 5000));
        }
        else if (itemID >= 7000 && itemID < 7500)
        {
            newProjectFrom (Library::getTemplates()[itemID - 7000]);
        }
        else if (itemID >= 6100 && itemID <= 6102)
        {
            engine->getPropertyStorage().getPropertiesFile().setValue ("appearance", juce::StringArray { "auto", "light", "dark" }[itemID - 6100]);
            applyAppearance (true);
        }
        else if (itemID == 6103)
        {
            auto& file = engine->getPropertyStorage().getPropertiesFile();
            file.setValue ("moreContrast", ! file.getBoolValue ("moreContrast", false));
            applyAppearance (true);
        }
        else if (itemID == 6200)
        {
            AboutComponent::show();
        }
        else if (itemID == 6002)
        {
            saveAsTemplate (false);
        }
        else if (itemID == 6003)
        {
            saveAsTemplate (true);
        }
        else if (itemID == 6001 && mainComponent != nullptr)
        {
            mainComponent->getProject().getEditFile().revealToUser();
        }
    }
};

} // namespace elsewhere

START_JUCE_APPLICATION (elsewhere::ElsewhereApplication)
