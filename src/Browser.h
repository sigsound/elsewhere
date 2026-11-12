// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ArrangementView.h"

namespace elsewhere
{
//==============================================================================
/** Left-hand browser: quick places plus a file tree. Drag audio/MIDI files onto tracks or slots.

    Two places collect samples recorded on Somewhere (the iPhone app): "Somewhere" is the
    project's Samples/Somewhere folder, and "Somewhere Inbox" is any folder you pick (for samples not
    meant for one project). Each phone writes into its own sub-folder there, and the place
    shows how many new samples have arrived since you last looked. */
class Browser : public juce::Component, private juce::FileBrowserListener, private juce::Timer
{
public:
    Browser (te::Engine& e, const juce::File& projectFolder) : engine (e)
    {
        previewToggle.setClickingTogglesState (true);
        previewToggle.setToggleState (true, juce::dontSendNotification);
        previewToggle.setTooltip ("Preview audio files when you click them");
        previewToggle.onClick = [this] { if (! previewToggle.getToggleState()) stopPreview(); };
        addAndMakeVisible (previewToggle);

        previewPlayer.setSource (&previewTransport);
        engine.getDeviceManager().deviceManager.addAudioCallback (&previewPlayer);

        places.push_back ({ "Project", projectFolder });
        places.push_back ({ "Somewhere", projectFolder.getChildFile ("Samples/Somewhere"), true });

        auto inbox = juce::File (props().getValue ("somewhereInbox"));
        if (inbox.isDirectory())
            places.push_back ({ "Somewhere Inbox", inbox, true });

        places.push_back ({ "Music", juce::File::getSpecialLocation (juce::File::userMusicDirectory) });
        places.push_back ({ "Desktop", juce::File::getSpecialLocation (juce::File::userDesktopDirectory) });
        places.push_back ({ "Downloads", juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads") });
        places.push_back ({ "Home", juce::File::getSpecialLocation (juce::File::userHomeDirectory) });

        auto dropbox = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Dropbox");
        if (dropbox.isDirectory())
            places.push_back ({ "Dropbox", dropbox });

        for (auto& p : places)
            p.seenUntil = juce::Time (props().getValue ("somewhereSeen:" + p.folder.getFullPathName()).getLargeIntValue());

        rebuildPlaceBox();
        placeBox.onChange = [this] { placeChosen(); };
        addAndMakeVisible (placeBox);

        thread.startThread (juce::Thread::Priority::low);
        contents.setDirectory (projectFolder, true, true);
        tree.setDragAndDropDescription (browserDragDescription);
        tree.addListener (this);
        tree.setItemHeight (20);
        addAndMakeVisible (tree);

        placeBox.setSelectedId (1, juce::dontSendNotification);
        checkSomewhere();
        startTimer (3000);
    }

    ~Browser() override
    {
        tree.removeListener (this);
        engine.getDeviceManager().deviceManager.removeAudioCallback (&previewPlayer);
        previewTransport.setSource (nullptr);
        previewPlayer.setSource (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panel);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("BROWSER", getLocalBounds().removeFromTop (24).reduced (8, 0), juce::Justification::centredLeft);
        g.setColour (Colours::outline);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedRight (1);
        r.removeFromTop (24);
        auto top = r.removeFromTop (26).reduced (4, 2);
        previewToggle.setBounds (top.removeFromRight (64));
        top.removeFromRight (4);
        placeBox.setBounds (top);
        tree.setBounds (r.reduced (2));
    }

    /** Called when a file is double-clicked (e.g. to add it to the selected track). */
    std::function<void (const juce::File&)> onFileChosen;

private:
    struct Place
    {
        juce::String name;
        juce::File folder;
        bool isSomewhere = false;
        juce::Time seenUntil;       // Somewhere places: samples newer than this count as new
        int newCount = 0, fileCount = 0;
    };

    std::vector<Place> places;
    int currentPlace = 0;
    static constexpr int chooseInboxId = 1000;
    std::unique_ptr<juce::FileChooser> inboxChooser;

    juce::PropertiesFile& props()   { return engine.getPropertyStorage().getPropertiesFile(); }

    void rebuildPlaceBox()
    {
        placeBox.clear (juce::dontSendNotification);

        for (size_t i = 0; i < places.size(); ++i)
        {
            auto& p = places[i];
            placeBox.addItem (p.newCount > 0 ? p.name + " (" + juce::String (p.newCount) + " new)" : p.name, (int) i + 1);
        }

        placeBox.addSeparator();
        placeBox.addItem ("Choose Somewhere Inbox...", chooseInboxId);
        placeBox.setSelectedId (currentPlace + 1, juce::dontSendNotification);
    }

    void placeChosen()
    {
        if (placeBox.getSelectedId() == chooseInboxId)
        {
            placeBox.setSelectedId (currentPlace + 1, juce::dontSendNotification);
            chooseInbox();
            return;
        }

        currentPlace = placeBox.getSelectedId() - 1;
        auto& p = places[(size_t) currentPlace];

        if (p.isSomewhere)
            p.folder.createDirectory();

        setRoot (p.folder);
        markSeen (p);
    }

    void chooseInbox()
    {
        inboxChooser = std::make_unique<juce::FileChooser> ("Choose the folder Somewhere sends samples to",
                                                            juce::File::getSpecialLocation (juce::File::userHomeDirectory));
        inboxChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                   [this] (const juce::FileChooser& fc)
        {
            auto folder = fc.getResult();
            if (! folder.isDirectory())
                return;

            props().setValue ("somewhereInbox", folder.getFullPathName());
            auto existing = std::find_if (places.begin(), places.end(), [] (auto& p) { return p.name == "Somewhere Inbox"; });

            if (existing != places.end())
                existing->folder = folder;
            else
                places.insert (places.begin() + 2, { "Somewhere Inbox", folder, true });

            currentPlace = 2;
            checkSomewhere();
            placeChosen();
        });
    }

    void markSeen (Place& p)
    {
        if (! p.isSomewhere)
            return;

        p.seenUntil = juce::Time::getCurrentTime();
        p.newCount = 0;
        props().setValue ("somewhereSeen:" + p.folder.getFullPathName(), juce::String (p.seenUntil.toMilliseconds()));
        rebuildPlaceBox();
    }

    /** Counts samples that arrived in the Somewhere places, and refreshes the tree when you're looking at one. */
    void checkSomewhere()
    {
        bool labelsChanged = false;

        for (size_t i = 0; i < places.size(); ++i)
        {
            auto& p = places[i];
            if (! p.isSomewhere)
                continue;

            int total = 0, fresh = 0;
            for (auto& f : p.folder.findChildFiles (juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.caf;*.flac;*.mp3;*.m4a"))
            {
                ++total;
                if (f.getCreationTime() > p.seenUntil || f.getLastModificationTime() > p.seenUntil)
                    ++fresh;
            }

            const bool looking = (int) i == currentPlace;

            if (total != p.fileCount)
            {
                p.fileCount = total;

                if (looking)
                {
                    tree.refresh();
                    if (isShowing())
                        markSeen (p);   // arrived while you were looking at it
                }
            }

            if (looking && isShowing())
                fresh = 0;

            if (fresh != p.newCount)
            {
                p.newCount = fresh;
                labelsChanged = true;
            }
        }

        if (labelsChanged)
            rebuildPlaceBox();
    }

    void timerCallback() override    { checkSomewhere(); }

    struct AudioAndMidiFilter : public juce::FileFilter
    {
        AudioAndMidiFilter() : juce::FileFilter ("Audio & MIDI") {}
        bool isFileSuitable (const juce::File& f) const override        { return f.hasFileExtension (te::soundFileAndMidiExtensions) || f.hasFileExtension ("elsewhere;tandem"); }
        bool isDirectorySuitable (const juce::File& d) const override   { return ! d.isHidden() && ! d.getFileName().startsWithChar ('.'); }
    };

    juce::ComboBox placeBox;
    AudioAndMidiFilter filter;
    juce::TimeSliceThread thread { "Browser scanner" };
    juce::DirectoryContentsList contents { &filter, thread };
    juce::FileTreeComponent tree { contents };

    void setRoot (const juce::File& f)
    {
        contents.setDirectory (f, true, true);
        tree.refresh();
    }

    // Audio preview: an extra device callback mixed alongside the engine's output
    te::Engine& engine;
    juce::TextButton previewToggle { "Preview" };
    juce::AudioSourcePlayer previewPlayer;
    juce::AudioTransportSource previewTransport;
    std::unique_ptr<juce::AudioFormatReaderSource> previewSource;
    juce::TimeSliceThread previewThread { "Browser preview" };

    void startPreview (const juce::File& f)
    {
        stopPreview();

        auto* reader = engine.getAudioFileFormatManager().readFormatManager.createReaderFor (f);

        if (reader == nullptr)
            return;

        if (! previewThread.isThreadRunning())
            previewThread.startThread();

        auto rate = reader->sampleRate;
        previewSource = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
        previewTransport.setSource (previewSource.get(), 32768, &previewThread, rate, 2);
        previewTransport.setGain (0.7f);
        previewTransport.start();
    }

    void stopPreview()
    {
        previewTransport.stop();
        previewTransport.setSource (nullptr);
        previewSource.reset();
    }

    void selectionChanged() override {}

    void fileClicked (const juce::File& f, const juce::MouseEvent&) override
    {
        if (previewToggle.getToggleState() && f.existsAsFile() && f.hasFileExtension (te::soundFileExtensions))
            startPreview (f);
        else
            stopPreview();
    }
    void browserRootChanged (const juce::File&) override {}

    void fileDoubleClicked (const juce::File& f) override
    {
        if (onFileChosen && f.existsAsFile())
            onFileChosen (f);
    }
};

} // namespace elsewhere
