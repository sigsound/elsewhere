// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Theme.h"
#include "Mixing.h"
#include "Routing.h"
#include "RightsNotice.h"

namespace elsewhere
{
//==============================================================================
/**
    Exporting: the mix, stems, or both, in WAV, AIFF, FLAC, MP3 or Ogg Vorbis.

    Stems are one file per track that's playing in the mix, numbered in track order, all
    starting at the same point so they line up in any DAW. Each keeps whatever feeds its
    sound (a sidechain source, say) and comes from before the master fader and its effects,
    so together the stems are the mix as the master track receives it. Effects on return tracks come out as their own
    stems. Loudness targets apply to the mix only, as raising each stem would change the balance.

    The engine does the rendering (Tracktion's RenderSpecification and RenderQueue); this
    decides what to render and shows it happening.
*/
namespace Export
{
    enum class What { mix, stems, mixAndStems };
    enum class Range { song, loop };
    enum class Loudness { off, peak, streaming, apple, broadcast };

    struct Settings
    {
        What what = What::mix;
        Range range = Range::song;
        te::RenderFormat format = te::RenderFormat::wav;
        int bitDepth = 24;              // WAV, AIFF, FLAC; 32 is floating point
        int quality = 0;                // MP3 and Ogg: an index into qualityChoices()
        double sampleRate = 48000.0;
        bool dither = true;             // for 16- and 24-bit
        Loudness loudness = Loudness::off;
        bool tails = true;
        bool reserveRights = true;      // the AI training notice (RightsNotice), in every file
        bool chosen = false;            // until the first export, the sample rate follows the interface

        bool hasBitDepth() const    { return format == te::RenderFormat::wav || format == te::RenderFormat::aiff || format == te::RenderFormat::flac; }
        bool canDither() const      { return hasBitDepth() && bitDepth < 32; }
    };

    inline Settings& lastSettings()     { static Settings s; return s; }

    //==============================================================================
    /** MP3 goes through the LAME encoder when it's installed (it's not built in). */
    inline juce::File findLame()
    {
       #if JUCE_WINDOWS
        const char* places[] = { "C:\\Program Files\\LAME\\lame.exe", "C:\\Program Files (x86)\\LAME\\lame.exe" };
       #else
        const char* places[] = { "/opt/homebrew/bin/lame", "/usr/local/bin/lame", "/usr/bin/lame" };
       #endif

        for (auto p : places)
            if (juce::File (p).existsAsFile())
                return juce::File (p);

        return {};
    }

    inline void registerMp3Encoder (te::Engine& engine)
    {
       #if JUCE_USE_LAME_AUDIO_FORMAT
        if (auto lame = findLame(); lame.existsAsFile())
            engine.getAudioFileFormatManager().addLameFormat (std::make_unique<juce::LAMEEncoderAudioFormat> (lame),
                                                              std::make_unique<juce::LAMEEncoderAudioFormat> (lame));
       #else
        juce::ignoreUnused (engine);
       #endif
    }

    inline bool canWriteMp3 (te::Engine& engine)    { return engine.getAudioFileFormatManager().getLameFormat() != nullptr; }

    struct Choice { juce::String name; int value; };

    /** The quality choices for MP3 and Ogg, best first, with the encoder's index for each. */
    inline std::vector<Choice> qualityChoices (te::RenderFormat f)
    {
        if (f == te::RenderFormat::mp3)
            return { { "320 kbps", 23 }, { "256 kbps", 22 }, { "192 kbps", 20 }, { "VBR, best", 0 } };

        if (f == te::RenderFormat::ogg)
            return { { "320 kbps", 9 }, { "256 kbps", 8 }, { "192 kbps", 6 }, { "128 kbps", 4 } };

        return {};
    }

    inline juce::String formatName (te::RenderFormat f)
    {
        switch (f)
        {
            case te::RenderFormat::wav:    return "WAV";
            case te::RenderFormat::aiff:   return "AIFF";
            case te::RenderFormat::flac:   return "FLAC";
            case te::RenderFormat::mp3:    return "MP3";
            case te::RenderFormat::ogg:    return "Ogg Vorbis";
            case te::RenderFormat::midi:   break;
        }

        return {};
    }

    inline juce::String extensionFor (te::RenderFormat f)
    {
        switch (f)
        {
            case te::RenderFormat::wav:    return ".wav";
            case te::RenderFormat::aiff:   return ".aif";
            case te::RenderFormat::flac:   return ".flac";
            case te::RenderFormat::mp3:    return ".mp3";
            case te::RenderFormat::ogg:    return ".ogg";
            case te::RenderFormat::midi:   break;
        }

        return ".mid";
    }

    /** "24-bit 48 kHz WAV", "320 kbps MP3". */
    inline juce::String describeFormat (const Settings& s)
    {
        auto khz = juce::String (s.sampleRate / 1000.0, s.sampleRate == 44100.0 || s.sampleRate == 88200.0 ? 1 : 0) + " kHz";

        if (s.hasBitDepth())
            return (s.bitDepth == 32 ? juce::String ("32-bit float") : juce::String (s.bitDepth) + "-bit") + " " + khz + " " + formatName (s.format);

        for (auto& q : qualityChoices (s.format))
            if (q.value == s.quality)
                return q.name + " " + formatName (s.format);

        return formatName (s.format);
    }

    //==============================================================================
    /** The tracks heard in the mix that have something to play: the stems. */
    inline juce::Array<te::AudioTrack*> getStemTracks (te::Edit& edit)
    {
        juce::Array<te::AudioTrack*> tracks;
        const bool anySolo = edit.areAnyTracksSolo();

        for (auto t : te::getAudioTracks (edit))
        {
            // Not muted, and soloed when anything is (as the engine decides what's heard)
            if (t->isMuted (true) || (anySolo && ! t->isSolo (true) && ! t->isSoloIsolate (true)))
                continue;

            // A track playing into a bus is in the bus's stem, and one with no output isn't heard
            if (t->getOutput().getDestinationTrack() != nullptr || t->getOutput().outputsToNone())
                continue;

            // Returns are fed by sends and buses by tracks, so they count without clips of their own
            if (t->getClips().isEmpty() && ! Mixing::isReturnTrack (*t) && Routing::getFeeders (*t).isEmpty())
                continue;

            tracks.add (t);
        }

        return tracks;
    }

    inline te::TimeRange getRange (te::Edit& edit, Range r)
    {
        auto& transport = edit.getTransport();
        return r == Range::loop ? transport.getLoopRange() : te::TimeRange (te::TimePosition(), edit.getLength());
    }

    /** What an export would write, and the engine's jobs to write it. */
    struct Plan
    {
        std::vector<te::PlannedRenderJob> jobs;
        juce::File folder;              // where the files go (the stems folder, for stems)
        juce::File mixFile;
        int numStems = 0;
        juce::StringArray problems;
        std::optional<RightsNotice::Notice> notice;     // written into each file once it's rendered
    };

    /** What happens to a file once it's rendered: the notice goes in. Returns why it couldn't. */
    inline juce::String finishFile (const Plan& p, const juce::File& f)
    {
        return p.notice.has_value() ? RightsNotice::write (f, *p.notice) : juce::String();
    }

    /** Plans an export into `exportsFolder` (the project's Exports folder), named after the project. */
    inline Plan plan (te::Edit& edit, const Settings& s, const juce::File& exportsFolder, const juce::String& name)
    {
        Plan p;
        auto ext = extensionFor (s.format);
        const bool stems = s.what != What::mix, mix = s.what != What::stems;

        te::RenderSpecification base;
        base.format = s.format;
        base.sampleRate = s.sampleRate;
        base.bitDepth = s.hasBitDepth() ? s.bitDepth : 16;
        base.quality = s.hasBitDepth() ? 0 : s.quality;
        base.dither = s.canDither() && s.dither;
        base.time = getRange (edit, s.range);
        base.includeTails = s.tails;
        base.channelLayout = "stereo";

        auto addJob = [&] (te::RenderSpecification spec, const juce::String& what)
        {
            if (auto result = te::validateRenderSpecification (edit, spec); result.failed())
                p.problems.add (what + ": " + result.getErrorMessage());
            else if (auto job = te::createRenderJob (edit, spec))
            {
                // A track with no output isn't heard when playing, but a render sums every track that
                // doesn't play into another, so mute it there too (a key it sends still reaches its
                // compressor: the engine takes that before the mute)
                for (auto t : te::getAudioTracks (edit))
                    if (t->getOutput().outputsToNone())
                        job->tracksToMute.addIfNotAlreadyThere (t->itemID);

                p.jobs.push_back (std::move (*job));
            }
        };

        if (s.reserveRights)
            p.notice = RightsNotice::Notice { edit.getEditMetadata().artist };

        // Stems go in a folder of their own, with the mix beside them
        p.folder = stems ? exportsFolder.getNonexistentChildFile (name + " Stems", {}, false) : exportsFolder;

        if (mix)
        {
            auto spec = base;
            spec.useMasterPlugins = true;
            spec.destination = stems ? p.folder.getChildFile ("00 " + juce::File::createLegalFileName (name) + " (Mix)" + ext)
                                     : exportsFolder.getNonexistentChildFile (juce::File::createLegalFileName (name), ext, false);

            switch (s.loudness)
            {
                case Loudness::off:         break;
                case Loudness::peak:        spec.normalise = true; spec.normaliseToLevelDb = -1.0f; break;
                case Loudness::streaming:   spec.normaliseByLUFS = true; spec.normaliseToLevelDb = -14.0f; break;
                case Loudness::apple:       spec.normaliseByLUFS = true; spec.normaliseToLevelDb = -16.0f; break;
                case Loudness::broadcast:   spec.normaliseByLUFS = true; spec.normaliseToLevelDb = -23.0f; break;
            }

            spec.limitTruePeak = true;
            spec.truePeakCeilingDb = -1.0f;
            p.mixFile = spec.destination;
            addJob (spec, "Mix");
        }

        if (stems)
        {
            juce::StringArray used;
            int number = 1;

            for (auto t : getStemTracks (edit))
            {
                auto stemName = juce::File::createLegalFileName (t->getName());

                for (int suffix = 2; used.contains (stemName); ++suffix)
                    stemName = juce::File::createLegalFileName (t->getName()) + " " + juce::String (suffix);

                used.add (stemName);

                auto spec = base;
                spec.tracks = { t->itemID };
                spec.includeSourceTracks = true;
                spec.useMasterPlugins = false;
                spec.destination = p.folder.getChildFile (juce::String (number++).paddedLeft ('0', 2) + " " + stemName + ext);
                addJob (spec, t->getName());
                ++p.numStems;
            }
        }

        return p;
    }

    /** "Writes My Song.wav to Exports, 24-bit 48 kHz WAV." */
    inline juce::String describe (const Plan& p, const Settings& s, const juce::File& projectFolder)
    {
        auto where = p.folder.getRelativePathFrom (projectFolder);
        juce::String text;

        if (s.what == What::mix)
            text << "Writes " << p.mixFile.getFileName() << " to " << where;
        else
            text << "Writes " << (s.what == What::mixAndStems ? "the mix and " : "") << p.numStems
                 << (p.numStems == 1 ? " stem" : " stems") << " to " << where;

        text << ", " << describeFormat (s) << ".";

        if (s.what != What::mix)
            text << "\nStems start together, before the master fader and effects.";

        if (s.loudness != Loudness::off && s.what != What::stems)
            text << "\nThe mix goes to " << (s.loudness == Loudness::peak ? juce::String ("-1 dB peak")
                                                    : juce::String (s.loudness == Loudness::streaming ? -14 : s.loudness == Loudness::apple ? -16 : -23)
                                                        + " LUFS, peaks under -1 dBTP")
                 << (s.what == What::mixAndStems ? "; stems keep their level." : ".");

        if (s.reserveRights)
            text << (s.what == What::mix ? "\nMarked as not licensed for AI training in its metadata. The sound is unchanged."
                                         : "\nEach file is marked as not licensed for AI training in its metadata. The sound is unchanged.");

        if (! p.problems.isEmpty())
            text << "\n" << p.problems.joinIntoString ("\n");

        return text;
    }

    //==============================================================================
    /** The dialog's controls, a row each, with what it'll write underneath. */
    class DialogContent : public juce::Component
    {
    public:
        DialogContent (te::Edit& e, Settings s, juce::File exports, juce::File project, juce::String n)
            : edit (e), settings (s), exportsFolder (exports), projectFolder (project), name (n)
        {
            what.addItemList ({ "Mix", "Stems", "Mix and Stems" }, 1);
            what.setSelectedItemIndex ((int) settings.what, juce::dontSendNotification);
            what.onChange = [this] { settings.what = (What) what.getSelectedItemIndex(); update(); };

            auto loop = edit.getTransport().getLoopRange();
            auto& ts = edit.tempoSequence;
            auto bar = [&ts] (te::TimePosition t) { return juce::String (ts.toBarsAndBeats (t).bars + 1); };
            range.addItem ("Song", 1);
            range.addItem ("Loop, bars " + bar (loop.getStart()) + juce::String::fromUTF8 ("\xe2\x80\x93") + bar (loop.getEnd()), 2);
            range.setSelectedItemIndex ((int) settings.range, juce::dontSendNotification);
            range.onChange = [this] { settings.range = (Range) range.getSelectedItemIndex(); update(); };

            const bool mp3 = canWriteMp3 (edit.engine);
            for (auto f : { te::RenderFormat::wav, te::RenderFormat::aiff, te::RenderFormat::flac, te::RenderFormat::mp3, te::RenderFormat::ogg })
                format.addItem (formatName (f) + (f == te::RenderFormat::mp3 && ! mp3 ? " (install LAME to use)" : juce::String()),
                                (int) f + 1);
            format.setItemEnabled ((int) te::RenderFormat::mp3 + 1, mp3);

            if (settings.format == te::RenderFormat::mp3 && ! mp3)
                settings.format = te::RenderFormat::wav;

            format.setSelectedId ((int) settings.format + 1, juce::dontSendNotification);
            format.onChange = [this]
            {
                settings.format = (te::RenderFormat) (format.getSelectedId() - 1);

                if (settings.format == te::RenderFormat::flac && settings.bitDepth == 32)
                    settings.bitDepth = 24;

                if (auto q = qualityChoices (settings.format); ! q.empty())
                    settings.quality = q.front().value;

                updateDepth();
                update();
            };

            depth.onChange = [this]
            {
                auto id = depth.getSelectedId();
                (settings.hasBitDepth() ? settings.bitDepth : settings.quality) = id - 1;
                update();
            };

            for (double r : { 44100.0, 48000.0, 88200.0, 96000.0 })
                rate.addItem (juce::String (r / 1000.0, r == 44100.0 || r == 88200.0 ? 1 : 0) + " kHz", (int) r);
            rate.setSelectedId ((int) settings.sampleRate, juce::dontSendNotification);
            rate.onChange = [this] { settings.sampleRate = rate.getSelectedId(); update(); };

            loudness.addItemList ({ "Off", "Peak, -1 dB", "Streaming, -14 LUFS", "Apple Music, -16 LUFS", "Broadcast, -23 LUFS" }, 1);
            loudness.setSelectedItemIndex ((int) settings.loudness, juce::dontSendNotification);
            loudness.onChange = [this] { settings.loudness = (Loudness) loudness.getSelectedItemIndex(); update(); };

            dither.setButtonText ("Dither");
            dither.setTooltip ("Adds a trace of noise when reducing to 16 or 24 bits, so quiet passages fade smoothly instead of breaking up");
            dither.setToggleState (settings.dither, juce::dontSendNotification);
            dither.onClick = [this] { settings.dither = dither.getToggleState(); update(); };

            tails.setButtonText ("Reverb tails");
            tails.setTooltip ("Carry on past the end until reverbs and delays have died away (up to 10 seconds)");
            tails.setToggleState (settings.tails, juce::dontSendNotification);
            tails.onClick = [this] { settings.tails = tails.getToggleState(); update(); };

            aiTraining.addItem ("Reserve rights", 1);
            aiTraining.addItem ("No notice", 2);
            aiTraining.setTooltip ("Writes a notice into each file's metadata that it isn't licensed for AI training, in forms both people "
                                   "and software can read. It doesn't change the sound, and it can't stop anyone who ignores it.");
            aiTraining.setSelectedId (settings.reserveRights ? 1 : 2, juce::dontSendNotification);
            aiTraining.onChange = [this] { settings.reserveRights = aiTraining.getSelectedId() == 1; update(); };

            for (auto* c : std::initializer_list<juce::Component*> { &what, &range, &format, &depth, &rate, &loudness, &aiTraining, &dither, &tails })
                addAndMakeVisible (c);

            preview.setJustificationType (juce::Justification::topLeft);
            preview.setFont (juce::FontOptions (12.5f));
            preview.setColour (juce::Label::textColourId, Colours::textDim);
            addAndMakeVisible (preview);

            setSize (480, 7 * rowH + 30 + 108);
            updateDepth();
            update();
        }

        const Settings& getSettings() const     { return settings; }
        Plan makePlan() const                   { return plan (edit, settings, exportsFolder, name); }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (13.0f));
            int y = 0;

            for (auto* label : { "Export", "Range", "Format", settings.hasBitDepth() ? "Bit depth" : "Quality", "Sample rate", "Loudness", "AI training" })
            {
                g.drawText (label, 0, y, labelW - 10, rowH - 6, juce::Justification::centredRight);
                y += rowH;
            }
        }

        void resized() override
        {
            auto r = getLocalBounds();

            for (auto* c : std::initializer_list<juce::Component*> { &what, &range, &format, &depth, &rate, &loudness, &aiTraining })
                c->setBounds (r.removeFromTop (rowH).withTrimmedLeft (labelW).withTrimmedBottom (6));

            auto toggles = r.removeFromTop (30).withTrimmedLeft (labelW - 4);
            dither.setBounds (toggles.removeFromLeft (100));
            tails.setBounds (toggles.removeFromLeft (140));
            preview.setBounds (r.withTrimmedLeft (labelW - 4).withTrimmedTop (6));
        }

    private:
        static constexpr int rowH = 34, labelW = 100;
        te::Edit& edit;
        Settings settings;
        juce::File exportsFolder, projectFolder;
        juce::String name;
        juce::ComboBox what, range, format, depth, rate, loudness, aiTraining;
        juce::ToggleButton dither, tails;
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
                for (auto& q : qualityChoices (settings.format))
                    depth.addItem (q.name, q.value + 1);

                depth.setSelectedId (settings.quality + 1, juce::dontSendNotification);
            }

            repaint();
        }

        void update()
        {
            dither.setEnabled (settings.canDither());
            loudness.setEnabled (settings.what != What::stems);
            preview.setText (describe (makePlan(), settings, projectFolder), juce::dontSendNotification);
        }
    };

    //==============================================================================
    /** Runs the jobs one after another with a progress bar, then shows the files. */
    class Progress : public juce::Component, private juce::Timer
    {
    public:
        Progress (te::Edit& e, Plan p) : edit (e), planned (std::move (p))
        {
            label.setFont (juce::FontOptions (12.5f));
            label.setColour (juce::Label::textColourId, Colours::textDim);
            addAndMakeVisible (label);
            addAndMakeVisible (bar);
            setSize (400, 56);

            edit.getTransport().stop (false, true);
            total = (int) planned.jobs.size();
            planned.folder.createDirectory();
            queue.addJobs (std::move (planned.jobs));
            queue.onJobStarted = [this] (te::RenderQueue::Job& job) { label.setText ("Rendering " + job.getName() + "...", juce::dontSendNotification); };
            queue.onJobFinished = [this] (te::RenderQueue::Job& job)
            {
                if (job.getState() == te::RenderQueue::Job::State::failed)
                    failures.add (job.getName() + ": " + job.getError());
                else if (job.getState() == te::RenderQueue::Job::State::completed)
                    if (auto problem = finishFile (planned, job.getFile()); problem.isNotEmpty())
                        unmarked.add (job.getFile().getFileName() + ": " + problem);
            };
            queue.onFinished = [this] { finished(); };
            queue.start();
            startTimerHz (15);
        }

        /** Given a title and a summary when something went wrong, both empty otherwise. */
        std::function<void (juce::String, juce::String)> onDone;

        void cancel()   { cancelled = true; queue.cancelAll(); }

        void resized() override
        {
            auto r = getLocalBounds();
            label.setBounds (r.removeFromTop (22));
            bar.setBounds (r.removeFromTop (14).reduced (0, 2));
        }

    private:
        te::Edit& edit;
        Plan planned;
        te::RenderQueue queue;
        juce::Label label;
        double progress = 0.0;
        juce::ProgressBar bar { progress };
        juce::StringArray failures, unmarked;
        int total = 0;
        bool cancelled = false;

        void timerCallback() override   { progress = queue.getTotalProgress(); }

        void finished()
        {
            stopTimer();

            if (! cancelled && failures.isEmpty())
                (planned.mixFile.existsAsFile() && planned.numStems == 0 ? planned.mixFile : planned.folder).revealToUser();

            if (! onDone)
                return;

            if (cancelled || (failures.isEmpty() && unmarked.isEmpty()))
                onDone ({}, {});
            else if (! failures.isEmpty())
                onDone ("Some files weren't exported", failures.joinIntoString ("\n")
                            + (unmarked.isEmpty() ? juce::String() : "\n\nThese were exported without the AI training notice:\n" + unmarked.joinIntoString ("\n")));
            else
                onDone ("Exported without the AI training notice", "The audio is fine, but the notice couldn't be added to:\n" + unmarked.joinIntoString ("\n"));
        }
    };

    /** The Export dialog, then the export. */
    inline void showDialog (te::Edit& edit, const juce::File& projectFolder, const juce::String& name)
    {
        auto exports = projectFolder.getChildFile ("Exports");
        auto settings = lastSettings();

        // Start from the loop when it's on, as play would
        settings.range = edit.getTransport().looping ? Range::loop : Range::song;

        if (auto rate = edit.engine.getDeviceManager().getSampleRate(); ! settings.chosen && (rate == 44100.0 || rate == 48000.0 || rate == 88200.0 || rate == 96000.0))
            settings.sampleRate = rate;

        auto content = std::make_shared<DialogContent> (edit, settings, exports, projectFolder, name);
        auto* aw = Theme::newAlertWindow ("Export", name);
        aw->addCustomComponent (content.get());
        aw->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create ([content, &edit, name] (int result)
        {
            if (result != 1)
                return;

            lastSettings() = content->getSettings();
            lastSettings().chosen = true;
            auto p = content->makePlan();

            if (p.jobs.empty())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Nothing to export",
                                                        p.problems.isEmpty() ? juce::String ("There's nothing playing to export.") : p.problems.joinIntoString ("\n"));
                return;
            }

            auto progress = std::make_shared<Progress> (edit, std::move (p));
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

} // namespace elsewhere
