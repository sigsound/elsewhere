// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Which main view is showing (Arrangement, Session or Mixer) and which panels are open around
    it. Every panel is a toggle in the View menu, and how they're left is remembered across
    projects.

    Starts as: Track List | Arrangement, with the clip / device panel below. The browser, the
    clip & marker list (right) and the Session grid's scenes are there when wanted.
*/
namespace ViewLayout
{
    enum class MainView { arrangement, session, mixer };

    inline juce::String getName (MainView v)
    {
        switch (v)
        {
            case MainView::arrangement:  return "Arrangement";
            case MainView::session:      return "Session";
            case MainView::mixer:        return "Mixer";
        }

        return {};
    }

    struct Layout
    {
        MainView view = MainView::arrangement;
        MainView lastNonMixerView = MainView::arrangement;   // where the Mixer button goes back to
        bool trackList = true, browser = false, detail = true, scenes = false, trackIO = false, clipList = false;
        int detailHeight = 240;
    };

    inline juce::String key (const char* what)      { return juce::String ("layout.") + what; }

    inline Layout load (juce::PropertiesFile& props)
    {
        Layout l;
        l.view = (MainView) juce::jlimit (0, 2, props.getIntValue (key ("view"), (int) l.view));
        l.lastNonMixerView = l.view == MainView::mixer ? MainView::arrangement : l.view;
        l.trackList = props.getBoolValue (key ("trackList"), l.trackList);
        l.browser = props.getBoolValue (key ("browser"), l.browser);
        l.detail = props.getBoolValue (key ("detail"), l.detail);
        l.scenes = props.getBoolValue (key ("scenes"), l.scenes);
        l.trackIO = props.getBoolValue (key ("trackIO"), l.trackIO);
        l.clipList = props.getBoolValue (key ("clipList"), l.clipList);
        l.detailHeight = props.getIntValue (key ("detailHeight"), l.detailHeight);
        return l;
    }

    inline void save (juce::PropertiesFile& props, const Layout& l)
    {
        props.setValue (key ("view"), (int) l.view);
        props.setValue (key ("trackList"), l.trackList);
        props.setValue (key ("browser"), l.browser);
        props.setValue (key ("detail"), l.detail);
        props.setValue (key ("scenes"), l.scenes);
        props.setValue (key ("trackIO"), l.trackIO);
        props.setValue (key ("clipList"), l.clipList);
        props.setValue (key ("detailHeight"), l.detailHeight);
    }
}

} // namespace elsewhere
