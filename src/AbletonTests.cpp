// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AbletonTests.h"
#include "Ableton.h"
#include "Project.h"

namespace elsewhere::AbletonTests
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

    void writeSine (const juce::File& f, double seconds, juce::AudioFormat& format)
    {
        const double rate = 44100.0;
        juce::AudioBuffer<float> buffer (2, (int) (seconds * rate));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / rate));

        f.getParentDirectory().createDirectory();
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream());
        auto writer = format.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
    }

    void writeSet (const juce::File& f, const juce::String& xml)
    {
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::GZIPCompressorOutputStream gz (out, 9, 15 + 16);    // gzip, as Live writes it
        gz.writeText (xml, false, false, nullptr);
    }

    // Live 12 layout. Tempo 100 jumping to 140 at beat 16 (8 beats in at 100 = 9.6 s)
    const char* live12 = R"(<?xml version="1.0" encoding="UTF-8"?>
<Ableton MajorVersion="5" MinorVersion="12.0_12300" Creator="Ableton Live 12.1">
  <LiveSet>
    <Tracks>
      <AudioTrack Id="10">
        <Name><EffectiveName Value="1-Drums" /><UserName Value="Drums" /></Name>
        <TrackGroupId Value="-1" />
        <DeviceChain>
          <MainSequencer><Sample><ArrangerAutomation><Events>
            <AudioClip Id="1" Time="4">
              <CurrentStart Value="4" /><CurrentEnd Value="12" />
              <Loop><LoopStart Value="0" /><LoopEnd Value="8" /><StartRelative Value="0" /><LoopOn Value="false" /></Loop>
              <Name Value="Beat" />
              <Disabled Value="false" />
              <IsWarped Value="true" />
              <WarpMode Value="0" />
              <SampleRef><FileRef><RelativePathType Value="1" /><RelativePath Value="Samples/Imported/drums.wav" /><Path Value="/nowhere/drums.wav" /></FileRef></SampleRef>
              <Fades><FadeInLength Value="1" /><FadeOutLength Value="0" /></Fades>
              <PitchCoarse Value="2" /><PitchFine Value="0" />
              <SampleVolume Value="0.5" />
              <WarpMarkers><WarpMarker Id="0" SecTime="0" BeatTime="0" /><WarpMarker Id="1" SecTime="2" BeatTime="4" /></WarpMarkers>
            </AudioClip>
          </Events></ArrangerAutomation></Sample></MainSequencer>
          <Mixer>
            <Speaker><Manual Value="true" /></Speaker>
            <Pan><Manual Value="-0.5" /></Pan>
            <Volume><Manual Value="0.5" /></Volume>
          </Mixer>
        </DeviceChain>
      </AudioTrack>
      <GroupTrack Id="20">
        <Name><EffectiveName Value="Band" /><UserName Value="" /></Name>
        <TrackGroupId Value="-1" />
        <DeviceChain><Mixer><Volume><Manual Value="1" /></Volume></Mixer></DeviceChain>
      </GroupTrack>
      <AudioTrack Id="21">
        <Name><EffectiveName Value="3-Audio" /><UserName Value="" /></Name>
        <TrackGroupId Value="20" />
        <DeviceChain>
          <MainSequencer><Sample><ArrangerAutomation><Events>
            <AudioClip Id="2" Time="20">
              <CurrentStart Value="20" /><CurrentEnd Value="24" />
              <Loop><LoopStart Value="1.5" /><LoopEnd Value="3.5" /><StartRelative Value="0" /><LoopOn Value="false" /></Loop>
              <Name Value="Unwarped" />
              <IsWarped Value="false" />
              <SampleRef><FileRef><RelativePath Value="../elsewhere/bass.wav" /><Path Value="/also/gone/bass.wav" /></FileRef></SampleRef>
              <SampleVolume Value="1" />
            </AudioClip>
          </Events></ArrangerAutomation></Sample></MainSequencer>
          <Mixer><Speaker><Manual Value="false" /></Speaker></Mixer>
        </DeviceChain>
      </AudioTrack>
      <MidiTrack Id="30">
        <Name><EffectiveName Value="Keys" /><UserName Value="" /></Name>
        <TrackGroupId Value="-1" />
        <DeviceChain>
          <MainSequencer><ClipTimeable><ArrangerAutomation><Events>
            <MidiClip Id="3" Time="0">
              <CurrentStart Value="0" /><CurrentEnd Value="8" />
              <Loop><LoopStart Value="0" /><LoopEnd Value="4" /><StartRelative Value="0" /><LoopOn Value="true" /></Loop>
              <Name Value="Chords" />
              <Notes><KeyTracks>
                <KeyTrack Id="0"><Notes><MidiNoteEvent Time="0" Duration="1" Velocity="127" /></Notes><MidiKey Value="60" /></KeyTrack>
                <KeyTrack Id="1"><Notes><MidiNoteEvent Time="2" Duration="1" Velocity="64" /></Notes><MidiKey Value="64" /></KeyTrack>
              </KeyTracks></Notes>
            </MidiClip>
          </Events></ArrangerAutomation></ClipTimeable></MainSequencer>
        </DeviceChain>
      </MidiTrack>
      <ReturnTrack Id="40"><Name><EffectiveName Value="A-Reverb" /></Name></ReturnTrack>
    </Tracks>
    <MainTrack>
      <AutomationEnvelopes><Envelopes>
        <AutomationEnvelope Id="0">
          <EnvelopeTarget><PointeeId Value="8" /></EnvelopeTarget>
          <Automation><Events>
            <FloatEvent Id="1" Time="-63072000" Value="100" />
            <FloatEvent Id="2" Time="16" Value="100" />
            <FloatEvent Id="3" Time="16" Value="140" />
          </Events></Automation>
        </AutomationEnvelope>
      </Envelopes></AutomationEnvelopes>
      <DeviceChain><Mixer>
        <Volume><Manual Value="0.25" /></Volume>
        <Tempo><Manual Value="100" /><AutomationTarget Id="8" /></Tempo>
        <TimeSignature><Manual Value="200" /></TimeSignature>
      </Mixer></DeviceChain>
    </MainTrack>
  </LiveSet>
</Ableton>)";

    // Live 8 layout: values as automation events, files as folders relative to the project
    const char* live8 = R"(<?xml version="1.0" encoding="UTF-8"?>
<Ableton MajorVersion="4" MinorVersion="8.1_223">
  <LiveSet>
    <Tracks>
      <AudioTrack>
        <Name><EffectiveName Value="Guitar" /><UserName Value="Guitar" /></Name>
        <TrackGroupId Value="-1" />
        <DeviceChain>
          <MainSequencer><Sample><ArrangerAutomation><Events>
            <AudioClip Time="8">
              <CurrentStart Value="8" /><CurrentEnd Value="16" />
              <Loop><LoopStart Value="1" /><LoopEnd Value="9" /><StartRelative Value="0" /><LoopOn Value="false" /></Loop>
              <Name Value="0003 guitar" />
              <IsWarped Value="true" />
              <SampleRef><FileRef><HasRelativePath Value="true" /><RelativePathType Value="3" />
                <RelativePath><RelativePathElement Dir="Samples" /><RelativePathElement Dir="Recorded" /></RelativePath>
                <Name Value="0003 guitar.aif" /></FileRef></SampleRef>
              <SampleVolume Value="1" />
              <WarpMarkers><WarpMarker SecTime="0" BeatTime="1" /><WarpMarker SecTime="0.015625" BeatTime="1.0234375" /></WarpMarkers>
            </AudioClip>
          </Events></ArrangerAutomation></Sample></MainSequencer>
          <Mixer>
            <Speaker><ArrangerAutomation><Events><BoolEvent Time="-63072000" Value="true" /></Events></ArrangerAutomation></Speaker>
            <Pan><ArrangerAutomation><Events><FloatEvent Time="-63072000" Value="0.25" /></Events></ArrangerAutomation></Pan>
            <Volume><ArrangerAutomation><Events><FloatEvent Time="-63072000" Value="0.25" /></Events></ArrangerAutomation></Volume>
          </Mixer>
        </DeviceChain>
      </AudioTrack>
    </Tracks>
    <MasterTrack>
      <MasterChain><Mixer>
        <Tempo><ArrangerAutomation><Events><FloatEvent Time="-63072000" Value="90" /></Events></ArrangerAutomation></Tempo>
        <TimeSignature><ArrangerAutomation><Events><EnumEvent Time="-63072000" Value="201" /></Events></ArrangerAutomation></TimeSignature>
      </Mixer></MasterChain>
    </MasterTrack>
  </LiveSet>
</Ableton>)";

    te::AudioTrack* findTrack (te::Edit& edit, const juce::String& name)
    {
        for (auto t : te::getAudioTracks (edit))
            if (t->getName() == name)
                return t;
        return nullptr;
    }

    template <typename ClipType>
    juce::Array<ClipType*> clipsOf (te::AudioTrack* t)
    {
        juce::Array<ClipType*> clips;
        if (t != nullptr)
            for (auto* c : t->getClips())
                if (auto* typed = dynamic_cast<ClipType*> (c))
                    clips.add (typed);
        return clips;
    }

    std::unique_ptr<Project> import (te::Engine& engine, Log& log, const juce::File& set, const juce::File& outDir, SessionImport::Summary& summary)
    {
        outDir.deleteRecursively();
        auto editFile = Project::createProjectFolder (outDir, "Imported");

        Ableton::Importer importer (set);
        log.check (importer.getError().isEmpty(), "opened " + set.getFileName() + " " + importer.getError());
        log.check (importer.extractAudio (editFile.getParentDirectory().getChildFile ("Samples/Imported")), "copied the audio");

        if (auto* xml = importer.getConvertedProject())
            xml->writeTo (outDir.getChildFile (set.getFileNameWithoutExtension() + "-as-dawproject.xml"));

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
    juce::WavAudioFormat wav;
    juce::AiffAudioFormat aiff;

    //==============================================================================
    log.text << "Live 12 set\n";
    auto setDir = outDir.getChildFile ("Live12 Project");
    setDir.deleteRecursively();
    writeSine (setDir.getChildFile ("Samples/Imported/drums.wav"), 4.0, wav);
    writeSine (setDir.getChildFile ("Some Other Folder/bass.wav"), 4.0, wav);     // moved: found by name
    writeSet (setDir.getChildFile ("Live12.als"), live12);

    SessionImport::Summary summary;
    auto project = import (engine, log, setDir.getChildFile ("Live12.als"), outDir.getChildFile ("live12-import"), summary);
    auto& edit = project->getEdit();
    log.check (summary.applicationName == "Ableton Live 12.1", "application name: " + summary.applicationName);
    log.check (summary.missingFiles.isEmpty(), "found every file, including one that moved");
    log.check (summary.skippedReturnTracks == 1, "return track left out");

    auto& ts = edit.tempoSequence;
    log.check (ts.getNumTempos() == 2, "two tempos: " + juce::String (ts.getNumTempos()));
    log.near (ts.getTempo (0)->getBpm(), 100.0, "starting tempo");

    if (ts.getNumTempos() == 2)
    {
        log.near (ts.getTempo (1)->getBpm(), 140.0, "tempo after the jump");
        log.near (ts.getTempo (1)->getStartBeat().inBeats(), 16.0, "jump beat");
    }

    log.check (ts.getTimeSig (0)->numerator == 3 && ts.getTimeSig (0)->denominator == 4, "time signature 3/4 (Live's 200)");
    log.near (edit.getMasterVolumePlugin()->getVolumeDb(), -12.04, "master volume (0.25 = -12 dB)", 0.01);

    // Warped at 120 bpm, played at 100: 8 beats = 4.8 s from 4 s of audio
    auto* drums = findTrack (edit, "Drums");
    auto d = clipsOf<te::WaveAudioClip> (drums);
    log.check (d.size() == 1, "Drums has 1 clip");

    if (d.size() == 1)
    {
        auto pos = d[0]->getPosition();
        log.near (pos.getStart().inSeconds(), 2.4, "warped clip start (beat 4 at 100 bpm)");
        log.near (pos.getEnd().inSeconds(), 7.2, "warped clip end");
        log.near (d[0]->getSpeedRatio(), 4.0 / 4.8, "warped clip speed (120 bpm audio at 100)");
        log.near (d[0]->getGainDB(), -6.0206, "clip gain (0.5)", 0.01);
        log.near (d[0]->getPitchChange(), 2.0, "transposed +2");
        log.near (d[0]->getFadeIn().inSeconds(), 0.6, "fade in (1 beat)");
        log.check (d[0]->getTimeStretchMode() != te::TimeStretcher::disabled, "keeps its pitch while stretched");
    }

    if (drums != nullptr)
    {
        log.near (drums->getVolumePlugin()->getVolumeDb(), -6.0206, "Drums volume", 0.01);
        log.near (drums->getVolumePlugin()->getPan(), -0.5, "Drums pan");
        log.check (! drums->isMuted (false), "Drums not muted");
    }

    // Unwarped, after the jump to 140: beat 20 = 9.6 + 4 * 60/140 s; real-time playback from 1.5 s in
    auto* bass = findTrack (edit, "Audio 3");
    log.check (bass != nullptr, "unnamed track named \"Audio 3\"");
    log.check (bass != nullptr && bass->isMuted (false), "muted track stays muted");
    log.check (bass != nullptr && dynamic_cast<te::FolderTrack*> (bass->getParentTrack()) != nullptr
                && bass->getParentTrack()->getName() == "Band", "it's inside the Band group");

    auto b = clipsOf<te::WaveAudioClip> (bass);
    log.check (b.size() == 1, "unwarped clip came across");

    if (b.size() == 1)
    {
        auto start = 9.6 + 4.0 * 60.0 / 140.0;
        log.near (b[0]->getPosition().getStart().inSeconds(), start, "unwarped clip start");
        log.near (b[0]->getPosition().getEnd().inSeconds(), start + 4.0 * 60.0 / 140.0, "unwarped clip end");
        log.near (b[0]->getPosition().getOffset().inSeconds(), 1.5, "unwarped clip offset (seconds)");
        log.near (b[0]->getSpeedRatio(), 1.0, "unwarped clip plays at its own speed");
    }

    auto* keys = findTrack (edit, "Keys");
    auto m = clipsOf<te::MidiClip> (keys);
    int notes = 0;
    for (auto* c : m)
        notes += c->getSequence().getNumNotes();

    log.check (m.size() == 2 && notes == 4, "looped MIDI clip: 2 passes, 4 notes (" + juce::String (m.size()) + ", " + juce::String (notes) + ")");

    if (m.size() == 2)
    {
        log.near (m[1]->getPosition().getStart().inSeconds(), 2.4, "second pass starts at beat 4");
        auto& seq = m[0]->getSequence();
        log.check (seq.getNumNotes() == 2 && seq.getNotes()[0]->getNoteNumber() == 60 && seq.getNotes()[1]->getNoteNumber() == 64, "note pitches");
        log.check (seq.getNumNotes() == 2 && seq.getNotes()[1]->getVelocity() == 64, "velocity");
    }

    //==============================================================================
    log.text << "\nLive 8 set\n";
    auto oldDir = outDir.getChildFile ("Live8 Project");
    oldDir.deleteRecursively();
    writeSine (oldDir.getChildFile ("Samples/Recorded/0003 guitar.aif"), 8.0, aiff);
    writeSet (oldDir.getChildFile ("Live8.als"), live8);

    SessionImport::Summary oldSummary;
    auto oldProject = import (engine, log, oldDir.getChildFile ("Live8.als"), outDir.getChildFile ("live8-import"), oldSummary);
    auto& oldEdit = oldProject->getEdit();
    log.check (oldSummary.applicationName == "Ableton Live 8.1", "application name: " + oldSummary.applicationName);
    log.check (oldSummary.missingFiles.isEmpty(), "found the file through its relative folders");
    log.near (oldEdit.tempoSequence.getTempo (0)->getBpm(), 90.0, "tempo from its automation event");

    auto* guitar = findTrack (oldEdit, "Guitar");
    auto g = clipsOf<te::WaveAudioClip> (guitar);
    log.check (g.size() == 1, "Guitar has 1 clip");

    // Warp markers are 1.5 s per 3 beats... 0.0234375 beats per 0.015625 s = 90 bpm, so speed 1;
    // the clip starts 1 beat into its content, which is 0 s in the file
    if (g.size() == 1)
    {
        log.near (g[0]->getPosition().getStart().inSeconds(), 8.0 * 60.0 / 90.0, "clip start (beat 8 at 90)");
        log.near (g[0]->getPosition().getOffset().inSeconds(), 0.0, "clip offset");
        log.near (g[0]->getSpeedRatio(), 1.0, "speed");
    }

    if (guitar != nullptr)
    {
        log.near (guitar->getVolumePlugin()->getVolumeDb(), -12.04, "volume from its automation event", 0.01);
        log.near (guitar->getVolumePlugin()->getPan(), 0.25, "pan from its automation event");
    }

    log.text << "\n" << (log.failures == 0 ? "All passed" : juce::String (log.failures) + " failed") << "\n";
    outDir.getChildFile ("ableton-test.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::AbletonTests
