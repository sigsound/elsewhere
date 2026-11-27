# Free and Pro

Elsewhere launches free and open source, the way Blender is for 3D: the whole DAW, for everyone, with no
locked features. Money comes later from **Elsewhere Pro, $5 a month**, and from **Somewhere**, sold
separately ($4.99 on iOS). This page says which features stay free, what Pro is for, and why.

## The rule

**Everything in the app stays free. Pro pays for things that cost Fuzz LLC money to run.**

Three reasons:

1. **The licence.** Elsewhere is AGPL-3.0 (see [LICENSING.md](LICENSING.md)). Anyone can build it from
   source, and a fork can delete a licence check in one line. A feature locked inside the app isn't really
   locked, and trying makes the open-source story look hollow.
2. **The positioning.** "Blender for musicians" only works if the free version is the real thing. Blender
   has no paid tier at all; people pay because they love it and because it saves them time, not because a
   button is greyed out.
3. **Growth.** The early goal is community (Discord, shared songs, people inviting collaborators). Every
   collaborator who needs to pay before they can open a shared project is a lost user. Collaboration has to
   be free on both ends.

Hosted services are different: a server, storage or bandwidth costs money every month, a fork doesn't get
them for free either, and charging for them feels fair.

## Stays free: everything Elsewhere does today

| Area | Features | Why free |
|---|---|---|
| Making music | Session and Arrangement views, piano roll, clip editor, normalize, recording, Capture MIDI, automation, return and group tracks, mixer | The core of a DAW. Gating any of it breaks "the real thing, free". |
| Sounds and devices | Mono, Poly, Modular, Haze, Organic, Drums, Arpeggiator, Step Sequencer, EQ, Compressor, Reverb, Tape Echo, Amp, Master, Somewhere Control, LFO, VST3/AU hosting, the CC0 sound packs | All in the AGPL source. The packs are CC0, so they can't be restricted anyway. |
| Drum Designer | Sections, styles, feel pad, fills, Convert to MIDI | A headline feature for new users and for marketing. |
| Finishing | Export (all formats, stems, loudness targets), batch export, Reference, Match Level, Mastering projects, AI-training rights reservation | People need to get their music out. Rights reservation is a values feature; charging for it would look bad. |
| Projects | Self-contained folders, Version History, project templates, Pro Tools import | Local files, no running cost. |
| Working together | Sharing through Dropbox, iCloud Drive or Google Drive, live sync, merging, presence, comments | The reason Elsewhere exists, and it runs on the user's own cloud storage, so it costs us nothing. Both people must be able to join free. |
| Live jamming | Interval mode, low-latency mode, shared Session view, recording a live session, direct peer-to-peer connections | Runs peer-to-peer with only Google's free STUN server. No cost to us. |
| Somewhere in Elsewhere | Somewhere Link, Beats from Somewhere, Somewhere Inbox, TAKE, MONITOR | Somewhere is the paid product here. Elsewhere's side should be free so that buying Somewhere is the only step. |
| Accessibility | Keyboard use, screen readers, contrast and motion settings | Never a paid extra. |

## Pro, $5 a month: hosted services that don't exist yet

Pro is built from new, hosted things, not from what's already in the app. In rough order of how soon
they're needed:

| Pro feature | Free version | Why it's Pro |
|---|---|---|
| **Relay for live jamming** (the "Next" item in the README roadmap) | A generous monthly allowance of relayed time, enough for a first few sessions | 10–20% of home networks block direct connections, and a relay carries real bandwidth. A first jam that fails is the worst possible first impression, so free users get some relay; regular players on blocked networks pay. Only one person in a session needs Pro for the session to use the relay. |
| **Sharing links** (a web page to listen to a song or mix, with comments) | Public links for finished songs, with a cap on how many are live at once, in a good compressed format | Sharing links are how Elsewhere spreads, so free has to have them. Pro adds what working musicians need: private and password-protected links, lossless audio, more live links, longer storage, listener comments on a timeline, and download counts. |
| **Hosted project sync** (no Dropbox needed) | Sharing through the user's own Dropbox, iCloud Drive or Google Drive, as now | Storage and sync cost money. It's a convenience, not a requirement, since folder sharing keeps working. |
| **Cloud backup of Version History** | Version History stays local, as now | Storage cost. |
| **Supporter perks** | — | A Pro badge in the app's presence chips and in Discord, early test builds, a vote on the roadmap. Costs nothing and gives people who pay for Elsewhere's values a visible reason to. |

Things to keep out of Pro:

- **Somewhere.** It stays its own purchase. Bundling it into a subscription runs into App Store rules and
  muddies two simple prices.
- **The paid plug-ins** (closed source, see [LICENSING.md](LICENSING.md)). Sell them on their own at first.
  Including them in Pro later is a good way to make Pro worth more without touching the free DAW.
- **Anything a collaborator needs to open, play or edit a shared project.** Only the person who wants the
  extra service should ever need to pay.

## What this means for launch

- Launch says "free and open source", with no Pro mention beyond "Pro, with hosted extras, is coming". The
  site already leads with "Free and open source".
- Build the relay and sharing links first. They are the two services the community plan (Discord and
  sharing links) needs, and the two that make Pro worth $5.
- Put the hosted services in their own service code (the relay, the link server), outside this repository's
  app code, so the AGPL app stays complete on its own and the services can be priced on their own.
- Keep a supporter route open for people who just want to fund development, as Blender's Development Fund
  does.

## Open questions

- How big the free relay and sharing-link allowances are. Set them once the services exist and their real
  costs are known.
- A yearly price (for example $50 a year).
- Whether Somewhere on Android is priced the same as on iOS.
