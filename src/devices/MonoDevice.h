// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "MonoEngine.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Mono: a monophonic analog-style synth with three models.

    Seventy (after the classic 70s three-oscillator monosynth)
      Controllers -> tune, glide, modulation (oscillator 3 and/or noise, to pitch and/or filter)
      Oscillators -> three, each with a footage range (LO, 32' ... 2') and six waveforms;
                     oscillator 3 can leave the keyboard and run as a slow modulator
      Mixer       -> the oscillators, noise (white/pink) and "feedback" (the output patched
                     back into the mixer, a famous trick for grit)
      Filter      -> 24 dB transistor ladder that saturates when the mixer is pushed and
                     self-oscillates at high emphasis, with its own contour
      Loudness    -> attack / decay / sustain; "Release" makes the release follow decay

    Acid (after the classic bassline machine)
      One oscillator (saw or square) into an 18 dB ladder with a decay-only envelope.
      Accented notes (played hard) are louder and brighter, and the accent "wow" builds
      up over runs of accents. Overlapping notes slide without retriggering.

    Sync Lead (after the classic 80s sync lead)
      Oscillator B is hard-synced to oscillator A; the filter envelope sweeps B's pitch for
      the tearing sync sound. Pulse width, ADSR filter and amp envelopes, and an LFO.

    The sound is MonoEngine's: the voice runs at four times the sample rate, so the saturating
    filter, the feedback and hard sync stay clean of aliasing.

    Character is the analog-ness. Every model gets a warm, slightly asymmetric output stage;
    Seventy also gets the oscillators' slightly curved ramps, the droop of their squares (AC
    coupling), cycle-to-cycle pitch jitter and a mixer that overloads into the filter when it's
    pushed. Seventy's filter is a zero-delay-feedback transistor ladder that saturates in
    every stage.
*/
class MonoDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereMono";

    enum Model { seventy = MonoEngine::seventy, acid = MonoEngine::acid, syncLead = MonoEngine::syncLead, numModels = MonoEngine::numModels };

    static juce::StringArray modelNames()       { return { "Seventy", "Acid", "Sync Lead" }; }
    static juce::StringArray rangeNames()       { return { "LO", "32'", "16'", "8'", "4'", "2'" }; }
    static juce::StringArray keyTrackNames()    { return { "Off", "1/3", "2/3", "Full" }; }
    static juce::StringArray priorityNames()    { return { "Last", "Low", "High" }; }
    static juce::StringArray lfoDestNames()     { return { "B Pitch", "Width", "Cutoff", "Vibrato" }; }

    /** Seventy: oscillators 1 and 2 have the "shark tooth" (tri-saw); oscillator 3 a reverse saw instead. */
    static std::vector<SynthCore::Wave> wavesFor (int osc)
    {
        std::vector<SynthCore::Wave> result;
        for (int i = 0; i < 6; ++i)
            result.push_back (MonoEngine::waveFor (osc, i));
        return result;
    }

    static juce::StringArray waveNames (int osc)
    {
        return { "Triangle", osc == 2 ? "Reverse Saw" : "Shark Tooth", "Sawtooth", "Square", "Wide Pulse", "Narrow Pulse" };
    }

    MonoDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto tenths = [] (float v) { return juce::String (v, 1); };
        auto semis = [] (float v) { return (v >= 0.0f ? "+" : "") + juce::String (v, 2) + " st"; };
        auto cents = [] (float v) { return (v >= 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        auto timeRange = skewed (0.001f, 10.0f, 0.4f);
        const MonoEngine::Params d;     // the defaults

        model = addParameter ("model", "Model", d.model, stepped (0, numModels - 1), pick (modelNames()));

        // ---- Seventy -------------------------------------------------------------
        tune      = addParameter ("tune", "Tune", d.tune, { -100.0f, 100.0f }, cents);
        glide     = addParameter ("glide", "Glide", d.glide, skewed (0.0f, 5.0f, 0.3f), secs);
        modMix    = addParameter ("modmix", "Mod Mix", d.modMix, { 0.0f, 100.0f },
                                  [] (float v) { return v < 1.0f ? juce::String ("Osc 3") : v > 99.0f ? juce::String ("Noise") : juce::String (juce::roundToInt (v)) + "% noise"; });
        modAmount = addParameter ("modamt", "Mod Amount", d.modAmount, { 0.0f, 100.0f }, percentText);
        oscMod    = addParameter ("oscmod", "Osc Mod", d.oscMod, stepped (0, 1), onOffText);
        filterMod = addParameter ("filtmod", "Filter Mod", d.filterMod, stepped (0, 1), onOffText);

        const float rangeD[] = { d.o1range, d.o2range, d.o3range }, waveD[] = { d.o1wave, d.o2wave, d.o3wave }, freqD[] = { 0.0f, d.o2freq, d.o3freq };
        for (int i = 0; i < 3; ++i)
        {
            auto& o = oscs[(size_t) i];
            const auto id = "o" + juce::String (i + 1), nm = "Osc " + juce::String (i + 1) + " ";
            o.range = addParameter (id + "range", nm + "Range", rangeD[i], stepped (0, 5), pick (rangeNames()));
            o.wave  = addParameter (id + "wave", nm + "Wave", waveD[i], stepped (0, 5), pick (waveNames (i)));
            if (i > 0)
                o.freq = addParameter (id + "freq", nm + "Frequency", freqD[i], { -7.0f, 7.0f }, semis);
        }
        osc3Key = addParameter ("o3key", "Osc 3 Keyboard", d.osc3Key, stepped (0, 1), onOffText);

        const float mixD[] = { d.m1, d.m2, d.m3 }, mixOnD[] = { d.m1on, d.m2on, d.m3on };
        for (int i = 0; i < 3; ++i)
        {
            const auto id = "m" + juce::String (i + 1);
            mix[(size_t) i]   = addParameter (id, "Osc " + juce::String (i + 1) + " Level", mixD[i], { 0.0f, 10.0f }, tenths);
            mixOn[(size_t) i] = addParameter (id + "on", "Osc " + juce::String (i + 1) + " On", mixOnD[i], stepped (0, 1), onOffText);
        }
        feedback    = addParameter ("fb", "Feedback", d.feedback, { 0.0f, 10.0f }, tenths);
        feedbackOn  = addParameter ("fbon", "Feedback On", d.feedbackOn, stepped (0, 1), onOffText);
        noise       = addParameter ("noise", "Noise Level", d.noise, { 0.0f, 10.0f }, tenths);
        noiseOn     = addParameter ("noiseon", "Noise On", d.noiseOn, stepped (0, 1), onOffText);
        noiseColour = addParameter ("noisecol", "Noise Colour", d.noiseColour, stepped (0, 1), pick ({ "White", "Pink" }));

        cutoff   = addParameter ("cutoff", "Cutoff", d.cutoff, freqRange (20.0f, 20000.0f), hzText);
        emphasis = addParameter ("emph", "Emphasis", d.emphasis, { 0.0f, 10.0f }, tenths);
        contour  = addParameter ("contour", "Contour Amount", d.contour, { 0.0f, 10.0f }, tenths);
        keyTrack = addParameter ("keytrack", "Keyboard Control", d.keyTrack, stepped (0, 3), pick (keyTrackNames()));
        fAttack  = addParameter ("fa", "Filter Attack", d.fAttack, timeRange, secs);
        fDecay   = addParameter ("fd", "Filter Decay", d.fDecay, timeRange, secs);
        fSustain = addParameter ("fs", "Filter Sustain", d.fSustain, { 0.0f, 10.0f }, tenths);

        aAttack  = addParameter ("aa", "Attack", d.aAttack, timeRange, secs);
        aDecay   = addParameter ("ad", "Decay", d.aDecay, timeRange, secs);
        aSustain = addParameter ("as", "Sustain", d.aSustain, { 0.0f, 10.0f }, tenths);
        release  = addParameter ("release", "Release", d.release, stepped (0, 1),
                                 [] (float v) { return v > 0.5f ? juce::String ("Follows decay") : juce::String ("Short"); });

        // ---- Acid ----------------------------------------------------------------
        acWave     = addParameter ("acwave", "Acid Wave", d.acWave, stepped (0, 1), pick ({ "Sawtooth", "Square" }));
        acTune     = addParameter ("actune", "Acid Tune", d.acTune, { -12.0f, 12.0f }, semis);
        acCutoff   = addParameter ("accutoff", "Acid Cutoff", d.acCutoff, freqRange (60.0f, 8000.0f), hzText);
        acRes      = addParameter ("acres", "Acid Resonance", d.acRes, { 0.0f, 10.0f }, tenths);
        acEnvMod   = addParameter ("acenv", "Env Mod", d.acEnvMod, { 0.0f, 10.0f }, tenths);
        acDecay    = addParameter ("acdecay", "Acid Decay", d.acDecay, skewed (0.1f, 3.0f, 0.6f), secs);
        acAccent   = addParameter ("acaccent", "Accent", d.acAccent, { 0.0f, 10.0f }, tenths);
        acSlide    = addParameter ("acslide", "Slide Time", d.acSlide, skewed (0.01f, 0.5f, 0.08f), secs);
        acDist     = addParameter ("acdist", "Distortion", d.acDist, { 0.0f, 100.0f }, percentText);

        // ---- Sync Lead -----------------------------------------------------------
        syAWave    = addParameter ("syawave", "Osc A Wave", d.syAWave, stepped (0, 1), pick ({ "Sawtooth", "Pulse" }));
        syWidth    = addParameter ("sywidth", "Pulse Width", d.syWidth, { 5.0f, 50.0f }, percentText);
        syBWave    = addParameter ("sybwave", "Osc B Wave", d.syBWave, stepped (0, 2), pick ({ "Sawtooth", "Pulse", "Triangle" }));
        syBPitch   = addParameter ("sybpitch", "Osc B Pitch", d.syBPitch, { 0.0f, 36.0f }, semis);
        sySync     = addParameter ("sysync", "Sync", d.sySync, stepped (0, 1), onOffText);
        syEnvToB   = addParameter ("syenvb", "Env to B Pitch", d.syEnvToB, { 0.0f, 48.0f }, semis);
        syMixA     = addParameter ("symixa", "Osc A Level", d.syMixA, { 0.0f, 10.0f }, tenths);
        syMixB     = addParameter ("symixb", "Osc B Level", d.syMixB, { 0.0f, 10.0f }, tenths);
        syNoise    = addParameter ("synoise", "Sync Noise", d.syNoise, { 0.0f, 10.0f }, tenths);
        syCutoff   = addParameter ("sycutoff", "Sync Cutoff", d.syCutoff, freqRange (20.0f, 20000.0f), hzText);
        syRes      = addParameter ("syres", "Sync Resonance", d.syRes, { 0.0f, 10.0f }, tenths);
        syEnvAmt   = addParameter ("syenv", "Filter Env", d.syEnvAmt, { 0.0f, 10.0f }, tenths);
        syKey      = addParameter ("sykey", "Sync Key Track", d.syKey, stepped (0, 3), pick (keyTrackNames()));
        syFA = addParameter ("syfa", "Filter Attack", d.syFA, timeRange, secs);
        syFD = addParameter ("syfd", "Filter Decay", d.syFD, timeRange, secs);
        syFS = addParameter ("syfs", "Filter Sustain", d.syFS, { 0.0f, 10.0f }, tenths);
        syFR = addParameter ("syfr", "Filter Release", d.syFR, timeRange, secs);
        syAA = addParameter ("syaa", "Amp Attack", d.syAA, timeRange, secs);
        syAD = addParameter ("syad", "Amp Decay", d.syAD, timeRange, secs);
        syAS = addParameter ("syas", "Amp Sustain", d.syAS, { 0.0f, 10.0f }, tenths);
        syAR = addParameter ("syar", "Amp Release", d.syAR, timeRange, secs);
        syLfoRate  = addParameter ("sylforate", "LFO Rate", d.syLfoRate, skewed (0.05f, 20.0f, 3.0f), [] (float v) { return juce::String (v, 2) + " Hz"; });
        syLfoAmt   = addParameter ("sylfoamt", "LFO Amount", d.syLfoAmt, { 0.0f, 100.0f }, percentText);
        syLfoDest  = addParameter ("sylfodest", "LFO Target", d.syLfoDest, stepped (0, 3), pick (lfoDestNames()));

        // ---- Output (all models) -------------------------------------------------
        volume   = addParameter ("volume", "Volume", d.volume, { -60.0f, 6.0f }, dbText);
        drive    = addParameter ("drive", "Drive", d.drive, { 0.0f, 100.0f }, percentText);
        drift    = addParameter ("drift", "Drift", d.drift, { 0.0f, 100.0f }, percentText);
        velocity = addParameter ("vel", "Velocity", d.velocity, { 0.0f, 100.0f }, percentText);
        bendRange = addParameter ("bend", "Bend Range", d.bendRange, stepped (1, 12), [] (float v) { return juce::String (juce::roundToInt (v)) + " st"; });
        legato   = addParameter ("legato", "Legato", d.legato, stepped (0, 1), onOffText);
        priority = addParameter ("priority", "Note Priority", d.priority, stepped (0, 2), pick (priorityNames()));
        character = addParameter ("character", "Character", d.character, { 0.0f, 100.0f }, percentText);

        // Each parameter and the engine control it drives, by ID
        for (auto& p : params)
        {
            auto* target = engine.params.find (p->getParameter().paramID.toRawUTF8());
            jassert (target != nullptr);
            if (target != nullptr)
                links.push_back ({ p.get(), target });
        }
    }

    juce::String getName() const override              { return "Elsewhere Mono"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Mono"; }
    int getDeviceViewWidth() const override            { return 1400; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1180, 620 }; }

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 3.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    double getLatencySeconds() override                 { return engine.isPrepared() ? engine.latencySamples() / rate : 0.0; }

    int getModel() const    { return juce::jlimit (0, numModels - 1, juce::roundToInt (model.get())); }

    //==============================================================================
    // Factory patches (MonoEngine keeps them, so anything playing the engine has the same ones):
    // bank A is Seventy, B is Acid, C is Sync Lead
    using Patch = MonoEngine::Patch;
    static const std::vector<Patch>& patches()          { return MonoEngine::patches(); }

    /** Global patch indices for a model, in order. */
    static std::vector<int> patchesFor (int m)
    {
        std::vector<int> result;
        for (int i = 0; i < (int) patches().size(); ++i)
            if (patches()[(size_t) i].model == m)
                result.push_back (i);
        return result;
    }

    /** "A001 Classic Bass": bank letter for the model, number within it. */
    static juce::String patchLabel (int index)
    {
        if (index < 0 || index >= (int) patches().size())
            return "INIT";

        const auto& p = patches()[(size_t) index];
        auto inModel = patchesFor (p.model);
        const int n = (int) (std::find (inModel.begin(), inModel.end(), index) - inModel.begin()) + 1;
        return juce::String::charToString ((juce::juce_wchar) ('A' + p.model)) + juce::String (n).paddedLeft ('0', 3) + " " + p.name;
    }

    float patchValue (int index, te::AutomatableParameter& ap) const
    {
        return MonoEngine::patchValue (index, ap.paramID.toRawUTF8());
    }

    /** Sets every parameter to the patch's value (or its default): as its own undo step, or as part of the one in progress. */
    void loadPatch (int index, bool ownUndoStep = true)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);

        if (ownUndoStep)
            edit.getUndoManager().beginNewTransaction ("Load Mono patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (index, p->getParameter()), juce::sendNotification);

        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    /** Whether the current settings still match the loaded patch. */
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
    MonoEngine engine;

    // For the display
    SynthCore::ScopeBuffer& scope = engine.scope;
    float getSamplesPerCycle() const    { return engine.samplesPerCycle.load(); }

    struct OscParams { Param range, wave, freq; };
    std::array<OscParams, 3> oscs;
    std::array<Param, 3> mix, mixOn;
    Param model;
    Param tune, glide, modMix, modAmount, oscMod, filterMod, osc3Key;
    Param feedback, feedbackOn, noise, noiseOn, noiseColour;
    Param cutoff, emphasis, contour, keyTrack, fAttack, fDecay, fSustain;
    Param aAttack, aDecay, aSustain, release;
    Param acWave, acTune, acCutoff, acRes, acEnvMod, acDecay, acAccent, acSlide, acDist;
    Param syAWave, syWidth, syBWave, syBPitch, sySync, syEnvToB, syMixA, syMixB, syNoise;
    Param syCutoff, syRes, syEnvAmt, syKey, syFA, syFD, syFS, syFR, syAA, syAD, syAS, syAR;
    Param syLfoRate, syLfoAmt, syLfoDest;
    Param volume, drive, drift, velocity, bendRange, legato, priority, character;

private:
    double rate = 44100.0;
    std::vector<float> right;       // for a mono buffer
    std::vector<std::pair<te::ParameterWithStateValue*, float*>> links;

    /** The parameters as they are now (automation included) into the engine. */
    void updateEngine()
    {
        for (auto& [p, target] : links)
            *target = p->getCurrentValue();
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
using MonoPanelBase = SynthPanel::PanelBase;

//==============================================================================
/** Seventy: controllers, oscillator bank, mixer, filter, loudness. */
class SeventyPanel : public MonoPanelBase
{
public:
    SeventyPanel (MonoDevice& d)
    {
        const auto cCtrl = Colours::clips[6], cOsc = Colours::accent, cMix = Colours::clips[5], cFilt = Colours::solo, cLoud = Colours::clips[2];

        ctrl = { knob (d.tune, "Tune", cCtrl), knob (d.glide, "Glide", cCtrl), knob (d.modMix, "Mod Mix", cCtrl), knob (d.modAmount, "Mod", cCtrl) };
        ctrlToggles = { toggle (d.oscMod, "Osc Mod", cCtrl), toggle (d.filterMod, "Filter Mod", cCtrl) };

        for (int i = 0; i < 3; ++i)
        {
            auto& o = d.oscs[(size_t) i];
            rangeSel[(size_t) i] = selector (o.range, MonoDevice::rangeNames(), cOsc);
            waveSel[(size_t) i] = waves (o.wave, MonoDevice::wavesFor (i), cOsc);
            if (i > 0) freqKnob[(size_t) i] = knob (o.freq, "Freq", cOsc);
        }
        osc3Key = toggle (d.osc3Key, "Key", cOsc);

        const char* mixNames[] = { "Osc 1", "Osc 2", "Osc 3" };
        for (int i = 0; i < 3; ++i)
        {
            mixToggles.push_back (toggle (d.mixOn[(size_t) i], mixNames[i], cMix));
            bars.push_back (bar (d.mix[(size_t) i], cMix));
        }
        mixToggles.push_back (toggle (d.feedbackOn, "Feedback", cMix)); bars.push_back (bar (d.feedback, cMix));
        mixToggles.push_back (toggle (d.noiseOn, "Noise", cMix));       bars.push_back (bar (d.noise, cMix));
        noiseColour = selector (d.noiseColour, { "W", "P" }, cMix);

        filt = { knob (d.cutoff, "Cutoff", cFilt), knob (d.emphasis, "Emphasis", cFilt), knob (d.contour, "Contour", cFilt), knob (d.keyTrack, "Key", cFilt),
                 knob (d.fAttack, "Attack", cFilt), knob (d.fDecay, "Decay", cFilt), knob (d.fSustain, "Sustain", cFilt) };
        loud = { knob (d.aAttack, "Attack", cLoud), knob (d.aDecay, "Decay", cLoud), knob (d.aSustain, "Sustain", cLoud) };
        releaseToggle = toggle (d.release, "Release", cLoud);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, areas[0], "Controllers", Colours::clips[6]);
        SynthPanel::paintSection (g, areas[1], "Oscillators", Colours::accent);
        SynthPanel::paintSection (g, areas[2], "Mixer", Colours::clips[5]);
        SynthPanel::paintSection (g, areas[3], "Filter", Colours::solo);
        SynthPanel::paintSection (g, areas[4], "Loudness", Colours::clips[2]);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        for (int i = 0; i < 3; ++i)
            g.drawText (juce::String (i + 1), oscRows[(size_t) i].withWidth (14), juce::Justification::centred);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 118, 320, 166, 220, 140 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        {
            auto c = content (areas[0]);
            auto toggles = c.removeFromBottom (36);
            const int w = c.getWidth() / 2, h = c.getHeight() / 2;
            for (size_t i = 0; i < ctrl.size(); ++i)
                ctrl[i]->setBounds (c.getX() + (int) (i % 2) * w, c.getY() + (int) (i / 2) * h, w, h);
            for (auto* t : ctrlToggles) t->setBounds (toggles.removeFromTop (18));
        }

        {
            auto c = content (areas[1]);
            osc3Key->setBounds (areas[1].withHeight (16).removeFromRight (52).translated (-6, 0));
            const int rowH = c.getHeight() / 3;
            for (int i = 0; i < 3; ++i)
            {
                auto r = c.removeFromTop (rowH);
                oscRows[(size_t) i] = r;
                r.removeFromLeft (16);
                const int selH = juce::jmin (20, r.getHeight() - 8);
                rangeSel[(size_t) i]->setBounds (r.removeFromLeft (124).withSizeKeepingCentre (124, selH));
                r.removeFromLeft (4);
                auto knobArea = r.removeFromLeft (46);
                if (freqKnob[(size_t) i] != nullptr) freqKnob[(size_t) i]->setBounds (knobArea);
                r.removeFromLeft (4);
                waveSel[(size_t) i]->setBounds (r.withSizeKeepingCentre (r.getWidth(), selH));
            }
        }

        {
            auto c = content (areas[2]);
            const int rowH = c.getHeight() / 5;
            for (int i = 0; i < 5; ++i)
            {
                auto r = c.removeFromTop (rowH);
                // (no taller than in the chain: a toggle's dot takes a square off its left)
                mixToggles[(size_t) i]->setBounds (r.removeFromLeft (70).withSizeKeepingCentre (70, juce::jmin (rowH, 30)));
                if (i == 4)
                {
                    noiseColour->setBounds (r.removeFromRight (30).withSizeKeepingCentre (30, juce::jmin (16, rowH - 4)));
                    r.removeFromRight (4);
                }
                bars[(size_t) i]->setBounds (r);
            }
        }

        {
            auto c = content (areas[3]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { filt[0], filt[1], filt[2], filt[3] }, 4);
            row (c, { filt[4], filt[5], filt[6] }, 4);
        }

        {
            auto c = content (areas[4]);
            auto bottom = c.removeFromBottom (c.getHeight() / 2);
            row (c, { loud[0], loud[1], loud[2] }, 3);
            releaseToggle->setBounds (bottom.removeFromTop (20).reduced (4, 0));
        }
    }

private:
    std::array<juce::Rectangle<int>, 5> areas;
    std::array<juce::Rectangle<int>, 3> oscRows;
    std::vector<SynthPanel::Knob*> ctrl, filt, loud;
    std::vector<SynthPanel::Toggle*> ctrlToggles, mixToggles;
    std::vector<SynthPanel::BarSlider*> bars;
    std::array<SynthPanel::Selector*, 3> rangeSel {}, waveSel {};
    std::array<SynthPanel::Knob*, 3> freqKnob {};
    SynthPanel::Toggle* osc3Key = nullptr;
    SynthPanel::Toggle* releaseToggle = nullptr;
    SynthPanel::Selector* noiseColour = nullptr;
};

//==============================================================================
/** Acid: one row of big controls, like the original's panel. */
class AcidPanel : public MonoPanelBase
{
public:
    AcidPanel (MonoDevice& d)
    {
        const auto cOsc = Colours::accent, cFilt = Colours::solo, cAcc = Colours::clips[6], cDist = Colours::record;
        waveSel = waves (d.acWave, { SynthCore::Wave::saw, SynthCore::Wave::square }, cOsc);
        tune = knob (d.acTune, "Tune", cOsc);
        filt = { knob (d.acCutoff, "Cutoff", cFilt), knob (d.acRes, "Resonance", cFilt), knob (d.acEnvMod, "Env Mod", cFilt), knob (d.acDecay, "Decay", cFilt) };
        acc = { knob (d.acAccent, "Accent", cAcc), knob (d.acSlide, "Slide", cAcc) };
        dist = knob (d.acDist, "Distortion", cDist);

        // Big knobs, like the original's single row
        for (auto* k : { tune, filt[0], filt[1], filt[2], filt[3], acc[0], acc[1], dist })
            k->maxSize = 56;
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, areas[0], "Oscillator", Colours::accent);
        SynthPanel::paintSection (g, areas[1], "Filter", Colours::solo);
        SynthPanel::paintSection (g, areas[2], "Accent & Slide", Colours::clips[6]);
        SynthPanel::paintSection (g, areas[3], "Distortion", Colours::record);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawFittedText ("Play hard (velocity 100+) to accent a note; overlap notes to slide between them.",
                          hint, juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 170, 440, 230, 150 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        auto big = [] (juce::Rectangle<int> c) { return c.withTrimmedBottom (c.getHeight() / 4); };

        {
            auto c = content (areas[0]);
            waveSel->setBounds (c.removeFromTop (24).reduced (4, 0));
            tune->setBounds (big (c));
        }
        { auto c = content (areas[1]); row (big (c), { filt[0], filt[1], filt[2], filt[3] }, 4); }
        {
            auto c = content (areas[2]);
            hint = c.removeFromBottom (c.getHeight() / 4).reduced (4, 0);
            row (c, { acc[0], acc[1] }, 2);
        }
        { auto c = content (areas[3]); dist->setBounds (big (c)); }
    }

private:
    std::array<juce::Rectangle<int>, 4> areas;
    juce::Rectangle<int> hint;
    SynthPanel::Selector* waveSel = nullptr;
    SynthPanel::Knob* tune = nullptr;
    SynthPanel::Knob* dist = nullptr;
    std::vector<SynthPanel::Knob*> filt, acc;
};

//==============================================================================
/** Sync Lead: two oscillators, mixer, filter, amp, LFO. */
class SyncPanel : public MonoPanelBase
{
public:
    SyncPanel (MonoDevice& d)
    {
        const auto cOsc = Colours::accent, cMix = Colours::clips[5], cFilt = Colours::solo, cAmp = Colours::clips[2], cLfo = Colours::clips[6];
        aWave = waves (d.syAWave, { SynthCore::Wave::saw, SynthCore::Wave::square }, cOsc);
        aKnobs = { knob (d.syWidth, "Width", cOsc), knob (d.glide, "Glide", cOsc) };
        bWave = waves (d.syBWave, { SynthCore::Wave::saw, SynthCore::Wave::square, SynthCore::Wave::triangle }, cOsc);
        bKnobs = { knob (d.syBPitch, "Pitch", cOsc), knob (d.syEnvToB, "Env > B", cOsc) };
        syncToggle = toggle (d.sySync, "Sync", cOsc);

        mixNames = { "Osc A", "Osc B", "Noise" };
        bars = { bar (d.syMixA, cMix), bar (d.syMixB, cMix), bar (d.syNoise, cMix) };

        filt = { knob (d.syCutoff, "Cutoff", cFilt), knob (d.syRes, "Resonance", cFilt), knob (d.syEnvAmt, "Env", cFilt), knob (d.syKey, "Key", cFilt),
                 knob (d.syFA, "A", cFilt), knob (d.syFD, "D", cFilt), knob (d.syFS, "S", cFilt), knob (d.syFR, "R", cFilt) };
        amp = { knob (d.syAA, "A", cAmp), knob (d.syAD, "D", cAmp), knob (d.syAS, "S", cAmp), knob (d.syAR, "R", cAmp) };
        lfo = { knob (d.syLfoRate, "Rate", cLfo), knob (d.syLfoAmt, "Amount", cLfo) };
        lfoDest = selector (d.syLfoDest, { "B", "PW", "Cut", "Vib" }, cLfo);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, areas[0], "Osc A", Colours::accent);
        SynthPanel::paintSection (g, areas[1], "Osc B", Colours::accent);
        SynthPanel::paintSection (g, areas[2], "Mixer", Colours::clips[5]);
        SynthPanel::paintSection (g, areas[3], "Filter", Colours::solo);
        SynthPanel::paintSection (g, areas[4], "Amp", Colours::clips[2]);
        SynthPanel::paintSection (g, areas[5], "LFO", Colours::clips[6]);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        for (int i = 0; i < 3; ++i)
            g.drawText (mixNames[i], mixRows[(size_t) i].withWidth (44), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 130, 190, 140, 250, 140, 130 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        {
            auto c = content (areas[0]);
            aWave->setBounds (c.removeFromTop (22).reduced (2, 0));
            c.removeFromTop (4);
            row (c, { aKnobs[0], aKnobs[1] }, 2);
        }
        {
            auto c = content (areas[1]);
            auto top = c.removeFromTop (22);
            syncToggle->setBounds (top.removeFromRight (54));
            top.removeFromRight (4);
            bWave->setBounds (top.reduced (2, 0));
            c.removeFromTop (4);
            row (c, { bKnobs[0], bKnobs[1] }, 2);
        }
        {
            auto c = content (areas[2]);
            const int rowH = c.getHeight() / 3;
            for (int i = 0; i < 3; ++i)
            {
                auto r = c.removeFromTop (rowH);
                mixRows[(size_t) i] = r;
                bars[(size_t) i]->setBounds (r.withTrimmedLeft (46));
            }
        }
        {
            auto c = content (areas[3]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { filt[0], filt[1], filt[2], filt[3] }, 4);
            row (c, { filt[4], filt[5], filt[6], filt[7] }, 4);
        }
        {
            auto c = content (areas[4]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { amp[0], amp[1] }, 2);
            row (c, { amp[2], amp[3] }, 2);
        }
        {
            auto c = content (areas[5]);
            lfoDest->setBounds (c.removeFromBottom (20));
            c.removeFromBottom (4);
            row (c, { lfo[0], lfo[1] }, 2);
        }
    }

private:
    std::array<juce::Rectangle<int>, 6> areas;
    std::array<juce::Rectangle<int>, 3> mixRows;
    juce::StringArray mixNames;
    SynthPanel::Selector *aWave = nullptr, *bWave = nullptr, *lfoDest = nullptr;
    SynthPanel::Toggle* syncToggle = nullptr;
    std::vector<SynthPanel::Knob*> aKnobs, bKnobs, filt, amp, lfo;
    std::vector<SynthPanel::BarSlider*> bars;
};

//==============================================================================
/** The Mono: model choice and display on the left, the model's panel, then the output. */
class MonoView : public MonoPanelBase, private juce::Timer
{
public:
    /** `inWindow`: the layout for the device's own window (a big display, the sections in two rows). */
    MonoView (MonoDevice& d, bool inWindow = false) : mono (d), window (inWindow)
    {
        if (window) setWindowRows (2);
        modelSel = selector (d.model, MonoDevice::modelNames(), Colours::accent);

        oled.readScope = [this] (float* dst, int n) { mono.scope.read (dst, n); };
        oled.getSamplesPerCycle = [this] { return mono.getSamplesPerCycle(); };
        addAndMakeVisible (oled);
        onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };

        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto cOut = Colours::accent;
        out = { knob (d.volume, "Volume", cOut), knob (d.drive, "Drive", cOut), knob (d.drift, "Drift", cOut),
                knob (d.velocity, "Velocity", cOut), knob (d.bendRange, "Bend", cOut), knob (d.character, "Character", cOut) };
        legatoToggle = toggle (d.legato, "Legato", cOut);
        prioritySel = selector (d.priority, MonoDevice::priorityNames(), cOut);

        rebuildBody();
        updatePatchName();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, outArea, "Output", Colours::accent);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = window ? 8 : 6;

        auto display = r.removeFromLeft (window ? juce::jmin (300, r.getWidth() * 32 / 100) : 200);
        modelSel->setBounds (display.removeFromTop (window ? 22 : 20));
        display.removeFromTop (window ? 6 : 4);
        if (window)
        {
            // The output sits under the display, leaving the rest of the window to the model's panel
            outArea = display.removeFromBottom (186);
            display.removeFromBottom (gap);
        }
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (window ? 6 : 4);
        oled.setBounds (display);
        prevButton.setBounds (nav.removeFromLeft (30));
        nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30));
        nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (gap);

        if (! window)
        {
            outArea = r.removeFromRight (186);
            r.removeFromRight (gap);
        }
        // A narrow window: a third row, so the sections keep their width
        if (window && body != nullptr) body->setWindowRows (r.getWidth() < 500 ? 3 : 2);
        if (body != nullptr) body->setBounds (r);
        sections = { outArea };

        auto c = content (outArea);
        auto bottom = c.removeFromBottom (22);
        auto top = c.removeFromTop (c.getHeight() / 2);
        row (top, { out[0], out[1], out[2] }, 3);
        row (c, { out[3], out[4], out[5] }, 3);
        legatoToggle->setBounds (bottom.removeFromLeft (64));
        prioritySel->setBounds (bottom.withSizeKeepingCentre (bottom.getWidth(), 18));
    }

private:
    MonoDevice& mono;
    const bool window;
    SynthPanel::Oled oled;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    std::unique_ptr<MonoPanelBase> body;
    int bodyModel = -1;
    SynthPanel::Selector* modelSel = nullptr;
    SynthPanel::Selector* prioritySel = nullptr;
    SynthPanel::Toggle* legatoToggle = nullptr;
    std::vector<SynthPanel::Knob*> out;
    juce::Rectangle<int> outArea;

    void rebuildBody()
    {
        bodyModel = mono.getModel();
        body.reset();
        switch (bodyModel)
        {
            case MonoDevice::acid:      body = std::make_unique<AcidPanel> (mono); break;
            case MonoDevice::syncLead:  body = std::make_unique<SyncPanel> (mono); break;
            default:                    body = std::make_unique<SeventyPanel> (mono); break;
        }
        body->onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };
        if (window) body->setWindowRows (2);
        addAndMakeVisible (*body);
        oled.header = MonoDevice::modelNames()[bodyModel].toUpperCase() + "  /  MONO";
        resized();
    }

    void step (int delta)
    {
        auto list = MonoDevice::patchesFor (mono.getModel());
        if (list.empty()) return;
        auto it = std::find (list.begin(), list.end(), mono.getPatchIndex());
        int pos = it == list.end() ? (delta > 0 ? -1 : 0) : (int) (it - list.begin());
        pos = (pos + delta + (int) list.size()) % (int) list.size();
        mono.loadPatch (list[(size_t) pos]);
        updatePatchName();
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int model = 0; model < MonoDevice::numModels; ++model)
        {
            juce::PopupMenu sub;
            for (auto i : MonoDevice::patchesFor (model))
                sub.addItem (i + 1, MonoDevice::patchLabel (i), true, i == mono.getPatchIndex());
            m.addSubMenu (MonoDevice::modelNames()[model], sub, true, nullptr, model == mono.getModel());
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<MonoView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
            {
                safe->mono.loadPatch (result - 1);
                safe->updatePatchName();
            }
        });
    }

    void updatePatchName()
    {
        const int index = mono.getPatchIndex();
        auto name = MonoDevice::patchLabel (index);
        if (index >= 0 && mono.isPatchModified())
            name << " *";
        oled.patchName = name;
    }

    void timerCallback() override
    {
        if (mono.getModel() != bodyModel)
            rebuildBody();
        updatePatchName();
    }
};

inline std::unique_ptr<juce::Component> MonoDevice::createDeviceView()     { return std::make_unique<MonoView> (*this); }
inline std::unique_ptr<juce::Component> MonoDevice::createWindowView()     { return std::make_unique<SynthPanel::Zoomed> (std::make_unique<MonoView> (*this, true), 1.25f, getWindowViewSize()); }

} // namespace elsewhere
