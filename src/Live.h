// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Collab.h"

namespace elsewhere
{
//==============================================================================
/**
    Live sessions: playing together over the internet.

    Nobody can play perfectly in sync across the internet, so Elsewhere uses the
    interval approach pioneered by NINJAM: everyone plays to the same tempo, and you
    hear each other exactly one interval (a few bars) late. A phrase your friend plays
    in bars 1-4 comes out of your speakers in your bars 5-8, still in time musically,
    so network delay stops mattering.

    How the pieces fit:
      - LiveTapPlugin sits on the master bus. It records your output one interval at a
        time and mixes in collaborators' previous interval, lined up to your bars.
      - Each interval is compressed (Ogg Vorbis) and sent straight to the other players
        over WebRTC data channels (peer-to-peer, encrypted, through home routers via ICE).
      - Connection setup ("signalling") uses the shared project folder: each side writes a
        small offer/answer file that Dropbox / Google Drive syncs to the other. There is
        no Elsewhere server; the only outside service is a public STUN server that tells
        each computer its public address.
*/
namespace Live
{
    inline const juce::Identifier barsPerIntervalProp { "elsewhereLiveBarsPerInterval" };
    inline constexpr const char* tapPluginType = "elsewhereLiveTap";

    /** Creates the master-bus plugin (used by the EngineBehaviour). */
    te::Plugin::Ptr createTapPlugin (const te::PluginCreationInfo&);

    /** Removes any leftover tap plugin from a loaded project. */
    void removeTapPlugins (te::Edit&);

    struct PeerStatus
    {
        juce::String userId, name;
        juce::Colour colour;
        juce::String state;         // "Connecting...", "Connected" ...
        bool connected = false;
        double roundTripMs = 0.0;
        juce::int64 lastIntervalReceived = -1;
        float level = 0.0f;         // peak of their audio, 0..1
        float gain = 1.0f;
        bool muted = false;
        double theirBpm = 0.0;
        juce::String theirMode;

        // Real-time mode
        double jitterBufferMs = 0.0, estimatedLatencyMs = 0.0;
        double bufferedMs = 0.0;        // what's actually waiting in the jitter buffer
        double clockDriftPpm = 0.0;     // correction for the difference between the two audio clocks
        int underruns = 0;
    };

    struct Stats
    {
        juce::int64 currentInterval = -1;
        double intervalProgress = 0.0;      // 0..1 through the current interval
        int intervalsSent = 0, intervalsReceived = 0;
        bool realtime = false;
    };

    //==============================================================================
    class Session
    {
    public:
        Session (Project&, Collab::Session&);
        ~Session();

        void start();
        void stop();
        bool isActive() const;

        /** Real-time (low latency) streaming instead of intervals. Stored in the project. */
        bool isRealtime() const;
        void setRealtime (bool);

        int getBarsPerInterval() const;
        void setBarsPerInterval (int bars);

        /** Clip/scene launches shared with the other players (stored in the project). */
        bool isSharingLaunches() const;
        void setSharingLaunches (bool);

        /** Sends a Session-view action (see Launcher::share) to everyone connected. */
        void sendLauncherAction (const juce::var&);

        /** Called on the message thread with a Session-view action from another player. */
        std::function<void (const juce::String& fromName, const juce::var&)> onLauncherAction;

        std::vector<PeerStatus> getPeers() const;
        Stats getStats() const;

        void setPeerGain (const juce::String& userId, float gain);
        void setPeerMuted (const juce::String& userId, bool muted);

        /** Called on the message thread whenever peers connect/disconnect. */
        std::function<void()> onPeersChanged;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

} // namespace elsewhere
