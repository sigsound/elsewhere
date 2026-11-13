// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"
#include "AboutResources.h"
#include <map>
#include <tuple>

namespace elsewhere
{
//==============================================================================
/** The Elsewhere wordmark: the letters caught mid-orbit around the mark, each smeared a little along
    its path with a hint of colour at the edges. It's a picture, one for light themes
    (resources/WordmarkLight.png) and one for dark (resources/WordmarkDark.png), drawn in the
    theme's own ink. The motion trail spills a little outside the letters, mostly above them. */
namespace Wordmark
{
    /** Where the letters sit in the images (both are the same size), in image pixels. */
    inline constexpr float imageW = 1582.0f, imageH = 224.0f;
    inline constexpr float lettersX = 5.26f, lettersY = 50.57f, lettersW = 1572.57f, lettersH = 160.0f;

    /** Width over height of the letters. */
    inline constexpr float aspect = lettersW / lettersH;

    /** The source picture for a dark background (pale ink) or a light one. */
    inline const juce::Image& source (bool onDark)
    {
        static const juce::Image dark  = juce::ImageCache::getFromMemory (AboutResources::WordmarkDark_png,  AboutResources::WordmarkDark_pngSize);
        static const juce::Image light = juce::ImageCache::getFromMemory (AboutResources::WordmarkLight_png, AboutResources::WordmarkLight_pngSize);
        return onDark ? dark : light;
    }

    /** The picture shrunk to an exact pixel size, halving first so fine detail averages out
        instead of shimmering. Kept for each size asked for (there are only a couple). */
    inline juce::Image sized (bool onDark, int w, int h)
    {
        static std::map<std::tuple<bool, int, int>, juce::Image> cache;
        auto& image = cache[{ onDark, w, h }];

        if (! image.isValid())
        {
            auto img = source (onDark);

            while (img.isValid() && img.getWidth() >= 2 * w)
                img = img.rescaled (img.getWidth() / 2, img.getHeight() / 2, juce::Graphics::highResamplingQuality);

            image = img.isValid() ? img.rescaled (w, h, juce::Graphics::highResamplingQuality) : img;
        }

        return image;
    }

    /** Draws the wordmark with its letters as large as fit in the area (keeping their proportions),
        placed by justification. The colour picks the version: pale ink means a dark theme. */
    inline void draw (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour,
                      juce::RectanglePlacement placement = juce::RectanglePlacement::centred)
    {
        const bool onDark = colour.getPerceivedBrightness() > 0.5f;
        auto letters = placement.appliedTo (juce::Rectangle<float> (lettersW, lettersH), area);
        const float s = letters.getHeight() / lettersH;
        juce::Rectangle<float> whole (letters.getX() - lettersX * s, letters.getY() - lettersY * s, imageW * s, imageH * s);

        const float pixels = g.getInternalContext().getPhysicalPixelScaleFactor();
        const int w = juce::jmax (1, juce::roundToInt (whole.getWidth() * pixels));
        const int h = juce::jmax (1, juce::roundToInt (whole.getHeight() * pixels));

        g.setOpacity (1.0f);
        g.drawImage (sized (onDark, w, h), whole, juce::RectanglePlacement::stretchToFit);
    }
}
}
