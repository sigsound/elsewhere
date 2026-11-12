// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Live.h"
#include "EditViewState.h"
#include "Theme.h"

namespace elsewhere
{
//==============================================================================
/** The "Live" tab: start/stop, interval length, the interval countdown, and each player. */
class LivePanel : public juce::Component, private juce::Timer
{
public:
    LivePanel (EditViewState& evs, Live::Session& s, Collab::Session& c) : viewState (evs), live (s), collab (c)
    {
        startButton.onClick = [this]
        {
            if (live.isActive())
                live.stop();
            else if (collab.isActive())
                live.start();
            else if (onNeedsSharing)
                onNeedsSharing();

            refresh();
        };
        addAndMakeVisible (startButton);

        for (int bars : { 1, 2, 4, 8, 16 })
            intervalBox.addItem (juce::String (bars) + (bars == 1 ? " bar" : " bars"), bars);

        intervalBox.setSelectedId (live.getBarsPerInterval(), juce::dontSendNotification);

        modeBox.addItem ("Interval (in time, bars late)", 1);
        modeBox.addItem ("Low latency (real time)", 2);
        modeBox.setSelectedId (live.isRealtime() ? 2 : 1, juce::dontSendNotification);
        modeBox.onChange = [this] { live.setRealtime (modeBox.getSelectedId() == 2); refresh(); repaint(); };
        modeBox.setTooltip ("Interval: always in time, you hear each other a few bars late (works over any distance).\n"
                            "Low latency: you hear each other almost immediately (best within ~1000 km, wired internet).");
        addAndMakeVisible (modeBox);
        addAndMakeVisible (modeLabel);
        modeLabel.attachToComponent (&modeBox, true);
        intervalBox.onChange = [this] { live.setBarsPerInterval (intervalBox.getSelectedId()); };
        addAndMakeVisible (intervalBox);
        addAndMakeVisible (intervalLabel);
        intervalLabel.attachToComponent (&intervalBox, true);

        shareLaunches.setButtonText ("Share clip launches");
        shareLaunches.setTooltip ("When on, launching a clip or scene in the Session view launches it for everyone, on their next bar");
        shareLaunches.onClick = [this] { live.setSharingLaunches (shareLaunches.getToggleState()); };
        addAndMakeVisible (shareLaunches);

        live.onPeersChanged = [this] { rebuildPeers(); };
        rebuildPeers();
        refresh();
        startTimerHz (20);
    }

    ~LivePanel() override { live.onPeersChanged = nullptr; }

    std::function<void()> onNeedsSharing;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panelDark);
        auto r = getLocalBounds().reduced (10);
        r.removeFromTop (30);

        // Explanation / status
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (12.0f));
        auto bars = live.getBarsPerInterval();
        juce::String text;

        if (! collab.isActive())
            text = "Live sessions work in shared projects. Turn on sharing (File > Share Project...) and put the project in "
                   "Google Drive or Dropbox; when your collaborator opens it and also goes live, you'll connect automatically.";
        else if (! live.isActive())
            text = "Go live to play together. Everyone plays to the same tempo and hears each other exactly one interval ("
                   + juce::String (bars) + (bars == 1 ? " bar" : " bars") + ") late, so internet delay doesn't matter.";
        else if (live.getPeers().empty())
            text = "You're live. Waiting for collaborators to go live in this project... (connecting can take up to a "
                   "minute while the cloud folder syncs)";
        else if (live.isRealtime())
            text = "Low latency: you hear each other almost immediately, like being in the same room. It feels natural "
                   "under about 30 ms; a wired connection and a small audio buffer (128 samples) help.";
        else
            text = "Press play and jam. You hear everyone " + juce::String (bars) + (bars == 1 ? " bar" : " bars")
                   + " late, in time with your bars. They hear you the same way.";

        g.drawFittedText (text, r.removeFromTop (34), juce::Justification::topLeft, 2);

        // Interval countdown
        if (live.isActive() && ! live.isRealtime())
        {
            auto stats = live.getStats();
            auto bar = r.removeFromTop (26).reduced (0, 3);
            g.setColour (Colours::panel);
            g.fillRoundedRectangle (bar.toFloat(), 4.0f);

            auto* ts = viewState.edit.tempoSequence.getTimeSig (0);
            auto beats = bars * ts->numerator.get();
            auto filled = (int) std::floor (stats.intervalProgress * beats);

            for (int i = 0; i < beats; ++i)
            {
                auto cellW = (float) bar.getWidth() / (float) beats;
                auto cell = bar.toFloat().withWidth (cellW).translated ((float) i * cellW, 0.0f).reduced (1.5f);
                bool downbeat = i % ts->numerator.get() == 0;
                g.setColour (i <= filled && viewState.edit.getTransport().isPlaying()
                                ? (downbeat ? Colours::accent : Colours::accent.withAlpha (0.6f))
                                : Colours::panelLight.withAlpha (downbeat ? 1.0f : 0.6f));
                g.fillRoundedRectangle (cell, 2.0f);
            }

            r.removeFromTop (4);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("Interval " + juce::String (juce::jmax ((juce::int64) 0, stats.currentInterval + 1))
                          + "   -   sent " + juce::String (stats.intervalsSent) + ", received " + juce::String (stats.intervalsReceived),
                        r.removeFromTop (16), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        auto top = r.removeFromTop (26);
        shareLaunches.setBounds (top.removeFromRight (170));
        startButton.setBounds (top.removeFromLeft (180));
        top.removeFromLeft (60);
        modeBox.setBounds (top.removeFromLeft (220));
        top.removeFromLeft (80);
        intervalBox.setBounds (top.removeFromLeft (100));
        intervalBox.setVisible (! live.isRealtime());

        // Room for the explanation, plus the interval countdown in interval mode
        r.removeFromTop (4 + 34 + (live.isActive() && ! live.isRealtime() ? 26 + 4 + 16 : 0) + 8);

        for (auto* row : rows)
            row->setBounds (r.removeFromTop (30));
    }

private:
    //==============================================================================
    struct PeerRow : public juce::Component
    {
        PeerRow (Live::Session& s, Live::PeerStatus st) : live (s), status (std::move (st))
        {
            volume.setSliderStyle (juce::Slider::LinearBar);
            volume.setRange (0.0, 1.5, 0.01);
            volume.setValue (status.gain, juce::dontSendNotification);
            volume.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
            volume.onValueChange = [this] { live.setPeerGain (status.userId, (float) volume.getValue()); };
            addAndMakeVisible (volume);

            mute.setClickingTogglesState (true);
            mute.setToggleState (status.muted, juce::dontSendNotification);
            mute.setColour (juce::TextButton::buttonOnColourId, Colours::mute);
            mute.onClick = [this] { live.setPeerMuted (status.userId, mute.getToggleState()); };
            addAndMakeVisible (mute);
        }

        void update (const Live::PeerStatus& s, double ourBpm)
        {
            status = s;
            level = juce::jmax (s.level, level * 0.85f);
            bpmMismatch = s.theirBpm > 0.0 && std::abs (s.theirBpm - ourBpm) > 0.01;
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (0, 2);
            g.setColour (Colours::panel);
            g.fillRoundedRectangle (r.toFloat(), 3.0f);
            r.reduce (8, 0);

            g.setColour (status.colour.isTransparent() ? Colours::textDim : status.colour);
            g.fillEllipse (r.removeFromLeft (12).withSizeKeepingCentre (12, 12).toFloat());
            r.removeFromLeft (8);

            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (status.name.isNotEmpty() ? status.name : juce::String ("Someone"), r.removeFromLeft (120), juce::Justification::centredLeft, true);

            g.setColour (status.connected ? Colours::meterGreen : Colours::textDim);
            g.setFont (juce::FontOptions (11.5f));
            juce::String detail = status.state;

            if (status.connected && status.estimatedLatencyMs > 0.0)
            {
                detail << "  -  ~" << juce::roundToInt (status.estimatedLatencyMs) << " ms delay (buffer "
                       << juce::roundToInt (status.jitterBufferMs) << " ms)";
                if (std::abs (status.clockDriftPpm) >= 1.0)
                    detail << "  -  clock " << (status.clockDriftPpm > 0 ? "+" : "") << juce::roundToInt (status.clockDriftPpm) << " ppm";
            }
            else if (status.connected)
                detail << "  -  " << juce::roundToInt (status.roundTripMs) << " ms";

            if (bpmMismatch)
                detail << "  -  their tempo is " << juce::String (status.theirBpm, 1) << " BPM!";

            g.drawText (detail, r.removeFromLeft (330), juce::Justification::centredLeft, true);

            // Their level
            auto meter = r.removeFromLeft (120).withSizeKeepingCentre (110, 8).toFloat();
            g.setColour (Colours::panelDark);
            g.fillRect (meter);
            g.setColour (Colours::meterGreen);
            g.fillRect (meter.withWidth (meter.getWidth() * juce::jlimit (0.0f, 1.0f, level)));
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (8, 5);
            mute.setBounds (r.removeFromRight (26));
            r.removeFromRight (6);
            volume.setBounds (r.removeFromRight (140));
        }

        Live::Session& live;
        Live::PeerStatus status;
        juce::Slider volume;
        juce::TextButton mute { "M" };
        float level = 0.0f;
        bool bpmMismatch = false;
    };

    EditViewState& viewState;
    Live::Session& live;
    Collab::Session& collab;
    juce::TextButton startButton;
    juce::ComboBox intervalBox;
    juce::Label intervalLabel { {}, "Interval" }, modeLabel { {}, "Mode" };
    juce::ComboBox modeBox;
    juce::ToggleButton shareLaunches;
    juce::OwnedArray<PeerRow> rows;
    int lastLayoutKey = -1;

    void rebuildPeers()
    {
        rows.clear();

        for (auto& p : live.getPeers())
            addAndMakeVisible (rows.add (new PeerRow (live, p)));

        resized();
        repaint();
    }

    void refresh()
    {
        startButton.setButtonText (live.isActive() ? "Leave Live Session" : "Go Live");
        startButton.setColour (juce::TextButton::buttonColourId, live.isActive() ? Colours::record.withAlpha (0.6f) : Colours::accent.withAlpha (0.8f));
        intervalBox.setSelectedId (live.getBarsPerInterval(), juce::dontSendNotification);
        modeBox.setSelectedId (live.isRealtime() ? 2 : 1, juce::dontSendNotification);
        shareLaunches.setToggleState (live.isSharingLaunches(), juce::dontSendNotification);

        auto layoutKey = (live.isActive() ? 1 : 0) + (live.isRealtime() ? 2 : 0);

        if (layoutKey != lastLayoutKey)
        {
            lastLayoutKey = layoutKey;
            resized();
        }
    }

    void timerCallback() override
    {
        auto peers = live.getPeers();

        if (peers.size() != (size_t) rows.size())
            return rebuildPeers();

        auto bpm = viewState.edit.tempoSequence.getTempo (0)->getBpm();

        for (size_t i = 0; i < peers.size(); ++i)
            rows[(int) i]->update (peers[i], bpm);

        refresh();
        repaint (getLocalBounds().withHeight (130));
    }
};

} // namespace elsewhere
