// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"
#include "AboutResources.h"

namespace elsewhere
{
//==============================================================================
/** Elsewhere > About Elsewhere (Help > About Elsewhere on Windows): the version, the copyright and
    licence, a link to this version's source code (the AGPL asks for one), the "Powered by
    Tracktion Engine" credit, and the third-party notices (resources/ThirdPartyNotices.txt). */
class AboutComponent : public juce::Component
{
public:
    AboutComponent()
    {
        icon = juce::ImageCache::getFromMemory (AboutResources::AppIcon_png, AboutResources::AppIcon_pngSize);

        for (auto* link : { &sourceLink, &tracktionLink, &juceLink })
        {
            link->setFont (juce::FontOptions (13.0f), false, juce::Justification::centred);
            link->setColour (juce::HyperlinkButton::textColourId, Colours::accent);
            addAndMakeVisible (*link);
        }

        notices.setMultiLine (true, false);
        notices.setReadOnly (true);
        notices.setCaretVisible (false);
        notices.setScrollbarsShown (true);
        notices.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
        notices.setColour (juce::TextEditor::backgroundColourId, Colours::panelDark);
        notices.setColour (juce::TextEditor::textColourId, Colours::textDim);
        notices.setColour (juce::TextEditor::outlineColourId, Colours::outline);
        notices.setColour (juce::TextEditor::focusedOutlineColourId, Colours::outline);
        notices.setText (juce::String::fromUTF8 (AboutResources::ThirdPartyNotices_txt, AboutResources::ThirdPartyNotices_txtSize), false);
        addAndMakeVisible (notices);

        setSize (600, 662);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panel);

        auto r = getLocalBounds().reduced (24).removeFromTop (headerHeight);
        g.drawImage (icon, r.removeFromTop (96).withSizeKeepingCentre (96, 96).toFloat(), juce::RectanglePlacement::centred);
        r.removeFromTop (10);

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
        g.drawText ("Elsewhere", r.removeFromTop (30), juce::Justification::centred);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Version " + juce::String (ELSEWHERE_VERSION), r.removeFromTop (20), juce::Justification::centred);
        g.drawText (juce::String::fromUTF8 ("\xc2\xa9 ") + juce::String (copyrightYear) + " " + ProjectInfo::companyName
                      + ". Free software under the GNU AGPL v3.", r.removeFromTop (20), juce::Justification::centred);

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("Third-party software", noticesTitle, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24);
        r.removeFromTop (headerHeight + 8);

        sourceLink.setBounds (r.removeFromTop (22));
        tracktionLink.setBounds (r.removeFromTop (22));
        juceLink.setBounds (r.removeFromTop (22));
        r.removeFromTop (14);

        noticesTitle = r.removeFromTop (22);
        notices.setBounds (r);
    }

    /** Opens the About window, or brings it to the front if it's already open. */
    static void show()
    {
        static juce::Component::SafePointer<juce::DialogWindow> window;

        if (window != nullptr)
        {
            window->toFront (true);
            return;
        }

        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "About Elsewhere";
        o.dialogBackgroundColour = Colours::panel;
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.content.setOwned (new AboutComponent());
        window = o.launchAsync();
    }

private:
    static constexpr int headerHeight = 96 + 10 + 30 + 20 + 20;
    static constexpr int copyrightYear = 2026;

    juce::Image icon;
    juce::HyperlinkButton sourceLink { "Source code for this version",
                                       juce::URL (juce::String (ELSEWHERE_SOURCE_URL) + "/tree/v" + ELSEWHERE_VERSION) },
                          tracktionLink { "Powered by Tracktion Engine", juce::URL ("https://www.tracktion.com/develop/tracktion-engine") },
                          juceLink { "Built with JUCE", juce::URL ("https://juce.com") };
    juce::Rectangle<int> noticesTitle;
    juce::TextEditor notices;
};
}
