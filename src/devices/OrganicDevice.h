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
    static constexpr int maxVoices = 12;
    static constexpr int chunk = 64;

    OrganicDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto semis = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " st"; };
        auto hz = [] (float v) { return juce::String (v, v < 1.0f ? 2 : 1) + " Hz"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        const juce::NormalisableRange<float> pct { 0.0f, 100.0f };

        aTexture  = addParameter ("atex", "A Texture", 10.0f, pct, percentText);
        aPosition = addParameter ("apos", "A Position", 0.0f, pct, percentText);
        aTune     = addParameter ("atune", "A Tune", 0.0f, stepped (-24, 24), semis);
        aLevel    = addParameter ("alevel", "A Level", 80.0f, pct, percentText);
        bTexture  = addParameter ("btex", "B Texture", 55.0f, pct, percentText);
        bPosition = addParameter ("bpos", "B Position", 20.0f, pct, percentText);
        bTune     = addParameter ("btune", "B Tune", 0.0f, stepped (-24, 24), semis);
        bLevel    = addParameter ("blevel", "B Level", 80.0f, pct, percentText);
        morph     = addParameter ("morph", "Morph", 30.0f, pct, percentText);
        tide      = addParameter ("tide", "Tide", 0.08f, skewed (0.01f, 2.0f, 0.15f), hz);
        tideDepth = addParameter ("tdepth", "Tide Depth", 25.0f, pct, percentText);
        bodyType  = addParameter ("body", "Body", 0.0f, stepped (0, OrganicDsp::Body::numTypes - 1), pick (OrganicDsp::Body::names()));
        resonance = addParameter ("res", "Resonance", 25.0f, pct, percentText);
        bodyDecay = addParameter ("bdecay", "Body Decay", 2.5f, skewed (0.1f, 10.0f, 2.0f), secs);
        air       = addParameter ("air", "Air", 15.0f, pct, percentText);
        swell     = addParameter ("swell", "Swell", 100.0f, pct, percentText);
        dynamics  = addParameter ("dyn", "Dynamics", 60.0f, pct, percentText);
        bloom     = addParameter ("bloom", "Bloom", 0.8f, skewed (0.0f, 6.0f, 1.0f), secs);
        bright    = addParameter ("bright", "Brightness", 9000.0f, freqRange (200.0f, 20000.0f), hzText);
        warmth    = addParameter ("warmth", "Warmth", 20.0f, pct, percentText);
        attack    = addParameter ("attack", "Attack", 0.06f, skewed (0.001f, 8.0f, 0.3f), secs);
        release   = addParameter ("release", "Release", 1.8f, skewed (0.02f, 12.0f, 1.5f), secs);
        life      = addParameter ("life", "Life", 35.0f, pct, percentText);
        spaceSize = addParameter ("space", "Space", 45.0f, pct, percentText);
        spaceMix  = addParameter ("smix", "Space Mix", 28.0f, pct, percentText);
        width     = addParameter ("width", "Width", 70.0f, pct, percentText);
        volume    = addParameter ("volume", "Volume", -6.0f, { -60.0f, 6.0f }, dbText);

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

    /** Families of the Organic Sounds pack's sources (so a missing one gets a fitting stand-in). */
    static juce::String familyOf (const juce::String& key)
    {
        static const std::map<juce::String, juce::String> known = {
            { "cello", "Bowed" }, { "violins", "Bowed" }, { "solovln", "Bowed" }, { "bass", "Bowed" },
            { "flute", "Breath" }, { "clarinet", "Breath" }, { "bassoon", "Breath" }, { "horn", "Breath" },
            { "harp", "Plucked" }, { "bpizz", "Plucked" }, { "vpizz", "Plucked" },
            { "marimba", "Struck" }, { "glock", "Struck" },
            { "bells", "Texture" }, { "cymbal", "Texture" }, { "gong", "Texture" },
            // Organic Sounds 2
            { "violas", "Bowed" }, { "vlntrem", "Bowed" }, { "celtrem", "Bowed" },
            { "oboe", "Breath" }, { "mhorn", "Breath" }, { "trumpet", "Breath" }, { "trombone", "Breath" }, { "tuba", "Breath" },
            { "piano", "Struck" }, { "tubular", "Struck" }, { "organ", "Keys" },
            { "timproll", "Texture" }, { "belltree", "Texture" }, { "cymswell", "Texture" }, { "longbells", "Texture" },
        };
        const auto id = key.fromFirstOccurrenceOf ("/", false, false);
        for (auto& b : OrganicDsp::Builtins::list())
            if (key == juce::String ("builtin/") + b.id)
                return b.family;
        auto it = known.find (id);
        return it != known.end() ? it->second : juce::String ("Bowed");
    }

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
        return s.held != nullptr ? s.held->name : juce::String ("Loading");
    }

    /** After a pack is installed: load the real sources in place of the stand-ins. */
    void reloadSources()                            { requestLoad (0); requestLoad (1); }

    bool isSourceMissing (int layer) const          { return slots[(size_t) layer].missing.load(); }
    bool isLoaded() const                           { return slots[0].live.load() != nullptr && slots[1].live.load() != nullptr; }

    std::vector<float> getOverview (int layer) const
    {
        const juce::ScopedLock sl (slotLock);
        auto& s = slots[(size_t) layer];
        return s.held != nullptr ? s.held->overview : std::vector<float> {};
    }

    /** Loads the current sources right away (blocking). The engine does this itself when it
        starts; tests and offline renders can call it too. */
    void loadSourcesNow()
    {
        for (int layer = 0; layer < 2; ++layer)
        {
            const auto key = getSourceKey (layer);
            if (slots[(size_t) layer].loadedKey == key && slots[(size_t) layer].live.load() != nullptr)
                continue;
            bool missing = false;
            auto src = fetch (key, missing);
            install (layer, key, src, missing);
        }
    }

    //==============================================================================
    // Patches
    struct Patch { const char* name; const char* a; const char* b; std::vector<std::pair<const char*, float>> values; };

    static const std::vector<Patch>& patches()
    {
        static const std::vector<Patch> all = {
            { "Horizon Strings",    "organic/cello", "organic/violins",
              { { "atex", 12 }, { "btex", 45 }, { "bpos", 30 }, { "morph", 40 }, { "tdepth", 30 }, { "res", 20 }, { "bloom", 1.2f },
                { "space", 55 }, { "smix", 30 }, { "attack", 0.12f }, { "release", 2.2f } } },
            { "Glass Breath",       "organic/flute", "organic/glock",
              { { "atex", 25 }, { "btex", 80 }, { "bpos", 0 }, { "morph", 35 }, { "body", 3 }, { "res", 45 }, { "bdecay", 4.0f }, { "air", 30 },
                { "space", 60 }, { "smix", 38 } } },
            { "Wooden Air",         "organic/clarinet", "organic/marimba",
              { { "atex", 8 }, { "btex", 30 }, { "bpos", 0 }, { "morph", 25 }, { "body", 4 }, { "res", 35 }, { "bloom", 0.3f }, { "attack", 0.02f } } },
            { "Iron Cathedral",     "organic/horn", "organic/cymbal",
              { { "atex", 15 }, { "btex", 70 }, { "morph", 45 }, { "tdepth", 40 }, { "body", 2 }, { "res", 30 }, { "bdecay", 5.0f },
                { "space", 85 }, { "smix", 45 }, { "release", 4.0f } } },
            { "Harp Rain",          "organic/harp", "organic/bells",
              { { "atex", 35 }, { "btex", 65 }, { "bpos", 0 }, { "morph", 30 }, { "res", 30 }, { "space", 65 }, { "smix", 40 },
                { "attack", 0.002f }, { "release", 3.0f } } },
            { "Deep Current",       "organic/bass", "organic/bassoon",
              { { "atex", 5 }, { "btex", 40 }, { "morph", 35 }, { "dyn", 85 }, { "bloom", 1.5f }, { "bright", 4000 }, { "warmth", 40 },
                { "space", 35 }, { "smix", 20 } } },
            { "Paper Moon",         "organic/solovln", "organic/vpizz",
              { { "atex", 60 }, { "btex", 20 }, { "bpos", 0 }, { "morph", 50 }, { "tide", 0.15f }, { "tdepth", 50 }, { "body", 3 }, { "res", 20 },
                { "space", 60 }, { "smix", 35 }, { "volume", -2 } } },
            { "Bowl Garden",        "organic/glock", "organic/gong",
              { { "atex", 50 }, { "btex", 85 }, { "morph", 40 }, { "body", 2 }, { "res", 55 }, { "bdecay", 6.0f }, { "air", 20 },
                { "space", 75 }, { "smix", 45 }, { "attack", 0.8f }, { "release", 5.0f } } },
            { "Frozen Lake",        "organic/violins", "organic/cymbal",
              { { "atex", 95 }, { "apos", 40 }, { "btex", 90 }, { "morph", 30 }, { "space", 90 }, { "smix", 50 }, { "attack", 1.5f },
                { "release", 6.0f }, { "bloom", 2.5f }, { "volume", -2 } } },
            { "Pizz Constellation", "organic/bpizz", "organic/vpizz",
              { { "atex", 70 }, { "btex", 90 }, { "bpos", 0 }, { "morph", 50 }, { "tide", 0.3f }, { "tdepth", 60 }, { "body", 3 }, { "res", 25 },
                { "space", 70 }, { "smix", 42 }, { "attack", 0.002f }, { "release", 2.5f }, { "volume", 3 } } },
            // Organic Sounds 2: the orchestra, keys and bells
            { "Adagio Strings",     "organic/violins", "organic/violas",
              { { "atex", 8 }, { "btex", 15 }, { "morph", 45 }, { "tdepth", 20 }, { "res", 10 }, { "bloom", 1.5f }, { "space", 60 }, { "smix", 32 },
                { "attack", 0.35f }, { "release", 2.5f }, { "life", 25 } } },
            { "Brass Choir",        "organic/horn", "organic/trombone",
              { { "atex", 10 }, { "btex", 15 }, { "morph", 45 }, { "tdepth", 15 }, { "res", 0 }, { "dyn", 80 }, { "bloom", 0.8f }, { "space", 55 },
                { "smix", 28 }, { "attack", 0.1f }, { "release", 1.6f } } },
            { "Trumpet Fanfare",    "organic/trumpet", "organic/horn",
              { { "atex", 0 }, { "btex", 5 }, { "morph", 40 }, { "tdepth", 0 }, { "res", 0 }, { "dyn", 75 }, { "bloom", 0.0f }, { "space", 45 },
                { "smix", 25 }, { "attack", 0.03f }, { "release", 1.2f } } },
            { "Low Brass",          "organic/tuba", "organic/trombone",
              { { "atex", 5 }, { "btex", 10 }, { "morph", 50 }, { "tdepth", 0 }, { "res", 0 }, { "dyn", 85 }, { "bloom", 0.5f }, { "space", 40 },
                { "smix", 22 }, { "attack", 0.06f }, { "release", 1.4f } } },
            { "Muted Brass Night",  "organic/mhorn", "organic/cello",
              { { "atex", 10 }, { "btex", 20 }, { "morph", 35 }, { "res", 0 }, { "bright", 5000 }, { "space", 60 }, { "smix", 34 },
                { "attack", 0.15f }, { "release", 2.2f } } },
            { "Oboe Pastorale",     "organic/oboe", "organic/harp",
              { { "atex", 0 }, { "btex", 35 }, { "blevel", 60 }, { "morph", 40 }, { "tdepth", 10 }, { "body", 1 }, { "res", 10 }, { "space", 50 },
                { "smix", 30 }, { "attack", 0.05f }, { "release", 1.6f } } },
            { "Tremolo Suspense",   "organic/vlntrem", "organic/celtrem",
              { { "atex", 10 }, { "btex", 10 }, { "morph", 50 }, { "tdepth", 30 }, { "res", 0 }, { "dyn", 70 }, { "bloom", 1.0f }, { "space", 55 },
                { "smix", 30 }, { "attack", 0.2f }, { "release", 2.0f } } },
            { "Timpani Swell",      "organic/celtrem", "organic/timproll",
              { { "atex", 10 }, { "btex", 20 }, { "morph", 45 }, { "res", 0 }, { "dyn", 90 }, { "bloom", 2.0f }, { "space", 65 }, { "smix", 35 },
                { "attack", 0.6f }, { "release", 3.0f } } },
            { "Nocturne Piano",     "organic/piano", "organic/violins",
              { { "atex", 0 }, { "btex", 40 }, { "blevel", 45 }, { "morph", 30 }, { "tdepth", 15 }, { "res", 0 }, { "dyn", 70 }, { "bloom", 0.0f },
                { "space", 50 }, { "smix", 28 }, { "attack", 0.002f }, { "release", 1.8f }, { "life", 15 }, { "volume", 2 } } },
            { "Cathedral Organ",    "organic/organ", "organic/cymswell",
              { { "atex", 5 }, { "btex", 70 }, { "blevel", 35 }, { "morph", 25 }, { "tdepth", 10 }, { "res", 0 }, { "dyn", 40 }, { "space", 85 },
                { "smix", 45 }, { "attack", 0.08f }, { "release", 3.5f }, { "volume", -14 } } },
            { "Tubular Hall",       "organic/tubular", "organic/longbells",
              { { "atex", 0 }, { "btex", 60 }, { "blevel", 50 }, { "morph", 30 }, { "res", 15 }, { "body", 2 }, { "bdecay", 5.0f }, { "space", 90 },
                { "smix", 50 }, { "attack", 0.002f }, { "release", 6.0f } } },
            { "Bell Tree Sparkle",  "organic/glock", "organic/belltree",
              { { "atex", 30 }, { "btex", 50 }, { "bpos", 0 }, { "morph", 45 }, { "body", 3 }, { "res", 20 }, { "space", 70 }, { "smix", 40 },
                { "attack", 0.002f }, { "release", 3.0f } } },
            { "Breathing Choir",    "builtin/choir", "organic/flute",
              { { "atex", 20 }, { "btex", 70 }, { "morph", 35 }, { "air", 35 }, { "body", 1 }, { "res", 25 }, { "bloom", 1.0f } } },
            { "Air Organ",          "builtin/bow", "builtin/choir",
              { { "atex", 20 }, { "btex", 50 }, { "morph", 40 }, { "body", 1 }, { "res", 30 }, { "space", 50 } } },
            { "Wind Glass",         "builtin/wind", "builtin/glass",
              { { "atex", 60 }, { "btex", 40 }, { "bpos", 0 }, { "morph", 55 }, { "body", 3 }, { "res", 40 }, { "bdecay", 4.0f }, { "space", 70 },
                { "smix", 40 }, { "attack", 0.4f }, { "release", 3.5f } } },
        };
        return all;
    }

    float patchValue (const Patch& patch, te::AutomatableParameter& ap) const
    {
        for (auto& [id, value] : patch.values)
            if (ap.paramID == id)
                return value;
        return ap.getDefaultValue().value_or (ap.getCurrentBaseValue());
    }

    void loadPatch (int index)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        const auto& patch = patches()[(size_t) index];

        edit.getUndoManager().beginNewTransaction ("Load Organic patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (patch, p->getParameter()), juce::sendNotification);

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
            const float v = patchValue (patch, ap);
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
        space.prepare (rate);
        for (size_t i = 0; i < voice.size(); ++i)
        {
            voice[i] = Voice();
            voice[i].drift = SynthCore::Drift ((int) i * 13 + 7);
            voice[i].flutter = SynthCore::Drift ((int) i * 17 + 3);
        }
        noteCounter = 0;
        bend = 0.0f;
        pedal = false;
        loadSourcesNow();
    }

    void deinitialise() override {}

    void midiPanic() override
    {
        for (auto& v : voice) { v.held = v.sustained = false; v.amp.gate (false); }
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
    // For the display
    struct VoiceView { float x[2] {}, amp = 0.0f; bool active = false; };
    std::array<VoiceView, maxVoices> voiceView;
    std::atomic<float> liveSwell { 0.0f };
    std::atomic<int> expressionSource { 0 };     // 0 velocity, 1 mod wheel, 2 breath, 3 expression, 4 pressure
    static juce::StringArray expressionNames()   { return { "Velocity", "Mod Wheel", "Breath", "Expression", "Pressure" }; }
    std::atomic<float> morphNow { 0.3f };

    Param aTexture, aPosition, aTune, aLevel, bTexture, bPosition, bTune, bLevel, morph, tide, tideDepth;
    Param bodyType, resonance, bodyDecay, air, swell, dynamics, bloom, bright, warmth, attack, release;
    Param life, spaceSize, spaceMix, width, volume;

private:
    struct Voice
    {
        int note = -1, take = 0;
        float vel = 1.0f, press = 0.0f, sw = 0.0f, bloomT = 0.0f, pan = 0.0f, tideOffset = 0.0f, kill = 1.0f;
        bool held = false, sustained = false, killing = false;
        juce::uint32 age = 0;
        std::array<OrganicDsp::Cloud, 2> cloud;
        std::array<const SoundLibrary::Source*, 2> src {};
        OrganicDsp::Body body;
        SynthCore::Svf toneL, toneR, airBp;
        SynthCore::AnalogEnvelope amp;
        SynthCore::Drift drift, flutter;
        bool active() const { return amp.isActive(); }
    };

    struct Slot
    {
        SoundLibrary::SourcePtr held;
        std::atomic<const SoundLibrary::Source*> live { nullptr };
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
    std::vector<std::pair<SoundLibrary::SourcePtr, juce::uint32>> graveyard;

    double rate = 48000.0;
    std::array<Voice, maxVoices> voice;
    OrganicDsp::Space space;
    SynthCore::Noise noise;
    juce::uint32 noteCounter = 0;
    float bend = 0.0f, ctrl = 0.0f, tidePhase = 0.0f, morphSmooth = 0.3f;
    bool ctrlActive = false, pedal = false;
    std::array<float, chunk> tl {}, tr {}, bl {}, br {}, ml {}, mr {};

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

    static juce::String nameOf (const juce::String& key)
    {
        static const std::map<juce::String, juce::String> names = {
            { "cello", "Cello Ensemble" }, { "violins", "Violin Ensemble" }, { "solovln", "Solo Violin" }, { "bass", "Contrabass" },
            { "flute", "Flute" }, { "clarinet", "Clarinet" }, { "bassoon", "Bassoon" }, { "horn", "French Horn" },
            { "harp", "Harp" }, { "bpizz", "Contrabass Pizz" }, { "vpizz", "Violin Pizz" }, { "marimba", "Marimba" },
            { "glock", "Glockenspiel" }, { "bells", "Nepalese Bells" }, { "cymbal", "Bowed Cymbal" }, { "gong", "Gong Scrape" },
            { "violas", "Viola Ensemble" }, { "vlntrem", "Violin Tremolo" }, { "celtrem", "Cello Tremolo" }, { "oboe", "Oboe" },
            { "mhorn", "Muted Horn" }, { "trumpet", "Trumpet" }, { "trombone", "Trombone" }, { "tuba", "Tuba" },
            { "piano", "Upright Piano" }, { "tubular", "Tubular Bells" }, { "organ", "Pipe Organ" }, { "timproll", "Timpani Roll" },
            { "belltree", "Bell Tree" }, { "cymswell", "Cymbal Swell" }, { "longbells", "Bell Drone" },
        };
        auto it = names.find (key.fromFirstOccurrenceOf ("/", false, false));
        return it != names.end() ? it->second : key;
    }

    void install (int layer, const juce::String& key, SoundLibrary::SourcePtr src, bool missing)
    {
        const juce::ScopedLock sl (slotLock);
        auto& s = slots[(size_t) layer];
        if (s.held != nullptr)
            graveyard.push_back ({ s.held, juce::Time::getMillisecondCounter() });
        s.held = src;
        s.loadedKey = key;
        s.wantedName = nameOf (key);
        s.missing = missing;
        s.live = src.get();
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
        // Free replaced sources once no voice can still be reading them
        const juce::ScopedLock sl (slotLock);
        const auto now = juce::Time::getMillisecondCounter();
        graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                         [now] (auto& g) { return now - g.second > 3000; }), graveyard.end());
    }

    //==============================================================================
    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
        else if (m.isControllerOfType (1))      setCtrl ((float) m.getControllerValue() / 127.0f, 1);
        else if (m.isControllerOfType (2))      setCtrl ((float) m.getControllerValue() / 127.0f, 2);
        else if (m.isControllerOfType (11))     setCtrl ((float) m.getControllerValue() / 127.0f, 3);
        else if (m.isChannelPressure())         setCtrl ((float) m.getChannelPressureValue() / 127.0f, 4);
        else if (m.isAftertouch())
        {
            for (auto& v : voice)
                if (v.note == m.getNoteNumber() && v.held)
                    v.press = (float) m.getAfterTouchValue() / 127.0f;
        }
        else if (m.isSustainPedalOn())          pedal = true;
        else if (m.isSustainPedalOff())
        {
            pedal = false;
            for (auto& v : voice)
                if (v.sustained && ! v.held) { v.sustained = false; v.amp.gate (false); }
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            midiPanic();
    }

    void setCtrl (float value, int source)
    {
        ctrl = value;
        ctrlActive = true;
        expressionSource = source;
    }

    void noteOn (int note, float vel)
    {
        Voice* target = nullptr;
        for (auto& v : voice)
            if (v.active() && v.note == note) { target = &v; break; }
        if (target == nullptr)
            for (auto& v : voice)
                if (! v.active()) { target = &v; break; }
        if (target == nullptr)
        {
            for (int pass = 0; pass < 2 && target == nullptr; ++pass)
            {
                juce::uint32 oldest = std::numeric_limits<juce::uint32>::max();
                for (auto& v : voice)
                    if ((pass == 1 || ! v.held) && v.age < oldest) { oldest = v.age; target = &v; }
            }
        }

        auto& v = *target;
        const bool wasSilent = ! v.active();
        v.note = note;
        v.vel = vel;
        v.press = 0.0f;
        v.held = true;
        v.sustained = false;
        v.killing = false;
        v.kill = 1.0f;
        v.age = ++noteCounter;
        v.take = (int) (noteCounter % 4);
        v.bloomT = 0.0f;
        v.pan = ((noteCounter % 2) == 0 ? 1.0f : -1.0f) * (0.08f + 0.22f * (float) ((noteCounter / 2) % 3) / 2.0f);
        v.tideOffset = (float) (noteCounter % 7) / 7.0f;
        if (wasSilent)
        {
            v.sw = initialSwell (v);
            v.body.reset();
            v.toneL.reset(); v.toneR.reset(); v.airBp.reset();
        }

        const float startA = aPosition.get() / 100.0f, startB = bPosition.get() / 100.0f;
        for (int k = 0; k < 2; ++k)
        {
            v.src[(size_t) k] = slots[(size_t) k].live.load();
            const auto len = v.src[(size_t) k] != nullptr ? v.src[(size_t) k]->lengthSeconds() : 1.0;
            v.cloud[(size_t) k].start ((k == 0 ? startA : startB) * len * 0.85, (int) noteCounter * 31 + k);
        }
        v.amp.gate (true);
    }

    void noteOff (int note)
    {
        for (auto& v : voice)
        {
            if (v.note != note || ! v.held) continue;
            v.held = false;
            if (pedal) v.sustained = true;
            else v.amp.gate (false);
        }
    }

    float swellTarget (const Voice& v) const
    {
        const float velCurve = 0.25f + 0.75f * std::pow (v.vel, 0.8f);
        float s = ctrlActive ? juce::jmap (0.8f, velCurve, ctrl) : velCurve;
        s = juce::jmax (s, v.press);
        return juce::jlimit (0.0f, 1.0f, s * swell.get() / 100.0f);
    }

    float initialSwell (const Voice& v) const   { return swellTarget (v); }

    static float mtof (float note)      { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }

    void render (juce::AudioBuffer<float>& buf, int start, int num)
    {
        for (int done = 0; done < num; )
        {
            const int n = juce::jmin (chunk, num - done);
            renderChunk (buf, start + done, n);
            done += n;
        }
    }

    void renderChunk (juce::AudioBuffer<float>& buf, int start, int n)
    {
        auto* L = buf.getWritePointer (0, start);
        auto* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1, start) : nullptr;
        std::fill (ml.begin(), ml.begin() + n, 0.0f);
        std::fill (mr.begin(), mr.begin() + n, 0.0f);

        const float sr = (float) rate;
        const float lifeAmt = life.get() / 100.0f;
        const auto setA = OrganicDsp::CloudSettings::forTexture (aTexture.get() / 100.0f, lifeAmt);
        const auto setB = OrganicDsp::CloudSettings::forTexture (bTexture.get() / 100.0f, lifeAmt);
        const float levelA = std::pow (aLevel.get() / 100.0f, 1.5f), levelB = std::pow (bLevel.get() / 100.0f, 1.5f);
        const float res = resonance.get() / 100.0f, airAmt = air.get() / 100.0f;
        const float dyn = dynamics.get() / 100.0f;
        const float bloomTime = bloom.get();
        const float bendSemis = bend * 2.0f;
        const float wid = width.get() / 100.0f;
        const int type = juce::roundToInt (bodyType.get());
        const float decay = bodyDecay.get();
        const float att = attack.get(), rel = release.get();
        const float cutoff = bright.get();
        const float swCoeff = 1.0f - std::exp (-(float) n / (0.03f * sr));

        // Tide: the morph sways slowly (each voice at its own point of the swell)
        tidePhase += tide.get() * (float) n / sr;
        tidePhase -= std::floor (tidePhase);
        morphSmooth += (morph.get() / 100.0f - morphSmooth) * swCoeff;
        morphNow = morphSmooth;

        const auto* liveA = slots[0].live.load();
        const auto* liveB = slots[1].live.load();
        float maxSwell = 0.0f;

        for (size_t vi = 0; vi < voice.size(); ++vi)
        {
            auto& v = voice[vi];
            auto& view = voiceView[vi];
            if (! v.active())
            {
                view.active = false;
                continue;
            }

            // A source changed under the voice: fade it out quickly
            if (v.src[0] != liveA || v.src[1] != liveB)
                v.killing = true;

            // Expression
            v.bloomT += (float) n / sr;
            const float bloomShape = bloomTime <= 0.001f ? 1.0f : 1.0f - 0.65f * std::exp (-v.bloomT / (bloomTime * 0.4f));
            v.sw += (swellTarget (v) * bloomShape - v.sw) * swCoeff;
            const float s = v.sw;
            maxSwell = juce::jmax (maxSwell, s);
            const float levelDb = (s - 1.0f) * 30.0f * dyn;
            const float expGain = juce::Decibels::decibelsToGain (levelDb);
            const float brightOct = (s - 0.75f) * 3.0f * dyn;
            const float layerMix = juce::jlimit (0.0f, 1.0f, (0.75f + (s - 0.75f) * dyn - 0.2f) / 0.7f);

            const float driftSemis = v.drift.process (0.12f * lifeAmt, sr / (float) n) ;
            const float pitch = (float) v.note + bendSemis + driftSemis;
            const float flutter = 1.0f + v.flutter.process (0.12f * lifeAmt, sr / (float) n);

            const float m = juce::jlimit (0.0f, 1.0f, morphSmooth + tideDepth.get() / 100.0f * 0.5f
                                                       * std::sin (juce::MathConstants<float>::twoPi * (tidePhase + v.tideOffset)));
            const float gA = std::cos (m * juce::MathConstants<float>::halfPi) * levelA;
            const float gB = std::sin (m * juce::MathConstants<float>::halfPi) * levelB;

            std::fill (tl.begin(), tl.begin() + n, 0.0f); std::fill (tr.begin(), tr.begin() + n, 0.0f);
            std::fill (bl.begin(), bl.begin() + n, 0.0f); std::fill (br.begin(), br.begin() + n, 0.0f);

            const float tunes[2] = { aTune.get(), bTune.get() };
            const OrganicDsp::CloudSettings* sets[2] = { &setA, &setB };
            float* outs[2][2] = { { tl.data(), tr.data() }, { bl.data(), br.data() } };
            const float gains[2] = { gA, gB };

            for (int k = 0; k < 2; ++k)
            {
                auto* src = v.src[(size_t) k];
                if (src == nullptr || gains[k] <= 0.0005f)
                    continue;
                const float p = pitch + tunes[k];
                const int note = juce::roundToInt (p);
                const auto* soft = src->pick (note, 0, v.take);
                const auto* loud = src->pick (note, src->layers - 1, v.take);
                v.cloud[(size_t) k].render (outs[k][0], outs[k][1], n, *src, soft, loud, layerMix, p, rate, *sets[k], src->gain * gains[k]);
                view.x[k] = v.cloud[(size_t) k].lastX;
            }

            // Body and air
            const float f0 = mtof (pitch);
            v.body.set (f0, type, decay, 0.3f + 0.7f * s, rate);
            v.airBp.set (juce::jlimit (300.0f, 9000.0f, f0 * 3.0f), 0.35f, sr);
            const float airLevel = airAmt * (0.25f + 0.75f * s) * 0.5f;
            const float toneCut = juce::jlimit (60.0f, sr * 0.45f, cutoff * std::pow (2.0f, brightOct + ((float) v.note - 60.0f) / 12.0f * 0.4f));
            v.toneL.set (toneCut, 0.1f, sr);
            v.toneR.set (toneCut, 0.1f, sr);

            float envLast = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                float l = tl[(size_t) i] + bl[(size_t) i];
                float r = tr[(size_t) i] + br[(size_t) i];

                const float breath = noise.pink() * airLevel;
                float bl2, bb2, bh2;
                v.airBp.process (breath, bl2, bb2, bh2);
                float rl = 0.0f, rr = 0.0f;
                v.body.process ((l + r) * 0.5f * res, breath, rl, rr);
                const float dryAmt = 1.0f - res * 0.45f;
                const float wet = 0.6f * res + 0.4f * airAmt;
                l = l * dryAmt + rl * wet + bb2 * 0.12f;
                r = r * dryAmt + rr * wet + bb2 * 0.12f;

                l = v.toneL.lowpass (l);
                r = v.toneR.lowpass (r);

                const float env = v.amp.process (att, 0.1f, 1.0f, rel, sr);
                if (v.killing) v.kill = juce::jmax (0.0f, v.kill - 1.0f / (0.03f * sr));
                const float gain = env * expGain * flutter * v.kill;
                envLast = env;

                const float pan = v.pan * wid;
                ml[(size_t) i] += l * gain * (1.0f - juce::jmax (0.0f, pan));
                mr[(size_t) i] += r * gain * (1.0f + juce::jmin (0.0f, pan));
            }

            if (v.killing && v.kill <= 0.0f)
                v.amp.reset();

            view.amp = envLast * expGain;
            view.active = v.active();
        }

        liveSwell = maxSwell;

        // Warmth: gentle saturation
        const float w = warmth.get() / 100.0f;
        const float drive = 1.0f + 3.0f * w;
        const float comp = 1.0f / std::tanh (drive * 0.5f) * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            if (w > 0.001f)
            {
                ml[(size_t) i] = juce::jmap (w, ml[(size_t) i], std::tanh (ml[(size_t) i] * drive * 0.5f) * comp);
                mr[(size_t) i] = juce::jmap (w, mr[(size_t) i], std::tanh (mr[(size_t) i] * drive * 0.5f) * comp);
            }
        }

        space.process (ml.data(), mr.data(), n, spaceSize.get() / 100.0f, spaceMix.get() / 100.0f, 0.4f + 0.6f * wid);

        const float out = juce::Decibels::decibelsToGain (volume.get(), -60.0f);
        for (int i = 0; i < n; ++i)
        {
            // Never clip hard: a soft ceiling just under 0 dBFS
            const float l = ml[(size_t) i] * out, r = mr[(size_t) i] * out;
            L[i] += std::abs (l) < 0.8f ? l : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (l) - 0.8f) / 0.18f), l);
            if (R != nullptr)
                R[i] += std::abs (r) < 0.8f ? r : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (r) - 0.8f) / 0.18f), r);
        }
    }
};

} // namespace elsewhere

#include "OrganicView.h"
