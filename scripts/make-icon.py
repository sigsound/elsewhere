#!/usr/bin/env python3
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Makes the app icon (resources/AppIcon.png, 1024x1024) from a square logo.

Usage: scripts/make-icon.py [logo.png]      (default: resources/Logo.png)

The logo is placed on a white rounded square with a soft shadow, sized to Apple's
macOS icon grid, so it sits nicely next to other apps in the Dock. If you already
have a finished Mac icon, skip this and just replace resources/AppIcon.png.
Rebuild afterwards (Xcode or scripts/make-dmg.sh) to pick up the new icon.
Needs Pillow: pip install pillow
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter

root = os.path.join(os.path.dirname(__file__), "..")
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "resources", "Logo.png")

S = 1024
inset, radius = 100, 185            # 824x824 rounded square, like Apple's template
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))

shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(shadow).rounded_rectangle((inset, inset + 12, S - inset, S - inset + 12), radius, fill=(0, 0, 0, 90))
img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18)))

mask = Image.new("L", (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((inset, inset, S - inset, S - inset), radius, fill=255)
img.paste(Image.new("RGBA", (S, S), (255, 255, 255, 255)), (0, 0), mask)

# Logo fills most of the square (its own white margin provides the padding)
logo = Image.open(src).convert("RGBA")
size = S - 2 * inset
logo = logo.resize((size, size), Image.LANCZOS)
tile = Image.new("RGBA", (S, S), (0, 0, 0, 0))
tile.paste(logo, (inset, inset))
img.paste(tile, (0, 0), Image.composite(tile.getchannel("A"), Image.new("L", (S, S), 0), mask))

out = os.path.join(root, "resources", "AppIcon.png")
img.save(out)
print("Wrote", os.path.normpath(out))
