// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "DrumKits.h"
#include "KitSamples.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Drums: a 16-pad drum machine.

    Pads play MIDI notes 36-51 (C1-D#2, the usual drum-rack layout). Every pad starts
    with a sound from the chosen kit, synthesised in code so no sample files are needed
    (Classic, Trap, Lo-Fi, Hyperpop, Amapiano, Acoustic Room, Techno; see DrumKits.h), and
    any pad can be replaced by dragging an audio file onto it. Dropped files are copied
    into the project so shared projects stay complete.

    Built-in sounds come in a few variations that take turns (round robin); softer hits
    are darker; each pad has tone and drive; the kit has a room (Space) and Crush.

    The closed hi-hat chokes the open hi-hat, like a real kit.
*/
class DrumsDevice : public ElsewhereDevice,
                    private juce::AsyncUpdater
{
public:
    static constexpr const char* xmlTypeName = "elsewhereDrums";
    static constexpr int numPads = 16, firstNote = 36;
    static constexpr double kitRate = 48000.0;

    static const char* padName (int i)
    {
        static const char* names[numPads] = { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom",
                                              "Rim", "Cowbell", "Crash", "Ride", "Shaker", "Clave", "808", "Zap" };
        return names[i];
    }

    struct Pad { Param level, pan, tune, decay, tone, drive; };

    /** A pad's sound: mono or stereo audio at its own sample rate. */
    struct DrumSound : public juce::ReferenceCountedObject
    {
        using Ptr = juce::ReferenceCountedObjectPtr<DrumSound>;
        juce::AudioBuffer<float> audio;
        std::vector<juce::AudioBuffer<float>> variations;   // more takes of the same sound, played in turn
        std::vector<std::vector<juce::AudioBuffer<float>>> layers;   // recorded kits: velocity layers (soft to hard), each with its takes
        double sampleRate = kitRate;

        int numTakes() const                                    { return 1 + (int) variations.size(); }
        const juce::AudioBuffer<float>& take (int i) const      { return i <= 0 || i > (int) variations.size() ? audio : variations[(size_t) i - 1]; }
    };

    DrumsDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        for (int i = 0; i < numPads; ++i)
        {
            auto id = "p" + juce::String (i + 1);
            auto nm = juce::String (padName (i)) + " ";
            auto& p = pads[(size_t) i];
            p.level = addParameter (id + "level", nm + "Level", 0.0f, { -60.0f, 6.0f }, dbText);
            p.pan   = addParameter (id + "pan", nm + "Pan", 0.0f, { -1.0f, 1.0f },
                                    [] (float v) { auto n = juce::roundToInt (std::abs (v) * 50); return n == 0 ? juce::String ("C") : juce::String (n) + (v < 0 ? "L" : "R"); });
            p.tune  = addParameter (id + "tune", nm + "Tune", 0.0f, { -24.0f, 24.0f },
                                    [] (float v) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " st"; });
            p.decay = addParameter (id + "decay", nm + "Decay", 100.0f, { 5.0f, 100.0f }, percentText);
        }

        volume = addParameter ("volume", "Volume", 0.0f, { -60.0f, 6.0f }, dbText);

        // Added later (so older projects keep their parameter order)
        for (int i = 0; i < numPads; ++i)
        {
            auto id = "p" + juce::String (i + 1);
            auto nm = juce::String (padName (i)) + " ";
            auto& p = pads[(size_t) i];
            p.tone  = addParameter (id + "tone", nm + "Tone", 0.0f, { -100.0f, 100.0f },
                                    [] (float v) { return std::abs (v) < 0.5f ? juce::String ("Open") : v < 0.0f ? "Dark " + juce::String (juce::roundToInt (-v)) : "Thin " + juce::String (juce::roundToInt (v)); });
            p.drive = addParameter (id + "drive", nm + "Drive", 0.0f, { 0.0f, 100.0f }, percentText);
        }
        space = addParameter ("space", "Space", 0.0f, { 0.0f, 100.0f }, percentText);
        crush = addParameter ("crush", "Crush", 0.0f, { 0.0f, 100.0f }, percentText);

        for (int i = 0; i < numPads; ++i)
            loadPadSound (i);

        // While a project loads, its file locations aren't known yet: load dropped-in samples
        // again once it has finished opening
        for (int i = 0; i < numPads; ++i)
            if (padHasUserSample (i))
                triggerAsyncUpdate();
    }

    ~DrumsDevice() override     { cancelPendingUpdate(); }

    juce::String getName() const override              { return "Elsewhere Drums"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Drums"; }
    int getDeviceViewWidth() const override            { return 1080; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 2.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }

    bool hasNameForMidiNoteNumber (int note, int, juce::String& name) override
    {
        if (note < firstNote || note >= firstNote + numPads)
            return false;

        name = getPadDisplayName (note - firstNote);
        return true;
    }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        for (auto& v : voices) v.active = false;
        roomFx.prepare (rate);
        crushHeldL = crushHeldR = 0.0f;
        crushCounter = 0;
    }

    //==============================================================================
    // Kits
    int getKitIndex() const                 { return DrumKits::indexOf (state.getProperty ("kit", "classic").toString()); }

    void setKit (int index)
    {
        index = juce::jlimit (0, (int) DrumKits::kits().size() - 1, index);
        state.setProperty ("kit", DrumKits::kits()[(size_t) index].id, getUndoManager());
    }

    void midiPanic() override
    {
        for (auto& v : voices) v.fadeOut = true;
    }

    /** Plays a pad from the UI (safe from any thread). */
    void audition (int pad, float velocity = 0.8f)
    {
        auditionQueue[(size_t) pad].store (velocity);
    }

    /** Incremented each time a pad plays (the UI uses it to flash the pad). */
    int getTriggerCount (int pad) const     { return triggerCounts[(size_t) pad].load(); }

    juce::String getPadDisplayName (int pad) const
    {
        auto file = state.getProperty (sampleProp (pad)).toString();
        return file.isEmpty() ? juce::String (DrumKits::kits()[(size_t) getKitIndex()].pads[(size_t) pad])
                              : juce::File::createFileWithoutCheckingPath (file).getFileNameWithoutExtension();
    }

    bool padHasUserSample (int pad) const   { return state.getProperty (sampleProp (pad)).toString().isNotEmpty(); }

    /** Replaces a pad's sound with an audio file (copied into the project), or restores the built-in one. */
    void setPadSample (int pad, const juce::File& file)
    {
        if (file == juce::File())
        {
            state.removeProperty (sampleProp (pad), getUndoManager());
            return;
        }

        auto f = importFileHook ? importFileHook (file) : file;
        juce::String path = f.getFullPathName();

        if (auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File(); editFile != juce::File())
            if (f.isAChildOf (editFile.getParentDirectory()))
                path = f.getRelativePathFrom (editFile.getParentDirectory());

        state.setProperty (sampleProp (pad), path, getUndoManager());
    }

    /**
        Which pad a sample belongs on, from its file name ("Kick 01.wav", "HH Open.aif",
        "Tom Floor.wav" ...). Returns -1 if the name doesn't say.
    */
    static int padForFileName (const juce::String& fileName)
    {
        auto n = " " + fileName.toLowerCase().replaceCharacters ("_-.()[]", "       ") + " ";
        auto has = [&] (std::initializer_list<const char*> words)
        {
            for (auto w : words) if (n.contains (w)) return true;
            return false;
        };

        if (has ({ " 808" }))                                            return 14;
        if (has ({ "kick", " bd ", "bassdrum", "bass drum", " kck" }))   return 0;
        if (has ({ "snare", " sd ", " snr" }))                           return 1;
        if (has ({ "clap", " cp ", " clp" }))                            return 2;
        if (has ({ "hat", " hh", " oh ", " ch " }))
            return has ({ "open", " oh " }) ? 4 : 3;
        if (has ({ "tom" }))
        {
            if (has ({ "floor", "low", " lo ", " lt " }))    return 5;
            if (has ({ "high", " hi ", " ht " }))            return 7;
            return 6;
        }
        if (has ({ "rim", "stick", " rs " }))                            return 8;
        if (has ({ "cowbell", " cb " }))                                 return 9;
        if (has ({ "crash", " cr ", "cymbal" }))                         return 10;
        if (has ({ "ride", " rd " }))                                    return 11;
        if (has ({ "shaker", "shk", "tamb", "maraca" }))                 return 12;
        if (has ({ "clave", " cl " }))                                   return 13;
        if (has ({ "perc", "zap", " fx " }))                             return 15;
        return -1;
    }

    /** Assigns a set of samples to pads by their names. Returns how many pads changed. */
    int assignByName (const juce::Array<juce::File>& files)
    {
        std::array<bool, numPads> taken {};
        int changed = 0;

        for (auto& f : files)
        {
            auto pad = padForFileName (f.getFileNameWithoutExtension());
            if (pad < 0 || taken[(size_t) pad])
                continue;

            taken[(size_t) pad] = true;
            setPadSample (pad, f);
            ++changed;
        }

        return changed;
    }

    /** Waveform of a pad's current sound, for display. */
    DrumSound::Ptr getSound (int pad) const
    {
        const juce::SpinLock::ScopedLockType sl (soundLock);
        return sounds[(size_t) pad];
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr)
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        buf.clear (fc.bufferStartSample, fc.bufferNumSamples);

        // Snapshot the sounds (the UI thread may swap them)
        std::array<DrumSound::Ptr, numPads> current;
        {
            const juce::SpinLock::ScopedTryLockType sl (soundLock);
            if (sl.isLocked())
                cachedSounds = sounds;
        }
        current = cachedSounds;

        // Collect note-ons (sample offset within this block)
        struct Hit { int offset, pad; float velocity; };
        std::array<Hit, 128> hits;
        int numHits = 0;

        if (fc.bufferForMidiMessages != nullptr)
        {
            if (fc.bufferForMidiMessages->isAllNotesOff)
                midiPanic();

            for (auto& m : *fc.bufferForMidiMessages)
            {
                if (! m.isNoteOn() || numHits >= (int) hits.size())
                    continue;

                auto pad = m.getNoteNumber() - firstNote;
                if (pad < 0 || pad >= numPads)
                    continue;

                auto offset = juce::jlimit (0, fc.bufferNumSamples - 1, juce::roundToInt (m.getTimeStamp() * rate));
                hits[(size_t) numHits++] = { offset, pad, m.getFloatVelocity() };
            }
        }

        for (int p = 0; p < numPads; ++p)
        {
            auto v = auditionQueue[(size_t) p].exchange (0.0f);
            if (v > 0.0f && numHits < (int) hits.size())
                hits[(size_t) numHits++] = { 0, p, v };
        }

        std::sort (hits.begin(), hits.begin() + numHits, [] (auto& a, auto& b) { return a.offset < b.offset; });

        // Render in segments between hits
        int done = 0, h = 0;
        while (done < fc.bufferNumSamples)
        {
            while (h < numHits && hits[(size_t) h].offset <= done)
            {
                startVoice (hits[(size_t) h].pad, hits[(size_t) h].velocity, current);
                ++h;
            }

            int end = h < numHits ? hits[(size_t) h].offset : fc.bufferNumSamples;
            renderVoices (buf, fc.bufferStartSample + done, end - done, current);
            done = end;
        }

        applyKitEffects (buf, fc.bufferStartSample, fc.bufferNumSamples);

        auto vol = juce::Decibels::decibelsToGain (volume.get(), -60.0f);
        for (int ch = 0; ch < juce::jmin (2, buf.getNumChannels()); ++ch)
            buf.applyGain (ch, fc.bufferStartSample, fc.bufferNumSamples, vol);
    }

    std::array<Pad, numPads> pads;
    Param volume, space, crush;

    /** The sound a built-in kit has on a pad (cached, shared by every Drums). */
    static DrumSound::Ptr kitSound (int kit, int pad);

    /** Synthesises one of the built-in kit sounds. */
    static DrumSound::Ptr synthesise (int pad);

private:
    struct Voice
    {
        bool active = false, fadeOut = false, filtered = false, highPassed = false;
        int pad = 0, take = 0, layer = -1;
        double pos = 0.0, step = 1.0;
        float gainL = 1.0f, gainR = 1.0f, env = 1.0f, envMul = 1.0f, fade = 1.0f, drive = 1.0f, driveNorm = 1.0f;
        std::array<SynthCore::Svf, 2> lp, hp;
    };

    /** A small stereo room for the whole kit (parallel combs into allpasses). */
    struct Room
    {
        struct Line { std::vector<float> d; int p = 0; float lp = 0.0f; };
        std::array<std::array<Line, 4>, 2> combs;
        std::array<std::array<Line, 2>, 2> allpasses;

        void prepare (double sr)
        {
            static constexpr int c[] = { 1116, 1188, 1277, 1356 }, a[] = { 556, 441 };
            for (int ch = 0; ch < 2; ++ch)
            {
                for (int i = 0; i < 4; ++i) { combs[(size_t) ch][(size_t) i].d.assign ((size_t) ((c[i] + ch * 23) * sr / 44100.0 * 0.8), 0.0f); combs[(size_t) ch][(size_t) i].p = 0; }
                for (int i = 0; i < 2; ++i) { allpasses[(size_t) ch][(size_t) i].d.assign ((size_t) ((a[i] + ch * 23) * sr / 44100.0), 0.0f); allpasses[(size_t) ch][(size_t) i].p = 0; }
            }
        }

        float process (int ch, float in) noexcept
        {
            float out = 0.0f;
            for (auto& l : combs[(size_t) ch])
            {
                if (l.d.empty()) return 0.0f;
                const float y = l.d[(size_t) l.p];
                l.lp = y * 0.55f + l.lp * 0.45f;
                l.d[(size_t) l.p] = in + l.lp * 0.8f;
                if (++l.p >= (int) l.d.size()) l.p = 0;
                out += y;
            }
            for (auto& l : allpasses[(size_t) ch])
            {
                const float b = l.d[(size_t) l.p];
                const float y = -out + b;
                l.d[(size_t) l.p] = out + b * 0.5f;
                if (++l.p >= (int) l.d.size()) l.p = 0;
                out = y;
            }
            return out * 0.25f;
        }
    };

    Room roomFx;
    float crushHeldL = 0.0f, crushHeldR = 0.0f;
    int crushCounter = 0;
    std::array<int, numPads> nextTake {};

    void applyKitEffects (juce::AudioBuffer<float>& buf, int start, int num)
    {
        const float spaceAmt = space.get() / 100.0f, crushAmt = crush.get() / 100.0f;
        if (spaceAmt <= 0.0f && crushAmt <= 0.0f)
            return;

        float* L = buf.getWritePointer (0, start);
        float* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1, start) : nullptr;
        const int hold = 1 + juce::roundToInt (crushAmt * crushAmt * 15.0f);
        const float steps = std::pow (2.0f, 15.0f - crushAmt * 10.0f);

        for (int i = 0; i < num; ++i)
        {
            float l = L[i], r = R != nullptr ? R[i] : L[i];

            if (crushAmt > 0.0f)
            {
                if (crushCounter++ % hold == 0)
                {
                    crushHeldL = std::round (l * steps) / steps;
                    crushHeldR = std::round (r * steps) / steps;
                }
                l = crushHeldL; r = crushHeldR;
            }

            if (spaceAmt > 0.0f)
            {
                const float mono = (l + r) * 0.5f * spaceAmt;
                l += roomFx.process (0, mono) * 1.6f;
                r += roomFx.process (1, mono) * 1.6f;
            }

            L[i] = l;
            if (R != nullptr) R[i] = r;
        }
    }

    double rate = 44100.0;
    std::array<Voice, 32> voices;
    std::array<std::atomic<float>, numPads> auditionQueue {};
    std::array<std::atomic<int>, numPads> triggerCounts {};

    juce::SpinLock soundLock;
    std::array<DrumSound::Ptr, numPads> sounds, cachedSounds;

    static juce::Identifier sampleProp (int pad)   { return juce::Identifier ("sample" + juce::String (pad + 1)); }

    void startVoice (int pad, float velocity, const std::array<DrumSound::Ptr, numPads>& current)
    {
        if (current[(size_t) pad] == nullptr)
            return;

        triggerCounts[(size_t) pad].fetch_add (1);

        // Retriggering a pad (or the closed hat choking the open hat) fades the old voice out quickly
        for (auto& v : voices)
            if (v.active && (v.pad == pad || (pad == 3 && v.pad == 4)))
                v.fadeOut = true;

        Voice* voice = nullptr;
        for (auto& v : voices)
            if (! v.active) { voice = &v; break; }

        if (voice == nullptr)
        {
            // Steal the quietest voice
            voice = &voices[0];
            for (auto& v : voices)
                if (v.env * v.fade < voice->env * voice->fade)
                    voice = &v;
        }

        auto& p = pads[(size_t) pad];
        auto& snd = *current[(size_t) pad];
        const float gain = juce::Decibels::decibelsToGain (p.level.get(), -60.0f) * (0.2f + 0.8f * velocity);
        const float pan = p.pan.get();

        voice->active = true;
        voice->fadeOut = false;
        voice->pad = pad;
        voice->pos = 0.0;

        // Velocity layer (recorded kits), then round robin through that layer's takes
        voice->layer = -1;
        if (! snd.layers.empty())
        {
            const int n = (int) snd.layers.size();
            voice->layer = juce::jlimit (0, n - 1, n == 3 ? (velocity < 0.45f ? 0 : velocity < 0.8f ? 1 : 2) : (int) (velocity * (float) n * 0.999f));
            const int takes = (int) snd.layers[(size_t) voice->layer].size();
            voice->take = nextTake[(size_t) pad] % takes;
            nextTake[(size_t) pad] = (voice->take + 1) % juce::jmax (1, takes);
        }
        else
        {
            voice->take = nextTake[(size_t) pad] % snd.numTakes();
            nextTake[(size_t) pad] = (voice->take + 1) % snd.numTakes();
        }

        // Tone: dark (low-pass) one way, thin (high-pass) the other; softer hits are darker too
        const float tone = p.tone.get() / 100.0f;
        // (recorded kits are darker at low velocity already: they were played softer)
        const float velDark = snd.layers.empty() ? juce::square (0.35f + 0.65f * velocity) : 1.0f;
        const float lpHz = 20000.0f * (tone < 0.0f ? std::pow (2.0f, tone * 7.0f) : 1.0f) * (0.08f + 0.92f * velDark);
        voice->filtered = lpHz < 18000.0f;
        voice->highPassed = tone > 0.01f;
        for (int ch = 0; ch < 2; ++ch)
        {
            voice->lp[(size_t) ch].reset();
            voice->hp[(size_t) ch].reset();
            voice->lp[(size_t) ch].set (lpHz, 0.1f, (float) rate);
            voice->hp[(size_t) ch].set (20.0f * std::pow (2.0f, tone * 8.5f), 0.15f, (float) rate);
        }
        voice->drive = 1.0f + p.drive.get() / 100.0f * 7.0f;
        voice->driveNorm = 1.0f / std::tanh (voice->drive);
        voice->step = snd.sampleRate / rate * std::pow (2.0, p.tune.get() / 12.0);
        voice->gainL = gain * std::cos ((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f) * 1.414f;
        voice->gainR = gain * std::sin ((pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f) * 1.414f;
        voice->env = 1.0f;
        voice->fade = 1.0f;

        // Decay < 100%: an extra exponential fade that reaches -40 dB at that fraction of the sound's length
        auto decayFrac = p.decay.get() / 100.0f;
        auto lengthSamples = (double) snd.audio.getNumSamples() / voice->step;
        voice->envMul = decayFrac >= 0.999f ? 1.0f
                                            : (float) std::pow (0.01, 1.0 / juce::jmax (1.0, lengthSamples * decayFrac));
    }

    void renderVoices (juce::AudioBuffer<float>& buf, int start, int num, const std::array<DrumSound::Ptr, numPads>& current)
    {
        if (num <= 0)
            return;

        const bool stereoOut = buf.getNumChannels() > 1;
        auto* outL = buf.getWritePointer (0, start);
        auto* outR = stereoOut ? buf.getWritePointer (1, start) : nullptr;
        const float fadeStep = 1.0f / (float) juce::jmax (1.0, rate * 0.003);   // 3 ms

        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            auto snd = current[(size_t) v.pad];
            if (snd == nullptr) { v.active = false; continue; }

            const auto& audio = v.layer >= 0 && v.layer < (int) snd->layers.size() && ! snd->layers[(size_t) v.layer].empty()
                                    ? snd->layers[(size_t) v.layer][(size_t) juce::jmin (v.take, (int) snd->layers[(size_t) v.layer].size() - 1)]
                                    : snd->take (v.take);
            const int len = audio.getNumSamples();
            const float* srcL = audio.getReadPointer (0);
            const float* srcR = audio.getNumChannels() > 1 ? audio.getReadPointer (1) : srcL;
            const bool shaped = v.filtered || v.highPassed || v.drive > 1.001f;

            for (int i = 0; i < num; ++i)
            {
                const int i0 = (int) v.pos;
                if (i0 >= len - 1 || v.env < 0.0001f || v.fade <= 0.0f)
                {
                    v.active = false;
                    break;
                }

                const float frac = (float) (v.pos - i0);
                float l = srcL[i0] + frac * (srcL[i0 + 1] - srcL[i0]);
                float r = srcR[i0] + frac * (srcR[i0 + 1] - srcR[i0]);

                if (shaped)
                {
                    if (v.drive > 1.001f) { l = std::tanh (l * v.drive) * v.driveNorm; r = std::tanh (r * v.drive) * v.driveNorm; }
                    if (v.filtered)       { l = v.lp[0].lowpass (l); r = v.lp[1].lowpass (r); }
                    if (v.highPassed)     { l = v.hp[0].highpass (l); r = v.hp[1].highpass (r); }
                }
                const float g = v.env * v.fade;

                if (outR != nullptr)
                {
                    outL[i] += l * g * v.gainL;
                    outR[i] += r * g * v.gainR;
                }
                else
                {
                    outL[i] += (l + r) * 0.5f * g * (v.gainL + v.gainR) * 0.5f;
                }

                v.pos += v.step;
                v.env *= v.envMul;
                if (v.fadeOut) v.fade -= fadeStep;
            }
        }
    }

    //==============================================================================
    void loadPadSound (int pad)
    {
        DrumSound::Ptr snd;
        auto path = state.getProperty (sampleProp (pad)).toString();

        if (path.isNotEmpty())
            snd = loadFile (path);

        if (snd == nullptr)
            snd = getKitIndex() == 0 ? synthesise (pad) : kitSound (getKitIndex(), pad);

        const juce::SpinLock::ScopedLockType sl (soundLock);
        sounds[(size_t) pad] = snd;
    }

    DrumSound::Ptr loadFile (const juce::String& path)
    {
        auto file = edit.filePathResolver ? edit.filePathResolver (path) : juce::File (path);

        if (! file.existsAsFile())
            return {};

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));

        if (reader == nullptr)
            return {};

        // Pads are one-shots: cap at 20 seconds
        auto len = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 20.0));
        DrumSound::Ptr snd = new DrumSound();
        snd->sampleRate = reader->sampleRate;
        snd->audio.setSize (juce::jmin (2, (int) reader->numChannels), len + 1);
        snd->audio.clear();
        reader->read (&snd->audio, 0, len, 0, true, snd->audio.getNumChannels() > 1);
        return snd;
    }

    void handleAsyncUpdate() override
    {
        for (int i = 0; i < numPads; ++i)
            if (padHasUserSample (i))
                loadPadSound (i);
    }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& prop) override
    {
        ElsewhereDevice::valueTreePropertyChanged (v, prop);

        if (v != state)
            return;

        for (int i = 0; i < numPads; ++i)
            if (prop == sampleProp (i))
                loadPadSound (i);

        if (prop.toString() == "kit")
            for (int i = 0; i < numPads; ++i)
                if (! padHasUserSample (i))
                    loadPadSound (i);
    }
};

//==============================================================================
inline DrumsDevice::DrumSound::Ptr DrumsDevice::kitSound (int kit, int pad)
{
    static std::map<int, DrumSound::Ptr> cache;
    static std::mutex cacheLock;
    std::lock_guard<std::mutex> lock (cacheLock);

    const int key = kit * 100 + pad;
    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    const auto& info = DrumKits::kits()[(size_t) juce::jlimit (0, (int) DrumKits::kits().size() - 1, kit)];
    if (info.recorded)
    {
        // Recorded kits: FLAC files built into the app, <kit>_<pad>_v<layer>_r<take>
        DrumSound::Ptr s = new DrumSound();
        juce::FlacAudioFormat flac;
        for (int layer = 1; layer <= 3; ++layer)
        {
            std::vector<juce::AudioBuffer<float>> takes;
            for (int take = 1; take <= 2; ++take)
            {
                const auto name = juce::String (info.id) + "_" + juce::String (pad).paddedLeft ('0', 2) + "_v" + juce::String (layer) + "_r" + juce::String (take) + "_flac";
                int size = 0;
                const char* data = KitSamples::getNamedResource (name.toRawUTF8(), size);
                if (data == nullptr) continue;
                std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (new juce::MemoryInputStream (data, (size_t) size, false), true));
                if (reader == nullptr) continue;
                juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples + 1);
                b.clear();
                reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, true);
                s->sampleRate = reader->sampleRate;
                takes.push_back (std::move (b));
            }
            if (! takes.empty())
                s->layers.push_back (std::move (takes));
        }
        if (! s->layers.empty())
            s->audio = s->layers.back().front();
        cache[key] = s;
        return s;
    }

    DrumSound::Ptr s = new DrumSound();
    s->sampleRate = DrumKits::rate;
    s->audio = DrumKits::render (kit, pad, 0);
    for (int v = 1; v < DrumKits::numVariations; ++v)
        s->variations.push_back (DrumKits::render (kit, pad, v));
    cache[key] = s;
    return s;
}

inline DrumsDevice::DrumSound::Ptr DrumsDevice::synthesise (int pad)
{
    // Cached: every Drums instance shares the built-in kit
    static std::array<DrumSound::Ptr, numPads> cache;
    static std::mutex cacheLock;
    std::lock_guard<std::mutex> lock (cacheLock);

    if (cache[(size_t) pad] != nullptr)
        return cache[(size_t) pad];

    const double sr = kitRate;
    const double twoPi = juce::MathConstants<double>::twoPi;
    juce::Random rng (1234 + pad);   // deterministic: everyone gets the same kit
    auto noise = [&rng] { return rng.nextFloat() * 2.0f - 1.0f; };

    auto make = [&] (double seconds, auto&& gen)
    {
        DrumSound::Ptr s = new DrumSound();
        s->sampleRate = sr;
        auto n = (int) (seconds * sr);
        s->audio.setSize (1, n + 1);
        s->audio.clear();
        auto* d = s->audio.getWritePointer (0);
        for (int i = 0; i < n; ++i)
            d[i] = gen (i / sr);

        // Tiny fade-out so nothing clicks at the end, then normalise to -1 dBFS
        auto fade = juce::jmin (n, (int) (0.005 * sr));
        for (int i = 0; i < fade; ++i)
            d[n - fade + i] *= 1.0f - (float) i / (float) fade;

        auto peak = s->audio.getMagnitude (0, n);
        if (peak > 0.0f)
            s->audio.applyGain (0.89f / peak);

        return s;
    };

    // Metallic tone from six detuned square waves (the classic analog cymbal recipe)
    auto metallic = [&] (double t, double scale)
    {
        static const double f[] = { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        float sum = 0.0f;
        for (auto freq : f)
            sum += std::fmod (t * freq * scale, 1.0) < 0.5 ? 1.0f : -1.0f;
        return sum / 6.0f;
    };

    auto hp = [] (double cutoff, double sampleRate)
    {
        juce::IIRFilter filt;
        filt.setCoefficients (juce::IIRCoefficients::makeHighPass (sampleRate, cutoff));
        return filt;
    };

    auto bp = [] (double centre, double q, double sampleRate)
    {
        juce::IIRFilter filt;
        filt.setCoefficients (juce::IIRCoefficients::makeBandPass (sampleRate, centre, q));
        return filt;
    };

    auto tom = [&] (double freq, double length)
    {
        double phase = 0.0;
        return make (length, [&, phase] (double t) mutable
        {
            auto f = freq * (1.0 + 0.6 * std::exp (-t * 30.0));
            phase += twoPi * f / sr;
            return (float) (std::sin (phase) * std::exp (-t * 7.0 / length)) + noise() * 0.05f * (float) std::exp (-t * 80.0);
        });
    };

    DrumSound::Ptr s;

    switch (pad)
    {
        case 0:     // Kick: pitch-swept sine with a click
        {
            double phase = 0.0;
            s = make (0.6, [&] (double t)
            {
                auto f = 48.0 + 180.0 * std::exp (-t * 38.0);
                phase += twoPi * f / sr;
                auto body = std::sin (phase) * std::exp (-t * 6.5);
                auto click = std::exp (-t * 900.0) * 0.6;
                return (float) std::tanh ((body + click) * 1.6);
            });
            break;
        }
        case 1:     // Snare: two tuned modes + filtered noise
        {
            auto filt = hp (1800.0, sr);
            s = make (0.35, [&] (double t)
            {
                auto tone = (std::sin (twoPi * 185.0 * t) + 0.6 * std::sin (twoPi * 330.0 * t)) * std::exp (-t * 22.0);
                auto n = filt.processSingleSampleRaw (noise()) * std::exp (-t * 13.0);
                return (float) (tone * 0.6 + n * 1.1);
            });
            break;
        }
        case 2:     // Clap: three quick bursts then a tail
        {
            auto filt = bp (1150.0, 1.4, sr);
            s = make (0.4, [&] (double t)
            {
                double env = 0.0;
                for (double b : { 0.0, 0.011, 0.022 })
                    if (t >= b) env = juce::jmax (env, std::exp (-(t - b) * 180.0));
                if (t >= 0.031) env = juce::jmax (env, 0.8 * std::exp (-(t - 0.031) * 14.0));
                return (float) (filt.processSingleSampleRaw (noise()) * env);
            });
            break;
        }
        case 3:     // Closed hat
        case 4:     // Open hat
        {
            auto filt = hp (7000.0, sr);
            auto decay = pad == 3 ? 55.0 : 6.0;
            s = make (pad == 3 ? 0.15 : 0.8, [&] (double t)
            {
                auto x = metallic (t, 1.7) * 0.8f + noise() * 0.3f;
                return (float) (filt.processSingleSampleRaw (x) * std::exp (-t * decay));
            });
            break;
        }
        case 5:     s = tom (95.0, 0.6);  break;
        case 6:     s = tom (135.0, 0.5); break;
        case 7:     s = tom (185.0, 0.45); break;
        case 8:     // Rim shot
        {
            auto filt = hp (500.0, sr);
            s = make (0.08, [&] (double t)
            {
                auto tone = (std::sin (twoPi * 1720.0 * t) * 0.6 + std::sin (twoPi * 490.0 * t)) * std::exp (-t * 90.0);
                return (float) (tone + filt.processSingleSampleRaw (noise()) * std::exp (-t * 300.0) * 0.8);
            });
            break;
        }
        case 9:     // Cowbell
        {
            auto filt = bp (800.0, 1.2, sr);
            s = make (0.45, [&] (double t)
            {
                auto sq = (std::fmod (t * 540.0, 1.0) < 0.5 ? 1.0 : -1.0) + (std::fmod (t * 800.0, 1.0) < 0.5 ? 1.0 : -1.0);
                auto env = 0.6 * std::exp (-t * 60.0) + 0.4 * std::exp (-t * 7.0);
                return (float) (filt.processSingleSampleRaw ((float) sq * 0.5f) * env);
            });
            break;
        }
        case 10:    // Crash
        {
            auto filt = hp (4000.0, sr);
            s = make (2.2, [&] (double t)
            {
                auto x = metallic (t, 2.3) * 0.5f + noise() * 0.7f;
                return (float) (filt.processSingleSampleRaw (x) * std::exp (-t * 2.2));
            });
            break;
        }
        case 11:    // Ride
        {
            auto filt = hp (5000.0, sr);
            s = make (1.8, [&] (double t)
            {
                auto x = metallic (t, 3.1) * 0.8f + noise() * 0.15f;
                auto ping = std::sin (twoPi * 3300.0 * t) * 0.25 * std::exp (-t * 12.0);
                return (float) ((filt.processSingleSampleRaw (x) + ping) * std::exp (-t * 2.6));
            });
            break;
        }
        case 12:    // Shaker
        {
            auto filt = hp (6000.0, sr);
            s = make (0.16, [&] (double t)
            {
                auto env = (1.0 - std::exp (-t * 120.0)) * std::exp (-t * 28.0);
                return (float) (filt.processSingleSampleRaw (noise()) * env);
            });
            break;
        }
        case 13:    // Clave
            s = make (0.08, [&] (double t) { return (float) (std::sin (twoPi * 2500.0 * t) * std::exp (-t * 60.0)); });
            break;
        case 14:    // 808: long, deep and slightly driven
        {
            double phase = 0.0;
            s = make (1.6, [&] (double t)
            {
                auto f = 46.0 + 60.0 * std::exp (-t * 25.0);
                phase += twoPi * f / sr;
                return (float) std::tanh (std::sin (phase) * std::exp (-t * 2.2) * 2.2);
            });
            break;
        }
        default:    // Zap
        {
            double phase = 0.0;
            s = make (0.15, [&] (double t)
            {
                auto f = 90.0 + 2400.0 * std::exp (-t * 45.0);
                phase += twoPi * f / sr;
                return (float) (std::sin (phase) * std::exp (-t * 20.0));
            });
            break;
        }
    }

    cache[(size_t) pad] = s;
    return s;
}

//==============================================================================
/**
    The Drums panel, flat:
      - an LED display: the kit, the selected sound's waveform, and the value of whatever
        you're turning; kit buttons and a kit menu underneath
      - the pads (a 4x4 grid, C1 bottom-left) or the kit (drums seen from above), colour-coded
        by kind: kicks orange, snares and claps magenta, toms violet, hats and cymbals cyan,
        percussion green. A dot marks a pad playing your own sample.
      - the selected pad's controls, then the kit's (Space, Crush, Volume)
*/
class DrumsView : public SynthPanel::PanelBase,
                  public juce::FileDragAndDropTarget,
                  public juce::DragAndDropTarget,
                  private juce::Timer
{
public:
    DrumsView (DrumsDevice& d) : drums (d)
    {
        // Kit view by default on drum tracks, pads elsewhere; remembered per instance
        auto view = drums.state.getProperty ("view").toString();
        if (view.isEmpty())
            if (auto t = drums.getOwnerTrack())
                view = (bool) t->state.getProperty ("elsewhereDrumTrack", false) ? "kit" : "pads";
        kitMode = view == "kit";

        oled.drawBody = [this] (juce::Graphics& g, juce::Rectangle<float> r) { drawWaveform (g, r); };
        addAndMakeVisible (oled);
        onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };

        for (auto* b : { &prevKit, &nextKit, &kitMenu })
            addAndMakeVisible (b);
        prevKit.onClick = [this] { stepKit (-1); };
        prevKit.setTitle ("Previous Kit");
        nextKit.onClick = [this] { stepKit (1); };
        nextKit.setTitle ("Next Kit");
        kitMenu.onClick = [this] { showKitMenu(); };

        for (auto* b : { &kitButton, &padsButton })
        {
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonOnColourId, Colours::accent);
            addAndMakeVisible (b);
        }
        kitButton.onClick = [this] { setKitMode (true); };
        padsButton.onClick = [this] { setKitMode (false); };

        kitKnobs = { knob (d.space, "Space", Colours::clips[5]), knob (d.crush, "Crush", Colours::clips[6]), knob (d.volume, "Volume", Colours::accent) };

        selectPad (0);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        // The pads or the kit
        g.setColour (Colours::panelDark.withAlpha (0.55f));
        g.fillRoundedRectangle (gridArea.toFloat().expanded (3.0f), 5.0f);

        if (kitMode)
            paintKit (g);

        for (int i = 0; i < DrumsDevice::numPads; ++i)
            if (! kitMode || isExtra (i))
                paintPad (g, i);

        SynthPanel::paintSection (g, padArea, drums.getPadDisplayName (selected) + "  " + juce::MidiMessage::getMidiNoteName (DrumsDevice::firstNote + selected, true, true, 3),
                                  colourFor (selected));
        SynthPanel::paintSection (g, kitArea, "Kit", Colours::clips[5]);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = 6;

        auto display = r.removeFromLeft (210);
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (4);
        oled.setBounds (display);
        prevKit.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextKit.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        kitMenu.setBounds (nav);
        r.removeFromLeft (gap + 3);

        auto padsColumn = r.removeFromLeft (kitMode ? juce::jmin (r.getHeight() * 3, 470) : juce::jmin (r.getHeight() * 2, 330));
        auto switchRow = padsColumn.removeFromTop (20);
        kitButton.setBounds (switchRow.removeFromLeft (48));
        switchRow.removeFromLeft (2);
        padsButton.setBounds (switchRow.removeFromLeft (48));
        padsColumn.removeFromTop (6);
        gridArea = padsColumn.withTrimmedBottom (3);
        drumArea = gridArea.withTrimmedBottom (kitMode ? 28 : 0);
        kitButton.setToggleState (kitMode, juce::dontSendNotification);
        padsButton.setToggleState (! kitMode, juce::dontSendNotification);
        r.removeFromLeft (gap + 3);

        kitArea = r.removeFromRight (96);
        r.removeFromRight (gap);
        padArea = r;

        {
            auto c = content (padArea);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { padKnobs[0], padKnobs[1], padKnobs[2] }, 3);
            row (c, { padKnobs[3], padKnobs[4], padKnobs[5] }, 3);
        }
        {
            auto c = content (kitArea);
            const int h = c.getHeight() / 3;
            for (auto* k : kitKnobs) k->setBounds (c.removeFromTop (h));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto pad = padAt (e.getPosition());
        if (pad < 0) return;

        selectPad (pad);

        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem ("Replace with Audio File...", [this, pad]
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose a sample", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [this, pad] (const juce::FileChooser& fc)
                                      {
                                          if (fc.getResult().existsAsFile())
                                              drums.setPadSample (pad, fc.getResult());
                                          repaint();
                                      });
            });
            m.addItem ("Use the Kit's Sound", drums.padHasUserSample (pad), false, [this, pad] { drums.setPadSample (pad, {}); repaint(); });
            m.showMenuAsync ({});
            return;
        }

        // Velocity from where you hit it: the middle is loudest on a drum, the top of a pad
        const auto b = padBounds (pad).toFloat();
        const float vel = kitMode && ! isExtra (pad)
                            ? 1.0f - 0.6f * juce::jlimit (0.0f, 1.0f, e.position.getDistanceFrom (b.getCentre()) / (b.getWidth() * 0.5f))
                            : 1.0f - (e.position.y - b.getY()) / b.getHeight() * 0.6f;
        drums.audition (pad, juce::jlimit (0.2f, 1.0f, vel));
    }

    // Files dragged from Finder / Explorer
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (juce::File (f).isDirectory() || juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
                return true;
        return false;
    }

    void fileDragMove (const juce::StringArray&, int x, int y) override    { setDragOver (padAt ({ x, y })); }
    void fileDragExit (const juce::StringArray&) override                  { setDragOver (-1); }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        setDragOver (-1);

        // One sample on a drum: that drum. A folder or several samples: sorted out by name.
        juce::Array<juce::File> audio;
        for (auto& path : files)
        {
            juce::File f (path);
            if (f.isDirectory())
                audio.addArray (f.findChildFiles (juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"));
            else if (f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg"))
                audio.add (f);
        }

        if (audio.size() == 1 && ! juce::File (files[0]).isDirectory())
        {
            if (auto pad = padAt ({ x, y }); pad >= 0)
            {
                drums.setPadSample (pad, audio[0]);
                selectPad (pad);
            }
            return;
        }

        audio.sort();
        auto n = drums.assignByName (audio);
        oled.showValue ("Samples", n > 0 ? juce::String (n) + " drums replaced" : "Names not recognised", n > 0 ? 1.0f : 0.0f);
        repaint();
    }

    // Files dragged from Elsewhere's own browser
    bool isInterestedInDragSource (const SourceDetails& d) override     { return fileFrom (d).existsAsFile(); }
    void itemDragMove (const SourceDetails& d) override                 { setDragOver (padAt (d.localPosition)); }
    void itemDragExit (const SourceDetails&) override                   { setDragOver (-1); }

    void itemDropped (const SourceDetails& d) override
    {
        setDragOver (-1);
        if (auto pad = padAt (d.localPosition); pad >= 0)
        {
            drums.setPadSample (pad, fileFrom (d));
            selectPad (pad);
        }
    }

private:
    DrumsDevice& drums;
    SynthPanel::Oled oled;
    juce::TextButton prevKit { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextKit { juce::String::fromUTF8 ("\xe2\x96\xb6") }, kitMenu { "Kits" };
    juce::TextButton kitButton { "Kit" }, padsButton { "Pads" };
    std::vector<SynthPanel::Knob*> padKnobs, kitKnobs;
    std::vector<std::unique_ptr<SynthPanel::Knob>> padKnobOwners;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> gridArea, drumArea, padArea, kitArea;
    int selected = 0, dragOverPad = -1, shownKit = -1;
    bool kitMode = false;

    // Pad numbers: 0 kick, 1 snare, 2 clap, 3 closed hat, 4 open hat, 5 low tom, 6 mid tom,
    // 7 high tom, 8 rim, 9 cowbell, 10 crash, 11 ride, 12 shaker, 13 clave, 14 808, 15 zap
    static bool isCymbal (int pad)      { return pad == 3 || pad == 4 || pad == 10 || pad == 11; }
    static bool isExtra (int pad)       { return pad == 2 || pad == 8 || pad == 9 || pad == 12 || pad == 13 || pad == 14 || pad == 15; }

    static juce::Colour colourFor (int pad)
    {
        switch (pad)
        {
            case 0: case 14:                return Colours::accent;
            case 1: case 2: case 8: case 13: return Colours::clips[6];
            case 5: case 6: case 7:         return Colours::clips[5];
            case 3: case 4: case 10: case 11: return Colours::solo;
            default:                        return Colours::clips[2];
        }
    }

    static const std::vector<int>& drawOrder()
    {
        static const std::vector<int> order { 0, 5, 7, 6, 1, 3, 4, 10, 11 };   // back to front
        return order;
    }

    std::array<float, DrumsDevice::numPads> flashes {};
    std::array<int, DrumsDevice::numPads> lastCounts {};

    void paintPad (juce::Graphics& g, int i)
    {
        auto r = padBounds (i).toFloat().reduced (2.0f);
        const auto c = colourFor (i);
        const float flash = flashes[(size_t) i];
        const bool sel = i == selected;

        g.setColour (Colours::panel.interpolatedWith (c, 0.16f + 0.84f * flash));
        g.fillRoundedRectangle (r, 4.0f);
        if (sel || i == dragOverPad)
        {
            g.setColour (i == dragOverPad ? Colours::text : c);
            g.drawRoundedRectangle (r.reduced (1.0f), 4.0f, 2.0f);
        }
        if (drums.padHasUserSample (i))
        {
            g.setColour (Colours::text);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ r.getRight() - 7.0f, r.getY() + 7.0f }));
        }

        g.setColour (flash > 0.5f ? Colours::textInverse : Colours::text);
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        const bool small = kitMode;
        g.drawFittedText (drums.getPadDisplayName (i), r.toNearestInt().reduced (4, 2), small ? juce::Justification::centred : juce::Justification::topLeft, small ? 1 : 2, 0.7f);
        if (! small)
        {
            g.setColour (flash > 0.5f ? Colours::textInverse : Colours::textDim);
            g.setFont (juce::FontOptions (9.0f));
            g.drawText (juce::MidiMessage::getMidiNoteName (DrumsDevice::firstNote + i, true, true, 3), r.toNearestInt().reduced (5, 3), juce::Justification::bottomLeft);
        }
    }

    /**
        The kit from above, as line art: drums with their tension lugs (the snare with its
        wires), the kick as a long shell under the toms with its pedal, cymbals with grooves,
        a bell and a stand, the hi-hats on theirs. Each piece fills with light when it's hit.
    */
    void paintKit (juce::Graphics& g)
    {
        const auto ink = Colours::text;

        auto strokeFor = [&] (int i) { return (i == selected || i == dragOverPad) ? 2.2f : 1.3f; };
        auto lineColour = [&] (int i) { return i == dragOverPad ? ink : colourFor (i).withAlpha (i == selected ? 1.0f : 0.8f); };
        auto fillColour = [&] (int i) { return Colours::panel.interpolatedWith (colourFor (i), 0.08f + (i == selected ? 0.12f : 0.0f) + 0.8f * flashes[(size_t) i]); };

        auto label = [&] (int i, juce::Rectangle<float> e)
        {
            g.setColour (flashes[(size_t) i] > 0.5f ? Colours::textInverse : (i == selected ? ink : Colours::textDim));
            g.setFont (juce::FontOptions (9.0f, i == selected ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (drums.getPadDisplayName (i), e.withTrimmedTop (e.getHeight() * 0.55f).toNearestInt(), juce::Justification::centredTop, 1, 0.6f);
        };

        // A stand: a line out from the centre, ending in three feet
        auto stand = [&] (juce::Point<float> from, float angle, float length, juce::Colour c)
        {
            const juce::Point<float> end { from.x + std::cos (angle) * length, from.y + std::sin (angle) * length };
            g.setColour (c.withAlpha (0.45f));
            g.drawLine ({ from, end }, 1.0f);
            for (float a : { -2.2f, 0.0f, 2.2f })
                g.drawLine ({ end, end.translated (std::cos (angle + a) * 6.0f, std::sin (angle + a) * 6.0f) }, 1.0f);
        };

        auto drum = [&] (int i, juce::Rectangle<float> e, int lugs, bool wires)
        {
            const auto c = lineColour (i);
            g.setColour (fillColour (i));
            g.fillEllipse (e);
            g.setColour (c);
            g.drawEllipse (e, strokeFor (i));
            g.setColour (c.withAlpha (0.3f));
            g.drawEllipse (e.reduced (e.getWidth() * 0.1f), 1.0f);

            if (wires)
            {
                g.setColour (c.withAlpha (0.45f));
                for (int k = -1; k <= 1; ++k)
                {
                    const float y = e.getCentreY() + (float) k * e.getHeight() * 0.09f;
                    const float half = std::sqrt (juce::jmax (0.0f, juce::square (e.getWidth() * 0.38f) - juce::square (y - e.getCentreY())));
                    g.drawLine (e.getCentreX() - half, y, e.getCentreX() + half, y, 1.0f);
                }
            }

            const float r = e.getWidth() * 0.5f;
            for (int k = 0; k < lugs; ++k)
            {
                const float a = juce::MathConstants<float>::twoPi * (float) k / (float) lugs + 0.2f;
                const juce::Point<float> p { e.getCentreX() + std::cos (a) * r, e.getCentreY() + std::sin (a) * r };
                g.setColour (fillColour (i));
                g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (p));
                g.setColour (c);
                g.drawEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (p), 1.0f);
            }
            label (i, e);
        };

        auto cymbal = [&] (int i, juce::Rectangle<float> e, float standAngle, bool hat)
        {
            const auto c = lineColour (i);
            stand (e.getCentre(), standAngle, e.getWidth() * 0.5f + 10.0f, c);

            if (hat)   // the bottom cymbal peeking out
            {
                g.setColour (c.withAlpha (0.35f));
                g.drawEllipse (e.translated (2.5f, 2.5f), 1.0f);
            }

            g.setColour (fillColour (i));
            g.fillEllipse (e);
            g.setColour (c);
            g.drawEllipse (e, strokeFor (i));

            // Grooves: partial arcs, like light catching the lathe lines
            g.setColour (c.withAlpha (0.4f));
            for (float k : { 0.78f, 0.6f, 0.42f })
            {
                juce::Path arc;
                const auto ring = e.withSizeKeepingCentre (e.getWidth() * k, e.getHeight() * k);
                arc.addCentredArc (ring.getCentreX(), ring.getCentreY(), ring.getWidth() * 0.5f, ring.getHeight() * 0.5f, 0.0f,
                                   0.9f + k, 2.6f + k * 1.5f, true);
                g.strokePath (arc, juce::PathStrokeType (1.0f));
            }
            g.setColour (c.withAlpha (0.6f));
            g.drawEllipse (e.withSizeKeepingCentre (e.getWidth() * 0.18f, e.getHeight() * 0.18f), 1.0f);
            g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (e.getCentre()));
            label (i, e);
        };

        for (int i : drawOrder())
        {
            const auto e = padBounds (i).toFloat();
            switch (i)
            {
                case 0:   // the kick, seen from above: a long shell, hoops at each end, and its pedal
                {
                    const auto c = lineColour (i);
                    g.setColour (fillColour (i));
                    g.fillRoundedRectangle (e, 3.0f);
                    g.setColour (c);
                    g.drawRoundedRectangle (e, 3.0f, strokeFor (i));
                    g.setColour (c.withAlpha (0.4f));
                    g.drawHorizontalLine ((int) (e.getY() + 5.0f), e.getX() + 2.0f, e.getRight() - 2.0f);
                    g.drawHorizontalLine ((int) (e.getBottom() - 5.0f), e.getX() + 2.0f, e.getRight() - 2.0f);
                    auto pedal = juce::Rectangle<float> (e.getWidth() * 0.32f, 9.0f).withCentre ({ e.getCentreX(), e.getBottom() + 7.0f });
                    g.drawRoundedRectangle (pedal, 2.0f, 1.0f);
                    g.drawLine (pedal.getCentreX(), pedal.getY(), pedal.getCentreX(), e.getBottom() - 1.0f, 1.0f);
                    label (i, e.withTrimmedTop (e.getHeight() * 0.1f));
                    break;
                }
                case 1:   drum (i, e, 8, true); break;
                case 5:   drum (i, e, 8, false); break;
                case 6: case 7: drum (i, e, 6, false); break;
                case 3:   cymbal (i, e, 2.5f, true); break;
                case 4:   cymbal (i, e, 3.6f, true); break;
                case 10:  cymbal (i, e, -2.3f, false); break;
                case 11:  cymbal (i, e, -0.8f, false); break;
                default:  break;
            }

            if (drums.padHasUserSample (i))
            {
                g.setColour (ink);
                g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ e.getCentreX(), e.getY() + 7.0f }));
            }
        }
    }

    /** The selected sound on the display: its waveform, in the screen's light. */
    void drawWaveform (juce::Graphics& g, juce::Rectangle<float> r)
    {
        auto snd = drums.getSound (selected);
        if (snd == nullptr || snd->audio.getNumSamples() < 2)
            return;

        const auto& a = snd->audio;
        const int n = a.getNumSamples();
        const int w = juce::jmax (1, (int) r.getWidth() / 2);
        const float mid = r.getCentreY();
        g.setColour (SynthPanel::Oled::faint());
        g.fillRect (r.getX(), mid, r.getWidth(), 1.0f);
        g.setColour (SynthPanel::Oled::ink());
        for (int x = 0; x < w; ++x)
        {
            const int s0 = (int) ((juce::int64) x * n / w), s1 = (int) ((juce::int64) (x + 1) * n / w);
            float peak = 0.0f;
            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                peak = juce::jmax (peak, a.getMagnitude (ch, s0, juce::jmax (1, s1 - s0)));
            const float h = juce::jmax (0.5f, peak * r.getHeight() * 0.46f);
            g.fillRect (r.getX() + (float) x * 2.0f, mid - h, 1.2f, h * 2.0f);
        }
    }

    juce::Rectangle<int> padBounds (int i) const
    {
        if (kitMode)
        {
            if (isExtra (i))
            {
                static const int extras[] = { 2, 8, 9, 12, 13, 14, 15 };
                auto row = gridArea.withTop (gridArea.getBottom() - 24);
                auto w = row.getWidth() / 7;
                for (int k = 0; k < 7; ++k)
                    if (extras[k] == i)
                        return { row.getX() + k * w, row.getY(), w, row.getHeight() };
            }

            // The kit from above: centre (x as a fraction of the width, y of the height) and
            // diameter (a fraction of the height); the kick is a box (width, height fractions)
            struct Spot { float x, y, d; };
            static const std::map<int, Spot> spots {
                { 10, { 0.25f, 0.28f, 0.56f } },     // crash
                { 11, { 0.81f, 0.30f, 0.6f } },      // ride
                { 4,  { 0.08f, 0.46f, 0.34f } },     // open hat
                { 3,  { 0.12f, 0.79f, 0.32f } },     // closed hat
                { 7,  { 0.42f, 0.26f, 0.3f } },      // high tom
                { 6,  { 0.565f, 0.26f, 0.32f } },    // mid tom
                { 5,  { 0.71f, 0.72f, 0.44f } },     // floor tom
                { 1,  { 0.32f, 0.71f, 0.38f } } };   // snare
            auto a = drumArea.toFloat().reduced (6.0f);
            if (i == 0)
                return juce::Rectangle<float> (a.getWidth() * 0.085f, a.getHeight() * 0.6f)
                         .withCentre ({ a.getX() + a.getWidth() * 0.495f, a.getY() + a.getHeight() * 0.56f }).toNearestInt();
            auto it = spots.find (i);
            if (it == spots.end()) return {};
            auto& sp = it->second;
            const float size = a.getHeight() * sp.d;
            return juce::Rectangle<float> (size, size)
                     .withCentre ({ a.getX() + a.getWidth() * sp.x, a.getY() + a.getHeight() * sp.y }).toNearestInt();
        }

        // Row 0 at the bottom, like hardware drum machines (C1 bottom-left)
        auto cell = gridArea.getWidth() / 4;
        auto cellH = gridArea.getHeight() / 4;
        int col = i % 4, row = 3 - i / 4;
        return { gridArea.getX() + col * cell, gridArea.getY() + row * cellH, cell, cellH };
    }

    int padAt (juce::Point<int> p) const
    {
        if (kitMode)
        {
            // Front-most first, and inside the drum's circle (not just its bounding box)
            auto order = drawOrder();
            for (auto it = order.rbegin(); it != order.rend(); ++it)
            {
                auto e = padBounds (*it).toFloat();
                if (*it == 0)
                {
                    if (e.contains (p.toFloat())) return 0;
                    continue;
                }
                auto d = (p.toFloat() - e.getCentre());
                if (juce::square (d.x / (e.getWidth() * 0.5f)) + juce::square (d.y / (e.getHeight() * 0.5f)) <= 1.0f)
                    return *it;
            }
        }

        for (int i = 0; i < DrumsDevice::numPads; ++i)
            if ((! kitMode || isExtra (i)) && padBounds (i).contains (p))
                return i;

        return -1;
    }

    void setDragOver (int pad)  { if (pad != dragOverPad) { dragOverPad = pad; repaint(); } }

    void setKitMode (bool kit)
    {
        kitMode = kit;
        drums.state.setProperty ("view", kit ? "kit" : "pads", nullptr);
        resized();
        repaint();
    }

    void selectPad (int pad)
    {
        selected = pad;
        auto& p = drums.pads[(size_t) pad];
        const auto c = colourFor (pad);
        padKnobOwners.clear();
        padKnobs.clear();
        for (auto [param, name] : { std::pair { p.level, "Level" }, std::pair { p.pan, "Pan" }, std::pair { p.tune, "Tune" },
                                    std::pair { p.decay, "Decay" }, std::pair { p.tone, "Tone" }, std::pair { p.drive, "Drive" } })
        {
            auto k = std::make_unique<SynthPanel::Knob> (param.param(), name, c);
            k->onTouch = [this] (te::AutomatableParameter& ap) { oled.showParameter (ap); };
            addAndMakeVisible (*k);
            padKnobs.push_back (k.get());
            padKnobOwners.push_back (std::move (k));
        }

        updateDisplay();
        resized();
        repaint();
    }

    void updateDisplay()
    {
        const int kit = drums.getKitIndex();
        shownKit = kit;
        oled.header = "KIT  /  " + juce::String (DrumKits::kits()[(size_t) kit].name).toUpperCase();
        oled.patchName = drums.getPadDisplayName (selected) + (drums.padHasUserSample (selected) ? "  (sample)" : "");
    }

    void stepKit (int delta)
    {
        const int count = (int) DrumKits::kits().size();
        drums.edit.getUndoManager().beginNewTransaction ("Change drum kit");
        drums.setKit ((drums.getKitIndex() + delta + count) % count);
        updateDisplay();
        repaint();
    }

    void showKitMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Synthesised");
        for (int i = 0; i < (int) DrumKits::kits().size(); ++i)
        {
            const auto& k = DrumKits::kits()[(size_t) i];
            if (k.recorded && (i == 0 || ! DrumKits::kits()[(size_t) i - 1].recorded))
                m.addSectionHeader ("Recorded (Karoryfer, CC0)");
            m.addItem (i + 1, juce::String (k.name) + juce::String::fromUTF8 ("  \xe2\x80\x94  ") + k.description, true, i == drums.getKitIndex());
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitMenu), [safe = juce::Component::SafePointer<DrumsView> (this)] (int result)
        {
            if (safe == nullptr || result <= 0) return;
            safe->drums.edit.getUndoManager().beginNewTransaction ("Change drum kit");
            safe->drums.setKit (result - 1);
            safe->updateDisplay();
            safe->repaint();
        });
    }

    static juce::File fileFrom (const SourceDetails& d)
    {
        juce::File f;
        if (auto ft = dynamic_cast<juce::FileTreeComponent*> (d.sourceComponent.get()))
            f = ft->getSelectedFile();
        return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg") ? f : juce::File();
    }

    void timerCallback() override
    {
        bool changed = false;

        for (int i = 0; i < DrumsDevice::numPads; ++i)
        {
            auto c = drums.getTriggerCount (i);
            if (c != lastCounts[(size_t) i]) { lastCounts[(size_t) i] = c; flashes[(size_t) i] = 1.0f; changed = true; }
            else if (flashes[(size_t) i] > 0.0f) { flashes[(size_t) i] = juce::jmax (0.0f, flashes[(size_t) i] - 0.12f); changed = true; }
        }

        if (drums.getKitIndex() != shownKit)
        {
            updateDisplay();
            changed = true;
        }

        if (changed)
            repaint (gridArea.expanded (4));
    }
};


inline std::unique_ptr<juce::Component> DrumsDevice::createDeviceView()    { return std::make_unique<DrumsView> (*this); }

} // namespace elsewhere
