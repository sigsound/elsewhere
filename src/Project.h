// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "LegacyNames.h"
#include "History.h"

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
            .My Song.history        <- every saved version, compressed (Version History)
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
    }

    //==============================================================================
    /** With recover, opens the unsaved changes kept from a session that didn't close properly
        (see hasRecovery) in place of the saved file. */
    Project (te::Engine& e, const juce::File& file, bool recover = false) : engine (e), editFile (file)
    {
        createSubfolders (getFolder());
        History::importBackups (editFile);     // projects from before the history file

        // Shared projects may have fresher (or intact) per-user snapshots than the main file
        auto source = chooseFileToLoad (editFile);
        auto recovery = getRecoveryFile (engine, editFile);

        if (recover && isReadable (recovery))
        {
            source = recovery;
            openedFromRecovery = true;
        }
        else
        {
            // Left by a session that didn't close properly: kept until the question is answered
            recoveryPending = recovery.existsAsFile();
        }

        if (source.existsAsFile())
            edit = loadEdit (engine, source);

        if (edit == nullptr)
        {
            // A file is there but can't be read (damaged, or half synced). Showing an empty
            // project is all that's left, and it must never be saved over what's on disk
            unreadable = editFile.existsAsFile();
            edit = te::createEmptyEdit (engine, editFile);
        }

        usePathsBeside (*edit, editFile);
        edit->playInStopEnabled = true;

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

        if (openedFromRecovery)
            edit->markAsChanged();   // not on disk until it's saved

        changes = std::make_unique<ChangeWatcher> (edit->state);

        // Write new projects to disk straight away so audio paths are always stored
        // the same way ("Samples/...", relative to the project folder)
        if (! editFile.existsAsFile())
            te::EditFileOperations (*edit).save (true, true, false);
    }

    ~Project()
    {
        // Closed properly: whatever wasn't saved was left unsaved on purpose
        if (! recoveryPending)
            getRecoveryFile (engine, editFile).deleteFile();

        releaseLock();
    }

    //==============================================================================
    // Recovery: while a project has unsaved changes a copy of it is kept outside the project
    // folder (so it's this computer's alone, and nothing extra syncs), written every so often
    // and deleted when the project is saved or closed. One that's still there on opening
    // means Elsewhere didn't close properly.

    static juce::File getRecoveryFile (te::Engine& engine, const juce::File& editFile)
    {
        auto id = juce::String::toHexString (editFile.getFullPathName().hashCode64());
        return engine.getPropertyStorage().getAppPrefsFolder().getChildFile ("Recovery")
                 .getChildFile (editFile.getFileNameWithoutExtension() + " " + id + fileExtension);
    }

    /** True if the file holds a project (and isn't cut short or half synced). */
    static bool isReadable (const juce::File& f)
    {
        if (! f.existsAsFile())
            return false;

        auto xml = juce::XmlDocument::parse (f);
        return xml != nullptr && xml->hasTagName ("EDIT");
    }

    /** True if the project file is there but nothing in it (or its shared snapshots) can be read. */
    static bool isDamaged (const juce::File& editFile)
    {
        return editFile.existsAsFile() && ! chooseFileToLoad (editFile).existsAsFile();
    }

    /** When unsaved changes from a session that didn't close properly are waiting, the time
        they were last written. */
    static std::optional<juce::Time> getRecoveryTime (te::Engine& engine, const juce::File& editFile)
    {
        auto f = getRecoveryFile (engine, editFile);

        if (! isReadable (f))
            return std::nullopt;

        return f.getLastModificationTime();
    }

    /** True while an earlier session's unsaved changes are waiting for an answer. */
    bool hasRecoveryPending() const     { return recoveryPending; }

    /** Opened as an empty project because the file couldn't be read: it won't save. */
    bool isUnreadable() const           { return unreadable; }

    /** The answer was "open the last saved one": the unsaved changes go into the history, where
        Version History finds them, and this session keeps its own recovery copy from now on. */
    void setRecoveryAside()
    {
        if (! recoveryPending)
            return;

        auto f = getRecoveryFile (engine, editFile);
        History::addFile (editFile, f, History::Kind::unsaved, f.getLastModificationTime());
        f.deleteFile();

        recoveryPending = false;
    }

    /** Call regularly: writes the recovery copy when there are unsaved changes it doesn't have yet.
        Returns true if it wrote one. */
    bool updateRecovery()
    {
        if (recoveryPending || unreadable || changes == nullptr || ! changes->changed
             || ! edit->hasChangedSinceSaved() || edit->isSaveInhibited())
            return false;

        edit->flushState();   // devices and plug-ins put their settings into the project
        auto xml = edit->state.createXml();
        auto file = getRecoveryFile (engine, editFile);
        file.getParentDirectory().createDirectory();
        juce::TemporaryFile temp (file);

        if (xml == nullptr || ! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
            return false;

        changes->changed = false;
        return true;
    }

    /** For a damaged project file: puts the newest saved version that can be read in its place,
        keeping the damaged file in the history. Returns when that version was saved. */
    static std::optional<juce::Time> restoreLatestBackup (const juce::File& editFile)
    {
        auto list = History::read (editFile);

        for (int i = list.size(); --i >= 0;)
        {
            // Saved versions only: changes that were set aside unsaved stay in Version History
            auto& c = list.getReference (i);

            if (c.kind != History::Kind::saved)
                continue;

            auto content = History::load (c);
            auto xml = content.isEmpty() ? nullptr : juce::parseXML (content.toString());

            if (xml == nullptr || ! xml->hasTagName ("EDIT"))
                continue;

            if (editFile.existsAsFile() && ! History::addFile (editFile, editFile, History::Kind::damaged))
                return std::nullopt;

            juce::TemporaryFile temp (editFile);

            if (! temp.getFile().replaceWithData (content.getData(), content.getSize()) || ! temp.overwriteTargetFileWithTemporary())
                return std::nullopt;

            return c.time;
        }

        return std::nullopt;
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

    /** Points the edit's relative audio paths at the project folder `file` is in. */
    static void usePathsBeside (te::Edit& edit, const juce::File& file)
    {
        edit.alwaysUseRelativePaths = true;
        edit.editFileRetriever = [f = file] { return f; };
        edit.filePathResolver = [f = file] (const juce::String& path) -> juce::File
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
    }

    /** Opens a project's saved arrangement on its own, to render it: no lock, no backups,
        nothing written. Null if it can't be read. */
    static std::unique_ptr<te::Edit> openForRendering (te::Engine& engine, const juce::File& file)
    {
        auto source = chooseFileToLoad (file);

        if (! isReadable (source))
            return {};

        auto edit = loadEdit (engine, source);

        if (edit != nullptr)
        {
            usePathsBeside (*edit, file);
            edit->ensureMasterTrack();
        }

        return edit;
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
        if (unreadable)
            return false;   // what's on disk may be the only copy of the real project

        if (beforeSave)
            beforeSave();

        if (savedByName.isNotEmpty())
            edit->state.setProperty ("elsewhereSavedBy", savedByName, nullptr);

        // Someone else's save (a synced folder), or one from before the history held the latest
        // save: keep it before it's replaced
        if (editFile.getLastModificationTime() != backedUpModTime && ! isInHistory())
            addToHistory();

        // The answer comes back before save() returns, unless the engine is showing its own
        // failure message (which answers later, and then only "not saved" matters)
        auto written = std::make_shared<bool> (false);
        te::EditFileOperations (*edit).save (warnOfFailure, true, false, [written] (bool r) { *written = r; });
        auto ok = *written;

        if (afterSave)
            afterSave();

        if (ok)
        {
            // The history always holds what was last saved, so a project file that gets damaged
            // loses nothing
            addToHistory();

            if (! recoveryPending)
                getRecoveryFile (engine, editFile).deleteFile();

            if (changes != nullptr)
                changes->changed = false;
        }

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
        auto others = readForeignLocks (folder);
        return others.empty() ? std::nullopt : std::optional<LockInfo> (others.front());
    }

    /** Everyone else who has the project open. The lock file names whoever wrote it last and,
        under "others", everyone that writer could see, so two machines taking turns to write it
        don't hide each other (versions before this only wrote, and only read, the first). */
    static std::vector<LockInfo> readForeignLocks (const juce::File& folder)
    {
        std::vector<LockInfo> result;
        auto f = folder.getChildFile (lockFileName);

        if (! f.existsAsFile())
            return result;

        auto json = juce::JSON::parse (f.loadFileAsString());

        auto add = [&result] (const juce::var& entry)
        {
            LockInfo info;
            info.user = entry["user"].toString();
            info.machine = entry["machine"].toString();
            info.heartbeat = juce::Time ((juce::int64) entry["heartbeat"]);
            info.isOurs = info.machine == juce::SystemStats::getComputerName()
                            && info.user == juce::SystemStats::getFullUserName();

            if (info.isOurs || info.machine.isEmpty()
                 || juce::Time::getCurrentTime() - info.heartbeat > juce::RelativeTime::minutes (lockStaleMinutes))
                return;

            for (auto& existing : result)
                if (existing.machine == info.machine && existing.user == info.user)
                    return;

            result.push_back (info);
        };

        add (json);

        if (auto* others = json["others"].getArray())
            for (auto& o : *others)
                add (o);

        return result;
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
    bool recoveryPending = false, openedFromRecovery = false, unreadable = false;
    juce::Time backedUpModTime;     // the project file as it was when it last went into the history

    /** Notes that something in the project changed, so the recovery copy is only rewritten
        when there's something new to keep. */
    struct ChangeWatcher : private juce::ValueTree::Listener
    {
        explicit ChangeWatcher (const juce::ValueTree& v) : state (v)     { state.addListener (this); }
        ~ChangeWatcher() override                                         { state.removeListener (this); }

        bool changed = false;

    private:
        juce::ValueTree state;

        void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override   { changed = true; }
        void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override               { changed = true; }
        void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override        { changed = true; }
        void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override                { changed = true; }
    };

    std::unique_ptr<ChangeWatcher> changes;

    /** True if the history already holds exactly what's in the project file (as it does when
        the last session's final save is being opened again). */
    bool isInHistory() const
    {
        juce::MemoryBlock content;
        return editFile.loadFileAsData (content) && History::contains (editFile, History::hashOf (content.getData(), content.getSize()));
    }

    /** Adds what's in the project file to the history, as a saved version. */
    void addToHistory()
    {
        juce::MemoryBlock content;

        if (! editFile.loadFileAsData (content))
            return;

        // Saving with nothing changed adds nothing
        if (! History::isLatest (editFile, content))
            History::add (editFile, content, History::Kind::saved);

        backedUpModTime = editFile.getLastModificationTime();
    }

    void writeLock()
    {
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("user", juce::SystemStats::getFullUserName());
        o->setProperty ("machine", juce::SystemStats::getComputerName());
        o->setProperty ("heartbeat", juce::Time::getCurrentTime().toMilliseconds());
        o->setProperty ("app", juce::String ("Elsewhere ") + ELSEWHERE_VERSION);

        // Keep everyone else who's here in the file, with the time they last said so
        juce::Array<juce::var> others;

        for (auto& other : readForeignLocks (getFolder()))
        {
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty ("user", other.user);
            e->setProperty ("machine", other.machine);
            e->setProperty ("heartbeat", other.heartbeat.toMilliseconds());
            others.add (juce::var (e.get()));
        }

        if (! others.isEmpty())
            o->setProperty ("others", others);

        getFolder().getChildFile (lockFileName).replaceWithText (juce::JSON::toString (juce::var (o.get())));
    }

    void acquireLock()      { writeLock(); }

    void releaseLock()
    {
        auto f = getFolder().getChildFile (lockFileName);

        if (! f.existsAsFile())
            return;

        // Someone else is still here: leave the file to them (their next heartbeat drops us)
        if (readForeignLocks (getFolder()).empty())
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
