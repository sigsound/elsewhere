// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include <SoundCatalog.h>

namespace elsewhere
{
//==============================================================================
/**
    Optional sound packs (like Logic's additional content): downloaded once into the user's
    library folder and shared by every project and device.

        <library>/<pack id>/pack.json       name, credit and the list of sources
        <library>/<pack id>/<name>.flac      the samples (48 kHz stereo)

    A source is one playable sound (an instrument or a texture): zones with a root note, a
    dynamic layer and a take. Devices refer to sources by key, "<pack id>/<source id>", and
    store that key in their state, so projects open with the same sounds on any machine that
    has the pack (and fall back gracefully on one that hasn't).
*/
namespace SoundLibrary
{
    inline juce::File root()
    {
        auto env = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_SOUNDS_DIR", {});
        if (env.isNotEmpty())
            return juce::File (env);

        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        return base.getChildFile ("Application Support/Elsewhere/Sounds");
       #else
        return base.getChildFile ("Elsewhere/Sounds");
       #endif
    }

    struct PackInfo
    {
        juce::String id, name, description, url;
        int sizeMB = 0;
        int version = 1;    // the version the catalog offers; an older installed pack can update
    };

    /** The packs Elsewhere knows how to download: those with a download URL in sounds/catalog.json,
        the catalog shared with Somewhere (built into the app). */
    inline const std::vector<PackInfo>& catalog()
    {
        static const std::vector<PackInfo> all = []
        {
            std::vector<PackInfo> packs;
            auto json = juce::JSON::parse (juce::String::fromUTF8 (SoundCatalog::catalog_json, SoundCatalog::catalog_jsonSize));

            if (auto* list = json["packs"].getArray())
                for (auto& p : *list)
                    if (p["url"].toString().isNotEmpty())
                        packs.push_back ({ p["id"].toString(), p["name"].toString(), p["description"].toString(),
                                           p["url"].toString(), (int) p["sizeMB"], p.hasProperty ("version") ? (int) p["version"] : 1 });
            return packs;
        }();
        return all;
    }

    inline const PackInfo* findPack (const juce::String& id)
    {
        for (auto& p : catalog())
            if (p.id == id)
                return &p;
        return nullptr;
    }

    inline juce::File packFolder (const juce::String& id)   { return root().getChildFile (id); }
    inline bool isInstalled (const juce::String& id)        { return packFolder (id).getChildFile ("pack.json").existsAsFile(); }

    inline juce::var readManifest (const juce::String& id);

    /** The installed pack's version (0 if it isn't installed). */
    inline int installedVersion (const juce::String& id)
    {
        if (! isInstalled (id)) return 0;
        auto v = readManifest (id)["version"];
        return v.isVoid() ? 1 : (int) v;
    }

    /** True when the catalog offers a newer version than the one installed. */
    inline bool hasUpdate (const juce::String& id)
    {
        auto* p = findPack (id);
        return p != nullptr && isInstalled (id) && installedVersion (id) < p->version;
    }

    inline juce::var readManifest (const juce::String& id)
    {
        return juce::JSON::parse (packFolder (id).getChildFile ("pack.json"));
    }

    //==============================================================================
    /** A playable sound: zones of audio with a root note, dynamic layer and take. */
    struct Zone
    {
        int root = 60, layer = 0, take = 0;
        juce::AudioBuffer<float> audio;     // stereo, at Source::rate
    };

    struct Source
    {
        juce::String key, name, family;
        bool pitched = true;
        int layers = 1, takes = 1;
        double rate = 48000.0;
        float gain = 1.0f;                  // loudness normalisation across sources
        std::vector<Zone> zones;
        std::vector<float> overview;        // amplitude outline of a typical zone, for drawing

        /** Sustaining sounds (bowed, blown, textures) hold while the key is down; struck and
            plucked ones ring out. */
        bool sustains() const   { return family != "Plucked" && family != "Struck"; }

        /** The zone to play for a note, in a dynamic layer, for a take (round robin). */
        const Zone* pick (int note, int layer, int take) const
        {
            const Zone* best = nullptr;
            int bestScore = std::numeric_limits<int>::max();
            for (auto& z : zones)
            {
                // Prefer the right layer and take; then the closest root, slightly preferring
                // roots below the note (shifting a sample up sounds more natural than down)
                int score = std::abs (z.root - note) * 4 + (z.root > note ? 1 : 0);
                if (z.layer != layer) score += 2000 + std::abs (z.layer - layer) * 100;
                if (z.take != take % juce::jmax (1, takes)) score += 500;
                if (score < bestScore) { bestScore = score; best = &z; }
            }
            return best;
        }

        void finish()
        {
            for (auto& z : zones)
            {
                layers = juce::jmax (layers, z.layer + 1);
                takes = juce::jmax (takes, z.take + 1);
            }

            // Loudness: RMS over the first two seconds of the loudest layer
            double sum = 0.0; int count = 0;
            for (auto& z : zones)
            {
                if (z.layer != layers - 1) continue;
                const int n = juce::jmin (z.audio.getNumSamples(), (int) (rate * 2.0));
                if (n <= 0) continue;
                sum += z.audio.getRMSLevel (0, 0, n) + z.audio.getRMSLevel (1, 0, n);
                count += 2;
            }
            const auto rms = count > 0 ? (float) (sum / count) : 0.1f;
            gain = juce::jlimit (0.5f, 24.0f, 0.36f / juce::jmax (1.0e-4f, rms));

            // Outline of the zone nearest middle C, loudest layer
            overview.assign (160, 0.0f);
            if (auto* z = pick (60, layers - 1, 0))
            {
                const int n = z->audio.getNumSamples();
                float peak = 1.0e-6f;
                for (int i = 0; i < 160 && n > 0; ++i)
                {
                    const int a = n * i / 160, b = juce::jmax (a + 1, n * (i + 1) / 160);
                    float m = 0.0f;
                    for (int s = a; s < b; s += 8)
                        m = juce::jmax (m, std::abs (z->audio.getSample (0, s)), std::abs (z->audio.getSample (1, s)));
                    overview[(size_t) i] = m;
                    peak = juce::jmax (peak, m);
                }
                for (auto& v : overview) v /= peak;
            }
        }

        double lengthSeconds() const
        {
            int n = 0;
            for (auto& z : zones) n = juce::jmax (n, z.audio.getNumSamples());
            return n / rate;
        }
    };

    using SourcePtr = std::shared_ptr<const Source>;

    struct SourceInfo
    {
        juce::String key, name, family, pack;
        bool installed = true;
    };

    /** The sources in every installed pack (and, for packs in the catalog that aren't
        installed yet, their names from a small built-in list so menus can offer them). */
    inline std::vector<SourceInfo> listPackSources()
    {
        std::vector<SourceInfo> out;
        for (auto& pack : catalog())
        {
            auto manifest = readManifest (pack.id);
            if (auto* sources = manifest["sources"].getArray())
                for (auto& s : *sources)
                    out.push_back ({ pack.id + "/" + s["id"].toString(), s["name"].toString(), s["family"].toString(), pack.id, true });
        }
        return out;
    }

    /** Loads a pack source from disk (blocking; call from a background thread). */
    inline SourcePtr loadPackSource (const juce::String& key)
    {
        const auto packId = key.upToFirstOccurrenceOf ("/", false, false);
        const auto sourceId = key.fromFirstOccurrenceOf ("/", false, false);
        auto manifest = readManifest (packId);
        auto* sources = manifest["sources"].getArray();
        if (sources == nullptr)
            return {};

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        for (auto& s : *sources)
        {
            if (s["id"].toString() != sourceId)
                continue;

            auto src = std::make_shared<Source>();
            src->key = key;
            src->name = s["name"].toString();
            src->family = s["family"].toString();
            src->pitched = (bool) s["pitched"];

            if (auto* zones = s["zones"].getArray())
                for (auto& zv : *zones)
                {
                    auto file = packFolder (packId).getChildFile (zv["file"].toString());
                    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
                    if (reader == nullptr)
                        continue;

                    Zone z;
                    z.root = (int) zv["root"];
                    z.layer = (int) zv["layer"];
                    z.take = (int) zv["take"];
                    z.audio.setSize (2, (int) reader->lengthInSamples);
                    reader->read (&z.audio, 0, (int) reader->lengthInSamples, 0, true, true);
                    if (reader->numChannels == 1)
                        z.audio.copyFrom (1, 0, z.audio, 0, 0, z.audio.getNumSamples());
                    src->rate = reader->sampleRate;
                    src->zones.push_back (std::move (z));
                }

            if (src->zones.empty())
                return {};

            src->finish();
            return src;
        }
        return {};
    }

    //==============================================================================
    /**
        Downloads and installs packs in the background, one at a time. Listeners hear about
        progress and completion on the message thread.
    */
    class Installer : public juce::ChangeBroadcaster, private juce::Thread, private juce::AsyncUpdater
    {
    public:
        enum class State { idle, downloading, installing, failed, done };

        static Installer& get()
        {
            static Installer instance;
            return instance;
        }

        ~Installer() override    { stopThread (4000); }

        void install (const PackInfo& pack)
        {
            if (isThreadRunning())
                return;
            current = pack;
            source = juce::File();
            state = State::downloading;
            progress = 0.0f;
            startThread();
            sendChangeMessage();
        }

        /** Installs from a zip someone already has (e.g. shared directly). */
        void installFromZip (const PackInfo& pack, const juce::File& zip)
        {
            if (isThreadRunning())
                return;
            current = pack;
            source = zip;
            state = State::installing;
            progress = 1.0f;
            startThread();
            sendChangeMessage();
        }

        void cancel()                       { signalThreadShouldExit(); }

        State getState() const              { return state.load(); }
        float getProgress() const           { return progress.load(); }
        juce::String getPackId() const      { return current.id; }
        juce::String getError() const       { const juce::ScopedLock sl (lock); return error; }

    private:
        Installer() : juce::Thread ("Elsewhere sound pack installer") {}

        PackInfo current;
        juce::File source;
        std::atomic<State> state { State::idle };
        std::atomic<float> progress { 0.0f };
        juce::CriticalSection lock;
        juce::String error;

        void fail (const juce::String& message)
        {
            { const juce::ScopedLock sl (lock); error = message; }
            state = State::failed;
            triggerAsyncUpdate();
        }

        void run() override
        {
            root().createDirectory();
            auto zip = source;

            if (zip == juce::File())
            {
                zip = root().getChildFile ("." + current.id + ".download.zip");
                zip.deleteFile();

                int status = 0;
                auto stream = juce::URL (current.url).createInputStream (
                    juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (20000).withNumRedirectsToFollow (8).withStatusCode (&status));

                if (stream == nullptr || status >= 400)
                    return fail ("Couldn't reach the download (" + (status > 0 ? "HTTP " + juce::String (status) : juce::String ("no connection")) + ")");

                const auto total = stream->getTotalLength();
                std::unique_ptr<juce::FileOutputStream> out (zip.createOutputStream());
                if (out == nullptr)
                    return fail ("Couldn't write to " + root().getFullPathName());

                juce::HeapBlock<char> buffer (1 << 16);
                juce::int64 got = 0;
                while (! stream->isExhausted())
                {
                    if (threadShouldExit())
                    {
                        out.reset();
                        zip.deleteFile();
                        state = State::idle;
                        triggerAsyncUpdate();
                        return;
                    }
                    const auto n = stream->read (buffer, 1 << 16);
                    if (n <= 0) break;
                    out->write (buffer, (size_t) n);
                    got += n;
                    if (total > 0)
                        progress = (float) ((double) got / (double) total);
                    triggerAsyncUpdate();
                }
                out.reset();

                if (total > 0 && got < total)
                    return fail ("The download stopped early");
            }

            state = State::installing;
            triggerAsyncUpdate();

            const auto staging = root().getChildFile ("." + current.id + ".installing");
            staging.deleteRecursively();
            juce::ZipFile archive (zip);
            if (archive.getNumEntries() == 0 || archive.uncompressTo (staging).failed())
                return fail ("The download isn't a valid sound pack");

            // The pack is either at the top of the zip or in one folder inside it
            auto packDir = staging;
            if (! packDir.getChildFile ("pack.json").existsAsFile())
                for (auto& sub : staging.findChildFiles (juce::File::findDirectories, false))
                    if (sub.getChildFile ("pack.json").existsAsFile())
                        packDir = sub;

            if (! packDir.getChildFile ("pack.json").existsAsFile())
                return fail ("The download isn't a valid sound pack");

            packFolder (current.id).deleteRecursively();
            if (! packDir.moveFileTo (packFolder (current.id)))
                return fail ("Couldn't install into " + root().getFullPathName());

            staging.deleteRecursively();
            if (source == juce::File())
                zip.deleteFile();

            state = State::done;
            triggerAsyncUpdate();
        }

        void handleAsyncUpdate() override   { sendChangeMessage(); }
    };
}

} // namespace elsewhere
