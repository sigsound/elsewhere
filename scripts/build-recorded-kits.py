#!/usr/bin/env python3
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
"""
Builds Elsewhere Drums' recorded kits from Karoryfer's CC0 drum libraries.

  scripts/build-recorded-kits.py <folder with the unzipped libraries> resources/kits

The libraries (all CC0 1.0, https://shop.karoryfer.com/pages/free-samples):
  rusty/     Big Rusty Drums      (Big_Rusty_Drums_1100.zip)
  unruly/    Unruly Drums         (Unruly_Drums_1100.zip)
  gogodze2/  Gogodze Phu Vol II   (Karoryfer_Gogodze_Phu_vol_II.v1.001.zip)

For every kit and pad: up to three velocity layers x up to two takes (most detail where it
matters: kick, snare and closed hat get three layers and two takes), the mic
positions mixed to stereo, trimmed after the tail dies away, all layers of a pad scaled by
the same gain (so the dynamics stay), written as 16-bit FLAC named
  <kit>_<pad 00-15>_v<layer 1-3>_r<take 1-2>.flac
Needs ffmpeg and ffprobe.
"""
import glob, os, re, subprocess, sys, tempfile

LIB, OUT = sys.argv[1], sys.argv[2]
os.makedirs (OUT, exist_ok=True)

# Longest each slot is allowed to ring (seconds): kick, snare, clap, closed hat, open hat,
# low/mid/high tom, rim, cowbell, crash, ride, shaker, clave, 808, zap
MAXLEN = [1.2, 1.0, 0.8, 0.5, 1.6, 1.3, 1.2, 1.1, 0.5, 1.2, 2.6, 2.2, 0.6, 0.5, 1.5, 2.4]

# (velocity layers, takes) per slot: the most detail where it matters, small for the rest
PLAN = [(3, 2), (3, 2), (2, 2), (3, 2), (2, 1), (2, 1), (2, 1), (2, 1),
        (2, 2), (2, 1), (2, 1), (2, 1), (2, 2), (2, 2), (2, 1), (2, 1)]

# A source: (path pattern with {vl} and {rr}, gain, placement): C centre, L/R one side, S already stereo
def src (pattern, gain = 1.0, place = "C"):
    return (pattern, gain, place)

R, U, G = "rusty/Samples/", "unruly/Samples/", "gogodze2/Gogodze_Phu_vol_II/Samples/"

def rusty (piece, art, mics, name, gains = None, ext = "flac", layered = True):
    gains = gains or {}
    tail = "_vl{vl}_rr{rr}" if layered else "_rr{rr}"
    return [src (f"{R}{piece}/{art}/{m}/{name}{tail}.{ext}", gains.get (m, 1.0), "S" if m == "oh" else "C") for m in mics]

def unruly (piece, mics, name, gains = None):
    gains = gains or {}
    out = []
    for m in mics:
        place = "L" if m == "ohl" else "R" if m == "ohr" else "C"
        out.append (src (f"{U}{piece}/{m}/rr{{rr}}/{name}_vl{{vl}}_rr{{rr}}.flac", gains.get (m, 1.0), place))
    return out

def gogo (code, mics):
    return [src (f"{G}{m}_mic/{code}_vl{{vl}}_rr{{rr}}.wav", g) for m, g in mics]

# ---- The kits ----------------------------------------------------------------------------
KITS = {}

KITS["bigrusty"] = [
    rusty ("kick_24", "kick", ["kick", "oh"], "k", { "oh": 0.45 }),
    rusty ("snare_14", "center", ["top", "btm", "oh"], "sn_center", { "btm": 0.35, "oh": 0.5 }),
    rusty ("snare_14", "edge", ["top", "btm", "oh"], "sn_edge", { "btm": 0.3, "oh": 0.5 }),
    rusty ("hihat_14", "tc", ["cl", "oh"], "ht_tc", { "oh": 0.6 }),
    rusty ("hihat_14", "open", ["cl", "oh"], "ht_open", { "oh": 0.6 }),
    rusty ("tom_22", "center", ["cl", "oh"], "t22", { "oh": 0.5 }),
    rusty ("tom_18", "center", ["cl", "oh"], "t18", { "oh": 0.5 }),
    rusty ("tom_14", "center", ["cl", "oh"], "t14", { "oh": 0.5 }),
    rusty ("tom_14", "rc", ["cl", "oh"], "t14_rc", { "oh": 0.4 }),
    rusty ("ride_22", "bl", ["cl", "oh"], "rd_bl", { "oh": 0.5 }),
    rusty ("crash_17", "cr", ["cl", "oh"], "cr", { "oh": 0.8 }),
    rusty ("ride_22", "rd", ["cl", "oh"], "rd", { "oh": 0.6 }),
    rusty ("hihat_14", "chik", ["cl", "oh"], "ht_chik", { "oh": 0.6 }),
    rusty ("hihat_14", "perc", ["cl", "oh"], "ht_perc", { "oh": 0.6 }),
    rusty ("kick_24_nodamp", "kick", ["kick", "oh"], "k_nodamp", { "oh": 0.45 }),
    rusty ("china_18", "cn", ["cl", "oh"], "cn", { "oh": 0.8 }),
]

KITS["brushes"] = [
    rusty ("kick_24", "kick", ["kick", "oh"], "k", { "oh": 0.6 }),
    rusty ("snare_14", "brush", ["top", "btm", "oh"], "sn_b", { "btm": 0.3, "oh": 0.6 }),
    rusty ("snare_14", "bstir", ["top", "oh"], "sn_stir_dl2", { "oh": 0.6 }, "wav", False),
    rusty ("hihat_14", "tc_brush", ["cl", "oh"], "ht_tc_b", { "oh": 0.6 }),
    rusty ("hihat_14", "open_brush", ["cl", "oh"], "ht_open_b", { "oh": 0.6 }),
    rusty ("tom_22", "brush", ["cl", "oh"], "t22_b", { "oh": 0.6 }),
    rusty ("tom_18", "brush", ["cl", "oh"], "t18_b", { "oh": 0.6 }),
    rusty ("tom_14", "brush", ["cl", "oh"], "t14_b", { "oh": 0.6 }),
    rusty ("snare_14", "bflutter", ["top", "oh"], "sn_flutter", { "oh": 0.6 }, "wav", False),
    rusty ("ride_sizzle_19", "brush", ["cl", "oh"], "rds_b", { "oh": 0.6 }),
    rusty ("crash_17", "brush", ["cl", "oh"], "cr_b", { "oh": 0.8 }),
    rusty ("ride_22", "brush", ["cl", "oh"], "rd_b", { "oh": 0.6 }),
    rusty ("hihat_14", "lc_brush", ["cl", "oh"], "ht_lc_b", { "oh": 0.6 }),
    rusty ("snare_14", "brelease", ["top", "oh"], "sn_release", { "oh": 0.6 }, "flac", False),
    rusty ("kick_24_nodamp", "kick", ["kick", "oh"], "k_nodamp", { "oh": 0.6 }),
    rusty ("china_18", "brush", ["cl", "oh"], "cn_b", { "oh": 0.8 }),
]

def unrulyKit (room):
    close, oh = (0.55, 1.0) if room else (1.0, 0.0)
    def g (cl, extra = None):
        d = { "top": cl, "btm": cl * 0.3, "cl": cl, "in": cl, "out": cl * 0.5, "ohl": oh, "ohr": oh, "oh": oh }
        if extra: d.update (extra)
        return d
    snareMics = ["top", "btm"] + (["ohl", "ohr"] if room else [])
    hatMics = ["cl"] + (["ohl", "ohr"] if room else [])
    cymMics = ["cl"] + (["oh"] if room else [])
    kickMics = ["in", "out"] + (["oh"] if room else [])
    snare = "s22" if room else "s14"
    return [
        unruly ("k20", kickMics, "kick_clean", g (1.0)),
        unruly (snare, snareMics, f"{snare}_center", g (1.0)),
        unruly (snare, snareMics, f"{snare}_rimshot" if snare != "s22" else "s22_edge", g (1.0)),
        unruly ("h14", hatMics, "hh_tight_tip", g (1.0)),
        unruly ("h14", hatMics, "hh_open_tip", g (1.0)),
        unruly ("s22", snareMics, "s22_tom_clean", g (1.0)),
        unruly ("s14", snareMics, "s14_tom_clean", g (1.0)),
        unruly ("s13", snareMics, "s13_tom_clean", g (1.0)),
        unruly ("s14", snareMics, "s14_sstick", g (1.0)),
        unruly ("wb", cymMics, "woodblock_hi", g (1.0)),
        unruly ("c16", cymMics, "cr_edge", g (1.0)),
        unruly ("r20", cymMics, "ride_bow", g (1.0)),
        unruly ("h14", hatMics, "hh_footchik", g (1.0)),
        unruly ("wb", cymMics, "woodblock_lo", g (1.0)),
        unruly ("k20", kickMics, "kick_dirtiest", g (1.0)),
        unruly ("r20", cymMics, "ride_bell", g (1.0)),
    ]

KITS["studiodry"] = unrulyKit (False)
KITS["unrulyroom"] = unrulyKit (True)

def gogodzeKit (mix):
    m = mix
    return [
        gogo ("ks", m["kick"]), gogo ("sc", m["snare"]), gogo ("se", m["snare"]),
        gogo ("hc", m["hat"]), gogo ("ho", m["hat"]),
        gogo ("tl", m["tom"]), gogo ("tm", m["tom"]), gogo ("th", m["tom"]),
        gogo ("ss", m["snare"]), gogo ("hf", m["hat"]), gogo ("hl", m["hat"]), gogo ("hh", m["hat"]),
        gogo ("ht", m["hat"]), gogo ("ss", m["room"]), gogo ("ks", m["room"]), gogo ("sc", m["room"]),
    ]

KITS["seventies"] = gogodzeKit ({
    "kick":  [("kick", 1.0), ("front", 0.4), ("oh", 0.35)],
    "snare": [("snare", 1.0), ("oh", 0.5), ("front", 0.3)],
    "tom":   [("front", 0.7), ("oh", 0.6), ("side", 0.3)],
    "hat":   [("oh", 0.8), ("side", 0.5)],
    "room":  [("wndw", 1.0), ("oh", 0.4)],
})

KITS["retrotape"] = gogodzeKit ({
    "kick":  [("retro", 1.0), ("kick", 0.25)],
    "snare": [("retro", 1.0)],
    "tom":   [("retro", 1.0)],
    "hat":   [("retro", 1.0)],
    "room":  [("retro", 0.7), ("wndw", 0.6)],
})

# ---- Rendering -------------------------------------------------------------------------
def files (pattern, vl, rr):
    return glob.glob (os.path.join (LIB, pattern.format (vl = vl, rr = rr)))

def velocity_layers (sources, count):
    pat = sources[0][0]
    if "{vl}" not in pat:
        return [0] if glob.glob (os.path.join (LIB, pat.format (vl = 0, rr = "*"))) else []
    found = set()
    for f in glob.glob (os.path.join (LIB, pat.format (vl = "*", rr = "*"))):
        m = re.search (r"_vl(\d+)_rr", f)
        if m: found.add (int (m.group (1)))
    if not found: return []
    vls = sorted (found)
    n = len (vls)
    points = [0.35, 0.7, 1.0] if count >= 3 else [0.55, 1.0] if count == 2 else [1.0]
    return sorted ({ vls[max (0, round (n * p) - 1)] for p in points })

def channels (path):
    out = subprocess.run (["ffprobe", "-v", "error", "-show_entries", "stream=channels", "-of", "csv=p=0", path], capture_output = True, text = True).stdout
    return int (out.strip() or 1)

def render (sources, vl, rr, maxlen, dest):
    inputs, chains = [], []
    for pat, gain, place in sources:
        found = files (pat, vl, rr) or files (pat, vl, 1)
        if not found: continue
        idx = len (chains)
        inputs += ["-i", found[0]]
        st = channels (found[0]) > 1
        pan = ("pan=stereo|c0=c0|c1=c1" if st else
               "pan=stereo|c0=c0|c1=c0" if place == "C" else
               "pan=stereo|c0=c0|c1=0*c0" if place == "L" else
               "pan=stereo|c0=0*c0|c1=c0")
        chains.append (f"[{idx}:a]aresample=44100,{pan},volume={gain}[a{idx}]")
    if not inputs: return False
    labels = "".join (f"[a{i}]" for i in range (len (chains)))
    graph = ";".join (chains) + f";{labels}amix=inputs={len (chains)}:normalize=0,atrim=0:{maxlen},"
    graph += "areverse,silenceremove=start_periods=1:start_threshold=-64dB,areverse[out]"
    subprocess.run (["ffmpeg", "-v", "error", "-y", *inputs, "-filter_complex", graph, "-map", "[out]", "-c:a", "pcm_f32le", dest], check = True)
    return True

def peak_db (path):
    err = subprocess.run (["ffmpeg", "-hide_banner", "-i", path, "-af", "volumedetect", "-f", "null", "-"], capture_output = True, text = True).stderr
    m = re.search (r"max_volume: ([-\d.]+) dB", err)
    return float (m.group (1)) if m else 0.0

def duration (path):
    out = subprocess.run (["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path], capture_output = True, text = True).stdout
    return float (out.strip() or 0)

total = 0
with tempfile.TemporaryDirectory() as tmp:
    for kit, pads in KITS.items():
        for pad, sources in enumerate (pads):
            layerCount, takes = PLAN[pad]
            layers = velocity_layers (sources, layerCount)
            if not layers:
                print (f"  ! {kit} pad {pad}: no samples found for {sources[0][0]}")
                continue
            temps = []
            for li, vl in enumerate (layers):
                for take in range (1, takes + 1):
                    t = os.path.join (tmp, f"{kit}_{pad}_{li}_{take}.wav")
                    if render (sources, vl, take, MAXLEN[pad], t):
                        temps.append ((li + 1, take, t))
            if not temps: continue
            gain = -1.0 - max (peak_db (t) for l, _, t in temps if l == len (layers))
            for layer, take, t in temps:
                d = duration (t)
                dest = os.path.join (OUT, f"{kit}_{pad:02d}_v{layer}_r{take}.flac")
                fade = min (0.03, d * 0.2)
                subprocess.run (["ffmpeg", "-v", "error", "-y", "-i", t, "-af", f"volume={gain}dB,afade=t=out:st={max (0, d - fade)}:d={fade}",
                                 "-c:a", "flac", "-sample_fmt", "s16", "-compression_level", "12", dest], check = True)
                total += os.path.getsize (dest)
            print (f"  {kit} pad {pad:2d}: {len (layers)} layers, {len (temps)} files")

print (f"Done: {total / 1048576:.1f} MB")
