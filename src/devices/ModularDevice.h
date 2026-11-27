// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "ModularEngine.h"
#include <map>

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Modular: a rack of modules joined by patch cables.

    The patch lives in the plugin's state (a PATCH tree of MODULE and CABLE children), so
    it's saved with the project, undoable, and synced to collaborators like everything else.
    Module knobs are properties of their MODULE; four Macro parameters bring automation,
    modulation and MIDI mapping into the patch through the Macros module.

    Signals are plain numbers: audio is about +-1; pitch is volts per octave with 0 V = C4;
    gates are 0 or 1; envelopes run 0..1; LFOs +-1.

    The whole patch runs once per voice (1-8 voices), so patches are polyphonic. The sound is
    ModularEngine: the device keeps the patch, builds it into the engine and plays through it.
*/
namespace Modular
{
    // The modules themselves (specs, processing, factory patches) are in ModularDsp.h, in plain
    // C++; these are the parts the device and its rack need from JUCE.
    inline const Spec* findSpec (const juce::String& type)     { return findSpec (type.toRawUTF8()); }
    inline int portIndex (const std::vector<Port>& ports, const juce::String& id)  { return portIndex (ports, id.toRawUTF8()); }

    inline juce::String paramProperty (const ParamSpec& p)     { return juce::String ("p_") + p.id; }

    inline juce::StringArray scaleNames()   { return { "Chromatic", "Major", "Minor", "Major Pent", "Minor Pent", "Dorian", "Whole Tone" }; }

    inline juce::String formatValue (const ParamSpec& p, float v)
    {
        const juce::String unit (p.unit);
        if (juce::String (p.id) == "exp")   return v > 0.5f ? "Exponential" : "Linear";
        if (juce::String (p.id) == "scale") return scaleNames()[juce::jlimit (0, scaleNames().size() - 1, juce::roundToInt (v))];
        if (juce::String (p.id) == "root")  return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, false, 4);
        if (unit == "Hz")   return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, v < 10.0f ? 2 : 0) + " Hz";
        if (unit == "s")    return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
        if (unit == "ms")   return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (juce::roundToInt (v)) + " ms";
        if (unit == "st" || unit == "oct") return (v > 0.0f ? "+" : "") + juce::String (p.kind == ParamSpec::stepped ? juce::String (juce::roundToInt (v)) : juce::String (v, 2)) + " " + unit;
        if (unit == "ct")   return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct";
        if (unit == "V")    return (v > 0.0f ? "+" : "") + juce::String (v, 2) + " V";
        if (unit == "%")    return juce::String (juce::roundToInt (v)) + " %";
        return juce::String (v, 2);
    }

    inline juce::NormalisableRange<double> rangeFor (const ParamSpec& p)
    {
        juce::NormalisableRange<double> r (p.min, p.max, p.kind == ParamSpec::stepped ? 1.0 : 0.0);
        if (p.kind == ParamSpec::log)
            r.setSkewForCentre (p.min > 0.0f ? std::sqrt ((double) p.min * p.max) : p.max * 0.1);
        return r;
    }

    inline const juce::Identifier patchId { "PATCH" }, moduleId { "MODULE" }, cableId { "CABLE" };
}

//==============================================================================
class ModularDevice : public ElsewhereDevice,
                      private juce::Timer
{
public:
    static constexpr const char* xmlTypeName = "elsewhereModular";

    ModularDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        voices = addParameter ("voices", "Voices", 1.0f, stepped (1, Modular::maxVoices), [] (float v) { return juce::String (juce::roundToInt (v)); });
        volume = addParameter ("volume", "Volume", -6.0f, { -60.0f, 6.0f }, dbText);
        for (int i = 0; i < 4; ++i)
            macros[(size_t) i] = addParameter ("macro" + juce::String (i + 1), "Macro " + juce::String (i + 1), 0.5f, { 0.0f, 1.0f }, percentFromUnit);

        if (! state.getChildWithName (Modular::patchId).isValid() || state.getChildWithName (Modular::patchId).getNumChildren() == 0)
            buildPatch (Modular::factoryPatches()[0], nullptr);

        state.addListener (&watcher);
        rebuild();
        startTimer (500);
    }

    ~ModularDevice() override
    {
        stopTimer();
        state.removeListener (&watcher);
    }

    juce::String getName() const override              { return "Elsewhere Modular"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Modular"; }
    int getDeviceViewWidth() const override            { return 1400; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1200, 720 }; }

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 4.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }

    juce::ValueTree getPatch()                          { return state.getOrCreateChildWithName (Modular::patchId, nullptr); }
    juce::UndoManager* getUndo()                        { return &edit.getUndoManager(); }

    /** Presets copy the patch too, not just the parameters. */
    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        ElsewhereDevice::restorePluginStateFromValueTree (v);
        auto incoming = v.getChildWithName (Modular::patchId);
        if (incoming.isValid())
            getPatch().copyPropertiesAndChildrenFrom (incoming, getUndo());
    }

    //==============================================================================
    // Editing the patch (message thread; all undoable)
    int nextModuleId()
    {
        auto patch = getPatch();
        int id = 1;
        for (auto m : patch)
            if (m.hasType (Modular::moduleId))
                id = juce::jmax (id, (int) m["id"] + 1);
        return id;
    }

    juce::ValueTree addModule (const juce::String& type, juce::UndoManager* um)
    {
        auto* spec = Modular::findSpec (type);
        if (spec == nullptr) return {};

        // New modules go at the end of the rack, just before Output
        auto list = modulesInOrder();
        int insertAt = (int) list.size();
        if (! list.empty() && list.back()["type"].toString() == "output")
            insertAt = (int) list.size() - 1;

        juce::ValueTree m (Modular::moduleId);
        m.setProperty ("id", nextModuleId(), nullptr);
        m.setProperty ("type", type, nullptr);
        m.setProperty ("pos", (int) list.size(), nullptr);
        for (auto& p : spec->params)
            m.setProperty (Modular::paramProperty (p), p.def, nullptr);
        getPatch().appendChild (m, um);
        moveModule ((int) m["id"], insertAt, um);
        return m;
    }

    void removeModule (int id, juce::UndoManager* um)
    {
        auto patch = getPatch();
        for (int i = patch.getNumChildren(); --i >= 0;)
        {
            auto c = patch.getChild (i);
            if ((c.hasType (Modular::moduleId) && (int) c["id"] == id)
                || (c.hasType (Modular::cableId) && ((int) c["from"] == id || (int) c["to"] == id)))
                patch.removeChild (i, um);
        }
        normalisePositions (um);
    }

    void connect (int fromId, const juce::String& out, int toId, const juce::String& in, juce::UndoManager* um)
    {
        disconnectInput (toId, in, um);
        juce::ValueTree c (Modular::cableId);
        c.setProperty ("from", fromId, nullptr);
        c.setProperty ("out", out, nullptr);
        c.setProperty ("to", toId, nullptr);
        c.setProperty ("in", in, nullptr);
        getPatch().appendChild (c, um);
    }

    void disconnectInput (int toId, const juce::String& in, juce::UndoManager* um)
    {
        auto patch = getPatch();
        for (int i = patch.getNumChildren(); --i >= 0;)
        {
            auto c = patch.getChild (i);
            if (c.hasType (Modular::cableId) && (int) c["to"] == toId && c["in"].toString() == in)
                patch.removeChild (i, um);
        }
    }

    /** Modules in rack order. */
    std::vector<juce::ValueTree> modulesInOrder()
    {
        std::vector<juce::ValueTree> list;
        for (auto m : getPatch())
            if (m.hasType (Modular::moduleId))
                list.push_back (m);
        std::stable_sort (list.begin(), list.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) { return (int) a["pos"] < (int) b["pos"]; });
        return list;
    }

    void normalisePositions (juce::UndoManager* um)
    {
        int i = 0;
        for (auto& m : modulesInOrder())
            if ((int) m["pos"] != i)
                m.setProperty ("pos", i++, um);
            else
                ++i;
    }

    /** Moves a module to a new place in the rack. */
    void moveModule (int id, int newIndex, juce::UndoManager* um)
    {
        auto list = modulesInOrder();
        auto it = std::find_if (list.begin(), list.end(), [id] (const juce::ValueTree& m) { return (int) m["id"] == id; });
        if (it == list.end()) return;
        auto m = *it;
        list.erase (it);
        newIndex = juce::jlimit (0, (int) list.size(), newIndex);
        list.insert (list.begin() + newIndex, m);
        for (int i = 0; i < (int) list.size(); ++i)
            if ((int) list[(size_t) i]["pos"] != i)
                list[(size_t) i].setProperty ("pos", i, um);
    }

    //==============================================================================
    static const std::vector<Modular::FactoryPatch>& patches()     { return Modular::factoryPatches(); }

    /** Builds a factory patch in the rack: as its own undo step, or as part of the one in progress. */
    void loadPatch (int index, bool ownUndoStep = true)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        if (ownUndoStep)
            edit.getUndoManager().beginNewTransaction ("Load Modular patch");
        buildPatch (patches()[(size_t) index], getUndo());
        voices.param().setParameter ((float) patches()[(size_t) index].voices, juce::sendNotification);
        state.setProperty ("patch", index, getUndo());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = (float) info.sampleRate;
        // The patch runs at twice the sample rate, so FM, sync, wavefolding and the filter's
        // saturation stay clean of aliasing
        sound.prepare (info.sampleRate, info.blockSizeSamples);
        scratch.setSize (2, ModularEngine::maxChunk);
    }

    void midiPanic() override       { sound.allNotesOff(); }

    double getLatencySeconds() override     { return sound.isPrepared() ? sound.latencySamples() / rate : 0.0; }

    /** Listens to one output jack (voice 1) for the hover scope; -1 to stop. */
    void setProbe (int moduleId, int port)  { sound.setProbe (moduleId, port); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr || ! sound.isPrepared())
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        buf.clear (fc.bufferStartSample, fc.bufferNumSamples);
        updateEngine();

        int done = 0;
        if (fc.bufferForMidiMessages != nullptr)
        {
            if (fc.bufferForMidiMessages->isAllNotesOff)
                midiPanic();

            for (auto& m : *fc.bufferForMidiMessages)
            {
                auto pos = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * rate));
                if (pos > done) { render (buf, fc.bufferStartSample + done, pos - done); done = pos; }
                handleMidi (m);
            }
        }

        if (done < fc.bufferNumSamples)
            render (buf, fc.bufferStartSample + done, fc.bufferNumSamples - done);
    }

    /** The sound itself: the device builds its patch into it. */
    ModularEngine sound;

    // For the screen
    SynthCore::ScopeBuffer& scope = sound.scope;
    SynthCore::ScopeBuffer& probeScope = sound.probeScope;
    float getSamplesPerCycle() const    { return sound.samplesPerCycle.load(); }

    /** What a macro (0..3) does in the patch loaded last, such as "Cutoff"; "" if it's unpatched there. */
    juce::String macroName (int macro) const    { return ModularEngine::macroName (getPatchIndex(), macro); }

    Param voices, volume;
    std::array<Param, 4> macros;

private:
    float rate = 48000.0f;
    juce::AudioBuffer<float> scratch;   // for a mono buffer

    static juce::String percentFromUnit (float v)  { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }

    /** A factory patch into the tree, its macros patched in: just as ModularEngine::setPatch builds it. */
    void buildPatch (const Modular::FactoryPatch& fp, juce::UndoManager* um)
    {
        auto patch = state.getOrCreateChildWithName (Modular::patchId, um);
        patch.removeAllChildren (um);

        const auto desc = Modular::describe (fp);
        for (int i = 0; i < (int) desc.modules.size(); ++i)
        {
            const auto& dm = desc.modules[(size_t) i];
            auto* spec = Modular::findSpec (dm.type);
            juce::ValueTree m (Modular::moduleId);
            m.setProperty ("id", dm.id, nullptr);
            m.setProperty ("type", juce::String (dm.type), nullptr);
            m.setProperty ("pos", i, nullptr);
            for (size_t k = 0; k < spec->params.size(); ++k)
                m.setProperty (Modular::paramProperty (spec->params[k]), dm.params[k], nullptr);
            patch.appendChild (m, um);
        }

        for (auto& c : desc.cables)
        {
            juce::ValueTree cable (Modular::cableId);
            cable.setProperty ("from", c.from, nullptr);
            cable.setProperty ("out", juce::String (c.out), nullptr);
            cable.setProperty ("to", c.to, nullptr);
            cable.setProperty ("in", juce::String (c.in), nullptr);
            patch.appendChild (cable, um);
        }
    }

    //==============================================================================
    // Patch -> program (message thread). te::Plugin is already a ValueTree listener, so the
    // patch is watched by a small helper object instead.
    struct Watcher : juce::ValueTree::Listener
    {
        ModularDevice& owner;
        explicit Watcher (ModularDevice& o) : owner (o) {}
        void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override        { owner.patchPropertyChanged (t, p); }
        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override                 { if (owner.isPatchTree (parent)) owner.rebuild(); }
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override          { if (owner.isPatchTree (parent)) owner.rebuild(); }
        void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override                  { if (owner.isPatchTree (parent)) owner.rebuild(); }
        void valueTreeRedirected (juce::ValueTree&) override                                          { owner.rebuild(); }
    };
    Watcher watcher { *this };

    void patchPropertyChanged (juce::ValueTree& t, const juce::Identifier& prop)
    {
        if (t.hasType (Modular::moduleId) && prop.toString().startsWith ("p_"))
        {
            setParamFromTree (t, prop);
            return;
        }
        if (t.hasType (Modular::moduleId) || t.hasType (Modular::cableId))
            rebuild();
    }

    bool isPatchTree (const juce::ValueTree& t) const   { return t.hasType (Modular::patchId) || t == state; }

    void setParamFromTree (const juce::ValueTree& m, const juce::Identifier& prop)
    {
        auto* d = sound.module ((int) m["id"]);
        auto* spec = Modular::findSpec (m["type"].toString());
        if (d == nullptr || spec == nullptr) return;
        for (size_t i = 0; i < spec->params.size(); ++i)
            if (prop.toString() == Modular::paramProperty (spec->params[i]))
                d->params[i].store ((float) m[prop], std::memory_order_relaxed);
    }

    /** The patch tree as plain data, built into the engine (which keeps the state of the modules still there). */
    void rebuild()
    {
        Modular::PatchDesc desc;
        for (auto& m : modulesInOrder())
        {
            Modular::PatchDesc::Module module { (int) m["id"], m["type"].toString().toStdString(), {} };
            if (auto* spec = Modular::findSpec (m["type"].toString()))
                for (auto& p : spec->params)
                    module.params.push_back ((float) m.getProperty (Modular::paramProperty (p), p.def));
            desc.modules.push_back (std::move (module));
        }
        for (auto c : getPatch())
            if (c.hasType (Modular::cableId))
                desc.cables.push_back ({ (int) c["from"], c["out"].toString().toStdString(), (int) c["to"], c["in"].toString().toStdString() });
        sound.build (desc);
    }

    /** Programs and modules that have been replaced are freed once the audio thread is done with them. */
    void timerCallback() override       { sound.releaseRetired(); }

    //==============================================================================
    /** The parameters as they are now (automation included) into the sound. */
    void updateEngine()
    {
        auto& p = sound.params;
        p.voices = voices.get();
        p.volume = volume.get();
        p.macro1 = macros[0].get(); p.macro2 = macros[1].get();
        p.macro3 = macros[2].get(); p.macro4 = macros[3].get();
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       sound.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 sound.noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              sound.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isControllerOfType (1))      sound.setModWheel ((float) m.getControllerValue() / 127.0f);
        else if (m.isAllNotesOff() || m.isAllSoundOff()) midiPanic();
    }

    void render (juce::AudioBuffer<float>& buf, int start, int num)
    {
        if (buf.getNumChannels() > 1)
            return sound.render (buf.getWritePointer (0, start), buf.getWritePointer (1, start), num);

        // A mono buffer: both sides, averaged
        float* out = buf.getWritePointer (0, start);
        for (int done = 0; done < num; )
        {
            const int n = juce::jmin (scratch.getNumSamples(), num - done);
            scratch.clear();
            sound.render (scratch.getWritePointer (0), scratch.getWritePointer (1), n);
            for (int i = 0; i < n; ++i)
                out[done + i] += (scratch.getSample (0, i) + scratch.getSample (1, i)) * 0.5f;
            done += n;
        }
    }
};

//==============================================================================
/** Flat jacks, knobs and modules for the rack. */
namespace ModularUI
{
    class Rack;

    /** A knob bound to a module property (undoable), drawn with the flat dot knob. */
    class ModKnob : public juce::Slider
    {
    public:
        std::function<void (const juce::String&, const juce::String&, float)> onTouch;

        ModKnob (juce::ValueTree module, const Modular::ParamSpec& p, juce::UndoManager* um, juce::Colour colour)
            : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox), spec (p), undo (um)
        {
            setNormalisableRange (Modular::rangeFor (p));
            setDoubleClickReturnValue (true, p.def);
            setColour (juce::Slider::rotarySliderFillColourId, colour);
            getValueObject().referTo (module.getPropertyAsValue (Modular::paramProperty (p), um));
            onDragStart = [this] { if (undo != nullptr) undo->beginNewTransaction ("Modular knob"); report(); };
            onValueChange = [this] { report(); };
        }

        void report()
        {
            if (onTouch)
                onTouch (spec.name, Modular::formatValue (spec, (float) getValue()), (float) valueToProportionOfLength (getValue()));
        }

    private:
        Modular::ParamSpec spec;
        juce::UndoManager* undo;
    };

    struct JackRef
    {
        int moduleId = -1;
        bool isInput = true;
        int index = -1;
        bool operator== (const JackRef& o) const { return moduleId == o.moduleId && isInput == o.isInput && index == o.index; }
    };

    inline juce::Colour moduleColour (const juce::String& type);

    /** Every jack and cable shows its signal: audio orange, CV violet, gates gold. */
    inline juce::Colour signalColour (Modular::Signal s)
    {
        return Theme::indicator (s == Modular::Signal::audio ? Colours::accent : s == Modular::Signal::gate ? Colours::solo : Colours::clips[5]);
    }

    /** One module: title bar, knobs, input jacks, then output jacks on a darker plate. */
    class ModulePanel : public juce::Component
    {
    public:
        static constexpr int colW = 44, knobH = 42, jackH = 30, titleH = 18;

        ModulePanel (Rack& r, juce::ValueTree m, juce::UndoManager* um);

        static int widthFor (const Modular::Spec& s)    { return s.columns * colW + 8; }

        /** The height at which all of a module shows: its knobs, its inputs and its outputs. */
        static int heightFor (const Modular::Spec& s)
        {
            auto rowsOf = [&s] (size_t count) { return ((int) count + s.columns - 1) / s.columns; };
            return titleH + 2 + rowsOf (s.params.size()) * knobH + rowsOf (s.ins.size()) * jackH + 2 + rowsOf (s.outs.size()) * jackH + 4;
        }

        int getModuleId() const         { return (int) tree["id"]; }
        const Modular::Spec& getSpec() const { return *spec; }

        /** Centre of a jack, in this panel's coordinates. */
        juce::Point<float> jackCentre (bool isInput, int index) const
        {
            auto r = jackBounds (isInput, index);
            return { r.getCentreX(), r.getY() + 9.0f };
        }

        juce::Rectangle<float> jackBounds (bool isInput, int index) const
        {
            const int cols = spec->columns;
            const int row = index / cols, col = index % cols;
            const float y = (float) (isInput ? inputsTop : outputsTop) + (float) (row * jackH);
            return { 4.0f + (float) (col * colW), y, (float) colW, (float) jackH };
        }

        std::optional<JackRef> jackAt (juce::Point<float> p) const
        {
            for (int i = 0; i < (int) spec->ins.size(); ++i)
                if (jackBounds (true, i).contains (p)) return JackRef { getModuleId(), true, i };
            for (int i = 0; i < (int) spec->outs.size(); ++i)
                if (jackBounds (false, i).contains (p)) return JackRef { getModuleId(), false, i };
            return {};
        }

        void paint (juce::Graphics& g) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseMove (const juce::MouseEvent& e) override;
        void mouseExit (const juce::MouseEvent& e) override;

    private:
        Rack& rack;
        juce::ValueTree tree;
        const Modular::Spec* spec;
        std::vector<std::unique_ptr<ModKnob>> knobs;
        int inputsTop = 0, outputsTop = 0;
        bool draggingTitle = false;
    };

    //==============================================================================
    /** The row of modules, with the cables drawn over them. */
    class Rack : public juce::Component, public Reflow::Sections, private juce::ValueTree::Listener, private juce::Timer
    {
    public:
        /** Each module is a section: a wrapped device window starts a new row between modules. */
        std::vector<juce::Rectangle<int>> getLayoutSections() const override
        {
            std::vector<juce::Rectangle<int>> out;
            for (auto& p : panels) out.push_back (p->getBounds());
            out.push_back (addButton.getBounds());
            return out;
        }

        std::function<void (const juce::String&, const juce::String&, float)> onTouch;

        Rack (ModularDevice& d) : device (d), patch (d.getPatch())
        {
            addButton.onClick = [this] { showAddMenu(); };
            addAndMakeVisible (addButton);
            patch.addListener (this);
            rebuild();
        }

        ~Rack() override
        {
            patch.removeListener (this);
            device.setProbe (-1, -1);
        }

        // While a cable is being dragged: where it comes from and the jack it would plug into
        std::optional<JackRef> dragSource() const   { return dragFrom; }
        std::optional<JackRef> dropTarget() const   { return target; }

        /** The output jack under the mouse (or none): it gets a live scope. */
        void hoverJack (std::optional<JackRef> jack)
        {
            if (jack && jack->isInput) jack.reset();
            if (jack.has_value() == hovered.has_value() && (! jack || (jack->moduleId == hovered->moduleId && jack->index == hovered->index)))
                return;
            hovered = jack;
            device.setProbe (jack ? jack->moduleId : -1, jack ? jack->index : -1);
            if (jack) startTimerHz (30); else stopTimer();
            repaint();
        }

        ModularDevice& getDevice()      { return device; }

        int idealWidth() const
        {
            int w = 8;
            for (auto& p : panels) w += p->getWidth() + 6;
            return w + 70;
        }

        //==============================================================================
        // In the device's own window the modules wrap into rows instead of one long strip.
        // Cables and jacks are worked out from where each module is, so they follow.
        static constexpr int rowGap = 8;
        std::function<void()> onRebuilt;    // the modules changed: the window lays the rows out again

        /** Rows no wider than `width`, each `rowHeight` tall (a width of 0: one long row). */
        void setWrap (int width, int rowHeight)
        {
            wrapWidth = width;
            wrapRowH = rowHeight;
            resized();
        }

        /** How many rows the modules (and the add button) need at this width. */
        int rowsFor (int width) const
        {
            int rows = 1, x = 4;
            auto place = [&] (int w)
            {
                if (x > 4 && x + w > width - 4) { x = 4; ++rows; }
                x += w + 6;
            };
            for (auto& p : panels) place (ModulePanel::widthFor (p->getSpec()));
            place (addButtonW - 6);
            return rows;
        }

        static int heightFor (int rows, int rowHeight)    { return rows * rowHeight + (rows - 1) * rowGap; }

        /** The row height the tallest module here needs. */
        int tallestModule() const
        {
            int h = 0;
            for (auto& p : panels) h = juce::jmax (h, ModulePanel::heightFor (p->getSpec()));
            return h;
        }

        void resized() override
        {
            const bool wrap = wrapWidth > 0;
            const int h = wrap ? wrapRowH : getHeight();
            int x = 4, y = 0;
            auto place = [&] (int w)
            {
                if (wrap && x > 4 && x + w > wrapWidth - 4) { x = 4; y += h + rowGap; }
                const juce::Point<int> at (x, y);
                x += w + 6;
                return at;
            };
            for (auto& p : panels)
            {
                const int w = ModulePanel::widthFor (p->getSpec());
                const auto at = place (w);
                p->setBounds (at.x, at.y, w, h);
            }
            const auto at = place (addButtonW - 6);
            addButton.setBounds (at.x, at.y + h / 2 - 30, addButtonW, 60);
        }

        void paint (juce::Graphics&) override {}

        void paintOverChildren (juce::Graphics& g) override
        {
            int index = 0;
            for (auto c : patch)
            {
                if (! c.hasType (Modular::cableId)) continue;
                auto from = jackPosition ({ (int) c["from"], false, outIndex (c) });
                auto to = jackPosition ({ (int) c["to"], true, inIndex (c) });
                if (from && to)
                    drawCable (g, *from, *to, signalColour (signalOf ({ (int) c["from"], false, outIndex (c) })), floorBelow (*from, *to));
                ++index;
            }

            if (dragFrom)
                if (auto p = jackPosition (*dragFrom))
                {
                    const auto end = target ? *jackPosition (*target) : dragPoint;
                    drawCable (g, *p, end, signalColour (signalOf (*dragFrom)), floorBelow (*p, end));
                }

            drawProbe (g);
        }

        // Jack dragging, called by the panels
        void beginCable (JackRef jack)
        {
            if (jack.isInput)
            {
                // Pick up the cable already in this input, from its other end
                for (auto c : patch)
                    if (c.hasType (Modular::cableId) && (int) c["to"] == jack.moduleId && inIndex (c) == jack.index)
                    {
                        dragFrom = JackRef { (int) c["from"], false, outIndex (c) };
                        device.getUndo()->beginNewTransaction ("Patch cable");
                        device.disconnectInput (jack.moduleId, c["in"].toString(), device.getUndo());
                        return;
                    }
            }
            dragFrom = jack;
            device.getUndo()->beginNewTransaction ("Patch cable");
        }

        void dragCable (juce::Point<float> rackPos)
        {
            dragPoint = rackPos;
            hoverJack ({});
            auto t = jackAt (rackPos);
            if (t && dragFrom && t->isInput == dragFrom->isInput) t.reset();
            const bool changed = t.has_value() != target.has_value() || (t && (t->moduleId != target->moduleId || t->index != target->index || t->isInput != target->isInput));
            target = t;
            if (changed) for (auto& p : panels) p->repaint();
            repaint();
        }

        void endCable (juce::Point<float> rackPos)
        {
            if (dragFrom)
                if (auto target = jackAt (rackPos))
                    if (target->isInput != dragFrom->isInput)
                    {
                        const auto out = dragFrom->isInput ? *target : *dragFrom;
                        const auto in = dragFrom->isInput ? *dragFrom : *target;
                        auto* fromPanel = panelFor (out.moduleId);
                        auto* toPanel = panelFor (in.moduleId);
                        if (fromPanel != nullptr && toPanel != nullptr)
                            device.connect (out.moduleId, fromPanel->getSpec().outs[(size_t) out.index].id,
                                            in.moduleId, toPanel->getSpec().ins[(size_t) in.index].id, device.getUndo());
                    }

            dragFrom.reset();
            target.reset();
            for (auto& p : panels) p->repaint();
            repaint();
        }

        void moveModule (int id, juce::Point<float> rackPos)
        {
            // The modules it was dropped after: those to its left and, when they wrap, those in the rows above
            int index = 0;
            for (auto& p : panels)
            {
                const auto b = p->getBounds().toFloat();
                const bool sameRow = wrapWidth <= 0 || (rackPos.y >= b.getY() && rackPos.y < b.getBottom() + (float) rowGap);
                const bool rowAbove = wrapWidth > 0 && rackPos.y >= b.getBottom() + (float) rowGap;
                if (rowAbove || (sameRow && rackPos.x > b.getCentreX())) ++index;
            }
            device.getUndo()->beginNewTransaction ("Move module");
            auto list = device.modulesInOrder();
            auto it = std::find_if (list.begin(), list.end(), [id] (const juce::ValueTree& m) { return (int) m["id"] == id; });
            const int current = (int) (it - list.begin());
            device.moveModule (id, index > current ? index - 1 : index, device.getUndo());
        }

        void touched (const juce::String& n, const juce::String& v, float p)   { if (onTouch) onTouch (n, v, p); }

    private:
        ModularDevice& device;
        juce::ValueTree patch;
        std::vector<std::unique_ptr<ModulePanel>> panels;
        juce::TextButton addButton { "+" };
        std::optional<JackRef> dragFrom, target, hovered;
        juce::Point<float> dragPoint;
        static constexpr int addButtonW = 56;
        int wrapWidth = 0, wrapRowH = 0;

        /** How low a cable between these two points may hang: the bottom of the lower one's row. */
        float floorBelow (juce::Point<float> a, juce::Point<float> b) const
        {
            if (wrapWidth <= 0) return (float) getHeight();
            const int step = wrapRowH + rowGap;
            const int row = juce::jmax (0, (int) juce::jmax (a.y, b.y) / juce::jmax (1, step));
            return (float) (row * step + wrapRowH);
        }

        void timerCallback() override   { repaint(); }

        Modular::Signal signalOf (JackRef j) const
        {
            auto* p = panelFor (j.moduleId);
            if (p == nullptr || j.index < 0) return Modular::Signal::audio;
            const auto& ports = j.isInput ? p->getSpec().ins : p->getSpec().outs;
            return j.index < (int) ports.size() ? ports[(size_t) j.index].signal : Modular::Signal::audio;
        }

        /** A little scope over the hovered output jack. */
        void drawProbe (juce::Graphics& g)
        {
            if (! hovered || dragFrom) return;
            auto pos = jackPosition (*hovered);
            auto* panel = panelFor (hovered->moduleId);
            if (! pos || panel == nullptr) return;
            const auto& port = panel->getSpec().outs[(size_t) hovered->index];
            auto box = juce::Rectangle<float> (112.0f, 52.0f).withCentre ({ pos->x, pos->y - 44.0f });
            box = box.constrainedWithin (getLocalBounds().toFloat().reduced (2.0f));
            g.setColour (Colours::display);
            g.fillRoundedRectangle (box, 5.0f);
            g.setColour (Colours::outline);
            g.drawRoundedRectangle (box, 5.0f, 1.0f);

            // About 16 ms (a couple of cycles of a bass note), drawn at every 4th sample
            std::array<float, 768> raw {};
            device.probeScope.read (raw.data(), (int) raw.size());
            std::array<float, 192> data {};
            for (size_t i = 0; i < data.size(); ++i) data[i] = raw[i * 4];
            float lo = 1.0e9f, hi = -1.0e9f;
            for (auto v : data) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
            const float span = juce::jmax (0.05f, hi - lo);
            auto plot = box.reduced (6.0f, 6.0f).withTrimmedTop (10.0f);
            juce::Path path;
            for (size_t i = 0; i < data.size(); ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) (data.size() - 1);
                const float y = plot.getBottom() - (data[i] - lo) / span * plot.getHeight();
                if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
            }
            g.setColour (signalColour (port.signal));
            g.strokePath (path, juce::PathStrokeType (1.4f));
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
            g.drawText (juce::String (port.name).toUpperCase() + "   " + juce::String (lo, 2) + " .. " + juce::String (hi, 2),
                        box.reduced (6.0f, 3.0f).removeFromTop (10.0f), juce::Justification::centredLeft);
        }

        ModulePanel* panelFor (int id) const
        {
            for (auto& p : panels)
                if (p->getModuleId() == id) return p.get();
            return nullptr;
        }

        int outIndex (const juce::ValueTree& c) const
        {
            auto* p = panelFor ((int) c["from"]);
            return p != nullptr ? Modular::portIndex (p->getSpec().outs, c["out"].toString()) : -1;
        }

        int inIndex (const juce::ValueTree& c) const
        {
            auto* p = panelFor ((int) c["to"]);
            return p != nullptr ? Modular::portIndex (p->getSpec().ins, c["in"].toString()) : -1;
        }

        std::optional<juce::Point<float>> jackPosition (JackRef j) const
        {
            auto* p = panelFor (j.moduleId);
            if (p == nullptr || j.index < 0) return {};
            return p->jackCentre (j.isInput, j.index) + p->getPosition().toFloat();
        }

        std::optional<JackRef> jackAt (juce::Point<float> rackPos) const
        {
            for (auto& p : panels)
                if (p->getBounds().toFloat().contains (rackPos))
                    return p->jackAt (rackPos - p->getPosition().toFloat());
            return {};
        }

        static void drawCable (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour colour, float height)
        {
            // Hangs a little with distance, but never below the bottom of the rack
            const float room = height - juce::jmax (a.y, b.y) - 6.0f;
            const float sag = juce::jlimit (6.0f, juce::jmax (6.0f, room * 1.3f), 18.0f + a.getDistanceFrom (b) * 0.22f);
            juce::Path p;
            p.startNewSubPath (a);
            p.cubicTo (a.translated (0.0f, sag), b.translated (0.0f, sag), b);
            g.setColour (colour.withAlpha (0.9f));
            g.strokePath (p, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (a));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (b));
        }

        void showAddMenu()
        {
            juce::PopupMenu m;
            int i = 1;
            for (auto& s : Modular::catalogue())
            {
                if (! s.fixed) m.addItem (i, s.name);
                ++i;
            }
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton), [safe = juce::Component::SafePointer<Rack> (this)] (int result)
            {
                if (safe == nullptr || result <= 0) return;
                safe->device.getUndo()->beginNewTransaction ("Add module");
                safe->device.addModule (Modular::catalogue()[(size_t) result - 1].type, safe->device.getUndo());
            });
        }

        void rebuild()
        {
            panels.clear();
            for (auto& m : device.modulesInOrder())
                if (Modular::findSpec (m["type"].toString()) != nullptr)
                {
                    panels.push_back (std::make_unique<ModulePanel> (*this, m, device.getUndo()));
                    addAndMakeVisible (*panels.back());
                }
            if (wrapWidth <= 0)
                setSize (idealWidth(), juce::jmax (getHeight(), 100));
            resized();
            if (wrapWidth > 0 && onRebuilt)
                onRebuilt();
            repaint();
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
        {
            if (parent == patch) { if (child.hasType (Modular::moduleId)) rebuild(); else repaint(); }
        }

        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
        {
            if (parent == patch) { if (child.hasType (Modular::moduleId)) rebuild(); else repaint(); }
        }

        void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& prop) override
        {
            if (t.hasType (Modular::moduleId) && prop.toString() == "pos") { rebuild(); return; }
            if (t.hasType (Modular::cableId)) repaint();
        }

        void valueTreeRedirected (juce::ValueTree&) override  { rebuild(); }
    };

    //==============================================================================
    inline juce::Colour moduleColour (const juce::String& type)
    {
        if (type == "vco" || type == "noise")                  return Colours::accent;
        if (type == "filter")                                   return Colours::solo;
        if (type == "vca" || type == "adsr" || type == "mixer") return Colours::clips[2];
        if (type == "lfo" || type == "sh" || type == "seq")     return Colours::clips[6];
        if (type == "delay")                                    return Colours::clips[5];
        if (type == "fold" || type == "ring")                   return Colours::accent;
        if (type == "lpg")                                      return Colours::clips[2];
        if (type == "quant" || type == "slew" || type == "clkdiv" || type == "random") return Colours::clips[6];
        return Colours::textDim;
    }

    inline ModulePanel::ModulePanel (Rack& r, juce::ValueTree m, juce::UndoManager* um)
        : rack (r), tree (m), spec (Modular::findSpec (m["type"].toString()))
    {
        for (auto& p : spec->params)
        {
            auto k = std::make_unique<ModKnob> (tree, p, um, moduleColour (spec->type));
            k->onTouch = [this] (const juce::String& n, const juce::String& v, float pr) { rack.touched (n, v, pr); };
            addAndMakeVisible (*k);
            knobs.push_back (std::move (k));
        }
    }

    inline void ModulePanel::resized()
    {
        const int cols = spec->columns;
        int y = titleH + 2;
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            const int row = (int) i / cols, col = (int) i % cols;
            knobs[i]->setBounds (4 + col * colW + 6, y + row * knobH, colW - 12, knobH - 12);
        }
        const int knobRows = ((int) knobs.size() + cols - 1) / cols;
        y += knobRows * knobH;
        inputsTop = y;
        y += (((int) spec->ins.size() + cols - 1) / cols) * jackH;
        outputsTop = juce::jmax (y + 2, getHeight() - (((int) spec->outs.size() + cols - 1) / cols) * jackH - 4);
    }

    inline void ModulePanel::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        const auto colour = moduleColour (spec->type);
        g.setColour (Colours::panelDark.withAlpha (0.55f));
        g.fillRoundedRectangle (r, 4.0f);

        // Title
        auto title = getLocalBounds().removeFromTop (titleH).reduced (6, 0);
        g.setColour (colour);
        g.fillRect (title.removeFromLeft (3).withSizeKeepingCentre (3, 8));
        title.removeFromLeft (5);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (juce::String (spec->name).toUpperCase(), title, juce::Justification::centredLeft);

        // Knob names
        g.setFont (juce::FontOptions (9.0f));
        for (size_t i = 0; i < knobs.size(); ++i)
            g.drawFittedText (spec->params[i].name, knobs[i]->getBounds().translated (0, knobs[i]->getHeight()).withHeight (11).expanded (5, 0),
                              juce::Justification::centred, 1, 0.7f);

        // Outputs sit on a darker plate
        if (! spec->outs.empty())
        {
            auto plate = juce::Rectangle<float> (2.0f, (float) outputsTop - 2.0f, r.getWidth() - 4.0f, r.getBottom() - (float) outputsTop);
            g.setColour (Colours::panelDark);
            g.fillRoundedRectangle (plate, 3.0f);
        }

        auto drawJack = [&] (bool isInput, int i, const Modular::Port& port)
        {
            auto c = jackCentre (isInput, i);
            const auto ring = juce::Rectangle<float> (13.0f, 13.0f).withCentre (c);
            const auto colour = signalColour (port.signal);
            // While a cable is being dragged, every jack it can plug into lights up
            if (auto src = rack.dragSource(); src && src->isInput != isInput)
            {
                const auto t = rack.dropTarget();
                const bool isTarget = t && t->moduleId == getModuleId() && t->isInput == isInput && t->index == i;
                g.setColour (colour.withAlpha (isTarget ? 0.55f : 0.22f));
                g.fillEllipse (ring.expanded (isTarget ? 6.0f : 4.0f));
            }
            g.setColour (isInput ? Colours::panel : Colours::panelLight);
            g.fillEllipse (ring);
            g.setColour (colour);
            g.drawEllipse (ring.reduced (1.0f), 2.0f);
            g.setColour (Colours::outline);
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c));
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (8.5f));
            g.drawFittedText (port.name, juce::Rectangle<float> (c.x - colW * 0.5f, c.y + 7.0f, (float) colW, 10.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
        };

        for (int i = 0; i < (int) spec->ins.size(); ++i)  drawJack (true, i, spec->ins[(size_t) i]);
        for (int i = 0; i < (int) spec->outs.size(); ++i) drawJack (false, i, spec->outs[(size_t) i]);
    }

    inline void ModulePanel::mouseDown (const juce::MouseEvent& e)
    {
        if (auto jack = jackAt (e.position))
        {
            rack.beginCable (*jack);
            rack.dragCable (e.position + getPosition().toFloat());
            return;
        }

        if (e.y < titleH)
        {
            if (e.mods.isPopupMenu())
            {
                if (spec->fixed) return;
                juce::PopupMenu m;
                m.addItem (1, "Delete " + juce::String (spec->name));
                m.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<ModulePanel> (this)] (int result)
                {
                    if (safe == nullptr || result != 1) return;
                    auto& dev = safe->rack.getDevice();
                    dev.getUndo()->beginNewTransaction ("Delete module");
                    dev.removeModule (safe->getModuleId(), dev.getUndo());
                });
                return;
            }
            draggingTitle = true;
        }
    }

    inline void ModulePanel::mouseDrag (const juce::MouseEvent& e)
    {
        if (draggingTitle)
        {
            setAlpha (0.6f);
            return;
        }
        rack.dragCable (e.position + getPosition().toFloat());
    }

    inline void ModulePanel::mouseMove (const juce::MouseEvent& e)     { rack.hoverJack (jackAt (e.position)); }
    inline void ModulePanel::mouseExit (const juce::MouseEvent&)       { rack.hoverJack ({}); }

    inline void ModulePanel::mouseUp (const juce::MouseEvent& e)
    {
        if (draggingTitle)
        {
            draggingTitle = false;
            setAlpha (1.0f);
            if (e.getDistanceFromDragStart() > 10)
                rack.moveModule (getModuleId(), e.position + getPosition().toFloat());
            return;
        }
        rack.endCable (e.position + getPosition().toFloat());
    }
}

//==============================================================================
/** Modular: display and patches on the left, the rack (scrolling sideways) on the right. */
class ModularView : public SynthPanel::PanelBase, private juce::Timer
{
public:
    /** `inWindow`: the layout for the device's own window (display and patch across the top, the rack in rows below). */
    ModularView (ModularDevice& d, bool inWindow = false) : dev (d), window (inWindow), rack (d)
    {
        if (window) setWindowRows (1);
        oled.readScope = [this] (float* dst, int n) { dev.scope.read (dst, n); };
        oled.getSamplesPerCycle = [this] { return dev.getSamplesPerCycle(); };
        oled.header = "MODULAR";
        addAndMakeVisible (oled);
        onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };
        rack.onTouch = [this] (const juce::String& n, const juce::String& v, float p) { oled.showValue (n, v, p); };

        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto c = Colours::clips[5];
        controls = { knob (d.voices, "Voices", c), knob (d.volume, "Volume", Colours::accent),
                     knob (d.macros[0], "Macro 1", Colours::clips[6]), knob (d.macros[1], "Macro 2", Colours::clips[6]),
                     knob (d.macros[2], "Macro 3", Colours::clips[6]), knob (d.macros[3], "Macro 4", Colours::clips[6]) };

        if (window)
        {
            // The rack is enlarged inside a holder, so the viewport scrolls its enlarged size
            rackHolder.addAndMakeVisible (rack);
            viewport.setViewedComponent (&rackHolder, false);
            viewport.setScrollBarsShown (true, false);
            rack.onRebuilt = [this] { resized(); };
        }
        else
        {
            viewport.setViewedComponent (&rack, false);
            viewport.setScrollBarsShown (false, true);
        }
        addAndMakeVisible (viewport);

        updatePatchName();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, controlArea, "Patch", Colours::clips[5]);
    }

    /** In its own window the whole rack is laid out, so it can wrap module by module. */
    int getNaturalWidth() const override    { return 200 + 6 + 110 + 6 + rack.idealWidth(); }

    void resized() override
    {
        if (window)
        {
            layoutWindow();
            return;
        }

        auto r = getLocalBounds();
        auto display = r.removeFromLeft (200);
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (4);
        oled.setBounds (display);
        prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (6);

        controlArea = r.removeFromLeft (110);
        r.removeFromLeft (6);
        auto c = content (controlArea);
        const int rowH = c.getHeight() / 3;
        for (int i = 0; i < 3; ++i)
            row (c.removeFromTop (rowH), { controls[(size_t) i * 2], controls[(size_t) i * 2 + 1] }, 2);

        viewport.setBounds (r);
        rack.setSize (juce::jmax (rack.idealWidth(), r.getWidth()), r.getHeight() - viewport.getScrollBarThickness());
        sections = { controlArea };
    }

private:
    ModularDevice& dev;
    const bool window;
    SynthPanel::Oled oled;
    ModularUI::Rack rack;
    juce::Component rackHolder;
    juce::Viewport viewport;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    std::vector<SynthPanel::Knob*> controls;
    juce::Rectangle<int> controlArea;
    int lastRackWidth = 0;

    /**
        The display and the patch controls across the top; under them the rack, its modules
        wrapped into rows and enlarged as far as the window allows, so a whole patch is in view.
    */
    void layoutWindow()
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (juce::jlimit (150, 190, r.getHeight() / 4));
        r.removeFromTop (8);

        oled.setBounds (top.removeFromLeft (top.getWidth() * 46 / 100));
        top.removeFromLeft (8);
        auto nav = top.removeFromTop (26);
        top.removeFromTop (6);
        prevButton.setBounds (nav.removeFromLeft (34)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (34)); nav.removeFromLeft (4);
        patchButton.setBounds (nav.removeFromLeft (juce::jmin (nav.getWidth(), 320)));

        controlArea = top;
        auto c = content (controlArea);
        row (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 84)),
             { controls[0], controls[1], controls[2], controls[3], controls[4], controls[5] }, 6);

        // The biggest size at which every row still fits the height (from a little under the rest
        // of the panel's size to a little over it); failing that, the smallest, and scrolling.
        // A row is tall enough for all of its tallest module, outputs included.
        const int minRow = juce::jmax (164, rack.tallestModule()), maxRow = minRow + 40;
        float zoom = 0.8f;
        int width = r.getWidth(), rows = 1;
        bool fits = false;
        for (int percent = 120; percent >= 80 && ! fits; percent -= 5)
        {
            const float z = (float) percent / 100.0f;
            const int w = (int) ((float) r.getWidth() / z);
            const int n = rack.rowsFor (w);
            if ((float) ModularUI::Rack::heightFor (n, minRow) * z <= (float) r.getHeight())
            {
                zoom = z; width = w; rows = n; fits = true;
            }
        }
        if (! fits)
        {
            width = (int) ((float) (r.getWidth() - viewport.getScrollBarThickness()) / zoom);
            rows = rack.rowsFor (width);
        }

        const int rowH = fits ? juce::jlimit (minRow, maxRow, ((int) ((float) r.getHeight() / zoom) - (rows - 1) * ModularUI::Rack::rowGap) / rows)
                              : minRow;
        const int height = ModularUI::Rack::heightFor (rows, rowH);
        rack.setTransform (juce::AffineTransform::scale (zoom));
        rack.setBounds (0, 0, width, height);
        rack.setWrap (width, rowH);
        rackHolder.setSize ((int) ((float) width * zoom), (int) ((float) height * zoom));
        viewport.setBounds (r);
    }

    void step (int delta)
    {
        const int count = (int) ModularDevice::patches().size();
        const int current = dev.getPatchIndex();
        dev.loadPatch (((current < 0 ? (delta > 0 ? -1 : 0) : current) + delta + count) % count);
        updatePatchName();
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int i = 0; i < (int) ModularDevice::patches().size(); ++i)
            m.addItem (i + 1, ModularDevice::patches()[(size_t) i].name, true, i == dev.getPatchIndex());

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<ModularView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
            {
                safe->dev.loadPatch (result - 1);
                safe->updatePatchName();
            }
        });
    }

    void updatePatchName()
    {
        const int index = dev.getPatchIndex();
        oled.patchName = index >= 0 && index < (int) ModularDevice::patches().size()
                           ? juce::String ("M") + juce::String (index + 1).paddedLeft ('0', 3) + " " + ModularDevice::patches()[(size_t) index].name
                           : juce::String ("Custom patch");
    }

    void timerCallback() override
    {
        updatePatchName();
        if (rack.idealWidth() != lastRackWidth)
        {
            lastRackWidth = rack.idealWidth();
            resized();
        }
    }
};

inline std::unique_ptr<juce::Component> ModularDevice::createDeviceView()      { return std::make_unique<ModularView> (*this); }
inline std::unique_ptr<juce::Component> ModularDevice::createWindowView()      { return std::make_unique<SynthPanel::Zoomed> (std::make_unique<ModularView> (*this, true), 1.25f, getWindowViewSize()); }

} // namespace elsewhere
