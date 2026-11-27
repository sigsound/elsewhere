// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "EqDevice.h"
#include "CompressorDevice.h"
#include "ReverbDevice.h"
#include "DrumsDevice.h"
#include "AmpDevice.h"
#include "MasterDevice.h"
#include "MonoDevice.h"
#include "PolyDevice.h"
#include "ModularDevice.h"
#include "OrganicDevice.h"
#include "HazeDevice.h"
#include "HazeEngine.h"
#include "MidiEffects.h"
#include "StepsDevice.h"
#include "EchoDevice.h"
#include "ReferenceDevice.h"
#include "SomewhereControlDevice.h"
#include "../Modulators.h"
#include "../SessionRecorder.h"
#include "../drums/DrumTrack.h"
#include "../drums/DrumSectionEditor.h"
#include "../Mixing.h"
#include "../Normalise.h"
#include "../SessionView.h"
#include "../ArrangementView.h"
#include "../AudioEditor.h"
#include <tracktion_engine/testing/tracktion_EnginePlayer.h>
#include "DeviceTests.h"
#include <set>

namespace elsewhere::DeviceTests
{
namespace
{
    constexpr double sr = 48000.0;
    constexpr int block = 256;

    struct Result { int failures = 0; juce::String log; };

    void check (Result& r, bool ok, const juce::String& what)
    {
        r.log << (ok ? "  PASS  " : "  FAIL  ") << what << "\n";
        if (! ok) ++r.failures;
    }

    void setParam (ElsewhereDevice::Param p, float v)     { p.param().setParameter (v, juce::sendNotification); }

    /** Runs a buffer through a device in blocks (with optional MIDI at the first sample). */
    void process (te::Plugin& p, juce::AudioBuffer<float>& buf, std::vector<std::pair<int, juce::MidiMessage>> midi = {}, double startSeconds = 0.0)
    {
        for (int start = 0; start < buf.getNumSamples(); start += block)
        {
            auto n = juce::jmin (block, buf.getNumSamples() - start);
            te::MidiMessageArray mma;

            for (auto& [pos, m] : midi)
                if (pos >= start && pos < start + n)
                    mma.addMidiMessage (m, (pos - start) / sr, te::createUniqueMPESourceID());

            te::PluginRenderContext rc (&buf, start, n, &mma, 0.0,
                                        te::TimeRange (te::TimePosition::fromSeconds (startSeconds + start / sr), te::TimePosition::fromSeconds (startSeconds + (start + n) / sr)),
                                        true, false, true, false);
            p.applyToBuffer (rc);
        }
    }

    /** Runs MIDI through a MIDI effect; returns (absolute sample, message) for everything it outputs. */
    std::vector<std::pair<int, juce::MidiMessage>> processMidi (te::Plugin& p, int numSamples, bool playing,
                                                                 std::vector<std::pair<int, juce::MidiMessage>> midi, int startSample = 0,
                                                                 bool mixingDown = true)
    {
        std::vector<std::pair<int, juce::MidiMessage>> result;
        juce::AudioBuffer<float> buf (2, block);

        for (int start = 0; start < numSamples; start += block)
        {
            auto n = juce::jmin (block, numSamples - start);
            buf.clear();
            te::MidiMessageArray mma;
            for (auto& [pos, m] : midi)
                if (pos >= start && pos < start + n)
                    mma.addMidiMessage (m, (pos - start) / sr, {});

            auto t0 = (startSample + start) / sr;
            te::PluginRenderContext rc (&buf, 0, n, &mma, 0.0,
                                        te::TimeRange (te::TimePosition::fromSeconds (t0), te::TimePosition::fromSeconds (t0 + n / sr)),
                                        playing, false, mixingDown, false);
            p.applyToBuffer (rc);

            for (auto& m : mma)
                result.push_back ({ start + juce::roundToInt (m.getTimeStamp() * sr), m });
        }
        return result;
    }

    std::vector<std::pair<int, int>> noteOns (const std::vector<std::pair<int, juce::MidiMessage>>& events)
    {
        std::vector<std::pair<int, int>> r;
        for (auto& [pos, m] : events) if (m.isNoteOn()) r.push_back ({ pos, m.getNoteNumber() });
        return r;
    }

    juce::String describe (const std::vector<std::pair<int, int>>& ons, size_t count)
    {
        juce::StringArray s;
        for (size_t i = 0; i < juce::jmin (count, ons.size()); ++i) s.add (juce::String (ons[i].second) + "@" + juce::String (ons[i].first));
        return s.joinIntoString (" ");
    }

    juce::AudioBuffer<float> sine (double freq, float amp, double seconds)
    {
        juce::AudioBuffer<float> b (2, (int) (seconds * sr));
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            auto v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / sr);
            b.setSample (0, i, v); b.setSample (1, i, v);
        }
        return b;
    }

    /** RMS of the last half (after filters/envelopes settle), in dB. */
    float tailRmsDb (const juce::AudioBuffer<float>& b)
    {
        auto half = b.getNumSamples() / 2;
        return juce::Decibels::gainToDecibels (b.getRMSLevel (0, half, b.getNumSamples() - half), -200.0f);
    }

    bool isFinite (const juce::AudioBuffer<float>& b)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (ch, i)))
                    return false;
        return true;
    }

    /** Pitch by YIN (cumulative mean normalised difference): robust to waveforms that cross zero more than once a cycle. */
    double yinPitch (const juce::AudioBuffer<float>& b, int start, int w = 4096)
    {
        const int maxLag = (int) (sr / 40.0), minLag = (int) (sr / 2000.0);
        if (start + w + maxLag >= b.getNumSamples()) return 0.0;
        const float* x = b.getReadPointer (0) + start;
        std::vector<double> cm ((size_t) maxLag + 1, 1.0);
        double run = 0.0;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            double sum = 0.0;
            for (int i = 0; i < w; ++i) { const double a = x[i] - x[i + tau]; sum += a * a; }
            run += sum;
            cm[(size_t) tau] = run > 0 ? sum * tau / run : 1.0;
        }
        for (int tau = minLag; tau < maxLag; ++tau)
            if (cm[(size_t) tau] < 0.15)
            {
                while (tau + 1 < maxLag && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau;
                const double a = cm[(size_t) tau - 1], c = cm[(size_t) tau + 1], m = cm[(size_t) tau];
                return sr / (tau + (a - c) / (2.0 * (a - 2.0 * m + c) + 1e-12));
            }
        return 0.0;
    }

    void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b)
    {
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out = f.createOutputStream();
        if (out != nullptr)
            if (auto w = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24)))
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    void refreshLookAndFeel()
    {
        if (auto* lf = dynamic_cast<ElsewhereLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
            lf->refreshColours();
    }

    /** Saves a picture of any component, at 2x. */
    void picture (juce::Component& c, const juce::File& f)
    {
        auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
        f.deleteFile();
        if (auto os = f.createOutputStream())
            juce::PNGImageFormat().writeImageToStream (img, *os);
    }

    void snapshot (ElsewhereDevice& d, const juce::File& f)
    {
        auto view = d.createDeviceView();
        juce::Component holder;
        holder.setSize (d.getDeviceViewWidth() + 16, 190);
        holder.addAndMakeVisible (*view);
        view->setBounds (holder.getLocalBounds().reduced (8));

        juce::Image img (juce::Image::ARGB, holder.getWidth(), holder.getHeight(), true);
        {
            juce::Graphics g (img);
            g.fillAll (Colours::panel);
            holder.paintEntireComponent (g, true);
        }

        f.deleteFile();
        juce::PNGImageFormat png;
        if (auto out = f.createOutputStream())
            png.writeImageToStream (img, *out);
    }

    /** A picture of a device's own window panel, at the size it opens at. */
    void snapshotWindow (ElsewhereDevice& d, const juce::File& f)
    {
        auto view = d.createWindowView();
        if (view == nullptr) return;
        juce::Component holder;
        holder.setSize (d.getWindowViewSize().x + 16, d.getWindowViewSize().y + 16);
        holder.addAndMakeVisible (*view);
        view->setBounds (holder.getLocalBounds().reduced (8));

        juce::Image img (juce::Image::ARGB, holder.getWidth(), holder.getHeight(), true);
        {
            juce::Graphics g (img);
            g.fillAll (Colours::panel);
            holder.paintEntireComponent (g, true);
        }

        f.deleteFile();
        juce::PNGImageFormat png;
        if (auto out = f.createOutputStream())
            png.writeImageToStream (img, *out);
    }

    std::vector<te::Plugin::Ptr> keepAlive;   // released before the test edit goes away

    template <typename T>
    T& make (te::Edit& edit, const char* type)
    {
        auto p = DeviceRegistry::create (edit, type);
        jassert (p != nullptr);
        keepAlive.push_back (p);
        auto& d = *dynamic_cast<T*> (p.get());
        d.initialise ({ te::TimePosition(), sr, block });
        return d;
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    outDir.createDirectory();
    auto edit = te::createEmptyEdit (engine, outDir.getChildFile ("device-test.tracktionedit"));
    Result r;

    //==============================================================================
    r.log << "EQ\n";
    {
        auto& eq = make<EqDevice> (*edit, EqDevice::xmlTypeName);
        auto flat = sine (1000.0, 0.5f, 1.0);
        process (eq, flat);
        check (r, std::abs (tailRmsDb (flat) - tailRmsDb (sine (1000.0, 0.5f, 1.0))) < 0.1f, "flat EQ leaves the signal unchanged");

        setParam (eq.bands[3].freq, 600.0f);
        setParam (eq.bands[3].gain, 12.0f);
        setParam (eq.bands[3].q, 1.0f);
        auto boosted = sine (600.0, 0.1f, 1.0);
        process (eq, boosted);
        auto boost = tailRmsDb (boosted) - tailRmsDb (sine (600.0, 0.1f, 1.0));
        check (r, std::abs (boost - 12.0f) < 0.5f, "bell +12 dB at 600 Hz gives " + juce::String (boost, 2) + " dB");
        check (r, std::abs (eq.getResponseDb (600.0) - 12.0) < 0.5, "drawn curve matches (+" + juce::String (eq.getResponseDb (600.0), 2) + " dB)");

        auto far = sine (8000.0, 0.1f, 1.0);
        process (eq, far);
        auto farDiff = tailRmsDb (far) - tailRmsDb (sine (8000.0, 0.1f, 1.0));
        check (r, std::abs (farDiff) < 1.0f, "8 kHz barely touched by the 600 Hz bell (" + juce::String (farDiff, 2) + " dB)");

        setParam (eq.bands[3].gain, 0.0f);
        setParam (eq.bands[0].on, 1.0f);
        setParam (eq.bands[0].freq, 200.0f);
        setParam (eq.bands[0].slope, 2.0f);
        auto low = sine (50.0, 0.5f, 1.5);
        process (eq, low);
        auto cut = tailRmsDb (low) - tailRmsDb (sine (50.0, 0.5f, 1.5));
        check (r, cut < -40.0f, "48 dB/oct low cut at 200 Hz removes 50 Hz (" + juce::String (cut, 1) + " dB)");
        check (r, isFinite (low), "no NaN/inf");
        snapshot (eq, outDir.getChildFile ("eq.png"));
        snapshotWindow (eq, outDir.getChildFile ("eq-window.png"));
    }

    //==============================================================================
    r.log << "Compressor\n";
    {
        auto& comp = make<CompressorDevice> (*edit, CompressorDevice::xmlTypeName);
        setParam (comp.model, (float) CompressorDevice::vca);
        setParam (comp.threshold, -20.0f);
        setParam (comp.ratio, 4.0f);
        setParam (comp.knee, 0.0f);
        setParam (comp.autoGain, 0.0f);
        setParam (comp.attack, 5.0f);
        auto loud = sine (1000.0, 1.0f, 1.0);
        process (comp, loud);
        auto peakDb = juce::Decibels::gainToDecibels (loud.getMagnitude (0, loud.getNumSamples() / 2, loud.getNumSamples() / 2));
        // 0 dB peak, 20 dB over at 4:1 -> 15 dB of reduction -> about -15 dB
        check (r, std::abs (peakDb + 15.0f) < 1.5f, "VCA 4:1 at -20 dB: 0 dB peak comes out at " + juce::String (peakDb, 2) + " dB (expect ~-15)");

        auto quiet = sine (1000.0, 0.01f, 1.0);
        process (comp, quiet);
        check (r, std::abs (tailRmsDb (quiet) - tailRmsDb (sine (1000.0, 0.01f, 1.0))) < 0.2f, "signals below threshold pass unchanged");

        for (int m = 0; m < CompressorDevice::numModels; ++m)
        {
            setParam (comp.model, (float) m);
            auto b = sine (220.0, 0.9f, 0.5);
            process (comp, b);
            check (r, isFinite (b) && b.getMagnitude (0, b.getNumSamples()) < 4.0f, CompressorDevice::modelNames()[m] + " model is stable");
        }

        setParam (comp.model, (float) CompressorDevice::limiter);
        setParam (comp.threshold, -6.0f);
        auto hot = sine (1000.0, 1.0f, 1.0);
        hot.applyGain (1.8f);   // +5 dB over full scale
        process (comp, hot);
        auto limPeak = hot.getMagnitude (0, hot.getNumSamples());
        check (r, limPeak <= 0.99f, "limiter ceiling holds (peak " + juce::String (juce::Decibels::gainToDecibels (limPeak), 2) + " dBFS)");
        snapshot (comp, outDir.getChildFile ("compressor.png"));
        snapshotWindow (comp, outDir.getChildFile ("compressor-window.png"));
    }

    //==============================================================================
    r.log << "Reverb\n";
    {
        auto& rev = make<ReverbDevice> (*edit, ReverbDevice::xmlTypeName);
        setParam (rev.mix, 100.0f);
        setParam (rev.decay, 2.0f);
        setParam (rev.predelay, 0.0f);
        setParam (rev.damping, 20000.0f);
        setParam (rev.lowCut, 20.0f);
        setParam (rev.highCut, 20000.0f);
        setParam (rev.modDepth, 0.0f);
        setParam (rev.lowDecay, 1.0f);
        setParam (rev.highDecay, 1.0f);
        juce::AudioBuffer<float> ir (2, (int) (sr * 4.0));
        ir.clear();
        ir.setSample (0, 0, 1.0f); ir.setSample (1, 0, 1.0f);
        process (rev, ir);
        writeWav (outDir.getChildFile ("reverb-ir.wav"), ir);

        auto energyDb = [&] (double t0, double t1)
        {
            return juce::Decibels::gainToDecibels (ir.getRMSLevel (0, (int) (t0 * sr), (int) ((t1 - t0) * sr)), -200.0f);
        };

        // Decay rate from 0.3 s to 1.3 s -> RT60
        auto drop = energyDb (0.2, 0.4) - energyDb (1.2, 1.4);
        auto rt60 = 60.0f / drop * 1.0f;
        check (r, isFinite (ir) && ir.getMagnitude (0, ir.getNumSamples()) < 2.0f, "impulse response is stable");
        check (r, rt60 > 1.4f && rt60 < 2.8f, "decay 2 s measures RT60 = " + juce::String (rt60, 2) + " s");
        check (r, std::abs (energyDb (0.1, 0.3) - energyDb (0.1, 0.3)) < 1.0f && ir.getRMSLevel (0, 0, ir.getNumSamples()) > 0.0001f, "produces a tail");

        // Stereo decorrelation
        double corr = 0, e0 = 0, e1 = 0;
        for (int i = (int) (0.1 * sr); i < (int) (1.0 * sr); ++i)
        {
            auto a = ir.getSample (0, i), b = ir.getSample (1, i);
            corr += a * b; e0 += a * a; e1 += b * b;
        }
        auto c = corr / std::sqrt (e0 * e1 + 1e-20);
        check (r, std::abs (c) < 0.5, "stereo tail is wide (L/R correlation " + juce::String (c, 2) + ")");

        setParam (rev.freeze, 1.0f);
        auto fr = sine (440.0, 0.3f, 3.0);
        process (rev, fr);
        check (r, isFinite (fr) && fr.getMagnitude (0, fr.getNumSamples()) < 3.0f, "freeze is stable");
        setParam (rev.freeze, 0.0f);

        // Every space: stable, and a tail at a similar level
        juce::String levels;
        bool spacesOk = true;
        for (int sp = 0; sp < ReverbDevice::numSpaces; ++sp)
        {
            setParam (rev.space, (float) sp);
            juce::AudioBuffer<float> b (2, (int) (sr * 3.0));
            b.clear();
            b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f);
            process (rev, b);
            auto db = juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) (0.05 * sr), (int) (1.0 * sr)), -200.0f);
            spacesOk = spacesOk && isFinite (b) && b.getMagnitude (0, b.getNumSamples()) < 2.0f && db > -60.0f;
            levels << ReverbDevice::spaceNames()[sp] << " " << juce::String (db, 1) << "  ";
        }
        check (r, spacesOk, "all spaces stable with a tail (" + levels.trim() + " dB)");
        setParam (rev.space, (float) ReverbDevice::hall);

        // Decay EQ: lows ring longer than highs when Low Decay > High Decay
        {
            setParam (rev.lowDecay, 2.0f);
            setParam (rev.highDecay, 0.3f);
            setParam (rev.damping, 3000.0f);
            juce::AudioBuffer<float> b (2, (int) (sr * 3.0));
            b.clear();
            b.setSample (0, 0, 1.0f); b.setSample (1, 0, 1.0f);
            process (rev, b);
            auto bandDb = [&] (float lo, float hi)
            {
                juce::AudioBuffer<float> band; band.makeCopyOf (b);
                for (auto [type, freq] : { std::pair { 0, lo }, std::pair { 1, hi } })
                {
                    juce::IIRFilter flt;
                    flt.setCoefficients (type == 0 ? juce::IIRCoefficients::makeHighPass (sr, freq) : juce::IIRCoefficients::makeLowPass (sr, freq));
                    flt.processSamples (band.getWritePointer (0), band.getNumSamples());
                }
                return juce::Decibels::gainToDecibels (band.getRMSLevel (0, (int) (1.2 * sr), (int) (0.4 * sr)), -200.0f)
                     - juce::Decibels::gainToDecibels (band.getRMSLevel (0, (int) (0.1 * sr), (int) (0.4 * sr)), -200.0f);
            };
            auto lowDrop = bandDb (60.0f, 200.0f), highDrop = bandDb (5000.0f, 12000.0f);
            check (r, lowDrop > highDrop + 10.0f, "decay EQ: lows hold longer than highs (lows " + juce::String (lowDrop, 1)
                                                      + " dB vs highs " + juce::String (highDrop, 1) + " dB after 1.1 s)");
            setParam (rev.lowDecay, 1.2f);
            setParam (rev.highDecay, 0.5f);
            setParam (rev.damping, 5000.0f);
        }

        // Shimmer: full amount with a long decay stays bounded, and adds pitched energy
        {
            // Compared with the same reverb without shimmer: no extra build-up, and the tail still dies away
            auto run = [&] (float amount)
            {
                setParam (rev.decay, 20.0f);
                setParam (rev.shimmer, amount);
                rev.reset();
                auto b = sine (440.0, 0.3f, 1.0);
                juce::AudioBuffer<float> longBuf (2, (int) (sr * 12.0));
                longBuf.clear();
                for (int ch = 0; ch < 2; ++ch) longBuf.copyFrom (ch, 0, b, ch, 0, b.getNumSamples());
                process (rev, longBuf);
                return longBuf;
            };
            auto plain = run (0.0f), shim = run (100.0f);
            auto peakPlain = plain.getMagnitude (0, plain.getNumSamples()), peak = shim.getMagnitude (0, shim.getNumSamples());
            auto early = shim.getRMSLevel (0, (int) (2.0 * sr), (int) sr), late = shim.getRMSLevel (0, (int) (11.0 * sr), (int) sr);
            check (r, isFinite (shim) && peak < peakPlain * 2.5f && late < early * 0.1f,
                   "shimmer at 100% with a 20 s decay stays bounded (peak " + juce::String (peak, 2) + " vs " + juce::String (peakPlain, 2)
                   + " without; tail " + juce::String (juce::Decibels::gainToDecibels (late / early), 1) + " dB from 2 s to 11 s)");

            // +12: energy an octave above the 440 Hz input (880 Hz) grows in the tail
            auto octaveDb = [&] (float amount)
            {
                setParam (rev.shimmer, amount);
                setParam (rev.decay, 4.0f);
                rev.reset();
                juce::AudioBuffer<float> t (2, (int) (sr * 3.0));
                t.clear();
                auto tone = sine (440.0, 0.3f, 0.5);
                for (int ch = 0; ch < 2; ++ch) t.copyFrom (ch, 0, tone, ch, 0, tone.getNumSamples());
                process (rev, t);
                juce::IIRFilter bp;
                bp.setCoefficients (juce::IIRCoefficients::makeBandPass (sr, 880.0, 8.0));
                bp.processSamples (t.getWritePointer (0), t.getNumSamples());
                return juce::Decibels::gainToDecibels (t.getRMSLevel (0, (int) (1.5 * sr), (int) (1.0 * sr)), -200.0f);
            };
            auto withShimmer = octaveDb (80.0f), without = octaveDb (0.0f);
            check (r, withShimmer > without + 6.0f, "shimmer +12 adds the octave above (" + juce::String (withShimmer - without, 1) + " dB at 880 Hz)");
            setParam (rev.shimmer, 0.0f);
            setParam (rev.decay, 2.0f);
        }

        // Ducking: the tail drops while the input plays, then comes back
        {
            auto tailWhilePlaying = [&] (float duckAmount)
            {
                setParam (rev.duck, duckAmount);
                rev.reset();
                auto b = sine (220.0, 0.5f, 2.0);
                juce::AudioBuffer<float> dry; dry.makeCopyOf (b);
                process (rev, b);
                // Wet = output - dry (mix is 100%, so the output is all wet)
                return juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) (1.0 * sr), (int) (0.9 * sr)), -200.0f);
            };
            auto open = tailWhilePlaying (0.0f), ducked = tailWhilePlaying (100.0f);
            check (r, ducked < open - 12.0f, "ducking pulls the tail down while the input plays (" + juce::String (open - ducked, 1) + " dB)");
            setParam (rev.duck, 0.0f);
        }

        setParam (rev.decay, 3.0f);
        setParam (rev.mix, 30.0f);
        setParam (rev.shimmer, 25.0f);
        setParam (rev.modDepth, 40.0f);
        snapshot (rev, outDir.getChildFile ("reverb.png"));
        snapshotWindow (rev, outDir.getChildFile ("reverb-window.png"));
        setParam (rev.space, (float) ReverbDevice::plate);
        setParam (rev.diffusion, 20.0f);
        snapshot (rev, outDir.getChildFile ("reverb-plate.png"));
        setParam (rev.space, (float) ReverbDevice::bloom);
        setParam (rev.diffusion, 90.0f);
        setParam (rev.modDepth, 80.0f);
        setParam (rev.shimmer, 70.0f);
        snapshot (rev, outDir.getChildFile ("reverb-bloom.png"));
        setParam (rev.space, (float) ReverbDevice::hall);
        setParam (rev.diffusion, 75.0f);
        setParam (rev.modDepth, 40.0f);
        setParam (rev.shimmer, 25.0f);
        Theme::apply (Themes::light); refreshLookAndFeel();
        snapshot (rev, outDir.getChildFile ("reverb-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
        setParam (rev.shimmer, 0.0f);
    }

    //==============================================================================
    r.log << "Reverb on a return (stress)\n";
    {
        // Fed continuously, like a return channel, with the controls wandering: shimmer must never
        // make it build up beyond what the same settings do without shimmer, and nothing may break
        auto& rv = make<ReverbDevice> (*edit, ReverbDevice::xmlTypeName);
        auto run = [&] (int space, float shim, int interval, int seed)
        {
            SynthCore::Noise noise;
            noise.rng.setSeed (seed);
            juce::Random rng (seed);
            rv.initialise ({ te::TimePosition(), sr, block });
            setParam (rv.space, (float) space); setParam (rv.shimmer, shim); setParam (rv.shimmerInterval, (float) interval);
            setParam (rv.decay, 30.0f); setParam (rv.size, 100.0f); setParam (rv.diffusion, 100.0f); setParam (rv.modDepth, 100.0f);
            setParam (rv.lowDecay, 2.5f); setParam (rv.highDecay, 1.5f); setParam (rv.mix, 100.0f); setParam (rv.early, 100.0f);
            setParam (rv.freeze, 0.0f);
            float peak = 0.0f, late = 0.0f;
            for (int sec = 0; sec < 24; ++sec)
            {
                if (sec % 4 == 3)
                {
                    setParam (rv.size, rng.nextFloat() * 100.0f); setParam (rv.decay, 0.5f + rng.nextFloat() * 29.5f);
                    setParam (rv.freeze, sec == 11 ? 1.0f : 0.0f);
                }
                juce::AudioBuffer<float> b (2, (int) sr);
                for (int i = 0; i < b.getNumSamples(); ++i) { const float x = noise.pink() * 0.5f; b.setSample (0, i, x); b.setSample (1, i, x * 0.9f); }
                process (rv, b);
                if (! isFinite (b)) return std::pair<float, float> { 1.0e9f, 1.0e9f };
                peak = juce::jmax (peak, b.getMagnitude (0, b.getNumSamples()));
                if (sec >= 20) late = juce::jmax (late, b.getRMSLevel (0, 0, b.getNumSamples()));
            }
            return std::pair<float, float> { peak, late };
        };
        juce::String worst;
        float worstRatio = 0.0f;
        int runaways = 0, cases = 0;
        for (int space = 0; space < ReverbDevice::numSpaces; ++space)
        {
            const int seed = 2026 + space;
            const auto plain = run (space, 0.0f, 0, seed);
            for (float shim : { 50.0f, 100.0f })
                for (int interval = 0; interval < 4; ++interval)
                {
                    ++cases;
                    const auto [peak, late] = run (space, shim, interval, seed);
                    const float ratio = late / juce::jmax (1.0e-6f, plain.second);
                    if (ratio > worstRatio)
                    {
                        worstRatio = ratio;
                        worst = ReverbDevice::spaceNames()[space] + " " + juce::String (shim, 0) + "% " + ReverbDevice::shimmerNames()[interval];
                    }
                    if (peak > 8.0f || ratio > 2.0f)
                    {
                        ++runaways;
                        r.log << "        ran away: " << ReverbDevice::spaceNames()[space] << " shimmer " << juce::String (shim, 0) << "% "
                              << ReverbDevice::shimmerNames()[interval] << " peak " << juce::String (peak, 2) << ", "
                              << juce::String (juce::Decibels::gainToDecibels (ratio), 1) << " dB over no shimmer\n";
                    }
                }
        }
        check (r, runaways == 0, juce::String (cases) + " shimmer settings fed for 24 s each never run away (most: " + worst + ", "
                                   + juce::String (juce::Decibels::gainToDecibels (worstRatio), 1) + " dB over the same settings without shimmer)");
    }

    //==============================================================================
    r.log << "Drums\n";
    {
        auto& drums = make<DrumsDevice> (*edit, DrumsDevice::xmlTypeName);
        juce::AudioBuffer<float> all (2, (int) (sr * 16 * 0.5));
        std::vector<std::pair<int, juce::MidiMessage>> notes;
        for (int i = 0; i < DrumsDevice::numPads; ++i)
            notes.push_back ({ (int) (i * 0.5 * sr) + 17, juce::MidiMessage::noteOn (10, DrumsDevice::firstNote + i, (juce::uint8) 110) });

        all.clear();
        process (drums, all, notes);
        writeWav (outDir.getChildFile ("drums-kit.wav"), all);
        check (r, isFinite (all), "no NaN/inf");

        int silentPads = 0;
        for (int i = 0; i < DrumsDevice::numPads; ++i)
        {
            auto seg = all.getMagnitude (0, (int) (i * 0.5 * sr), (int) (0.5 * sr));
            if (seg < 0.05f) { ++silentPads; r.log << "        pad " << i << " (" << DrumsDevice::padName (i) << ") peak " << seg << "\n"; }
        }
        check (r, silentPads == 0, "all 16 pads make sound");
        check (r, all.getMagnitude (0, 0, 17) == 0.0f && all.getMagnitude (0, 17, 64) > 0.0f, "hits land on the exact sample");

        auto flush = [&]
        {
            drums.midiPanic();
            juce::AudioBuffer<float> b (2, block * 4);
            process (drums, b);
        };

        // Closed hat chokes the open hat
        flush();
        juce::AudioBuffer<float> choke (2, (int) sr);
        choke.clear();
        process (drums, choke, { { 0, juce::MidiMessage::noteOn (10, 40, (juce::uint8) 110) },
                                 { (int) (0.1 * sr), juce::MidiMessage::noteOn (10, 39, (juce::uint8) 110) } });
        auto after = choke.getMagnitude (0, (int) (0.4 * sr), (int) (0.2 * sr));
        check (r, after < 0.01f, "closed hat chokes open hat (level after " + juce::String (after, 4) + ")");

        // Tune: +12 st makes the kick about half as long
        auto kickLength = [&] (float tune)
        {
            flush();
            setParam (drums.pads[0].tune, tune);
            juce::AudioBuffer<float> b (2, (int) sr);
            b.clear();
            process (drums, b, { { 0, juce::MidiMessage::noteOn (10, 36, (juce::uint8) 127) } });
            int last = 0;
            for (int i = 0; i < b.getNumSamples(); ++i) if (std::abs (b.getSample (0, i)) > 0.001f) last = i;
            return last;
        };
        auto l0 = kickLength (0.0f), l12 = kickLength (12.0f);
        setParam (drums.pads[0].tune, 0.0f);
        check (r, std::abs ((double) l12 / l0 - 0.5) < 0.05, "tune +12 st halves the length (" + juce::String ((double) l12 / l0, 3) + ")");

        // Dropping a sample on a pad replaces its sound
        auto sampleFile = outDir.getChildFile ("drums-kit.wav");
        drums.setPadSample (15, sampleFile);
        auto replaced = drums.getSound (15);
        check (r, replaced != nullptr && std::abs (replaced->audio.getNumSamples() - all.getNumSamples()) <= 1
                    && drums.getPadDisplayName (15) == "drums-kit", "dropped sample replaces the pad's sound");
        drums.setPadSample (15, {});
        check (r, drums.getSound (15) == DrumsDevice::synthesise (15), "pad goes back to its built-in sound");

        snapshot (drums, outDir.getChildFile ("drums.png"));
        snapshotWindow (drums, outDir.getChildFile ("drums-window.png"));

        // The kits: each one renders all 16 pads, quickly enough, and plays a demo beat
        {
            const int numKits = (int) DrumKits::kits().size();
            juce::String slow, quiet;

            // A two-bar groove at 100 BPM over all the slots (16th notes)
            struct Step { int pad, step, vel; };
            std::vector<Step> groove;
            for (int s16 = 0; s16 < 32; ++s16)
            {
                if (s16 % 2 == 0 && s16 != 30) groove.push_back ({ 3, s16, s16 % 4 == 0 ? 105 : 80 });   // closed hat 8ths
                if (s16 % 4 == 3) groove.push_back ({ 12, s16, 60 });                                     // shaker
            }
            for (auto st : { 0, 6, 10, 16, 22, 27 }) groove.push_back ({ 0, st, 120 });                 // kick
            for (auto st : { 4, 12, 20, 28 })        groove.push_back ({ 1, st, 115 });                 // snare
            for (auto st : { 12, 28 })               groove.push_back ({ 2, st, 100 });                 // clap
            for (auto st : { 7, 15, 23 })            groove.push_back ({ 8, st, 70 });                  // rim
            for (auto st : { 29, 30, 31 })           groove.push_back ({ 3, st, 90 });                  // hat roll
            groove.push_back ({ 4, 14, 100 }); groove.push_back ({ 4, 30, 100 });                        // open hat
            groove.push_back ({ 14, 0, 110 }); groove.push_back ({ 14, 16, 110 });                       // 808 slot
            groove.push_back ({ 5, 24, 100 }); groove.push_back ({ 6, 25, 100 }); groove.push_back ({ 7, 26, 100 });
            groove.push_back ({ 9, 18, 90 }); groove.push_back ({ 13, 21, 90 }); groove.push_back ({ 15, 31, 90 });
            groove.push_back ({ 10, 0, 100 }); groove.push_back ({ 11, 8, 80 });

            for (int k = 0; k < numKits; ++k)
            {
                const auto start = juce::Time::getMillisecondCounterHiRes();
                drums.setKit (k);
                const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
                if (ms > 2500.0) slow << DrumKits::kits()[(size_t) k].name << " (" << juce::roundToInt (ms) << " ms) ";

                for (int p = 0; p < DrumsDevice::numPads; ++p)
                {
                    auto snd = drums.getSound (p);
                    if (snd == nullptr || snd->audio.getMagnitude (0, snd->audio.getNumSamples()) < 0.5f)
                        quiet << DrumKits::kits()[(size_t) k].name << "/" << drums.getPadDisplayName (p) << " ";
                }

                flush();
                const double step = 60.0 / 100.0 / 4.0;
                std::vector<std::pair<int, juce::MidiMessage>> midi;
                for (auto& g : groove)
                    midi.push_back ({ (int) (g.step * step * sr), juce::MidiMessage::noteOn (10, DrumsDevice::firstNote + g.pad, (juce::uint8) g.vel) });
                std::sort (midi.begin(), midi.end(), [] (auto& a, auto& b) { return a.first < b.first; });
                juce::AudioBuffer<float> beat (2, (int) (32 * step * sr + sr * 1.5));
                beat.clear();
                process (drums, beat, midi);
                const float peak = beat.getMagnitude (0, beat.getNumSamples());
                if (peak > 0.0f) beat.applyGain (0.9f / juce::jmax (peak, 0.9f));
                writeWav (outDir.getChildFile ("kit-" + juce::String (DrumKits::kits()[(size_t) k].id) + ".wav"), beat);
                if (! isFinite (beat)) quiet << DrumKits::kits()[(size_t) k].name << " (not finite) ";
            }
            check (r, quiet.isEmpty(), "all " + juce::String (numKits) + " kits fill all 16 pads" + (quiet.isEmpty() ? juce::String() : ": " + quiet));
            check (r, slow.isEmpty(), "every kit builds in under 2.5 s" + (slow.isEmpty() ? juce::String() : ": " + slow));

            // Recorded kits: a soft hit plays a softer recording, not just the loud one turned down
            {
                drums.setKit (DrumKits::indexOf ("studiodry"));
                auto snd = drums.getSound (1);
                const bool layered = snd != nullptr && snd->layers.size() == 3;
                auto crest = [&] (int vel)
                {
                    flush();
                    juce::AudioBuffer<float> b (2, (int) (sr * 0.4));
                    b.clear();
                    process (drums, b, { { 0, juce::MidiMessage::noteOn (10, 37, (juce::uint8) vel) } });
                    return b.getMagnitude (0, 0, b.getNumSamples()) / juce::jmax (1.0e-9f, b.getRMSLevel (0, 0, b.getNumSamples()));
                };
                const auto softCrest = crest (40), hardCrest = crest (127);
                check (r, layered && std::abs (softCrest - hardCrest) > 0.05f,
                       "recorded snare has 3 velocity layers, and soft and hard hits differ in shape, not just level (crest "
                       + juce::String (softCrest, 2) + " vs " + juce::String (hardCrest, 2) + ")");
            }

            // Round robin: three snare hits in a row are not identical
            drums.setKit (DrumKits::indexOf ("trap"));
            flush();
            juce::AudioBuffer<float> rr (2, (int) (sr * 0.9));
            rr.clear();
            process (drums, rr, { { 0, juce::MidiMessage::noteOn (10, 37, (juce::uint8) 110) },
                                  { (int) (0.3 * sr), juce::MidiMessage::noteOn (10, 37, (juce::uint8) 110) },
                                  { (int) (0.6 * sr), juce::MidiMessage::noteOn (10, 37, (juce::uint8) 110) } });
            double diff = 0.0;
            for (int i = 0; i < (int) (0.1 * sr); ++i)
                diff += std::abs (rr.getSample (0, i) - rr.getSample (0, i + (int) (0.3 * sr)));
            check (r, diff > 1.0, "round robin: repeated hits use different takes");

            // Softer hits are darker: high frequencies drop more than the level does
            auto brightness = [&] (int vel)
            {
                flush();
                juce::AudioBuffer<float> b (2, (int) (sr * 0.3));
                b.clear();
                process (drums, b, { { 0, juce::MidiMessage::noteOn (10, 37, (juce::uint8) vel) } });
                juce::AudioBuffer<float> hi; hi.makeCopyOf (b);
                juce::IIRFilter f; f.setCoefficients (juce::IIRCoefficients::makeHighPass (sr, 4000.0));
                f.processSamples (hi.getWritePointer (0), hi.getNumSamples());
                return juce::Decibels::gainToDecibels (hi.getRMSLevel (0, 0, hi.getNumSamples()) / juce::jmax (1.0e-9f, b.getRMSLevel (0, 0, b.getNumSamples())));
            };
            const auto hard = brightness (127), soft = brightness (35);
            check (r, soft < hard - 3.0f, "softer hits are darker (" + juce::String (hard - soft, 1) + " dB less top end)");

            // Tone, drive, space and crush all the way: still finite and sane
            for (int p = 0; p < DrumsDevice::numPads; ++p) { setParam (drums.pads[(size_t) p].tone, p % 2 == 0 ? -100.0f : 100.0f); setParam (drums.pads[(size_t) p].drive, 100.0f); }
            setParam (drums.space, 100.0f); setParam (drums.crush, 100.0f);
            flush();
            juce::AudioBuffer<float> hot (2, (int) (sr * 6.0));
            hot.clear();
            process (drums, hot, notes);
            check (r, isFinite (hot) && hot.getMagnitude (0, hot.getNumSamples()) < 8.0f, "tone, drive, space and crush at the extremes stay stable");
            for (int p = 0; p < DrumsDevice::numPads; ++p) { setParam (drums.pads[(size_t) p].tone, 0.0f); setParam (drums.pads[(size_t) p].drive, 0.0f); }
            setParam (drums.space, 0.0f); setParam (drums.crush, 0.0f);

            drums.setKit (DrumKits::indexOf ("amapiano"));
            drums.state.setProperty ("view", "pads", nullptr);
            snapshot (drums, outDir.getChildFile ("drums-pads.png"));
            snapshotWindow (drums, outDir.getChildFile ("drums-pads-window.png"));
            drums.state.setProperty ("view", "kit", nullptr);
            snapshot (drums, outDir.getChildFile ("drums-kitview.png"));
            snapshotWindow (drums, outDir.getChildFile ("drums-kitview-window.png"));
            Theme::apply (Themes::light); refreshLookAndFeel();
            snapshot (drums, outDir.getChildFile ("drums-light.png"));
            Theme::apply (Themes::dark); refreshLookAndFeel();
            drums.setKit (0);
        }
    }

    //==============================================================================
    r.log << "Amp\n";
    {
        auto& amp = make<AmpDevice> (*edit, AmpDevice::xmlTypeName);

        // Harmonic content: energy outside the fundamental, relative to the total
        auto thd = [] (const juce::AudioBuffer<float>& b, double freq)
        {
            // Remove the fundamental with a narrow notch (Goertzel-style projection)
            auto n = b.getNumSamples() / 2, start = b.getNumSamples() - n;
            double sc = 0, cc = 0, total = 0;
            for (int i = start; i < b.getNumSamples(); ++i)
            {
                auto ph = juce::MathConstants<double>::twoPi * freq * i / sr;
                auto x = (double) b.getSample (0, i);
                sc += x * std::sin (ph); cc += x * std::cos (ph); total += x * x;
            }
            auto fundamental = 2.0 * (sc * sc + cc * cc) / n;
            return std::sqrt (juce::jmax (0.0, total - fundamental) / juce::jmax (1e-12, total));
        };

        auto run = [&] (int model, float gainKnob, int cabinet, double freq, float amp_)
        {
            setParam (amp.model, (float) model);
            setParam (amp.gain, gainKnob);
            setParam (amp.cab, (float) cabinet);
            setParam (amp.room, 0.0f);
            amp.initialise ({ te::TimePosition(), sr, block });   // rebuild the cab IR (the app does this on a timer)
            auto b = sine (freq, amp_, 1.0);
            process (amp, b);
            return b;
        };

        auto cleanOut = run (AmpDevice::clean, 2.0f, AmpDevice::cabOff, 220.0, 0.05f);
        auto heavyOut = run (AmpDevice::heavy, 8.0f, AmpDevice::cabOff, 220.0, 0.05f);
        auto thdClean = thd (cleanOut, 220.0), thdHeavy = thd (heavyOut, 220.0);
        check (r, thdClean < 0.05, "clean amp at low gain stays clean (THD " + juce::String (thdClean * 100, 1) + " %)");
        check (r, thdHeavy > 0.2, "heavy amp distorts (THD " + juce::String (thdHeavy * 100, 1) + " %)");

        bool allOk = true;
        float worst = 0.0f;
        for (int m = 0; m < AmpDevice::numModels; ++m)
            for (int c = 0; c < AmpDevice::numCabs; ++c)
            {
                auto b = run (m, 10.0f, c, 110.0, 0.8f);
                worst = juce::jmax (worst, b.getMagnitude (0, b.getNumSamples()));
                allOk = allOk && isFinite (b);
            }
        check (r, allOk && worst < 1.0f, "every amp/cab combination is stable at full gain (loudest peak "
                                           + juce::String (juce::Decibels::gainToDecibels (worst), 1) + " dBFS)");

        auto noCab = run (AmpDevice::clean, 2.0f, AmpDevice::cabOff, 9000.0, 0.05f);
        auto withCab = run (AmpDevice::clean, 2.0f, AmpDevice::cab4x12, 9000.0, 0.05f);
        auto cabCut = tailRmsDb (withCab) - tailRmsDb (noCab);
        check (r, cabCut < -20.0f, "4x12 cab rolls off 9 kHz (" + juce::String (cabCut, 1) + " dB)");
        check (r, amp.getLatencySeconds() > 0.0 && amp.getLatencySeconds() < 0.005, "reports its oversampling latency ("
                     + juce::String (amp.getLatencySeconds() * 1000.0, 2) + " ms)");

        setParam (amp.model, (float) AmpDevice::crunch);
        setParam (amp.gain, 6.0f);
        setParam (amp.cab, (float) AmpDevice::cab2x12);
        setParam (amp.room, 15.0f);
        juce::AudioBuffer<float> riff (2, (int) (sr * 3));
        for (int i = 0; i < riff.getNumSamples(); ++i)
        {
            auto t = i / sr;
            auto note = std::fmod (t, 0.5) < 0.25 ? 82.4 : 110.0;   // E2 / A2 power-chord-ish
            auto env = std::exp (-std::fmod (t, 0.25) * 6.0);
            auto v = (float) (0.2 * env * (std::sin (juce::MathConstants<double>::twoPi * note * t) + 0.5 * std::sin (juce::MathConstants<double>::twoPi * note * 1.5 * t)));
            riff.setSample (0, i, v); riff.setSample (1, i, v);
        }
        process (amp, riff);
        writeWav (outDir.getChildFile ("amp-crunch.wav"), riff);
        snapshot (amp, outDir.getChildFile ("amp.png"));

        // --- Cabinet impulse responses ---
        {
            // Convolver against direct convolution, odd block sizes
            juce::Random rng (3);
            std::vector<float> ir (1000), x (6000), y (6000, 0.0f);
            for (auto& v : ir) v = rng.nextFloat() * 2.0f - 1.0f;
            for (auto& v : x) v = rng.nextFloat() * 2.0f - 1.0f;
            Convolver conv;
            conv.setIrNow (Convolver::makeIr (ir.data(), (int) ir.size()));
            auto processed = x;
            for (int pos = 0; pos < (int) processed.size(); pos += 37)
                conv.process (processed.data() + pos, juce::jmin (37, (int) processed.size() - pos));

            double maxErr = 0.0;
            const int lat = Convolver::partition;
            for (int n = lat; n < (int) x.size(); ++n)
            {
                double ref = 0.0;
                for (int k = 0; k < (int) ir.size() && k <= n - lat; ++k) ref += (double) ir[(size_t) k] * x[(size_t) (n - lat - k)];
                maxErr = juce::jmax (maxErr, std::abs (ref - processed[(size_t) n]));
            }
            check (r, maxErr < 1.0e-3, "convolution matches direct convolution (max error " + juce::String (maxErr, 6) + ")");

            // Generated cab IRs
            auto energyUpTo = [] (const std::vector<float>& v, int n) { double e = 0; for (int i = 0; i < n && i < (int) v.size(); ++i) e += v[(size_t) i] * v[(size_t) i]; return e; };
            auto band = [] (const std::vector<float>& v, double lo, double hi)
            {
                Fft fft (14);
                std::vector<std::complex<float>> s ((size_t) fft.size());
                for (size_t i = 0; i < v.size() && i < s.size(); ++i) s[i] = v[i];
                fft.forward (s.data());
                double e = 0; int c = 0;
                for (int k = 1; k < fft.size() / 2; ++k) { auto f = k * sr / fft.size(); if (f >= lo && f < hi) { e += std::norm (s[(size_t) k]); ++c; } }
                return 10.0 * std::log10 (e / juce::jmax (1, c) + 1e-20);
            };

            bool minPhase = true;
            for (auto c : { AmpDevice::cab1x12, AmpDevice::cab2x12, AmpDevice::cab4x12, AmpDevice::cabBass })
            {
                auto irC = AmpDevice::makeCabIr (c, 0.3f, sr);
                minPhase = minPhase && energyUpTo (irC, (int) (0.003 * sr)) > 0.8 * energyUpTo (irC, (int) irC.size());
            }
            check (r, minPhase, "built-in cab IRs are minimum phase (80 % of the energy in the first 3 ms)");

            auto ir412 = AmpDevice::makeCabIr (AmpDevice::cab4x12, 0.3f, sr);
            auto roll = band (ir412, 8000, 10000) - band (ir412, 1000, 2000);
            check (r, roll < -20.0, "4x12 IR rolls off the top (" + juce::String (roll, 1) + " dB at 8-10 kHz vs 1-2 kHz)");

            auto centre = AmpDevice::makeCabIr (AmpDevice::cab4x12, 0.0f, sr), edgeMic = AmpDevice::makeCabIr (AmpDevice::cab4x12, 1.0f, sr);
            auto darker = band (edgeMic, 3000, 8000) - band (centre, 3000, 8000);
            check (r, darker < -3.0, "mic at the cone's edge is darker than the centre (" + juce::String (darker, 1) + " dB above 3 kHz)");

            // Custom IR file at another sample rate (44.1 kHz), resampled to ours
            auto irFile = outDir.getChildFile ("test-cab-ir.wav");
            {
                irFile.deleteFile();
                juce::AudioBuffer<float> b (1, 8820);   // 200 ms at 44.1 kHz
                juce::Random rr (9);
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, (rr.nextFloat() * 2.0f - 1.0f) * std::exp ((float) -i / 800.0f));
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> os = irFile.createOutputStream();
                if (auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (24)))
                    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            }
            amp.loadCustomIr (irFile);
            auto loaded = amp.getCurrentIr();
            check (r, juce::roundToInt (amp.cab.get()) == AmpDevice::cabCustom && std::abs ((int) loaded.size() - 9600) <= 2 && ! amp.isCustomIrMissing(),
                   "custom IR loads and is resampled 44.1 -> 48 kHz (" + juce::String ((int) loaded.size()) + " samples)");

            auto b = sine (220.0, 0.2f, 0.5);
            process (amp, b);
            check (r, isFinite (b) && b.getMagnitude (0, b.getNumSamples()) < 1.0f && b.getRMSLevel (0, 0, b.getNumSamples()) > 0.001f, "plays through the custom IR");

            setParam (amp.cab, (float) AmpDevice::cab4x12);
            amp.initialise ({ te::TimePosition(), sr, block });
            snapshot (amp, outDir.getChildFile ("amp.png"));
            snapshotWindow (amp, outDir.getChildFile ("amp-window.png"));
            Theme::apply (Themes::light); refreshLookAndFeel();
            snapshot (amp, outDir.getChildFile ("amp-light.png"));
            Theme::apply (Themes::dark); refreshLookAndFeel();
        }
    }

    //==============================================================================
    r.log << "Master\n";
    {
        // Loudness meter: a 1 kHz sine at -20 dBFS peak in both channels reads -20 LUFS
        {
            LoudnessMeter meter;
            meter.prepare (sr);
            auto s = sine (997.0, 0.1f, 10.0);
            meter.process (s.getReadPointer (0), s.getReadPointer (1), s.getNumSamples());
            check (r, std::abs (meter.getIntegrated() + 20.0) < 0.2, "LUFS meter: -20 dBFS sine reads " + juce::String (meter.getIntegrated(), 2) + " LUFS");
        }

        auto& master = make<MasterDevice> (*edit, MasterDevice::xmlTypeName);

        // A dull, quiet "mix": noise with too much low end and too little top, around -24 LUFS
        auto makeMix = [] (double seconds)
        {
            juce::AudioBuffer<float> b (2, (int) (seconds * sr));
            juce::Random rng (7);
            Biquad lowBoost, highCut;
            lowBoost.set (Biquad::Type::lowShelf, 150.0, 0.7, 8.0, sr);
            highCut.set (Biquad::Type::highShelf, 4000.0, 0.7, -14.0, sr);
            // Pink noise (music-like: equal energy per octave), Paul Kellet's filter
            double p[2][7] {};
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                // Drum-ish bursts every 0.5 s on top of the noise bed, so there are peaks to limit
                auto t = i / sr;
                auto burst = std::exp (-std::fmod (t, 0.5) * 30.0) * 3.0;
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto w = rng.nextDouble() * 2.0 - 1.0;
                    auto* q = p[ch];
                    q[0] = 0.99886 * q[0] + w * 0.0555179; q[1] = 0.99332 * q[1] + w * 0.0750759;
                    q[2] = 0.96900 * q[2] + w * 0.1538520; q[3] = 0.86650 * q[3] + w * 0.3104856;
                    q[4] = 0.55000 * q[4] + w * 0.5329522; q[5] = -0.7616 * q[5] - w * 0.0168980;
                    auto pink = (q[0] + q[1] + q[2] + q[3] + q[4] + q[5] + q[6] + w * 0.5362) * 0.11;
                    q[6] = w * 0.115926;
                    auto x = (float) (pink * 0.08 * (1.0 + burst));
                    b.setSample (ch, i, highCut.process (lowBoost.process (x, ch), ch));
                }
            }
            return b;
        };

        auto analysisMix = makeMix (20.0);
        LoudnessMeter before;
        before.prepare (sr);
        before.process (analysisMix.getReadPointer (0), analysisMix.getReadPointer (1), analysisMix.getNumSamples());

        master.startAnalysis();
        auto listened = analysisMix;
        process (master, listened);
        auto ok = master.finishAnalysis();
        check (r, ok, "assistant finishes its analysis");
        auto expectedGain = -14.0 - before.getIntegrated() + 0.5;
        check (r, std::abs (master.inputGain.get() - expectedGain) < 0.5, "sets gain " + juce::String (master.inputGain.get(), 1)
                     + " dB for a " + juce::String (before.getIntegrated(), 1) + " LUFS mix");
        check (r, master.eq[0].get() < -1.0f && master.eq[5].get() > 1.0f, "tames the boomy lows (" + juce::String (master.eq[0].get(), 1)
                     + " dB) and opens up the dull top (" + juce::String (master.eq[5].get(), 1) + " dB)");

        // Fine-tuning rounds (in the app these apply via the message loop)
        for (int round = 0; round < 4 && master.isFineTuning(); ++round)
        {
            auto chunk = makeMix (7.0);
            process (master, chunk);
            master.applyPendingTrim();
        }
        check (r, ! master.isFineTuning(), "loudness fine-tuning settles");

        auto mastered = makeMix (20.0);
        process (master, mastered);
        LoudnessMeter after;
        after.prepare (sr);
        after.process (mastered.getReadPointer (0), mastered.getReadPointer (1), mastered.getNumSamples());
        check (r, std::abs (after.getIntegrated() + 14.0) < 0.5, "mastered output is " + juce::String (after.getIntegrated(), 1) + " LUFS (target -14)");

        // True peak: measure 4x oversampled
        juce::dsp::Oversampling<float> os (2, 2, juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true);
        os.initProcessing ((size_t) mastered.getNumSamples());
        juce::dsp::AudioBlock<float> blk (mastered);
        auto up = os.processSamplesUp (blk);
        float tp = 0.0f;
        for (size_t ch = 0; ch < 2; ++ch)
            for (size_t i = 0; i < up.getNumSamples(); ++i)
                tp = juce::jmax (tp, std::abs (up.getSample ((int) ch, (int) i)));
        check (r, juce::Decibels::gainToDecibels (tp) < -0.95f, "true peak stays under the -1 dBTP ceiling ("
                     + juce::String (juce::Decibels::gainToDecibels (tp), 2) + " dBTP)");
        check (r, isFinite (mastered), "no NaN/inf");
        writeWav (outDir.getChildFile ("master-after.wav"), makeMix (10.0));   // reference input
        {
            auto out = makeMix (10.0);
            process (master, out);
            writeWav (outDir.getChildFile ("master-before-after.wav"), out);
        }

        // Width 0 = mono
        setParam (master.width, 0.0f);
        auto st = makeMix (1.0);
        process (master, st);
        float diff = 0.0f;
        for (int i = st.getNumSamples() / 2; i < st.getNumSamples(); ++i)
            diff = juce::jmax (diff, std::abs (st.getSample (0, i) - st.getSample (1, i)));
        check (r, diff < 1.0e-4f, "width 0 % folds to mono");
        setParam (master.width, 100.0f);

        for (int c = 0; c < MasterDevice::numCharacters; ++c)
        {
            setParam (master.character, (float) c);
            auto b = makeMix (2.0);
            process (master, b);
            check (r, isFinite (b) && b.getMagnitude (0, b.getNumSamples()) <= 1.0f, MasterDevice::characterNames()[c] + " character is stable");
        }
        setParam (master.character, 0.0f);
        snapshot (master, outDir.getChildFile ("master.png"));
        snapshotWindow (master, outDir.getChildFile ("master-window.png"));
    }

    //==============================================================================
    r.log << "Mono\n";
    {
        auto& mono = make<MonoDevice> (*edit, MonoDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto frequency = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            double first = -1, last = -1; int n = 0;
            for (int i = from + 1; i < to; ++i)
            {
                auto a = b.getSample (0, i - 1), c = b.getSample (0, i);
                if (a < 0.0f && c >= 0.0f)
                {
                    auto t = (double) (i - 1) + a / (a - c);
                    if (first < 0) first = t; else ++n;
                    last = t;
                }
            }
            return n > 0 ? n * sr / (last - first) : 0.0;
        };
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, int from, int len)
        {
            return juce::Decibels::gainToDecibels (b.getRMSLevel (0, from, len), -200.0f);
        };

        // A clean, open voice: oscillator 1 only, filter wide open, no drift
        auto clean = [&]
        {
            setParam (mono.cutoff, 20000.0f); setParam (mono.emphasis, 0.0f); setParam (mono.contour, 0.0f);
            setParam (mono.keyTrack, 0.0f); setParam (mono.drift, 0.0f); setParam (mono.drive, 0.0f);
            setParam (mono.mixOn[1], 0.0f); setParam (mono.mixOn[2], 0.0f); setParam (mono.mix[0], 6.0f);
            setParam (mono.aSustain, 10.0f); setParam (mono.glide, 0.0f); setParam (mono.oscs[0].range, 3.0f);
        };
        clean();

        setParam (mono.oscs[0].wave, 0.0f);   // triangle
        juce::AudioBuffer<float> b (2, (int) sr);
        b.clear();
        process (mono, b, { { 0, on (57) } });
        auto f = frequency (b, (int) (0.2 * sr), (int) sr);
        check (r, std::abs (f - 220.0) < 1.0, "8' plays A3 at " + juce::String (f, 2) + " Hz");

        setParam (mono.oscs[0].range, 2.0f);   // 16'
        b.clear();
        process (mono, b, { { 0, off (57) }, { 1, on (57) } });
        f = frequency (b, (int) (0.3 * sr), (int) sr);
        check (r, std::abs (f - 110.0) < 0.6, "16' is an octave down (" + juce::String (f, 2) + " Hz)");
        setParam (mono.oscs[0].range, 3.0f);

        // Alias-free: a sawtooth at G7 (3.1 kHz); anything below 1 kHz is aliasing
        {
            setParam (mono.oscs[0].wave, 2.0f);
            juce::AudioBuffer<float> hi (2, (int) sr);
            hi.clear();
            process (mono, hi, { { 0, off (57) }, { 1, on (103) } });
            const auto total = rmsDb (hi, (int) (0.3 * sr), (int) (0.6 * sr));
            for (int k = 0; k < 4; ++k)
            {
                juce::IIRFilter lp;
                lp.setCoefficients (juce::IIRCoefficients::makeLowPass (sr, 1000.0));
                lp.processSamples (hi.getWritePointer (0), hi.getNumSamples());
            }
            const auto aliasing = rmsDb (hi, (int) (0.3 * sr), (int) (0.6 * sr)) - total;
            check (r, aliasing < -50.0f, "sawtooth at 3.1 kHz is alias-free (aliasing " + juce::String (aliasing, 1) + " dB)");
            b.clear();
            process (mono, b, { { 0, off (103) } });
        }

        // Filter self-oscillation: no oscillators, emphasis at 10, cutoff 1 kHz
        auto selfOsc = [&] (int note)
        {
            setParam (mono.mixOn[0], 0.0f);
            setParam (mono.emphasis, 10.0f);
            setParam (mono.cutoff, 1000.0f);
            juce::AudioBuffer<float> s (2, (int) sr);
            s.clear();
            process (mono, s, { { 0, on (note) } });
            auto fr = frequency (s, (int) (0.4 * sr), (int) sr);
            auto lvl = rmsDb (s, (int) (0.4 * sr), (int) (0.5 * sr));
            juce::AudioBuffer<float> rel (2, (int) (sr * 0.5));
            rel.clear();
            process (mono, rel, { { 0, off (note) } });
            return std::pair { fr, lvl };
        };
        auto [oscF, oscLvl] = selfOsc (60);
        check (r, oscF > 900.0 && oscF < 1100.0 && oscLvl > -30.0f, "filter self-oscillates at the cutoff (" + juce::String (oscF, 1) + " Hz, " + juce::String (oscLvl, 1) + " dB)");

        setParam (mono.keyTrack, 3.0f);
        auto lowF = selfOsc (60).first, highF = selfOsc (72).first;
        check (r, std::abs (highF / lowF - 2.0) < 0.1, "full keyboard control tracks the filter an octave per octave (" + juce::String (highF / lowF, 3) + "x)");
        setParam (mono.keyTrack, 0.0f);
        clean();
        setParam (mono.mixOn[0], 1.0f);
        setParam (mono.oscs[0].wave, 0.0f);

        // Glide: hold C3, then C4 legato; halfway there shortly after, arrived later
        {
            setParam (mono.glide, 0.2f);
            juce::AudioBuffer<float> g (2, (int) (sr * 2.0));
            g.clear();
            process (mono, g, { { 0, on (48) }, { (int) (0.5 * sr), on (60) } });
            const auto during = frequency (g, (int) (0.52 * sr), (int) (0.56 * sr));
            const auto after = frequency (g, (int) (1.6 * sr), (int) (1.95 * sr));
            check (r, during > 140.0 && during < 250.0 && std::abs (after - 261.63) < 2.0,
                   "glide moves from C3 to C4 (" + juce::String (during, 1) + " Hz on the way, " + juce::String (after, 1) + " Hz after)");
            juce::AudioBuffer<float> rel (2, (int) (sr * 0.5));
            rel.clear();
            process (mono, rel, { { 0, off (48) }, { 1, off (60) } });
            setParam (mono.glide, 0.0f);
        }

        // Every factory patch: plays, stays clean
        {
            juce::String quiet;
            bool ok = true;
            for (int i = 0; i < (int) MonoDevice::patches().size(); ++i)
            {
                mono.loadPatch (i);
                juce::AudioBuffer<float> p (2, (int) (sr * 1.5));
                p.clear();
                process (mono, p, { { 0, on (48) }, { (int) (0.4 * sr), on (55) }, { (int) (0.9 * sr), off (55) }, { (int) (1.0 * sr), off (48) } });
                const auto lvl = rmsDb (p, 0, (int) (1.0 * sr));
                if (! isFinite (p) || p.getMagnitude (0, p.getNumSamples()) > 1.01f || lvl < -45.0f)
                {
                    ok = false;
                    quiet << MonoDevice::patches()[(size_t) i].name << " (" << juce::String (lvl, 1) << " dB) ";
                }
                juce::AudioBuffer<float> tail (2, (int) (sr * 3.5));
                process (mono, tail);
            }
            check (r, ok, "all " + juce::String ((int) MonoDevice::patches().size()) + " factory patches play cleanly" + (quiet.isEmpty() ? juce::String() : ": " + quiet));
        }

        // Everything pushed: feedback, drive, emphasis and mixer at maximum
        {
            for (int i = 0; i < 3; ++i) { setParam (mono.mix[(size_t) i], 10.0f); setParam (mono.mixOn[(size_t) i], 1.0f); }
            setParam (mono.feedback, 10.0f); setParam (mono.feedbackOn, 1.0f); setParam (mono.drive, 100.0f); setParam (mono.emphasis, 10.0f);
            setParam (mono.noise, 10.0f); setParam (mono.noiseOn, 1.0f);
            juce::AudioBuffer<float> hot (2, (int) (sr * 2.0));
            hot.clear();
            process (mono, hot, { { 0, on (36) }, { (int) (1.0 * sr), on (84) } });
            check (r, isFinite (hot) && hot.getMagnitude (0, hot.getNumSamples()) <= 1.0f, "maxed out (feedback, drive, emphasis) stays stable and under 0 dBFS");
            juce::AudioBuffer<float> rel (2, (int) (sr * 3.0));
            process (mono, rel, { { 0, off (36) }, { 1, off (84) } });
        }

        // The ladder, measured directly: 24 dB per octave above the cutoff, a resonant peak at it
        {
            const float rate = (float) sr * 4.0f;
            auto gainAt = [&] (float freq, float res)
            {
                SynthCore::MoogLadder lad;
                lad.set (500.0f, res, rate);
                double e = 0.0;
                const int n = (int) (rate * 0.5f);
                for (int i = 0; i < n; ++i)
                {
                    const float y = lad.process (0.05f * std::sin (juce::MathConstants<float>::twoPi * freq * (float) i / rate));
                    if (i > n / 2) e += y * y;
                }
                return 10.0 * std::log10 (e / (n / 2) / (0.05 * 0.05 / 2.0));
            };
            const double slope = gainAt (2000.0f, 0.0f) - gainAt (4000.0f, 0.0f);
            const double peak = gainAt (500.0f, 0.85f) - gainAt (500.0f, 0.0f);
            check (r, slope > 21.0 && slope < 27.0 && peak > 6.0,
                   "transistor ladder: " + juce::String (slope, 1) + " dB per octave above the cutoff, resonance lifts the cutoff " + juce::String (peak, 1) + " dB");
        }

        // The mixer overloads into the filter when it's pushed: louder, but less than the levels say, and richer
        {
            mono.loadPatch (MonoDevice::patchesFor (MonoDevice::seventy)[0]);
            for (auto& p : { std::pair<ElsewhereDevice::Param, float> { mono.drift, 0.0f }, { mono.cutoff, 6000.0f }, { mono.emphasis, 0.0f }, { mono.contour, 0.0f },
                             { mono.aSustain, 10.0f }, { mono.drive, 40.0f }, { mono.character, 100.0f } })
                setParam (p.first, p.second);
            auto at = [&] (float lvl)
            {
                for (int i = 0; i < 3; ++i) { setParam (mono.mix[(size_t) i], lvl); setParam (mono.mixOn[(size_t) i], 1.0f); }
                juce::AudioBuffer<float> b (2, (int) (sr * 0.8));
                b.clear();
                process (mono, b, { { 0, on (45) } });
                juce::AudioBuffer<float> rel (2, (int) (sr * 0.5));
                process (mono, rel, { { 0, off (45) } });
                return tailRmsDb (b);
            };
            const auto quiet = at (3.0f), pushed = at (10.0f);
            auto lvl = [] (float t) { const float x = t / 10.0f; return x * x * 0.6f + x * 0.4f; };   // the mixer's level curve
            const auto linear = 20.0f * std::log10 (lvl (10.0f) / lvl (3.0f));
            check (r, pushed > quiet + 3.0f && pushed - quiet < linear - 4.0f,
                   "the mixer overloads into the filter: levels 3 to 10 is +" + juce::String (pushed - quiet, 1) + " dB (clean would be +" + juce::String (linear, 1) + ")");
        }

        // A few Seventy patches to listen to
        for (auto name : { "Fat Stack", "Funk Bass", "Prog Lead", "Mixer Overload" })
        {
            for (int i = 0; i < (int) MonoDevice::patches().size(); ++i)
                if (juce::String (MonoDevice::patches()[(size_t) i].name) == name)
                    mono.loadPatch (i);
            const int line[] = { 33, 33, 45, 33, 36, 38, 40, 43 };
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            for (int k = 0; k < 8; ++k)
            {
                const int note = juce::String (name) == "Prog Lead" ? line[k] + 36 : line[k] + 12;
                midi.push_back ({ (int) (k * sr * 0.3), on (note, 100) });
                midi.push_back ({ (int) (k * sr * 0.3 + sr * 0.24), off (note) });
            }
            juce::AudioBuffer<float> b (2, (int) (sr * 3.2));
            b.clear();
            process (mono, b, midi);
            writeWav (outDir.getChildFile ("mono-" + juce::String (name).toLowerCase().replaceCharacter (' ', '-') + ".wav"), b);
        }

        // Acid: accents are louder; overlapping notes slide, separate notes jump
        {
            mono.loadPatch (MonoDevice::patchesFor (MonoDevice::acid)[0]);
            setParam (mono.drift, 0.0f);
            auto level = [&] (int vel)
            {
                juce::AudioBuffer<float> a (2, (int) (sr * 0.4));
                a.clear();
                process (mono, a, { { 0, on (45, vel) }, { (int) (0.3 * sr), off (45) } });
                juce::AudioBuffer<float> rest (2, (int) (sr * 0.3));
                process (mono, rest);
                return rmsDb (a, 0, (int) (0.25 * sr));
            };
            auto soft = level (70), hard = level (127);
            check (r, hard > soft + 3.0f, "acid accent is louder (+" + juce::String (hard - soft, 1) + " dB)");

            setParam (mono.acRes, 0.0f); setParam (mono.acEnvMod, 0.0f); setParam (mono.acCutoff, 8000.0f); setParam (mono.acSlide, 0.1f);
            juce::AudioBuffer<float> sl (2, (int) sr);
            sl.clear();
            process (mono, sl, { { 0, on (45) }, { (int) (0.4 * sr), on (57) }, { (int) (0.41 * sr), off (45) } });
            const auto mid = frequency (sl, (int) (0.42 * sr), (int) (0.45 * sr)), end = frequency (sl, (int) (0.8 * sr), (int) (0.98 * sr));
            juce::AudioBuffer<float> rel (2, (int) (sr * 0.3));
            process (mono, rel, { { 0, off (57) } });
            check (r, mid > 115.0 && mid < 210.0 && std::abs (end - 220.0) < 2.0,
                   "acid slide glides between overlapping notes (" + juce::String (mid, 1) + " Hz on the way, " + juce::String (end, 1) + " Hz after)");
        }

        // Sync Lead: oscillator B synced to A takes A's pitch
        {
            mono.loadPatch (MonoDevice::patchesFor (MonoDevice::syncLead)[0]);
            setParam (mono.drift, 0.0f); setParam (mono.syMixA, 0.0f); setParam (mono.syMixB, 8.0f);
            setParam (mono.syEnvToB, 0.0f); setParam (mono.syBPitch, 7.0f); setParam (mono.syCutoff, 20000.0f);
            setParam (mono.syEnvAmt, 0.0f); setParam (mono.syRes, 0.0f); setParam (mono.syAS, 10.0f);
            auto played = [&] (bool syncOn)
            {
                setParam (mono.sySync, syncOn ? 1.0f : 0.0f);
                juce::AudioBuffer<float> sb (2, (int) sr);
                sb.clear();
                process (mono, sb, { { 0, on (57) } });
                juce::AudioBuffer<float> rel (2, (int) (sr * 0.5));
                process (mono, rel, { { 0, off (57) } });
                return yinPitch (sb, (int) (0.3 * sr));
            };
            const auto synced = played (true), free = played (false);
            check (r, std::abs (synced - 220.0) < 2.0 && std::abs (free - 220.0 * std::pow (2.0, 7.0 / 12.0)) < 3.0,
                   "hard sync locks oscillator B to A (" + juce::String (synced, 1) + " Hz synced, " + juce::String (free, 1) + " Hz free)");
        }

        mono.loadPatch (MonoDevice::patchesFor (MonoDevice::acid)[0]);
        snapshot (mono, outDir.getChildFile ("mono-acid.png"));
        snapshotWindow (mono, outDir.getChildFile ("mono-acid-window.png"));
        mono.loadPatch (MonoDevice::patchesFor (MonoDevice::syncLead)[0]);
        snapshot (mono, outDir.getChildFile ("mono-sync.png"));
        snapshotWindow (mono, outDir.getChildFile ("mono-sync-window.png"));

        mono.loadPatch (1);
        juce::AudioBuffer<float> warm (2, (int) (sr * 0.5));
        warm.clear();
        process (mono, warm, { { 0, on (57) } });
        snapshot (mono, outDir.getChildFile ("mono.png"));
        snapshotWindow (mono, outDir.getChildFile ("mono-window.png"));
        Theme::apply (Themes::light); refreshLookAndFeel();
        snapshot (mono, outDir.getChildFile ("mono-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
    }

    //==============================================================================
    r.log << "Mono engine\n";
    {
        // The device is MonoEngine: the same notes through the engine on its own come out the
        // same, sample for sample, for each model (glide or slide, bend and mod wheel included)
        auto& md = make<MonoDevice> (*edit, MonoDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto reset = [&] { md.initialise ({ te::TimePosition(), sr, block }); };
        const int s1 = (int) sr;

        auto renderEngine = [&] (MonoEngine& e, const std::vector<std::pair<int, juce::MidiMessage>>& midi, int numSamples)
        {
            juce::AudioBuffer<float> out (2, numSamples);
            out.clear();
            auto* L = out.getWritePointer (0);
            auto* R = out.getWritePointer (1);

            // In the same pieces the device renders in: blocks, split at each message
            for (int start = 0; start < numSamples; start += block)
            {
                const int n = juce::jmin (block, numSamples - start);
                int done = 0;
                for (auto& [pos, m] : midi)
                {
                    if (pos < start || pos >= start + n) continue;
                    if (pos - start > done) { e.render (L + start + done, R + start + done, pos - start - done); done = pos - start; }
                    if (m.isNoteOn()) e.noteOn (m.getNoteNumber(), m.getFloatVelocity());
                    else if (m.isNoteOff()) e.noteOff (m.getNoteNumber());
                    else if (m.isController()) e.setModWheel ((float) m.getControllerValue() / 127.0f);
                    else if (m.isPitchWheel()) e.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
                }
                if (done < n) e.render (L + start + done, R + start + done, n - done);
            }
            return out;
        };

        for (int m = 0; m < MonoDevice::numModels; ++m)
        {
            const int patch = MonoDevice::patchesFor (m)[1];
            md.loadPatch (patch);
            if (m != MonoDevice::acid) setParam (md.glide, 0.12f);
            reset();
            std::vector<std::pair<int, juce::MidiMessage>> midi = {
                { 0, on (45) }, { 9000, on (52, 120) }, { 9500, off (45) },                 // legato: glide or slide
                { s1 / 2, juce::MidiMessage::controllerEvent (1, 1, 80) }, { s1 / 2 + 300, juce::MidiMessage::pitchWheel (1, 12000) },
                { s1, off (52) }, { s1 + 7000, on (57, 60) }, { s1 + 7000 + s1 / 3, off (57) },
                { s1 + 7000 + s1 / 3 + 100, juce::MidiMessage::pitchWheel (1, 8192) }, { s1 + 7000 + s1 / 3 + 100, juce::MidiMessage::controllerEvent (1, 1, 0) } };
            juce::AudioBuffer<float> viaDevice (2, (int) (sr * 2.2));
            viaDevice.clear();
            process (md, viaDevice, midi);

            MonoEngine e;
            e.setPatch (patch);
            if (m != MonoDevice::acid) e.params.glide = 0.12f;
            e.prepare (sr, block);
            auto viaEngine = renderEngine (e, midi, viaDevice.getNumSamples());

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < viaDevice.getNumSamples(); ++i)
                    worst = juce::jmax (worst, std::abs (viaDevice.getSample (ch, i) - viaEngine.getSample (ch, i)));
            check (r, worst == 0.0f && viaDevice.getMagnitude (0, viaDevice.getNumSamples()) > 0.05f,
                   juce::String (MonoEngine::modelName (m)) + ": the device plays MonoEngine, the engine on its own renders the same, sample for sample (worst "
                       + juce::String (worst) + ")");
        }
        setParam (md.glide, 0.0f);

        // prepare() starts it afresh
        {
            MonoEngine again;
            again.setPatch (MonoDevice::patchesFor (MonoDevice::seventy)[0]);
            again.prepare (sr);
            juce::AudioBuffer<float> b1 (2, s1), b2 (2, s1);
            b1.clear(); b2.clear();
            again.noteOn (48, 0.8f); again.render (b1.getWritePointer (0), b1.getWritePointer (1), s1);
            again.prepare (sr);
            again.noteOn (48, 0.8f); again.render (b2.getWritePointer (0), b2.getWritePointer (1), s1);
            bool same = true;
            for (int i = 0; i < s1; ++i) same = same && b1.getSample (0, i) == b2.getSample (0, i);
            check (r, same && b1.getMagnitude (0, s1) > 0.05f, "MonoEngine: prepare() starts it afresh, the same notes render the same audio every time");
        }

        bool samePatches = MonoEngine::patchCount() == (int) MonoDevice::patches().size() && MonoEngine::patchCount() == 29;
        for (int i = 0; i < MonoEngine::patchCount(); ++i)
            samePatches = samePatches && MonoEngine::patchModel (i) == MonoDevice::patches()[(size_t) i].model
                          && juce::String (MonoEngine::patchName (i)) == MonoDevice::patches()[(size_t) i].name;
        check (r, samePatches && juce::String (MonoEngine::patchName (17)) == "Acid Line", "MonoEngine has the device's patches");

        // A control written straight into params between renders is heard in the next render
        {
            auto run = [&] (bool change)
            {
                MonoEngine e;
                e.setPatch (0);
                e.prepare (sr);
                juce::AudioBuffer<float> b (2, s1);
                b.clear();
                e.noteOn (45, 0.8f);
                e.render (b.getWritePointer (0), b.getWritePointer (1), s1 / 2);
                if (change) { *e.params.find ("cutoff") = 3000.0f; *e.params.find ("emph") = 7.0f; }
                e.render (b.getWritePointer (0, s1 / 2), b.getWritePointer (1, s1 / 2), s1 / 2);
                return b;
            };
            auto a = run (false), b = run (true);
            bool beforeSame = true;
            float after = 0.0f;
            for (int i = 0; i < s1 / 2; ++i) beforeSame = beforeSame && a.getSample (0, i) == b.getSample (0, i);
            for (int i = s1 / 2; i < s1 / 2 + 2048; ++i) after = juce::jmax (after, std::abs (a.getSample (0, i) - b.getSample (0, i)));
            check (r, beforeSame && after > 0.01f, "MonoEngine: changing params between renders changes the next render (difference " + juce::String (after, 3) + ")");
        }

        // The controls: every one resolves, they cover every parameter, and match the device's ranges
        {
            MonoEngine::Params p;
            const MonoEngine::Params defaults;
            juce::String bad;
            std::set<juce::String> ids;
            for (auto& c : MonoEngine::controls())
            {
                ids.insert (c.id);
                auto* v = p.find (c.id);
                auto ap = md.getAutomatableParameterByID (c.id);
                if (v == nullptr || ap == nullptr) { bad << c.id << " (missing) "; continue; }
                const auto range = ap->getValueRange();
                if (std::abs (range.getStart() - c.min) > 1.0e-4f || std::abs (range.getEnd() - c.max) > 1.0e-4f
                     || std::abs (*v - c.defaultValue) > 1.0e-6f || std::abs (ap->getDefaultValue().value_or (-999.0f) - c.defaultValue) > 1.0e-6f
                     || (! c.choices.empty() && (int) c.choices.size() != juce::roundToInt (c.max - c.min) + 1))
                    bad << c.id << " ";
            }
            check (r, bad.isEmpty() && ids.size() == MonoEngine::Params::table().size(),
                   "MonoEngine: all " + juce::String ((int) ids.size()) + " controls resolve through Params::find and match the device's parameters" + (bad.isEmpty() ? juce::String() : ": " + bad));

            bool forOk = true;
            juce::String sizes;
            for (int m = 0; m < MonoEngine::numModels; ++m)
            {
                auto list = MonoEngine::controlsFor (m);
                bool hasGlide = false;
                for (auto* c : list)
                {
                    const juce::String section (juce::CharPointer_UTF8 (c->section));
                    hasGlide = hasGlide || juce::String (c->id) == "glide";
                    const bool shared = section == "Model" || section == "Output";
                    forOk = forOk && (shared || section.startsWith (MonoEngine::modelName (m)) || juce::String (c->id) == "glide");
                }
                forOk = forOk && list.size() > 9 && juce::String (list.front()->id) == "model" && hasGlide == (m != MonoEngine::acid);
                sizes << MonoEngine::modelName (m) << " " << (int) list.size() << "  ";
            }
            check (r, forOk, "MonoEngine: controlsFor gives each model the shared controls and its own (" + sizes.trim() + ")");
        }
    }

    //==============================================================================
    r.log << "Poly\n";
    {
        auto& pd = make<PolyDevice> (*edit, PolyDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto silence = [&] { juce::AudioBuffer<float> rest (2, (int) (sr * 3.0)); process (pd, rest, { { 0, juce::MidiMessage::allNotesOff (1) } }); };

        // Voice modes: how many voices one or three keys use
        auto voicesFor = [&] (int mode, std::vector<int> notes)
        {
            setParam (pd.voiceMode, (float) mode);
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            for (auto n : notes) midi.push_back ({ 0, on (n) });
            juce::AudioBuffer<float> b (2, (int) (sr * 0.2));
            b.clear();
            process (pd, b, midi);
            const int count = pd.getActiveVoices();
            silence();
            return count;
        };
        setParam (pd.voices, 8.0f);
        const int polyV = voicesFor (PolyDevice::poly, { 60, 64, 67 });
        const int uniV = voicesFor (PolyDevice::unison, { 60 });
        setParam (pd.chordType, 7.0f);   // Maj7: four notes
        const int chordV = voicesFor (PolyDevice::chord, { 60 });
        const int monoV = voicesFor (PolyDevice::mono, { 60, 64, 67 });
        check (r, polyV == 3 && uniV == 8 && chordV == 4 && monoV == 1,
               "voice modes: poly 3 keys = " + juce::String (polyV) + ", unison = " + juce::String (uniV) + ", Maj7 chord = " + juce::String (chordV) + ", mono = " + juce::String (monoV));
        setParam (pd.voiceMode, (float) PolyDevice::poly);

        setParam (pd.model, (float) PolyDevice::quad);
        const int quadV = voicesFor (PolyDevice::poly, { 48, 52, 55, 60, 64, 67 });
        check (r, quadV == 4, "Quad is a four-voice synth (" + juce::String (quadV) + " voices for six keys)");

        // Pitch in every model: a clean, open sound at A3
        juce::String pitches;
        bool pitchOk = true;
        for (int m = 0; m < PolyDevice::numModels; ++m)
        {
            pd.loadPatch (PolyDevice::patchesFor (m)[0]);
            setParam (pd.drift, 0.0f); setParam (pd.chorus, 0.0f); setParam (pd.voiceMode, (float) PolyDevice::poly);
            setParam (pd.pCutoff, 20000.0f); setParam (pd.pRes, 0.0f); setParam (pd.pMix2, 0.0f); setParam (pd.pMixM, 0.0f); setParam (pd.pWave1, 1.0f);
            setParam (pd.pAS, 10.0f); setParam (pd.pAA, 0.002f); setParam (pd.pLfoInt, 0.0f);
            setParam (pd.qLp, 20000.0f); setParam (pd.qLpRes, 0.0f); setParam (pd.qMix2, 0.0f); setParam (pd.qSub, 0.0f); setParam (pd.qWave1, 2.0f);
            setParam (pd.qAS, 10.0f); setParam (pd.qDrive, 0.0f); setParam (pd.qHp, 20.0f);
            setParam (pd.vCutoff, 20000.0f); setParam (pd.vRes, 0.0f); setParam (pd.vSub, 0.0f); setParam (pd.vPulse, 0.0f); setParam (pd.vS, 10.0f);
            setParam (pd.vA, 0.002f); setParam (pd.vHpf, 1.0f); setParam (pd.vLfoPitch, 0.0f);
            setParam (pd.nLpf, 18000.0f); setParam (pd.nRes, 0.0f); setParam (pd.nEnv, 0.0f); setParam (pd.nHpf, 20.0f); setParam (pd.nBalance, 0.0f);
            setParam (pd.nAfterVib, 0.0f); setParam (pd.nVib, 0.0f); setParam (pd.nAS, 10.0f); setParam (pd.nAA, 0.002f);
            juce::AudioBuffer<float> b (2, (int) sr);
            b.clear();
            process (pd, b, { { 0, on (57) } });
            auto f = yinPitch (b, (int) (0.4 * sr));
            pitchOk = pitchOk && std::abs (f - 220.0) < 1.5;
            pitches << PolyDevice::modelNames()[m] << " " << juce::String (f, 1) << " Hz  ";
            silence();
        }
        check (r, pitchOk, "every model plays A3 in tune (" + pitches.trim() + ")");

        // Every factory patch, played as a chord
        {
            juce::String bad;
            bool ok = true;
            for (int i = 0; i < (int) PolyDevice::patches().size(); ++i)
            {
                pd.loadPatch (i);
                juce::AudioBuffer<float> p (2, (int) (sr * 1.5));
                p.clear();
                process (pd, p, { { 0, on (48) }, { 0, on (55) }, { 0, on (64) }, { (int) (1.2 * sr), off (48) }, { (int) (1.2 * sr), off (55) }, { (int) (1.2 * sr), off (64) } });
                const auto lvl = juce::Decibels::gainToDecibels (p.getRMSLevel (0, 0, (int) (1.2 * sr)), -200.0f);
                if (! isFinite (p) || p.getMagnitude (0, p.getNumSamples()) > 1.0f || lvl < -45.0f)
                {
                    ok = false;
                    bad << PolyDevice::patches()[(size_t) i].name << " (" << juce::String (lvl, 1) << " dB) ";
                }
                silence();
            }
            check (r, ok, "all " + juce::String ((int) PolyDevice::patches().size()) + " factory patches play cleanly" + (bad.isEmpty() ? juce::String() : ": " + bad));
        }

        // Sixteen voices, everything up, in each model
        {
            bool ok = true;
            for (int m = 0; m < PolyDevice::numModels; ++m)
            {
                pd.loadPatch (PolyDevice::patchesFor (m)[0]);
                setParam (pd.voices, 16.0f); setParam (pd.pRes, 10.0f); setParam (pd.pDrive, 2.0f); setParam (pd.pXmod, 100.0f); setParam (pd.pMixM, 10.0f);
                setParam (pd.qLpRes, 10.0f); setParam (pd.qDrive, 100.0f); setParam (pd.qNoise, 10.0f); setParam (pd.vRes, 10.0f); setParam (pd.vSub, 10.0f);
                setParam (pd.volume, 6.0f);
                std::vector<std::pair<int, juce::MidiMessage>> midi;
                for (int n = 0; n < 16; ++n) midi.push_back ({ n * 100, on (36 + n * 3, 127) });
                juce::AudioBuffer<float> hot (2, (int) (sr * 2.0));
                hot.clear();
                process (pd, hot, midi);
                ok = ok && isFinite (hot) && hot.getMagnitude (0, hot.getNumSamples()) <= 1.0f;
                silence();
            }
            check (r, ok, "16 voices with resonance, drive and level maxed stay stable and under 0 dBFS in every model");
        }

        // Nova: touch. Aftertouch brightens a held note and adds vibrato; the filter envelope swells from its initial level
        {
            pd.loadPatch (PolyDevice::patchesFor (PolyDevice::nova)[0]);
            setParam (pd.drift, 0.0f); setParam (pd.chorus, 0.0f); setParam (pd.voiceMode, (float) PolyDevice::poly);
            auto bright = [] (const juce::AudioBuffer<float>& b, double from, double to)
            {
                double e = 0, ed = 0;
                const float* x = b.getReadPointer (0);
                for (int i = (int) (from * sr) + 1; i < (int) (to * sr); ++i) { e += x[i] * x[i]; ed += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); }
                return ed / (e + 1e-12);
            };
            auto pressed = [&] (int amount)
            {
                juce::AudioBuffer<float> b (2, (int) (sr * 3.0));
                b.clear();
                process (pd, b, { { 0, on (57, 90) }, { (int) (sr * 1.0), juce::MidiMessage::aftertouchChange (1, 57, amount) } });
                silence();
                return b;
            };
            setParam (pd.nAfterVib, 0.0f);
            const auto light = bright (pressed (0), 2.2, 3.0), hard = bright (pressed (127), 2.2, 3.0);
            check (r, hard > light * 1.4, "Nova: aftertouch brightens a held note (x" + juce::String (hard / light, 2) + ")");

            setParam (pd.nAfterVib, 100.0f); setParam (pd.nAfterBright, 0.0f); setParam (pd.nLpf, 18000.0f); setParam (pd.nEnv, 0.0f); setParam (pd.nBalance, 0.0f);
            auto wobble = [&] (int amount)
            {
                auto b = pressed (amount);
                double lo = 1e9, hi = -1e9;
                for (double t = 1.8; t < 2.9; t += 0.03)
                {
                    const double f = yinPitch (b, (int) (t * sr), 2048);
                    if (f > 0) { lo = juce::jmin (lo, f); hi = juce::jmax (hi, f); }
                }
                return 1200.0 * std::log2 (hi / lo);
            };
            const auto still = wobble (0), vib = wobble (127);
            check (r, vib > 30.0 && still < 8.0, "Nova: pressing adds vibrato (" + juce::String (vib, 0) + " cents peak to peak, " + juce::String (still, 0) + " without)");

            pd.loadPatch (PolyDevice::patchesFor (PolyDevice::nova)[0]);
            setParam (pd.drift, 0.0f); setParam (pd.chorus, 0.0f);
            setParam (pd.nIL, -100.0f); setParam (pd.nAL, 100.0f); setParam (pd.nFA, 0.6f); setParam (pd.nFD, 6.0f);
            juce::AudioBuffer<float> sw (2, (int) (sr * 1.2));
            sw.clear();
            process (pd, sw, { { 0, on (57, 100) } });
            silence();
            const auto early = bright (sw, 0.02, 0.1), later = bright (sw, 0.7, 0.9);
            check (r, later > early * 2.0, "Nova: the filter swells from its initial level to its attack level (x" + juce::String (later / early, 2) + " brighter)");

            // Nova patches to listen to: a slow progression
            for (auto name : { "Nova Brass", "Rain Pad", "Night Rain Choir", "Memory Bell" })
            {
                for (int i = 0; i < (int) PolyDevice::patches().size(); ++i)
                    if (juce::String (PolyDevice::patches()[(size_t) i].name) == name)
                        pd.loadPatch (i);
                std::vector<std::pair<int, juce::MidiMessage>> midi;
                const int chords[2][4] = { { 45, 52, 57, 60 }, { 41, 48, 53, 57 } };
                for (int c2 = 0; c2 < 2; ++c2)
                    for (int k = 0; k < 4; ++k)
                    {
                        const int at = (int) (sr * (c2 * 3.5 + k * 0.03));
                        midi.push_back ({ at, on (chords[c2][k], 85) });
                        for (int a = 1; a <= 20; ++a)
                            midi.push_back ({ at + (int) (sr * (0.8 + a * 0.08)), juce::MidiMessage::aftertouchChange (1, chords[c2][k], juce::jmin (127, a * 7)) });
                        midi.push_back ({ at + (int) (sr * 3.2), off (chords[c2][k]) });
                    }
                juce::AudioBuffer<float> b (2, (int) (sr * 9.0));
                b.clear();
                process (pd, b, midi);
                writeWav (outDir.getChildFile ("nova-" + juce::String (name).toLowerCase().replaceCharacter (' ', '-') + ".wav"), b);
                silence();
            }
        }

        // Chorus widens the sound: left and right differ more with it on
        {
            pd.loadPatch (PolyDevice::patchesFor (PolyDevice::velvet)[0]);
            setParam (pd.voiceMode, (float) PolyDevice::mono);   // one centred voice, so any width is the chorus
            auto width = [&] (int mode)
            {
                setParam (pd.chorus, (float) mode);
                juce::AudioBuffer<float> b (2, (int) sr);
                b.clear();
                process (pd, b, { { 0, on (57) } });
                double corr = 0, e0 = 0, e1 = 0;
                for (int i = (int) (0.3 * sr); i < (int) sr; ++i)
                {
                    auto a = b.getSample (0, i), c = b.getSample (1, i);
                    corr += a * c; e0 += a * a; e1 += c * c;
                }
                silence();
                return corr / std::sqrt (e0 * e1 + 1e-20);
            };
            const auto dry = width (0), wide = width (2);
            check (r, dry > 0.99 && wide < 0.9, "chorus widens the stereo image (L/R correlation " + juce::String (dry, 2) + " off, " + juce::String (wide, 2) + " with chorus II)");
        }

        for (int m = 0; m < PolyDevice::numModels; ++m)
        {
            pd.loadPatch (PolyDevice::patchesFor (m)[0]);
            juce::AudioBuffer<float> warm (2, (int) (sr * 0.5));
            warm.clear();
            process (pd, warm, { { 0, on (57) }, { 0, on (60) }, { 0, on (64) } });
            const juce::StringArray files { "poly-prism.png", "poly-quad.png", "poly-velvet.png", "poly-nova.png" };
            snapshot (pd, outDir.getChildFile (files[m]));
            snapshotWindow (pd, outDir.getChildFile (m == 0 ? juce::String ("poly-window.png") : files[m].replace (".png", "-window.png")));
            silence();
        }
        pd.loadPatch (PolyDevice::patchesFor (PolyDevice::velvet)[0]);
        Theme::apply (Themes::light); refreshLookAndFeel();
        snapshot (pd, outDir.getChildFile ("poly-velvet-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
    }

    //==============================================================================
    r.log << "Poly engine\n";
    {
        auto& pd = make<PolyDevice> (*edit, PolyDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto reset = [&] { pd.initialise ({ te::TimePosition(), sr, block }); };
        auto patchNamed = [] (const char* name)
        {
            for (int i = 0; i < PolyEngine::patchCount(); ++i)
                if (juce::String (PolyEngine::patchName (i)) == name)
                    return i;
            return 0;
        };

        // The engine on its own, in the same pieces the device renders in: blocks, split at each message
        auto viaEngine = [&] (PolyEngine& e, const std::vector<std::pair<int, juce::MidiMessage>>& midi, int length)
        {
            juce::AudioBuffer<float> b (2, length);
            b.clear();
            auto* L = b.getWritePointer (0);
            auto* R = b.getWritePointer (1);
            for (int start = 0; start < length; start += block)
            {
                const int n = juce::jmin (block, length - start);
                int done = 0;
                for (auto& [pos, m] : midi)
                {
                    if (pos < start || pos >= start + n) continue;
                    if (pos - start > done) { e.render (L + start + done, R + start + done, pos - start - done); done = pos - start; }
                    if (m.isNoteOn()) e.noteOn (m.getNoteNumber(), m.getFloatVelocity());
                    else if (m.isNoteOff()) e.noteOff (m.getNoteNumber());
                    else if (m.isControllerOfType (1)) e.setModWheel ((float) m.getControllerValue() / 127.0f);
                    else if (m.isPitchWheel()) e.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
                    else if (m.isAftertouch()) e.setAftertouch (m.getNoteNumber(), (float) m.getAfterTouchValue() / 127.0f);
                }
                if (done < n) e.render (L + start + done, R + start + done, n - done);
            }
            return b;
        };

        // The device is PolyEngine: the same notes, bend, mod wheel and aftertouch through the
        // engine on its own come out the same, sample for sample, in several models and modes
        {
            const int s1 = (int) sr;
            const std::vector<std::pair<int, juce::MidiMessage>> midi = {
                { 0, on (60) }, { 0, on (64, 70) }, { 4800, on (67, 40) }, { 9000, juce::MidiMessage::aftertouchChange (1, 64, 100) },
                { s1 / 2, off (60) }, { s1 / 2 + 9600, juce::MidiMessage::controllerEvent (1, 1, 90) }, { s1 / 2 + 13000, juce::MidiMessage::pitchWheel (1, 12000) },
                { s1, off (64) }, { s1, off (67) }, { s1 + 4000, on (48, 110) }, { s1 + 4100, on (55, 90) },
                { s1 + 20000, juce::MidiMessage::pitchWheel (1, 3000) }, { 2 * s1, off (48) }, { 2 * s1, off (55) } };

            juce::String report;
            bool ok = true;
            for (auto name : { "Prism Pad", "Ring Lead", "Unison Saws", "Quad Pluck", "PWM Strings", "Lush Pad", "Velvet Bass", "Nova Brass", "Chord Stab" })
            {
                const int patch = patchNamed (name);
                pd.loadPatch (patch);
                reset();
                juce::AudioBuffer<float> dev (2, 3 * s1);
                dev.clear();
                process (pd, dev, midi);

                PolyEngine e;
                e.setPatch (patch);
                e.prepare (sr, block);
                auto eng = viaEngine (e, midi, 3 * s1);

                float worst = 0.0f;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = 0; i < dev.getNumSamples(); ++i)
                        worst = juce::jmax (worst, std::abs (dev.getSample (ch, i) - eng.getSample (ch, i)));
                const bool same = worst == 0.0f && dev.getMagnitude (0, dev.getNumSamples()) > 0.02f;
                ok = ok && same;
                if (! same) report << name << " (worst " << juce::String (worst) << ") ";
            }
            check (r, ok, "the device plays PolyEngine: the engine on its own renders the same, sample for sample, in all four models"
                          + (report.isEmpty() ? juce::String() : ": " + report));
            pd.loadPatch (0);
            reset();        // bend and mod wheel back to rest
        }

        // prepare() starts it afresh
        {
            PolyEngine e;
            e.setPatch (patchNamed ("Nova Brass"));
            e.params.drift = 100.0f;
            const int n = (int) sr;
            auto once = [&]
            {
                e.prepare (sr);
                juce::AudioBuffer<float> b (2, n);
                b.clear();
                e.noteOn (57, 0.8f); e.noteOn (61, 0.6f);
                e.render (b.getWritePointer (0), b.getWritePointer (1), n);
                return b;
            };
            const auto b1 = once(), b2 = once();
            bool same = b1.getMagnitude (0, n) > 0.01f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < n; ++i) same = same && b1.getSample (ch, i) == b2.getSample (ch, i);
            check (r, same, "PolyEngine: prepare() starts it afresh, so the same notes render the same audio every time");
        }

        // The engine has the device's patches, and its controls are the device's parameters
        {
            bool names = PolyEngine::patchCount() == (int) PolyDevice::patches().size() && PolyEngine::patchCount() == 25;
            for (int i = 0; i < PolyEngine::patchCount(); ++i)
                names = names && juce::String (PolyEngine::patchName (i)) == PolyDevice::patches()[(size_t) i].name
                              && PolyEngine::patchModel (i) == PolyDevice::patches()[(size_t) i].model;
            check (r, names, "PolyEngine has the device's " + juce::String (PolyEngine::patchCount()) + " patches");

            pd.loadPatch (patchNamed ("Rain Pad"));
            bool loaded = true;
            for (auto& [id, member] : PolyEngine::Params::table())
                if (auto ap = pd.getAutomatableParameterByID (id))
                    loaded = loaded && std::abs (ap->getCurrentBaseValue() - PolyEngine::patchValue (patchNamed ("Rain Pad"), id)) < 1.0e-4f;
            check (r, loaded && pd.getModel() == PolyDevice::nova && ! pd.isPatchModified(), "loadPatch sets every parameter to the engine's patch");

            juce::String mismatched;
            PolyEngine::Params p;
            std::set<juce::String> ids;
            int unresolved = 0;
            for (auto& c : PolyEngine::controls())
            {
                ids.insert (c.id);
                if (p.find (c.id) == nullptr) ++unresolved;
                auto ap = pd.getAutomatableParameterByID (c.id);
                if (ap == nullptr) { mismatched << c.id << " "; continue; }
                const auto range = ap->getValueRange();
                const bool menuOk = c.choices.empty() || (int) c.choices.size() == juce::roundToInt (c.max - c.min) + 1;
                if (std::abs (range.getStart() - c.min) > 1.0e-6f || std::abs (range.getEnd() - c.max) > 1.0e-6f
                    || std::abs (ap->getDefaultValue().value_or (-999.0f) - c.defaultValue) > 1.0e-6f || ! menuOk)
                    mismatched << c.id << " ";
            }
            check (r, unresolved == 0 && ids.size() == PolyEngine::controls().size() && ids.size() == PolyEngine::Params::table().size()
                      && (int) ids.size() == pd.getAutomatableParameters().size() && mismatched.isEmpty(),
                   "PolyEngine::controls(): all " + juce::String ((int) ids.size()) + " resolve through Params::find, once each, with the device's ranges and defaults"
                   + (mismatched.isEmpty() ? juce::String() : ": " + mismatched));

            const juce::StringArray models { "Prism", "Quad", "Velvet", "Nova" };
            bool forOk = true;
            juce::String counts;
            size_t total = 0;
            for (int m = 0; m < PolyEngine::numModels; ++m)
            {
                auto list = PolyEngine::controlsFor (m);
                int shared = 0;
                for (auto* c : list)
                {
                    const juce::String section (juce::CharPointer_UTF8 (c->section));
                    bool other = false;
                    for (int o = 0; o < PolyEngine::numModels; ++o)
                        if (o != m && section.startsWith (models[o] + " ")) other = true;
                    forOk = forOk && ! other;
                    if (! section.startsWith (models[m] + " ")) ++shared;
                }
                forOk = forOk && shared == 11 && juce::String (list.front()->id) == "model";
                total += list.size() - (size_t) shared;
                counts << models[m] << " " << (int) list.size() << "  ";
            }
            check (r, forOk && total + 11 == PolyEngine::controls().size(),
                   "PolyEngine::controlsFor: the 11 shared controls and that model's own (" + counts.trim() + ")");
        }

        // A control written straight into params between renders is heard in the next render
        {
            auto run = [&] (bool change)
            {
                PolyEngine e;
                e.setPatch (patchNamed ("Velvet Strings"));
                e.prepare (sr);
                juce::AudioBuffer<float> b (2, 2 * block);
                b.clear();
                e.noteOn (57, 0.8f);
                for (int i = 0; i < 40; ++i)
                {
                    b.clear();
                    e.render (b.getWritePointer (0), b.getWritePointer (1), block);
                }
                if (change)
                {
                    *e.params.find ("vcutoff") = 300.0f;
                    *e.params.find ("vhpf") = 3.0f;
                }
                b.clear();
                e.render (b.getWritePointer (0), b.getWritePointer (1), block);
                return b;
            };
            const auto before = run (false), after = run (true);
            double diff = 0, energy = 0;
            for (int i = 0; i < block; ++i)
            {
                diff += std::pow (before.getSample (0, i) - after.getSample (0, i), 2.0);
                energy += std::pow (before.getSample (0, i), 2.0);
            }
            check (r, energy > 0.0 && diff > energy * 0.05,
                   "PolyEngine: writing a control into params between renders changes the very next render (difference " + juce::String (10.0 * std::log10 ((diff + 1e-20) / (energy + 1e-20)), 1) + " dB)");
        }

        // Latency: the device reports the engine's
        {
            PolyEngine e;
            e.prepare (sr);
            reset();
            const double latency = e.latencySamples();
            check (r, latency > 0.0 && latency < 8.0 && std::abs (pd.getLatencySeconds() - latency / sr) < 1.0e-12,
                   "the device's latency is the engine's downsampling delay (" + juce::String (latency, 3) + " samples)");
        }
    }

    //==============================================================================
    r.log << "Modular\n";
    {
        auto& md = make<ModularDevice> (*edit, ModularDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto frequency = [] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            double first = -1, last = -1; int n = 0;
            for (int i = from + 1; i < to; ++i)
            {
                auto a = b.getSample (0, i - 1), c = b.getSample (0, i);
                if (a < 0.0f && c >= 0.0f)
                {
                    auto t = (double) (i - 1) + a / (a - c);
                    if (first < 0) first = t; else ++n;
                    last = t;
                }
            }
            return n > 0 ? n * sr / (last - first) : 0.0;
        };
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, int from, int len) { return juce::Decibels::gainToDecibels (b.getRMSLevel (0, from, len), -200.0f); };
        auto silence = [&] { juce::AudioBuffer<float> rest (2, (int) (sr * 3.0)); process (md, rest, { { 0, juce::MidiMessage::allNotesOff (1) } }); };
        auto moduleOfType = [&] (const char* type)
        {
            for (auto& m : md.modulesInOrder())
                if (m["type"].toString() == type) return m;
            return juce::ValueTree();
        };

        // The basic voice, filter opened up: in tune
        md.loadPatch (0);
        auto filter = moduleOfType ("filter");
        filter.setProperty ("p_cutoff", 20000.0f, nullptr);
        filter.setProperty ("p_res", 0.0f, nullptr);
        filter.setProperty ("p_cvamt", 0.0f, nullptr);
        moduleOfType ("vco").setProperty ("p_fine", 0.0f, nullptr);
        moduleOfType ("adsr").setProperty ("p_s", 100.0f, nullptr);
        {
            juce::AudioBuffer<float> b (2, (int) sr);
            b.clear();
            process (md, b, { { 0, on (57) } });
            auto f = frequency (b, (int) (0.3 * sr), (int) sr);
            check (r, std::abs (f - 220.0) < 1.0, "Basic Voice plays A3 at " + juce::String (f, 2) + " Hz");
        }

        // Re-patching while it plays: pull the output cable, then put it back
        {
            const int vcaId = (int) moduleOfType ("vca")["id"], outId = (int) moduleOfType ("output")["id"];
            md.disconnectInput (outId, "l", nullptr);
            juce::AudioBuffer<float> b (2, (int) (sr * 0.3));
            b.clear();
            process (md, b);
            const auto pulled = rmsDb (b, (int) (0.1 * sr), (int) (0.2 * sr));
            md.connect (vcaId, "out", outId, "l", nullptr);
            b.clear();
            process (md, b);
            const auto back = rmsDb (b, (int) (0.1 * sr), (int) (0.2 * sr));
            check (r, pulled < -100.0f && back > -30.0f, "patching live: output cable pulled = silence, plugged back = sound (" + juce::String (back, 1) + " dB)");

            // Add and remove modules while it plays
            auto lfo = md.addModule ("lfo", nullptr);
            md.connect ((int) lfo["id"], "sine", (int) moduleOfType ("vco")["id"], "fm", nullptr);
            b.clear();
            process (md, b);
            md.removeModule ((int) lfo["id"], nullptr);
            b.clear();
            process (md, b);
            check (r, isFinite (b) && md.modulesInOrder().back()["type"].toString() == "output",
                   "modules can be added and removed while playing (Output stays last)");
        }

        // A feedback loop (filter output back into the VCO's FM) is stable
        {
            md.connect ((int) moduleOfType ("filter")["id"], "lp24", (int) moduleOfType ("vco")["id"], "fm", nullptr);
            moduleOfType ("vco").setProperty ("p_fm", 100.0f, nullptr);
            moduleOfType ("filter").setProperty ("p_res", 100.0f, nullptr);
            juce::AudioBuffer<float> b (2, (int) sr);
            b.clear();
            process (md, b);
            check (r, isFinite (b) && b.getMagnitude (0, b.getNumSamples()) <= 1.0f, "a feedback loop through the patch stays stable");
            silence();
        }

        // Polyphony: four voices, a three-note chord
        {
            md.loadPatch (0);
            setParam (md.voices, 4.0f);
            juce::AudioBuffer<float> one (2, (int) (sr * 0.5)), three (2, (int) (sr * 0.5));
            one.clear(); three.clear();
            process (md, one, { { 0, on (60) } });
            silence();
            process (md, three, { { 0, on (60) }, { 0, on (64) }, { 0, on (67) } });
            silence();
            const auto a = rmsDb (one, (int) (0.2 * sr), (int) (0.3 * sr)), c = rmsDb (three, (int) (0.2 * sr), (int) (0.3 * sr));
            check (r, c > a + 2.0f, "polyphonic patch: a chord is louder than one note (+" + juce::String (c - a, 1) + " dB)");
        }

        // Sequencer: the pitch changes from step to step
        {
            md.loadPatch (4);
            juce::AudioBuffer<float> b (2, (int) (sr * 1.2));
            b.clear();
            process (md, b, { { 0, on (48) } });
            juce::Array<double> found;
            for (double t = 0.05; t < 1.1; t += 1.0 / 7.0)
                found.add (frequency (b, (int) (t * sr), (int) ((t + 0.05) * sr)));
            int distinct = 0;
            for (int i = 1; i < found.size(); ++i)
                if (std::abs (found[i] - found[i - 1]) > 5.0) ++distinct;
            check (r, distinct >= 3, "sequencer steps change the pitch (" + juce::String (distinct) + " changes in 1 s)");
            silence();
        }

        // Every starter patch plays cleanly
        {
            juce::String bad;
            bool ok = true;
            for (int i = 0; i < (int) ModularDevice::patches().size(); ++i)
            {
                md.loadPatch (i);
                juce::AudioBuffer<float> p (2, (int) (sr * 2.0));
                p.clear();
                process (md, p, { { 0, on (48) }, { 0, on (55) }, { (int) (1.6 * sr), off (48) }, { (int) (1.6 * sr), off (55) } });
                const auto lvl = rmsDb (p, 0, (int) (1.6 * sr));
                if (! isFinite (p) || p.getMagnitude (0, p.getNumSamples()) > 1.0f || lvl < -45.0f)
                {
                    ok = false;
                    bad << ModularDevice::patches()[(size_t) i].name << " (" << juce::String (lvl, 1) << " dB) ";
                }
                silence();
            }
            check (r, ok, "all " + juce::String ((int) ModularDevice::patches().size()) + " starter patches play cleanly" + (bad.isEmpty() ? juce::String() : ": " + bad));
        }

        md.loadPatch (1);
        juce::AudioBuffer<float> warm (2, (int) (sr * 0.4));
        warm.clear();
        process (md, warm, { { 0, on (45) } });
        snapshot (md, outDir.getChildFile ("modular.png"));
        snapshotWindow (md, outDir.getChildFile ("modular-window.png"));
        Theme::apply (Themes::light); refreshLookAndFeel();
        snapshot (md, outDir.getChildFile ("modular-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
    }

    //==============================================================================
    r.log << "Modular modules\n";
    {
        Modular::Context ctx;
        ctx.sr = (float) sr * 2.0f;
        const float pitch0 = 0.0f, gate0 = 0.0f, vel0 = 1.0f, mac[4] {};
        ctx.pitch = &pitch0; ctx.gate = &gate0; ctx.vel = &vel0; ctx.macros = mac;
        const bool patchedAll[Modular::maxPorts] = { true, true, true, true, true, true, true, true };
        auto setP = [] (Modular::Dsp& d, std::initializer_list<float> v) { int i = 0; for (auto x : v) d.params[(size_t) i++] = x; };

        // Wavefolder: a sine comes out richer the more it folds
        {
            auto bright = [&] (float fold)
            {
                Modular::FoldDsp d; setP (d, { fold, 0.0f });
                double e = 0, ed = 0; float last = 0;
                for (int i = 0; i < 9600; ++i)
                {
                    const float in[] = { 0.8f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / ctx.sr), 0.0f };
                    d.process (0, in, patchedAll, ctx);
                    const float y = d.out[0][0];
                    e += y * y; ed += (y - last) * (y - last); last = y;
                }
                return ed / (e + 1e-12);
            };
            check (r, bright (80.0f) > bright (0.0f) * 4.0, "wavefolder adds harmonics as it folds (x" + juce::String (bright (80.0f) / bright (0.0f), 1) + " brighter)");
        }

        // Low-pass gate: a strike opens it at once, then it rings closed
        {
            Modular::LpgDsp d; setP (d, { 60.0f, 0.4f });
            const bool patched[] = { true, false, true, false, false, false, false, false };
            float early = 0, late = 0;
            for (int i = 0; i < (int) (ctx.sr * 1.2f); ++i)
            {
                const float in[] = { std::sin (juce::MathConstants<float>::twoPi * 330.0f * (float) i / ctx.sr), 0.0f, i < 200 ? 1.0f : 0.0f };
                d.process (0, in, patched, ctx);
                const float a = std::abs (d.out[0][0]);
                if (i < (int) (ctx.sr * 0.05f)) early = juce::jmax (early, a);
                if (i > (int) (ctx.sr * 1.0f)) late = juce::jmax (late, a);
            }
            check (r, early > 0.5f && late < early * 0.1f, "low-pass gate: struck, it opens (" + juce::String (early, 2) + ") then rings closed (" + juce::String (late, 3) + ")");
        }

        // Quantizer: C major, 1.2 semitones rounds to D; A minor pentatonic keeps A
        {
            Modular::QuantDsp d; setP (d, { 1.0f, 0.0f });
            const float in1[] = { 1.2f / 12.0f };
            d.process (0, in1, patchedAll, ctx);
            const float toD = d.out[0][0] * 12.0f;
            setP (d, { 4.0f, 9.0f });
            const float in2[] = { 9.3f / 12.0f };
            d.process (0, in2, patchedAll, ctx);
            const float toA = d.out[0][0] * 12.0f;
            check (r, std::abs (toD - 2.0f) < 1.0e-3f && std::abs (toA - 9.0f) < 1.0e-3f && d.out[0][1] > 0.5f,
                   "quantizer rounds to the scale (" + juce::String (toD, 2) + ", " + juce::String (toA, 2) + " st) and fires a trigger on a new note");
        }

        // Clock divider: 24 clocks give 12, 8, 6 and 3 pulses
        {
            Modular::ClockDivDsp d;
            std::array<int, 4> pulses {}; std::array<float, 4> last {};
            for (int clk = 0; clk < 24; ++clk)
                for (int i = 0; i < 20; ++i)
                {
                    const float in[] = { i < 10 ? 1.0f : 0.0f, 0.0f };
                    d.process (0, in, patchedAll, ctx);
                    for (int k = 0; k < 4; ++k) { if (d.out[0][(size_t) k] > 0.5f && last[(size_t) k] <= 0.5f) ++pulses[(size_t) k]; last[(size_t) k] = d.out[0][(size_t) k]; }
                }
            check (r, pulses[0] == 12 && pulses[1] == 8 && pulses[2] == 6 && pulses[3] == 3,
                   "clock divider: " + juce::String (pulses[0]) + ", " + juce::String (pulses[1]) + ", " + juce::String (pulses[2]) + ", " + juce::String (pulses[3]) + " pulses from 24 clocks");
        }

        // Random: Rate new values a second; stepped holds, smooth glides
        {
            Modular::RandomDsp d; setP (d, { 4.0f, 100.0f });
            int trigs = 0; float lastT = 0; double jumps = 0; float prevSmooth = 0;
            for (int i = 0; i < (int) ctx.sr * 2; ++i)
            {
                const float in[] = { 0.0f };
                d.process (0, in, patchedAll, ctx);
                if (d.out[0][2] > 0.5f && lastT <= 0.5f) ++trigs;
                lastT = d.out[0][2];
                if (i > 0) jumps = juce::jmax (jumps, (double) std::abs (d.out[0][0] - prevSmooth));
                prevSmooth = d.out[0][0];
            }
            check (r, trigs >= 7 && trigs <= 9 && jumps < 0.01, "random: " + juce::String (trigs) + " new values in 2 s at 4 Hz, smooth output glides (biggest step " + juce::String (jumps, 4) + ")");
        }

        // A whole patch renders through the oversampler cleanly
        {
            auto& md = make<ModularDevice> (*edit, ModularDevice::xmlTypeName);
            juce::String bad;
            for (int i = 0; i < (int) ModularDevice::patches().size(); ++i)
            {
                md.loadPatch (i);
                md.initialise ({ te::TimePosition(), sr, block });
                juce::AudioBuffer<float> b (2, (int) (sr * 3.0));
                b.clear();
                std::vector<std::pair<int, juce::MidiMessage>> midi { { 0, juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100) },
                                                                      { (int) (sr * 2.0), juce::MidiMessage::noteOff (1, 57) } };
                process (md, b, midi);
                const float lvl = juce::Decibels::gainToDecibels (b.getRMSLevel (0, 0, (int) (sr * 2.0)), -200.0f);
                if (! isFinite (b) || b.getMagnitude (0, b.getNumSamples()) > 1.0f || lvl < -50.0f)
                    bad << ModularDevice::patches()[(size_t) i].name << " (" << juce::String (lvl, 1) << " dB) ";
                if (juce::String (ModularDevice::patches()[(size_t) i].name).containsIgnoreCase ("Garden") || juce::String (ModularDevice::patches()[(size_t) i].name).containsIgnoreCase ("Pluck")
                    || juce::String (ModularDevice::patches()[(size_t) i].name) == "Ring Bells")
                    writeWav (outDir.getChildFile ("modular-" + juce::String (ModularDevice::patches()[(size_t) i].name).toLowerCase().replaceCharacter (' ', '-') + ".wav"), b);
            }
            check (r, bad.isEmpty(), "every Modular patch plays through the oversampler" + (bad.isEmpty() ? juce::String() : ": " + bad));

            // The hover scope: point at a VCO's saw output while the patch plays
            md.loadPatch (0);
            md.initialise ({ te::TimePosition(), sr, block });
            auto view = md.createDeviceView();
            juce::Component holder;
            holder.setSize (md.getDeviceViewWidth() + 16, 190);
            holder.addAndMakeVisible (*view);
            view->setBounds (holder.getLocalBounds().reduced (8));
            ModularUI::Rack* rack = nullptr;
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* rk = dynamic_cast<ModularUI::Rack*> (&c)) rack = rk;
                for (auto* ch : c.getChildren()) find (*ch);
            };
            find (holder);
            int vcoId = -1;
            for (auto m : md.getPatch()) if (m["type"].toString() == "vco") { vcoId = (int) m["id"]; break; }
            if (rack != nullptr && vcoId > 0)
            {
                rack->hoverJack (ModularUI::JackRef { vcoId, false, 2 });
                juce::AudioBuffer<float> b (2, (int) (sr * 0.3));
                b.clear();
                process (md, b, { { 0, juce::MidiMessage::noteOn (1, 45, (juce::uint8) 100) } });
                juce::Image img (juce::Image::ARGB, holder.getWidth(), holder.getHeight(), true);
                { juce::Graphics g (img); g.fillAll (Colours::panel); holder.paintEntireComponent (g, true); }
                auto file = outDir.getChildFile ("modular-hover.png");
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto os = file.createOutputStream()) png.writeImageToStream (img, *os);
                rack->hoverJack ({});
            }
            check (r, rack != nullptr && vcoId > 0, "hover scope rendered (modular-hover.png)");
        }
    }

    //==============================================================================
    r.log << "Modular engine\n";
    {
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        const int s1 = (int) sr;
        const std::vector<std::pair<int, juce::MidiMessage>> phrase = {
            { 0, on (48) }, { 0, on (55, 70) }, { 4800, on (60, 40) }, { s1 / 2, off (48) },
            { s1 / 2 + 9600, juce::MidiMessage::controllerEvent (1, 1, 90) }, { s1, off (55) }, { s1, off (60) },
            { s1 + 4800, juce::MidiMessage::pitchWheel (1, 12000) }, { s1 + 9600, on (43, 110) }, { 2 * s1, off (43) } };

        // The engine on its own, fed the same notes in the same pieces the device renders in:
        // blocks, split at each message
        auto playEngine = [&] (ModularEngine& e, const std::vector<std::pair<int, juce::MidiMessage>>& midi, int length)
        {
            juce::AudioBuffer<float> b (2, length);
            b.clear();
            auto* L = b.getWritePointer (0);
            auto* R = b.getWritePointer (1);
            for (int start = 0; start < length; start += block)
            {
                const int n = juce::jmin (block, length - start);
                int done = 0;
                for (auto& [pos, m] : midi)
                {
                    if (pos < start || pos >= start + n) continue;
                    if (pos - start > done) { e.render (L + start + done, R + start + done, pos - start - done); done = pos - start; }
                    if (m.isNoteOn()) e.noteOn (m.getNoteNumber(), m.getFloatVelocity());
                    else if (m.isNoteOff()) e.noteOff (m.getNoteNumber());
                    else if (m.isController()) e.setModWheel ((float) m.getControllerValue() / 127.0f);
                    else if (m.isPitchWheel()) e.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
                }
                if (done < n) e.render (L + start + done, R + start + done, n - done);
            }
            return b;
        };
        auto worstDifference = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    worst = juce::jmax (worst, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
            return worst;
        };

        // The device is ModularEngine: a factory patch on the device, macros moved, and the same
        // patch in the engine render the same, sample for sample
        auto& md = make<ModularDevice> (*edit, ModularDevice::xmlTypeName);
        for (auto [index, voiceCount] : { std::pair<int, float> { 1, 1.0f }, { 3, 1.0f }, { 6, 3.0f } })
        {
            md.loadPatch (index);
            setParam (md.voices, voiceCount);
            setParam (md.volume, -3.0f);
            setParam (md.macros[0], 0.8f); setParam (md.macros[1], 0.25f); setParam (md.macros[2], 0.65f); setParam (md.macros[3], 0.1f);
            md.initialise ({ te::TimePosition(), sr, block });
            juce::AudioBuffer<float> viaDevice (2, 3 * s1);
            viaDevice.clear();
            process (md, viaDevice, phrase);

            ModularEngine e;
            e.setPatch (index);
            e.params.voices = md.voices.get();
            e.params.volume = md.volume.get();
            e.params.macro1 = md.macros[0].get(); e.params.macro2 = md.macros[1].get();
            e.params.macro3 = md.macros[2].get(); e.params.macro4 = md.macros[3].get();
            e.prepare (sr, block);
            auto viaEngine = playEngine (e, phrase, 3 * s1);

            const float worst = worstDifference (viaDevice, viaEngine);
            check (r, worst == 0.0f && viaDevice.getMagnitude (0, viaDevice.getNumSamples()) > 0.05f,
                   juce::String ("the device plays ModularEngine: ") + ModularEngine::patchName (index) + " (macros moved) renders the same in the engine on its own, sample for sample (worst "
                       + juce::String (worst) + ")");
        }
        setParam (md.volume, -6.0f);
        for (auto& m : md.macros) setParam (m, 0.5f);

        // prepare() starts it afresh
        {
            ModularEngine e;
            e.setPatch (8);     // Generative Garden: random voltages, delay
            e.prepare (sr);
            auto a = playEngine (e, phrase, 2 * s1);
            e.prepare (sr);
            auto b = playEngine (e, phrase, 2 * s1);
            check (r, worstDifference (a, b) == 0.0f && a.getMagnitude (0, a.getNumSamples()) > 0.01f,
                   "prepare() starts it afresh: the same notes render the same audio every time");
        }

        // The patches: the device's, by name, with the device's voice counts and macro names
        {
            bool same = ModularEngine::patchCount() == (int) ModularDevice::patches().size();
            for (int i = 0; same && i < ModularEngine::patchCount(); ++i)
            {
                md.loadPatch (i);
                same = juce::String (ModularEngine::patchName (i)) == ModularDevice::patches()[(size_t) i].name
                       && ModularEngine::patchValue (i, "voices") == (float) ModularDevice::patches()[(size_t) i].voices
                       && md.macroName (0) == ModularEngine::macroName (i, 0) && md.macroName (3) == ModularEngine::macroName (i, 3);
            }
            check (r, same && juce::String (ModularEngine::macroName (0, 0)) == "Cutoff",
                   "the engine has the device's patches (" + juce::String (ModularEngine::patchCount()) + "), voice counts and macro names");
        }

        // With the macros at their middle (0.5) a factory patch sounds exactly as it does without them patched in
        {
            juce::String moved;
            bool neutral = true;
            for (int i = 0; i < ModularEngine::patchCount(); ++i)
            {
                auto bare = ModularEngine::patches()[(size_t) i];
                bare.macros.clear();
                ModularEngine with, without;
                with.setPatch (i);
                without.setPatch (i);
                without.build (Modular::describe (bare));
                with.prepare (sr); without.prepare (sr);
                neutral = neutral && worstDifference (playEngine (with, phrase, s1), playEngine (without, phrase, s1)) == 0.0f;
            }
            check (r, neutral, "every factory patch with its macros at their middle (0.5) renders exactly as it does without them");
        }

        // Each control is an engine parameter and a device parameter, with the device's range
        {
            bool ok = true;
            juce::String wrong;
            ModularEngine::Params defaults;
            for (auto& c : ModularEngine::controls())
            {
                auto* v = defaults.find (c.id);
                auto ap = md.getAutomatableParameterByID (c.id);
                if (v == nullptr || ap == nullptr || *v != c.defaultValue
                    || std::abs (ap->getValueRange().getStart() - c.min) > 1.0e-4f || std::abs (ap->getValueRange().getEnd() - c.max) > 1.0e-4f
                    || std::abs (ap->getDefaultValue().value_or (-1000.0f) - c.defaultValue) > 1.0e-4f)
                { ok = false; wrong << c.id << " "; }
            }
            check (r, ok && ModularEngine::controls().size() == ModularEngine::Params::table().size(),
                   "every Modular control is an engine and a device parameter, with its range and default" + (wrong.isEmpty() ? juce::String() : " (not: " + wrong + ")"));
        }

        // A macro written straight into params between renders is heard in the next render
        {
            ModularEngine a, b;
            a.setPatch (0); b.setPatch (0);
            a.prepare (sr); b.prepare (sr);
            juce::AudioBuffer<float> outA (2, s1), outB (2, s1);
            outA.clear(); outB.clear();
            a.noteOn (57, 0.9f); b.noteOn (57, 0.9f);
            a.render (outA.getWritePointer (0), outA.getWritePointer (1), s1 / 2);
            b.render (outB.getWritePointer (0), outB.getWritePointer (1), s1 / 2);
            *a.params.find ("macro1") = 0.95f;      // Cutoff, wide open
            a.render (outA.getWritePointer (0, s1 / 2), outA.getWritePointer (1, s1 / 2), block);
            b.render (outB.getWritePointer (0, s1 / 2), outB.getWritePointer (1, s1 / 2), block);
            float before = 0.0f, after = 0.0f;
            for (int i = 0; i < s1 / 2 + block; ++i)
                (i < s1 / 2 ? before : after) = juce::jmax (i < s1 / 2 ? before : after, std::abs (outA.getSample (0, i) - outB.getSample (0, i)));
            check (r, before == 0.0f && after > 1.0e-3f,
                   "a macro set straight into params between renders changes the very next render (by up to " + juce::String (after, 3) + ")");
        }
        md.loadPatch (0);
    }

    //==============================================================================
    r.log << "Organic\n";
    {
        auto& od = make<OrganicDevice> (*edit, OrganicDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto reset = [&] { od.initialise ({ te::TimePosition(), sr, block }); };
        auto play = [&] (std::vector<std::pair<int, juce::MidiMessage>> midi, double seconds)
        {
            juce::AudioBuffer<float> b (2, (int) (sr * seconds));
            b.clear();
            process (od, b, midi);
            return b;
        };
        auto sources = [&] (const char* a, const char* b) { od.setSourceKey (0, a); od.setSourceKey (1, b); od.loadSourcesNow(); reset(); };

        /** Pitch by YIN (cumulative mean normalised difference) on a stretch of the left channel. */
        auto pitchOf = [] (const juce::AudioBuffer<float>& b, double fromSec, double fmin = 40.0, double fmax = 2000.0)
        {
            const int start = (int) (fromSec * sr), w = 4096;
            const int maxLag = (int) (sr / fmin), minLag = (int) (sr / fmax);
            if (start + w + maxLag >= b.getNumSamples()) return 0.0;
            const float* x = b.getReadPointer (0) + start;
            std::vector<double> d ((size_t) maxLag + 1, 0.0), cm ((size_t) maxLag + 1, 1.0);
            double run = 0.0;
            for (int tau = 1; tau <= maxLag; ++tau)
            {
                double s = 0.0;
                for (int i = 0; i < w; ++i) { const double a = x[i] - x[i + tau]; s += a * a; }
                d[(size_t) tau] = s; run += s;
                cm[(size_t) tau] = run > 0 ? s * tau / run : 1.0;
            }
            for (int tau = minLag; tau < maxLag; ++tau)
                if (cm[(size_t) tau] < 0.15)
                {
                    while (tau + 1 < maxLag && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau;
                    const double a = cm[(size_t) tau - 1], c = cm[(size_t) tau + 1], m = cm[(size_t) tau];
                    const double shift = (a - c) / (2.0 * (a - 2.0 * m + c) + 1e-12);
                    return sr / (tau + shift);
                }
            return 0.0;
        };
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, double from, double to)
        {
            const int a = (int) (from * sr), n = juce::jmin (b.getNumSamples(), (int) (to * sr)) - a;
            return juce::Decibels::gainToDecibels ((b.getRMSLevel (0, a, n) + b.getRMSLevel (1, a, n)) * 0.5f, -200.0f);
        };
        /** Rough brightness: share of energy above ~2 kHz (first difference vs signal). */
        auto brightness = [] (const juce::AudioBuffer<float>& b, double from, double to)
        {
            double e = 0, ed = 0;
            const float* x = b.getReadPointer (0);
            for (int i = (int) (from * sr) + 1; i < (int) (to * sr); ++i) { e += x[i] * x[i]; ed += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); }
            return ed / (e + 1e-12);
        };
        auto plain = [&]
        {
            // One layer, played as recorded: no texture, body, air, space, drift or saturation
            for (auto& p : { std::pair<ElsewhereDevice::Param, float> { od.aTexture, 0.0f }, { od.aPosition, 0.0f }, { od.aTune, 0.0f }, { od.aLevel, 100.0f },
                             { od.morph, 0.0f }, { od.tideDepth, 0.0f }, { od.resonance, 0.0f }, { od.air, 0.0f }, { od.life, 0.0f },
                             { od.spaceMix, 0.0f }, { od.warmth, 0.0f }, { od.bright, 20000.0f }, { od.dynamics, 0.0f }, { od.bloom, 0.0f },
                             { od.attack, 0.001f }, { od.release, 0.2f }, { od.swell, 100.0f }, { od.volume, 0.0f } })
                setParam (p.first, p.second);
        };

        const bool packInstalled = SoundLibrary::isInstalled (OrganicDevice::packId());
        r.log << "  (Organic Sounds pack " << (packInstalled ? "installed at " + SoundLibrary::root().getFullPathName() : juce::String ("not installed")) << ")\n";

        // Built-in sources play in tune, at the right octave
        {
            sources ("builtin/bow", "builtin/glass");
            plain();
            juce::String got;
            bool ok = true;
            for (int note : { 48, 60, 67, 79 })
            {
                auto b = play ({ { 0, on (note) } }, 1.2);
                const double f = pitchOf (b, 0.6), want = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                ok = ok && std::abs (12.0 * std::log2 (f / want)) < 0.2;
                got << note << ":" << juce::String (f, 1) << " Hz  ";
                reset();
            }
            check (r, ok, "built-in Air Bow plays in tune across the range (" + got.trim() + ")");
        }

        // Endless sustain: a 6 s recording held for 14 s keeps sounding, without dropouts
        {
            sources ("builtin/bow", "builtin/glass");
            plain();
            setParam (od.aTexture, 15.0f);
            auto b = play ({ { 0, on (57) } }, 14.0);
            float lo = 0.0f, hi = -200.0f;
            lo = 200.0f;
            for (double t = 1.0; t < 13.9; t += 0.05) { const auto db = rmsDb (b, t, t + 0.05); lo = juce::jmin (lo, db); hi = juce::jmax (hi, db); }
            const auto early = rmsDb (b, 1.0, 3.0), late = rmsDb (b, 11.0, 13.0);
            check (r, late > early - 6.0f && hi - lo < 12.0f && isFinite (b),
                   "a held note sustains past the end of its recording (" + juce::String (early, 1) + " dB early, " + juce::String (late, 1)
                   + " dB at 11-13 s, 50 ms windows vary " + juce::String (hi - lo, 1) + " dB)");
            reset();
        }

        // Struck sources ring out and the voice frees itself
        {
            sources ("builtin/glass", "builtin/glass");
            plain();
            setParam (od.release, 0.05f);
            auto b = play ({ { 0, on (72) }, { (int) (sr * 0.1), off (72) } }, 1.0);
            juce::AudioBuffer<float> after (2, (int) (sr * 8.0));
            after.clear();
            process (od, after);
            int activeVoices = 0;
            for (auto& v : od.voiceView) activeVoices += v.active ? 1 : 0;
            check (r, rmsDb (b, 0.0, 0.1) > -40.0f && activeVoices == 0, "a struck note sounds, then its voice is free again (" + juce::String (activeVoices) + " active)");
            reset();
        }

        // Texture turns the recording into a wider, more diffuse cloud
        {
            sources ("builtin/glass", "builtin/glass");
            plain();
            auto widthOf = [&] (float texture)
            {
                setParam (od.aTexture, texture);
                auto b = play ({ { 0, on (72) } }, 1.0);
                double corr = 0, e0 = 0, e1 = 0;
                for (int i = (int) (0.05 * sr); i < (int) (0.8 * sr); ++i)
                {
                    auto a = b.getSample (0, i), c = b.getSample (1, i);
                    corr += a * c; e0 += a * a; e1 += c * c;
                }
                reset();
                return corr / std::sqrt (e0 * e1 + 1e-20);
            };
            const auto natural = widthOf (0.0f), cloud = widthOf (90.0f);
            check (r, natural > 0.9 && cloud < natural - 0.2, "Texture spreads the sound into a cloud (L/R correlation " + juce::String (natural, 2) + " natural, " + juce::String (cloud, 2) + " at 90 %)");
        }

        // At high Texture a struck note freezes into a sustained shimmer
        {
            sources ("builtin/glass", "builtin/glass");
            plain();
            setParam (od.aTexture, 80.0f);
            auto b = play ({ { 0, on (72) } }, 6.0);
            check (r, rmsDb (b, 4.5, 6.0) > rmsDb (b, 0.1, 1.0) - 12.0f, "Texture freezes a struck note while the key is held ("
                   + juce::String (rmsDb (b, 0.1, 1.0), 1) + " dB at first, " + juce::String (rmsDb (b, 4.5, 6.0), 1) + " dB after 4.5 s)");
            reset();
        }

        // Expression: more mod wheel is louder and brighter (and Dynamics sets how much)
        {
            sources ("builtin/bow", "builtin/glass");
            plain();
            setParam (od.dynamics, 100.0f);
            setParam (od.bright, 6000.0f);
            auto with = [&] (int cc)
            {
                auto b = play ({ { 0, juce::MidiMessage::controllerEvent (1, 1, cc) }, { 1, on (60, 100) } }, 2.0);
                reset();
                return b;
            };
            auto soft = with (15), loud = with (127);
            const auto dbSoft = rmsDb (soft, 1.0, 2.0), dbLoud = rmsDb (loud, 1.0, 2.0);
            const auto brSoft = brightness (soft, 1.0, 2.0), brLoud = brightness (loud, 1.0, 2.0);
            check (r, dbLoud - dbSoft > 12.0f && brLoud > brSoft * 1.3,
                   "mod wheel swells level (+" + juce::String (dbLoud - dbSoft, 1) + " dB) and brightness (x" + juce::String (brLoud / brSoft, 2) + ")");
            setParam (od.dynamics, 0.0f);
            soft = with (15); loud = with (127);
            check (r, std::abs (rmsDb (loud, 1.0, 2.0) - rmsDb (soft, 1.0, 2.0)) < 3.0f, "with Dynamics at 0 the level hardly moves");
        }

        // Bloom: the note grows after it starts
        {
            sources ("builtin/bow", "builtin/glass");
            plain();
            setParam (od.dynamics, 80.0f);
            setParam (od.bloom, 2.0f);
            auto b = play ({ { 0, on (60) } }, 4.0);
            check (r, rmsDb (b, 3.0, 4.0) - rmsDb (b, 0.2, 0.5) > 4.0f, "Bloom swells a held note (+" + juce::String (rmsDb (b, 3.0, 4.0) - rmsDb (b, 0.2, 0.5), 1) + " dB over 3 s)");
            reset();
        }

        // Morph moves between the layers; the body resonates at the note
        {
            sources ("builtin/bow", "builtin/glass");
            plain();
            setParam (od.bLevel, 100.0f); setParam (od.bTexture, 0.0f); setParam (od.bPosition, 0.0f);
            auto at = [&] (float m) { setParam (od.morph, m); auto b = play ({ { 0, on (72) } }, 1.5); reset(); return b; };
            auto a = at (0.0f), bb = at (100.0f);
            double corr = 0, e0 = 0, e1 = 0;
            for (int i = (int) (0.2 * sr); i < (int) (1.5 * sr); ++i) { corr += a.getSample (0, i) * bb.getSample (0, i); e0 += a.getSample (0, i) * a.getSample (0, i); e1 += bb.getSample (0, i) * bb.getSample (0, i); }
            check (r, std::abs (corr / std::sqrt (e0 * e1 + 1e-20)) < 0.5, "Morph 0 and 100 play different layers (correlation " + juce::String (corr / std::sqrt (e0 * e1 + 1e-20), 2) + ")");

            setParam (od.morph, 0.0f); setParam (od.resonance, 100.0f); setParam (od.air, 100.0f); setParam (od.aLevel, 0.0f); setParam (od.bLevel, 0.0f);
            setParam (od.bodyType, (float) OrganicDsp::Body::string);
            auto body = play ({ { 0, on (57) } }, 1.5);
            const auto f = pitchOf (body, 0.8);
            check (r, std::abs (f - 220.0) < 4.0, "Air through a String body rings at the note (" + juce::String (f, 1) + " Hz for A3)");
            reset();
        }

        // Twelve voices, everything up: stable and never past 0 dBFS
        {
            sources ("builtin/choir", "builtin/wind");
            setParam (od.aTexture, 100.0f); setParam (od.bTexture, 100.0f); setParam (od.aLevel, 100.0f); setParam (od.bLevel, 100.0f);
            setParam (od.morph, 50.0f); setParam (od.resonance, 100.0f); setParam (od.air, 100.0f); setParam (od.bodyDecay, 10.0f);
            setParam (od.warmth, 100.0f); setParam (od.spaceMix, 100.0f); setParam (od.spaceSize, 100.0f); setParam (od.volume, 6.0f);
            setParam (od.life, 100.0f); setParam (od.dynamics, 0.0f);
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            for (int n = 0; n < 14; ++n) midi.push_back ({ n * 300, on (30 + n * 5, 127) });
            midi.push_back ({ (int) (sr * 1.0), juce::MidiMessage::controllerEvent (1, 1, 127) });
            auto b = play (midi, 3.0);
            check (r, isFinite (b) && b.getMagnitude (0, b.getNumSamples()) <= 1.0f,
                   "14 notes on 12 voices, everything maxed: finite, peak " + juce::String (juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples())), 1) + " dBFS");
            reset();
        }

        // Changing a source while notes play is safe
        {
            sources ("builtin/bow", "builtin/glass");
            plain();
            juce::AudioBuffer<float> b (2, (int) (sr * 1.0));
            b.clear();
            process (od, b, { { 0, on (60) }, { 0, on (64) } });
            od.setSourceKey (0, "builtin/choir");
            od.loadSourcesNow();
            juce::AudioBuffer<float> b2 (2, (int) (sr * 1.0));
            b2.clear();
            process (od, b2, { { (int) (sr * 0.2), on (67) } });
            check (r, isFinite (b2) && rmsDb (b2, 0.5, 1.0) > -50.0f, "swapping a source mid-note fades the old voices and keeps playing");
            reset();
        }

        // The pack: each instrument plays at its real pitch (catches tuning mistakes in the pack)
        if (packInstalled)
        {
            juce::String got;
            bool ok = true;
            for (auto [key, note] : { std::pair<const char*, int> { "organic/cello", 48 }, { "organic/violins", 69 }, { "organic/solovln", 76 }, { "organic/bass", 40 },
                                      { "organic/flute", 72 }, { "organic/clarinet", 62 }, { "organic/bassoon", 45 }, { "organic/horn", 53 },
                                      { "organic/harp", 60 }, { "organic/bpizz", 40 }, { "organic/marimba", 72 },
                                      { "organic/violas", 62 }, { "organic/oboe", 72 }, { "organic/mhorn", 60 }, { "organic/trumpet", 67 },
                                      { "organic/trombone", 50 }, { "organic/tuba", 41 }, { "organic/piano", 60 }, { "organic/organ", 60 } })
            {
                sources (key, "builtin/glass");
                plain();
                auto b = play ({ { 0, on (note, 90) } }, 1.6);
                const bool struck = OrganicDevice::familyOf (key) == "Plucked" || OrganicDevice::familyOf (key) == "Struck";
                const double f = pitchOf (b, struck ? 0.12 : 0.9), want = 440.0 * std::pow (2.0, (note - 69) / 12.0);
                const double cents = f > 0 ? 1200.0 * std::log2 (f / want) : 9999.0;
                const bool good = std::abs (cents) < 60.0 && ! od.isSourceMissing (0);
                ok = ok && good;
                got << juce::String (key).fromFirstOccurrenceOf ("/", false, false) << (good ? " " : " OFF ") << juce::roundToInt (cents) << "ct  ";
                reset();
            }
            check (r, ok, "pack instruments play at their real pitch: " + got.trim());

            // Every factory patch plays cleanly; a short demo of each for listening
            juce::String bad, levels;
            for (int i = 0; i < (int) OrganicDevice::patches().size(); ++i)
            {
                od.loadPatch (i);
                od.loadSourcesNow();
                reset();
                const int len = (int) (sr * 7.0);
                std::vector<std::pair<int, juce::MidiMessage>> midi;
                const int chord[] = { 48, 55, 60, 64, 71 };
                for (int k = 0; k < 5; ++k) midi.push_back ({ k * (int) (sr * 0.12), on (chord[k], 70 + k * 8) });
                for (int c = 0; c < 40; ++c) midi.push_back ({ (int) (sr * (0.5 + c * 0.08)), juce::MidiMessage::controllerEvent (1, 1, juce::jlimit (0, 127, 40 + c * 2)) });
                for (int k = 0; k < 5; ++k) midi.push_back ({ (int) (sr * 4.5), off (chord[k]) });
                auto b = play (midi, len / sr);
                const auto peak = b.getMagnitude (0, len);
                if (! isFinite (b) || peak > 1.0f || rmsDb (b, 1.0, 4.0) < -40.0f)
                    bad << OrganicDevice::patches()[(size_t) i].name << " (" << juce::String (rmsDb (b, 1.0, 4.0), 1) << " dB, peak " << juce::String (peak, 2) << ") ";
                levels << OrganicDevice::patches()[(size_t) i].name << " " << juce::String (rmsDb (b, 1.0, 4.0), 1) << "  ";
                writeWav (outDir.getChildFile ("organic-" + juce::String (OrganicDevice::patches()[(size_t) i].name).toLowerCase().replaceCharacter (' ', '-') + ".wav"), b);
            }
            check (r, bad.isEmpty(), "all " + juce::String ((int) OrganicDevice::patches().size()) + " factory patches play cleanly" + (bad.isEmpty() ? juce::String() : ": " + bad));
            r.log << "        levels (dB RMS, 1-4 s): " << levels << "\n";
        }

        od.loadPatch (0);
        od.loadSourcesNow();
        reset();
        juce::AudioBuffer<float> warm (2, (int) (sr * 1.2));
        warm.clear();
        process (od, warm, { { 0, on (48) }, { 0, on (55) }, { 0, on (64) } });
        snapshot (od, outDir.getChildFile ("organic.png"));
        snapshotWindow (od, outDir.getChildFile ("organic-window.png"));
        Theme::apply (Themes::light); refreshLookAndFeel();
        snapshot (od, outDir.getChildFile ("organic-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
        reset();

        // The device is OrganicEngine: the same notes through the engine on its own, with the
        // built-in sources it builds itself, come out the same, sample for sample (mod wheel,
        // breath, expression, pressure, aftertouch, bend and pedal included)
        {
            const int patch = 24;                   // Wind Glass: built-in sources only
            od.loadPatch (patch);
            od.loadSourcesNow();
            setParam (od.life, 60.0f);
            setParam (od.tideDepth, 70.0f);
            reset();
            const int s1 = (int) sr;
            std::vector<std::pair<int, juce::MidiMessage>> midi = {
                { 0, on (60) }, { 0, on (64, 70) }, { 4800, on (67, 40) }, { 9000, juce::MidiMessage::controllerEvent (1, 64, 127) },
                { s1, off (60) }, { s1 + 2000, off (64) }, { s1 + 9600, juce::MidiMessage::controllerEvent (1, 1, 90) },
                { s1 + 14000, juce::MidiMessage::controllerEvent (1, 64, 0) }, { s1 + 16000, on (72, 100) },
                { s1 + 20000, juce::MidiMessage::controllerEvent (1, 2, 40) }, { s1 + 24000, juce::MidiMessage::aftertouchChange (1, 72, 110) },
                { s1 + 28800, juce::MidiMessage::pitchWheel (1, 12000) }, { s1 + 30000, juce::MidiMessage::controllerEvent (1, 11, 64) },
                { s1 + 33600, on (48, 110) }, { s1 + 40000, juce::MidiMessage::channelPressureChange (1, 100) },
                { 2 * s1 + 12000, off (48) }, { 2 * s1 + 12000, off (72) }, { 2 * s1 + 12000, off (67) } };
            auto viaDevice = play (midi, 4.0);
            writeWav (outDir.getChildFile ("organic-phrase.wav"), viaDevice);

            OrganicEngine e;
            e.setPatch (patch);
            e.params.life = 60.0f;
            e.params.tideDepth = 70.0f;
            e.useBuiltinsFor (patch);
            e.prepare (sr, block);
            juce::AudioBuffer<float> viaEngine (2, viaDevice.getNumSamples());
            viaEngine.clear();
            auto* L = viaEngine.getWritePointer (0);
            auto* R = viaEngine.getWritePointer (1);

            // In the same pieces the device renders in: blocks, split at each message
            for (int start = 0; start < viaEngine.getNumSamples(); start += block)
            {
                const int n = juce::jmin (block, viaEngine.getNumSamples() - start);
                int done = 0;
                for (auto& [pos, m] : midi)
                {
                    if (pos < start || pos >= start + n) continue;
                    if (pos - start > done) { e.render (L + start + done, R + start + done, pos - start - done); done = pos - start; }
                    if (m.isNoteOn()) e.noteOn (m.getNoteNumber(), m.getFloatVelocity());
                    else if (m.isNoteOff()) e.noteOff (m.getNoteNumber());
                    else if (m.isPitchWheel()) e.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
                    else if (m.isControllerOfType (1)) e.setModWheel ((float) m.getControllerValue() / 127.0f);
                    else if (m.isControllerOfType (2)) e.setBreath ((float) m.getControllerValue() / 127.0f);
                    else if (m.isControllerOfType (11)) e.setExpression ((float) m.getControllerValue() / 127.0f);
                    else if (m.isChannelPressure()) e.setChannelPressure ((float) m.getChannelPressureValue() / 127.0f);
                    else if (m.isAftertouch()) e.setNotePressure (m.getNoteNumber(), (float) m.getAfterTouchValue() / 127.0f);
                    else if (m.isSustainPedalOn()) e.setSustainPedal (true);
                    else if (m.isSustainPedalOff()) e.setSustainPedal (false);
                }
                if (done < n) e.render (L + start + done, R + start + done, n - done);
            }

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < viaDevice.getNumSamples(); ++i)
                    worst = juce::jmax (worst, std::abs (viaDevice.getSample (ch, i) - viaEngine.getSample (ch, i)));
            check (r, worst == 0.0f && viaDevice.getMagnitude (0, viaDevice.getNumSamples()) > 0.05f,
                   "the device plays OrganicEngine: the engine on its own, with its own built-in sources, renders the same, sample for sample (worst "
                   + juce::String (worst) + ")");

            // prepare() starts it afresh
            OrganicEngine again;
            again.setPatch (patch);
            again.useBuiltinsFor (patch);
            again.prepare (sr);
            juce::AudioBuffer<float> b1 (2, s1), b2 (2, s1);
            b1.clear(); b2.clear();
            again.noteOn (60, 0.8f); again.setModWheel (0.5f); again.render (b1.getWritePointer (0), b1.getWritePointer (1), s1);
            again.prepare (sr);
            again.noteOn (60, 0.8f); again.setModWheel (0.5f); again.render (b2.getWritePointer (0), b2.getWritePointer (1), s1);
            bool same = b1.getMagnitude (0, s1) > 0.01f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < s1; ++i) same = same && b1.getSample (ch, i) == b2.getSample (ch, i);
            check (r, same, "OrganicEngine prepare() starts it afresh: the same notes render the same audio every time");

            // A control written straight into params takes effect at the next render
            {
                auto firstHalf = [&] (OrganicEngine& en, juce::AudioBuffer<float>& b)
                {
                    b.clear();
                    en.prepare (sr);
                    en.noteOn (60, 0.8f);
                    en.render (b.getWritePointer (0), b.getWritePointer (1), s1 / 2);
                };
                juce::AudioBuffer<float> plainRun (2, s1), changed (2, s1);
                firstHalf (again, plainRun);
                again.render (plainRun.getWritePointer (0, s1 / 2), plainRun.getWritePointer (1, s1 / 2), s1 / 2);
                firstHalf (again, changed);
                bool changedAll = true;
                for (auto [id, value] : { std::pair<const char*, float> { "bright", 600.0f }, { "volume", -20.0f } })
                {
                    auto* v = again.params.find (id);
                    changedAll = changedAll && v != nullptr;
                    if (v != nullptr) *v = value;
                }
                again.render (changed.getWritePointer (0, s1 / 2), changed.getWritePointer (1, s1 / 2), s1 / 2);
                bool firstSame = true;
                for (int i = 0; i < s1 / 2; ++i) firstSame = firstSame && plainRun.getSample (0, i) == changed.getSample (0, i);
                const int a = s1 / 2, n = 2400;
                const auto before = plainRun.getRMSLevel (0, a, n), after = changed.getRMSLevel (0, a, n);
                check (r, changedAll && firstSame && after < before * 0.5f,
                       "a control written into OrganicEngine::params between renders takes effect in the next one (RMS "
                       + juce::String (juce::Decibels::gainToDecibels (before), 1) + " dB -> " + juce::String (juce::Decibels::gainToDecibels (after), 1) + " dB)");
                again.setPatch (patch);
            }

            // Patches, sources and controls
            bool patchesMatch = OrganicEngine::patchCount() == (int) OrganicDevice::patches().size()
                                && juce::String (OrganicEngine::patchName (1)) == "Glass Breath";
            for (int i = 0; i < OrganicEngine::patchCount(); ++i)
                patchesMatch = patchesMatch && juce::String (OrganicEngine::patchSource (i, 0)) == OrganicDevice::patches()[(size_t) i].a
                                            && juce::String (OrganicEngine::patchSource (i, 1)) == OrganicDevice::patches()[(size_t) i].b;
            check (r, patchesMatch, "OrganicEngine has the device's patches, source keys included");

            {
                bool allFound = true, rangesMatch = true;
                OrganicEngine::Params defaults;
                juce::String wrong;
                for (auto& c : OrganicEngine::controls())
                {
                    auto* v = defaults.find (c.id);
                    auto ap = od.getAutomatableParameterByID (c.id);
                    if (v == nullptr || ap == nullptr) { allFound = false; wrong << c.id << " "; continue; }
                    auto range = ap->getValueRange();
                    if (*v != c.defaultValue || std::abs (range.getStart() - c.min) > 1.0e-4f || std::abs (range.getEnd() - c.max) > 1.0e-4f
                        || std::abs (ap->getDefaultValue().value_or (-999.0f) - c.defaultValue) > 1.0e-6f
                        || (! c.choices.empty() && (int) c.choices.size() != juce::roundToInt (c.max - c.min) + 1))
                    { rangesMatch = false; wrong << c.id << " "; }
                }
                check (r, allFound && rangesMatch && OrganicEngine::controls().size() == OrganicEngine::Params::table().size()
                          && (int) OrganicEngine::controls().size() == (int) od.getAutomatableParameters().size(),
                       "every Organic control is a parameter, with its range and default" + (wrong.isEmpty() ? juce::String() : " (not: " + wrong + ")"));
            }

            od.loadPatch (0);
            od.loadSourcesNow();
            reset();
        }
    }

    //==============================================================================
    r.log << "Controls\n";
    {
        // The shared knob and fader styles, in both themes, for a look
        auto render = [&] (const juce::File& file)
        {
            juce::Component holder;
            holder.setSize (420, 240);
            juce::OwnedArray<juce::Slider> sliders;
            const float values[] = { 0.0f, 0.35f, 0.7f, 1.0f };
            for (int i = 0; i < 4; ++i)
            {
                auto* k = sliders.add (new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox));
                k->setRange (0.0, 1.0); k->setValue (values[i]);
                k->setBounds (14 + i * 56, 14, 48, 48);
                holder.addAndMakeVisible (k);
                auto* small = sliders.add (new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox));
                small->setRange (-1.0, 1.0); small->setValue (values[i] * 2.0 - 1.0);
                small->setBounds (24 + i * 56, 74, 28, 28);
                holder.addAndMakeVisible (small);
            }
            for (int i = 0; i < 3; ++i)
            {
                auto* f = sliders.add (new juce::Slider (juce::Slider::LinearVertical, juce::Slider::NoTextBox));
                f->setRange (-60.0, 6.0); f->setValue (i == 0 ? 0.0 : i == 1 ? -12.0 : -40.0);
                f->setColour (juce::Slider::trackColourId, Colours::clips[(size_t) (i * 2 + 1)]);
                f->setBounds (260 + i * 48, 10, 40, 220);
                holder.addAndMakeVisible (f);
            }
            juce::Image img (juce::Image::ARGB, holder.getWidth(), holder.getHeight(), true);
            {
                juce::Graphics g (img);
                g.fillAll (Colours::panel);
                holder.paintEntireComponent (g, true);
            }
            file.deleteFile();
            juce::PNGImageFormat png;
            if (auto out = file.createOutputStream())
                png.writeImageToStream (img, *out);
        };
        render (outDir.getChildFile ("controls.png"));
        Theme::apply (Themes::light); refreshLookAndFeel();
        render (outDir.getChildFile ("controls-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
        check (r, true, "rendered the knob and fader styles");
    }

    //==============================================================================
    r.log << "Haze\n";
    {
        auto& hz = make<HazeDevice> (*edit, HazeDevice::xmlTypeName);
        auto on  = [] (int note, int vel = 100) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) vel); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        auto reset = [&] { hz.initialise ({ te::TimePosition(), sr, block }); };
        auto play = [&] (std::vector<std::pair<int, juce::MidiMessage>> midi, double seconds)
        {
            juce::AudioBuffer<float> b (2, (int) (sr * seconds));
            b.clear();
            process (hz, b, midi);
            return b;
        };

        auto pitchOf = [] (const juce::AudioBuffer<float>& b, double fromSec, int w = 4096)
        {
            const int start = (int) (fromSec * sr);
            const int maxLag = (int) (sr / 40.0), minLag = (int) (sr / 2000.0);
            if (start + w + maxLag >= b.getNumSamples()) return 0.0;
            const float* x = b.getReadPointer (0) + start;
            std::vector<double> cm ((size_t) maxLag + 1, 1.0);
            double run = 0.0;
            for (int tau = 1; tau <= maxLag; ++tau)
            {
                double s = 0.0;
                for (int i = 0; i < w; ++i) { const double a = x[i] - x[i + tau]; s += a * a; }
                run += s;
                cm[(size_t) tau] = run > 0 ? s * tau / run : 1.0;
            }
            for (int tau = minLag; tau < maxLag; ++tau)
                if (cm[(size_t) tau] < 0.15)
                {
                    while (tau + 1 < maxLag && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau;
                    const double a = cm[(size_t) tau - 1], c = cm[(size_t) tau + 1], m = cm[(size_t) tau];
                    return sr / (tau + (a - c) / (2.0 * (a - 2.0 * m + c) + 1e-12));
                }
            return 0.0;
        };
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, double from, double to)
        {
            const int a = (int) (from * sr), n = juce::jmin (b.getNumSamples(), (int) (to * sr)) - a;
            return juce::Decibels::gainToDecibels ((b.getRMSLevel (0, a, n) + b.getRMSLevel (1, a, n)) * 0.5f, -200.0f);
        };
        auto brightness = [] (const juce::AudioBuffer<float>& b, double from, double to)
        {
            double e = 0, ed = 0;
            const float* x = b.getReadPointer (0);
            for (int i = (int) (from * sr) + 1; i < (int) (to * sr); ++i) { e += x[i] * x[i]; ed += (x[i] - x[i - 1]) * (x[i] - x[i - 1]); }
            return ed / (e + 1e-12);
        };
        auto clean = [&]
        {
            // A plain voice: no tape, no VHS, no space
            for (auto& p : { std::pair<ElsewhereDevice::Param, float> { hz.wave, (float) HazeDevice::soft }, { hz.detune, 0.0f }, { hz.sub, 0.0f }, { hz.noise, 0.0f },
                             { hz.range, 0.0f }, { hz.cutoff, 18000.0f }, { hz.resonance, 0.0f }, { hz.envAmount, 0.0f }, { hz.attack, 0.005f },
                             { hz.sustain, 100.0f }, { hz.release, 0.1f }, { hz.wow, 0.0f }, { hz.flutter, 0.0f }, { hz.age, 0.0f }, { hz.speed, 100.0f },
                             { hz.tracking, 0.0f }, { hz.bandwidth, 20000.0f }, { hz.chroma, 0.0f }, { hz.hiss, 0.0f }, { hz.spaceMix, 0.0f },
                             { hz.drift, 0.0f }, { hz.volume, 0.0f } })
                setParam (p.first, p.second);
        };

        // In tune when the tape is perfect; Speed slows it down like tape played slow
        {
            clean();
            auto b = play ({ { 0, on (69) } }, 1.0);
            const double f = pitchOf (b, 0.5);
            reset();
            setParam (hz.speed, 100.0f * std::pow (2.0f, -3.0f / 12.0f));
            auto slow = play ({ { 0, on (69) } }, 1.0);
            const double fs = pitchOf (slow, 0.5);
            reset();
            check (r, std::abs (f - 440.0) < 1.5 && std::abs (fs - 440.0 * std::pow (2.0, -3.0 / 12.0)) < 1.5,
                   "plays in tune with a perfect tape (" + juce::String (f, 1) + " Hz), Speed 84 % sounds 3 semitones down (" + juce::String (fs, 1) + " Hz)");
        }

        // Wow bends the pitch; with none it stays put
        {
            auto spreadCents = [&] (float wowAmount)
            {
                clean();
                setParam (hz.wow, wowAmount);
                auto b = play ({ { 0, on (69) } }, 4.0);
                reset();
                double lo = 1e9, hi = -1e9;
                for (double t = 1.0; t < 3.8; t += 0.1)
                {
                    const double f = pitchOf (b, t, 2048);
                    if (f <= 0) continue;
                    const double c = 1200.0 * std::log2 (f / 440.0);
                    lo = juce::jmin (lo, c); hi = juce::jmax (hi, c);
                }
                return hi - lo;
            };
            const auto none = spreadCents (0.0f), lots = spreadCents (100.0f);
            check (r, none < 3.0 && lots > 15.0 && lots < 120.0,
                   "Wow warbles the pitch (" + juce::String (lots, 1) + " cents peak to peak at 100 %, " + juce::String (none, 1) + " with none)");
        }

        // Age darkens and warms
        {
            auto bright = [&] (float ageAmount)
            {
                clean();
                setParam (hz.wave, (float) HazeDevice::saw);
                setParam (hz.age, ageAmount);
                setParam (hz.bandwidth, 9000.0f);
                auto b = play ({ { 0, on (57) } }, 1.0);
                reset();
                return brightness (b, 0.3, 1.0);
            };
            const auto young = bright (0.0f), old = bright (100.0f);
            check (r, old < young * 0.8, "Age rolls off the highs (brightness x" + juce::String (old / young, 2) + ")");
        }

        // Tracking drops the sound out now and then; a good tape never does
        {
            auto dropouts = [&] (float amount)
            {
                clean();
                setParam (hz.tracking, amount);
                auto b = play ({ { 0, on (57) } }, 8.0);
                reset();
                const auto ref = rmsDb (b, 0.5, 8.0);
                int dips = 0; bool in = false;
                for (double t = 0.5; t < 7.98; t += 0.02)
                {
                    const bool dip = rmsDb (b, t, t + 0.02) < ref - 4.0f;
                    if (dip && ! in) ++dips;
                    in = dip;
                }
                return dips;
            };
            const int none = dropouts (0.0f), lots = dropouts (100.0f);
            check (r, none == 0 && lots >= 2, "Tracking glitches drop out (" + juce::String (lots) + " dropouts in 8 s at 100 %, " + juce::String (none) + " with none)");
        }

        // Hiss only while the tape rolls
        {
            clean();
            setParam (hz.hiss, 100.0f);
            setParam (hz.age, 100.0f);
            auto idle = play ({}, 1.0);
            auto playing = play ({ { 0, on (60) }, { (int) (sr * 0.5), off (60) } }, 1.0);
            juce::AudioBuffer<float> after (2, (int) (sr * 6.0));
            after.clear();
            process (hz, after);
            check (r, rmsDb (idle, 0.0, 1.0) < -100.0f && rmsDb (after, 5.0, 6.0) < -80.0f,
                   "a silent track stays silent: hiss only while playing (idle " + juce::String (rmsDb (idle, 0.0, 1.0), 0) + " dB, 5 s after "
                   + juce::String (rmsDb (after, 5.0, 6.0), 0) + " dB)");
            reset();
        }

        // Everything up, eight voices and more: stable and under 0 dBFS
        {
            for (auto& p : { std::pair<ElsewhereDevice::Param, float> { hz.detune, 50.0f }, { hz.sub, 100.0f }, { hz.noise, 100.0f }, { hz.resonance, 100.0f },
                             { hz.envAmount, 100.0f }, { hz.wow, 100.0f }, { hz.flutter, 100.0f }, { hz.age, 100.0f }, { hz.tracking, 100.0f },
                             { hz.chroma, 3.0f }, { hz.hiss, 100.0f }, { hz.space, 100.0f }, { hz.spaceMix, 100.0f }, { hz.volume, 6.0f } })
                setParam (p.first, p.second);
            std::vector<std::pair<int, juce::MidiMessage>> midi;
            for (int n = 0; n < 10; ++n) midi.push_back ({ n * 200, on (30 + n * 6, 127) });
            midi.push_back ({ (int) sr, juce::MidiMessage::controllerEvent (1, 1, 127) });
            auto b = play (midi, 3.0);
            check (r, isFinite (b) && b.getMagnitude (0, b.getNumSamples()) <= 1.0f,
                   "10 notes on 8 voices, everything maxed, full warble: finite, peak " + juce::String (juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples())), 1) + " dBFS");
            reset();
        }

        // Every patch plays cleanly; a short demo of each to listen to (a slow minor 9 to major 7)
        {
            juce::String bad, levels;
            for (int i = 0; i < (int) HazeDevice::patches().size(); ++i)
            {
                hz.loadPatch (i);
                reset();
                std::vector<std::pair<int, juce::MidiMessage>> midi;
                const int first[] = { 50, 57, 60, 64, 65 }, second[] = { 46, 53, 57, 60, 62 };
                for (int k = 0; k < 5; ++k) midi.push_back ({ k * 600, on (first[k], 80) });
                for (int k = 0; k < 5; ++k) midi.push_back ({ (int) (sr * 3.0), off (first[k]) });
                for (int k = 0; k < 5; ++k) midi.push_back ({ (int) (sr * 3.0) + k * 600, on (second[k], 80) });
                for (int k = 0; k < 5; ++k) midi.push_back ({ (int) (sr * 6.0), off (second[k]) });
                auto b = play (midi, 9.0);
                const auto peak = b.getMagnitude (0, b.getNumSamples());
                const auto level = rmsDb (b, 0.5, 6.0);
                levels << HazeDevice::patches()[(size_t) i].name << " " << juce::String (level, 1) << "  ";
                if (! isFinite (b) || peak > 1.0f || level < -40.0f)
                    bad << HazeDevice::patches()[(size_t) i].name << " (" << juce::String (level, 1) << " dB, peak " << juce::String (peak, 2) << ") ";
                writeWav (outDir.getChildFile ("haze-" + juce::String (HazeDevice::patches()[(size_t) i].name).toLowerCase().replaceCharacter (' ', '-') + ".wav"), b);
            }
            check (r, bad.isEmpty(), "all " + juce::String ((int) HazeDevice::patches().size()) + " factory patches play cleanly" + (bad.isEmpty() ? juce::String() : ": " + bad));
            r.log << "        levels (dB RMS): " << levels << "\n";
        }

        hz.loadPatch (0);
        reset();
        juce::AudioBuffer<float> warm (2, (int) (sr * 1.0));
        warm.clear();
        process (hz, warm, { { 0, on (50) }, { 0, on (57) }, { 0, on (64) } });
        snapshot (hz, outDir.getChildFile ("haze.png"));
        snapshotWindow (hz, outDir.getChildFile ("haze-window.png"));
        Theme::apply (Themes::light); refreshLookAndFeel();
        snapshot (hz, outDir.getChildFile ("haze-light.png"));
        Theme::apply (Themes::dark); refreshLookAndFeel();
        reset();

        // The device is HazeEngine: the same notes through the engine on its own come out the
        // same, sample for sample (glitches, hiss, mod wheel and bend included)
        {
            hz.loadPatch (3);                       // Late Broadcast
            setParam (hz.tracking, 100.0f);
            setParam (hz.chroma, 3.0f);
            reset();
            const int s1 = (int) sr;
            std::vector<std::pair<int, juce::MidiMessage>> midi = {
                { 0, on (60) }, { 0, on (64, 70) }, { 4800, on (67, 40) }, { s1, off (60) },
                { s1 + 9600, juce::MidiMessage::controllerEvent (1, 1, 90) }, { s1 + 24000, off (64) }, { s1 + 24000, off (67) },
                { s1 + 28800, juce::MidiMessage::pitchWheel (1, 12000) }, { s1 + 33600, on (48, 110) }, { 2 * s1 + 12000, off (48) } };
            auto viaDevice = play (midi, 4.0);
            writeWav (outDir.getChildFile ("haze-phrase.wav"), viaDevice);

            HazeEngine e;
            e.setPatch (3);
            e.params.tracking = 100.0f;
            e.params.chroma = 3.0f;
            e.prepare (sr, block);
            juce::AudioBuffer<float> viaEngine (2, viaDevice.getNumSamples());
            viaEngine.clear();
            auto* L = viaEngine.getWritePointer (0);
            auto* R = viaEngine.getWritePointer (1);

            // In the same pieces the device renders in: blocks, split at each message
            for (int start = 0; start < viaEngine.getNumSamples(); start += block)
            {
                const int n = juce::jmin (block, viaEngine.getNumSamples() - start);
                int done = 0;
                for (auto& [pos, m] : midi)
                {
                    if (pos < start || pos >= start + n) continue;
                    if (pos - start > done) { e.render (L + start + done, R + start + done, pos - start - done); done = pos - start; }
                    if (m.isNoteOn()) e.noteOn (m.getNoteNumber(), m.getFloatVelocity());
                    else if (m.isNoteOff()) e.noteOff (m.getNoteNumber());
                    else if (m.isController()) e.setModWheel ((float) m.getControllerValue() / 127.0f);
                    else if (m.isPitchWheel()) e.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
                }
                if (done < n) e.render (L + start + done, R + start + done, n - done);
            }

            float worst = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < viaDevice.getNumSamples(); ++i)
                    worst = juce::jmax (worst, std::abs (viaDevice.getSample (ch, i) - viaEngine.getSample (ch, i)));
            check (r, worst == 0.0f && viaDevice.getMagnitude (0, viaDevice.getNumSamples()) > 0.05f,
                   "the device plays HazeEngine: the engine on its own renders the same, sample for sample (worst " + juce::String (worst) + ")");

            HazeEngine again;
            again.setPatch (3); again.params.tracking = 100.0f; again.params.chroma = 3.0f;
            again.prepare (sr);
            juce::AudioBuffer<float> b1 (2, s1), b2 (2, s1);
            b1.clear(); b2.clear();
            again.noteOn (60, 0.8f); again.render (b1.getWritePointer (0), b1.getWritePointer (1), s1);
            again.prepare (sr);
            again.noteOn (60, 0.8f); again.render (b2.getWritePointer (0), b2.getWritePointer (1), s1);
            bool same = true;
            for (int i = 0; i < s1; ++i) same = same && b1.getSample (0, i) == b2.getSample (0, i);
            check (r, same, "prepare() starts it afresh: the same notes render the same audio every time");
            check (r, HazeEngine::patchCount() == (int) HazeDevice::patches().size() && juce::String (HazeEngine::patchName (1)) == "Pastel Mall",
                   "the engine has the device's patches");

            // The controls the phone draws: each one is a real parameter, with the device's range and default
            {
                bool allFound = true, rangesMatch = true;
                HazeEngine::Params defaults;
                juce::String wrong;
                for (auto& c : HazeEngine::controls())
                {
                    auto* v = defaults.find (c.id);
                    auto ap = hz.getAutomatableParameterByID (c.id);
                    if (v == nullptr || ap == nullptr) { allFound = false; wrong << c.id << " "; continue; }
                    auto range = ap->getValueRange();
                    if (*v != c.defaultValue || std::abs (range.getStart() - c.min) > 1.0e-4f || std::abs (range.getEnd() - c.max) > 1.0e-4f
                        || (! c.choices.empty() && (int) c.choices.size() != juce::roundToInt (c.max - c.min) + 1))
                    { rangesMatch = false; wrong << c.id << " "; }
                }
                check (r, allFound && rangesMatch && HazeEngine::controls().size() == HazeEngine::Params::table().size(),
                       "every Haze control is a parameter, with its range and default" + (wrong.isEmpty() ? juce::String() : " (not: " + wrong + ")"));
            }
            hz.loadPatch (0);
            reset();        // bend and mod wheel back to rest
        }
    }

    //==============================================================================
    r.log << "Device windows (responsive)\n";
    {
        // The instruments have a layout of their own for their window (they used to show the chain
        // panel, wrapped into rows). It fills the window, and at the size it opens at, and when the
        // window is made bigger or somewhat smaller, every control is inside it and has some size.
        juce::String report;
        bool ok = true;
        for (auto type : { MonoDevice::xmlTypeName, PolyDevice::xmlTypeName, HazeDevice::xmlTypeName, OrganicDevice::xmlTypeName,
                           ModularDevice::xmlTypeName, DrumsDevice::xmlTypeName })
        {
            auto p = DeviceRegistry::create (*edit, type);
            keepAlive.push_back (p);
            auto& d = *dynamic_cast<ElsewhereDevice*> (p.get());
            d.initialise ({ te::TimePosition(), sr, block });
            const auto name = d.getName().fromFirstOccurrenceOf ("Elsewhere ", false, false);
            const auto open = d.getWindowViewSize();
            auto editor = d.createEditor();

            // One panel (no rows), at the size the device asks for, which fits a laptop screen
            juce::Component* view = editor->getNumChildComponents() == 1 ? editor->getChildComponent (0) : nullptr;
            bool good = view != nullptr && open.x >= 600 && open.x <= 1280 && open.y >= 400 && open.y <= 740
                          && editor->getWidth() == open.x + 16 && editor->getHeight() == open.y + 16;

            // Everything showing, in the panel's coordinates (scrolling areas count as one thing)
            std::function<void (juce::Component&, juce::String&)> inside = [&] (juce::Component& c, juce::String& stray)
            {
                for (auto* ch : c.getChildren())
                {
                    if (! ch->isVisible()) continue;
                    const auto b = view->getLocalArea (ch, ch->getLocalBounds());
                    if (b.getWidth() < 8 || b.getHeight() < 8 || ! view->getLocalBounds().expanded (1).contains (b))
                        stray << " " << (ch->getTitle().isNotEmpty() ? ch->getTitle() : juce::String (typeid (*ch).name())) << " " << b.toString();
                    if (dynamic_cast<juce::Viewport*> (ch) == nullptr)
                        inside (*ch, stray);
                }
            };

            for (auto size : { juce::Point<int> (open.x + 16, open.y + 16), juce::Point<int> (1500, 900), juce::Point<int> (900, 600) })
            {
                if (view == nullptr) break;
                editor->setSize (size.x, size.y);
                juce::String stray;
                inside (*view, stray);
                const bool fills = view->getBounds() == editor->getLocalBounds().reduced (8);
                good = good && fills && stray.isEmpty() && view->getNumChildComponents() > 0;
                if (! fills || stray.isNotEmpty())
                    report << name << " at " << size.x << "x" << size.y << (fills ? "" : " does not fill the window") << (stray.isEmpty() ? "" : " stray:" + stray) << "  ";
            }

            ok = ok && good;
            report << name << " " << open.x << "x" << open.y << (good ? "" : " (bad)") << "  ";

            editor->setSize (720, 560);
            juce::Image img (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
            { juce::Graphics g (img); editor->paintEntireComponent (g, true); }
            auto file = outDir.getChildFile ("window-" + juce::String (type).fromFirstOccurrenceOf ("elsewhere", false, false).toLowerCase() + ".png");
            file.deleteFile();
            juce::PNGImageFormat png;
            if (auto os = file.createOutputStream()) png.writeImageToStream (img, *os);
            editor.reset();
        }
        check (r, ok, "instrument windows have their own layout that fills the window with every control inside it: " + report.trim());
    }

    //==============================================================================
    r.log << "Arpeggiator\n";
    {
        // 120 BPM at 48 kHz: a beat is 24000 samples, a 16th is 6000
        auto& arp = make<ArpDevice> (*edit, ArpDevice::xmlTypeName);
        auto on  = [] (int note) { return juce::MidiMessage::noteOn (1, note, (juce::uint8) 100); };
        auto off = [] (int note) { return juce::MidiMessage::noteOff (1, note); };
        const std::vector<std::pair<int, juce::MidiMessage>> chord = { { 0, on (60) }, { 0, on (64) }, { 0, on (67) } };

        auto releaseAll = [&]
        {
            std::vector<std::pair<int, juce::MidiMessage>> offs;
            for (int n = 0; n < 128; ++n) offs.push_back ({ 0, off (n) });
            processMidi (arp, block, false, offs);
        };
        auto run = [&] (int samples) { releaseAll(); return noteOns (processMidi (arp, samples, true, chord)); };
        auto expect = [&] (const std::vector<std::pair<int, int>>& ons, std::vector<int> notes, int spacing, int firstAt = 0)
        {
            if (ons.size() < notes.size()) return false;
            for (size_t i = 0; i < notes.size(); ++i)
                if (ons[i].second != notes[i] || std::abs (ons[i].first - (firstAt + (int) i * spacing)) > 1) return false;
            return true;
        };

        auto ons = run (48000);
        check (r, expect (ons, { 60, 64, 67, 60, 64, 67 }, 6000), "Up, 1/16: " + describe (ons, 6));

        setParam (arp.mode, (float) ArpDevice::down);
        ons = run (48000);
        check (r, expect (ons, { 67, 64, 60, 67 }, 6000), "Down: " + describe (ons, 4));

        setParam (arp.mode, (float) ArpDevice::upDown);
        setParam (arp.octaves, 2.0f);
        ons = run (96000);
        check (r, expect (ons, { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60 }, 6000), "Up/Down over 2 octaves: " + describe (ons, 11));
        setParam (arp.octaves, 1.0f);
        setParam (arp.mode, (float) ArpDevice::up);

        setParam (arp.rateP, 1.0f);   // 1/8
        ons = run (48000);
        check (r, expect (ons, { 60, 64, 67, 60 }, 12000), "rate 1/8: " + describe (ons, 4));
        setParam (arp.rateP, 3.0f);

        setParam (arp.gate, 50.0f);
        releaseAll();
        auto events = processMidi (arp, 24000, true, chord);
        int firstOff = -1;
        for (auto& [pos, m] : events) if (m.isNoteOff() && m.getNoteNumber() == 60) { firstOff = pos; break; }
        check (r, std::abs (firstOff - 3000) <= 1, "gate 50 % ends notes after half a step (" + juce::String (firstOff) + ")");
        setParam (arp.gate, 60.0f);

        setParam (arp.swing, 50.0f);
        ons = run (24000);
        check (r, ons.size() >= 2 && std::abs (ons[1].first - 8000) <= 1, "swing 50 % pushes every second step by a sixth of a beat (" + juce::String (ons.size() >= 2 ? ons[1].first : -1) + ")");
        setParam (arp.swing, 0.0f);

        // Releasing the keys stops it (no latch); with latch it keeps going
        releaseAll();
        events = processMidi (arp, 48000, true, { { 0, on (60) }, { 0, on (64) }, { 13000, off (60) }, { 13000, off (64) } });
        auto count = noteOns (events).size();
        check (r, count == 3, "stops when keys are released (" + juce::String ((int) count) + " notes)");

        setParam (arp.latch, 1.0f);
        releaseAll();
        events = processMidi (arp, 48000, true, { { 0, on (60) }, { 0, on (64) }, { 13000, off (60) }, { 13000, off (64) } });
        count = noteOns (events).size();
        check (r, count == 8, "latch keeps playing after release (" + juce::String ((int) count) + " notes)");
        setParam (arp.latch, 0.0f);
        releaseAll();

        // Stopped transport: free-running, starting the moment the first key is pressed
        ons = noteOns (processMidi (arp, 24000, false, { { 1000, on (60) }, { 1000, on (67) } }));
        check (r, expect (ons, { 60, 67, 60 }, 6000, 1000), "free-running while stopped, starts with the key: " + describe (ons, 3));

        bool balanced = true;
        releaseAll();
        {
            std::map<int, int> open;
            for (auto& [pos, m] : processMidi (arp, 96000, true, { { 0, on (60) }, { 0, on (64) }, { 50000, off (60) }, { 50000, off (64) } }))
            {
                if (m.isNoteOn()) ++open[m.getNoteNumber()];
                if (m.isNoteOff()) --open[m.getNoteNumber()];
            }
            for (auto& [n, c] : open) balanced = balanced && c == 0;
        }
        check (r, balanced, "every note-on gets a note-off (no stuck notes)");
        snapshot (arp, outDir.getChildFile ("arp.png"));
    }

    //==============================================================================
    r.log << "Step Sequencer\n";
    {
        using S = StepsDevice;
        auto& seq = make<StepsDevice> (*edit, StepsDevice::xmlTypeName);
        auto ons = noteOns (processMidi (seq, 48000, true, {}));
        // Default pattern: steps 1,3,5,7,8 on in the first half bar -> 48, 55, 60, 55, 58
        bool ok = ons.size() >= 5 && ons[0] == std::pair<int, int> { 0, 48 } && ons[1] == std::pair<int, int> { 12000, 55 }
                  && ons[2] == std::pair<int, int> { 24000, 60 } && ons[3] == std::pair<int, int> { 36000, 55 } && ons[4] == std::pair<int, int> { 42000, 58 };
        check (r, ok, "plays its pattern with the transport: " + describe (ons, 5));

        ons = noteOns (processMidi (seq, 48000, false, {}));
        check (r, ons.empty(), "silent while stopped");

        setParam (seq.length, 4.0f);
        ons = noteOns (processMidi (seq, 48000, true, {}));
        check (r, ons.size() == 4 && ons[2] == std::pair<int, int> { 24000, 48 }, "length 4 loops every 4 steps: " + describe (ons, 4));
        setParam (seq.length, 16.0f);

        setParam (seq.follow, 1.0f);
        processMidi (seq, 4800, false, {});
        ons = noteOns (processMidi (seq, 24000, false, { { 500, juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100) } }));
        check (r, ons.size() >= 2 && ons[0] == std::pair<int, int> { 500, 62 } && ons[1] == std::pair<int, int> { 12500, 69 },
               "Follow Keys: moves to the held key and starts with it: " + describe (ons, 2));
        ons = noteOns (processMidi (seq, 24000, false, { { 0, juce::MidiMessage::noteOff (1, 62) } }));
        check (r, ons.empty(), "Follow Keys: stops when the key is released");
        setParam (seq.follow, 0.0f);

        // A plain pattern to measure against: one row, 4 steps of 1/16 (6000 samples), all on, all C2
        auto plain = [&]
        {
            seq.loadDefaults();
            setParam (seq.length, 4.0f);
            for (int i = 0; i < S::numSteps; ++i) seq.setOn (0, i, true);
            for (int l = 0; l < S::numLanes; ++l) seq.resetRow (0, l);
            seq.initialise ({ te::TimePosition(), sr, block });
        };
        auto pitches = [] (const std::vector<std::pair<int, int>>& v, size_t count)
        {
            std::vector<int> p;
            for (size_t i = 0; i < juce::jmin (count, v.size()); ++i) p.push_back (v[i].second);
            return p;
        };
        auto timesOf = [] (const std::vector<std::pair<int, int>>& v, int note)
        {
            std::vector<int> t;
            for (auto& o : v) if (o.second == note) t.push_back (o.first);
            return t;
        };
        auto offOf = [] (const std::vector<std::pair<int, juce::MidiMessage>>& events, int after)
        {
            for (auto& [pos, m] : events) if (m.isNoteOff() && pos > after) return pos;
            return -1;
        };

        plain();
        for (int i = 0; i < 4; ++i) seq.setStep (0, S::pitchLane, i, i);
        ons = noteOns (processMidi (seq, 48000, true, {}));
        check (r, pitches (ons, 8) == std::vector<int> { 48, 49, 50, 51, 48, 49, 50, 51 }, "Forward: " + describe (ons, 8));
        setParam (seq.direction, (float) S::backward);
        ons = noteOns (processMidi (seq, 48000, true, {}));
        check (r, pitches (ons, 8) == std::vector<int> { 51, 50, 49, 48, 51, 50, 49, 48 }, "Back: " + describe (ons, 8));
        setParam (seq.direction, (float) S::bounce);
        ons = noteOns (processMidi (seq, 48000, true, {}));
        check (r, pitches (ons, 8) == std::vector<int> { 48, 49, 50, 51, 50, 49, 48, 49 }, "Bounce: " + describe (ons, 8));
        setParam (seq.direction, (float) S::randomOrder);
        {
            seq.initialise ({ te::TimePosition(), sr, block });
            auto a = pitches (noteOns (processMidi (seq, 192000, true, {})), 32);
            seq.initialise ({ te::TimePosition(), sr, block });
            auto again = pitches (noteOns (processMidi (seq, 192000, true, {})), 32);
            std::set<int> seen (a.begin(), a.end());
            check (r, a.size() == 32 && seen.size() == 4 && a != std::vector<int> (a.size(), a[0]) && a == again,
                   "Random: every step turns up, out of order, and the same way on a second mixdown");
        }

        // Rows: each its own note and name, each going round in its own number of steps
        plain();
        {
            const int second = seq.addRow ("Bell", 72);
            seq.setRowSteps (second, 3);
            seq.setOn (second, 0, true);
            ons = noteOns (processMidi (seq, 72000, true, {}));
            check (r, second == 1 && seq.getNumRows() == 2 && seq.getRowName (1) == "Bell", "a second row, with a name");
            check (r, timesOf (ons, 72) == std::vector<int> { 0, 18000, 36000, 54000 } && timesOf (ons, 48).size() == 12,
                   "a row of 3 steps goes round under a row of 4 (" + juce::String ((int) timesOf (ons, 72).size()) + " and " + juce::String ((int) timesOf (ons, 48).size()) + " notes)");

            seq.setRowMute (second, true);
            ons = noteOns (processMidi (seq, 72000, true, {}));
            check (r, timesOf (ons, 72).empty() && timesOf (ons, 48).size() == 12, "a muted row is silent, the others carry on");
            seq.setRowMute (second, false);

            seq.duplicateRow (second);
            seq.setRowNote (2, 79);
            ons = noteOns (processMidi (seq, 24000, true, {}));
            check (r, seq.getNumRows() == 3 && timesOf (ons, 79) == std::vector<int> { 0, 18000 }, "a duplicated row plays the same steps on its own note");
            seq.removeRow (2);
            seq.removeRow (1);
            check (r, seq.getNumRows() == 1, "rows can be deleted");

            for (int i = 0; i < 12; ++i) seq.addRow ("Row", 60 + i);
            check (r, seq.getNumRows() == S::maxRows && ! seq.canAddRow(), "no more than " + juce::String (S::maxRows) + " rows");
        }

        // A drum pattern to start from
        plain();
        setParam (seq.length, 16.0f);
        seq.loadDrumRows();
        ons = noteOns (processMidi (seq, 96000, true, {}));
        check (r, seq.getNumRows() == 5 && seq.getRowName (0) == "Kick" && seq.getRowName (3) == "Closed Hat"
                   && timesOf (ons, 36) == std::vector<int> { 0, 48000 } && timesOf (ons, 37) == std::vector<int> { 24000, 72000 } && timesOf (ons, 39).size() == 8,
               "Start a Drum Pattern: rows for the first pads of the Drums, kick on 1 and 3, snare on 2 and 4, hats between");
        snapshot (seq, outDir.getChildFile ("steps-drums.png"));

        // Scale: pitches move down to the nearest note of it
        plain();
        for (int i = 0; i < 4; ++i) seq.setStep (0, S::pitchLane, i, i + 1);   // +1 +2 +3 +4
        setParam (seq.scale, 2.0f);   // Minor
        ons = noteOns (processMidi (seq, 24000, true, {}));
        check (r, pitches (ons, 4) == std::vector<int> { 48, 50, 51, 51 }, "Scale Minor keeps to the scale: " + describe (ons, 4));
        check (r, S::snapToScale (-1, 1) == -1 && S::snapToScale (-2, 1) == -3 && S::snapToScale (13, 1) == 12,
               "and works below the root and above the octave");

        // Repeat: several notes inside one step
        plain();
        seq.setStep (0, S::repeatLane, 1, 3);
        {
            auto events = processMidi (seq, 24000, true, {});
            ons = noteOns (events);
            const bool ok3 = ons.size() == 6 && ons[1].first == 6000 && ons[2].first == 8000 && ons[3].first == 10000 && ons[4].first == 12000;
            check (r, ok3, "Repeat 3x plays three notes in the step: " + describe (ons, 6));
            check (r, std::abs (offOf (events, 6000) - 7000) <= 1, "each as long as the Gate allows (" + juce::String (offOf (events, 6000) - 6000) + " samples)");
        }

        // Length: longer than the step, the note runs into the next one (a slide on a synth with glide)
        plain();
        seq.setStep (0, S::lengthLane, 0, 50);
        seq.setStep (0, S::lengthLane, 1, 300);
        seq.setStep (0, S::pitchLane, 2, 5);
        {
            auto events = processMidi (seq, 24000, true, {});
            int firstOff = -1, secondOff = -1;
            for (auto& [pos, m] : events)
                if (m.isNoteOff() && m.getNoteNumber() == 48) { if (firstOff < 0) firstOff = pos; else if (secondOff < 0) secondOff = pos; }
            check (r, std::abs (firstOff - 1500) <= 1, "Length 50 % halves the note (" + juce::String (firstOff) + ")");
            check (r, std::abs (secondOff - 15000) <= 1, "Length 300 % holds it over the next step (" + juce::String (secondOff) + ", next note at 12000)");
        }

        // Nudge: earlier or later than the grid
        plain();
        seq.setStep (0, S::nudgeLane, 1, 25);
        seq.setStep (0, S::nudgeLane, 2, -25);
        ons = noteOns (processMidi (seq, 24000, true, {}));
        check (r, ons.size() == 4 && ons[1].first == 7500 && ons[2].first == 10500 && ons[3].first == 18000, "Nudge moves a step late or early: " + describe (ons, 4));

        // Every: only on some trips round
        plain();
        seq.setStep (0, S::everyLane, 1, 2);    // 2nd of 2
        seq.setStep (0, S::everyLane, 2, 6);    // 1st of 4
        ons = noteOns (processMidi (seq, 4 * 24000, true, {}));
        {
            std::vector<int> at;
            for (auto& o : ons) at.push_back (o.first / 6000);
            check (r, at == std::vector<int> { 0, 2, 3,  4, 5, 7,  8, 11,  12, 13, 15 }, "Every: 2nd of 2 and 1st of 4 (" + juce::String ((int) at.size()) + " notes in 4 trips)");
        }

        // Chance
        plain();
        setParam (seq.length, 1.0f);
        seq.setStep (0, S::chanceLane, 0, 0);
        ons = noteOns (processMidi (seq, 96000, true, {}));
        check (r, ons.empty(), "Chance 0 % never plays");
        seq.setStep (0, S::chanceLane, 0, 50);
        {
            seq.initialise ({ te::TimePosition(), sr, block });
            auto a = noteOns (processMidi (seq, 6000 * 400, true, {}));
            seq.initialise ({ te::TimePosition(), sr, block });
            auto again = noteOns (processMidi (seq, 6000 * 400, true, {}));
            check (r, a.size() > 160 && a.size() < 240, "Chance 50 % plays about half the time (" + juce::String ((int) a.size()) + " of 400)");
            check (r, a == again, "and a mixdown comes out the same twice");
            // Playing live, each time a loop goes round the dice fall differently
            auto live = noteOns (processMidi (seq, 6000 * 400, true, {}, 0, false));
            auto liveAgain = noteOns (processMidi (seq, 6000 * 400, true, {}, 0, false));
            check (r, live != liveAgain, "but playing live, each time round a loop is different");
        }

        // 32 steps
        plain();
        setParam (seq.length, 32.0f);
        seq.setStep (0, S::pitchLane, 31, 7);
        ons = noteOns (processMidi (seq, 6000 * 33, true, {}));
        check (r, ons.size() == 33 && ons[31].second == 55 && ons[32].second == 48, "32 steps: the last one plays, then it starts over");

        // Everything at once, with swing: no note left hanging
        plain();
        setParam (seq.length, 16.0f);
        setParam (seq.swing, 60.0f);
        seq.addRow ("Two", 55);
        seq.addRow ("Three", 62);
        seq.setRowSteps (1, 5);
        seq.setRowSteps (2, 7);
        for (int row = 0; row < 3; ++row)
        {
            for (int l = 0; l < S::numLanes; ++l) seq.randomiseRow (row, l);
            seq.randomiseRowSteps (row);
            seq.setOn (row, 0, true);
        }
        {
            std::map<int, int> open;
            bool ordered = true;
            int last = 0;
            auto events = processMidi (seq, 480000, true, {});
            for (auto& [pos, m] : events)
            {
                if (m.isNoteOn()) ++open[m.getNoteNumber()];
                if (m.isNoteOff()) --open[m.getNoteNumber()];
                ordered = ordered && pos >= last;
                last = pos;
            }
            for (auto& [pos, m] : processMidi (seq, 48000, false, {}))
                if (m.isNoteOff()) --open[m.getNoteNumber()];
            bool balanced = true;
            for (auto& [n, c] : open) balanced = balanced && c == 0;
            check (r, balanced && ordered && noteOns (events).size() > 20, "three busy random rows: every note ends, nothing out of order (" + juce::String ((int) noteOns (events).size()) + " notes)");
        }
        setParam (seq.swing, 0.0f);
        snapshot (seq, outDir.getChildFile ("steps-rows.png"));
        snapshotWindow (seq, outDir.getChildFile ("steps-window.png"));

        // Tools
        plain();
        for (int i = 0; i < 4; ++i) seq.setStep (0, S::pitchLane, i, i);
        seq.shiftPattern (1);
        ons = noteOns (processMidi (seq, 24000, true, {}));
        check (r, pitches (ons, 4) == std::vector<int> { 51, 48, 49, 50 }, "Shift Right: " + describe (ons, 4));
        seq.shiftPattern (-1);
        seq.doubleLength();
        seq.setStep (0, S::pitchLane, 7, 12);
        ons = noteOns (processMidi (seq, 48000, true, {}));
        check (r, pitches (ons, 8) == std::vector<int> { 48, 49, 50, 51, 48, 49, 50, 60 }, "Double: a copy to change (" + describe (ons, 8) + ")");

        // Undo puts a step back
        plain();
        seq.beginEdit ("Draw Steps");
        seq.setOn (0, 2, false);
        const bool wasOff = ! seq.getPattern().rows[0].on[2];
        edit->getUndoManager().undo();
        check (r, wasOff && seq.getPattern().rows[0].on[2], "a step drawn or rubbed out can be undone");

        // An old project (one line of steps, kept as parameters) loads and plays as it did
        {
            juce::ValueTree old (te::IDs::PLUGIN);
            old.setProperty (te::IDs::type, StepsDevice::xmlTypeName, nullptr);
            old.setProperty ("length", 8.0f, nullptr);
            old.setProperty ("s2on", 1.0f, nullptr);
            old.setProperty ("s2pitch", 3.0f, nullptr);
            old.setProperty ("s3rep", 2.0f, nullptr);
            auto p = edit->getPluginCache().createNewPlugin (old);
            keepAlive.push_back (p);
            auto& loaded = *dynamic_cast<StepsDevice*> (p.get());
            loaded.initialise ({ te::TimePosition(), sr, block });
            ons = noteOns (processMidi (loaded, 96000, true, {}));
            check (r, loaded.getNumRows() == 1 && ons.size() >= 8 && ons[1] == std::pair<int, int> { 6000, 51 } && ons[2] == std::pair<int, int> { 12000, 55 }
                       && ons[3] == std::pair<int, int> { 15000, 55 } && ons[7] == std::pair<int, int> { 48000, 48 },
                   "a pattern saved before there were rows plays as it did, as one row: " + describe (ons, 8));

            // ... and rows are saved with the project and come back
            loaded.addRow ("Top", 84);
            loaded.setOn (1, 0, true);
            auto saved = loaded.state.createCopy();
            auto q = edit->getPluginCache().createNewPlugin (saved);
            keepAlive.push_back (q);
            auto& reopened = *dynamic_cast<StepsDevice*> (q.get());
            check (r, reopened.getNumRows() == 2 && reopened.getRowName (1) == "Top" && reopened.getPattern().rows[1].on[0] && reopened.getPattern().rows[1].note == 84,
                   "rows are saved with the project and come back");
        }

        // Picture: the pattern a new sequencer starts with
        plain();
        seq.loadDefaults();
        processMidi (seq, 4800, true, {});
        snapshot (seq, outDir.getChildFile ("steps.png"));
    }

    //==============================================================================
    r.log << "Reference\n";
    {
        // A quiet song (-26 dB sine) and a mix 20 dB louder
        auto song = outDir.getChildFile ("reference-song.wav");
        writeWav (song, sine (1000.0, 0.05f, 8.0));
        auto& ref = make<ReferenceDevice> (*edit, ReferenceDevice::xmlTypeName);
        ref.setFile (song);

        for (int i = 0; i < 500 && ref.getStatus() != ReferenceDevice::Status::ready; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

        check (r, ref.getStatus() == ReferenceDevice::Status::ready, "loads the song");

        // Played, not rendered: the context an export uses keeps the mix
        auto play = [&] (juce::AudioBuffer<float>& buf, double from, bool rendering)
        {
            for (int start = 0; start < buf.getNumSamples(); start += block)
            {
                auto n = juce::jmin (block, buf.getNumSamples() - start);
                te::MidiMessageArray mma;
                te::PluginRenderContext rc (&buf, start, n, &mma, 0.0,
                                            te::TimeRange (te::TimePosition::fromSeconds (from + start / sr), te::TimePosition::fromSeconds (from + (start + n) / sr)),
                                            true, false, rendering, false);
                ref.applyToBuffer (rc);
            }
        };

        auto mix = sine (1000.0, 0.5f, 4.0);
        play (mix, 0.0, false);
        check (r, std::abs (tailRmsDb (mix) - tailRmsDb (sine (1000.0, 0.5f, 4.0))) < 0.01f, "listening to the mix leaves it alone");
        check (r, std::abs (ref.getMixLufs() - ref.getReferenceLufs() - 20.0) < 0.5,
               "measures the mix 20 dB louder than the song (" + juce::String (ref.getMixLufs() - ref.getReferenceLufs(), 2) + " LU)");

        setParam (ref.listen, 1.0f);
        auto heard = sine (1000.0, 0.5f, 2.0);
        play (heard, 4.0, false);
        auto matched = tailRmsDb (heard) - tailRmsDb (sine (1000.0, 0.5f, 2.0));
        check (r, std::abs (matched) < 0.5f, "the song plays in its place, matched to the mix (" + juce::String (matched, 2) + " dB)");

        setParam (ref.match, 0.0f);
        auto plain = sine (1000.0, 0.5f, 2.0);
        play (plain, 4.0, false);
        auto unmatched = tailRmsDb (plain) - tailRmsDb (sine (1000.0, 0.5f, 2.0));
        check (r, std::abs (unmatched + 20.0f) < 0.5f, "with Match off it plays at its own level (" + juce::String (unmatched, 2) + " dB)");

        auto exported = sine (1000.0, 0.5f, 2.0);
        play (exported, 4.0, true);
        check (r, std::abs (tailRmsDb (exported) - tailRmsDb (sine (1000.0, 0.5f, 2.0))) < 0.01f, "exports always render the mix");
        snapshot (ref, outDir.getChildFile ("reference.png"));
    }

    r.log << "Somewhere Control\n";
    {
        auto& sc = make<SomewhereControlDevice> (*edit, SomewhereControlDevice::xmlTypeName);
        auto fresh = [&] (int effect, float x, float y)
        {
            setParam (sc.effect, (float) effect); setParam (sc.x, x); setParam (sc.y, y);
            setParam (sc.on, 0.0f); setParam (sc.latch, 0.0f);
            sc.initialise ({ te::TimePosition(), sr + 1.0, block });   // a new rate starts it afresh
            sc.initialise ({ te::TimePosition(), sr, block });
        };
        auto same = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                for (int i = 0; i < a.getNumSamples(); ++i)
                    if (a.getSample (ch, i) != b.getSample (ch, i))
                        return false;
            return true;
        };
        auto rmsDb = [] (const juce::AudioBuffer<float>& b, double from, double to)
        {
            auto s0 = (int) (from * sr), s1 = juce::jmin (b.getNumSamples(), (int) (to * sr));
            return juce::Decibels::gainToDecibels (b.getRMSLevel (0, s0, s1 - s0), -200.0f);
        };

        check (r, SomewhereControlDevice::effectNames().size() == PerformanceFx::numEffects, "a name for each of the seven effects");

        // Off, a loud sound passes through untouched (not even rounded off near full scale)
        fresh (PerformanceFx::filter, 0.05f, 0.9f);
        auto loud = sine (220.0, 0.98f, 1.0), dry = loud;
        process (sc, loud);
        check (r, same (loud, dry), "off: the sound passes through untouched");

        // Filter, left: a low-pass that takes out a 5 kHz tone
        fresh (PerformanceFx::filter, 0.05f, 0.0f);
        setParam (sc.on, 1.0f);
        auto high = sine (5000.0, 0.5f, 1.0);
        process (sc, high);
        check (r, tailRmsDb (high) < tailRmsDb (sine (5000.0, 0.5f, 1.0)) - 30.0f, "Filter at the left closes a low-pass (" + juce::String (tailRmsDb (high), 1) + " dB)");

        // Filter, right: a high-pass that takes out a 60 Hz tone
        fresh (PerformanceFx::filter, 0.95f, 0.0f);
        setParam (sc.on, 1.0f);
        auto low = sine (60.0, 0.5f, 1.0);
        process (sc, low);
        check (r, tailRmsDb (low) < tailRmsDb (sine (60.0, 0.5f, 1.0)) - 30.0f, "Filter at the right opens a high-pass (" + juce::String (tailRmsDb (low), 1) + " dB)");

        // Latch keeps it on with the pad let go
        fresh (PerformanceFx::filter, 0.05f, 0.0f);
        setParam (sc.latch, 1.0f);
        auto latched = sine (5000.0, 0.5f, 1.0);
        process (sc, latched);
        check (r, tailRmsDb (latched) < -20.0f, "Latch keeps the effect on");

        // Delay: a click while on comes back in time with the song, and keeps ringing once let go
        {
            fresh (PerformanceFx::delay, 0.9f, 0.5f);    // a 1/4 note: at 120 bpm, 500 ms
            juce::AudioBuffer<float> b (2, (int) (2.0 * sr));
            b.clear();
            for (int i = 0; i < 48; ++i) { b.setSample (0, 4800 + i, 0.5f); b.setSample (1, 4800 + i, 0.5f); }

            // On for the first 0.2 s (so the click goes in), then off
            juce::AudioBuffer<float> first (b.getArrayOfWritePointers(), 2, 0, (int) (0.2 * sr));
            setParam (sc.on, 1.0f);
            process (sc, first);
            setParam (sc.on, 0.0f);
            juce::AudioBuffer<float> rest (b.getArrayOfWritePointers(), 2, (int) (0.2 * sr), b.getNumSamples() - (int) (0.2 * sr));
            process (sc, rest, {}, 0.2);

            int echoAt = -1;
            for (int i = 4800 + 600; i < b.getNumSamples() && echoAt < 0; ++i)
                if (std::abs (b.getSample (0, i)) > 0.05f || std::abs (b.getSample (1, i)) > 0.05f)
                    echoAt = i;
            const auto ms = (echoAt - 4800) / sr * 1000.0;
            check (r, std::abs (ms - 500.0) < 15.0, "Delay: the echo comes back a 1/4 note later (" + juce::String (ms, 1) + " ms at 120 bpm)");
            check (r, rmsDb (b, 0.5, 1.0) > -70.0f, "Delay: the tail keeps ringing after the pad is let go");

            // Once the echoes have died away it goes back to leaving the sound untouched
            setParam (sc.y, 0.0f);
            auto later = sine (220.0, 0.98f, 4.0), later0 = later;
            process (sc, later, {}, 2.0);
            juce::AudioBuffer<float> end (later.getArrayOfWritePointers(), 2, (int) (3.5 * sr), (int) (0.5 * sr));
            juce::AudioBuffer<float> end0 (later0.getArrayOfWritePointers(), 2, (int) (3.5 * sr), (int) (0.5 * sr));
            check (r, same (end, end0), "once the echoes have died away the sound passes through untouched again");
        }

        // Stutter repeats what just played: a tone that stops keeps sounding while it's on
        {
            fresh (PerformanceFx::stutter, 0.3f, 0.0f);
            auto b = sine (440.0, 0.5f, 1.0);
            for (int i = (int) (0.5 * sr); i < b.getNumSamples(); ++i) { b.setSample (0, i, 0.0f); b.setSample (1, i, 0.0f); }
            juce::AudioBuffer<float> first (b.getArrayOfWritePointers(), 2, 0, (int) (0.5 * sr));
            process (sc, first);
            setParam (sc.on, 1.0f);
            juce::AudioBuffer<float> rest (b.getArrayOfWritePointers(), 2, (int) (0.5 * sr), (int) (0.5 * sr));
            process (sc, rest, {}, 0.5);
            check (r, rmsDb (b, 0.6, 1.0) > -15.0f, "Stutter repeats the slice that just played (" + juce::String (rmsDb (b, 0.6, 1.0), 1) + " dB after the tone stopped)");
        }

        // Tape stop slows to silence
        {
            fresh (PerformanceFx::tapeStop, 0.0f, 0.5f);     // 0.15 s
            setParam (sc.on, 1.0f);
            auto b = sine (440.0, 0.5f, 1.0);
            process (sc, b);
            check (r, rmsDb (b, 0.5, 1.0) < -60.0f, "Tape Stop slows to a stop (" + juce::String (rmsDb (b, 0.5, 1.0), 1) + " dB)");
        }

        // Every effect at every corner stays finite and in range, on a mono track too
        bool stable = true;
        for (int e = 0; e < PerformanceFx::numEffects; ++e)
            for (auto [x, y] : { std::pair { 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 1.0f }, { 1.0f, 0.0f } })
            {
                fresh (e, x, y);
                setParam (sc.on, 1.0f);
                auto b = sine (110.0, 0.9f, 0.5);
                process (sc, b);
                juce::AudioBuffer<float> mono (1, (int) (0.2 * sr));
                for (int i = 0; i < mono.getNumSamples(); ++i) mono.setSample (0, i, 0.9f * (float) std::sin (i * 0.02));
                process (sc, mono);
                if (! isFinite (b) || ! isFinite (mono) || b.getMagnitude (0, b.getNumSamples()) > 2.0f)
                {
                    stable = false;
                    r.log << "        " << SomewhereControlDevice::effectNames()[e] << " at " << x << ", " << y << " misbehaved\n";
                }
            }
        check (r, stable, "every effect at every corner of the pad stays finite, stereo and mono");

        // The readouts match the effect
        check (r, SomewhereControlDevice::describeX (PerformanceFx::filter, 0.5f) == "Off"
                   && SomewhereControlDevice::describeX (PerformanceFx::delay, 0.95f) == "1/4"
                   && SomewhereControlDevice::describeX (PerformanceFx::stutter, 0.95f) == "1/32", "the pad reads out what X does");

        // Added to a track, it goes after the track's own devices and before the fader and meter
        {
            auto scEdit = te::createEmptyEdit (engine, outDir.getChildFile ("somewhere-control-test.tracktionedit"));
            scEdit->ensureNumberOfAudioTracks (1);
            auto* t = te::getAudioTracks (*scEdit)[0];
            t->pluginList.insertPlugin (DeviceRegistry::create (*scEdit, EqDevice::xmlTypeName), 0, nullptr);
            auto* added = SomewhereControlDevice::findOrAddTo (*t);
            auto* again = SomewhereControlDevice::findOrAddTo (*t);
            juce::StringArray order;
            for (auto p : t->pluginList)
                order.add (dynamic_cast<SomewhereControlDevice*> (p) ? "control" : dynamic_cast<EqDevice*> (p) ? "eq"
                           : EngineHelpers::isMixerPlugin (*p) ? "mixer" : "other");
            check (r, added != nullptr && added == again, "a track gets one Somewhere Control, found again after");
            check (r, order.joinIntoString (" ").startsWith ("eq control mixer"), "it goes after the track's devices, before the fader (" + order.joinIntoString (" ") + ")");
        }

        fresh (PerformanceFx::filter, 0.3f, 0.7f);
        setParam (sc.on, 1.0f);
        snapshot (sc, outDir.getChildFile ("somewhere-control.png"));
        setParam (sc.on, 0.0f);
    }

    //==============================================================================
    r.log << "Tape Echo\n";
    {
        auto& ec = make<EchoDevice> (*edit, EchoDevice::xmlTypeName);

        // A machine in perfect order: only Head 1, one repeat, nothing but the echo
        auto fresh = [&] (double rateToUse = sr)
        {
            for (auto p : { ec.wow, ec.flutter, ec.wear, ec.noise, ec.drive, ec.spread, ec.intensity, ec.dry, ec.sync, ec.bass, ec.treble })
                setParam (p, 0.0f);
            setParam (ec.echo, 100.0f);
            setParam (ec.time, 200.0f);
            setParam (ec.heads[0], 1.0f); setParam (ec.heads[1], 0.0f); setParam (ec.heads[2], 0.0f);
            ec.initialise ({ te::TimePosition(), rateToUse, block });
        };

        // Short clicks (1 ms) at the given times
        auto clicks = [] (double seconds, std::vector<double> at, double rateToUse = sr)
        {
            juce::AudioBuffer<float> b (2, (int) (seconds * rateToUse));
            b.clear();
            const int w = (int) (rateToUse * 0.001);
            for (auto t : at)
                for (int i = 0; i < w; ++i)
                {
                    const int pos = (int) (t * rateToUse) + i;
                    if (pos >= b.getNumSamples()) break;
                    const auto v = 0.5f * (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / w));
                    b.setSample (0, pos, v); b.setSample (1, pos, v);
                }
            return b;
        };

        auto peakIn = [] (const juce::AudioBuffer<float>& b, int ch, double from, double to, double rateToUse = sr)
        {
            std::pair<int, float> best { -1, 0.0f };
            for (int i = (int) (from * rateToUse); i < juce::jmin (b.getNumSamples(), (int) (to * rateToUse)); ++i)
                if (std::abs (b.getSample (ch, i)) > best.second)
                    best = { i, std::abs (b.getSample (ch, i)) };
            return best;
        };

        fresh();
        auto b = clicks (1.0, { 0.0 });
        process (ec, b);
        auto first = peakIn (b, 0, 0.0, 1.0);
        check (r, std::abs (first.first - (9600 + 24)) <= 48, "Head 1 plays the sound back after the Repeat Rate (200 ms): " + juce::String (first.first / sr * 1000.0, 1) + " ms");
        check (r, peakIn (b, 0, 0.0, 0.15).second < 1.0e-4f, "Dry off: nothing but the echo");
        check (r, first.second > 0.1f && first.second < 0.6f, "the echo comes back at a sensible level (" + juce::String (first.second, 3) + " from 0.5)");

        for (int h = 1; h < 3; ++h)
        {
            fresh();
            setParam (ec.heads[0], 0.0f);
            setParam (ec.heads[(size_t) h], 1.0f);
            ec.initialise ({ te::TimePosition(), sr, block });
            b = clicks (1.0, { 0.0 });
            process (ec, b);
            const auto pk = peakIn (b, 0, 0.0, 1.0);
            check (r, std::abs (pk.first - (9600 * (h + 1) + 24)) <= 48,
                   "Head " + juce::String (h + 1) + " is " + juce::String (h + 1) + " times as far along the tape: " + juce::String (pk.first / sr * 1000.0, 1) + " ms");
        }

        fresh();
        setParam (ec.intensity, 50.0f);
        b = clicks (1.5, { 0.0 });
        process (ec, b);
        {
            const float p1 = peakIn (b, 0, 0.15, 0.25).second, p2 = peakIn (b, 0, 0.35, 0.45).second, p3 = peakIn (b, 0, 0.55, 0.65).second;
            check (r, p2 < p1 * 0.9f && p2 > p1 * 0.2f && p3 < p2 * 0.9f && p3 > p2 * 0.2f,
                   "Intensity feeds the echo back, each repeat quieter: " + juce::String (p1, 3) + " " + juce::String (p2, 3) + " " + juce::String (p3, 3));
        }

        // Each repeat has been through the tape once more, so it has less treble
        {
            fresh();
            setParam (ec.intensity, 60.0f);
            auto tone = sine (6000.0, 0.2f, 1.2);
            tone.applyGain ((int) (0.1 * sr), tone.getNumSamples() - (int) (0.1 * sr), 0.0f);
            process (ec, tone);
            const float hi1 = tone.getRMSLevel (0, (int) (0.22 * sr), (int) (0.06 * sr)), hi2 = tone.getRMSLevel (0, (int) (0.42 * sr), (int) (0.06 * sr));
            fresh();
            setParam (ec.intensity, 60.0f);
            tone = sine (300.0, 0.2f, 1.2);
            tone.applyGain ((int) (0.1 * sr), tone.getNumSamples() - (int) (0.1 * sr), 0.0f);
            process (ec, tone);
            const float lo1 = tone.getRMSLevel (0, (int) (0.22 * sr), (int) (0.06 * sr)), lo2 = tone.getRMSLevel (0, (int) (0.42 * sr), (int) (0.06 * sr));
            check (r, hi2 / hi1 < lo2 / lo1 * 0.8f, "repeats lose treble as they go round (6 kHz falls to " + juce::String (hi2 / hi1, 2) + ", 300 Hz to " + juce::String (lo2 / lo1, 2) + ")");
        }

        // Turning Repeat Rate changes the tape speed, so what is on the tape bends in pitch
        {
            fresh();
            setParam (ec.time, 100.0f);
            ec.initialise ({ te::TimePosition(), sr, block });
            auto a = sine (1000.0, 0.2f, 1.0);
            process (ec, a);
            const double before = yinPitch (a, (int) (0.6 * sr));
            setParam (ec.time, 200.0f);
            auto glide = sine (1000.0, 0.2f, 3.0);
            process (ec, glide, {}, 1.0);   // (carrying on from where the first second ended)
            const double during = yinPitch (glide, (int) (0.05 * sr)), after = yinPitch (glide, (int) (2.5 * sr));
            check (r, std::abs (before - 1000.0) < 3.0 && during < 950.0 && std::abs (after - 1000.0) < 3.0,
                   "slowing the tape drops the pitch of the echo, then it settles: " + juce::String (before, 1) + " -> " + juce::String (during, 1) + " -> " + juce::String (after, 1) + " Hz");
        }

        // Wow: the pitch of a steady note wavers
        {
            auto wander = [&] (float amount)
            {
                fresh();
                setParam (ec.wow, amount);
                auto tone = sine (1000.0, 0.2f, 6.0);
                process (ec, tone);
                double lo = 1.0e9, hi = 0.0;
                for (double t = 0.5; t < 5.8; t += 0.1)
                {
                    const double f = yinPitch (tone, (int) (t * sr), 2048);
                    lo = juce::jmin (lo, f); hi = juce::jmax (hi, f);
                }
                return hi - lo;
            };
            const double still = wander (0.0f), moving = wander (100.0f);
            check (r, still < 1.0 && moving > 4.0 && moving < 60.0, "Wow wavers the pitch (1 kHz moves by " + juce::String (moving, 1) + " Hz; " + juce::String (still, 2) + " Hz with it off)");
        }

        // Wear: the same sound comes back different every time
        {
            auto unevenness = [&] (float amount)
            {
                fresh();
                setParam (ec.wear, amount);
                std::vector<double> at;
                for (int i = 0; i < 60; ++i) at.push_back (0.3 * i);
                auto train = clicks (18.3, at);
                process (ec, train);
                float lo = 10.0f, hi = 0.0f;
                for (int i = 0; i < 60; ++i)
                {
                    const float p = peakIn (train, 0, 0.3 * i + 0.17, 0.3 * i + 0.25).second;
                    lo = juce::jmin (lo, p); hi = juce::jmax (hi, p);
                }
                return std::pair<float, float> { lo, hi };
            };
            const auto good = unevenness (0.0f), worn = unevenness (100.0f);
            check (r, good.first > good.second * 0.98f, "a machine in good order repeats every sound alike (" + juce::String (good.first, 3) + " to " + juce::String (good.second, 3) + ")");
            check (r, worn.first < worn.second * 0.75f, "a worn one doesn't: 60 identical sounds come back between " + juce::String (worn.first, 3) + " and " + juce::String (worn.second, 3));
        }

        // Nothing in, nothing out, however worn (until Noise is turned up)
        {
            fresh();
            for (auto p : { ec.wow, ec.flutter, ec.wear }) setParam (p, 100.0f);
            setParam (ec.intensity, 100.0f);
            juce::AudioBuffer<float> quiet (2, (int) (4.0 * sr));
            quiet.clear();
            process (ec, quiet);
            check (r, quiet.getMagnitude (0, quiet.getNumSamples()) < 1.0e-6f, "silence in gives silence out");

            setParam (ec.intensity, 30.0f);
            setParam (ec.noise, 100.0f);
            quiet.clear();
            process (ec, quiet);
            const float hissDb = tailRmsDb (quiet);
            check (r, hissDb > -75.0f && hissDb < -35.0f, "Noise adds tape hiss (" + juce::String (hissDb, 1) + " dB)");
        }

        // Mixed down twice, a worn machine misbehaves in exactly the same way
        {
            auto worn = [&]
            {
                auto train = clicks (6.0, { 0.0, 0.7, 1.4, 2.1, 2.8, 3.5 });
                process (ec, train);
                return train;
            };
            fresh();
            for (auto p : { ec.wow, ec.flutter, ec.wear }) setParam (p, 100.0f);
            setParam (ec.noise, 40.0f);
            setParam (ec.intensity, 70.0f);
            const auto one = worn(), two = worn();
            bool same = true;
            for (int ch = 0; same && ch < 2; ++ch)
                for (int i = 0; same && i < one.getNumSamples(); ++i)
                    same = one.getSample (ch, i) == two.getSample (ch, i);
            check (r, same, "two mixdowns of the same thing match sample for sample, wear and noise included");
        }

        // Intensity all the way up: the machine feeds on itself, and the tape holds it in
        {
            fresh();
            setParam (ec.intensity, 100.0f);
            setParam (ec.wear, 40.0f);
            for (auto h : ec.heads) setParam (h, 1.0f);
            auto run = sine (220.0, 0.5f, 20.0);
            run.applyGain ((int) (0.5 * sr), run.getNumSamples() - (int) (0.5 * sr), 0.0f);
            process (ec, run);
            const float peak = run.getMagnitude (0, run.getNumSamples());
            const float late = juce::Decibels::gainToDecibels (run.getRMSLevel (0, (int) (18.0 * sr), (int) (2.0 * sr)), -200.0f);
            check (r, isFinite (run) && peak < 4.0f && late > -30.0f,
                   "Intensity at 100 %: still going after 20 s (" + juce::String (late, 1) + " dB), never past " + juce::String (peak, 2));

            setParam (ec.intensity, 40.0f);
            juce::AudioBuffer<float> rest (2, (int) (12.0 * sr));
            rest.clear();
            process (ec, rest, {}, 20.0);
            check (r, tailRmsDb (rest) < -70.0f, "turned back down, it dies away (" + juce::String (tailRmsDb (rest), 1) + " dB)");
        }

        // In time with the song: 1/8 at 120 bpm is 250 ms
        fresh();
        setParam (ec.sync, 3.0f);
        ec.initialise ({ te::TimePosition(), sr, block });
        b = clicks (1.0, { 0.0 });
        process (ec, b);
        check (r, std::abs (peakIn (b, 0, 0.0, 1.0).first - (12000 + 24)) <= 60, "Sync 1/8 at 120 bpm: " + juce::String (peakIn (b, 0, 0.0, 1.0).first / sr * 1000.0, 1) + " ms");

        // Spread: Head 2 to the left, Head 3 to the right
        fresh();
        setParam (ec.spread, 100.0f);
        setParam (ec.heads[0], 0.0f); setParam (ec.heads[1], 1.0f);
        ec.initialise ({ te::TimePosition(), sr, block });
        b = clicks (1.0, { 0.0 });
        process (ec, b);
        check (r, peakIn (b, 1, 0.0, 1.0).second < peakIn (b, 0, 0.0, 1.0).second * 0.05f, "Spread puts Head 2 on the left");
        fresh();
        setParam (ec.spread, 100.0f);
        setParam (ec.heads[0], 0.0f); setParam (ec.heads[2], 1.0f);
        ec.initialise ({ te::TimePosition(), sr, block });
        b = clicks (1.0, { 0.0 });
        process (ec, b);
        check (r, peakIn (b, 0, 0.0, 1.0).second < peakIn (b, 1, 0.0, 1.0).second * 0.05f, "and Head 3 on the right");

        // The same times at another sample rate
        fresh (96000.0);
        b = clicks (1.0, { 0.0 }, 96000.0);
        process (ec, b);
        check (r, std::abs (peakIn (b, 0, 0.0, 1.0, 96000.0).first - (19200 + 48)) <= 96, "200 ms is 200 ms at 96 kHz too");

        // Dry on: the sound itself passes untouched
        fresh();
        setParam (ec.dry, 1.0f);
        setParam (ec.echo, 0.0f);
        ec.initialise ({ te::TimePosition(), sr, block });
        auto through = sine (440.0, 0.5f, 0.5);
        process (ec, through);
        check (r, std::abs (tailRmsDb (through) - tailRmsDb (sine (440.0, 0.5f, 0.5))) < 0.01f, "with Echo at 0 the sound passes unchanged");

        for (int i = 0; i < (int) EchoDevice::patches().size(); ++i)
        {
            ec.loadPatch (i);
            ec.initialise ({ te::TimePosition(), sr, block });
            auto s = sine (220.0, 0.9f, 4.0);
            s.applyGain ((int) (1.0 * sr), s.getNumSamples() - (int) (1.0 * sr), 0.0f);
            process (ec, s);
            check (r, isFinite (s) && s.getMagnitude (0, s.getNumSamples()) < 4.0f, juce::String ("patch ") + EchoDevice::patches()[(size_t) i].name + " is stable");
        }

        ec.loadPatch (2);
        ec.initialise ({ te::TimePosition(), sr, block });
        {
            // (some notes on the tape, so the picture has something to show)
            auto notes = sine (330.0, 0.6f, 0.75);
            for (int i = 0; i < notes.getNumSamples(); ++i)
                notes.applyGain (i, 1, 0.5f + 0.5f * (float) std::sin (i / sr * 40.0) > 0.7f ? 1.0f : 0.05f);
            process (ec, notes);
        }
        snapshot (ec, outDir.getChildFile ("echo.png"));
        snapshotWindow (ec, outDir.getChildFile ("echo-window.png"));
        for (auto p : { ec.wow, ec.flutter, ec.wear, ec.noise, ec.drive, ec.intensity }) setParam (p, 90.0f);
        setParam (ec.heads[1], 0.0f);
        snapshotWindow (ec, outDir.getChildFile ("echo-window-worn.png"));
    }

    //==============================================================================
    r.log << "End to end: Step Sequencer -> Synth -> Tape Echo, mixed down by the engine\n";
    {
        auto song = te::createEmptyEdit (engine, outDir.getChildFile ("steps-echo.tracktionedit"));
        song->ensureNumberOfAudioTracks (1);
        auto track = te::getAudioTracks (*song).getFirst();
        auto seqP = DeviceRegistry::create (*song, StepsDevice::xmlTypeName);
        auto synP = DeviceRegistry::create (*song, PolyDevice::xmlTypeName);
        auto echoP = DeviceRegistry::create (*song, EchoDevice::xmlTypeName);
        track->pluginList.insertPlugin (echoP, 0, nullptr);
        track->pluginList.insertPlugin (synP, 0, nullptr);
        track->pluginList.insertPlugin (seqP, 0, nullptr);

        auto& seq = *dynamic_cast<StepsDevice*> (seqP.get());
        auto& ec = *dynamic_cast<EchoDevice*> (echoP.get());
        for (int l = 0; l < StepsDevice::numLanes; ++l) seq.randomiseRow (0, l);
        seq.setStep (0, StepsDevice::chanceLane, 0, 100);
        seq.setStep (0, StepsDevice::chanceLane, 3, 50);
        seq.setStep (0, StepsDevice::repeatLane, 5, 3);
        setParam (ec.wear, 80.0f);
        setParam (ec.wow, 60.0f);
        setParam (ec.noise, 20.0f);
        setParam (ec.intensity, 60.0f);

        juce::BigInteger tracks;
        tracks.setBit (track->getIndexInEditTrackList());
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        auto mixdown = [&] (const juce::String& name)
        {
            auto out = outDir.getChildFile (name);
            out.deleteFile();
            te::Renderer::renderToFile ("Test", out, *song, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (6.0)), tracks, true, false, {}, true);
            juce::AudioBuffer<float> b;
            if (std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out)); reader != nullptr)
            {
                b.setSize (2, (int) reader->lengthInSamples);
                reader->read (&b, 0, b.getNumSamples(), 0, true, true);
            }
            return b;
        };

        const auto one = mixdown ("steps-echo.wav");
        check (r, one.getNumSamples() > 0 && isFinite (one) && one.getRMSLevel (0, 0, one.getNumSamples()) > 0.003f, "the pattern plays the synth through the echo");

        echoP->setEnabled (false);
        const auto without = mixdown ("steps-no-echo.wav");
        bool differs = false;
        for (int i = 0; ! differs && i < juce::jmin (one.getNumSamples(), without.getNumSamples()); ++i)
            differs = std::abs (one.getSample (0, i) - without.getSample (0, i)) > 1.0e-3f;
        check (r, differs, "and the echo is what makes the difference");
    }

    //==============================================================================
    r.log << "End to end: MIDI clip -> Arp -> Synth, rendered by the engine\n";
    {
        edit->ensureNumberOfAudioTracks (1);
        auto track = te::getAudioTracks (*edit).getFirst();
        auto& ts = edit->tempoSequence;
        auto clip = track->insertMIDIClip ("Chord", { ts.toTime (te::BeatPosition()), ts.toTime (te::BeatPosition::fromBeats (8.0)) }, nullptr);
        for (int n : { 60, 64, 67 })
            clip->getSequence().addNote (n, te::BeatPosition(), te::BeatDuration::fromBeats (8.0), 100, 0, nullptr);

        auto arpP = DeviceRegistry::create (*edit, ArpDevice::xmlTypeName);
        auto synP = DeviceRegistry::create (*edit, PolyDevice::xmlTypeName);
        track->pluginList.insertPlugin (synP, 0, nullptr);
        track->pluginList.insertPlugin (arpP, 0, nullptr);

        auto& syn = *dynamic_cast<PolyDevice*> (synP.get());
        setParam (syn.model, (float) PolyDevice::prism);
        setParam (syn.voiceMode, (float) PolyDevice::mono);
        setParam (syn.pWave1, 0.0f);
        setParam (syn.pMix2, 0.0f);
        setParam (syn.pCutoff, 20000.0f);
        setParam (syn.pEgInt, 0.0f);
        setParam (syn.pAR, 0.01f);
        setParam (syn.drift, 0.0f);
        setParam (syn.chorus, 0.0f);
        setParam (dynamic_cast<ArpDevice*> (arpP.get())->gate, 90.0f);

        auto out = outDir.getChildFile ("arp-synth-render.wav");
        out.deleteFile();
        juce::BigInteger tracks;
        tracks.setBit (track->getIndexInEditTrackList());
        te::Renderer::renderToFile ("Test", out, *edit, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (2.0)), tracks, true, false, {}, true);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out));
        check (r, reader != nullptr, "rendered");

        if (reader != nullptr)
        {
            juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
            reader->read (&b, 0, b.getNumSamples(), 0, true, true);
            const double rs = reader->sampleRate, sixteenth = rs * 0.125;

            // Pitch in the middle of each 16th note
            juce::StringArray heard;
            bool ok = true;
            const double expected[] = { 261.63, 329.63, 392.0, 261.63, 329.63, 392.0 };
            for (int s = 0; s < 6; ++s)
            {
                int from = (int) (s * sixteenth + sixteenth * 0.25), to = (int) (s * sixteenth + sixteenth * 0.8);
                double first = -1, last = -1; int n = 0;
                for (int i = from + 1; i < to; ++i)
                {
                    auto a = b.getSample (0, i - 1), c = b.getSample (0, i);
                    if (a < 0.0f && c >= 0.0f) { auto t = (double) (i - 1) + a / (a - c); if (first < 0) first = t; else ++n; last = t; }
                }
                auto f = n > 0 ? n * rs / (last - first) : 0.0;
                heard.add (juce::String (f, 0));
                ok = ok && std::abs (f - expected[s]) < 3.0;
            }
            check (r, ok, "arp feeds the synth in the real playback graph (Hz per 16th: " + heard.joinIntoString (", ") + ")");
        }

        track->pluginList.getPlugins().getFirst()->deleteFromParent();
    }

    //==============================================================================
    r.log << "LFO modulator\n";
    {
        edit->ensureNumberOfAudioTracks (2);
        auto track = te::getAudioTracks (*edit)[1];
        auto& ts = edit->tempoSequence;
        auto clip = track->insertMIDIClip ("Pad", { ts.toTime (te::BeatPosition()), ts.toTime (te::BeatPosition::fromBeats (8.0)) }, nullptr);
        clip->getSequence().addNote (45, te::BeatPosition(), te::BeatDuration::fromBeats (8.0), 100, 0, nullptr);

        auto synP = DeviceRegistry::create (*edit, PolyDevice::xmlTypeName);
        track->pluginList.insertPlugin (synP, 0, nullptr);
        auto& syn = *dynamic_cast<PolyDevice*> (synP.get());
        setParam (syn.model, (float) PolyDevice::prism);
        setParam (syn.pWave1, 2.0f);
        setParam (syn.pMix2, 0.0f);
        setParam (syn.pPoles, 1.0f);
        setParam (syn.pCutoff, 300.0f);
        setParam (syn.pEgInt, 0.0f);
        setParam (syn.pKey, 0.0f);
        setParam (syn.pRes, 0.0f);
        setParam (syn.pAA, 0.001f);
        setParam (syn.drift, 0.0f);

        auto lfo = Modulators::addLFO (*track);
        check (r, lfo != nullptr, "LFO added to the track");

        if (lfo != nullptr)
        {
            lfo->waveParam->setParameter ((float) te::LFOModifier::waveSquare, juce::sendNotification);
            lfo->rateTypeParam->setParameter ((float) te::ModifierCommon::hertz, juce::sendNotification);
            lfo->rateParam->setParameter (2.0f, juce::sendNotification);
            check (r, Modulators::canTarget (*track, *lfo, syn.pCutoff.param()), "can target the synth's cutoff");
            check (r, ! Modulators::canTarget (*track, *lfo, *lfo->rateParam), "can't target itself");

            syn.pCutoff.param().addModifier (*lfo, 0.5f);
            check (r, Modulators::getTargets (*track, *lfo).size() == 1, "target listed");

            auto out = outDir.getChildFile ("lfo-render.wav");
            out.deleteFile();
            juce::BigInteger tracks;
            tracks.setBit (track->getIndexInEditTrackList());
            te::Renderer::renderToFile ("Test", out, *edit, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (2.0)), tracks, true, false, {}, true);

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out));

            if (reader != nullptr)
            {
                juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
                reader->read (&b, 0, b.getNumSamples(), 0, true, true);
                const int win = (int) (reader->sampleRate * 0.25);

                // High-frequency energy per 250 ms window (48 dB/oct high-pass at 2 kHz)
                std::array<Biquad, 4> hpf;
                for (auto& h : hpf) h.set (Biquad::Type::highPass, 2000.0, 0.7, 0.0, reader->sampleRate);
                std::vector<double> energy;
                for (int w = 0; w + win <= b.getNumSamples(); w += win)
                {
                    double e = 0;
                    for (int i = w; i < w + win; ++i)
                    {
                        float v = b.getSample (0, i);
                        for (auto& h : hpf) v = h.process (v, 0);
                        if (i > w + win / 4) e += v * v;     // skip the transition
                    }
                    energy.push_back (10.0 * std::log10 (e + 1e-20));
                }

                juce::StringArray desc;
                bool alternates = energy.size() >= 6;
                for (size_t i = 0; i < energy.size(); ++i)
                {
                    desc.add (juce::String (energy[i], 0));
                    if (i > 0 && i < 6) alternates = alternates && std::abs (energy[i] - energy[i - 1]) > 20.0;
                }
                check (r, alternates, "square LFO at 2 Hz opens and closes the filter every 250 ms (dB: " + desc.joinIntoString (" ") + ")");
            }

            // The panel
            {
                LfoPanel panel (*track, *lfo);
                panel.setSize (LfoPanel::idealWidth, 180);
                juce::Image img (juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
                { juce::Graphics g (img); g.fillAll (Colours::panelDark); panel.paintEntireComponent (g, true); }
                auto f = outDir.getChildFile ("lfo.png");
                f.deleteFile();
                if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
            }

            lfo->remove();
            check (r, syn.pCutoff.param().getAssignments().isEmpty(), "deleting the LFO removes its assignment");
        }
    }

    //==============================================================================
    r.log << "Session -> Arrangement recording\n";
    {
        edit->ensureNumberOfAudioTracks (3);
        auto track = te::getAudioTracks (*edit)[2];
        auto& ts = edit->tempoSequence;
        auto beats = [&] (double b) { return ts.toTime (te::BeatPosition::fromBeats (b)); };

        // Existing arrangement clip over bars 1-4
        auto existing = track->insertMIDIClip ("Old", { beats (0), beats (16) }, nullptr);
        // A 4-beat looping Session clip
        edit->getSceneList().ensureNumberOfScenes (1);
        track->getClipSlotList().ensureNumberOfSlots (1);
        auto slot = track->getClipSlotList().getClipSlots()[0];
        auto slotClip = te::insertMIDIClip (*slot, "Loop", { beats (0), beats (4) });
        slotClip->getSequence().addNote (60, te::BeatPosition(), te::BeatDuration::fromBeats (1.0), 100, 0, nullptr);
        slotClip->setLoopRangeBeats ({ te::BeatPosition(), te::BeatDuration::fromBeats (4.0) });
        const auto slotStateBefore = slotClip->state.toXmlString();

        auto clipsOnTrack = [&]
        {
            std::vector<std::pair<double, double>> v;
            for (auto c : track->getClips())
                v.push_back ({ ts.toBeats (c->getPosition().getStart()).inBeats(), ts.toBeats (c->getPosition().getEnd()).inBeats() });
            std::sort (v.begin(), v.end());
            return v;
        };

        auto newClip = SessionRecorder::writeTake (*track, *slotClip, { te::BeatPosition::fromBeats (8.0), te::BeatDuration::fromBeats (4.0) }, te::BeatPosition());
        auto v = clipsOnTrack();
        bool ok = newClip != nullptr && v.size() == 3
                  && std::abs (v[0].first - 0) < 1e-6 && std::abs (v[0].second - 8) < 1e-6
                  && std::abs (v[1].first - 8) < 1e-6 && std::abs (v[1].second - 12) < 1e-6
                  && std::abs (v[2].first - 12) < 1e-6 && std::abs (v[2].second - 16) < 1e-6;
        juce::StringArray d;
        for (auto& [a, b] : v) d.add (juce::String (a) + "-" + juce::String (b));
        check (r, ok, "launched clip lands on beats 8-12, replacing what was there (" + d.joinIntoString (", ") + ")");
        check (r, newClip != nullptr && newClip->getName() == "Loop" && newClip->itemID != slotClip->itemID
                  && dynamic_cast<te::MidiClip*> (newClip) != nullptr && dynamic_cast<te::MidiClip*> (newClip)->getSequence().getNumNotes() == 1,
               "the timeline gets a copy of the clip with its notes");
        check (r, slotClip->state.toXmlString() == slotStateBefore && slot->getClip() == slotClip.get(), "the Session clip is left untouched");

        // Playing since beat 0, recording started at beat 22: captured from 22, starting 2 beats into its 4-beat loop
        auto late = SessionRecorder::writeTake (*track, *slotClip, { te::BeatPosition::fromBeats (0.0), te::BeatDuration::fromBeats (28.0) },
                                                te::BeatPosition::fromBeats (22.0));
        const auto startBeat = late != nullptr ? ts.toBeats (late->getPosition().getStart()).inBeats() : -1.0;
        const auto offsetBeats = late != nullptr ? late->getPosition().getOffset().inSeconds() * ts.getTempo (0)->getBpm() / 60.0 : -1.0;
        check (r, std::abs (startBeat - 22.0) < 1e-6 && std::abs (offsetBeats - 2.0) < 1e-6,
               "a clip already playing is captured from the record point, at the right place in its loop (start "
                   + juce::String (startBeat) + ", offset " + juce::String (offsetBeats, 3) + " beats)");

        check (r, SessionRecorder::writeTake (*track, *slotClip, { te::BeatPosition::fromBeats (30.0), te::BeatDuration::fromBeats (4.0) },
                                              te::BeatPosition::fromBeats (40.0)) == nullptr, "nothing recorded from before the record point");
        juce::ignoreUnused (existing);

        // End to end: the engine really playing (driven block by block, no sound card),
        // a clip launched and stopped while recording
        for (auto c : juce::Array<te::Clip*> (track->getClips())) c->removeFromParent();
        {
            te::test_utilities::EnginePlayer player (engine, { .sampleRate = sr, .blockSize = block, .inputChannels = 0, .outputChannels = 2,
                                                                .inputNames = {}, .outputNames = {} });
            SessionRecorder recorder (*edit);
            auto& tc = edit->getTransport();
            tc.ensureContextAllocated();
            tc.setPosition (te::TimePosition());

            // No count-in: it's an app setting, so whatever was last chosen would shift the take
            const auto countIn = edit->getCountInMode();
            edit->setCountInMode (te::Edit::CountIn::none);

            auto run = [&] (double seconds)
            {
                for (double t = 0; t < seconds; t += 0.02)
                {
                    player.process ((int) (sr * 0.02));
                    recorder.update();
                }
            };

            tc.record (false, true);
            run (1.0);                                       // 2 beats of recording before the launch
            if (auto lh = slotClip->getLaunchHandle()) lh->play ({});
            track->playSlotClips = true;
            run (3.0);                                       // plays ~6 beats
            if (auto lh = slotClip->getLaunchHandle()) lh->stop ({});
            run (1.0);
            tc.stop (false, false);
            recorder.update();

            auto v2 = clipsOnTrack();
            juce::StringArray d2;
            for (auto& [a, b] : v2) d2.add (juce::String (a, 2) + "-" + juce::String (b, 2));
            check (r, v2.size() == 1 && std::abs (v2[0].first - 2.0) < 0.1 && std::abs (v2[0].second - 8.0) < 0.1 && recorder.getNumCaptured() == 1,
                   "recording while launching: the clip appears on the timeline where it played (" + d2.joinIntoString (", ") + " beats)");
            track->playSlotClips = false;
            edit->setCountInMode (countIn);
        }
    }

    //==============================================================================
    r.log << "Drum patterns\n";
    {
        using namespace drums;
        SectionSettings s;
        s.fills = 0.0; s.fillAtEnd = false; s.crashAtStart = false; s.complexity = 0.0; s.loudness = 0.6;
        s.variation = { 0.0, 0.0, 0.0 }; s.seed = 42;

        auto at = [] (const std::vector<Hit>& hits, Piece p)
        {
            std::vector<double> v;
            for (auto& h : hits) if (h.piece == p) v.push_back (std::round (h.beat * 4.0) / 4.0);
            return v;
        };

        auto rock = generate (s, 0.0, 4.0, 4);
        check (r, at (rock, Piece::kick) == std::vector<double> { 0.0, 2.0 } && at (rock, Piece::snare) == std::vector<double> { 1.0, 3.0 },
               "simple rock: kick on 1 and 3, snare on 2 and 4");
        check (r, at (rock, Piece::closedHat).size() == 8, "simple rock: 8th-note hats (" + juce::String ((int) at (rock, Piece::closedHat).size()) + ")");

        check (r, generate (s, 0.0, 16.0, 4).size() == generate (s, 0.0, 16.0, 4).size()
                  && std::abs (generate (s, 0.0, 16.0, 4)[5].beat - generate (s, 0.0, 16.0, 4)[5].beat) < 1.0e-12, "same settings and seed give the same pattern");

        auto busy = s; busy.complexity = 1.0;
        check (r, generate (busy, 0.0, 16.0, 4).size() > generate (s, 0.0, 16.0, 4).size() * 3 / 2, "more complexity, more hits ("
                  + juce::String ((int) generate (s, 0.0, 16.0, 4).size()) + " -> " + juce::String ((int) generate (busy, 0.0, 16.0, 4).size()) + ")");

        auto avgVel = [] (const std::vector<Hit>& hits) { double v = 0; for (auto& h : hits) v += h.velocity; return v / (double) juce::jmax ((int) hits.size(), 1); };
        auto soft = s; soft.loudness = 0.1; auto loud = s; loud.loudness = 1.0;
        check (r, avgVel (generate (loud, 0.0, 16.0, 4)) > avgVel (generate (soft, 0.0, 16.0, 4)) + 0.25, "louder plays harder");

        auto fill = s; fill.fillAtEnd = true;
        auto withFill = generate (fill, 0.0, 16.0, 4);
        bool tomsAtEnd = false, hatsInFill = false;
        for (auto& h : withFill)
        {
            if (h.beat >= 15.0 && (h.piece == Piece::tomHigh || h.piece == Piece::tomMid || h.piece == Piece::tomLow)) tomsAtEnd = true;
            if (h.beat >= 15.0 && h.piece == Piece::closedHat) hatsInFill = true;
        }
        check (r, tomsAtEnd && ! hatsInFill, "fill at the end of the section: toms, and the hats make room");

        auto noHats = s; noHats.partOn[1] = false;
        check (r, at (generate (noHats, 0.0, 8.0, 4), Piece::closedHat).empty() && ! at (generate (noHats, 0.0, 8.0, 4), Piece::kick).empty(),
               "switching off Hats & Cymbals removes them, keeps the rest");

        // A section starting on beat 3 of a bar still lines up with the song's bars
        auto mid = generate (s, 2.0, 4.0, 4);
        check (r, at (mid, Piece::kick) == std::vector<double> { 0.0, 2.0 } && at (mid, Piece::snare) == std::vector<double> { 1.0, 3.0 },
               "a section starting mid-bar follows the song's bar grid");

        bool allStylesOk = true;
        for (int st = 0; st < (int) Style::count; ++st)
        {
            auto x = s; x.style = (Style) st; x.complexity = 0.7; x.fills = 0.7; x.fillAtEnd = true; x.variation = { 0.6, 0.6, 0.6 };
            auto h = generate (x, 0.0, 32.0, 4);
            bool inRange = ! h.empty();
            for (auto& hit : h) inRange = inRange && hit.beat >= 0.0 && hit.beat < 32.0 && hit.velocity > 0.0f && hit.velocity <= 1.0f;
            allStylesOk = allStylesOk && inRange;
        }
        check (r, allStylesOk, "every style generates hits inside the section with sane velocities");
    }

    r.log << "Drum tracks\n";
    {
        edit->ensureNumberOfAudioTracks (4);
        auto track = te::getAudioTracks (*edit)[3];
        for (auto c : juce::Array<te::Clip*> (track->getClips())) c->removeFromParent();
        auto kit = DeviceRegistry::create (*edit, "elsewhereDrums");
        track->pluginList.insertPlugin (kit, 0, nullptr);
        track->state.setProperty (DrumTrack::trackProp, true, nullptr);
        DrumTrack::Watcher watcher (*edit, [] { return false; });

        auto& um = edit->getUndoManager();
        um.beginNewTransaction ("create");
        auto section = DrumTrack::createSection (*track, 0.0, 16.0, "Verse", drums::Style::rock);
        check (r, section != nullptr && section->getSequence().getNumNotes() > 20, "a section is created with a pattern ("
                  + juce::String (section != nullptr ? section->getSequence().getNumNotes() : 0) + " notes)");

        auto notesSummary = [&]
        {
            juce::StringArray s;
            for (auto n : section->getSequence().getNotes()) s.add (juce::String (n->getNoteNumber()) + "@" + juce::String (n->getStartBeat().inBeats(), 2));
            return s.joinIntoString (",");
        };

        bool elsewhereNotes = true;
        for (auto n : section->getSequence().getNotes()) elsewhereNotes = elsewhereNotes && n->getNoteNumber() >= 36 && n->getNoteNumber() <= 51;
        check (r, elsewhereNotes, "notes are laid out for Elsewhere Drums' pads");

        const auto before = notesSummary();
        um.beginNewTransaction ("style");
        section->state.setProperty (DrumTrack::Props::style, (int) drums::Style::house, &um);
        const auto house = notesSummary();
        check (r, house != before, "changing the style rewrites the pattern");
        um.undo();
        check (r, notesSummary() == before && DrumTrack::readSettings (section->state).style == drums::Style::rock, "undo brings back the style and its notes together");

        um.beginNewTransaction ("resize");
        auto& ts = edit->tempoSequence;
        section->setLength (ts.toTime (te::BeatPosition::fromBeats (32.0)) - ts.toTime (te::BeatPosition()), true);
        double lastBeat = 0.0;
        for (auto n : section->getSequence().getNotes()) lastBeat = juce::jmax (lastBeat, n->getStartBeat().inBeats());
        check (r, lastBeat > 28.0, "lengthening the section extends the pattern (last hit at beat " + juce::String (lastBeat, 1) + ")");

        // Swap the kit for a drum plug-in style instrument: notes follow the General MIDI map
        um.beginNewTransaction ("instrument");
        kit->deleteFromParent();
        EngineHelpers::insertBuiltInPlugin (*track, te::FourOscPlugin::xmlTypeName, 0);
        bool gm = false;
        for (auto n : section->getSequence().getNotes()) gm = gm || n->getNoteNumber() == 38 || n->getNoteNumber() == 42;
        check (r, gm, "with another instrument, notes use the General MIDI drum map (for Sitala, MT Power Drum Kit ...)");
        for (auto p : juce::Array<te::Plugin*> (track->pluginList.getPlugins().begin(), track->pluginList.getPlugins().size()))
            if (p->isSynth()) p->deleteFromParent();
        auto kit2 = DeviceRegistry::create (*edit, "elsewhereDrums");
        track->pluginList.insertPlugin (kit2, 0, nullptr);

        // Kit note layouts: pads from C1, a custom map, back to automatic
        {
            auto firstNotes = [&]
            {
                std::set<int> notes;
                for (auto nt : section->getSequence().getNotes()) notes.insert (nt->getNoteNumber());
                return notes;
            };
            um.beginNewTransaction ("layout");
            track->state.setProperty (DrumTrack::noteMapProp, "gm", &um);
            auto gmNotes = firstNotes();
            track->state.setProperty (DrumTrack::noteMapProp, "c1", &um);
            auto c1Notes = firstNotes();
            check (r, gmNotes.count (38) == 1 && c1Notes.count (38) == 0 && c1Notes.count (37) == 1,
                   "choosing a kit layout rewrites the notes (GM snare 38, pads-from-C1 snare 37)");

            juce::StringArray custom;
            for (int i = 0; i < (int) drums::Piece::count; ++i) custom.add (juce::String (60 + i));
            track->state.setProperty (DrumTrack::noteMapProp, custom.joinIntoString (","), &um);
            auto customNotes = firstNotes();
            check (r, customNotes.count (60) == 1 && customNotes.count (61) == 1 && *customNotes.begin() >= 60, "a custom kit layout is used for every note");
            track->state.setProperty (DrumTrack::noteMapProp, {}, &um);
        }

        // Render it: the kit really plays the section
        auto out = outDir.getChildFile ("drum-track-render.wav");
        out.deleteFile();
        juce::BigInteger tracks;
        tracks.setBit (track->getIndexInEditTrackList());
        te::Renderer::renderToFile ("Test", out, *edit, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (4.0)), tracks, true, false, {}, true);
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out));
        float peak = 0.0f;
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
            reader->read (&b, 0, b.getNumSamples(), 0, true, true);
            peak = b.getMagnitude (0, b.getNumSamples());
        }
        check (r, peak > 0.1f, "the drum track plays through its kit (peak " + juce::String (juce::Decibels::gainToDecibels (peak), 1) + " dBFS)");

        const auto n = section->getSequence().getNumNotes();
        um.beginNewTransaction ("convert");
        DrumTrack::convertToMidi (*section);
        check (r, ! DrumTrack::isSection (*section) && section->getSequence().getNumNotes() == n, "Convert to MIDI keeps the notes as an ordinary clip");

        // Kit designer: samples sorted onto drums by their names
        {
            juce::StringArray names { "Kick 01", "BD_Punchy", "Snare Tight", "SD-2", "HH Closed", "Hat Open 3", "OH_long", "Tom Floor",
                                      "Hi Tom", "Tom Mid", "Crash 18in", "Ride Bell", "Clap stack", "Rimshot", "Shaker 16th", "Cowbell",
                                      "808 Sub", "Vox chop" };
            std::vector<int> expected { 0, 0, 1, 1, 3, 4, 4, 5, 7, 6, 10, 11, 2, 8, 12, 9, 14, -1 };
            juce::StringArray wrong;
            for (int i = 0; i < names.size(); ++i)
                if (DrumsDevice::padForFileName (names[i]) != expected[(size_t) i])
                    wrong.add (names[i] + " -> " + juce::String (DrumsDevice::padForFileName (names[i])));
            check (r, wrong.isEmpty(), "sample names map to the right drums" + (wrong.isEmpty() ? juce::String() : " (" + wrong.joinIntoString (", ") + ")"));

            // A folder dropped on the kit
            auto folder = outDir.getChildFile ("test-kit");
            folder.deleteRecursively();
            folder.createDirectory();
            for (auto sampleName : { "Kick Deep", "Snare Crack", "HH Closed", "Crash", "notes" })
            {
                juce::AudioBuffer<float> b (1, 4800);
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, std::sin ((float) i * 0.05f) * std::exp ((float) -i / 800.0f));
                juce::WavAudioFormat wav;
                std::unique_ptr<juce::OutputStream> os = folder.getChildFile (juce::String (sampleName) + ".wav").createOutputStream();
                if (auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (1).withBitsPerSample (16)))
                    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            }

            auto& drumsDev = *dynamic_cast<DrumsDevice*> (kit2.get());
            auto files = folder.findChildFiles (juce::File::findFiles, false, "*.wav");
            files.sort();
            auto changed = drumsDev.assignByName (files);
            check (r, changed == 4 && drumsDev.getPadDisplayName (0) == "Kick Deep" && drumsDev.getPadDisplayName (1) == "Snare Crack"
                      && drumsDev.getPadDisplayName (3) == "HH Closed" && drumsDev.getPadDisplayName (10) == "Crash" && ! drumsDev.padHasUserSample (4),
                   "a folder of samples replaces the matching drums (" + juce::String (changed) + " replaced)");

            snapshot (drumsDev, outDir.getChildFile ("drum-kit.png"));
        }

        // The editor panel
        {
            um.undo();
            DrumSectionEditor ed (*section);
            ed.setSize (1000, 230);
            juce::Image img (juce::Image::ARGB, ed.getWidth(), ed.getHeight(), true);
            { juce::Graphics g (img); ed.paintEntireComponent (g, true); }
            auto f = outDir.getChildFile ("drum-section.png");
            f.deleteFile();
            if (auto os = f.createOutputStream()) juce::PNGImageFormat().writeImageToStream (img, *os);
        }
    }

    //==============================================================================
    r.log << "Sends\n";
    {
        auto ret = Mixing::createReturnTrack (*edit);
        auto t = te::getAudioTracks (*edit)[0];
        Mixing::ensureSends (*t);
        auto sends = Mixing::getSends (*t);
        check (r, ret != nullptr && ! sends.isEmpty(), "each track gets a send to the new return");

        if (! sends.isEmpty())
        {
            auto* s = sends[0];
            auto order = [&]
            {
                juce::StringArray names;
                for (auto p : t->pluginList)
                    names.add (dynamic_cast<te::AuxSendPlugin*> (p) ? "send" : dynamic_cast<te::VolumeAndPanPlugin*> (p) ? "fader"
                               : dynamic_cast<te::LevelMeterPlugin*> (p) ? "meter" : "other");
                return names.joinIntoString (" > ");
            };
            check (r, ! Mixing::isPreFader (*t, *s), "sends start post-fader (" + order() + ")");
            Mixing::setPreFader (*t, *s, true);
            check (r, Mixing::isPreFader (*t, *s) && order().contains ("send > fader"), "switched to pre-fader: before the fader (" + order() + ")");
            Mixing::setPreFader (*t, *s, false);
            check (r, ! Mixing::isPreFader (*t, *s) && order().contains ("fader > send > meter"), "back to post-fader: after the fader, before the meter (" + order() + ")");
        }
    }

    //==============================================================================
    r.log << "Solo\n";
    {
        auto soloEdit = te::createEmptyEdit (engine, outDir.getChildFile ("solo-test.tracktionedit"));
        soloEdit->ensureNumberOfAudioTracks (3);
        auto t = te::getAudioTracks (*soloEdit);
        auto soloed = [&] { juce::String s; for (auto tr : t) s << (tr->isSolo (false) ? "S" : "-"); return s; };

        Mixing::clickSolo (*t[0]);
        Mixing::clickSolo (*t[1]);
        check (r, soloed() == "SS-", "a second solo adds to the first (" + soloed() + ")");

        juce::ModifierKeys::currentModifiers = juce::ModifierKeys (juce::ModifierKeys::commandModifier);
        Mixing::clickSolo (*t[2]);
        check (r, soloed() == "--S", "cmd-click solos just that track (" + soloed() + ")");
        Mixing::clickSolo (*t[2]);
        check (r, soloed() == "---", "cmd-click on the only solo un-solos it (" + soloed() + ")");
        juce::ModifierKeys::currentModifiers = {};

        Mixing::clickSolo (*t[0]);
        Mixing::clickSolo (*t[0]);
        check (r, soloed() == "---", "a second click un-solos (" + soloed() + ")");
    }

    //==============================================================================
    r.log << "Normalize\n";
    {
        auto normEdit = te::createEmptyEdit (engine, outDir.getChildFile ("normalise-test.tracktionedit"));
        normEdit->filePathResolver = [outDir] (const juce::String& path) { return outDir.getChildFile (path.fromLastOccurrenceOf ("/", false, false)); };
        normEdit->ensureNumberOfAudioTracks (3);
        auto t = te::getAudioTracks (*normEdit);

        // Sines peaking at -12 and -6 dBFS, a file that's quiet (-20) for a second then loud (-6),
        // and BS.1770's calibration tone: 997 Hz on both channels
        auto quietFile = outDir.getChildFile ("normalise-quiet.wav"), loudFile = outDir.getChildFile ("normalise-loud.wav");
        auto halvesFile = outDir.getChildFile ("normalise-halves.wav"), toneFile = outDir.getChildFile ("normalise-tone.wav");
        writeWav (quietFile, sine (220.0, 0.25f, 1.0));
        writeWav (loudFile, sine (220.0, 0.5f, 1.0));
        auto halves = sine (220.0, 0.1f, 2.0);
        halves.applyGain ((int) sr, (int) sr, 5.0f);
        writeWav (halvesFile, halves);
        writeWav (toneFile, sine (997.0, 0.25f, 3.0));

        auto second = te::TimeDuration::fromSeconds (1.0);
        auto quiet = t[0]->insertWaveClip ("Quiet", quietFile, { { 0_tp, second } }, false);
        auto loud = t[0]->insertWaveClip ("Loud", loudFile, { { 2_tp, second } }, false);
        auto dB = [] (te::AudioClipBase::Ptr c) { return juce::String (c->getGainDB(), 2); };

        auto whole = [] (std::initializer_list<te::AudioClipBase*> clips)
        {
            std::vector<Normalise::Part> parts;
            for (auto c : clips)
                parts.push_back (Normalise::wholeClip (*c));
            return parts;
        };
        auto run = [] (const std::vector<Normalise::Part>& parts, const Normalise::Settings& s)
        {
            std::vector<Normalise::Measurement> ms;
            for (auto& p : parts)
                ms.push_back (Normalise::measure (p));
            Normalise::applyToClips (parts, Normalise::plan (parts, ms, s));
        };

        Normalise::Settings peak;
        peak.peakTarget = 0.0f;

        run (whole ({ quiet.get(), loud.get() }), peak);
        check (r, std::abs (quiet->getGainDB() - 12.04f) < 0.05f && std::abs (loud->getGainDB() - 6.02f) < 0.05f,
               "peak, each clip to 0 dBFS: " + dB (quiet) + " and " + dB (loud) + " dB (expect 12.04, 6.02)");

        normEdit->getUndoManager().undo();
        check (r, quiet->getGainDB() == 0.0f && loud->getGainDB() == 0.0f, "undo puts the gains back (" + dB (quiet) + ", " + dB (loud) + ")");

        auto together = peak;
        together.process = Normalise::Process::together;
        run (whole ({ quiet.get(), loud.get() }), together);
        check (r, std::abs (quiet->getGainDB() - 6.02f) < 0.05f && std::abs (loud->getGainDB() - 6.02f) < 0.05f,
               "all equally, the loudest to 0 dBFS: " + dB (quiet) + " and " + dB (loud) + " dB (expect 6.02 each)");

        // Only the part of the file a clip plays counts
        auto firstHalf = t[1]->insertWaveClip ("First", halvesFile, { { 0_tp, te::TimeDuration::fromSeconds (0.9) } }, false);
        auto secondHalf = t[1]->insertWaveClip ("Second", halvesFile, { { 2_tp, te::TimeDuration::fromSeconds (0.9) }, te::TimeDuration::fromSeconds (1.05) }, false);
        run (whole ({ firstHalf.get(), secondHalf.get() }), peak);
        check (r, std::abs (firstHalf->getGainDB() - 20.0f) < 0.05f && std::abs (secondHalf->getGainDB() - 6.02f) < 0.05f,
               "a trimmed clip measures only what it plays: " + dB (firstHalf) + " and " + dB (secondHalf) + " dB (expect 20, 6.02)");

        auto tone = t[2]->insertWaveClip ("Tone", toneFile, { { 0_tp, te::TimeDuration::fromSeconds (3.0) } }, false);
        auto lufs = Normalise::loudnessOf (Normalise::measure (Normalise::wholeClip (*tone)).blocks);
        check (r, std::abs (lufs + 12.04) < 0.1, "a -12 dBFS 997 Hz tone on both channels measures " + juce::String (lufs, 2) + " LUFS (expect -12.04)");

        Normalise::Settings loudness;
        loudness.method = Normalise::Method::loudness;
        loudness.loudnessTarget = -16.0f;
        run (whole ({ tone.get() }), loudness);
        check (r, std::abs (tone->getGainDB() + 3.96f) < 0.1f, "loudness to -16 LUFS: " + dB (tone) + " dB (expect -3.96)");

        // A picture of the dialog, for two clips
        std::unique_ptr<juce::AlertWindow> dialog (Theme::newAlertWindow ("Normalize", "2 clips"));
        Normalise::DialogContent content (whole ({ quiet.get(), loud.get() }), Normalise::Settings());
        dialog->addCustomComponent (&content);
        dialog->addButton ("Apply", 1);
        dialog->addButton ("Cancel", 0);
        picture (*dialog, outDir.getChildFile ("normalize-dialog.png"));
    }

    //==============================================================================
    r.log << "Editing a selection of audio\n";
    {
        auto selEdit = te::createEmptyEdit (engine, outDir.getChildFile ("selection-test.tracktionedit"));
        selEdit->filePathResolver = [outDir] (const juce::String& path) { return outDir.getChildFile (path.fromLastOccurrenceOf ("/", false, false)); };
        selEdit->ensureNumberOfAudioTracks (1);
        auto track = te::getAudioTracks (*selEdit)[0];
        auto takeFile = outDir.getChildFile ("selection-take.wav");
        writeWav (takeFile, sine (220.0, 0.25f, 4.0));

        auto layout = [&]
        {
            auto clips = track->getClips();
            std::sort (clips.begin(), clips.end(), [] (auto a, auto b) { return a->getPosition().getStart() < b->getPosition().getStart(); });
            juce::StringArray s;
            for (auto c : clips)
                s.add (juce::String (c->getPosition().getStart().inSeconds(), 1) + "-" + juce::String (c->getPosition().getEnd().inSeconds(), 1)
                       + (dynamic_cast<te::AudioClipBase*> (c)->getIsReversed() ? "R" : "")
                       + (std::abs (dynamic_cast<te::AudioClipBase*> (c)->getGainDB()) > 0.01f ? "g" : ""));
            return s.joinIntoString (" ");
        };
        auto fresh = [&]
        {
            auto clips = track->getClips();   // a copy: removing changes the track's list
            for (auto c : clips)
                c->removeFromParent();
            track->insertWaveClip ("Take", takeFile, { { 0_tp, te::TimeDuration::fromSeconds (4.0) } }, false);
        };
        te::TimeRange middle (1s, te::TimePosition (2s));

        fresh();
        auto pieces = AudioEdits::isolate (*track, middle);
        check (r, pieces.size() == 1 && layout() == "0.0-1.0 1.0-2.0 2.0-4.0", "splitting off a selection (" + layout() + ")");

        std::vector<Normalise::Part> parts { Normalise::wholeClip (*pieces[0]) };
        Normalise::Settings peak;
        Normalise::applyToClips (parts, Normalise::plan (parts, { Normalise::measure (parts[0]) }, peak));
        check (r, layout() == "0.0-1.0 1.0-2.0g 2.0-4.0", "normalizing the selection changes only that piece (" + layout() + ")");

        fresh();
        AudioEdits::reverse (*track, middle);
        check (r, layout() == "0.0-1.0 1.0-2.0R 2.0-4.0", "reversing the selection reverses only that piece (" + layout() + ")");

        fresh();
        AudioEdits::remove (*track, middle, false);
        check (r, layout() == "0.0-1.0 2.0-4.0", "deleting the selection leaves a gap (" + layout() + ")");

        fresh();
        AudioEdits::remove (*track, middle, true);
        check (r, layout() == "0.0-1.0 1.0-3.0", "in Shuffle mode the rest closes up (" + layout() + ")");

        fresh();
        AudioEdits::trim (*track, { 0s, te::TimePosition (4s) }, { te::TimePosition (0.5s), te::TimePosition (3s) });
        check (r, layout() == "0.5-3.0", "trimming to the selection keeps just that (" + layout() + ")");

        // A picture of the clip editor, with part of a fresh take selected
        fresh();
        te::SelectionManager selection (engine);
        EditViewState vs (*selEdit, selection);
        AudioSelectionEditor editor (vs, *dynamic_cast<te::AudioClipBase*> (track->getClips()[0]));
        editor.setSize (1000, 180);
        editor.setSelection (te::TimeRange (te::TimePosition (1.2s), te::TimePosition (2.6s)));
        editor.createComponentSnapshot (editor.getLocalBounds());   // starts its waveform loading
        for (int i = 0; i < 40; ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        picture (editor, outDir.getChildFile ("clip-editor.png"));
    }

    //==============================================================================
    r.log << "Comments\n";
    {
        auto noteEdit = te::createEmptyEdit (engine, outDir.getChildFile ("comments-test.tracktionedit"));
        noteEdit->ensureNumberOfAudioTracks (2);
        noteEdit->tempoSequence.getTempo (0)->setBpm (120.0);   // a bar is 2 s
        auto t = te::getAudioTracks (*noteEdit);
        t[0]->setName ("Drums");
        t[1]->setName ("Vox");
        Collab::Identity who { "test", "Nicholas", juce::Colour (0xffd9875a) };

        Comments::add (*noteEdit, who, t[1]->itemID, { 22s, te::TimePosition (28s) }, "Hearing a bit of popping in the vocals at times");
        Comments::add (*noteEdit, who, t[0]->itemID, { 6s, te::TimePosition (6s) }, "Kick is too loud here");
        auto notes = Comments::inSongOrder (*noteEdit, false);

        check (r, notes.size() == 2 && notes[0].getText().startsWith ("Kick") && notes[1].getText().startsWith ("Hearing"),
               "the book reads in song order");
        check (r, notes.size() == 2 && Comments::describeRange (*noteEdit, notes[1].getRange()) == juce::String::fromUTF8 ("bars 12\xe2\x80\x93" "14")
                  && Comments::describeRange (*noteEdit, notes[0].getRange()) == "bar 4",
               "ranges read as bars (" + (notes.size() == 2 ? Comments::describeRange (*noteEdit, notes[1].getRange()) + ", " + Comments::describeRange (*noteEdit, notes[0].getRange()) : juce::String()) + ")");

        notes[0].state.setProperty ("resolved", true, nullptr);
        check (r, Comments::inSongOrder (*noteEdit, false).size() == 1 && Comments::inSongOrder (*noteEdit, true).size() == 2,
               "resolved notes leave the book unless shown");
        notes[0].state.setProperty ("resolved", false, nullptr);

        juce::ValueTree old (Comments::commentType);
        old.setProperty ("position", 10.0, nullptr);
        check (r, Comments::Comment { old }.getEnd() == 10.0, "an older comment without an end is a point");

        // A picture of the book, open at the second note
        te::SelectionManager selection (engine);
        EditViewState vs (*noteEdit, selection);
        vs.openCommentId = notes[1].getId();
        Comments::Book book (vs, who);
        book.setSize (1100, 240);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        picture (book, outDir.getChildFile ("notes-book.png"));
    }

    //==============================================================================
    r.log << "Rounded menus and dialogs\n";
    {
        // A window is see-through when JUCE gives it the semi-transparent style; the corners of its
        // snapshot show whether it draws a rounded panel
        auto look = [&] (juce::Component& c, const juce::String& name)
        {
            auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
            auto f = outDir.getChildFile (name);
            f.deleteFile();
            if (auto os = f.createOutputStream())
                juce::PNGImageFormat().writeImageToStream (img, *os);

            auto seeThrough = ! c.isOpaque() && c.getPeer() != nullptr
                              && (c.getPeer()->getStyleFlags() & juce::ComponentPeer::windowIsSemiTransparent) != 0;
            auto roundCorner = img.getPixelAt (1, 1).getAlpha() == 0 && img.getPixelAt (img.getWidth() / 2, img.getHeight() / 2).getAlpha() == 255;
            return seeThrough && roundCorner;
        };

        // A menu's window is see-through when its background colour isn't opaque (that's JUCE's
        // test); a live menu would close at once in a test run, so its painting is checked here
        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
        juce::Image menu (juce::Image::ARGB, 480, 220, true);
        {
            juce::Graphics g (menu);
            g.addTransform (juce::AffineTransform::scale (2.0f));
            lf.drawPopupMenuBackgroundWithOptions (g, 240, 110, {});
            lf.drawPopupMenuItem (g, { 6, 6, 228, 24 }, false, true, false, false, false, "Split at Playhead", {}, nullptr, nullptr);
            lf.drawPopupMenuItem (g, { 6, 30, 228, 24 }, false, true, true, false, false, "Normalize", {}, nullptr, nullptr);
            lf.drawPopupMenuItem (g, { 6, 54, 228, 24 }, false, true, false, false, true, "Track Colour", {}, nullptr, nullptr);
            lf.drawPopupMenuItem (g, { 6, 78, 228, 24 }, false, true, false, false, false, "Delete", {}, nullptr, nullptr);
        }
        if (auto os = std::unique_ptr<juce::OutputStream> (outDir.getChildFile ("menu.png").createOutputStream()))
            juce::PNGImageFormat().writeImageToStream (menu, *os);

        check (r, ! lf.findColour (juce::PopupMenu::backgroundColourId).isOpaque()
                  && menu.getPixelAt (1, 1).getAlpha() == 0 && menu.getPixelAt (240, 200).getAlpha() == 255,
               "a menu's window is see-through, with round corners");

        std::unique_ptr<juce::AlertWindow> dialog (Theme::newAlertWindow ("Rename Track", {}));
        dialog->addTextEditor ("name", "left guitar");
        dialog->addButton ("OK", 1);
        dialog->addButton ("Cancel", 0);
        check (r, look (*dialog, "dialog.png"), "a dialog is see-through, with round corners");

        std::unique_ptr<juce::AlertWindow> alert (juce::LookAndFeel::getDefaultLookAndFeel().createAlertWindow (
            "Bounce failed", "Couldn't render left guitar.", "OK", {}, {}, juce::MessageBoxIconType::WarningIcon, 1, nullptr));
        check (r, look (*alert, "alert.png"), "a message box is see-through, with round corners");
    }

    //==============================================================================
    r.log << "Loop handles in the ruler\n";
    {
        auto loopEdit = te::createEmptyEdit (engine, outDir.getChildFile ("loop-test.tracktionedit"));
        te::SelectionManager selection (engine);
        EditViewState vs (*loopEdit, selection);
        vs.pixelsPerBeat = 20.0;
        TimelineRuler ruler (vs);
        ruler.setSize (800, 26);

        auto& transport = loopEdit->getTransport();
        transport.setLoopRange ({ vs.beatToTime (8.0), vs.beatToTime (16.0) });
        transport.looping = true;

        auto source = juce::Desktop::getInstance().getMainMouseSource();
        auto event = [&] (float x, float y, float downX, bool dragged)
        {
            auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent (source, { x, y }, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &ruler, &ruler, now, { downX, y }, now, 1, dragged);
        };
        auto drag = [&] (float fromX, float y, float toX)
        {
            ruler.mouseDown (event (fromX, y, fromX, false));
            ruler.mouseDrag (event (toX, y, fromX, true));
            ruler.mouseUp (event (toX, y, fromX, true));
        };
        auto loopBeats = [&]
        {
            auto l = transport.getLoopRange();
            return juce::String (vs.timeToBeat (l.getStart()), 2) + " to " + juce::String (vs.timeToBeat (l.getEnd()), 2);
        };

        // Each end's dot sits half the capsule's height inside that end; grabbed in the top half,
        // where a click elsewhere moves the playhead
        auto startDot = vs.beatToX (8.0) + 11.0f, endDot = vs.beatToX (16.0) - 11.0f;
        drag (endDot, 5.0f, endDot + 4 * 20.0f);
        check (r, loopBeats() == "8.00 to 20.00", "dragging the end dot moves the end (" + loopBeats() + ", expect 8 to 20)");
        drag (startDot, 5.0f, startDot - 4 * 20.0f);
        check (r, loopBeats() == "4.00 to 20.00" && transport.getPosition() == te::TimePosition(),
               "dragging the start dot moves the start, not the playhead (" + loopBeats() + ", expect 4 to 20)");

        auto bar3 = vs.beatToX (10.0);
        ruler.mouseDown (event (bar3, 5.0f, bar3, false));
        ruler.mouseUp (event (bar3, 5.0f, bar3, false));
        check (r, std::abs (vs.timeToBeat (transport.getPosition()) - 10.0) < 1.0e-6 && loopBeats() == "4.00 to 20.00",
               "a click between the dots still moves the playhead (beat " + juce::String (vs.timeToBeat (transport.getPosition()), 2) + ")");

        drag (vs.beatToX (12.0), 20.0f, vs.beatToX (14.0));
        check (r, loopBeats() == "6.00 to 22.00", "dragging the loop's lower half moves the whole loop (" + loopBeats() + ")");
    }

    //==============================================================================
    r.log << "Time-stretch: warp, transpose and Pitch Shift\n";
    {
        // A 4-beat loop at 100 BPM: 440 Hz for the first 2 beats, then silence
        const double loopBpm = 100.0, beat = 60.0 / loopBpm;
        auto loop = sine (440.0, 0.5f, 4 * beat);
        const int soundLength = (int) (2 * beat * sr);
        loop.clear (soundLength, loop.getNumSamples() - soundLength);
        auto loopFile = outDir.getChildFile ("stretch-loop.wav");
        writeWav (loopFile, loop);

        auto stretchEdit = te::createEmptyEdit (engine, outDir.getChildFile ("stretch-test.tracktionedit"));
        stretchEdit->filePathResolver = [loopFile] (const juce::String&) { return loopFile; };   // the only file it uses
        stretchEdit->tempoSequence.getTempo (0)->setBpm (120.0);
        stretchEdit->ensureNumberOfAudioTracks (1);
        auto track = te::getAudioTracks (*stretchEdit).getFirst();
        auto clip = track->insertWaveClip ("Loop", loopFile, { { 0_tp, te::TimeDuration::fromSeconds (4 * beat) } }, false);
        clip->getLoopInfo().setBpm (loopBpm, te::AudioFile (engine, loopFile).getInfo());

        // Stretch in real time first (the proxy file is checked below)
        clip->setUsesProxy (false);

        // Renders the track; returns when the sound stops (s) and its pitch (Hz) near the start
        auto render = [&] (const juce::String& name, double& soundEnd, double& pitch)
        {
            auto out = outDir.getChildFile (name + ".wav");
            out.deleteFile();
            juce::BigInteger tracks;
            tracks.setBit (track->getIndexInEditTrackList());
            te::Renderer::renderToFile ("Test", out, *stretchEdit, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (2.5)), tracks, true, false, {}, true);

            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (out));
            soundEnd = pitch = 0.0;

            if (reader == nullptr)
                return;

            juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
            reader->read (&b, 0, b.getNumSamples(), 0, true, true);

            for (int i = b.getNumSamples(); --i >= 0;)
                if (std::abs (b.getSample (0, i)) > 0.05f) { soundEnd = i / reader->sampleRate; break; }

            pitch = yinPitch (b, (int) (0.3 * reader->sampleRate)) * reader->sampleRate / sr;
        };

        double end = 0, hz = 0;
        render ("stretch-off", end, hz);
        check (r, std::abs (end - 1.2) < 0.05 && std::abs (hz - 440.0) < 3.0,
               "unwarped: plays as recorded (sound ends at " + juce::String (end, 3) + " s, " + juce::String (hz, 1) + " Hz)");

        clip->setAutoTempo (true);
        clip->setLength (te::TimeDuration::fromSeconds (2.0), true);
        r.log << "        stretcher: " << te::TimeStretcher::getNameOfMode (te::TimeStretcher::defaultMode) << "\n";
        render ("stretch-warp", end, hz);
        check (r, std::abs (end - 1.0) < 0.08 && std::abs (hz - 440.0) < 3.0,
               "warp 100 -> 120 BPM: 2 beats now end at " + juce::String (end, 3) + " s (expect 1.0), pitch kept (" + juce::String (hz, 1) + " Hz)");

        clip->setPitchChange (12.0f);
        render ("stretch-transpose", end, hz);
        check (r, std::abs (hz - 880.0) < 6.0 && std::abs (end - 1.0) < 0.08,
               "transpose +12: " + juce::String (hz, 1) + " Hz (expect 880), timing kept (" + juce::String (end, 3) + " s)");

        // As the app plays it: from a stretched proxy file, rendered in the background
        clip->setPitchChange (0.0f);
        clip->setUsesProxy (true);
        clip->beginRenderingNewProxyIfNeeded();
        for (int i = 0; i < 200 && ! clip->getPlaybackFile().isValid(); ++i)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        check (r, clip->usesTimeStretchedProxy() && clip->getPlaybackFile().isValid(), "warped clip's proxy file rendered");
        render ("stretch-proxy", end, hz);
        check (r, std::abs (end - 1.0) < 0.08 && std::abs (hz - 440.0) < 3.0,
               "plays from the proxy: ends at " + juce::String (end, 3) + " s, " + juce::String (hz, 1) + " Hz");
        clip->setUsesProxy (false);

        clip->setAutoTempo (false);
        clip->setLength (te::TimeDuration::fromSeconds (4 * beat), true);
        auto shifter = stretchEdit->getPluginCache().createNewPlugin (te::PitchShiftPlugin::xmlTypeName, {});
        track->pluginList.insertPlugin (shifter, 0, nullptr);
        dynamic_cast<te::PitchShiftPlugin*> (shifter.get())->semitones->setParameter (7.0f, juce::sendNotificationSync);
        render ("stretch-pitchshift", end, hz);
        check (r, std::abs (hz - 659.3) < 5.0, "Pitch Shift +7: " + juce::String (hz, 1) + " Hz (expect 659)");
        shifter->deleteFromParent();
    }

    r.log << (r.failures == 0 ? "\nALL DEVICE TESTS PASSED\n" : "\n" + juce::String (r.failures) + " FAILURE(S)\n");
    std::cout << r.log << std::flush;
    outDir.getChildFile ("results.txt").replaceWithText (r.log);
    keepAlive.clear();
    return r.failures;
}

} // namespace elsewhere::DeviceTests
