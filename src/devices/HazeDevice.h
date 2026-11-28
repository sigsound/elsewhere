// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "OrganicDsp.h"
#include "HazeEngine.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Haze: a synth that sounds remembered rather than played. Warm, slightly detuned
    analog-style voices go through a worn tape and VHS chain: wow and flutter bend the pitch
    (the reverb tail too), Age saturates, lifts the low end and rolls off the highs, Tracking
    throws in the odd glitch and dropout, Chroma smears it wide like an old chorus, Hiss sits
    underneath, Speed slows the whole thing down like a tape played slow, and a dark Haze
    space holds it all. Mod wheel adds warble.

    Voices: two oscillators (saw, square, triangle or a soft wave) detuned from each other,
    a sub, noise, a 12 dB low-pass with its own envelope amount, and an amp envelope.
*/
class HazeDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereHaze";
    static constexpr int maxVoices = HazeEngine::maxVoices;

    using Wave = HazeEngine::Wave;
    static constexpr int saw = HazeEngine::saw, square = HazeEngine::square, triangle = HazeEngine::triangle,
                         soft = HazeEngine::soft, numWaves = HazeEngine::numWaves;
    static juce::StringArray waveNames()    { return { "Saw", "Square", "Triangle", "Soft" }; }
    static juce::StringArray chromaNames()  { return { "Off", "I", "II", "I+II" }; }

    HazeDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto cents = [] (float v) { return juce::String (juce::roundToInt (v)) + " ct"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        auto octave = [] (float v) { const int o = juce::roundToInt (v); return o == 0 ? juce::String ("0") : (o > 0 ? "+" : "") + juce::String (o) + " oct"; };
        const juce::NormalisableRange<float> pct { 0.0f, 100.0f };
        const HazeEngine::Params d;     // the defaults

        wave      = addParameter ("wave", "Wave", d.wave, stepped (0, numWaves - 1), pick (waveNames()));
        detune    = addParameter ("detune", "Detune", d.detune, { 0.0f, 50.0f }, cents);
        sub       = addParameter ("sub", "Sub", d.sub, pct, percentText);
        noise     = addParameter ("noise", "Noise", d.noise, pct, percentText);
        range     = addParameter ("range", "Range", d.range, stepped (-2, 1), octave);
        cutoff    = addParameter ("cutoff", "Cutoff", d.cutoff, freqRange (80.0f, 18000.0f), hzText);
        resonance = addParameter ("res", "Resonance", d.resonance, pct, percentText);
        envAmount = addParameter ("envamt", "Env", d.envAmount, pct, percentText);
        attack    = addParameter ("attack", "Attack", d.attack, skewed (0.001f, 6.0f, 0.4f), secs);
        decay     = addParameter ("decay", "Decay", d.decay, skewed (0.02f, 6.0f, 0.8f), secs);
        sustain   = addParameter ("sustain", "Sustain", d.sustain, pct, percentText);
        release   = addParameter ("release", "Release", d.release, skewed (0.02f, 8.0f, 1.2f), secs);
        wow       = addParameter ("wow", "Wow", d.wow, pct, percentText);
        flutter   = addParameter ("flutter", "Flutter", d.flutter, pct, percentText);
        age       = addParameter ("age", "Age", d.age, pct, percentText);
        speed     = addParameter ("speed", "Speed", d.speed, { 50.0f, 100.0f }, percentText);
        tracking  = addParameter ("tracking", "Tracking", d.tracking, pct, percentText);
        bandwidth = addParameter ("bandwidth", "Bandwidth", d.bandwidth, freqRange (1500.0f, 20000.0f), hzText);
        chroma    = addParameter ("chroma", "Chroma", d.chroma, stepped (0, 3), pick (chromaNames()));
        hiss      = addParameter ("hiss", "Hiss", d.hiss, pct, percentText);
        space     = addParameter ("space", "Space", d.space, pct, percentText);
        spaceMix  = addParameter ("smix", "Space Mix", d.spaceMix, pct, percentText);
        drift     = addParameter ("drift", "Drift", d.drift, pct, percentText);
        volume    = addParameter ("volume", "Volume", d.volume, { -60.0f, 6.0f }, dbText);

        if (! state.hasProperty ("patch"))
            state.setProperty ("patch", 0, nullptr);
    }

    juce::String getName() const override              { return "Elsewhere Haze"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Haze"; }
    int getDeviceViewWidth() const override            { return 1200; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1000, 700 }; }

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 8.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }

    //==============================================================================
    // Patches (HazeEngine keeps them, so anything playing the engine has the same ones)
    using Patch = HazeEngine::Patch;
    static const std::vector<Patch>& patches()          { return HazeEngine::patches(); }

    float patchValue (int index, te::AutomatableParameter& ap) const
    {
        return HazeEngine::patchValue (index, ap.paramID.toRawUTF8());
    }

    /** Sets every control to a patch: as its own undo step, or as part of the one in progress. */
    void loadPatch (int index, bool ownUndoStep = true)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        if (ownUndoStep)
            edit.getUndoManager().beginNewTransaction ("Load Haze patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (index, p->getParameter()), juce::sendNotification);
        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    juce::String getPatchName() const override      { return patchLabel (getPatchIndex()) + (isPatchModified() ? " *" : ""); }

    bool stepPatch (int delta) override
    {
        const int count = (int) patches().size();
        const int index = getPatchIndex();
        loadPatch (index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count);
        return true;
    }

    static juce::String patchLabel (int index)
    {
        if (index < 0 || index >= (int) patches().size())
            return "INIT";
        return "H" + juce::String (index + 1).paddedLeft ('0', 3) + " " + patches()[(size_t) index].name;
    }

    bool isPatchModified() const
    {
        const int index = getPatchIndex();
        if (index < 0 || index >= (int) patches().size())
            return true;
        for (auto& p : params)
        {
            auto& ap = p->getParameter();
            const float v = patchValue (index, ap);
            if (std::abs (ap.getCurrentBaseValue() - v) > 1.0e-3f * juce::jmax (1.0f, std::abs (v)))
                return true;
        }
        return false;
    }

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        engine.prepare (rate, info.blockSizeSamples);
        right.assign ((size_t) juce::jmax (info.blockSizeSamples, 512) * 2, 0.0f);
    }

    void deinitialise() override {}

    void midiPanic() override       { engine.allNotesOff(); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr || ! engine.isPrepared())
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        buf.clear (fc.bufferStartSample, fc.bufferNumSamples);
        updateEngine();

        int done = 0;
        if (fc.bufferForMidiMessages != nullptr)
        {
            if (fc.bufferForMidiMessages->isAllNotesOff)
                midiPanic();
            for (auto& m : *fc.bufferForMidiMessages)
            {
                auto pos = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * rate));
                if (pos > done) { render (buf, fc.bufferStartSample + done, pos - done); done = pos; }
                handleMidi (m);
            }
        }
        if (done < fc.bufferNumSamples)
            render (buf, fc.bufferStartSample + done, fc.bufferNumSamples - done);
    }

    /** The sound itself. */
    HazeEngine engine;

    // For the screen
    SynthCore::ScopeBuffer& scope = engine.scope;
    std::atomic<float>& wowNow = engine.wowNow;
    std::atomic<bool>& glitching = engine.glitching;
    std::atomic<double>& playSeconds = engine.playSeconds;
    int getActiveVoices() const                 { return engine.activeVoices.load(); }

    Param wave, detune, sub, noise, range, cutoff, resonance, envAmount, attack, decay, sustain, release;
    Param wow, flutter, age, speed, tracking, bandwidth, chroma, hiss, space, spaceMix, drift, volume;

private:
    double rate = 48000.0;
    std::vector<float> right;       // for a mono buffer

    /** The parameters as they are now (automation included) into the engine. */
    void updateEngine()
    {
        auto& p = engine.params;
        p.wave = wave.get(); p.detune = detune.get(); p.sub = sub.get(); p.noise = noise.get(); p.range = range.get();
        p.cutoff = cutoff.get(); p.resonance = resonance.get(); p.envAmount = envAmount.get();
        p.attack = attack.get(); p.decay = decay.get(); p.sustain = sustain.get(); p.release = release.get();
        p.wow = wow.get(); p.flutter = flutter.get(); p.age = age.get(); p.speed = speed.get();
        p.tracking = tracking.get(); p.bandwidth = bandwidth.get(); p.chroma = chroma.get(); p.hiss = hiss.get();
        p.space = space.get(); p.spaceMix = spaceMix.get(); p.drift = drift.get(); p.volume = volume.get();
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 engine.noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              engine.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isControllerOfType (1))      engine.setModWheel ((float) m.getControllerValue() / 127.0f);
        else if (m.isSustainPedalOn())          engine.setSustainPedal (true);
        else if (m.isSustainPedalOff())         engine.setSustainPedal (false);
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            midiPanic();
    }

    void render (juce::AudioBuffer<float>& buf, int start, int num)
    {
        auto* L = buf.getWritePointer (0, start);

        if (buf.getNumChannels() > 1)
            return engine.render (L, buf.getWritePointer (1, start), num);

        // A mono buffer: the right side goes nowhere
        for (int done = 0; done < num; )
        {
            const int n = juce::jmin ((int) right.size(), num - done);
            std::fill (right.begin(), right.begin() + n, 0.0f);
            engine.render (L + done, right.data(), n);
            done += n;
        }
    }
};

//==============================================================================
/**
    The screen: a flat VHS picture. The signal as a waveform, a little chroma fringe either
    side of it, scanlines that slide with the tape's wobble, a band of tracking noise in a
    glitch, and the on-screen display (PLAY, SP, the patch, the tape counter).
*/
class HazeScreen : public juce::Component, private juce::Timer
{
public:
    explicit HazeScreen (HazeDevice& d) : dev (d)   { startTimerHz (30); }

    juce::String patchName;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 6.0f);

        const float wobble = dev.wowNow.load();
        const auto ink = Theme::isLight() ? Colours::text : juce::Colour (0xffdde4f7);

        // A big screen (the device's own window) gets bigger type and more scanlines; in the chain it is as it was
        const float big = juce::jlimit (1.0f, 1.6f, r.getHeight() / 160.0f);
        auto pic = r.reduced (10.0f * big, 26.0f * big);

        // Scanlines, sliding sideways with the wobble
        const int lines = juce::jmax (8, juce::roundToInt (pic.getHeight() / 12.0f));
        for (int i = 0; i <= lines; ++i)
        {
            const float y = pic.getY() + pic.getHeight() * (float) i / (float) lines;
            const float shift = wobble * 6.0f * std::sin ((float) i * 0.9f + (float) frame * 0.05f);
            g.setColour (Colours::outline.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (pic.getX() + shift, y - 0.5f, pic.getWidth(), 1.0f));
        }

        // The signal, with a chroma fringe
        std::array<float, 256> s {};
        dev.scope.read (s.data(), (int) s.size());
        float peak = 0.0f;
        for (auto v : s) peak = juce::jmax (peak, std::abs (v));
        displayPeak = juce::jmax (peak, displayPeak * 0.97f);           // eases down so quiet tails don't jump
        const float gain = 0.9f / juce::jmax (0.08f, displayPeak);
        auto path = [&] (float dx)
        {
            juce::Path p;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const float x = pic.getX() + pic.getWidth() * (float) i / (float) (s.size() - 1) + dx
                              + wobble * 3.0f * std::sin ((float) i * 0.05f + (float) frame * 0.08f);
                const float y = pic.getCentreY() - juce::jlimit (-1.0f, 1.0f, s[i] * gain) * pic.getHeight() * 0.45f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        };
        const int chromaMode = juce::roundToInt (dev.chroma.get());
        if (chromaMode > 0)
        {
            const float fringe = 1.0f + (float) chromaMode;
            g.setColour (Colours::clips[4].withAlpha (0.55f));
            g.strokePath (path (-fringe * big), juce::PathStrokeType (1.4f * big));
            g.setColour (Colours::clips[6].withAlpha (0.55f));
            g.strokePath (path (fringe * big), juce::PathStrokeType (1.4f * big));
        }
        g.setColour (Theme::indicator (Colours::accent));
        g.strokePath (path (0.0f), juce::PathStrokeType (1.6f * big));

        // Tracking noise in a glitch
        if (dev.glitching.load())
        {
            juce::Random rng ((juce::int64) frame);
            const float bandY = pic.getY() + pic.getHeight() * (0.2f + 0.6f * rng.nextFloat());
            g.setColour (ink.withAlpha (0.6f));
            for (int i = 0; i < 40; ++i)
                g.fillRect (juce::Rectangle<float> (pic.getX() + rng.nextFloat() * pic.getWidth(), bandY + rng.nextFloat() * 14.0f,
                                                    4.0f + rng.nextFloat() * 22.0f, 1.5f));
        }

        // On-screen display
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f * big, juce::Font::bold));
        auto top = r.reduced (10.0f * big, 6.0f * big).removeFromTop (16.0f * big);
        auto bottom = r.reduced (10.0f * big, 6.0f * big).removeFromBottom (16.0f * big);
        const bool playing = dev.getActiveVoices() > 0;
        g.drawText (juce::String::fromUTF8 (playing ? "PLAY \xe2\x96\xb6" : "STOP \xe2\x96\xa0"), top, juce::Justification::centredLeft);
        g.drawText ("SP", top, juce::Justification::centredRight);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f * big, juce::Font::plain));
        const auto secs = (int) dev.playSeconds.load();
        g.drawText (juce::String (secs / 3600) + ":" + juce::String ((secs / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (secs % 60).paddedLeft ('0', 2),
                    bottom, juce::Justification::centredLeft);
        g.drawText (dev.glitching.load() ? juce::String ("TRACKING") : patchName.toUpperCase(), bottom, juce::Justification::centredRight);
    }

private:
    HazeDevice& dev;
    juce::uint32 frame = 0;
    float displayPeak = 0.0f;

    void timerCallback() override
    {
        ++frame;
        repaint();
    }
};

//==============================================================================
class HazeView : public SynthPanel::PanelBase, private juce::Timer
{
public:
    /** `inWindow`: the layout for the device's own window (the screen across the top, the sections in two rows). */
    HazeView (HazeDevice& d, bool inWindow = false) : dev (d), window (inWindow), screen (d)
    {
        if (window) setWindowRows (2);
        addAndMakeVisible (screen);
        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto cTone = Colours::accent, cFilt = Colours::clips[4], cEnv = Colours::clips[2], cTape = Colours::clips[6],
                   cVhs = Colours::clips[5], cHaze = Colours::clips[3];
        waveSel = selector (d.wave, HazeDevice::waveNames(), cTone);
        toneKnobs = { knob (d.detune, "Detune", cTone), knob (d.sub, "Sub", cTone), knob (d.noise, "Noise", cTone), knob (d.range, "Range", cTone) };
        filterKnobs = { knob (d.cutoff, "Cutoff", cFilt), knob (d.resonance, "Resonance", cFilt), knob (d.envAmount, "Env", cFilt) };
        envKnobs = { knob (d.attack, "Attack", cEnv), knob (d.decay, "Decay", cEnv), knob (d.sustain, "Sustain", cEnv), knob (d.release, "Release", cEnv) };
        tapeKnobs = { knob (d.wow, "Wow", cTape), knob (d.flutter, "Flutter", cTape), knob (d.age, "Age", cTape), knob (d.speed, "Speed", cTape) };
        chromaSel = selector (d.chroma, HazeDevice::chromaNames(), cVhs);
        vhsKnobs = { knob (d.tracking, "Tracking", cVhs), knob (d.bandwidth, "Bandwidth", cVhs), knob (d.hiss, "Hiss", cVhs) };
        hazeKnobs = { knob (d.space, "Space", cHaze), knob (d.spaceMix, "Mix", cHaze), knob (d.drift, "Drift", cHaze), knob (d.volume, "Volume", Colours::accent) };

        timerCallback();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, toneArea, "Tone", Colours::accent);
        SynthPanel::paintSection (g, filterArea, "Filter", Colours::clips[4]);
        SynthPanel::paintSection (g, envArea, "Envelope", Colours::clips[2]);
        SynthPanel::paintSection (g, tapeArea, "Tape", Colours::clips[6]);
        SynthPanel::paintSection (g, vhsArea, "VHS", Colours::clips[5]);
        SynthPanel::paintSection (g, hazeArea, "Haze", Colours::clips[3]);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (window)
        {
            // The picture across the top, the patch under its corner, then the sections
            auto display = r.removeFromTop (juce::jmax (150, r.getHeight() * 33 / 100));
            r.removeFromTop (6);
            auto nav = r.removeFromTop (24).removeFromLeft (300);
            r.removeFromTop (8);
            screen.setBounds (display);
            prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
            nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
            patchButton.setBounds (nav);
        }
        else
        {
            auto display = r.removeFromLeft (260);
            auto nav = display.removeFromBottom (24);
            display.removeFromBottom (4);
            screen.setBounds (display);
            prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
            nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
            patchButton.setBounds (nav);
            r.removeFromLeft (6);
        }

        auto cols = columns (r, { 176, 128, 128, 128, 176, 156 }, 6);
        toneArea = cols[0]; filterArea = cols[1]; envArea = cols[2]; tapeArea = cols[3]; vhsArea = cols[4]; hazeArea = cols[5];

        auto grid = [] (juce::Rectangle<int> c, std::vector<SynthPanel::Knob*>& k)
        {
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { k[0], k[1] }, 2);
            if (k.size() > 3) row (c, { k[2], k[3] }, 2);
            else row (c, { k[2] }, 2);
        };
        {
            auto c = content (toneArea);
            waveSel->setBounds (c.removeFromTop (18)); c.removeFromTop (3);
            grid (c, toneKnobs);
        }
        grid (content (filterArea), filterKnobs);
        grid (content (envArea), envKnobs);
        grid (content (tapeArea), tapeKnobs);
        {
            auto c = content (vhsArea);
            chromaSel->setBounds (c.removeFromTop (18)); c.removeFromTop (6);
            row (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 66)), { vhsKnobs[0], vhsKnobs[1], vhsKnobs[2] }, 3);
        }
        grid (content (hazeArea), hazeKnobs);
    }

private:
    HazeDevice& dev;
    const bool window;
    HazeScreen screen;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    SynthPanel::Selector *waveSel = nullptr, *chromaSel = nullptr;
    std::vector<SynthPanel::Knob*> toneKnobs, filterKnobs, envKnobs, tapeKnobs, vhsKnobs, hazeKnobs;
    juce::Rectangle<int> toneArea, filterArea, envArea, tapeArea, vhsArea, hazeArea;

    void step (int delta)
    {
        const int count = (int) HazeDevice::patches().size();
        const int index = dev.getPatchIndex();
        dev.loadPatch (index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count);
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int i = 0; i < (int) HazeDevice::patches().size(); ++i)
            m.addItem (i + 1, HazeDevice::patchLabel (i), true, i == dev.getPatchIndex());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<HazeView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
                safe->dev.loadPatch (result - 1);
        });
    }

    void timerCallback() override
    {
        const int index = dev.getPatchIndex();
        auto name = HazeDevice::patchLabel (index);
        if (index >= 0 && dev.isPatchModified())
            name << " *";
        screen.patchName = name;
    }
};

inline std::unique_ptr<juce::Component> HazeDevice::createDeviceView()     { return std::make_unique<HazeView> (*this); }
inline std::unique_ptr<juce::Component> HazeDevice::createWindowView()     { return std::make_unique<SynthPanel::Zoomed> (std::make_unique<HazeView> (*this, true), 1.25f, getWindowViewSize()); }

} // namespace elsewhere
