// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <JuceHeader.h>

namespace te = tracktion;
using namespace tracktion::literals;
using namespace std::literals;

namespace elsewhere
{
//==============================================================================
/** Small helpers around Tracktion Engine that the UI uses in many places. */
/** "Finder" on a Mac, "Explorer" on Windows: for "Show in ..." menu items. */
inline juce::String fileManagerName()
{
   #if JUCE_MAC
    return "Finder";
   #elif JUCE_WINDOWS
    return "Explorer";
   #else
    return "File Manager";
   #endif
}

namespace EngineHelpers
{
    inline void togglePlay (te::Edit& edit)
    {
        auto& transport = edit.getTransport();

        if (transport.isPlaying())
            transport.stop (false, false);
        else
            transport.play (false);
    }

    inline void stopOrReturnToZero (te::Edit& edit)
    {
        auto& transport = edit.getTransport();

        if (transport.isPlaying())
            transport.stop (false, false);
        else
            transport.setPosition (0s);
    }

    //==============================================================================
    /*  Punch and loop share the brace on the ruler: with punch on it marks where recording
        happens, and the engine can't do both at once, so turning one on turns the other off. */
    inline bool isPunching (te::Edit& edit)     { return edit.recordingPunchInOut.get(); }

    inline void setLooping (te::Edit& edit, bool loop)
    {
        if (loop)
            edit.recordingPunchInOut = false;

        edit.getTransport().looping = loop;
    }

    inline void setPunching (te::Edit& edit, bool punch)
    {
        if (punch)
            edit.getTransport().looping = false;

        edit.recordingPunchInOut = punch;
    }

    inline const juce::Identifier preRollProp { "elsewherePreRollBars" };

    /** Bars played before the punch-in when recording starts from stopped (0, 1 or 2). */
    inline int getPreRollBars (te::Edit& edit)              { return (int) edit.state.getProperty (preRollProp, 0); }
    inline void setPreRollBars (te::Edit& edit, int bars)   { edit.state.setProperty (preRollProp, bars, nullptr); }

    /** Where recording actually begins (after the count-in and any pre-roll), for the countdown. */
    inline te::TimePosition& recordStartTime()  { static te::TimePosition t; return t; }

    /*  When the audio callback runs late the engine mutes a buffer to catch up, and doesn't
        read the inputs for it either: that buffer is cut out of whatever is recording, and the
        rest of the take sits early. While recording, keeping every sample matters more than
        catching up, so the mute is off (the measured load never goes above 1). */
    inline void keepInputWhenOverloaded (te::Engine& engine, bool recording)
    {
        engine.getDeviceManager().setCpuLimitBeforeMuting (recording ? 1000.0 : 0.98);
    }

    inline void toggleRecord (te::Edit& edit)
    {
        auto& transport = edit.getTransport();

        if (transport.isRecording())
        {
            transport.stop (false, false);
            keepInputWhenOverloaded (edit.engine, false);
            return;
        }

        keepInputWhenOverloaded (edit.engine, true);

        auto pos = transport.getPosition();

        if (! transport.isPlaying())
        {
            auto range = transport.getLoopRange();

            if (transport.looping)
            {
                // Loop takes line up with the loop, so recording always starts at the top of it
                pos = range.getStart();
            }
            else if (isPunching (edit))
            {
                // Run up to the punch-in: the pre-roll if there is one, otherwise from the playhead
                // when it's before the punch-in (from the punch-in itself when it's past it)
                auto& ts = edit.tempoSequence;
                auto bars = getPreRollBars (edit);
                auto preRoll = ts.toTime (ts.toBeats (range.getStart()) - te::BeatDuration::fromBeats (bars * ts.getTimeSig (0)->numerator.get()));

                if (bars > 0)                       transport.setPosition (juce::jmax (te::TimePosition(), preRoll));
                else if (pos >= range.getStart())   transport.setPosition (range.getStart());
            }

            if (isPunching (edit))
                pos = range.getStart();
        }

        recordStartTime() = pos;
        transport.record (false, true);    // records Session launches and automation even with nothing armed
    }

    inline bool trackHasInput (te::AudioTrack& t)
    {
        for (auto instance : t.edit.getAllInputDevices())
            if (te::isOnTargetTrack (*instance, t, 0))
                return true;

        return false;
    }

    inline bool isTrackArmed (te::AudioTrack& t)
    {
        for (auto instance : t.edit.getAllInputDevices())
            if (te::isOnTargetTrack (*instance, t, 0))
                return instance->isRecordingEnabled (t.itemID);

        return false;
    }

    inline void armTrack (te::AudioTrack& t, bool arm)
    {
        for (auto instance : t.edit.getAllInputDevices())
            if (te::isOnTargetTrack (*instance, t, 0))
                instance->setRecordingEnabled (t.itemID, arm);
    }

    inline te::InputDeviceInstance* getInputOnTrack (te::AudioTrack& t)
    {
        for (auto instance : t.edit.getAllInputDevices())
            if (te::isOnTargetTrack (*instance, t, 0))
                return instance;

        return nullptr;
    }

    /** Routes the given input to a track (and only that track). */
    inline void assignInputToTrack (te::InputDeviceInstance& instance, te::AudioTrack& t)
    {
        [[maybe_unused]] auto res = instance.setTarget (t.itemID, true, &t.edit.getUndoManager(), 0);
    }

    inline void clearInputsFromTrack (te::AudioTrack& t)
    {
        for (auto instance : t.edit.getAllInputDevices())
            if (te::isOnTargetTrack (*instance, t, 0))
                [[maybe_unused]] auto res = instance->removeTarget (t.itemID, &t.edit.getUndoManager());
    }

    inline bool isInstrumentTrack (te::AudioTrack& t)
    {
        for (auto p : t.pluginList)
            if (p->isSynth())
                return true;

        return false;
    }

    /** Once a Session clip plays on a track, the track plays Session clips instead of
        its Arrangement clips until you go "back to arrangement". */
    inline bool isPlayingSessionClips (te::AudioTrack& t)   { return t.playSlotClips.get(); }

    inline void backToArrangement (te::AudioTrack& t)
    {
        for (auto slot : t.getClipSlotList().getClipSlots())
            if (auto c = slot->getClip())
                if (auto lh = c->getLaunchHandle())
                    lh->stop ({});

        t.playSlotClips = false;
    }

    inline bool anyTrackPlayingSessionClips (te::Edit& edit)
    {
        for (auto t : te::getAudioTracks (edit))
            if (isPlayingSessionClips (*t))
                return true;

        return false;
    }

    inline juce::Colour trackColourFor (int index)
    {
        // Muted clip palette
        static const juce::uint32 palette[] = { 0xffff9a3c, 0xfff3d34a, 0xff9ad64b, 0xff4cc9a6,
                                                0xff4fb3ff, 0xff8f7cff, 0xffe06cd8, 0xffff6b6b,
                                                0xffc9a27a, 0xff7fd1c7, 0xffb5c95a, 0xff6c8cff };
        return juce::Colour (palette[(size_t) index % std::size (palette)]);
    }

    inline void removeFXPlugins (te::AudioTrack& track)
    {
        for (auto p : track.pluginList.getPlugins())
            if (p != track.getVolumePlugin() && p != track.getLevelMeterPlugin())
                p->removeFromParent();
    }

    inline te::Plugin::Ptr insertBuiltInPlugin (te::AudioTrack& track, const juce::String& xmlType, int index = -1)
    {
        if (auto p = track.edit.getPluginCache().createNewPlugin (xmlType, {}))
        {
            track.pluginList.insertPlugin (p, index, nullptr);
            return p;
        }

        return {};
    }

    /** Returns the user-visible plugins on a track, i.e. everything except the
        built-in volume/pan and meter that every track carries. */
    /** Plugins Elsewhere manages itself at the end of a chain: faders, meters, the live-session tap. */
    inline bool isMixerPlugin (te::Plugin& p)
    {
        return dynamic_cast<te::VolumeAndPanPlugin*> (&p) != nullptr
            || dynamic_cast<te::LevelMeterPlugin*> (&p) != nullptr
            || p.getPluginType() == "elsewhereLiveTap";
    }

    inline juce::Array<te::Plugin*> getUserPlugins (te::Track& track)
    {
        juce::Array<te::Plugin*> result;
        te::Plugin* mixerVolume = nullptr;

        if (auto at = dynamic_cast<te::AudioTrack*> (&track))
            mixerVolume = at->getVolumePlugin();
        else if (auto ft = dynamic_cast<te::FolderTrack*> (&track))
            mixerVolume = ft->getVolumePlugin();

        // Sends, returns, meters and the fader live in the mixer, not the device chain
        for (auto p : track.pluginList)
            if (p != mixerVolume
                 && dynamic_cast<te::LevelMeterPlugin*> (p) == nullptr
                 && p->getPluginType() != "elsewhereLiveTap"
                 && dynamic_cast<te::AuxSendPlugin*> (p) == nullptr
                 && dynamic_cast<te::AuxReturnPlugin*> (p) == nullptr)
                result.add (p);

        return result;
    }

    inline juce::String formatBarsBeats (te::Edit& edit, te::TimePosition t)
    {
        auto bb = edit.tempoSequence.toBarsAndBeats (t);
        auto beat = bb.getWholeBeats();
        auto sixteenths = (int) std::floor (bb.getFractionalBeats().inBeats() * 4.0);
        return juce::String (bb.bars + 1) + "." + juce::String (beat + 1) + "." + juce::String (sixteenths + 1);
    }

    /** Turns an engine extension list ("wav;aif;mid") into a file-chooser pattern ("*.wav;*.aif;*.mid"). */
    inline juce::String toWildcards (const juce::String& extensions)
    {
        auto list = juce::StringArray::fromTokens (extensions, ";", {});
        list.removeEmptyStrings();

        for (auto& e : list)
            e = "*." + e.trim().trimCharactersAtStart ("*.");

        return list.joinIntoString (";");
    }

    inline juce::String formatTime (te::TimePosition t)
    {
        auto ms = juce::roundToInt (t.inSeconds() * 1000.0);
        return juce::String::formatted ("%d:%02d.%03d", ms / 60000, (ms / 1000) % 60, ms % 1000);
    }
}

//==============================================================================
/** Things that need to reach up to the application (e.g. to open another project). */
struct AppActions
{
    virtual ~AppActions() = default;
    virtual void newProject() = 0;
    virtual void openProject() = 0;
    virtual void openProject (const juce::File&) = 0;
    virtual void saveProject() = 0;
    virtual void saveProjectAs() = 0;
    virtual void exportAudio() = 0;
    virtual void exportSeveralProjects() = 0;
    virtual void importSession() = 0;
    virtual void exportDawProject() = 0;
    virtual void showAudioSettings() = 0;
    virtual void showPluginManager() = 0;
    virtual void showControlSurfaces() = 0;
};

} // namespace elsewhere
