#!/usr/bin/env python3
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Makes the README screenshots (docs/images/screenshots/*.webp) from the demo song.

Usage: scripts/make-screenshots.py [path to Elsewhere.app]
       (default: build/Elsewhere_artefacts/Release/Elsewhere.app)

Runs Elsewhere with --showcase, which builds "Night Drive" (src/Showcase.h) and saves each main
view in the dark and light themes, then scales them to 2000 pixels wide and saves them as WebP,
which keeps the repository small. Build the app first. Needs Pillow: pip install pillow
"""
import os, subprocess, sys, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/Elsewhere_artefacts/Release/Elsewhere.app")
OUT = os.path.join(ROOT, "docs/images/screenshots")
VIEWS = ["arrangement", "devices", "drums", "comments", "session", "mixer"]

with tempfile.TemporaryDirectory() as tmp:
    shots = os.path.join(tmp, "shots")
    subprocess.run([os.path.join(APP, "Contents/MacOS/Elsewhere"), "--no-audio", "--showcase", shots],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=180)
    os.makedirs(OUT, exist_ok=True)

    for view in VIEWS:
        for suffix, theme in (("", "dark"), ("_light", "light")):
            src = os.path.join(shots, view + suffix + ".png")
            if not os.path.exists(src):
                sys.exit("Missing " + src + ": did Elsewhere run?")
            im = Image.open(src).convert("RGB")
            im = im.resize((2000, round(im.height * 2000 / im.width)), Image.LANCZOS)
            dst = os.path.join(OUT, f"{view}-{theme}.webp")
            im.save(dst, "WEBP", quality=88, method=6)
            print(f"{os.path.relpath(dst, ROOT)}  {os.path.getsize(dst) // 1024} KB")
