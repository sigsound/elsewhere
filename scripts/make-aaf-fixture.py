# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Writes a Pro Tools / Media Composer style AAF for Elsewhere's AAF import self-test.

usage: python -I scripts/make-aaf-fixture.py <outDir>   (needs pyaaf2: pip install pyaaf2)
Then: Elsewhere --aaf-test <outDir> --no-audio
"""
import math, os, struct, sys, wave
import aaf2
from aaf2.auid import AUID
from aaf2.rational import AAFRational

out = sys.argv[1]
os.makedirs(os.path.join(out, "Audio Files"), exist_ok=True)

RATE = 48000

def write_wav(path, seconds, hz, channels=1, width=2):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels); w.setsampwidth(width); w.setframerate(RATE)
        frames = bytearray()
        for i in range(int(seconds * RATE)):
            v = int(0.4 * math.sin(2 * math.pi * hz * i / RATE) * (2 ** (8 * width - 1) - 1))
            sample = v.to_bytes(width, "little", signed=True)
            frames += sample * channels
        w.writeframes(bytes(frames))

kick = os.path.join(out, "kick.wav");             write_wav(kick, 2.0, 100)
vox = os.path.join(out, "vox.wav");               write_wav(vox, 4.0, 330, channels=2, width=3)
gtrL = os.path.join(out, "Audio Files", "Gtr.L.wav"); write_wav(gtrL, 3.0, 220)
gtrR = os.path.join(out, "Audio Files", "Gtr.R.wav"); write_wav(gtrR, 3.0, 221)

MonoAudioGain = AUID("9d2ea894-0968-11d3-8a38-0050040ef7d2")
MonoAudioPan = AUID("9d2ea893-0968-11d3-8a38-0050040ef7d2")
MonoAudioDissolve = AUID("0c3bea41-fc05-11d2-8a29-0050040ef7d2")
Amplitude = AUID("e4962321-2267-11d3-8a4c-0050040ef7d2")
Pan = AUID("e4962322-2267-11d3-8a4c-0050040ef7d2")
Rational = AUID("03010100-0000-0000-060e-2b3401040101")

with aaf2.open(os.path.join(out, "pt-style.aaf"), "w") as f:
    def opdef(auid, name):
        d = f.create.OperationDef(auid, name, "")
        d.media_kind = "sound"
        d["NumberInputs"].value = 2 if auid == MonoAudioDissolve else 1
        f.dictionary.register_def(d)
        return d

    def paramdef(auid, name):
        p = f.create.ParameterDef(auid, name, "", Rational)
        f.dictionary.register_def(p)
        return p

    gain_op, pan_op, dissolve_op = opdef(MonoAudioGain, "Mono Audio Gain"), opdef(MonoAudioPan, "Mono Audio Pan"), opdef(MonoAudioDissolve, "Mono Audio Dissolve")
    amp_param, pan_param = paramdef(Amplitude, "Amplitude"), paramdef(Pan, "Pan")

    def wrap(op, param, value, segment, length):
        g = f.create.OperationGroup(op, length, "sound")
        g.segments.append(segment)
        c = f.create.ConstantValue(param, AAFRational(value))
        g.parameters.append(c)
        return g

    def master(name, path):
        m = f.create.MasterMob(name)
        f.content.mobs.append(m)
        m.import_audio_essence(path, RATE)
        return m

    def external_master(name, path, stale_url):
        """A file source mob pointing at a WAV that's not embedded (the URL is from another machine)."""
        with wave.open(path) as w:
            frames = w.getnframes()

        src = f.create.SourceMob(name)
        f.content.mobs.append(src)
        slot = src.create_empty_slot(RATE, media_kind="sound", slot_id=1)
        slot.segment.length = frames
        d = f.create.WAVEDescriptor()
        d["SampleRate"].value = RATE
        d["Length"].value = frames
        with open(path, "rb") as fh:
            d["Summary"].value = fh.read(44)
        loc = f.create.NetworkLocator()
        loc["URLString"].value = stale_url
        d["Locator"].append(loc)
        src.descriptor = d

        m = f.create.MasterMob(name)
        f.content.mobs.append(m)
        ms = m.create_timeline_slot(RATE)
        ms.segment = src.create_source_clip(1, media_kind="sound")
        ms.segment.length = frames
        return m

    kick_m = master("Kick", kick)
    vox_m = master("Vox", vox)
    gl = external_master("Gtr.L", gtrL, "file:///Volumes/Studio/My%20Song/Audio%20Files/Gtr.L.wav")
    gr = external_master("Gtr.R", gtrR, "file:///Volumes/Studio/My%20Song/Audio%20Files/Gtr.R.wav")

    comp = f.create.CompositionMob("My Song")
    comp.usage = "Usage_TopLevel"
    f.content.mobs.append(comp)

    def clip(m, start, length):
        return m.create_source_clip(1, start=start, length=length, media_kind="sound")

    def filler(length, kind="sound"):
        return f.create.Filler(kind, length)

    # Kick (48k edit rate): 1 s gap, 1 s clip, 0.5 s gap, 0.5 s clip at -6 dB from 0.5 s in
    s = comp.create_sound_slot(edit_rate=RATE)
    s.name = "Kick"
    seq = f.create.Sequence("sound")
    seq.components.append(filler(48000))
    seq.components.append(clip(kick_m, 0, 48000))
    seq.components.append(filler(24000))
    seq.components.append(wrap(gain_op, amp_param, 0.5, clip(kick_m, 24000, 24000), 24000))
    s.segment = seq

    # Guitar L/R (25 fps edit rate, like Media Composer): 1 s gap, then 2 s from 0.48 s in, panned hard L / R
    for m, p in ((gl, 0.0), (gr, 1.0)):
        s = comp.create_sound_slot(edit_rate=25)
        s.name = "Gtr"
        seq = f.create.Sequence("sound")
        seq.components.append(filler(25))
        seq.components.append(m.create_source_clip(1, start=12, length=50, media_kind="sound"))
        s.segment = wrap(pan_op, pan_param, p, seq, 75)

    # Vox: 2 s clip, 0.2 s crossfade into 1.5 s from 2.5 s in, 0.1 s fade out, at +6 dB on the track
    s = comp.create_sound_slot(edit_rate=RATE)
    s.name = "Vox"
    seq = f.create.Sequence("sound")
    seq.components.append(clip(vox_m, 0, 96000))
    t = f.create.Transition("sound", 9600)
    t["CutPoint"].value = 4800
    t["OperationGroup"].value = f.create.OperationGroup(dissolve_op, 9600, "sound")
    seq.components.append(t)
    seq.components.append(clip(vox_m, 120000, 72000))
    t = f.create.Transition("sound", 4800)
    t["CutPoint"].value = 2400
    t["OperationGroup"].value = f.create.OperationGroup(dissolve_op, 4800, "sound")
    seq.components.append(t)
    seq.components.append(filler(48000))
    s.segment = wrap(gain_op, amp_param, 2.0, seq, 96000 - 9600 + 72000 - 4800 + 48000)

    # A picture track and a timecode track, which aren't audio
    v = comp.create_picture_slot(edit_rate=25)
    v.segment = filler(100, "picture")
    tc = comp.create_timeline_slot(25)
    tc.segment = f.create.Timecode(25, False)
    tc.segment.start = 90000
    tc.segment.length = 100

print("wrote", os.path.join(out, "pt-style.aaf"))
