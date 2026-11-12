// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace elsewhere
{
//==============================================================================
/** Offers the Organic Sounds pack when it isn't installed: download, progress, or install from a file. */
class OrganicPackPanel : public juce::Component, private juce::ChangeListener
{
public:
    std::function<void()> onInstalled;

    OrganicPackPanel()
    {
        addAndMakeVisible (download);
        addAndMakeVisible (fromFile);
        download.onClick = []
        {
            auto& inst = SoundLibrary::Installer::get();
            if (inst.getState() == SoundLibrary::Installer::State::downloading)
                inst.cancel();
            else if (auto* p = SoundLibrary::findPack (OrganicDevice::packId()))
                inst.install (*p);
        };
        fromFile.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Install Organic Sounds from a zip", juce::File(), "*.zip");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [] (const juce::FileChooser& fc)
            {
                if (fc.getResult().existsAsFile())
                    if (auto* p = SoundLibrary::findPack (OrganicDevice::packId()))
                        SoundLibrary::Installer::get().installFromZip (*p, fc.getResult());
            });
        };
        SoundLibrary::Installer::get().addChangeListener (this);
        update();
    }

    ~OrganicPackPanel() override    { SoundLibrary::Installer::get().removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (r, 6.0f);

        auto t = getLocalBounds().reduced (12, 8);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText ("Organic Sounds", t.removeFromTop (16), juce::Justification::centredLeft);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.5f));
        g.drawText (status, t.removeFromTop (14), juce::Justification::centredLeft);

        if (busy)
        {
            auto bar = getLocalBounds().reduced (12, 0).removeFromBottom (10).withTrimmedBottom (6).toFloat();
            g.setColour (Colours::panelDark);
            g.fillRoundedRectangle (bar, 2.0f);
            g.setColour (Colours::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * juce::jlimit (0.02f, 1.0f, progress)), 2.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        r.removeFromTop (34);
        auto row = r.removeFromTop (22);
        download.setBounds (row.removeFromLeft (110));
        row.removeFromLeft (6);
        fromFile.setBounds (row.removeFromLeft (86));
    }

private:
    juce::TextButton download { "Download" }, fromFile { "From File" };
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String status;
    float progress = 0.0f;
    bool busy = false;

    void update()
    {
        using S = SoundLibrary::Installer::State;
        auto& inst = SoundLibrary::Installer::get();
        const auto* pack = SoundLibrary::findPack (OrganicDevice::packId());
        const auto state = inst.getPackId() == OrganicDevice::packId() ? inst.getState() : S::idle;
        progress = inst.getProgress();
        busy = state == S::downloading || state == S::installing;

        switch (state)
        {
            case S::downloading:    status = "Downloading " + juce::String (juce::roundToInt (progress * 100.0f)) + " %"; break;
            case S::installing:     status = "Installing"; break;
            case S::failed:         status = inst.getError(); break;
            case S::done:           status = "Installed"; break;
            case S::idle:
            default:                status = SoundLibrary::hasUpdate (OrganicDevice::packId())
                                                 ? "New: brass, organ, piano, bells and more \xc2\xb7 " + juce::String (pack != nullptr ? pack->sizeMB : 0) + " MB"
                                                 : "Orchestra, keys, harp, mallets and textures \xc2\xb7 " + juce::String (pack != nullptr ? pack->sizeMB : 0) + " MB"; break;
        }
        status = juce::String::fromUTF8 (status.toRawUTF8());
        download.setButtonText (state == S::downloading ? "Cancel" : state == S::failed ? "Try Again"
                               : SoundLibrary::hasUpdate (OrganicDevice::packId()) ? "Update" : "Download");
        download.setEnabled (state != S::installing);
        fromFile.setEnabled (! busy);
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        update();
        if (SoundLibrary::Installer::get().getState() == SoundLibrary::Installer::State::done && onInstalled)
            onInstalled();
    }
};

//==============================================================================
/**
    The Field: both sources drawn as flat silhouettes (A rising above the horizon, B falling
    below it, each as strong as the morph lets it be), with every playing voice's grain
    cloud drifting along them.
*/
class OrganicField : public juce::Component, private juce::Timer
{
public:
    explicit OrganicField (OrganicDevice& d) : dev (d)
    {
        setInterceptsMouseClicks (false, true);
        startTimerHz (30);
    }

    juce::String patchName;

    void refreshSources()
    {
        for (int k = 0; k < 2; ++k)
        {
            auto o = dev.getOverview (k);
            std::vector<float> smooth (o.size(), 0.0f);
            for (size_t i = 0; i < o.size(); ++i)
            {
                float sum = 0.0f; int n = 0;
                for (int d = -4; d <= 4; ++d)
                {
                    const auto j = (int) i + d;
                    if (j >= 0 && j < (int) o.size()) { sum += o[(size_t) j]; ++n; }
                }
                smooth[i] = sum / (float) juce::jmax (1, n);
            }
            outline[(size_t) k] = smooth;
            names[(size_t) k] = dev.getSourceName (k).toUpperCase();
            missing[(size_t) k] = dev.isSourceMissing (k);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 6.0f);

        const float mid = r.getCentreY();
        const float m = dev.morphNow.load();
        const float wA = std::cos (m * juce::MathConstants<float>::halfPi), wB = std::sin (m * juce::MathConstants<float>::halfPi);
        const juce::Colour cols[2] = { Colours::accent, Colours::clips[5] };
        const float positions[2] = { dev.aPosition.get() / 100.0f * 0.85f, dev.bPosition.get() / 100.0f * 0.85f };
        auto inner = r.reduced (10.0f, 0.0f);

        // Silhouettes: each source as three nested contours, like terrain seen from the side
        for (int k = 0; k < 2; ++k)
        {
            const auto& o = outline[(size_t) k];
            if (o.empty()) continue;
            const float dir = k == 0 ? -1.0f : 1.0f;
            const float strength = k == 0 ? wA : wB;
            const float h = (r.getHeight() * 0.5f - 22.0f) * (0.35f + 0.65f * strength);

            for (int band = 0; band < 3; ++band)
            {
                const float scale = 1.0f - 0.3f * (float) band;
                juce::Path p;
                p.startNewSubPath (inner.getX(), mid);
                for (size_t i = 0; i < o.size(); ++i)
                {
                    const float x = inner.getX() + inner.getWidth() * (float) i / (float) (o.size() - 1);
                    p.lineTo (x, mid + dir * (1.0f + std::pow (o[i], 0.8f) * h * scale));
                }
                p.lineTo (inner.getRight(), mid);
                p.closeSubPath();
                g.setColour (cols[k].withAlpha ((0.07f + 0.10f * strength) * (1.0f + 0.6f * (float) band)));
                g.fillPath (p);
            }

            // Where the voices start reading
            const float px = inner.getX() + inner.getWidth() * positions[k];
            g.setColour (cols[k].withAlpha (0.6f));
            g.fillRect (juce::Rectangle<float> (px - 0.75f, k == 0 ? mid - h - 4.0f : mid, 1.5f, h + 4.0f));
        }

        g.setColour (Colours::outline);
        g.fillRect (juce::Rectangle<float> (inner.getX(), mid - 0.5f, inner.getWidth(), 1.0f));

        // Grain clouds of the playing voices
        const float tex[2] = { dev.aTexture.get() / 100.0f, dev.bTexture.get() / 100.0f };
        juce::Random rng ((juce::int64) frame);
        for (auto& v : dev.voiceView)
        {
            if (! v.active || v.amp < 0.002f) continue;
            for (int k = 0; k < 2; ++k)
            {
                const float strength = k == 0 ? wA : wB;
                if (strength < 0.05f) continue;
                const float dir = k == 0 ? -1.0f : 1.0f;
                const int dots = 2 + (int) (tex[k] * 9.0f);
                const float cx = inner.getX() + inner.getWidth() * v.x[k];
                for (int i = 0; i < dots; ++i)
                {
                    const float spreadX = (4.0f + 70.0f * tex[k] * tex[k]) * (rng.nextFloat() * 2.0f - 1.0f);
                    const float y = mid + dir * (6.0f + rng.nextFloat() * (r.getHeight() * 0.5f - 26.0f) * (0.3f + 0.7f * tex[k]));
                    const float rad = 1.5f + 2.5f * juce::jmin (1.0f, v.amp * 2.0f) * rng.nextFloat();
                    g.setColour (cols[k].withAlpha (juce::jlimit (0.15f, 1.0f, v.amp * 2.5f * strength)));
                    g.fillEllipse (cx + spreadX - rad, y - rad, rad * 2.0f, rad * 2.0f);
                }
            }
        }

        // Labels
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        auto top = r.reduced (10.0f, 6.0f).removeFromTop (14.0f);
        auto bottom = r.reduced (10.0f, 6.0f).removeFromBottom (14.0f);
        for (int k = 0; k < 2; ++k)
        {
            auto area = k == 0 ? top : bottom;
            g.setColour (cols[k]);
            g.drawText (k == 0 ? "A" : "B", area.removeFromLeft (12.0f), juce::Justification::centredLeft);
            g.setColour (Colours::textDim);
            g.drawText (names[(size_t) k] + (missing[(size_t) k] ? juce::String::fromUTF8 ("  \xc2\xb7  NOT INSTALLED") : juce::String()),
                        area, juce::Justification::centredLeft);
        }
        g.setColour (Colours::textDim);
        g.drawText (patchName, top, juce::Justification::centredRight);
    }

private:
    OrganicDevice& dev;
    std::array<std::vector<float>, 2> outline;
    std::array<juce::String, 2> names;
    std::array<bool, 2> missing { false, false };
    juce::uint32 frame = 0;

    void timerCallback() override
    {
        ++frame;
        bool any = false;
        for (auto& v : dev.voiceView) any = any || v.active;
        if (any || wasPlaying)
            repaint();
        wasPlaying = any;
    }

    bool wasPlaying = false;
};

//==============================================================================
/** The source of a layer: its name, and a menu of every source by family. */
class OrganicSourceButton : public juce::Component
{
public:
    OrganicSourceButton (OrganicDevice& d, int l, juce::Colour c) : dev (d), layer (l), colour (c) {}

    juce::String text;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::panelDark);
        g.fillRoundedRectangle (r, 3.0f);
        auto t = getLocalBounds().reduced (7, 0);
        g.setColour (colour);
        g.fillRect (t.removeFromLeft (3).withSizeKeepingCentre (3, 10));
        t.removeFromLeft (6);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (juce::String::fromUTF8 ("\xe2\x96\xbe"), t.removeFromRight (10), juce::Justification::centredRight);
        g.drawFittedText (text, t, juce::Justification::centredLeft, 1, 0.8f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        juce::PopupMenu m;
        std::vector<juce::String> keys;
        const auto current = dev.getSourceKey (layer);
        const auto sources = SoundLibrary::listPackSources();
        const bool installed = SoundLibrary::isInstalled (OrganicDevice::packId());

        if (! installed)
            m.addSectionHeader ("Organic Sounds aren't installed");

        for (auto family : { "Bowed", "Breath", "Keys", "Plucked", "Struck", "Texture" })
        {
            juce::PopupMenu sub;
            bool any = false;
            for (auto& s : sources)
                if (s.family == family)
                {
                    keys.push_back (s.key);
                    sub.addItem ((int) keys.size(), s.name, true, s.key == current);
                    any = true;
                }
            for (auto& b : OrganicDsp::Builtins::list())
                if (juce::String (b.family) == family)
                {
                    const auto key = juce::String ("builtin/") + b.id;
                    keys.push_back (key);
                    sub.addItem ((int) keys.size(), juce::String (b.name) + juce::String::fromUTF8 ("  \xc2\xb7  built in"), true, key == current);
                    any = true;
                }
            if (any)
                m.addSubMenu (family, sub, true, nullptr, OrganicDevice::familyOf (current) == family);
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe = juce::Component::SafePointer<OrganicSourceButton> (this), keys] (int result)
        {
            if (safe != nullptr && result > 0 && result <= (int) keys.size())
                safe->dev.setSourceKey (safe->layer, keys[(size_t) result - 1]);
        });
    }

private:
    OrganicDevice& dev;
    int layer;
    juce::Colour colour;
};

//==============================================================================
/** How hard the instrument is being played right now, and by what. */
class OrganicSwellMeter : public juce::Component
{
public:
    explicit OrganicSwellMeter (OrganicDevice& d) : dev (d) {}

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (OrganicDevice::expressionNames()[dev.expressionSource.load()].toUpperCase(), r.removeFromLeft (76), juce::Justification::centredLeft);
        auto bar = r.withSizeKeepingCentre (r.getWidth(), 6).toFloat();
        g.setColour (Colours::panelDark);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (Colours::clips[2]);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (6.0f, bar.getWidth() * dev.liveSwell.load())), 3.0f);
    }

private:
    OrganicDevice& dev;
};

//==============================================================================
class OrganicView : public SynthPanel::PanelBase, private juce::Timer
{
public:
    OrganicView (OrganicDevice& d)
        : dev (d), field (d), sourceA (d, 0, Colours::accent), sourceB (d, 1, Colours::clips[5]), meter (d)
    {
        addAndMakeVisible (field);
        addAndMakeVisible (sourceA);
        addAndMakeVisible (sourceB);
        addAndMakeVisible (meter);
        addChildComponent (packPanel);
        packPanel.onInstalled = [this] { dev.reloadSources(); updatePack(); };

        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        nextButton.onClick = [this] { step (1); };
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto cA = Colours::accent, cB = Colours::clips[5], cMorph = Colours::clips[4], cBody = Colours::clips[3],
                   cExp = Colours::clips[2], cTone = Colours::clips[1], cSpace = Colours::clips[4];
        layerA = { knob (d.aTexture, "Texture", cA), knob (d.aPosition, "Position", cA), knob (d.aTune, "Tune", cA), knob (d.aLevel, "Level", cA) };
        layerB = { knob (d.bTexture, "Texture", cB), knob (d.bPosition, "Position", cB), knob (d.bTune, "Tune", cB), knob (d.bLevel, "Level", cB) };
        morphKnobs = { knob (d.morph, "Morph", cMorph), knob (d.tide, "Tide", cMorph), knob (d.tideDepth, "Depth", cMorph) };
        bodySel = selector (d.bodyType, OrganicDsp::Body::names(), cBody);
        bodyKnobs = { knob (d.resonance, "Resonance", cBody), knob (d.bodyDecay, "Decay", cBody), knob (d.air, "Air", cBody) };
        expKnobs = { knob (d.swell, "Swell", cExp), knob (d.dynamics, "Dynamics", cExp), knob (d.bloom, "Bloom", cExp) };
        toneKnobs = { knob (d.bright, "Bright", cTone), knob (d.warmth, "Warmth", cTone), knob (d.attack, "Attack", cTone), knob (d.release, "Release", cTone) };
        spaceKnobs = { knob (d.spaceSize, "Space", cSpace), knob (d.spaceMix, "Mix", cSpace), knob (d.width, "Width", cSpace),
                       knob (d.life, "Life", Colours::accent), knob (d.volume, "Volume", Colours::accent) };

        refresh();
        updatePack();
        timerCallback();
        startTimerHz (8);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, aArea, "Layer A", Colours::accent);
        SynthPanel::paintSection (g, morphArea, "Morph", Colours::clips[4]);
        SynthPanel::paintSection (g, bArea, "Layer B", Colours::clips[5]);
        SynthPanel::paintSection (g, bodyArea, "Body", Colours::clips[3]);
        SynthPanel::paintSection (g, expArea, "Expression", Colours::clips[2]);
        SynthPanel::paintSection (g, toneArea, "Tone", Colours::clips[1]);
        SynthPanel::paintSection (g, spaceArea, "Space", Colours::clips[4]);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = 6;

        auto cols = columns (r.withTrimmedLeft (296), { 136, 112, 136, 176, 168, 128, 176 }, gap);
        aArea = cols[0]; morphArea = cols[1]; bArea = cols[2]; bodyArea = cols[3]; expArea = cols[4]; toneArea = cols[5]; spaceArea = cols[6];

        auto display = r.removeFromLeft (290);
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (4);
        field.setBounds (display);
        packPanel.setBounds (display.reduced (34, 0).withSizeKeepingCentre (display.getWidth() - 60, 76));
        prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        patchButton.setBounds (nav);

        auto layer = [] (juce::Rectangle<int> area, OrganicSourceButton& b, std::vector<SynthPanel::Knob*>& k)
        {
            auto c = content (area);
            b.setBounds (c.removeFromTop (20));
            c.removeFromTop (3);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { k[0], k[1] }, 2);
            row (c, { k[2], k[3] }, 2);
        };
        layer (aArea, sourceA, layerA);
        layer (bArea, sourceB, layerB);

        {
            auto c = content (morphArea);
            auto top = c.removeFromTop (c.getHeight() / 2);
            morphKnobs[0]->setBounds (top.withSizeKeepingCentre (top.getWidth() / 2 + 8, top.getHeight()));
            row (c, { morphKnobs[1], morphKnobs[2] }, 2);
        }
        {
            auto c = content (bodyArea);
            bodySel->setBounds (c.removeFromTop (18));
            c.removeFromTop (6);
            row (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 66)), { bodyKnobs[0], bodyKnobs[1], bodyKnobs[2] }, 3);
        }
        {
            auto c = content (expArea);
            meter.setBounds (c.removeFromTop (18));
            c.removeFromTop (6);
            row (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 66)), { expKnobs[0], expKnobs[1], expKnobs[2] }, 3);
        }
        {
            auto c = content (toneArea);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { toneKnobs[0], toneKnobs[1] }, 2);
            row (c, { toneKnobs[2], toneKnobs[3] }, 2);
        }
        {
            auto c = content (spaceArea);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { spaceKnobs[0], spaceKnobs[1], spaceKnobs[2] }, 3);
            row (c, { spaceKnobs[3], spaceKnobs[4] }, 3);
        }
    }

private:
    OrganicDevice& dev;
    OrganicField field;
    OrganicSourceButton sourceA, sourceB;
    OrganicSwellMeter meter;
    OrganicPackPanel packPanel;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    std::vector<SynthPanel::Knob*> layerA, layerB, morphKnobs, bodyKnobs, expKnobs, toneKnobs, spaceKnobs;
    SynthPanel::Selector* bodySel = nullptr;
    juce::Rectangle<int> aArea, morphArea, bArea, bodyArea, expArea, toneArea, spaceArea;
    juce::String shownKeys;

    void refresh()
    {
        sourceA.text = dev.getSourceName (0);
        sourceB.text = dev.getSourceName (1);
        sourceA.repaint();
        sourceB.repaint();
        field.refreshSources();
    }

    void updatePack()
    {
        using S = SoundLibrary::Installer::State;
        const auto& inst = SoundLibrary::Installer::get();
        const bool busy = inst.getPackId() == OrganicDevice::packId() && (inst.getState() == S::downloading || inst.getState() == S::installing);
        packPanel.setVisible (busy || ! SoundLibrary::isInstalled (OrganicDevice::packId()) || SoundLibrary::hasUpdate (OrganicDevice::packId()));
    }

    void step (int delta)
    {
        const int count = (int) OrganicDevice::patches().size();
        const int index = dev.getPatchIndex();
        dev.loadPatch (index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count);
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int i = 0; i < (int) OrganicDevice::patches().size(); ++i)
            m.addItem (i + 1, OrganicDevice::patchLabel (i), true, i == dev.getPatchIndex());

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<OrganicView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
                safe->dev.loadPatch (result - 1);
        });
    }

    void timerCallback() override
    {
        const int index = dev.getPatchIndex();
        auto name = OrganicDevice::patchLabel (index);
        if (index >= 0 && dev.isPatchModified())
            name << " *";
        if (name != field.patchName)
        {
            field.patchName = name;
            field.repaint();
        }

        const auto keys = dev.getSourceName (0) + "|" + dev.getSourceName (1) + "|" + juce::String ((int) dev.isSourceMissing (0)) + juce::String ((int) dev.isSourceMissing (1));
        if (keys != shownKeys)
        {
            shownKeys = keys;
            refresh();
        }
        meter.repaint();
        updatePack();
    }
};

inline std::unique_ptr<juce::Component> OrganicDevice::createDeviceView()     { return std::make_unique<OrganicView> (*this); }

} // namespace elsewhere
