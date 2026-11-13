// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "MainComponent.h"
#include "Mixing.h"
#include "Comments.h"
#include "devices/DeviceBase.h"
#include "devices/PolyDevice.h"
#include "devices/MonoDevice.h"

namespace elsewhere
{
//==============================================================================
/** A demo project for the README screenshots (--showcase <dir>): "Night Drive", a short song with
    a vocal and two guitars recorded as audio, keys and bass on Elsewhere's own instruments, a drum
    track in sections, a session grid, and comments and presence from two collaborators. The audio
    is synthesised here, so the screenshots can be made again on any machine. */
namespace Showcase
{
    inline constexpr double bpm = 92.0;
    inline const juce::String projectName = "Night Drive";

    /** A small, repeatable random source, so every run draws the same song. */
    struct Random
    {
        juce::uint32 state = 0x2545f491;
        float next()        { state = state * 1664525u + 1013904223u; return (float) (state >> 8) / (float) (1 << 24); }
        float range (float lo, float hi) { return lo + (hi - lo) * next(); }
    };

    inline juce::File writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double rate)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (rate)
                                                                                         .withNumChannels (buffer.getNumChannels()).withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        return file;
    }

    /** A sung line: syllables of a few harmonics with vibrato, phrased in two-bar lines with breaths. */
    inline juce::File makeVocal (const juce::File& file, int bars, double rate, juce::uint32 seed)
    {
        const double beat = 60.0 / bpm;
        juce::AudioBuffer<float> b (2, (int) (bars * 4 * beat * rate));
        b.clear();
        Random rnd { seed };
        const float scale[] = { 164.8f, 185.0f, 196.0f, 220.0f, 246.9f, 261.6f, 293.7f };

        for (int line = 0; line < bars / 2; ++line)
        {
            double t = line * 8 * beat + rnd.range (0.0f, 0.3f);
            const double end = (line + 1) * 8 * beat - beat * rnd.range (0.8f, 1.6f);   // a breath before the next line
            const float lineLevel = rnd.range (0.45f, 0.85f);

            while (t < end)
            {
                const double len = beat * (rnd.next() < 0.3f ? 1.5 : rnd.range (0.35f, 0.9f));
                const float f0 = scale[(int) rnd.range (0.0f, 6.99f)], level = lineLevel * rnd.range (0.55f, 1.0f);
                const int start = (int) (t * rate), n = (int) (juce::jmin (len, end - t) * rate);

                for (int i = 0; i < n && start + i < b.getNumSamples(); ++i)
                {
                    const double s = i / rate;
                    const float env = (float) juce::jmin (1.0, s / 0.04) * (float) juce::jmin (1.0, (n - i) / (0.09 * rate));
                    const double ph = juce::MathConstants<double>::twoPi * f0 * s + 0.6 * std::sin (juce::MathConstants<double>::twoPi * 5.2 * s) * (s > 0.15 ? 1.0 : 0.0);
                    const float v = level * env * (float) (0.6 * std::sin (ph) + 0.25 * std::sin (2 * ph) + 0.12 * std::sin (3 * ph) + 0.05 * std::sin (5 * ph));
                    b.addSample (0, start + i, v);
                    b.addSample (1, start + i, v);
                }

                t += len + beat * (rnd.next() < 0.25f ? rnd.range (0.2f, 0.5f) : 0.05f);
            }
        }

        return writeWav (file, b, rate);
    }

    /** Strummed chords on the beat (and a lighter strum on the off-beat), each ringing out. */
    inline juce::File makeRhythmGuitar (const juce::File& file, int bars, double rate)
    {
        const double beat = 60.0 / bpm;
        juce::AudioBuffer<float> b (2, (int) (bars * 4 * beat * rate));
        b.clear();
        Random rnd { 77 };
        const float chords[4][3] = { { 82.4f, 123.5f, 164.8f }, { 130.8f, 196.0f, 261.6f }, { 98.0f, 146.8f, 196.0f }, { 146.8f, 220.0f, 293.7f } };

        for (int step = 0; step < bars * 8; ++step)
        {
            const bool onBeat = step % 2 == 0;
            const auto& chord = chords[(step / 8) % 4];
            const float level = (onBeat ? 0.55f : 0.28f) * rnd.range (0.8f, 1.0f) * (step / 8 % 4 == 3 ? 1.15f : 1.0f);
            const int start = (int) (step * beat * 0.5 * rate), n = (int) (beat * 0.5 * rate);

            for (int i = 0; i < n && start + i < b.getNumSamples(); ++i)
            {
                const double s = i / rate;
                const float env = std::exp (-(float) s * 7.0f) * (float) juce::jmin (1.0, s / 0.004);
                float v = 0;
                for (int k = 0; k < 3; ++k)
                    v += (float) std::sin (juce::MathConstants<double>::twoPi * chord[k] * 2 * (s + k * 0.006)) / 3.0f;
                v += 0.15f * rnd.range (-1.0f, 1.0f) * std::exp (-(float) s * 60.0f);
                b.addSample (0, start + i, level * env * v);
                b.addSample (1, start + i, level * env * v * 0.9f);
            }
        }

        return writeWav (file, b, rate);
    }

    /** A lead line: long bent notes that swell and fade. */
    inline juce::File makeLeadGuitar (const juce::File& file, int bars, double rate)
    {
        const double beat = 60.0 / bpm;
        juce::AudioBuffer<float> b (2, (int) (bars * 4 * beat * rate));
        b.clear();
        Random rnd { 9001 };
        const float notes[] = { 329.6f, 392.0f, 440.0f, 493.9f, 587.3f };
        double t = beat * 0.5;

        while (t < bars * 4 * beat - beat)
        {
            const double len = beat * (rnd.next() < 0.5f ? 2.0 : rnd.range (0.5f, 1.5f));
            const float f0 = notes[(int) rnd.range (0.0f, 4.99f)], level = rnd.range (0.35f, 0.7f);
            const int start = (int) (t * rate), n = (int) (len * rate);

            for (int i = 0; i < n && start + i < b.getNumSamples(); ++i)
            {
                const double s = i / rate;
                const float env = (float) juce::jmin (1.0, s / 0.01) * std::exp (-(float) s * 1.4f) * (float) juce::jmin (1.0, (n - i) / (0.05 * rate));
                const double ph = juce::MathConstants<double>::twoPi * f0 * s + 0.9 * std::sin (juce::MathConstants<double>::twoPi * 6.0 * s) * juce::jmin (1.0, s / 0.4);
                const float v = level * env * (float) (0.7 * std::sin (ph) + 0.3 * std::sin (2 * ph));
                b.addSample (0, start + i, v * 0.85f);
                b.addSample (1, start + i, v);
            }

            t += len + (rnd.next() < 0.3f ? beat : 0.0);
        }

        return writeWav (file, b, rate);
    }

    inline te::TimeRange bars (te::Edit& edit, double first, double last)   // 1-based bars, inclusive of first, up to the start of last + 1
    {
        auto& ts = edit.tempoSequence;
        return { ts.toTime (te::BeatPosition::fromBeats ((first - 1) * 4)), ts.toTime (te::BeatPosition::fromBeats (last * 4)) };
    }

    inline te::AudioTrack* newTrack (te::Edit& edit, te::Track* after, const juce::String& name, int colour)
    {
        auto t = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, after), nullptr);
        if (t == nullptr)
            return nullptr;

        t->setName (name);
        t->setColour (EngineHelpers::trackColourFor (colour));
        EngineHelpers::clearInputsFromTrack (*t);
        return t.get();
    }

    inline void addDevice (te::AudioTrack& t, const juce::String& type)
    {
        if (auto p = DeviceRegistry::create (t.edit, type))
            t.pluginList.insertPlugin (p, t.pluginList.size() > 1 ? t.pluginList.size() - 2 : 0, nullptr);
    }

    inline void setMix (te::AudioTrack& t, float db, float pan)
    {
        if (auto v = t.getVolumePlugin())
        {
            v->setVolumeDb (db);
            v->setPan (pan);
        }
    }

    /** E minor: Em C G D, a bar each. */
    inline void addChords (te::MidiClip& clip, int numBars)
    {
        const int roots[] = { 52, 48, 55, 50 };
        const int shapes[4][4] = { { 0, 7, 12, 15 }, { 0, 7, 12, 16 }, { 0, 4, 7, 14 }, { 0, 7, 9, 16 } };
        auto& seq = clip.getSequence();

        for (int bar = 0; bar < numBars; ++bar)
            for (int k = 0; k < 4; ++k)
                seq.addNote (roots[bar % 4] + shapes[bar % 4][k], te::BeatPosition::fromBeats (bar * 4.0 + (k == 3 ? 2.0 : 0.0)),
                             te::BeatDuration::fromBeats (k == 3 ? 1.75 : 3.75), 70 + (bar * 13 + k * 7) % 30, 0, nullptr);
    }

    inline void addBassline (te::MidiClip& clip, int numBars)
    {
        const int roots[] = { 28, 36, 31, 38 };
        const int pattern[] = { 0, 0, 12, 0, 7, 0, 12, 10 };
        auto& seq = clip.getSequence();

        for (int bar = 0; bar < numBars; ++bar)
            for (int e = 0; e < 8; ++e)
                if (e != 5 || bar % 2 == 1)
                    seq.addNote (roots[bar % 4] + pattern[e], te::BeatPosition::fromBeats (bar * 4.0 + e * 0.5),
                                 te::BeatDuration::fromBeats (0.4), e % 2 == 0 ? 105 : 78, 0, nullptr);
    }

    /** No inputs on any track (focusing an instrument track routes the keyboard to it), so the
        screenshots don't show this machine's devices. */
    inline void clearInputs (te::Edit& edit)
    {
        for (auto t : te::getAudioTracks (edit))
            EngineHelpers::clearInputsFromTrack (*t);
    }

    struct Tracks
    {
        te::AudioTrack* vocal = nullptr, *rhythm = nullptr, *lead = nullptr, *keys = nullptr, *bass = nullptr, *drums = nullptr;
        te::FolderTrack* guitars = nullptr;
        te::MidiClip* keysClip = nullptr, *chorusDrums = nullptr;
    };

    /** Fills the open project with the song. */
    inline Tracks build (MainComponent& mc)
    {
        auto& edit = mc.getProject().getEdit();
        auto& vs = mc.getViewState();
        Tracks tr;

        edit.tempoSequence.getTempo (0)->setBpm (bpm);

        for (auto t : te::getAudioTracks (edit))
            edit.deleteTrack (t);

        // The recorded parts, written into the project's samples like any recording
        const double rate = 44100.0;
        auto samples = mc.getProject().getFolder().getChildFile ("Samples/Recorded");
        samples.createDirectory();
        auto vocalVerse  = makeVocal (samples.getChildFile ("Vocal Verse.wav"), 8, rate, 11);
        auto vocalChorus = makeVocal (samples.getChildFile ("Vocal Chorus.wav"), 8, rate, 23);
        auto rhythmTake  = makeRhythmGuitar (samples.getChildFile ("Rhythm Gtr.wav"), 20, rate);
        auto leadTake    = makeLeadGuitar (samples.getChildFile ("Lead Gtr.wav"), 8, rate);

        auto wave = [&] (te::AudioTrack* t, const juce::String& name, const juce::File& f, double firstBar, int numBars)
        {
            if (t != nullptr)
                t->insertWaveClip (name, f, { bars (edit, firstBar, firstBar + numBars - 1), {} }, false);
        };

        tr.vocal = newTrack (edit, nullptr, "Vocal", 7);
        wave (tr.vocal, "Vocal Verse", vocalVerse, 5, 8);
        wave (tr.vocal, "Vocal Chorus", vocalChorus, 13, 8);

        tr.rhythm = newTrack (edit, tr.vocal, "Rhythm Gtr", 4);
        wave (tr.rhythm, "Rhythm Gtr", rhythmTake, 1, 20);
        tr.lead = newTrack (edit, tr.rhythm, "Lead Gtr", 4);
        wave (tr.lead, "Lead Gtr", leadTake, 13, 8);

        if (tr.rhythm != nullptr && tr.lead != nullptr)
            if ((tr.guitars = Mixing::groupTracks (edit, { tr.rhythm, tr.lead })) != nullptr)
            {
                tr.guitars->setName ("Guitars");
                tr.guitars->setColour (EngineHelpers::trackColourFor (4));
            }

        tr.keys = newTrack (edit, tr.guitars != nullptr ? (te::Track*) tr.guitars : tr.lead, "Keys", 5);
        if (tr.keys != nullptr)
        {
            addDevice (*tr.keys, "elsewherePoly");
            if (auto poly = dynamic_cast<PolyDevice*> (tr.keys->pluginList[0]))
                poly->loadPatch (0);   // Prism Pad
            addDevice (*tr.keys, "elsewhereEQ");
            addDevice (*tr.keys, "elsewhereReverb");
            if ((tr.keysClip = tr.keys->insertMIDIClip ("Keys", bars (edit, 1, 20), nullptr).get()) != nullptr)
                addChords (*tr.keysClip, 20);
        }

        tr.bass = newTrack (edit, tr.keys, "Bass", 0);
        if (tr.bass != nullptr)
        {
            addDevice (*tr.bass, "elsewhereMono");
            if (auto mono = dynamic_cast<MonoDevice*> (tr.bass->pluginList[0]))
                mono->loadPatch (0);
            if (auto c = tr.bass->insertMIDIClip ("Bass", bars (edit, 5, 20), nullptr))
                addBassline (*c, 16);
        }

        // Drums, in sections: added after the bass, where a new track goes
        if (tr.bass != nullptr)
            vs.focusTrack (tr.bass);

        if ((tr.drums = mc.addDrumTrack()) != nullptr)
        {
            tr.drums->setColour (EngineHelpers::trackColourFor (1));
            EngineHelpers::clearInputsFromTrack (*tr.drums);

            for (auto c : tr.drums->getClips())
                c->removeFromParent();

            if (auto intro = DrumTrack::createSection (*tr.drums, 0.0, 16.0, "Intro", drums::Style::halfTime))
            {
                intro->state.setProperty (DrumTrack::Props::loudness, 0.3, nullptr);
                DrumTrack::regenerate (*intro);
            }

            DrumTrack::createSection (*tr.drums, 16.0, 48.0, "Verse", drums::Style::rock);

            if ((tr.chorusDrums = DrumTrack::createSection (*tr.drums, 48.0, 80.0, "Chorus", drums::Style::rock)) != nullptr)
            {
                tr.chorusDrums->state.setProperty (DrumTrack::Props::loudness, 0.85, nullptr);
                tr.chorusDrums->state.setProperty (DrumTrack::Props::complexity, 0.7, nullptr);
                DrumTrack::regenerate (*tr.chorusDrums);
            }
        }

        // A mix that looks worked on
        if (tr.vocal)  setMix (*tr.vocal, -1.5f, 0.0f);
        if (tr.rhythm) setMix (*tr.rhythm, -6.0f, -0.35f);
        if (tr.lead)   setMix (*tr.lead, -4.5f, 0.3f);
        if (tr.keys)   setMix (*tr.keys, -8.0f, 0.15f);
        if (tr.bass)   setMix (*tr.bass, -3.0f, 0.0f);
        if (tr.drums)  setMix (*tr.drums, -2.0f, 0.0f);

        // The session grid: four scenes, with loops from the song
        auto& scenes = edit.getSceneList();
        scenes.ensureNumberOfScenes (4);
        while (scenes.getScenes().size() > 4)
            scenes.deleteScene (*scenes.getScenes().getLast());
        const char* sceneNames[] = { "Intro", "Verse", "Chorus", "Outro" };

        for (auto t : te::getAudioTracks (edit))
            t->getClipSlotList().ensureNumberOfSlots (scenes.getScenes().size());

        auto slot = [] (te::AudioTrack* t, int i) -> te::ClipSlot* { return t != nullptr ? t->getClipSlotList().getClipSlots()[i] : nullptr; };
        auto& ts = edit.tempoSequence;
        auto loop = [&] (double numBars) { return te::TimeRange (0s, ts.toTime (te::BeatPosition::fromBeats (numBars * 4))); };

        for (int i = 0; i < 4; ++i)
        {
            if (auto s = slot (tr.keys, i))
                if (auto c = te::insertMIDIClip (*s, juce::String ("Keys ") + sceneNames[i], loop (4)))
                    addChords (*c, 4);

            if (i > 0)
                if (auto s = slot (tr.bass, i))
                    if (auto c = te::insertMIDIClip (*s, juce::String ("Bass ") + sceneNames[i], loop (4)))
                        addBassline (*c, 4);

            if (auto s = slot (tr.rhythm, i))
                te::insertWaveClip (*s, "Rhythm " + juce::String (sceneNames[i]), rhythmTake,
                                    { loop (4), {} }, te::DeleteExistingClips::no);
        }

        for (int i : { 1, 2 })
            if (auto s = slot (tr.vocal, i))
                te::insertWaveClip (*s, juce::String ("Vocal ") + sceneNames[i], i == 1 ? vocalVerse : vocalChorus,
                                    { loop (4), {} }, te::DeleteExistingClips::no);

        if (auto s = slot (tr.lead, 2))
            te::insertWaveClip (*s, "Lead Chorus", leadTake, { loop (4), {} }, te::DeleteExistingClips::no);

        // Two collaborators: comments from both, and one of them in the project right now
        Collab::Identity ada { "ada00000showcase", "Ada", Collab::Identity::colourFor ("ada00000showcase") };
        Collab::Identity jun { "jun00000showcase", "Jun", Collab::Identity::colourFor ("jun00000showcase") };

        if (tr.keys)
            Comments::add (edit, ada, tr.keys->itemID, bars (edit, 5, 8), "Love this voicing. Could it open up more in the chorus?");
        if (tr.vocal)
        {
            Comments::add (edit, jun, tr.vocal->itemID, bars (edit, 9, 10), "Breath before this line is a little loud");
            vs.openCommentId = Comments::add (edit, jun, tr.vocal->itemID, bars (edit, 13, 16), "Let's double the vocal here. I'll record one tonight");
        }

        mc.showPresence ({ { jun.name, jun.colour, tr.vocal != nullptr ? tr.vocal->itemID : te::EditItemID() } }, "Shared: " + projectName);

        // The guitars as one combined waveform, so the whole song fits
        if (tr.guitars != nullptr)
            vs.collapsedGroups.insert (tr.guitars->itemID);

        edit.getTransport().setPosition (bars (edit, 13, 13).getStart());
        vs.selectionManager.deselectAll();
        vs.sendChangeMessage();
        return tr;
    }
}
}
