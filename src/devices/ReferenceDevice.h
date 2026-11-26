// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "Dsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Reference: a finished song to hold your mix up against.

    It sits at the end of the master track, after the master's own devices, so the reference
    skips them. Switch between Mix and Reference (R does it from anywhere) and the reference
    plays in place of the mix, from the same point in the song.

    While the mix plays, the device measures how loud it is (BS.1770, gated, in LUFS); the
    reference's loudness is measured once when it's loaded. With Match on, the reference is
    turned up or down by the difference, so the two compare at the same loudness and the louder
    one doesn't win by being louder. Trim adds to that.

    Exports always render the mix: the reference never goes into a file.
*/
class ReferenceDevice : public ElsewhereDevice,
                        private juce::AsyncUpdater
{
public:
    static constexpr const char* xmlTypeName = "elsewhereReference";

    ReferenceDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        listen = addParameter ("listen", "Listen", 0.0f, stepped (0.0f, 1.0f),
                               [] (float v) { return v > 0.5f ? juce::String ("Reference") : juce::String ("Mix"); });
        match = addParameter ("match", "Match", 1.0f, stepped (0.0f, 1.0f), onOffText);
        trim = addParameter ("trim", "Trim", 0.0f, { -12.0f, 12.0f }, dbText);
        loadFile();
    }

    ~ReferenceDevice() override
    {
        if (loader != nullptr && loader->joinable())
            loader->join();

        cancelPendingUpdate();
    }

    juce::String getName() const override              { return "Elsewhere Reference"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Reference"; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    int getDeviceViewWidth() const override             { return 300; }

    Param listen, match, trim;

    bool isListeningToReference() const     { return listen.get() > 0.5f; }

    void setListeningToReference (bool b)
    {
        listen.param().parameterChangeGestureBegin();
        listen.param().setParameter (b ? 1.0f : 0.0f, juce::sendNotification);
        listen.param().parameterChangeGestureEnd();
    }

    //==============================================================================
    /** Loads a song to compare with. The file stays where it is (it isn't copied into the
        project); inside the project folder it's kept as a relative path. */
    void setFile (const juce::File& f)
    {
        juce::String path = f.getFullPathName();

        if (auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File(); editFile != juce::File())
            if (f.isAChildOf (editFile.getParentDirectory()))
                path = f.getRelativePathFrom (editFile.getParentDirectory());

        state.setProperty (fileProp, path, getUndoManager());
        loadFile();
    }

    juce::File getFile() const
    {
        auto path = state.getProperty (fileProp).toString();

        if (path.isEmpty())
            return {};

        if (juce::File::isAbsolutePath (path))
            return juce::File (path);

        auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File();
        return editFile.getParentDirectory().getChildFile (path);
    }

    enum class Status { none, loading, ready, missing };
    Status getStatus() const                { return status.load(); }

    /** The reference's loudness, and the mix's as measured so far (both LUFS, -120 if unknown). */
    double getReferenceLufs() const         { return referenceLufs.load(); }
    double getMixLufs() const               { return mixLufs.load(); }

    /** The gain the reference plays at now, in dB: matching plus trim. */
    float getReferenceGainDb() const
    {
        float g = trim.get();

        if (match.get() > 0.5f && referenceLufs.load() > -70.0 && mixLufs.load() > -70.0)
            g += (float) (mixLufs.load() - referenceLufs.load());

        return juce::jlimit (-24.0f, 24.0f, g);
    }

    /** Starts measuring the mix afresh (after big changes to it). */
    void resetMixMeasurement()              { resetMix = true; mixLufs = -120.0; }

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        mixMeter.prepare (rate);
        gain.reset (rate, 0.05);
        fade = isListeningToReference() ? 1.0f : 0.0f;
    }

    void reset() override
    {
        mixMeter.reset();
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked() || fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
            return;

        // Exports are always the mix
        if (fc.isRendering)
            return;

        auto& buf = *fc.destBuffer;
        const int start = fc.bufferStartSample, n = fc.bufferNumSamples;
        const int numCh = juce::jmin (2, buf.getNumChannels());

        if (numCh == 0)
            return;

        const float* l = buf.getReadPointer (0, start);
        const float* r = buf.getReadPointer (numCh > 1 ? 1 : 0, start);

        if (resetMix.exchange (false))
            mixMeter.reset();

        // Measure the mix while it plays and is what you hear
        const bool wantReference = isListeningToReference();

        if (fc.isPlaying && ! wantReference && fade <= 0.0f)
        {
            mixMeter.process (l, r, n);

            if (mixMeter.getSecondsAudible() > 2.0)
                mixLufs = mixMeter.getIntegrated();
        }

        const float target = wantReference ? 1.0f : 0.0f;

        if (fade == 0.0f && target == 0.0f)
            return;

        // The reference, from the same point in the song (silent while stopped)
        const juce::SpinLock::ScopedTryLockType audioGuard (audioLock);
        const auto* src = audioGuard.isLocked() && audio != nullptr ? audio.get() : nullptr;
        const double startSeconds = fc.editTime.getStart().inSeconds();
        const double step = src != nullptr ? src->sampleRate / rate : 0.0;
        double pos = startSeconds * (src != nullptr ? src->sampleRate : 0.0);
        gain.setTargetValue (juce::Decibels::decibelsToGain (getReferenceGainDb()));

        // Mix and reference cross over in 20 ms, so switching doesn't click
        const float fadeStep = (float) (1.0 / (0.02 * rate));

        for (int i = 0; i < n; ++i)
        {
            fade = fade < target ? juce::jmin (target, fade + fadeStep) : juce::jmax (target, fade - fadeStep);
            const float g = gain.getNextValue();
            float refL = 0.0f, refR = 0.0f;

            if (src != nullptr && fc.isPlaying && pos >= 0.0)
            {
                const auto i0 = (int) pos;

                if (i0 + 1 < src->buffer.getNumSamples())
                {
                    const auto frac = (float) (pos - i0);
                    const int last = src->buffer.getNumChannels() - 1;
                    auto at = [&] (int ch) { auto* d = src->buffer.getReadPointer (juce::jmin (ch, last)); return d[i0] + frac * (d[i0 + 1] - d[i0]); };
                    refL = at (0) * g;
                    refR = at (1) * g;
                }
            }

            pos += step;

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* d = buf.getWritePointer (ch, start);
                d[i] = d[i] * (1.0f - fade) + (ch == 0 ? refL : refR) * fade;
            }
        }
    }

private:
    static inline const juce::Identifier fileProp { "referenceFile" };

    struct Audio
    {
        juce::AudioBuffer<float> buffer;
        double sampleRate = 44100.0;
    };

    double rate = 44100.0;
    std::unique_ptr<Audio> audio;
    juce::SpinLock audioLock;
    LoudnessMeter mixMeter;
    std::atomic<bool> resetMix { false };
    std::atomic<double> referenceLufs { -120.0 }, mixLufs { -120.0 };
    std::atomic<Status> status { Status::none };
    juce::LinearSmoothedValue<float> gain { 1.0f };
    float fade = 0.0f;
    std::unique_ptr<std::thread> loader;
    std::unique_ptr<Audio> loaded;
    std::mutex loadedLock;

    /** Reads the file on a background thread, measures it, then swaps it in. */
    void loadFile()
    {
        if (loader != nullptr && loader->joinable())
            loader->join();

        auto file = getFile();

        if (file == juce::File())
        {
            status = Status::none;
            const juce::SpinLock::ScopedLockType g (audioLock);
            audio.reset();
            return;
        }

        if (! file.existsAsFile())
        {
            status = Status::missing;
            return;
        }

        status = Status::loading;
        loader = std::make_unique<std::thread> ([this, file]
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            auto a = std::make_unique<Audio>();
            double lufs = -120.0;

            if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file)); reader != nullptr)
            {
                const auto length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 60 * 20));
                a->sampleRate = reader->sampleRate;
                a->buffer.setSize (juce::jlimit (1, 2, (int) reader->numChannels), length);
                reader->read (&a->buffer, 0, length, 0, true, a->buffer.getNumChannels() > 1);

                LoudnessMeter m;
                m.prepare (a->sampleRate);
                auto* l = a->buffer.getReadPointer (0);
                m.process (l, a->buffer.getNumChannels() > 1 ? a->buffer.getReadPointer (1) : l, length);
                lufs = m.getIntegrated();
            }

            referenceLufs = lufs;
            {
                const std::lock_guard<std::mutex> g (loadedLock);
                loaded = a->buffer.getNumSamples() > 0 ? std::move (a) : nullptr;
            }
            triggerAsyncUpdate();
        });
    }

    void handleAsyncUpdate() override
    {
        std::unique_ptr<Audio> next;
        {
            const std::lock_guard<std::mutex> g (loadedLock);
            next = std::move (loaded);
        }

        status = next != nullptr ? Status::ready : Status::missing;
        {
            const juce::SpinLock::ScopedLockType g (audioLock);
            std::swap (audio, next);
        }
        // the old audio is freed here, off the audio thread
    }
};

//==============================================================================
class ReferenceView : public juce::Component, private juce::Timer
{
public:
    ReferenceView (ReferenceDevice& d)
        : device (d),
          mode (d.listen.param(), { "Mix", "Reference" }),
          matchToggle (d.match.param(), "Match loudness"),
          trimKnob (d.trim.param(), "Trim")
    {
        addAndMakeVisible (mode);
        addAndMakeVisible (matchToggle);
        addAndMakeVisible (trimKnob);

        choose.onClick = [this] { chooseFile(); };
        choose.setTooltip ("Choose a finished song to compare your mix with");
        addAndMakeVisible (choose);

        remeasure.onClick = [this] { device.resetMixMeasurement(); };
        remeasure.setTooltip ("Measure the mix's loudness again from here on");
        addAndMakeVisible (remeasure);

        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = info;
        g.setFont (juce::FontOptions (11.0f));
        auto line = [&] (const juce::String& label, const juce::String& value)
        {
            auto row = r.removeFromTop (16);
            g.setColour (Colours::textDim);
            g.drawText (label, row.removeFromLeft (70), juce::Justification::centredLeft);
            g.setColour (Colours::text);
            g.drawText (value, row, juce::Justification::centredLeft, true);
        };

        auto lufs = [] (double v) { return v > -70.0 ? juce::String (v, 1) + " LUFS" : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")); };

        juce::String file;
        switch (device.getStatus())
        {
            case ReferenceDevice::Status::none:     file = "No song chosen"; break;
            case ReferenceDevice::Status::loading:  file = "Loading " + device.getFile().getFileName(); break;
            case ReferenceDevice::Status::missing:  file = "Can't find " + device.getFile().getFileName(); break;
            case ReferenceDevice::Status::ready:    file = device.getFile().getFileName(); break;
        }

        line ("Song", file);
        line ("Reference", lufs (device.getReferenceLufs()));
        line ("Your mix", device.getMixLufs() > -70.0 ? lufs (device.getMixLufs()) : juce::String ("Play the mix to measure it"));
        line ("Plays at", juce::String (device.getReferenceGainDb(), 1) + " dB");
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (24);
        choose.setBounds (top.removeFromRight (70));
        top.removeFromRight (6);
        mode.setBounds (top);
        r.removeFromTop (6);
        auto right = r.removeFromRight (64);
        trimKnob.setBounds (right.removeFromTop (juce::jmin (64, right.getHeight())));
        info = r.removeFromTop (66);
        auto bottom = r.removeFromTop (22);
        remeasure.setBounds (bottom.removeFromRight (80));
        matchToggle.setBounds (bottom.removeFromLeft (juce::jmin (140, bottom.getWidth() - 4)));
    }

private:
    ReferenceDevice& device;
    DeviceUI::Choice mode;
    DeviceUI::Toggle matchToggle;
    DeviceUI::Knob trimKnob;
    juce::TextButton choose { "Choose..." }, remeasure { "Measure again" };
    juce::Rectangle<int> info;
    std::unique_ptr<juce::FileChooser> chooser;

    void timerCallback() override   { repaint (info); }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a song to compare with", device.getFile(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [d = te::Plugin::Ptr (&device)] (const juce::FileChooser& fc)
        {
            if (fc.getResult() != juce::File())
                if (auto* ref = dynamic_cast<ReferenceDevice*> (d.get()))
                {
                    ref->edit.getUndoManager().beginNewTransaction ("Choose Reference");
                    ref->setFile (fc.getResult());
                }
        });
    }
};

inline std::unique_ptr<juce::Component> ReferenceDevice::createDeviceView()   { return std::make_unique<ReferenceView> (*this); }

} // namespace elsewhere
