#!/usr/bin/env python3
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
Builds the "Organic Sounds" pack for Elsewhere Organic from VSCO 2 Community Edition
(Versilian Studios, CC0 1.0, https://github.com/sgossner/VSCO-2-CE).

  scripts/build-organic-pack.py <download cache folder> <output folder>

Only the curated files are downloaded (into the cache, so reruns are quick). For every
source: the notes and dynamic layers listed in SOURCES, trimmed of leading silence, cut to
the source's length with a short fade, resampled to 48 kHz stereo, all files of a source
scaled by one gain (so the dynamics between layers and notes stay), written as 16-bit FLAC
  <source>_<midi note>_v<layer>[_r<take>].flac
plus pack.json describing them. Root notes come from the file names plus the measured
octave corrections in OFFSETS.
Needs curl, ffmpeg and ffprobe.
"""
import json, os, re, subprocess, sys, urllib.parse
from concurrent.futures import ThreadPoolExecutor

CACHE, OUT = sys.argv[1], sys.argv[2]
REPO = "https://raw.githubusercontent.com/sgossner/VSCO-2-CE/master/"
PACK_ID, PACK_NAME, PACK_VERSION = "organic", "Organic Sounds", 2

# id, name, family, regex over repo paths (groups: note, layer), layer tokens in order soft -> loud,
# max seconds, pitched
SOURCES = [
    ("cello",    "Cello Ensemble",  "Bowed",   r"Strings/Cello Section/susvib/susvib_([A-G]#?\d)_(v1|v3)_1\.wav", ["v1", "v3"], 8.0, True),
    ("violins",  "Violin Ensemble", "Bowed",   r"Strings/Violin Section/susVib/VlnEns_susVib_([A-G]#?\d)_(v1|v2)\.wav", ["v1", "v2"], 8.0, True),
    ("solovln",  "Solo Violin",     "Bowed",   r"Strings/Solo Violin/Arco Vib/LLVln_ArcoVib_([A-G]#?\d)_(p|f)\.wav", ["p", "f"], 8.0, True),
    ("bass",     "Contrabass",      "Bowed",   r"Strings/Solo Contrabass/SusNV/BKCtbss_SusNV_([A-G]#?\d)_(v1|v3)_rr1\.wav", ["v1", "v3"], 8.0, True),
    ("flute",    "Flute",           "Breath",  r"Woodwinds/Flute/susNV/LDFlute_susNV_([A-G]#?\d)_(v1|v3)_1\.wav", ["v1", "v3"], 8.0, True),
    ("clarinet", "Clarinet",        "Breath",  r"Woodwinds/Clarinet/susLong/DCClar_susLong_([A-G]#?\d)_(v1|v3)_rr1_sum\.wav", ["v1", "v3"], 8.0, True),
    ("bassoon",  "Bassoon",         "Breath",  r"Woodwinds/Bassoon/sus/PSBassoon_([A-G]#?\d)_(v1|v2)_1\.wav", ["v1", "v2"], 8.0, True),
    ("horn",     "French Horn",     "Breath",  r"Brass/F Horn/sus/MOHorn_sus_([A-G]#?\d)_(v1|v3)_1\.wav", ["v1", "v3"], 8.0, True),
    ("harp",     "Harp",            "Plucked", r"Strings/Harp/KSHarp_([A-G]#?\d)_(mf)\.wav", ["mf"], 6.0, True),
    ("bpizz",    "Contrabass Pizz", "Plucked", r"Strings/Solo Contrabass/Pizz/BKCtbss_Pizz_([A-G]#?\d)_(v1|v3)_rr1\.wav", ["v1", "v3"], 4.0, True),
    ("vpizz",    "Violin Pizz",     "Plucked", r"Strings/Solo Violin/Pizz/LLVln_Pizz_([A-G]#?\d)_(p|f)_RR1\.wav", ["p", "f"], 3.0, True),
    ("marimba",  "Marimba",         "Struck",  r"Percussion/Marimba/Marimba_hit_Outrigger_([A-G]#?\d)_(loud)_01\.wav", ["loud"], 5.0, True),
    ("glock",    "Glockenspiel",    "Struck",  r"Percussion/Glock/glock_medium_([A-G]#?\d)()\.wav", [""], 5.0, True),
    ("bells",    "Nepalese Bells",  "Texture", r"Miscellania Raw/Misc 2/NepaleseBells/(fx1)_short_r0([1-6])_main\.wav", None, 6.0, False),
    ("cymbal",   "Bowed Cymbal",    "Texture", r"Percussion/susCymb1-bow-()([1-4])\.wav", None, 8.0, False),
    ("gong",     "Gong Scrape",     "Texture", r"Percussion/gongscrape_()(pp|mf)\.wav", None, 8.0, False),

    # Version 2: more of the orchestra, keys, bells and swells (the sources above are unchanged)
    ("violas",   "Viola Ensemble",  "Bowed",   r"Strings/Viola Section/susvib/ViolaEns_susvib_([A-G]#?\d)_(v1|v2)_1\.wav", ["v1", "v2"], 8.0, True),
    ("vlntrem",  "Violin Tremolo",  "Bowed",   r"Strings/Violin Section/Trem/VlnEns_Trem_([A-G]#?\d)_(v1|v2)\.wav", ["v1", "v2"], 8.0, True),
    ("celtrem",  "Cello Tremolo",   "Bowed",   r"Strings/Cello Section/trem/trem_([A-G]#?\d)_(v1|v2)_1\.wav", ["v1", "v2"], 8.0, True),
    ("oboe",     "Oboe",            "Breath",  r"Woodwinds/Oboe/Sus/Oboe_Sus_([A-G]#?\d)_(v1|v3)_Main\.wav", ["v1", "v3"], 8.0, True),
    ("mhorn",    "Muted Horn",      "Breath",  r"Brass/F Horn/mute/MOHorn_mute_([A-G]#?\d)_(v1|v2)_1\.wav", ["v1", "v2"], 8.0, True),
    ("trumpet",  "Trumpet",         "Breath",  r"Brass/Trumpet/sus/Sum_SHTrumpet_sus_([A-G]#?\d)_(v1|v3)_rr1\.wav", ["v1", "v3"], 8.0, True),
    ("trombone", "Trombone",        "Breath",  r"Brass/Tenor Trombone/sus/tenortbn_sus_([A-G]#?\d)_(v1|v2)_1\.wav", ["v1", "v2"], 8.0, True),
    ("tuba",     "Tuba",            "Breath",  r"Brass/Tuba/sus/Tuba3_sus_([A-G]#?\d)_(v1|v3)_rr1_Mid\.wav", ["v1", "v3"], 8.0, True),
    ("piano",    "Upright Piano",   "Struck",  r"Keys/Upright Piano/Player_rr1_(\d{3})_(dyn1|dyn3)\.wav", ["dyn1", "dyn3"], 8.0, True),
    ("organ",    "Pipe Organ",      "Keys",    r"Keys/Organ/Quiet/NT5_Man3Quiet_(1\d\d)()_rr1\.wav", [""], 8.0, True),
    ("tubular",  "Tubular Bells",   "Struck",  r"Percussion/TB_hit_([A-G]#?\d)_v[34]()_rr1\.wav", [""], 8.0, True),
    ("timproll", "Timpani Roll",    "Texture", r"Percussion/Timpani/Rolls/Timpani(\d)_Roll_(v[345])_rr1_Sum\.wav", None, 8.0, False),
    ("belltree", "Bell Tree",       "Texture", r"Percussion/BellTree_Stroke()([1-4])_v1_Sum\.wav", None, 6.0, False),
    ("cymswell", "Cymbal Swell",    "Texture", r"Percussion/susCymb1-cresc-()(Long|Median|Short)_v1\.wav", None, 8.0, False),
    ("longbells","Bell Drone",      "Texture", r"Miscellania Raw/Misc 2/NepaleseBells/fx_()(long_1|long2)_main\.wav", None, 10.0, False),
]

# Sources whose files are numbered rather than named by note
UPRIGHT_KEYS = [21, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 67, 69, 71, 73,
                75, 77, 79, 81, 83, 85, 87, 89, 91, 93, 95, 97, 99, 101, 103, 105, 107, 108]   # Upright Piano MappingChart.txt
NUMBERED = { "piano": lambda n: UPRIGHT_KEYS[int (n)],
             "organ": lambda n: int (n) - 86 }        # measured: 122 sounds C2, one number per semitone

NOTE = { "C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11 }
def midi (name):
    m = re.match (r"([A-G])(#?)(-?\d)", name)
    return 12 * (int (m.group (3)) + 1) + NOTE[m.group (1)] + (1 if m.group (2) else 0)

def run (*args):
    return subprocess.run (args, capture_output = True, text = True)

# ---- Pick the files ---------------------------------------------------------------------------
tree = json.loads (subprocess.run (["gh", "api", "repos/sgossner/VSCO-2-CE/git/trees/master?recursive=1"],
                                   capture_output = True, text = True, check = True).stdout)
paths = [t["path"] for t in tree["tree"] if t["type"] == "blob"]

plan = {}   # source id -> list of (path, root note, layer index, take index)
for sid, name, family, rx, layers, maxlen, pitched in SOURCES:
    items = []
    for p in paths:
        if sid == "piano":
            pm = re.fullmatch (r"Keys/Upright Piano/Player_(dyn1|dyn3)_rr1_(\d{3})\.wav", p)
            if pm:
                items.append ((p, NUMBERED["piano"] (pm.group (2)), layers.index (pm.group (1)), 0))
            continue
        m = re.fullmatch (rx, p)
        if not m:
            continue
        if pitched:
            note = NUMBERED[sid] (m.group (1)) if sid in NUMBERED else midi (m.group (1))
            items.append ((p, note, layers.index (m.group (2)), 0))
        elif m.group (2).isdigit():
            items.append ((p, 60, 0, int (m.group (2)) - 1))
        elif m.group (2) in ("pp", "mf"):
            items.append ((p, 60, 0, ["pp", "mf"].index (m.group (2))))
        else:
            items.append ((p, 60, 0, -1))     # numbered in order below
    items.sort (key = lambda x: (x[1], x[2], x[3], x[0]))
    if any (it[3] < 0 for it in items):
        items = [(p, n, l, i) for i, (p, n, l, _) in enumerate (sorted (items))]
    plan[sid] = items
    print (f"{sid:9s} {len (items):3d} files")

# ---- Download ---------------------------------------------------------------------------------
def fetch (p):
    dst = os.path.join (CACHE, p)
    if os.path.exists (dst) and os.path.getsize (dst) > 0:
        return
    os.makedirs (os.path.dirname (dst), exist_ok = True)
    r = run ("curl", "-sfL", "--retry", "3", "-o", dst, REPO + urllib.parse.quote (p))
    if r.returncode != 0:
        raise SystemExit (f"download failed: {p}")

allFiles = [it[0] for items in plan.values() for it in items]
with ThreadPoolExecutor (8) as ex:
    list (ex.map (fetch, allFiles))
print (f"downloaded {len (allFiles)} files")

# ---- Tuning -----------------------------------------------------------------------------------
# Most VSCO 2 CE files are named an octave below the pitch that sounds (measured: the named
# fundamental carries no energy, the octave above is the strongest partial). Semitones to add:
OFFSETS = { "cello": 12, "violins": 12, "solovln": 0, "bass": 12, "flute": 12, "clarinet": 12, "bassoon": 12,
            "horn": 12, "harp": 0, "bpizz": 12, "vpizz": 0, "marimba": 12, "glock": 12,
            "violas": 12, "vlntrem": 12, "celtrem": 12, "oboe": 12, "mhorn": 12, "trumpet": 12, "trombone": 12, "tuba": 12,
            "tubular": 12, "piano": 0, "organ": 0 }   # piano and organ: their numbering is already the sounding note
offsets = { sid: OFFSETS.get (sid, 0) for sid, *_ in SOURCES }

# ---- Convert ----------------------------------------------------------------------------------
os.makedirs (OUT, exist_ok = True)
for f in os.listdir (OUT):
    if f.endswith (".flac"):
        os.remove (os.path.join (OUT, f))

def peakDb (path):
    r = run ("ffmpeg", "-v", "info", "-i", path, "-af", "volumedetect", "-f", "null", "-")
    m = re.search (r"max_volume: (-?[\d.]+) dB", r.stderr)
    return float (m.group (1)) if m else 0.0

manifest = { "id": PACK_ID, "name": PACK_NAME, "version": PACK_VERSION,
             "credit": "VSCO 2 Community Edition by Versilian Studios (CC0 1.0)", "sources": [] }

for sid, name, family, rx, layers, maxlen, pitched in SOURCES:
    items = plan[sid]
    gain = -1.0 - max (peakDb (os.path.join (CACHE, p)) for p, *_ in items)
    zones = []
    def convert (it):
        p, note, layer, take = it
        root = note + offsets[sid]
        fname = f"{sid}_{root}_v{layer + 1}" + (f"_r{take + 1}" if not pitched else "") + ".flac"
        fade = 0.25 if pitched and family not in ("Struck", "Plucked") else 0.6
        af = (f"silenceremove=start_periods=1:start_threshold=-58dB:start_silence=0.004,"
              f"atrim=0:{maxlen},afade=t=out:st={maxlen - fade}:d={fade},volume={gain}dB,aresample=48000")
        r = run ("ffmpeg", "-v", "error", "-y", "-i", os.path.join (CACHE, p), "-af", af, "-ac", "2",
                 "-sample_fmt", "s16", "-c:a", "flac", "-compression_level", "8", os.path.join (OUT, fname))
        if r.returncode != 0:
            raise SystemExit (r.stderr)
        return { "file": fname, "root": root, "layer": layer, "take": take }
    with ThreadPoolExecutor (8) as ex:
        zones = list (ex.map (convert, items))
    manifest["sources"].append ({ "id": sid, "name": name, "family": family, "pitched": pitched,
                                  "layers": len (layers) if layers else 1, "zones": zones })

with open (os.path.join (OUT, "pack.json"), "w") as f:
    json.dump (manifest, f, indent = 1)

total = sum (os.path.getsize (os.path.join (OUT, f)) for f in os.listdir (OUT))
print (f"wrote {sum (len (s['zones']) for s in manifest['sources'])} files, {total / 1e6:.1f} MB")
