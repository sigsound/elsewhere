// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/**
    Normalize, as in Logic: turns audio clips (or parts of them) up or down so their loudest peak,
    or how loud they sound overall, reaches a target. It doesn't rewrite the audio, it sets the
    clip's gain, so the Gain knob in the clip's details shows it and Undo takes it back. Clip gain
    goes up to +24 dB.

    Peak looks at the single loudest sample, so a take with a few sharp spikes hardly moves.
    Loudness (ITU-R BS.1770, in LUFS) looks at the whole take, so it brings quiet ones up, though
    their peaks can then go over 0 dBFS: the mix runs in floating point, so nothing clips until the
    master, but the fader or a limiter may need to come down.
*/
namespace Normalise
{
    enum class Method { peak, loudness };
    enum class Process { individually, perTrack, together };

    struct Settings
    {
        Method method = Method::peak;
        float peakTarget = -1.0f, loudnessTarget = -16.0f;    // dBFS, LUFS
        Process process = Process::individually;

        float target() const    { return method == Method::peak ? peakTarget : loudnessTarget; }
    };

    /** The settings used last, offered again next time. */
    inline Settings& lastSettings()     { static Settings s; return s; }

    /** Some of a clip, in edit time. */
    struct Part
    {
        te::AudioClipBase::Ptr clip;
        te::TimeRange range;
    };

    inline Part wholeClip (te::AudioClipBase& c)    { return { &c, c.getEditTimeRange() }; }

    /** A part's audio before the clip's gain: its peak level (1.0 = 0 dBFS) and the K-weighted
        power of each 400 ms block (BS.1770's gating blocks, 100 ms apart), summed over channels. */
    struct Measurement
    {
        float peak = 0.0f;
        std::vector<double> blocks;
        bool ok = false;
    };

    /** Opens the file a part plays and finds the part in it, as sample positions. Same mapping as
        the clip's waveform: warped and looped clips play a rendered file that already includes it,
        plain clips play their source through the speed ratio. */
    inline std::unique_ptr<juce::AudioFormatReader> openPart (const Part& part, juce::int64& start, juce::int64& end)
    {
        auto& clip = *part.clip;
        auto file = clip.getPlaybackFile().getFile();
        auto offset = clip.getPosition().getOffset().inSeconds();
        auto from = (part.range.getStart() - clip.getPosition().getStart()).inSeconds();
        auto to = (part.range.getEnd() - clip.getPosition().getStart()).inSeconds();
        auto speed = (clip.usesTimeStretchedProxy() || clip.isLooping()) ? 1.0 : clip.getSpeedRatio();
        double s0 = (offset + from) * speed, s1 = (offset + to) * speed;

        // A rendered file that isn't ready yet: the whole source is the closest we can get
        if (! file.existsAsFile())
        {
            file = clip.getOriginalFile();
            s0 = 0.0;
            s1 = std::numeric_limits<double>::max();
        }

        std::unique_ptr<juce::AudioFormatReader> reader (te::AudioFileUtils::createReaderFor (clip.edit.engine, file));

        if (reader == nullptr || reader->lengthInSamples <= 0 || reader->numChannels == 0)
            return {};

        auto length = reader->lengthInSamples;
        auto sr = reader->sampleRate;
        start = juce::jlimit<juce::int64> (0, length, (juce::int64) (s0 * sr));
        end = s1 >= (double) length / sr ? length : juce::jlimit<juce::int64> (start, length, (juce::int64) std::ceil (s1 * sr));
        return reader;
    }

    struct Biquad
    {
        double b0, b1, b2, a1, a2, z1 = 0.0, z2 = 0.0;

        double process (double x)
        {
            auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    /** BS.1770's K-weighting at any sample rate: a high shelf (the head), then a high-pass. */
    inline std::pair<Biquad, Biquad> kWeighting (double sr)
    {
        const auto pi = juce::MathConstants<double>::pi;

        const double shelfF = 1681.974450955533, shelfGain = 3.999843853973347, shelfQ = 0.7071752369554196;
        auto k = std::tan (pi * shelfF / sr);
        auto vh = std::pow (10.0, shelfGain / 20.0), vb = std::pow (vh, 0.4996667741545416);
        auto a0 = 1.0 + k / shelfQ + k * k;
        Biquad shelf { (vh + vb * k / shelfQ + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / shelfQ + k * k) / a0,
                       2.0 * (k * k - 1.0) / a0, (1.0 - k / shelfQ + k * k) / a0 };

        const double passF = 38.13547087602444, passQ = 0.5003270373238773;
        k = std::tan (pi * passF / sr);
        a0 = 1.0 + k / passQ + k * k;
        Biquad highPass { 1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / passQ + k * k) / a0 };

        return { shelf, highPass };
    }

    inline Measurement measure (const Part& part)
    {
        Measurement m;
        juce::int64 start = 0, end = 0;
        auto reader = openPart (part, start, end);

        if (reader == nullptr || end <= start)
            return m;

        const auto numChannels = (int) reader->numChannels;
        const auto step = juce::jmax (1, juce::roundToInt (reader->sampleRate * 0.1));
        std::vector<std::pair<Biquad, Biquad>> filters ((size_t) numChannels, kWeighting (reader->sampleRate));
        std::vector<double> quarters;     // K-weighted energy of each 100 ms
        double energy = 0.0, total = 0.0;
        juce::int64 counted = 0;
        int inQuarter = 0;

        juce::AudioBuffer<float> buffer (numChannels, 32768);

        for (auto pos = start; pos < end;)
        {
            auto n = (int) std::min<juce::int64> (buffer.getNumSamples(), end - pos);
            reader->read (&buffer, 0, n, pos, true, true);

            for (int i = 0; i < n; ++i)
            {
                double e = 0.0;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto x = (double) buffer.getSample (ch, i);
                    m.peak = juce::jmax (m.peak, (float) std::abs (x));
                    auto& [shelf, highPass] = filters[(size_t) ch];
                    auto y = highPass.process (shelf.process (x));
                    e += y * y;
                }

                energy += e;
                total += e;
                ++counted;

                if (++inQuarter == step)
                {
                    quarters.push_back (energy);
                    energy = 0.0;
                    inQuarter = 0;
                }
            }

            pos += n;
        }

        for (size_t i = 0; i + 4 <= quarters.size(); ++i)
            m.blocks.push_back ((quarters[i] + quarters[i + 1] + quarters[i + 2] + quarters[i + 3]) / (4.0 * step));

        // Shorter than one block: the whole part counts as one
        if (m.blocks.empty() && counted > 0)
            m.blocks.push_back (total / (double) counted);

        m.ok = true;
        return m;
    }

    /** Integrated loudness (LUFS) of gating blocks, with BS.1770's absolute (-70) and relative
        (-10 LU) gates; minus infinity for silence. */
    inline double loudnessOf (const std::vector<double>& blocks)
    {
        auto lufs = [] (double power) { return -0.691 + 10.0 * std::log10 (juce::jmax (power, 1.0e-20)); };
        auto meanAbove = [&] (double gate, int& n)
        {
            double sum = 0.0;
            n = 0;
            for (auto b : blocks)
                if (lufs (b) > gate)
                    sum += b, ++n;
            return n > 0 ? sum / n : 0.0;
        };

        int n = 0;
        auto ungated = meanAbove (-70.0, n);

        if (n == 0)
            return -std::numeric_limits<double>::infinity();

        auto gated = meanAbove (juce::jmax (-70.0, lufs (ungated) - 10.0), n);
        return n > 0 ? lufs (gated) : -std::numeric_limits<double>::infinity();
    }

    /** A part's level as the method sees it, with its clip's current gain. */
    inline double levelOf (const Part& part, const Measurement& m, Method method)
    {
        if (! m.ok)
            return -std::numeric_limits<double>::infinity();

        if (method == Method::peak)
            return m.peak > 0.0f ? juce::Decibels::gainToDecibels (m.peak, -1000.0f) + part.clip->getGainDB() : -std::numeric_limits<double>::infinity();

        return loudnessOf (m.blocks) + part.clip->getGainDB();
    }

    /** How many dB each part should move to reach the target. Parts that move together share one
        change, set by their loudest peak or their loudness as a whole. Silent or unreadable parts
        get 0. */
    inline std::vector<float> plan (const std::vector<Part>& parts, const std::vector<Measurement>& ms, const Settings& s)
    {
        std::vector<float> changes (parts.size(), 0.0f);
        std::map<juce::String, std::vector<size_t>> groups;

        for (size_t i = 0; i < parts.size(); ++i)
        {
            auto key = s.process == Process::individually ? juce::String ((int) i)
                     : s.process == Process::perTrack     ? (parts[i].clip->getTrack() != nullptr ? parts[i].clip->getTrack()->itemID.toString() : juce::String())
                                                          : juce::String ("all");
            groups[key].push_back (i);
        }

        for (auto& [key, members] : groups)
        {
            auto level = -std::numeric_limits<double>::infinity();

            if (s.method == Method::peak)
            {
                for (auto i : members)
                    level = juce::jmax (level, levelOf (parts[i], ms[i], Method::peak));
            }
            else
            {
                std::vector<double> pooled;    // every block, at its clip's gain

                for (auto i : members)
                    if (ms[i].ok)
                        for (auto b : ms[i].blocks)
                            pooled.push_back (b * std::pow (10.0, parts[i].clip->getGainDB() / 10.0));

                level = loudnessOf (pooled);
            }

            if (std::isfinite (level))
                for (auto i : members)
                    changes[i] = (float) (s.target() - level);
        }

        return changes;
    }

    /** Adds the changes to the clips' gain, as one undoable step. */
    inline void applyToClips (const std::vector<Part>& parts, const std::vector<float>& changes)
    {
        if (parts.empty())
            return;

        parts.front().clip->edit.getUndoManager().beginNewTransaction ("Normalize");

        for (size_t i = 0; i < parts.size(); ++i)
            if (changes[i] != 0.0f)
                parts[i].clip->setGainDB (parts[i].clip->getGainDB() + changes[i]);
    }

    //==============================================================================
    /** The dialog's controls: Process (for more than one clip), Method and Target, with a line
        saying what it would do. */
    class DialogContent : public juce::Component
    {
    public:
        DialogContent (std::vector<Part> p, Settings s) : parts (std::move (p)), settings (s)
        {
            for (auto& part : parts)
                measurements.push_back (measure (part));

            showProcess = parts.size() > 1;

            process.addItemList ({ "Clips Individually", "Per Track Equally", "All Equally" }, 1);
            process.setSelectedItemIndex ((int) settings.process, juce::dontSendNotification);
            process.onChange = [this] { settings.process = (Process) process.getSelectedItemIndex(); update(); };
            addChildComponent (process);
            process.setVisible (showProcess);

            method.addItemList ({ "Peak", "Loudness" }, 1);
            method.setSelectedItemIndex ((int) settings.method, juce::dontSendNotification);
            method.onChange = [this] { settings.method = (Method) method.getSelectedItemIndex(); updateTarget(); update(); };
            addAndMakeVisible (method);

            target.setSliderStyle (juce::Slider::LinearBar);
            target.onValueChange = [this]
            {
                (settings.method == Method::peak ? settings.peakTarget : settings.loudnessTarget) = (float) target.getValue();
                update();
            };
            addAndMakeVisible (target);

            preview.setJustificationType (juce::Justification::topLeft);
            preview.setFont (juce::FontOptions (12.5f));
            preview.setColour (juce::Label::textColourId, Colours::textDim);
            addAndMakeVisible (preview);

            setSize (400, (showProcess ? 3 : 2) * rowH + 52);
            updateTarget();
            update();
        }

        std::vector<float> getChanges() const   { return plan (parts, measurements, settings); }
        const Settings& getSettings() const     { return settings; }

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (13.0f));
            int y = 0;

            for (auto* name : { "Process", "Method", "Target" })
            {
                if (juce::String (name) == "Process" && ! showProcess)
                    continue;

                g.drawText (name, 0, y, labelW - 10, rowH - 6, juce::Justification::centredRight);
                y += rowH;
            }
        }

        void resized() override
        {
            auto r = getLocalBounds();

            for (auto* c : { (juce::Component*) &process, (juce::Component*) &method, (juce::Component*) &target })
            {
                if (c == &process && ! showProcess)
                    continue;

                c->setBounds (r.removeFromTop (rowH).withTrimmedLeft (labelW).withTrimmedBottom (6));
            }

            preview.setBounds (r.withTrimmedLeft (labelW - 4).withTrimmedTop (4));
        }

    private:
        static constexpr int rowH = 34, labelW = 90;
        std::vector<Part> parts;
        std::vector<Measurement> measurements;
        Settings settings;
        bool showProcess = false;
        juce::ComboBox process, method;
        juce::Slider target;
        juce::Label preview;

        void updateTarget()
        {
            auto peak = settings.method == Method::peak;
            target.setRange (peak ? -30.0 : -40.0, peak ? 0.0 : -6.0, 0.1);
            target.setTextValueSuffix (peak ? " dBFS" : " LUFS");
            target.setValue (settings.target(), juce::dontSendNotification);
        }

        void update()
        {
            auto changes = getChanges();
            auto db = [] (double v) { return (v >= 0.0 ? "+" : "") + juce::String (v, 1) + " dB"; };

            float lo = std::numeric_limits<float>::max(), hi = std::numeric_limits<float>::lowest();
            auto peakAfter = -std::numeric_limits<double>::infinity();
            bool any = false, capped = false;

            for (size_t i = 0; i < parts.size(); ++i)
            {
                if (! std::isfinite (levelOf (parts[i], measurements[i], settings.method)))
                    continue;

                any = true;
                lo = juce::jmin (lo, changes[i]);
                hi = juce::jmax (hi, changes[i]);
                capped = capped || parts[i].clip->getGainDB() + changes[i] > 24.0f;
                peakAfter = juce::jmax (peakAfter, levelOf (parts[i], measurements[i], Method::peak)
                                                     + juce::jmin ((double) changes[i], 24.0 - parts[i].clip->getGainDB()));
            }

            if (! any)
            {
                preview.setText ("Nothing to normalize: it's silent.", juce::dontSendNotification);
                return;
            }

            juce::String text = std::abs (hi - lo) < 0.05f ? "Gain change: " + db (lo) : "Gain change: " + db (lo) + " to " + db (hi);

            if (capped)
                text << " (clip gain stops at +24 dB)";

            text << "\nPeaks reach " << juce::String (peakAfter, 1) << " dBFS";

            if (peakAfter > 0.05)
                text << ", over full scale: bring the fader down or add a limiter";

            preview.setText (text, juce::dontSendNotification);
        }
    };

    /** The Normalize dialog. `apply` is given the change for each part in dB; without one, the
        changes go on the clips' gain. `what` names what's being normalized ("ac2_01", "3 clips"). */
    inline void showDialog (std::vector<Part> parts, const juce::String& what,
                            std::function<void (const std::vector<Part>&, const std::vector<float>&)> apply = {},
                            std::optional<Process> process = {})
    {
        if (parts.empty())
            return;

        auto settings = lastSettings();
        if (process)
            settings.process = *process;

        auto content = std::make_shared<DialogContent> (parts, settings);
        auto* aw = Theme::newAlertWindow ("Normalize", what);
        aw->addCustomComponent (content.get());
        aw->addButton ("Apply", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create ([content, parts, apply] (int result)
        {
            if (result == 1)
            {
                lastSettings() = content->getSettings();
                auto changes = content->getChanges();

                if (apply)
                    apply (parts, changes);
                else
                    applyToClips (parts, changes);
            }
        }), true);
    }

    /** Normalize for whole clips, as from the clip and track menus. */
    inline void showForClips (const juce::Array<te::AudioClipBase*>& clips, std::optional<Process> process = {})
    {
        std::vector<Part> parts;
        for (auto c : clips)
            parts.push_back (wholeClip (*c));

        if (parts.empty())
            return;

        showDialog (parts, parts.size() == 1 ? parts.front().clip->getName() : juce::String ((int) parts.size()) + " clips", {}, process);
    }

    /** A track's audio clips. */
    inline juce::Array<te::AudioClipBase*> audioClipsOf (te::ClipTrack& track)
    {
        juce::Array<te::AudioClipBase*> result;

        for (auto c : track.getClips())
            if (auto ac = dynamic_cast<te::AudioClipBase*> (c))
                result.add (ac);

        return result;
    }
}

} // namespace elsewhere
