// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"
#include "AboutResources.h"

namespace elsewhere
{
//==============================================================================
/** The Elsewhere wordmark (resources/Wordmark.svg), drawn in any colour: the SVG is a single
    black shape, recoloured to the theme's ink so it reads in light and dark. */
namespace Wordmark
{
    /** Width over height of the wordmark, from the SVG's view box. */
    inline constexpr float aspect = 1807.0f / 183.0f;

    /** Draws the wordmark as large as fits in the area (keeping its proportions), placed by justification. */
    inline void draw (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour,
                      juce::RectanglePlacement placement = juce::RectanglePlacement::centred)
    {
        static std::unique_ptr<juce::Drawable> drawable;
        static juce::Colour drawnIn = juce::Colours::black;

        if (drawable == nullptr)
        {
            drawable = juce::Drawable::createFromImageData (AboutResources::Wordmark_svg, AboutResources::Wordmark_svgSize);
            drawnIn = juce::Colours::black;

            if (drawable == nullptr)
                return;
        }

        if (colour != drawnIn)
        {
            drawable->replaceColour (drawnIn, colour);
            drawnIn = colour;
        }

        drawable->drawWithin (g, area, placement, 1.0f);
    }
}
}
