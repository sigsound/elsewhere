// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DawProjectTests.h"
#include "DawProject.h"
#include "Mixing.h"
#include "Project.h"
#include "SessionImport.h"

namespace elsewhere::DawProjectTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
        void info (const juce::String& what)            { text << "        " << what << "\n"; }

        void near (double actual, double expected, const juce::String& what, double tolerance = 1.0e-3)
        {
            check (std::abs (actual - expected) <= tolerance, what + " = " + juce::String (actual, 4) + " (expected " + juce::String (expected, 4) + ")");
        }
    };

    void writeSine (const juce::File& f, double seconds, double hz)
    {
        const double rate = 44100.0;
        juce::AudioBuffer<float> buffer (2, (int) (seconds * rate));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate));

        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream());
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    // Laid out the way Bitwig writes it: audio in Clip > Clips > Clip > Warps > Audio
    const char* projectXml = R"(<?xml version="1.0" encoding="UTF-8"?>
<Project version="1.0">
  <Application name="Bitwig Studio" version="5.2"/>
  <Transport>
    <Tempo max="666.0" min="20.0" unit="bpm" value="120.0" id="id1" name="Tempo"/>
    <TimeSignature denominator="4" numerator="4" id="id2"/>
  </Transport>
  <Structure>
    <Track contentType="audio" loaded="true" id="t1" name="Drums" color="#ff0000">
      <Channel audioChannels="2" destination="cm" role="regular" solo="false" id="c1">
        <Mute value="false" id="m1" name="Mute"/>
        <Pan max="1.0" min="0.0" unit="normalized" value="0.25" id="p1" name="Pan"/>
        <Volume max="2.0" min="0.0" unit="linear" value="0.5" id="v1" name="Volume"/>
      </Channel>
    </Track>
    <Track contentType="tracks" loaded="true" id="g1" name="Band" color="#00ff00">
      <Channel audioChannels="2" destination="cm" role="submix" solo="false" id="cg">
        <Volume max="2.0" min="0.0" unit="linear" value="1.0" id="vg" name="Volume"/>
      </Channel>
      <Track contentType="audio" loaded="true" id="t2" name="Bass" color="#0000ff">
        <Channel audioChannels="2" destination="cg" role="regular" solo="false" id="c2">
          <Mute value="true" id="m2" name="Mute"/>
          <Volume max="2.0" min="0.0" unit="decibel" value="-3.0" id="v2" name="Volume"/>
        </Channel>
      </Track>
    </Track>
    <Track contentType="notes" loaded="true" id="t3" name="Keys">
      <Channel audioChannels="2" destination="cm" role="regular" solo="false" id="c3"/>
    </Track>
    <Track contentType="audio" loaded="true" id="t4" name="Reverb">
      <Channel audioChannels="2" destination="cm" role="effect" solo="false" id="c4"/>
    </Track>
    <Track contentType="audio notes" loaded="true" id="t5" name="Master">
      <Channel audioChannels="2" role="master" solo="false" id="cm">
        <Volume max="2.0" min="0.0" unit="linear" value="0.8" id="vm" name="Volume"/>
      </Channel>
    </Track>
  </Structure>
  <Arrangement id="a1">
    <TempoAutomation unit="bpm" id="ta"><Target parameter="id1"/>
      <RealPoint time="0.0" value="120.0" interpolation="hold"/>
      <RealPoint time="16.0" value="90.0" interpolation="hold"/>
    </TempoAutomation>
    <Lanes timeUnit="beats" id="l0">
      <Lanes track="t1" id="l1">
        <Clips id="cl1">
          <Clip time="4.0" duration="8.0" playStart="0.0" fadeTimeUnit="seconds" fadeInTime="0.1" fadeOutTime="0.0" name="Beat">
            <Clips id="cl1a">
              <Clip time="0.0" duration="8.0" contentTimeUnit="beats" playStart="0.0">
                <Warps contentTimeUnit="seconds" timeUnit="beats" id="w1">
                  <Audio algorithm="stretch" channels="2" duration="4.0" sampleRate="44100" id="au1"><File path="audio/drums.wav"/></Audio>
                  <Warp time="0.0" contentTime="0.0"/>
                  <Warp time="8.0" contentTime="4.0"/>
                </Warps>
              </Clip>
            </Clips>
          </Clip>
          <Clip time="14.0" duration="4.0" contentTimeUnit="seconds" playStart="0.5" name="Across tempo change">
            <Audio channels="2" duration="4.0" sampleRate="44100" id="au2"><File path="audio/drums.wav"/></Audio>
          </Clip>
          <Clip time="40.0" duration="6.0" contentTimeUnit="beats" playStart="1.0" loopStart="0.0" loopEnd="2.0" name="Loop">
            <Warps contentTimeUnit="seconds" timeUnit="beats" id="w3">
              <Audio channels="2" duration="4.0" sampleRate="44100" id="au3"><File path="audio/drums.wav"/></Audio>
              <Warp time="0.0" contentTime="0.0"/>
              <Warp time="2.0" contentTime="1.3333333333"/>
            </Warps>
          </Clip>
          <Clip time="60.0" duration="4.0" name="Missing">
            <Audio channels="2" duration="4.0" sampleRate="44100" id="au4"><File path="audio/missing.wav"/></Audio>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="t2" id="l2">
        <Clips id="cl2">
          <Clip time="0.0" duration="4.0" contentTimeUnit="beats" playStart="0.0" name="Double speed">
            <Warps contentTimeUnit="seconds" timeUnit="beats" id="w4">
              <Audio channels="2" duration="4.0" sampleRate="44100" id="au5"><File path="audio/bass.wav"/></Audio>
              <Warp time="0.0" contentTime="0.0"/>
              <Warp time="4.0" contentTime="4.0"/>
            </Warps>
          </Clip>
          <Clip time="32.0" duration="8.0" contentTimeUnit="beats" playStart="0.0" name="Bent">
            <Warps contentTimeUnit="seconds" timeUnit="beats" id="w5">
              <Audio channels="2" duration="4.0" sampleRate="44100" id="au6"><File path="audio/bass.wav"/></Audio>
              <Warp time="0.0" contentTime="0.0"/>
              <Warp time="4.0" contentTime="1.0"/>
              <Warp time="8.0" contentTime="3.0"/>
            </Warps>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="t3" id="l3">
        <Clips id="cl3">
          <Clip time="0.0" duration="4.0" contentTimeUnit="beats" playStart="0.0" name="Chords">
            <Notes id="n1">
              <Note time="0.0" duration="1.0" channel="0" key="60" vel="0.5"/>
              <Note time="2.0" duration="1.0" channel="0" key="64" vel="1.0"/>
              <Note time="5.0" duration="1.0" channel="0" key="67" vel="1.0"/>
            </Notes>
          </Clip>
        </Clips>
      </Lanes>
    </Lanes>
  </Arrangement>
</Project>)";

    juce::Array<te::WaveAudioClip*> waveClips (te::AudioTrack& t)
    {
        juce::Array<te::WaveAudioClip*> clips;
        for (auto* c : t.getClips())
            if (auto* w = dynamic_cast<te::WaveAudioClip*> (c))
                clips.add (w);
        return clips;
    }

    te::AudioTrack* findTrack (te::Edit& edit, const juce::String& name)
    {
        for (auto t : te::getAudioTracks (edit))
            if (t->getName() == name)
                return t;
        return nullptr;
    }

    void checkClip (Log& log, te::WaveAudioClip* c, double start, double end, double offsetInSource, double speed, const juce::String& what)
    {
        log.check (c != nullptr, what + " exists");

        if (c == nullptr)
            return;

        auto pos = c->getPosition();
        log.near (pos.getStart().inSeconds(), start, what + " start");
        log.near (pos.getEnd().inSeconds(), end, what + " end");
        log.near (pos.getOffset().inSeconds() * c->getSpeedRatio(), offsetInSource, what + " source offset");
        log.near (c->getSpeedRatio(), speed, what + " speed");
        log.check (! c->getAutoTempo(), what + " isn't tempo-following");
    }

    /** What we expect from the fixture (and from its round trip). */
    void checkImported (Log& log, te::Edit& edit, bool roundTrip)
    {
        auto& ts = edit.tempoSequence;
        log.check (ts.getNumTempos() == 2, "two tempos (" + juce::String (ts.getNumTempos()) + ")");
        log.near (ts.getTempo (0)->getBpm(), 120.0, "first tempo");

        if (ts.getNumTempos() > 1)
        {
            log.near (ts.getTempo (1)->getBpm(), 90.0, "second tempo");
            log.near (ts.getTempo (1)->getStartBeat().inBeats(), 16.0, "second tempo beat");
        }

        log.check (std::abs (std::abs (ts.getTempo (0)->getCurve()) - 1.0f) < 1.0e-4f, "tempo change is a jump, not a ramp");

        auto* drums = findTrack (edit, "Drums");
        auto* bass = findTrack (edit, "Bass");
        auto* keys = findTrack (edit, "Keys");
        log.check (drums && bass && keys, "Drums, Bass and Keys tracks");
        log.check (findTrack (edit, "Reverb") == nullptr, "return track left out");
        log.check (findTrack (edit, "Master") == nullptr, "master isn't a track");

        if (! (drums && bass && keys))
            return;

        log.near (drums->getVolumePlugin()->getVolumeDb(), -6.0206, "Drums volume dB", 0.01);
        log.near (drums->getVolumePlugin()->getPan(), -0.5, "Drums pan");
        log.check (drums->getColour().getRed() == 255 && drums->getColour().getGreen() == 0, "Drums colour");
        log.near (bass->getVolumePlugin()->getVolumeDb(), -3.0, "Bass volume dB", 0.01);
        log.check (bass->isMuted (false), "Bass muted");
        log.check (dynamic_cast<te::FolderTrack*> (bass->getParentTrack()) != nullptr
                    && bass->getParentTrack()->getName() == "Band", "Bass is inside the Band group");
        log.near (edit.getMasterVolumePlugin()->getVolumeDb(), juce::Decibels::gainToDecibels (0.8f), "master volume dB", 0.01);

        auto d = waveClips (*drums);
        log.check (d.size() == 6, "Drums has 6 clips (1 + 1 + 4 loop repeats): " + juce::String (d.size()));

        // 120 bpm until beat 16 (8 s), then 90 bpm (2/3 s per beat)
        checkClip (log, d[0], 2.0, 6.0, 0.0, 1.0, "nested warped clip");
        if (d[0] != nullptr)
            log.near (d[0]->getFadeIn().inSeconds(), 0.1, "nested clip fade in");

        checkClip (log, d[1], 7.0, 7.0 + 1.0 + 2.0 * 2.0 / 3.0, 0.5, 1.0, "unwarped clip across the tempo change");

        // Loop: content beats 1..2, then 0..2, 0..2, 0..1; 2 beats = 4/3 s of source
        auto at = 8.0 + 24.0 * 2.0 / 3.0;
        auto beat = 2.0 / 3.0;
        checkClip (log, d[2], at, at + beat, 2.0 / 3.0, 1.0, "loop pass 1");
        checkClip (log, d[3], at + beat, at + 3 * beat, 0.0, 1.0, "loop pass 2");
        checkClip (log, d[4], at + 3 * beat, at + 5 * beat, 0.0, 1.0, "loop pass 3");
        checkClip (log, d[5], at + 5 * beat, at + 6 * beat, 0.0, 1.0, "loop pass 4");

        auto b = waveClips (*bass);
        log.check (b.size() == 2, "Bass has 2 clips (the bent one is one clip with warp markers): " + juce::String (b.size()));
        checkClip (log, b[0], 0.0, 2.0, 0.0, 2.0, "double-speed clip");

        // Bent: source 0..1 s over the first 4 beats, 1..3 s over the next 4
        auto bent = 8.0 + 16.0 * 2.0 / 3.0;

        if (auto* w = b[1])
        {
            auto pos = w->getPosition();
            log.near (pos.getStart().inSeconds(), bent, "bent clip start");
            log.near (pos.getEnd().inSeconds(), bent + 8 * beat, "bent clip end");
            log.check (w->getWarpTime(), "bent clip uses warp markers");

            auto sourceAt = [w, pos] (double t)
            {
                return w->getWarpTimeManager().warpTimeToSourceTime (te::TimePosition::fromSeconds (pos.getOffset().inSeconds() + t - pos.getStart().inSeconds())).inSeconds();
            };

            log.near (sourceAt (bent), 0.0, "bent clip: file at its start");
            log.near (sourceAt (bent + 2 * beat), 0.5, "bent clip: file half way through the first stretch");
            log.near (sourceAt (bent + 4 * beat), 1.0, "bent clip: file at the bend");
            log.near (sourceAt (bent + 6 * beat), 2.0, "bent clip: file half way through the second stretch");
            log.near (sourceAt (bent + 8 * beat), 3.0, "bent clip: file at its end");
        }

        if (b[0] != nullptr)
            log.check (b[0]->getTimeStretchMode() != te::TimeStretcher::disabled, "stretched clip keeps its pitch");

        te::MidiClip* chords = nullptr;
        for (auto* c : keys->getClips())
            chords = chords != nullptr ? chords : dynamic_cast<te::MidiClip*> (c);

        log.check (chords != nullptr, "Keys has a MIDI clip");

        if (chords != nullptr)
        {
            log.near (chords->getPosition().getStart().inSeconds(), 0.0, "MIDI clip start");
            log.near (chords->getPosition().getEnd().inSeconds(), 2.0, "MIDI clip end");

            auto& notes = chords->getSequence().getNotes();
            log.check (notes.size() == 2, "2 notes inside the clip (one past its end dropped): " + juce::String (notes.size()));

            if (notes.size() == 2)
            {
                log.check (notes[0]->getNoteNumber() == 60 && notes[1]->getNoteNumber() == 64, "note pitches");
                log.near (notes[1]->getStartBeat().inBeats(), 2.0, "second note beat");
                log.check (notes[0]->getVelocity() == 64, "velocity 0.5 -> 64 (" + juce::String (notes[0]->getVelocity()) + ")");
            }

            if (! roundTrip)
            {
                bool hasSynth = false;
                for (auto* p : keys->pluginList)
                    hasSynth = hasSynth || p->isSynth();
                log.check (hasSynth, "MIDI track got a synth");
            }
        }
    }

    /** Imports into a new Elsewhere project, the same way the File menu does. */
    std::unique_ptr<Project> importInto (te::Engine& engine, Log& log, const juce::File& file, const juce::File& dir, DawProject::ImportSummary& summary)
    {
        dir.deleteRecursively();
        auto editFile = Project::createProjectFolder (dir, "Imported");

        DawProject::Importer importer (file);
        log.check (importer.getError().isEmpty(), "opened " + file.getFileName() + " " + importer.getError());
        log.check (importer.extractAudio (editFile.getParentDirectory().getChildFile ("Samples/Imported")), "copied the audio");

        auto project = std::make_unique<Project> (engine, editFile);
        summary = importer.build (project->getEdit());
        log.info (summary.describe().replace ("\n", "\n        "));
        return project;
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.createDirectory();
    auto fixtures = outDir.getChildFile ("fixture");
    fixtures.deleteRecursively();
    fixtures.createDirectory();

    writeSine (fixtures.getChildFile ("drums.wav"), 4.0, 220.0);
    writeSine (fixtures.getChildFile ("bass.wav"), 4.0, 55.0);

    // Written with juce::ZipFile, so our reader is checked against another writer
    auto source = outDir.getChildFile ("bitwig-style.dawproject");
    {
        source.deleteFile();
        juce::ZipFile::Builder zip;
        juce::String xml (projectXml);
        zip.addEntry (std::make_unique<juce::MemoryInputStream> (xml.toRawUTF8(), xml.getNumBytesAsUTF8(), true), 9, "project.xml", juce::Time::getCurrentTime());
        zip.addFile (fixtures.getChildFile ("drums.wav"), 0, "audio/drums.wav");
        zip.addFile (fixtures.getChildFile ("bass.wav"), 6, "audio/bass.wav");
        juce::FileOutputStream out (source);
        zip.writeToStream (out, nullptr);
    }

    log.text << "Import (Bitwig-style file)\n";
    DawProject::ImportSummary summary;
    auto project = importInto (engine, log, source, outDir.getChildFile ("import"), summary);
    auto& edit = project->getEdit();
    log.check (summary.applicationName == "Bitwig Studio", "read the application name");
    log.check (summary.missingFiles.contains ("missing.wav"), "reported the missing file");
    log.check (summary.skippedReturnTracks == 1, "reported the return track");
    checkImported (log, edit, false);

    log.text << "\nExport, then import the export\n";
    auto exported = outDir.getChildFile ("round-trip.dawproject");
    auto plan = DawProject::planExport (edit, "Round trip");
    auto result = DawProject::writeExport (plan, exported);
    log.check (result.wasOk(), "wrote " + exported.getFileName() + " " + result.getErrorMessage());
    log.check (plan.audioFiles.size() == 2, "each audio file stored once: " + juce::String ((int) plan.audioFiles.size()));

    {
        juce::ZipFile check (exported);
        log.check (check.getIndexOfFileName ("project.xml") >= 0 && check.getNumEntries() == 4,
                   "juce::ZipFile reads our archive (" + juce::String (check.getNumEntries()) + " entries)");

        if (auto i = check.getIndexOfFileName ("project.xml"); i >= 0)
        {
            std::unique_ptr<juce::InputStream> in (check.createStreamForEntry (i));
            auto xml = juce::parseXML (in->readEntireStreamAsString());
            log.check (xml != nullptr && xml->hasTagName ("Project"), "project.xml inflates and parses");

            if (xml != nullptr)
                xml->writeTo (outDir.getChildFile ("round-trip-project.xml"));
        }
    }

    DawProject::ImportSummary again;
    auto project2 = importInto (engine, log, exported, outDir.getChildFile ("reimport"), again);
    log.check (again.applicationName == "Elsewhere", "export says it's from Elsewhere");
    log.check (again.missingFiles.isEmpty(), "no missing files after the round trip");
    checkImported (log, project2->getEdit(), true);

    // Tempo-following and looped Elsewhere clips export as plain warped clips
    log.text << "\nElsewhere clip kinds\n";
    {
        auto file = fixtures.getChildFile ("drums.wav");
        outDir.getChildFile ("loop").deleteRecursively();
        Project loopProject (engine, Project::createProjectFolder (outDir.getChildFile ("loop"), "Loop"));
        auto* loopEdit = &loopProject.getEdit();
        loopEdit->tempoSequence.getTempo (0)->setBpm (100.0);
        loopEdit->ensureNumberOfAudioTracks (1);
        auto track = te::getAudioTracks (*loopEdit).getFirst();
        file = loopProject.importAudioFile (file);
        auto secs = [] (double s) { return te::TimePosition::fromSeconds (s); };
        auto loop = track->insertWaveClip ("Looped", file, { { secs (1.0), secs (5.0) }, {} }, false);
        loop->setAutoTempo (false);
        loop->setLoopRange ({ secs (0.5), secs (1.5) });      // 1 s loop, from 0.5 s in the file
        auto follow = track->insertWaveClip ("Follows tempo", file, { { secs (10.0), secs (12.4) }, {} }, false);
        follow->getLoopInfo().setBpm (120.0, follow->getAudioFile().getInfo());
        follow->setAutoTempo (true);
        follow->setPosition ({ { secs (10.0), secs (12.4) }, {} });

        auto loopFile = outDir.getChildFile ("loop.dawproject");
        DawProject::writeExport (DawProject::planExport (*loopEdit, "Loop"), loopFile);
        DawProject::ImportSummary s;
        auto back = importInto (engine, log, loopFile, outDir.getChildFile ("loop-reimport"), s);
        auto clips = waveClips (*te::getAudioTracks (back->getEdit()).getFirst());
        log.check (clips.size() == 5, "1 s loop over 4 s -> 4 clips, plus the tempo-following one: " + juce::String (clips.size()));

        for (int i = 0; i < std::min (4, clips.size()); ++i)
            checkClip (log, clips[i], 1.0 + i, 2.0 + i, 0.5, 1.0, "loop pass " + juce::String (i + 1));

        // 4 beats at 100 bpm play 2 s of a 120 bpm file
        if (clips.size() == 5)
            checkClip (log, clips[4], 10.0, 12.4, 0.0, 2.0 / 2.4, "tempo-following clip");
    }

    // The warped clip as it'll play: from its warped copy, rendered in the background
    log.text << "\nWarped clip playback\n";
    {
        // Short bursts at 0.5, 1.5 and 2.5 s in the file
        const double rate = 44100.0;
        juce::AudioBuffer<float> clicks (1, (int) (4.0 * rate));
        clicks.clear();

        for (auto at : { 0.5, 1.5, 2.5 })
            for (int i = 0; i < (int) (0.02 * rate); ++i)
                clicks.setSample (0, (int) (at * rate) + i, 0.8f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate));

        outDir.getChildFile ("warp").deleteRecursively();
        Project warpProject (engine, Project::createProjectFolder (outDir.getChildFile ("warp"), "Warp"));
        auto& warpEdit = warpProject.getEdit();
        warpEdit.ensureNumberOfAudioTracks (1);
        auto clickFile = warpProject.getFolder().getChildFile ("Samples/Imported/clicks.wav");
        {
            std::unique_ptr<juce::OutputStream> out (clickFile.createOutputStream());
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (24));
            writer->writeFromAudioSampleBuffer (clicks, 0, clicks.getNumSamples());
        }

        // From 1 s on the timeline: the file's first second over 2 s (half speed), then 2 s at normal speed.
        // So the bursts should land at 1 + 1.0 = 2.0 s, 1 + 2.5 = 3.5 s and 1 + 3.5 = 4.5 s
        auto track = te::getAudioTracks (warpEdit).getFirst();
        auto clip = SessionImport::addWarpedAudioClip (*track, "Warped", clickFile, { { 1.0, 0.0 }, { 3.0, 1.0 }, { 5.0, 3.0 } });
        log.check (clip != nullptr && clip->getWarpTime(), "made a clip with warp markers");

        // Waits for the file it plays (rendered in the background) to change and appear
        auto playing = clip != nullptr ? clip->getOriginalFile() : juce::File();

        auto waitForPlaybackFile = [&]
        {
            for (int i = 0; i < 400 && (clip->getPlaybackFile().getFile() == playing || ! clip->getPlaybackFile().isValid()); ++i)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (50);

            log.check (clip->getPlaybackFile().isValid() && clip->getPlaybackFile().getFile() != playing, "its playback file rendered");
            playing = clip->getPlaybackFile().getFile();
        };

        // The bursts' times, and the pitch of the first one
        auto renderAndCheck = [&] (const juce::String& name, double expectedHz)
        {
            auto out = outDir.getChildFile (name);
            out.deleteFile();
            juce::BigInteger tracks;
            tracks.setBit (track->getIndexInEditTrackList());
            te::Renderer::renderToFile ("Test", out, warpEdit, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (5.5)), tracks, true, false, {}, true);

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out));
            juce::Array<double> onsets;
            double hz = 0.0;

            if (reader != nullptr)
            {
                juce::AudioBuffer<float> b ((int) reader->numChannels, (int) reader->lengthInSamples);
                reader->read (&b, 0, b.getNumSamples(), 0, true, true);
                double lastLoud = -1.0;

                // Pitch: zero crossings through the middle of the first burst
                {
                    auto from = (int) (2.01 * reader->sampleRate), to = (int) (2.03 * reader->sampleRate);
                    int crossings = 0;

                    for (int i = from + 1; i < std::min (to, b.getNumSamples()); ++i)
                        if ((b.getSample (0, i - 1) < 0.0f) != (b.getSample (0, i) < 0.0f))
                            ++crossings;

                    hz = crossings / 2.0 / ((to - from) / reader->sampleRate);
                }

                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    if (std::abs (b.getSample (0, i)) > 0.2f)
                    {
                        auto t = i / reader->sampleRate;

                        if (t - lastLoud > 0.2)
                            onsets.add (t);

                        lastLoud = t;
                    }
                }
            }

            log.check (onsets.size() == 3, "3 bursts in the render: " + juce::String (onsets.size()));
            log.near (hz, expectedHz, "pitch of the first burst", expectedHz * 0.03);

            juce::Array<double> expected { 2.0, 3.5, 4.5 };
            for (int i = 0; i < std::min (3, onsets.size()); ++i)
                log.near (onsets[i], expected[i], "burst " + juce::String (i + 1), 0.025);
        };

        if (clip != nullptr)
        {
            waitForPlaybackFile();
            renderAndCheck ("warp-render.wav", 1000.0);
        }

        if (clip != nullptr)
        {
            // Transposed as well (Ableton keeps transposition separate from warping): same timing,
            // an octave up
            clip->setPitchChange (12.0f);
            waitForPlaybackFile();
            renderAndCheck ("warp-transposed-render.wav", 2000.0);
        }
    }

    log.text << "\n" << (log.failures == 0 ? "All passed" : juce::String (log.failures) + " failed") << "\n";
    outDir.getChildFile ("dawproject-test.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::DawProjectTests
