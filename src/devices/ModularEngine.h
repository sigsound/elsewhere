// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ModularDsp.h"
#include "Downsampler.h"
#include "EngineControl.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace elsewhere
{
//==============================================================================
/**
    The sound of Elsewhere Modular, on its own: a patch of modules run once per voice at twice
    the sample rate, brought back down, with the voice allocation, Volume and the four
    Macros. It needs nothing beyond the standard library, so it builds anywhere. The Modular
    device plays through it (building the patch from its own rack), and so can anything else
    that wants to sound the same.

    setPatch() (or build(), for any patch) compiles a patch; it allocates, so it isn't for the
    audio thread, but it can run while audio plays. Call prepare() before playing, then noteOn
    and noteOff as notes come, and render() for each block: none of those allocates or locks.
    prepare() starts every module afresh, so the same notes always render the same audio.
    Changes to params take effect in the next render().
*/
class ModularEngine
{
public:
    static constexpr int maxVoices = Modular::maxVoices;
    static constexpr int maxChunk = 256;

    /** Every control, in the device's own units: voices, dB, and each macro 0..1. */
    struct Params
    {
        float voices = 1.0f, volume = -6.0f;
        float macro1 = 0.5f, macro2 = 0.5f, macro3 = 0.5f, macro4 = 0.5f;     // 0..1, as the Macros module puts them out

        /** Each control by the ID the device saves it under. */
        float* find (const char* id) noexcept
        {
            for (auto& [name, value] : table())
                if (std::strcmp (name, id) == 0)
                    return &(this->*value);
            return nullptr;
        }

        static const std::vector<std::pair<const char*, float Params::*>>& table()
        {
            static const std::vector<std::pair<const char*, float Params::*>> all = {
                { "voices", &Params::voices }, { "volume", &Params::volume },
                { "macro1", &Params::macro1 }, { "macro2", &Params::macro2 }, { "macro3", &Params::macro3 }, { "macro4", &Params::macro4 },
            };
            return all;
        }
    };

    Params params;

    //==============================================================================
    using Control = EngineControl;

    /** Every control, in the order the device panel shows them. What a macro does depends on
        the patch: macroName() gives its name there. */
    static const std::vector<SynthControl>& controls()
    {
        static const std::vector<SynthControl> all = {
            { "voices", "Voices",  "Patch",  1, 8, 1, 1, false, "", {} },
            { "volume", "Volume",  "Patch",  -60, 6, -6, 0, false, "dB", {} },
            { "macro1", "Macro 1", "Macros", 0, 1, 0.5f, 0, false, "", {} },
            { "macro2", "Macro 2", "Macros", 0, 1, 0.5f, 0, false, "", {} },
            { "macro3", "Macro 3", "Macros", 0, 1, 0.5f, 0, false, "", {} },
            { "macro4", "Macro 4", "Macros", 0, 1, 0.5f, 0, false, "", {} },
        };
        return all;
    }

    //==============================================================================
    // Patches: the factory patches, each with its voice count and named macros
    static const std::vector<Modular::FactoryPatch>& patches()      { return Modular::factoryPatches(); }
    static int patchCount() noexcept                                { return (int) patches().size(); }
    static const char* patchName (int i) noexcept                   { return i >= 0 && i < patchCount() ? patches()[(size_t) i].name : ""; }

    /** What macro (0..3) does in patch i, such as "Cutoff"; "" if that patch leaves it unpatched. */
    static const char* macroName (int patch, int macro) noexcept
    {
        return patch >= 0 && patch < patchCount() ? Modular::macroName (patches()[(size_t) patch], macro) : "";
    }

    /** A control's value in a patch: its voice count, and the defaults for the rest (each macro
        rests at 0.5, where the patch sounds as designed). */
    static float patchValue (int i, const char* id)
    {
        if (i >= 0 && i < patchCount() && std::strcmp (id, "voices") == 0)
            return (float) patches()[(size_t) i].voices;

        Params defaults;
        auto* v = defaults.find (id);
        return v != nullptr ? *v : 0.0f;
    }

    /** Sets every control to the patch and builds it (not on the audio thread, though audio can play on). */
    void setPatch (int i)
    {
        i = dspmath::jlimit (0, patchCount() - 1, i);
        for (auto& [id, member] : Params::table())
            params.*member = patchValue (i, id);
        build (Modular::describe (patches()[(size_t) i]));
    }

    /** Builds any patch, keeping the state of modules (by ID) that were in the last one. */
    void build (const Modular::PatchDesc& desc)     { patch.build (desc); }

    /** A module's processing by ID, to set its knobs while it plays (nullptr if there's none). */
    Modular::Dsp* module (int id) const             { return patch.module (id); }

    /** Frees what earlier builds replaced, once render() can't be using it (not on the audio thread). */
    void releaseRetired()                           { patch.release(); }

    //==============================================================================
    void prepare (double sampleRate, int /*maxBlock*/ = 0)
    {
        rate = (float) sampleRate;
        patch.reset (rate * 2.0f);
        downL.prepare (1);
        downR.prepare (1);
        for (auto& v : voiceState) v = {};
        pendingGate.fill (false);
        numHeld = 0;
        noteCounter = 0;
        bend = modWheel = 0.0f;
        prepared = true;
    }

    bool isPrepared() const noexcept                { return prepared; }

    /** The delay the oversampling adds, in samples at the output rate. */
    double latencySamples() const noexcept          { return downL.latencySamples(); }

    //==============================================================================
    void noteOn (int note, float vel)
    {
        const int n = numVoices();
        if (n == 1)
        {
            // Mono: legato, last note priority (the gate stays up while keys are held)
            release (note);
            if (numHeld < (int) held.size()) held[(size_t) numHeld++] = note;
            auto& v = voiceState[0];
            v.note = note; v.pitchV = (float) (note - 60) / 12.0f; v.gate = 1.0f; v.held = true;
            if (numHeld == 1) v.vel = vel;
            return;
        }

        VoiceState* target = nullptr;
        for (int i = 0; i < n && target == nullptr; ++i)
            if (voiceState[(size_t) i].note == note) target = &voiceState[(size_t) i];
        for (int i = 0; i < n && target == nullptr; ++i)
            if (! voiceState[(size_t) i].held) target = &voiceState[(size_t) i];
        if (target == nullptr)
        {
            std::uint32_t oldest = std::numeric_limits<std::uint32_t>::max();
            for (int i = 0; i < n; ++i)
                if (voiceState[(size_t) i].age < oldest) { oldest = voiceState[(size_t) i].age; target = &voiceState[(size_t) i]; }
        }

        target->note = note; target->pitchV = (float) (note - 60) / 12.0f; target->vel = vel; target->held = true; target->age = ++noteCounter;
        target->gate = 0.0f;   // a one-sample dip so envelopes retrigger on a stolen voice
        pendingGate[(size_t) (target - voiceState.data())] = true;
    }

    void noteOff (int note)
    {
        if (numVoices() == 1)
        {
            release (note);
            auto& v = voiceState[0];
            if (numHeld == 0) { v.gate = 0.0f; v.held = false; }
            else { v.note = held[(size_t) numHeld - 1]; v.pitchV = (float) (v.note - 60) / 12.0f; }
            return;
        }

        for (auto& v : voiceState)
            if (v.note == note && v.held) { v.held = false; v.gate = 0.0f; }
    }

    /** Every note let go (they ring out through their release). */
    void allNotesOff()
    {
        numHeld = 0;
        for (auto& v : voiceState) { v.gate = 0.0f; v.held = false; }
    }

    void setPitchBend (float minusOneToOne)     { bend = minusOneToOne * 2.0f / 12.0f; }    // +-2 semitones
    void setModWheel (float zeroToOne)          { modWheel = zeroToOne; }                  // MIDI In's Mod jack

    //==============================================================================
    /** Renders n samples, adding them to L and R. */
    void render (float* L, float* R, int num)
    {
        const auto* prog = patch.begin();
        if (prog != nullptr && prepared)
            for (int done = 0; done < num; )
            {
                const int n = std::min (maxChunk, num - done);
                renderChunk (*prog, L + done, R + done, n);
                done += n;
            }
        patch.end();
    }

    /** Listens to one output jack (voice 1) for a scope; module -1 to stop. */
    void setProbe (int moduleId, int port)      { probePort = port; probeModule = moduleId; }

    // What's going on, for drawing
    SynthCore::ScopeBuffer scope, probeScope;
    std::atomic<float> samplesPerCycle { 200.0f };
    std::atomic<int> activeVoices { 0 };

private:
    struct VoiceState { int note = -1; float pitchV = 0.0f, gate = 0.0f, vel = 0.0f; bool held = false; std::uint32_t age = 0; };

    static constexpr float cableLimit = 64.0f;

    float rate = 48000.0f;
    bool prepared = false;
    Modular::Patch patch;
    Downsampler downL, downR;
    std::array<float, maxChunk * 2> osL {}, osR {};
    std::array<VoiceState, maxVoices> voiceState;
    std::array<bool, maxVoices> pendingGate {};
    std::array<int, 128> held {};
    int numHeld = 0;
    std::uint32_t noteCounter = 0;
    float bend = 0.0f, modWheel = 0.0f;
    std::atomic<int> probeModule { -1 }, probePort { -1 };

    int numVoices() const noexcept      { return dspmath::jlimit (1, maxVoices, Modular::roundToInt (params.voices)); }

    void release (int note) noexcept
    {
        int k = 0;
        for (int i = 0; i < numHeld; ++i)
            if (held[(size_t) i] != note) held[(size_t) k++] = held[(size_t) i];
        numHeld = k;
    }

    void renderChunk (const Modular::Program& prog, float* L, float* R, int num)
    {
        const int n = numVoices();
        std::array<float, maxVoices> pitch {}, gate {}, vel {};
        const std::array<float, 4> macroValues { params.macro1, params.macro2, params.macro3, params.macro4 };

        Modular::Context ctx;
        ctx.sr = rate * 2.0f;
        ctx.pitch = pitch.data(); ctx.gate = gate.data(); ctx.vel = vel.data();
        ctx.mod = modWheel;
        ctx.macros = macroValues.data();

        // The scope's jack, if any
        int probeNode = -1;
        const int pm = probeModule.load(), pp = probePort.load();
        if (pm >= 0)
            for (size_t i = 0; i < prog.nodes.size(); ++i)
                if (prog.nodes[i].moduleId == pm) probeNode = (int) i;

        const int osN = num * 2;
        float in[Modular::maxPorts];

        for (int s = 0; s < osN; ++s)
        {
            for (int v = 0; v < n; ++v)
            {
                auto& vs = voiceState[(size_t) v];
                pitch[(size_t) v] = vs.pitchV + bend;
                gate[(size_t) v] = vs.gate;
                vel[(size_t) v] = vs.vel;
                if (pendingGate[(size_t) v]) { pendingGate[(size_t) v] = false; vs.gate = 1.0f; }
            }

            ctx.accL = ctx.accR = 0.0f;
            for (int v = 0; v < n; ++v)
                for (auto& node : prog.nodes)
                {
                    for (int p = 0; p < node.numIns; ++p)
                    {
                        auto x = node.srcNode[(size_t) p] >= 0 ? prog.nodes[(size_t) node.srcNode[(size_t) p]].dsp->out[(size_t) v][(size_t) node.srcPort[(size_t) p]] : 0.0f;

                        // Cables can be patched in a loop with gain in it. What a cable carries is
                        // held to a range no patch needs to leave, so a runaway loop can't reach
                        // infinity and leave filters and delays (here and after this) stuck
                        in[p] = std::abs (x) <= cableLimit ? x : (x > 0.0f ? cableLimit : (x < 0.0f ? -cableLimit : 0.0f));
                    }
                    node.dsp->process (v, in, node.patched.data(), ctx);
                }

            osL[(size_t) s] = ctx.accL;
            osR[(size_t) s] = ctx.accR;
            if (probeNode >= 0 && (s & 1) == 0 && pp >= 0 && pp < Modular::maxPorts)
                probeScope.push (prog.nodes[(size_t) probeNode].dsp->out[0][(size_t) pp]);
        }

        downL.process (osL.data(), num);
        downR.process (osR.data(), num);

        const float outGain = params.volume > -60.0f ? std::pow (10.0f, params.volume * 0.05f) : 0.0f;
        for (int s = 0; s < num; ++s)
        {
            float l = std::tanh (osL[(size_t) s] * outGain);
            float r = std::tanh (osR[(size_t) s] * outGain);
            if (! std::isfinite (l)) l = 0.0f;
            if (! std::isfinite (r)) r = 0.0f;
            L[s] += l;
            R[s] += r;
            scope.push ((l + r) * 0.5f);
        }

        int active = 0;
        for (int v = 0; v < n; ++v)
            if (voiceState[(size_t) v].held) ++active;
        activeVoices = active;
        samplesPerCycle = rate / Modular::voctToHz (voiceState[0].pitchV);
    }
};

} // namespace elsewhere
