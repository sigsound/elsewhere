// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "LegacyNames.h"

namespace elsewhere
{
//==============================================================================
/**
    An Elsewhere project is a self-contained folder, so it can live in Dropbox / iCloud Drive
    and be shared as-is:

        My Song Project/
            My Song.elsewhere          <- the arrangement (Tracktion Edit XML)
            Samples/Imported/       <- audio dragged in is copied here
            Samples/Recorded/       <- new recordings land here
            Backups/                <- timestamped copies made on every save
            .elsewhere-lock            <- who currently has the project open

    All audio references inside the .elsewhere file are relative, so the folder can be
    moved between machines without breaking.
*/
class Project
{
public:
    static constexpr const char* fileExtension = ".elsewhere";
    static constexpr const char* lockFileName  = ".elsewhere-lock";
    static constexpr int lockStaleMinutes = 3;

    //==============================================================================
    static juce::File getDefaultProjectsFolder()
    {
        auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        return Legacy::preferExisting (music.getChildFile ("Elsewhere Projects"), music.getChildFile ("Tandem Projects"));
    }

    /** Creates the folder layout for a new project and returns the edit file inside it. */
    static juce::File createProjectFolder (juce::File parentDir, const juce::String& name)
    {
        auto folder = parentDir.getNonexistentChildFile (name + " Project", {}, false);
        folder.createDirectory();
        auto editFile = folder.getChildFile (juce::File::createLegalFileName (name) + fileExtension);
        createSubfolders (folder);
        return editFile;
    }

    static void createSubfolders (const juce::File& folder)
    {
        folder.getChildFile ("Samples/Imported").createDirectory();
        folder.getChildFile ("Samples/Recorded").createDirectory();
        folder.getChildFile ("Samples/Somewhere").createDirectory();   // samples sent from Somewhere (iPhone)
        folder.getChildFile ("Backups").createDirectory();
    }

    //==============================================================================
    Project (te::Engine& e, const juce::File& file) : engine (e), editFile (file)
    {
        createSubfolders (getFolder());

        // Shared projects may have fresher (or intact) per-user snapshots than the main file
        auto source = chooseFileToLoad (editFile);

        if (source.existsAsFile())
            edit = loadEdit (engine, source);
        else
            edit = te::createEmptyEdit (engine, editFile);

        edit->alwaysUseRelativePaths = true;
        edit->playInStopEnabled = true;
        edit->editFileRetriever = [f = editFile] { return f; };
        edit->filePathResolver = [f = editFile] (const juce::String& path) -> juce::File
        {
            if (juce::File::isAbsolutePath (path))
                return juce::File (path);

            // The engine writes paths relative to the edit file via File::getRelativePathFrom,
            // which gives "Samples/x.wav" if the edit file exists on disk but "../Samples/x.wav"
            // if it doesn't yet. Accept both forms.
            auto fromFolder = f.getParentDirectory().getChildFile (path);

            if (fromFolder.exists())
                return fromFolder;

            auto fromFile = f.getChildFile (path);
            return fromFile.exists() ? fromFile : fromFolder;
        };

        // Master bus gets a meter so the UI can show output levels
        edit->ensureMasterTrack();
        bool hasMasterMeter = false;

        for (auto p : edit->getMasterPluginList())
            hasMasterMeter = hasMasterMeter || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr;

        if (! hasMasterMeter)
            if (auto meter = edit->getPluginCache().createNewPlugin (te::LevelMeterPlugin::xmlTypeName, {}))
                edit->getMasterPluginList().insertPlugin (meter, -1, nullptr);

        // Automation arm starts off: nothing records until you ask it to
        edit->getAutomationRecordManager().setWritingAutomation (false);

        edit->getUndoManager().clearUndoHistory();
        acquireLock();

        // Write new projects to disk straight away so audio paths are always stored
        // the same way ("Samples/...", relative to the project folder)
        if (! editFile.existsAsFile())
            te::EditFileOperations (*edit).save (true, true, false);
    }

    ~Project()
    {
        releaseLock();
    }

    te::Edit& getEdit()                 { return *edit; }

    bool nameImportsByContent = false;

    /** Short fingerprint of a file's contents (64-bit FNV-1a), used in shared-project file names. */
    static juce::String hashFileContents (const juce::File& f)
    {
        juce::FileInputStream in (f);
        juce::uint64 h = 14695981039346656037ull;
        juce::HeapBlock<char> buffer (65536);

        while (in.openedOk() && ! in.isExhausted())
        {
            auto n = in.read (buffer, 65536);

            for (int i = 0; i < n; ++i)
                h = (h ^ (juce::uint8) buffer[i]) * 1099511628211ull;
        }

        return juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16).substring (0, 10);
    }

    /** Quick check of a project file's collaboration flag, without loading it. */
    static bool isSharedProjectFile (const juce::File& f)
    {
        juce::FileInputStream in (f);
        auto head = in.openedOk() ? in.readString().substring (0, 2000) : juce::String();
        return head.contains ("elsewhereCollab=\"1\"") || head.contains ("tandemCollab=\"1\"");
    }
    juce::File getEditFile() const      { return editFile; }
    juce::File getFolder() const        { return editFile.getParentDirectory(); }
    juce::String getName() const        { return editFile.getFileNameWithoutExtension(); }

    bool hasChangedSinceSaved() const   { return edit->hasChangedSinceSaved(); }

    //==============================================================================
    /** Called just before saving (the collaboration session flushes its changes here). */
    std::function<void()> beforeSave;

    /** Called after saving (the collaboration session writes its own snapshot here). */
    std::function<void()> afterSave;

    /**
        Picks what to load for a project. Normally the main file, but in a shared project each
        collaborator also keeps a snapshot in Collab/snapshots/, since two people saving the main
        file at once can leave a conflicted or damaged copy. The intact file that has the most
        collaboration changes applied wins; the rest are replayed from the change logs.
    */
    static juce::File chooseFileToLoad (const juce::File& editFile)
    {
        juce::Array<juce::File> candidates { editFile };
        candidates.addArray (editFile.getParentDirectory().getChildFile ("Collab/snapshots")
                               .findChildFiles (juce::File::findFiles, false, "*" + editFile.getFileExtension()));

        juce::File best;
        juce::int64 bestScore = -1;

        for (auto f : candidates)
        {
            if (! f.existsAsFile())
                continue;

            auto xml = juce::XmlDocument::parse (f);

            if (xml == nullptr || ! xml->hasTagName ("EDIT"))
                continue;   // damaged or half-synced

            juce::int64 score = 0;

            if (auto* clock = juce::JSON::parse (Legacy::getAttribute (*xml, "elsewhereCollabClock")).getDynamicObject())
                for (auto& p : clock->getProperties())
                    score += (juce::int64) p.value;

            // Prefer the main file when it's just as up to date
            if (score > bestScore || (score == bestScore && f == editFile))
            {
                best = f;
                bestScore = score;
            }
        }

        return best;
    }

    /** Loads an edit file, translating names saved before the rename (see LegacyNames.h). */
    static std::unique_ptr<te::Edit> loadEdit (te::Engine& engine, const juce::File& file)
    {
        auto state = te::loadEditFromFile (engine, file, te::ProjectItemID{});

        if (! state.isValid())
            return {};

        return te::Edit::createEdit ({ engine, Legacy::upgradeIdentifiers (state), te::ProjectItemID::createNewID (te::ProjectID{}),
                                       te::Edit::forEditing, nullptr, te::Edit::getDefaultNumUndoLevels(),
                                       [file] { return file; }, {} });
    }

    /** Who saved, recorded in the file for Version History. */
    juce::String savedByName;

    /** Returns whether the project was written. warnOfFailure shows the engine's own message
        when it wasn't; pass false to show your own. */
    bool save (bool warnOfFailure = true)
    {
        if (beforeSave)
            beforeSave();

        if (savedByName.isNotEmpty())
            edit->state.setProperty ("elsewhereSavedBy", savedByName, nullptr);

        makeBackup();
        // The answer comes back before save() returns, unless the engine is showing its own
        // failure message (which answers later, and then only "not saved" matters)
        auto written = std::make_shared<bool> (false);
        te::EditFileOperations (*edit).save (warnOfFailure, true, false, [written] (bool r) { *written = r; });
        auto ok = *written;

        if (afterSave)
            afterSave();

        lastSaveTime = juce::Time::getCurrentTime();
        return ok;
    }

    /** Copies the whole project folder (with all its audio) to a new location. Audio from outside
        the folder should be collected into it first (ProjectAudio::collectExternal), or its paths
        break in the copy. */
    std::optional<juce::File> saveCopyAs (juce::File newParentDir, const juce::String& newName)
    {
        save();

        // Saving "Song" from inside "Song Project/" would nest a new "Song Project/" inside it
        if (newParentDir.getFileName() == newName + " Project")
            newParentDir = newParentDir.getParentDirectory();

        auto newEditFile = createProjectFolder (newParentDir, newName);
        auto newFolder = newEditFile.getParentDirectory();

        // All the audio: recordings, imports, Somewhere takes, bounces...
        getFolder().getChildFile ("Samples").copyDirectoryTo (newFolder.getChildFile ("Samples"));

        if (! editFile.copyFileTo (newEditFile))
            return std::nullopt;

        return newEditFile;
    }

    //==============================================================================
    /** Copies an external audio file into the project so the project stays self-contained. */
    juce::File importAudioFile (const juce::File& source)
    {
        if (source.isAChildOf (getFolder()))
            return source;

        auto importDir = getFolder().getChildFile ("Samples/Imported");
        importDir.createDirectory();
        auto dest = importDir.getChildFile (source.getFileName());

        // Shared projects: name files by content, so two people importing different files
        // with the same name at the same time can't overwrite each other (and identical
        // files are only stored once)
        if (nameImportsByContent)
        {
            auto hash = hashFileContents (source);
            dest = importDir.getChildFile (source.getFileNameWithoutExtension() + "-" + hash + source.getFileExtension());

            if (! dest.existsAsFile())
                source.copyFileTo (dest);

            return dest;
        }

        // Same name but different content -> keep both
        if (dest.existsAsFile() && dest.getSize() != source.getSize())
            dest = importDir.getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), false);

        if (! dest.existsAsFile())
            source.copyFileTo (dest);

        return dest;
    }

    //==============================================================================
    // Collaboration lock: tells a friend sharing the Dropbox folder that someone has it open.
    struct LockInfo
    {
        juce::String user, machine;
        juce::Time heartbeat;
        bool isOurs = false;
    };

    std::optional<LockInfo> getForeignLock() const    { return readForeignLock (getFolder()); }

    /** Returns details of someone else's recent lock on a project folder, if there is one. */
    static std::optional<LockInfo> readForeignLock (const juce::File& folder)
    {
        auto f = folder.getChildFile (lockFileName);

        if (! f.existsAsFile())
            return std::nullopt;

        auto json = juce::JSON::parse (f.loadFileAsString());

        LockInfo info;
        info.user = json["user"].toString();
        info.machine = json["machine"].toString();
        info.heartbeat = juce::Time ((juce::int64) json["heartbeat"]);
        info.isOurs = info.machine == juce::SystemStats::getComputerName()
                        && info.user == juce::SystemStats::getFullUserName();

        if (info.isOurs)
            return std::nullopt;

        if (juce::Time::getCurrentTime() - info.heartbeat > juce::RelativeTime::minutes (lockStaleMinutes))
            return std::nullopt;

        return info;
    }

    void refreshLock()          { writeLock(); }

    /** Dropbox names conflicting edits "X (Someone's conflicted copy 2024-01-01).elsewhere". */
    juce::Array<juce::File> findConflictedCopies() const
    {
        juce::Array<juce::File> result;

        for (auto f : getFolder().findChildFiles (juce::File::findFiles, false, "*" + editFile.getFileExtension()))
            if (f != editFile && f.getFileName().containsIgnoreCase ("conflict"))
                result.add (f);

        return result;
    }

private:
    te::Engine& engine;
    juce::File editFile;
    std::unique_ptr<te::Edit> edit;
    juce::Time lastSaveTime;

    void makeBackup()
    {
        if (! editFile.existsAsFile())
            return;

        auto backups = getFolder().getChildFile ("Backups");
        backups.createDirectory();
        auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
        editFile.copyFileTo (backups.getChildFile (getName() + " " + stamp + editFile.getFileExtension()));

        // Keep the 30 most recent backups
        auto files = backups.findChildFiles (juce::File::findFiles, false, "*" + editFile.getFileExtension());
        files.sort();

        for (int i = 0; i < files.size() - 30; ++i)
            files.getReference (i).deleteFile();
    }

    void writeLock()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("user", juce::SystemStats::getFullUserName());
        o->setProperty ("machine", juce::SystemStats::getComputerName());
        o->setProperty ("heartbeat", juce::Time::getCurrentTime().toMilliseconds());
        o->setProperty ("app", juce::String ("Elsewhere ") + ELSEWHERE_VERSION);
        getFolder().getChildFile (lockFileName).replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }

    void acquireLock()
    {
        if (! getForeignLock())
            writeLock();
    }

    void releaseLock()
    {
        auto f = getFolder().getChildFile (lockFileName);

        if (f.existsAsFile() && ! getForeignLock())
            f.deleteFile();
    }
};

//==============================================================================
/** Engine-wide behaviour tweaks: where recordings go, etc. */
class ElsewhereEngineBehaviour : public te::EngineBehaviour
{
public:
    /** Launching with --no-audio skips opening the audio device (for testing, or when CoreAudio is misbehaving). */
    explicit ElsewhereEngineBehaviour (bool openAudioDevice = true) : openAudio (openAudioDevice) {}

    bool autoInitialiseDeviceManager() override     { return openAudio; }

    /** Added to recording file names so collaborators' takes never clash. */
    static juce::String& recordingTag()     { static juce::String tag; return tag; }

    juce::String getDefaultAudioRecordingFilePattern() override
    {
        auto tag = recordingTag();
        return "%projectdir%/Samples/Recorded/%track%_Take_%take%" + (tag.isNotEmpty() ? "_" + tag : juce::String());
    }

    bool autoAddClipEdgeFades() override    { return true; }

    /** Mackie Control, the protocol most fader surfaces speak (set up in Sync & Control Surfaces). */
    ControlSurfaces getDesiredControlSurfaces() override
    {
        ControlSurfaces c;
        c.mackieC4 = c.iconProG2 = c.tranzport = c.alphaTrack = c.remoteSL = c.remoteSLCompact = c.automap = false;
        return c;
    }

    /** Elsewhere's own plugins (the live-session tap); implemented in Live.cpp. */
    te::Plugin::Ptr createCustomPlugin (te::PluginCreationInfo) override;

    /** With no saved setup, open output only: combining the Mac's separate built-in mic and
        speakers into one device can deadlock CoreAudio at startup. Once an interface is chosen
        in Audio Settings its saved setup (with inputs) is restored instead. */
    bool shouldOpenAudioInputByDefault() override   { return false; }

    /** Exposes every interface input as a mono input (plus stereo pairs), which is what you
        want on a multi-input interface like a Scarlett 18i20: "1 Mic", "2 Guitar", "1/2" ... */
    bool isDescriptionOfWaveDevicesSupported() override     { return true; }

    void describeWaveDevices (std::vector<te::WaveDeviceDescription>& devices, juce::AudioIODevice& device, bool isInput) override
    {
        devices.clear();
        auto names = isInput ? device.getInputChannelNames() : device.getOutputChannelNames();
        auto label = [&names] (int i)
        {
            auto n = names[i].trim();
            return juce::String (i + 1) + (n.isNotEmpty() && n != juce::String (i + 1) ? " " + n : juce::String());
        };

        if (isInput)
        {
            for (int i = 0; i < names.size(); ++i)
                devices.push_back (te::WaveDeviceDescription::withNumChannels ("In " + label (i), (uint32_t) i, 1, true));

            for (int i = 0; i + 1 < names.size(); i += 2)
                devices.push_back (te::WaveDeviceDescription::withNumChannels ("In " + juce::String (i + 1) + "/" + juce::String (i + 2),
                                                                               (uint32_t) i, 2, true));
        }
        else
        {
            for (int i = 0; i + 1 < names.size(); i += 2)
                devices.push_back (te::WaveDeviceDescription::withNumChannels ("Out " + juce::String (i + 1) + "/" + juce::String (i + 2),
                                                                               (uint32_t) i, 2, true));

            if (names.size() % 2 == 1)
                devices.push_back (te::WaveDeviceDescription::withNumChannels ("Out " + label (names.size() - 1),
                                                                               (uint32_t) names.size() - 1, 1, true));
        }
    }

private:
    bool openAudio = true;
};

} // namespace elsewhere
