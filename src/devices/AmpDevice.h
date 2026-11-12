// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "Dsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Amp: guitar/bass amp and cabinet simulator.

    Signal path (per channel):
      noise gate -> input filter -> preamp gain stages (4x oversampled, asymmetric
      clipping, more stages for higher-gain models) -> tone stack (bass/mid/treble)
      -> presence -> power amp with sag -> cabinet + mic -> room -> output

    Amp models:  Clean, Crunch, Lead, Heavy, Bass
    Cabinets:    Off (DI), 1x12 Open, 2x12, 4x12, Bass 1x15
*/
class AmpDevice : public ElsewhereDevice,
                  private juce::Timer
{
public:
    static constexpr const char* xmlTypeName = "elsewhereAmp";
    enum Model { clean, crunch, lead, heavy, bass, numModels };
    enum Cab { cabOff, cab1x12, cab2x12, cab4x12, cabBass, cabCustom, numCabs };

    static juce::StringArray modelNames()   { return { "Clean", "Crunch", "Lead", "Heavy", "Bass" }; }
    static juce::StringArray cabNames()     { return { "No Cab", "1x12 Open", "2x12", "4x12", "Bass 1x15", "Custom IR" }; }
    static juce::StringArray cabShortNames(){ return { "Off", "1x12", "2x12", "4x12", "Bass", "IR File" }; }
    static inline const juce::Identifier customIrProp { "cabIr" };

    AmpDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto knobText = [] (float v) { return juce::String (v, 1); };
        model    = addParameter ("model", "Amp", (float) crunch, stepped (0, numModels - 1),
                                 [] (float v) { return modelNames()[juce::jlimit (0, numModels - 1, juce::roundToInt (v))]; });
        gain     = addParameter ("gain", "Gain", 5.0f, { 0.0f, 10.0f }, knobText);
        bassK    = addParameter ("bass", "Bass", 5.0f, { 0.0f, 10.0f }, knobText);
        mid      = addParameter ("mid", "Mid", 5.0f, { 0.0f, 10.0f }, knobText);
        treble   = addParameter ("treble", "Treble", 5.0f, { 0.0f, 10.0f }, knobText);
        presence = addParameter ("presence", "Presence", 5.0f, { 0.0f, 10.0f }, knobText);
        master   = addParameter ("master", "Master", 5.0f, { 0.0f, 10.0f }, knobText);
        cab      = addParameter ("cab", "Cabinet", (float) cab2x12, stepped (0, numCabs - 1),
                                 [] (float v) { return cabNames()[juce::jlimit (0, numCabs - 1, juce::roundToInt (v))]; });
        mic      = addParameter ("mic", "Mic Position", 30.0f, { 0.0f, 100.0f },
                                 [] (float v) { return v < 20 ? juce::String ("Centre") : v > 80 ? juce::String ("Edge") : juce::String (juce::roundToInt (v)) + " %"; });
        room     = addParameter ("room", "Room", 15.0f, { 0.0f, 100.0f }, percentText);
        gate     = addParameter ("gate", "Gate", -80.0f, { -80.0f, -20.0f },
                                 [] (float v) { return v <= -79.9f ? juce::String ("Off") : juce::String (v, 0) + " dB"; });
        output   = addParameter ("output", "Output", 0.0f, { -24.0f, 12.0f }, dbText);

        startTimerHz (10);    // rebuilds the cabinet IR when the cab, mic or IR file changes
    }

    ~AmpDevice() override   { stopTimer(); }

    juce::String getName() const override              { return "Elsewhere Amp"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Amp"; }
    int getDeviceViewWidth() const override            { return 670; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    double getLatencySeconds() override
    {
        // Constant whatever the cab setting (with no cab the convolver passes the sound straight
        // through), so changing the cab never shifts the track's timing
        return ((oversampler != nullptr ? oversampler->getLatencyInSamples() : 0.0) + Convolver::partition) / rate;
    }

    //==============================================================================
    /** Uses an impulse response file for the cabinet (copied into the project). */
    void loadCustomIr (const juce::File& file)
    {
        auto f = importFileHook ? importFileHook (file) : file;
        juce::String path = f.getFullPathName();

        if (auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File(); editFile != juce::File())
            if (f.isAChildOf (editFile.getParentDirectory()))
                path = f.getRelativePathFrom (editFile.getParentDirectory());

        state.setProperty (customIrProp, path, getUndoManager());
        cab.param().setParameter ((float) cabCustom, juce::sendNotification);
        rebuildIrIfNeeded (false);
    }

    juce::String getCustomIrName() const
    {
        auto p = state.getProperty (customIrProp).toString();
        return p.isEmpty() ? juce::String() : juce::File::createFileWithoutCheckingPath (p).getFileNameWithoutExtension();
    }

    bool isCustomIrMissing() const      { return customIrMissing.load(); }

    /** The current cabinet IR (left channel), for display and tests. */
    std::vector<float> getCurrentIr() const
    {
        const juce::ScopedLock sl (irLock);
        return currentIrL;
    }

    /** Builds one of the built-in cabinet impulse responses (see makeCabIr). */
    static std::vector<float> makeCabIr (Cab c, float micPos, double sr);

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        rate = info.sampleRate;
        rebuildIrIfNeeded (true);
        maxBlock = juce::jmax (64, info.blockSizeSamples);
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, osFactorLog2,
                          juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampler->initProcessing ((size_t) maxBlock);
        work.setSize (2, maxBlock);

        for (auto& ch : chans) ch = {};
        roomLine.setSize ((int) (0.08 * rate) + 8);
        lastHash = 0;
    }

    void deinitialise() override    { oversampler.reset(); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || oversampler == nullptr)
            return;

        juce::ScopedNoDenormals noDenormals;
        updateFilters();

        for (int done = 0; done < fc.bufferNumSamples;)
        {
            auto n = juce::jmin (maxBlock, fc.bufferNumSamples - done);
            processChunk (*fc.destBuffer, fc.bufferStartSample + done, n);
            done += n;
        }
    }

    /** Output level for the UI meter. */
    float takeOutputPeak()      { return outPeak.exchange (0.0f); }
    float takeInputPeak()       { return inPeak.exchange (0.0f); }

    Param model, gain, bassK, mid, treble, presence, master, cab, mic, room, gate, output;

private:
    static constexpr int osFactorLog2 = 2;      // 4x

    struct Channel
    {
        float gateEnv = 0.0f, gateGain = 1.0f, sag = 0.0f;
        Biquad inHp, tight, interStage, interLp, bassF, midF, trebleF, presenceF;
        float dcX = 0.0f, dcY = 0.0f;
    };

    // Cabinet impulse responses
    std::array<Convolver, 2> convolvers;
    juce::CriticalSection irLock;
    std::vector<float> currentIrL;
    struct IrKey
    {
        int cab = -1, mic = -1, rateHz = 0;     // mic in whole percent
        juce::String file;
        bool operator== (const IrKey& o) const  { return cab == o.cab && mic == o.mic && rateHz == o.rateHz && file == o.file; }
    };
    IrKey builtKey;
    std::atomic<bool> customIrMissing { false };

    void timerCallback() override   { rebuildIrIfNeeded (false); }

    IrKey wantedKey() const
    {
        IrKey k;
        k.cab = (int) currentCab();
        k.mic = k.cab == cabCustom || k.cab == cabOff ? 0 : juce::roundToInt (mic.get());      // 1 % steps
        k.file = k.cab == cabCustom ? state.getProperty (customIrProp).toString() : juce::String();
        k.rateHz = juce::roundToInt (rate);
        return k;
    }

    /** Builds the IR for the current settings. immediate = before the audio thread runs. */
    void rebuildIrIfNeeded (bool immediate)
    {
        const juce::ScopedLock sl (irLock);
        auto key = wantedKey();

        if (key == builtKey && ! immediate)
            return;

        builtKey = key;
        std::vector<float> irL, irR;
        customIrMissing = false;

        if (key.cab == cabCustom)
        {
            if (! readIrFile (key.file, irL, irR))
            {
                customIrMissing = key.file.isNotEmpty();
                irL = { 1.0f };      // no file yet: pass straight through
            }
        }
        else if (key.cab == cabOff)
        {
            irL = { 1.0f };
        }
        else
        {
            irL = makeCabIr ((Cab) key.cab, (float) key.mic / 100.0f, rate);
        }

        if (irR.empty())
            irR = irL;

        currentIrL = irL;

        if (immediate)
        {
            convolvers[0].setIrNow (Convolver::makeIr (irL.data(), (int) irL.size()));
            convolvers[1].setIrNow (Convolver::makeIr (irR.data(), (int) irR.size()));
        }
        else
        {
            convolvers[0].setIr (Convolver::makeIr (irL.data(), (int) irL.size()));
            convolvers[1].setIr (Convolver::makeIr (irR.data(), (int) irR.size()));
        }
    }

    /** Reads an IR file, resampled to our rate, trimmed to ~1 s and level-matched. */
    bool readIrFile (const juce::String& path, std::vector<float>& left, std::vector<float>& right) const
    {
        if (path.isEmpty())
            return false;

        auto file = edit.filePathResolver ? edit.filePathResolver (path) : juce::File (path);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (file.existsAsFile() ? fm.createReaderFor (file) : nullptr);

        if (reader == nullptr || reader->lengthInSamples < 1)
            return false;

        const auto maxIn = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 1.0));
        juce::AudioBuffer<float> in ((int) juce::jmin (2u, reader->numChannels), maxIn);
        reader->read (&in, 0, maxIn, 0, true, in.getNumChannels() > 1);

        const double ratio = reader->sampleRate / rate;
        const int outLen = juce::jmin ((int) (Convolver::maxParts * Convolver::partition), juce::jmax (1, (int) std::ceil (maxIn / ratio)));

        auto resample = [&] (int ch)
        {
            std::vector<float> out ((size_t) outLen);
            if (std::abs (ratio - 1.0) < 1.0e-9)
            {
                for (int i = 0; i < outLen && i < maxIn; ++i) out[(size_t) i] = in.getSample (ch, i);
            }
            else
            {
                juce::LagrangeInterpolator interp;
                interp.process (ratio, in.getReadPointer (ch), out.data(), outLen, maxIn, 0);
            }
            return out;
        };

        left = resample (0);
        right = in.getNumChannels() > 1 ? resample (1) : std::vector<float>();

        // Level-match using the left channel, applying the same gain to both
        auto matched = left;
        normaliseIr (matched, rate);
        const float g = gainOf (left, matched);
        for (auto& v : left) v *= g;
        for (auto& v : right) v *= g;
        return true;
    }

    static float gainOf (const std::vector<float>& before, const std::vector<float>& after)
    {
        for (size_t i = 0; i < before.size(); ++i)
            if (std::abs (before[i]) > 1.0e-9f)
                return after[i] / before[i];
        return 1.0f;
    }

    double rate = 44100.0;
    int maxBlock = 512;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> work;
    std::array<Channel, 2> chans;
    static constexpr float cabLevel = 0.5f;     // cab IRs are level-matched; this keeps peaks similar to no cab
    DelayLine roomLine;
    size_t lastHash = 0;
    std::atomic<float> outPeak { 0.0f }, inPeak { 0.0f };

    Model currentModel() const  { return (Model) juce::jlimit (0, numModels - 1, juce::roundToInt (model.get())); }
    Cab currentCab() const      { return (Cab) juce::jlimit (0, numCabs - 1, juce::roundToInt (cab.get())); }

    struct ModelSpec
    {
        float minGainDb, maxGainDb;     // preamp gain range across the knob
        int stages;                     // preamp clipping stages
        float bias;                     // asymmetry (even harmonics)
        float tightHz;                  // low cut before the gain stages
        float midHz;                    // tone stack mid frequency
        float toneRange;                // dB range of the tone knobs
        float powerDrive;               // how hard the power amp is hit at master 10
    };

    static ModelSpec spec (Model m)
    {
        switch (m)
        {
            case clean:  return { -6.0f, 20.0f, 1, 0.05f, 60.0f,  500.0f, 12.0f, 2.0f };
            case crunch: return { 6.0f,  34.0f, 2, 0.25f, 120.0f, 650.0f, 12.0f, 3.0f };
            case lead:   return { 14.0f, 46.0f, 2, 0.15f, 180.0f, 750.0f, 12.0f, 3.5f };
            case heavy:  return { 20.0f, 54.0f, 3, 0.10f, 250.0f, 800.0f, 15.0f, 4.0f };
            case bass:   return { -3.0f, 30.0f, 1, 0.20f, 30.0f,  450.0f, 15.0f, 2.5f };
            case numModels: break;
        }
        return {};
    }

    void updateFilters()
    {
        size_t h = std::hash<double>{} (rate);
        for (float v : { model.get(), bassK.get(), mid.get(), treble.get(), presence.get() })
            h = h * 31 + std::hash<float>{} (v);

        if (h == lastHash)
            return;

        lastHash = h;
        const auto s = spec (currentModel());
        const double osRate = rate * (1 << osFactorLog2);
        auto knobDb = [&] (float k) { return (k - 5.0f) / 5.0f * s.toneRange; };

        using T = Biquad::Type;

        for (auto& c : chans)
        {
            // These run at the oversampled rate (inside the distortion)
            c.inHp.set (T::highPass, 60.0, 0.7, 0, osRate);
            c.tight.set (T::highPass, s.tightHz, 0.7, 0, osRate);
            c.interStage.set (T::highPass, 180.0, 0.6, 0, osRate);
            c.interLp.set (T::lowPass, currentModel() == heavy ? 6500.0 : 8000.0, 0.7, 0, osRate);

            // Tone stack etc. at the normal rate
            c.bassF.set (T::lowShelf, 110.0, 0.7, knobDb (bassK.get()), rate);
            c.midF.set (T::bell, s.midHz, 0.8, knobDb (mid.get()), rate);
            c.trebleF.set (T::highShelf, 2800.0, 0.7, knobDb (treble.get()), rate);
            c.presenceF.set (T::highShelf, 4500.0, 0.7, (presence.get() - 5.0f) * 1.6f, rate);
        }
    }

    static float clipStage (float x, float bias) noexcept
    {
        // Asymmetric soft clip (like a biased triode): tanh around an offset, DC removed
        return std::tanh (x + bias) - std::tanh (bias);
    }

    void processChunk (juce::AudioBuffer<float>& buf, int start, int n)
    {
        const int numCh = juce::jmin (2, buf.getNumChannels());
        if (numCh == 0) return;

        const auto m = currentModel();
        const auto s = spec (m);
        const float preGain = juce::Decibels::decibelsToGain (juce::jmap (gain.get(), 0.0f, 10.0f, s.minGainDb, s.maxGainDb));
        const float gateThresh = gate.get() <= -79.9f ? 0.0f : juce::Decibels::decibelsToGain (gate.get());
        const float gateAtk = std::exp (-1.0f / (0.001f * (float) rate)), gateRel = std::exp (-1.0f / (0.08f * (float) rate));

        float inP = 0.0f;

        // 1. Gate + copy into the work buffer
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& c = chans[(size_t) ch];
            auto* src = buf.getReadPointer (juce::jmin (ch, numCh - 1), start);
            auto* dst = work.getWritePointer (ch);

            for (int i = 0; i < n; ++i)
            {
                float x = src[i];
                inP = juce::jmax (inP, std::abs (x));

                if (gateThresh > 0.0f)
                {
                    float a = std::abs (x);
                    c.gateEnv = a > c.gateEnv ? a : gateRel * c.gateEnv + (1.0f - gateRel) * a;
                    float target = c.gateEnv > gateThresh ? 1.0f : 0.0f;
                    c.gateGain = target > c.gateGain ? gateAtk * c.gateGain + (1.0f - gateAtk) * target
                                                     : gateRel * c.gateGain + (1.0f - gateRel) * target;
                    x *= c.gateGain;
                }

                dst[i] = x;
            }
        }

        // 2. Preamp, oversampled
        juce::dsp::AudioBlock<float> block (work.getArrayOfWritePointers(), 2, (size_t) n);
        auto os = oversampler->processSamplesUp (block);

        for (size_t ch = 0; ch < 2; ++ch)
        {
            auto& c = chans[ch];
            auto* d = os.getChannelPointer (ch);

            for (size_t i = 0; i < os.getNumSamples(); ++i)
            {
                float x = c.inHp.process (d[i], 0);
                x = c.tight.process (x, 0) * preGain;
                x = clipStage (x, s.bias);

                for (int st = 1; st < s.stages; ++st)
                {
                    x = c.interLp.process (c.interStage.process (x, st == 1 ? 0 : 1), st == 1 ? 0 : 1);
                    x = clipStage (x * (st == 1 ? 6.0f : 4.0f), s.bias * 0.5f);
                }

                d[i] = x;
            }
        }

        oversampler->processSamplesDown (block);

        // 3. Tone stack and power amp
        const float powerDrive = 0.5f + master.get() / 10.0f * s.powerDrive;
        const float sagCoeff = std::exp (-1.0f / (0.05f * (float) rate));
        const float dcR = 1.0f - 200.0f / (float) rate;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto& c = chans[(size_t) ch];
            auto* d = work.getWritePointer (ch);

            for (int i = 0; i < n; ++i)
            {
                float x = c.trebleF.process (c.midF.process (c.bassF.process (d[i], 0), 0), 0);
                x = c.presenceF.process (x, 0);

                // Power amp: soft clip whose headroom drops briefly under heavy load (sag)
                float drive = powerDrive * (1.0f - 0.3f * c.sag);
                float y = std::tanh (x * drive) * 0.5f;
                c.sag = sagCoeff * c.sag + (1.0f - sagCoeff) * juce::jmin (1.0f, std::abs (y));

                // DC blocker
                float dc = y - c.dcX + dcR * c.dcY;
                c.dcX = y; c.dcY = dc;
                d[i] = dc;
            }
        }

        // 4. Cabinet (impulse response)
        convolvers[0].process (work.getWritePointer (0), n);
        convolvers[1].process (work.getWritePointer (1), n);

        // 5. Room and output
        const float roomMix = room.get() / 100.0f * 0.35f;
        const float out = juce::Decibels::decibelsToGain (output.get()) * (builtKey.cab != cabOff ? cabLevel : 1.0f) * 0.9f;
        const float sr = (float) rate;
        float outP = 0.0f;
        auto* wl = work.getReadPointer (0);
        auto* wr = work.getReadPointer (1);
        auto* dl = buf.getWritePointer (0, start);
        auto* dr = numCh > 1 ? buf.getWritePointer (1, start) : nullptr;

        for (int i = 0; i < n; ++i)
        {
            float l = wl[i], r = wr[i];
            roomLine.push ((l + r) * 0.5f);

            if (roomMix > 0.001f)
            {
                // A few short reflections, different on each side
                float rl = roomLine.read (0.0071f * sr) * 0.5f + roomLine.read (0.0173f * sr) * 0.35f
                         + roomLine.read (0.0290f * sr) * 0.22f + roomLine.read (0.0470f * sr) * 0.12f;
                float rr = roomLine.read (0.0093f * sr) * 0.5f + roomLine.read (0.0151f * sr) * 0.35f
                         + roomLine.read (0.0335f * sr) * 0.22f + roomLine.read (0.0520f * sr) * 0.12f;
                l = l * (1.0f - roomMix * 0.5f) + rl * roomMix;
                r = r * (1.0f - roomMix * 0.5f) + rr * roomMix;
            }

            l *= out; r *= out;
            outP = juce::jmax (outP, std::abs (l), std::abs (r));
            dl[i] = dr != nullptr ? l : (l + r) * 0.5f;
            if (dr != nullptr) dr[i] = r;
        }

        inPeak.store (juce::jmax (inPeak.load(), inP));
        outPeak.store (juce::jmax (outPeak.load(), outP));
    }
};

//==============================================================================
/**
    Built-in cabinet impulse responses, generated rather than recorded.

    Each cabinet is described by the things that give real speaker cabinets their sound:
    the low-end roll-off and box/speaker resonance, the upper-mid presence peak, the
    steep top-end roll-off, and the cone "breakup" - a cluster of narrow peaks and dips
    between ~1.5 and 8 kHz that plain EQ curves don't have. Moving the mic from the
    centre of the cone to its edge darkens it and changes the breakup pattern. An
    open-back cab also gets the reflection of the sound from its open back.

    That magnitude response is turned into a minimum-phase impulse response, which is how a
    close-miked speaker behaves (energy arrives immediately, no pre-ringing).
*/
inline std::vector<float> AmpDevice::makeCabIr (Cab c, float micPos, double sr)
{
    using T = Biquad::Type;
    struct F { T type; float hz, q, db; };
    const float lpScale = 1.3f - 0.65f * micPos;       // centre is brighter
    const float presDb = 3.0f - 6.0f * micPos;
    std::vector<F> base;
    int seed = 0;
    float breakupDb = 5.0f;
    bool openBack = false;

    switch (c)
    {
        case cab1x12: seed = 11; openBack = true;
                      base = { { T::highPass, 85, 0.8f, 0 }, { T::bell, 120, 1.4f, 4 }, { T::bell, 2200, 1.3f, 3 + presDb },
                               { T::lowPass, 5600 * lpScale, 0.7f, 0 }, { T::lowPass, 6800 * lpScale, 0.6f, 0 } }; break;
        case cab2x12: seed = 23;
                      base = { { T::highPass, 75, 0.8f, 0 }, { T::bell, 110, 1.5f, 5 }, { T::bell, 1900, 1.2f, 2.5f + presDb },
                               { T::lowPass, 5200 * lpScale, 0.7f, 0 }, { T::lowPass, 5800 * lpScale, 0.6f, 0 } }; break;
        case cab4x12: seed = 37; breakupDb = 6.0f;
                      base = { { T::highPass, 72, 0.9f, 0 }, { T::bell, 100, 1.6f, 6 }, { T::bell, 400, 1.0f, -2.5f },
                               { T::bell, 2600, 1.6f, 4 + presDb }, { T::lowPass, 4800 * lpScale, 0.75f, 0 }, { T::lowPass, 5200 * lpScale, 0.6f, 0 } }; break;
        case cabBass: seed = 53; breakupDb = 4.0f;
                      base = { { T::highPass, 38, 0.8f, 0 }, { T::bell, 75, 1.2f, 4 }, { T::bell, 800, 0.9f, -3 },
                               { T::bell, 2000, 1.4f, presDb }, { T::lowPass, 3600 * lpScale, 0.7f, 0 }, { T::lowPass, 4300 * lpScale, 0.6f, 0 } }; break;
        case cabOff: case cabCustom: case numCabs:
            return { 1.0f };
    }

    // Cone breakup: narrow resonances whose pattern depends on the cab and (partly) the mic spot
    juce::Random rng (seed);
    juce::Random micRng (seed * 7 + juce::roundToInt (micPos * 4.0f));
    for (int i = 0; i < 9; ++i)
    {
        const float f = 1400.0f * std::pow (6.5f, rng.nextFloat());
        const float q = 3.0f + rng.nextFloat() * 6.0f;
        float g = (rng.nextFloat() * 2.0f - 1.0f) * breakupDb;
        g = g * (1.0f - micPos * 0.4f) + (micRng.nextFloat() * 2.0f - 1.0f) * breakupDb * 0.4f * micPos;
        base.push_back ({ T::bell, f, q, g });
    }

    std::vector<Biquad> filters (base.size());
    for (size_t i = 0; i < base.size(); ++i)
        filters[i].set (base[i].type, base[i].hz, base[i].q, base[i].db, sr);

    int order = 13;
    while ((1 << order) < (int) (sr * 0.17) && order < 16) ++order;     // ~8192 bins at 48 kHz
    const int n = 1 << order;
    std::vector<float> mag ((size_t) n / 2 + 1);

    for (int k = 0; k <= n / 2; ++k)
    {
        const double f = juce::jmax (1.0, k * sr / n);
        double db = 0.0;
        for (auto& flt : filters)
            db += flt.magnitudeDb (juce::jmin (f, sr * 0.499), sr);
        mag[(size_t) k] = (float) std::pow (10.0, juce::jlimit (-120.0, 40.0, db) / 20.0);
    }

    const int length = (int) std::lround (sr * 0.045);     // 45 ms
    auto ir = minimumPhaseIr (mag, order, length);

    // Open back: the sound from the back of the cone reflects off the wall behind the cab
    if (openBack)
    {
        const int d = (int) std::lround (sr * 0.0018);
        auto dry = ir;
        float lp = 0.0f;
        const float a = std::exp (-juce::MathConstants<float>::twoPi * 2500.0f / (float) sr);
        for (int i = d; i < length; ++i)
        {
            lp = a * lp + (1.0f - a) * dry[(size_t) (i - d)];
            ir[(size_t) i] -= 0.35f * lp;
        }
    }

    normaliseIr (ir, sr);
    return ir;
}

//==============================================================================
class AmpView : public juce::Component,
                public juce::FileDragAndDropTarget,
                public juce::DragAndDropTarget,
                private juce::Timer
{
public:
    AmpView (AmpDevice& d)
        : amp (d),
          modelChoice (d.model.param(), AmpDevice::modelNames(), Colours::deviceAmp),
          cabChoice (d.cab.param(), AmpDevice::cabShortNames(), Colours::deviceAmp)
    {
        auto gold = Colours::deviceAmp;
        for (auto* p : { &d.gain, &d.bassK, &d.mid, &d.treble, &d.presence, &d.master })
            addAndMakeVisible (ampKnobs.add (new DeviceUI::Knob (p->param(), {}, gold)));
        for (auto* p : { &d.mic, &d.room, &d.gate, &d.output })
            addAndMakeVisible (cabKnobs.add (new DeviceUI::Knob (p->param())));

        loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a cabinet impulse response", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc)
                                  {
                                      if (fc.getResult().existsAsFile())
                                          amp.loadCustomIr (fc.getResult());
                                  });
        };
        loadButton.setTooltip ("Load a cabinet impulse response (.wav). You can also drop one on the cabinet.");
        addAndMakeVisible (loadButton);

        addAndMakeVisible (modelChoice);
        addAndMakeVisible (cabChoice);
        addAndMakeVisible (meter);
        meter.getDb = [this] { return outDb; };
        startTimerHz (30);
    }

    /** Dark tolex on the dark theme, "blonde" tolex (like a vintage amp) on the light one. */
    struct Skin { juce::Colour tolex, trim, grille, grilleLine; };

    static Skin skin()
    {
        if (Theme::isLight())
            return { juce::Colour (0xffe6d4b0), juce::Colour (0xffc2a978), juce::Colour (0xffd9c59f), juce::Colour (0xffcdb78d) };

        return { juce::Colour (0xff1b1a19), juce::Colour (0xff3a3326), juce::Colour (0xff2b2823), juce::Colour (0xff34302a) };
    }

    void paint (juce::Graphics& g) override
    {
        const auto sk = skin();

        // Amp head: tolex with a gold-trimmed control panel
        auto head = ampArea.toFloat();
        g.setColour (sk.tolex);
        g.fillRoundedRectangle (head, 6.0f);
        g.setColour (sk.trim);
        g.drawRoundedRectangle (head.reduced (1.5f), 6.0f, 2.0f);

        const auto gold = Theme::isLight() ? Colours::deviceAmp.darker (0.8f) : Colours::deviceAmp;
        g.setColour (gold);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ("ELSEWHERE  " + AmpDevice::modelNames()[juce::roundToInt (amp.model.get())].toUpperCase(),
                    ampArea.withHeight (20).translated (10, 4), juce::Justification::centredLeft);

        // Cabinet: grille with the cabinet's frequency response drawn on it
        auto cab = cabArea.toFloat();
        g.setColour (sk.tolex);
        g.fillRoundedRectangle (cab, 6.0f);
        if (dragOver)
        {
            g.setColour (gold);
            g.drawRoundedRectangle (cab.reduced (1.0f), 6.0f, 2.0f);
        }

        auto grille = responseArea.toFloat();
        g.setColour (sk.grille);
        g.fillRoundedRectangle (grille, 3.0f);
        g.setColour (sk.grilleLine);
        for (float x = grille.getX() + 2; x < grille.getRight(); x += 4.0f)
            g.drawVerticalLine ((int) x, grille.getY(), grille.getBottom());

        const int cabType = juce::roundToInt (amp.cab.get());
        g.setFont (juce::FontOptions (10.0f));

        if (cabType == AmpDevice::cabOff)
        {
            g.setColour (Colours::textDim);
            g.drawText ("No cabinet (straight from the amp)", grille.toNearestInt(), juce::Justification::centred);
        }
        else if (cabType == AmpDevice::cabCustom && amp.getCustomIrName().isEmpty())
        {
            g.setColour (Colours::textDim);
            g.drawFittedText ("Load or drop a cabinet IR (.wav)", grille.toNearestInt().reduced (6), juce::Justification::centred, 2);
        }
        else if (! response.isEmpty())
        {
            g.setColour (gold.withAlpha (0.9f));
            g.strokePath (response, juce::PathStrokeType (1.5f));
            g.setColour (Colours::textDim);
            auto label = cabType == AmpDevice::cabCustom ? amp.getCustomIrName() + (amp.isCustomIrMissing() ? " (missing)" : "")
                                                         : AmpDevice::cabNames()[cabType] + juce::String::fromUTF8 (" \xc2\xb7 IR");
            g.drawText (label, grille.toNearestInt().reduced (5, 3), juce::Justification::topLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        cabArea = r.removeFromRight (260);
        r.removeFromRight (8);
        meter.setBounds (r.removeFromRight (10).withTrimmedTop (28).withTrimmedBottom (4));
        r.removeFromRight (6);
        ampArea = r;

        auto a = ampArea.reduced (8);
        a.removeFromTop (22);
        modelChoice.setBounds (a.removeFromTop (24));
        a.removeFromTop (8);
        auto kw = a.getWidth() / 6;
        auto kr = a.withHeight (juce::jmin (a.getHeight(), 90));
        for (auto* k : ampKnobs)
            k->setBounds (kr.removeFromLeft (kw));

        auto c = cabArea.reduced (8);
        cabChoice.setBounds (c.removeFromTop (22));
        c.removeFromTop (4);
        auto knobRow = c.removeFromBottom (66);
        auto w = knobRow.getWidth() / 4;
        for (auto* k : cabKnobs)
            k->setBounds (knobRow.removeFromLeft (w));
        c.removeFromBottom (4);

        const bool custom = juce::roundToInt (amp.cab.get()) == AmpDevice::cabCustom;
        loadButton.setVisible (custom);
        if (custom)
            loadButton.setBounds (c.removeFromBottom (20).removeFromRight (80));
        responseArea = c;
        updateResponse();
    }

    // Files dragged from Finder
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && juce::File (files[0]).hasFileExtension ("wav;aif;aiff;flac");
    }
    void fileDragEnter (const juce::StringArray&, int, int) override    { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override               { dragOver = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dragOver = false;
        amp.loadCustomIr (juce::File (files[0]));
    }

    // Files dragged from Elsewhere's browser
    bool isInterestedInDragSource (const SourceDetails& d) override     { return fileFrom (d).existsAsFile(); }
    void itemDragEnter (const SourceDetails&) override                  { dragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override                   { dragOver = false; repaint(); }
    void itemDropped (const SourceDetails& d) override
    {
        dragOver = false;
        amp.loadCustomIr (fileFrom (d));
    }

private:
    AmpDevice& amp;
    DeviceUI::Choice modelChoice, cabChoice;
    juce::OwnedArray<DeviceUI::Knob> ampKnobs, cabKnobs;
    DeviceUI::Meter meter;
    juce::TextButton loadButton { "Load IR..." };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> ampArea, cabArea, responseArea;
    juce::Path response;
    float outDb = -100.0f;
    int lastModel = -1, lastCab = -1;
    juce::String lastKey;
    bool dragOver = false;

    static juce::File fileFrom (const SourceDetails& d)
    {
        juce::File f;
        if (auto ft = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
            f = ft->getSelectedFile();
        return f.hasFileExtension ("wav;aif;aiff;flac") ? f : juce::File();
    }

    /** Frequency response of the current IR, 40 Hz - 16 kHz, drawn over the grille. */
    void updateResponse()
    {
        response.clear();
        auto ir = amp.getCurrentIr();
        if (ir.size() < 2 || responseArea.isEmpty())
            return;

        Fft fft (14);
        std::vector<std::complex<float>> x ((size_t) fft.size());
        for (size_t i = 0; i < ir.size() && i < x.size(); ++i) x[i] = ir[i];
        fft.forward (x.data());

        const double sr = 48000.0;     // display only: close enough whatever the device rate
        auto area = responseArea.toFloat().reduced (4, 14);
        for (int px = 0; px <= (int) area.getWidth(); px += 2)
        {
            const double f = 40.0 * std::pow (400.0, px / (double) area.getWidth());
            const auto bin = juce::jlimit (1, fft.size() / 2 - 1, (int) std::round (f / sr * fft.size()));
            // Light smoothing over neighbouring bins
            double p = 0.0;
            for (int k = -2; k <= 2; ++k) p += std::norm (x[(size_t) juce::jlimit (1, fft.size() / 2 - 1, bin + k)]);
            const float db = (float) (10.0 * std::log10 (p / 5.0 + 1.0e-12));
            const float y = juce::jmap (juce::jlimit (-36.0f, 12.0f, db), 12.0f, -36.0f, area.getY(), area.getBottom());
            if (px == 0) response.startNewSubPath (area.getX() + (float) px, y); else response.lineTo (area.getX() + (float) px, y);
        }
    }

    void timerCallback() override
    {
        outDb = juce::Decibels::gainToDecibels (amp.takeOutputPeak(), -100.0f);

        auto m = juce::roundToInt (amp.model.get()), c = juce::roundToInt (amp.cab.get());
        if (m != lastModel || c != lastCab)
        {
            const bool relayout = (c == AmpDevice::cabCustom) != (lastCab == AmpDevice::cabCustom);
            lastModel = m; lastCab = c;
            if (relayout) resized();
            repaint();
        }

        // Redraw the response when the IR changes (cab, mic, file)
        auto key = juce::String (c) + "/" + juce::String (juce::roundToInt (amp.mic.get())) + "/" + amp.getCustomIrName();
        if (key != lastKey)
        {
            lastKey = key;
            juce::Timer::callAfterDelay (150, [sp = SafePointer<AmpView> (this)] { if (sp != nullptr) { sp->updateResponse(); sp->repaint(); } });
        }
    }
};

inline std::unique_ptr<juce::Component> AmpDevice::createDeviceView()    { return std::make_unique<AmpView> (*this); }

} // namespace elsewhere
