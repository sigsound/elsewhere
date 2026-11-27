// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "OrganicDsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Organic: an instrument built from recordings of real instruments and textures.

    Two layers (A and B), each playing a recorded source through a grain cloud: at Texture 0
    the recording plays naturally (its real attack, its real tone); as Texture rises it
    dissolves into a slow, wide, evolving cloud. Sustaining sources hold for as long as a key
    is down by wandering through the steady part of the recording.

    Morph blends the layers (a slow Tide can sway it, differently for every voice). The blend
    rings in a tuned Body (string, tube, bowl, glass or wood) that the sound and a breath of
    Air excite. Expression drives everything at once: velocity, mod wheel, breath, expression
    pedal and aftertouch move the level, brightness, the recorded soft-to-loud dynamic layers
    and the breath together, with an optional slow Bloom after each note. A built-in Space
    finishes it.

    Sources come from sound packs (SoundLibrary), with built-in ones that always work.
*/
class OrganicDevice : public ElsewhereDevice, private juce::Timer
{
public:
    static constexpr const char* xmlTypeName = "elsewhereOrganic";
    static constexpr int maxVoices = OrganicEngine::maxVoices;

    OrganicDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto semis = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st"; };
        auto hz = [] (float v) { return juce::String (v, v < 1.0f ? 2 : 1) + " Hz"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        const juce::NormalisableRange<float> pct { 0.0f, 100.0f };
        const OrganicEngine::Params d;     // the defaults

        aTexture  = addParameter ("atex", "A Texture", d.aTexture, pct, percentText);
        aPosition = addParameter ("apos", "A Position", d.aPosition, pct, percentText);
        aTune     = addParameter ("atune", "A Tune", d.aTune, stepped (-24, 24), semis);
        aLevel    = addParameter ("alevel", "A Level", d.aLevel, pct, percentText);
        bTexture  = addParameter ("btex", "B Texture", d.bTexture, pct, percentText);
        bPosition = addParameter ("bpos", "B Position", d.bPosition, pct, percentText);
        bTune     = addParameter ("btune", "B Tune", d.bTune, stepped (-24, 24), semis);
        bLevel    = addParameter ("blevel", "B Level", d.bLevel, pct, percentText);
        morph     = addParameter ("morph", "Morph", d.morph, pct, percentText);
        tide      = addParameter ("tide", "Tide", d.tide, skewed (0.01f, 2.0f, 0.15f), hz);
        tideDepth = addParameter ("tdepth", "Tide Depth", d.tideDepth, pct, percentText);
        bodyType  = addParameter ("body", "Body", d.body, stepped (0, OrganicDsp::Body::numTypes - 1), pick (OrganicDsp::Body::names()));
        resonance = addParameter ("res", "Resonance", d.resonance, pct, percentText);
        bodyDecay = addParameter ("bdecay", "Body Decay", d.bodyDecay, skewed (0.1f, 10.0f, 2.0f), secs);
        air       = addParameter ("air", "Air", d.air, pct, percentText);
        swell     = addParameter ("swell", "Swell", d.swell, pct, percentText);
        dynamics  = addParameter ("dyn", "Dynamics", d.dynamics, pct, percentText);
        bloom     = addParameter ("bloom", "Bloom", d.bloom, skewed (0.0f, 6.0f, 1.0f), secs);
        bright    = addParameter ("bright", "Brightness", d.bright, freqRange (200.0f, 20000.0f), hzText);
        warmth    = addParameter ("warmth", "Warmth", d.warmth, pct, percentText);
        attack    = addParameter ("attack", "Attack", d.attack, skewed (0.001f, 8.0f, 0.3f), secs);
        release   = addParameter ("release", "Release", d.release, skewed (0.02f, 12.0f, 1.5f), secs);
        life      = addParameter ("life", "Life", d.life, pct, percentText);
        spaceSize = addParameter ("space", "Space", d.space, pct, percentText);
        spaceMix  = addParameter ("smix", "Space Mix", d.spaceMix, pct, percentText);
        width     = addParameter ("width", "Width", d.width, pct, percentText);
        volume    = addParameter ("volume", "Volume", d.volume, { -60.0f, 6.0f }, dbText);

        watcher = std::make_unique<Watcher> (*this);
        state.addListener (watcher.get());

        if (! state.hasProperty ("srcA"))
        {
            const auto& p = patches()[0];
            state.setProperty ("srcA", p.a, nullptr);
            state.setProperty ("srcB", p.b, nullptr);
            state.setProperty ("patch", 0, nullptr);
        }

        startTimer (1000);
    }

    ~OrganicDevice() override
    {
        stopTimer();
        state.removeListener (watcher.get());
        alive.reset();
    }

    juce::String getName() const override              { return "Elsewhere Organic"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Organic"; }
    int getDeviceViewWidth() const override            { return 1380; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1180, 720 }; }

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 12.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }

    //==============================================================================
    // Sources
    static juce::String packId()    { return "organic"; }

    /** The family of a source key (so a missing one gets a fitting stand-in). */
    static juce::String familyOf (const juce::String& key)     { return OrganicEngine::familyOf (key.toRawUTF8()); }

    juce::String getSourceKey (int layer) const     { return state.getProperty (layer == 0 ? "srcA" : "srcB").toString(); }

    void setSourceKey (int layer, const juce::String& key)
    {
        state.setProperty (layer == 0 ? "srcA" : "srcB", key, &edit.getUndoManager());
    }

    /** The playing source's name (or the wanted one, if its pack isn't installed). */
    juce::String getSourceName (int layer) const
    {
        const juce::ScopedLock sl (slotLock);
        auto& s = slots[(size_t) layer];
        if (s.missing)
            return s.wantedName;
        return s.held != nullptr && s.held->lib != nullptr ? s.held->lib->name : juce::String ("Loading");
    }

    /** After a pack is installed: load the real sources in place of the stand-ins. */
    void reloadSources()                            { requestLoad (0); requestLoad (1); }

    bool isSourceMissing (int layer) const          { return slots[(size_t) layer].missing.load(); }
    bool isLoaded() const                           { return engine.getSource (0) != nullptr && engine.getSource (1) != nullptr; }

    std::vector<float> getOverview (int layer) const
    {
        const juce::ScopedLock sl (slotLock);
        auto& s = slots[(size_t) layer];
        return s.held != nullptr && s.held->lib != nullptr ? s.held->lib->overview : std::vector<float> {};
    }

    /** Loads the current sources right away (blocking). The engine does this itself when it
        starts; tests and offline renders can call it too. */
    void loadSourcesNow()
    {
        for (int layer = 0; layer < 2; ++layer)
        {
            const auto key = getSourceKey (layer);
            if (slots[(size_t) layer].loadedKey == key && engine.getSource (layer) != nullptr)
                continue;
            bool missing = false;
            auto src = fetch (key, missing);
            install (layer, key, src, missing);
        }
    }

    //==============================================================================
    // Patches (OrganicEngine keeps them, so anything playing the engine has the same ones)
    using Patch = OrganicEngine::Patch;
    static const std::vector<Patch>& patches()          { return OrganicEngine::patches(); }

    float patchValue (int index, te::AutomatableParameter& ap) const
    {
        return OrganicEngine::patchValue (index, ap.paramID.toRawUTF8());
    }

    /** Sets every control and both sources to a patch: as its own undo step, or as part of the one in progress. */
    void loadPatch (int index, bool ownUndoStep = true)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        const auto& patch = patches()[(size_t) index];

        if (ownUndoStep)
            edit.getUndoManager().beginNewTransaction ("Load Organic patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (index, p->getParameter()), juce::sendNotification);

        setSourceKey (0, patch.a);
        setSourceKey (1, patch.b);
        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    static juce::String patchLabel (int index)
    {
        if (index < 0 || index >= (int) patches().size())
            return "INIT";
        return "O" + juce::String (index + 1).paddedLeft ('0', 3) + " " + patches()[(size_t) index].name;
    }

    bool isPatchModified() const
    {
        const int index = getPatchIndex();
        if (index < 0 || index >= (int) patches().size())
            return true;

        const auto& patch = patches()[(size_t) index];
        if (getSourceKey (0) != patch.a || getSourceKey (1) != patch.b)
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
        loadSourcesNow();
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

    //==============================================================================
    /** The sound itself. */
    OrganicEngine engine;

    // For the display
    using VoiceView = OrganicEngine::VoiceView;
    std::array<VoiceView, maxVoices>& voiceView = engine.voiceView;
    std::atomic<float>& liveSwell = engine.liveSwell;
    std::atomic<int>& expressionSource = engine.expressionSource;     // 0 velocity, 1 mod wheel, 2 breath, 3 expression, 4 pressure
    static juce::StringArray expressionNames()   { return { "Velocity", "Mod Wheel", "Breath", "Expression", "Pressure" }; }
    std::atomic<float>& morphNow = engine.morphNow;

    Param aTexture, aPosition, aTune, aLevel, bTexture, bPosition, bTune, bLevel, morph, tide, tideDepth;
    Param bodyType, resonance, bodyDecay, air, swell, dynamics, bloom, bright, warmth, attack, release;
    Param life, spaceSize, spaceMix, width, volume;

private:
    /** A layer's source: the one loaded (kept here, with the engine's view of it) and what was asked for. */
    struct Slot
    {
        std::shared_ptr<const OrganicDsp::Playable> held;
        std::atomic<bool> missing { false };
        juce::String loadedKey, wantedName;
    };

    struct Watcher : public juce::ValueTree::Listener
    {
        OrganicDevice& owner;
        explicit Watcher (OrganicDevice& o) : owner (o) {}
        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
        {
            if (v == owner.state && (id == juce::Identifier ("srcA") || id == juce::Identifier ("srcB")))
                owner.requestLoad (id == juce::Identifier ("srcA") ? 0 : 1);
        }
    };

    std::unique_ptr<Watcher> watcher;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);
    std::array<Slot, 2> slots;
    juce::CriticalSection slotLock;
    std::vector<std::pair<std::shared_ptr<const OrganicDsp::Playable>, juce::uint32>> graveyard;

    double rate = 48000.0;
    std::vector<float> right;       // for a mono buffer

    //==============================================================================
    static juce::ThreadPool& loaderPool()
    {
        static juce::ThreadPool pool (juce::ThreadPoolOptions().withThreadName ("Elsewhere Organic loader").withNumberOfThreads (1));
        return pool;
    }

    /** Fetches a source (shared between devices): loads it, or a built-in stand-in if its pack isn't installed. */
    static SoundLibrary::SourcePtr fetch (const juce::String& key, bool& missing)
    {
        static std::mutex lock;
        static std::map<juce::String, std::weak_ptr<const SoundLibrary::Source>> cache;

        auto get = [&] (const juce::String& k) -> SoundLibrary::SourcePtr
        {
            {
                std::lock_guard<std::mutex> g (lock);
                if (auto s = cache[k].lock())
                    return s;
            }
            auto s = k.startsWith ("builtin/") ? OrganicDsp::Builtins::make (k.fromFirstOccurrenceOf ("/", false, false))
                                               : SoundLibrary::loadPackSource (k);
            if (s != nullptr)
            {
                std::lock_guard<std::mutex> g (lock);
                cache[k] = s;
            }
            return s;
        };

        missing = false;
        if (auto s = get (key))
            return s;
        missing = true;
        return get (OrganicDsp::Builtins::fallbackFor (familyOf (key)));
    }

    static juce::String nameOf (const juce::String& key)    { return juce::String (OrganicEngine::nameOf (key.toRawUTF8())); }

    void install (int layer, const juce::String& key, SoundLibrary::SourcePtr src, bool missing)
    {
        const juce::ScopedLock sl (slotLock);
        auto& s = slots[(size_t) layer];
        if (s.held != nullptr)
            graveyard.push_back ({ s.held, juce::Time::getMillisecondCounter() });
        s.held = src != nullptr ? std::make_shared<const OrganicDsp::Playable> (src) : nullptr;
        s.loadedKey = key;
        s.wantedName = nameOf (key);
        s.missing = missing;
        // The engine moves its voices off the old source at its next render; the old one stays
        // in the graveyard for three seconds, long after any voice could still be reading it
        if (s.held != nullptr) engine.setSource (layer, s.held->source);
        else engine.clearSource (layer);
    }

    void requestLoad (int layer)
    {
        const auto key = getSourceKey (layer);
        std::weak_ptr<bool> token = alive;
        loaderPool().addJob ([this, token, key, layer]
        {
            bool missing = false;
            auto src = fetch (key, missing);
            juce::MessageManager::callAsync ([this, token, key, layer, src, missing]
            {
                if (token.expired() || getSourceKey (layer) != key)
                    return;
                install (layer, key, src, missing);
            });
        });
    }

    void timerCallback() override
    {
        // Free replaced sources once the engine can no longer be reading them
        const juce::ScopedLock sl (slotLock);
        const auto now = juce::Time::getMillisecondCounter();
        graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                         [now] (auto& g) { return now - g.second > 3000; }), graveyard.end());
    }

    //==============================================================================
    /** The parameters as they are now (automation included) into the engine. */
    void updateEngine()
    {
        auto& p = engine.params;
        p.aTexture = aTexture.get(); p.aPosition = aPosition.get(); p.aTune = aTune.get(); p.aLevel = aLevel.get();
        p.bTexture = bTexture.get(); p.bPosition = bPosition.get(); p.bTune = bTune.get(); p.bLevel = bLevel.get();
        p.morph = morph.get(); p.tide = tide.get(); p.tideDepth = tideDepth.get();
        p.body = bodyType.get(); p.resonance = resonance.get(); p.bodyDecay = bodyDecay.get(); p.air = air.get();
        p.swell = swell.get(); p.dynamics = dynamics.get(); p.bloom = bloom.get();
        p.bright = bright.get(); p.warmth = warmth.get(); p.attack = attack.get(); p.release = release.get();
        p.life = life.get(); p.space = spaceSize.get(); p.spaceMix = spaceMix.get(); p.width = width.get(); p.volume = volume.get();
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 engine.noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              engine.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isControllerOfType (1))      engine.setModWheel ((float) m.getControllerValue() / 127.0f);
        else if (m.isControllerOfType (2))      engine.setBreath ((float) m.getControllerValue() / 127.0f);
        else if (m.isControllerOfType (11))     engine.setExpression ((float) m.getControllerValue() / 127.0f);
        else if (m.isChannelPressure())         engine.setChannelPressure ((float) m.getChannelPressureValue() / 127.0f);
        else if (m.isAftertouch())              engine.setNotePressure (m.getNoteNumber(), (float) m.getAfterTouchValue() / 127.0f);
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

} // namespace elsewhere

#include "OrganicView.h"
