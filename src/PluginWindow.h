// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/** Hosts a third-party plugin's own editor (VST3/AU). */
struct ExternalPluginEditor : public te::Plugin::EditorComponent
{
    ExternalPluginEditor (te::ExternalPlugin& p) : plugin (p)
    {
        JUCE_AUTORELEASEPOOL
        {
            if (auto pi = plugin.getAudioPluginInstance())
            {
                editor.reset (pi->createEditorAndMakeActive());

                if (editor == nullptr)
                    editor = std::make_unique<juce::GenericAudioProcessorEditor> (*pi);

                addAndMakeVisible (*editor);
            }
        }

        resizeToFitEditor();
    }

    bool allowWindowResizing() override { return false; }

    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override
    {
        return editor != nullptr ? editor->getConstrainer() : nullptr;
    }

    void resized() override
    {
        if (editor != nullptr)
            editor->setBounds (getLocalBounds());
    }

    void childBoundsChanged (juce::Component* c) override
    {
        if (c == editor.get())
        {
            plugin.edit.pluginChanged (plugin);
            resizeToFitEditor();
        }
    }

    void resizeToFitEditor()
    {
        setSize (juce::jmax (8, editor != nullptr ? editor->getWidth() : 0),
                 juce::jmax (8, editor != nullptr ? editor->getHeight() : 0));
    }

    te::ExternalPlugin& plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

//==============================================================================
/** Fallback editor for built-in Tracktion plugins: a grid of knobs, one per parameter. */
struct GenericParameterEditor : public te::Plugin::EditorComponent
{
    GenericParameterEditor (te::Plugin& p) : plugin (p)
    {
        for (auto param : plugin.getAutomatableParameters())
        {
            auto* s = sliders.add (new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow));
            auto range = param->getValueRange();
            s->setRange (range.getStart(), range.getEnd(), 0.0);
            s->setValue (param->getCurrentValue(), juce::dontSendNotification);
            s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            s->textFromValueFunction = [param] (double) { return param->getCurrentValueAsStringWithLabel(); };
            s->onValueChange = [param, s] { param->setParameter ((float) s->getValue(), juce::sendNotification); };
            s->onDragStart = [param] { param->parameterChangeGestureBegin(); };
            s->onDragEnd = [param] { param->parameterChangeGestureEnd(); };
            addAndMakeVisible (s);

            auto* l = labels.add (new juce::Label ({}, param->getParameterName()));
            l->setJustificationType (juce::Justification::centred);
            l->setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (l);

            params.add (param);
        }

        const int cols = juce::jmax (1, juce::jmin (6, params.size()));
        const int rows = (params.size() + cols - 1) / cols;
        setSize (cols * cellW, juce::jmax (1, rows) * cellH);
        refreshTimer.startTimerHz (15);
    }

    bool allowWindowResizing() override                         { return true; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override { return nullptr; }

    void paint (juce::Graphics& g) override { g.fillAll (Colours::panel); }

    void resized() override
    {
        const int cols = juce::jmax (1, getWidth() / cellW);

        for (int i = 0; i < sliders.size(); ++i)
        {
            auto cell = juce::Rectangle<int> ((i % cols) * cellW, (i / cols) * cellH, cellW, cellH).reduced (4);
            labels[i]->setBounds (cell.removeFromTop (16));
            sliders[i]->setBounds (cell);
        }
    }

    static constexpr int cellW = 96, cellH = 110;
    te::Plugin& plugin;
    juce::OwnedArray<juce::Slider> sliders;
    juce::OwnedArray<juce::Label> labels;
    juce::Array<te::AutomatableParameter*> params;

    te::LambdaTimer refreshTimer { [this]
    {
        // Keep knobs in sync with automation / other editors
        for (int i = 0; i < params.size(); ++i)
            if (! sliders[i]->isMouseButtonDown())
                sliders[i]->setValue (params[i]->getCurrentValue(), juce::dontSendNotification);
    } };
};

//==============================================================================
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (te::Plugin& p)
        : DocumentWindow (p.getName(), juce::Colours::black, DocumentWindow::closeButton, true),
          plugin (p)
    {
        getConstrainer()->setMinimumOnscreenAmounts (0x10000, 50, 30, 50);
        setResizeLimits (100, 50, 4000, 4000);
        setUsingNativeTitleBar (true);

        recreateEditor();

        setBoundsConstrained (getLocalBounds() + plugin.windowState->choosePositionForPluginWindow());
        updateStoredBounds = true;
    }

    ~PluginWindow() override
    {
        updateStoredBounds = false;
        plugin.edit.flushPluginStateIfNeeded (plugin);
        setEditor (nullptr);
    }

    static std::unique_ptr<juce::Component> create (te::Plugin& plugin)
    {
        if (auto ext = dynamic_cast<te::ExternalPlugin*> (&plugin))
            if (ext->getAudioPluginInstance() == nullptr)
                return {};

        auto w = std::make_unique<PluginWindow> (plugin);

        if (w->editor == nullptr)
            return {};

        w->setVisible (true);
        w->toFront (true);
        return w;
    }

    void setEditor (std::unique_ptr<te::Plugin::EditorComponent> newEditor)
    {
        JUCE_AUTORELEASEPOOL
        {
            setConstrainer (nullptr);
            editor.reset();

            if (newEditor != nullptr)
            {
                editor = std::move (newEditor);
                setContentNonOwned (editor.get(), true);
            }

            setResizable (editor == nullptr || editor->allowWindowResizing(), false);

            if (editor != nullptr && editor->allowWindowResizing())
                setConstrainer (editor->getBoundsConstrainer());
        }
    }

    void recreateEditor()
    {
        setEditor (nullptr);

        if (auto ext = dynamic_cast<te::ExternalPlugin*> (&plugin))
            setEditor (std::make_unique<ExternalPluginEditor> (*ext));
        else if (auto e = plugin.createEditor())
            setEditor (std::move (e));
        else
            setEditor (std::make_unique<GenericParameterEditor> (plugin));
    }

    void recreateEditorAsync()
    {
        setEditor (nullptr);
        juce::Timer::callAfterDelay (50, [sp = SafePointer<PluginWindow> (this)]
        {
            if (sp != nullptr)
                sp->recreateEditor();
        });
    }

private:
    void moved() override
    {
        if (updateStoredBounds)
        {
            plugin.windowState->lastWindowBounds = getBounds();
            plugin.edit.pluginChanged (plugin);
        }
    }

    void closeButtonPressed() override              { plugin.windowState->closeWindowExplicitly(); }
    float getDesktopScaleFactor() const override    { return 1.0f; }

    te::Plugin& plugin;
    std::unique_ptr<te::Plugin::EditorComponent> editor;
    bool updateStoredBounds = false;
};

//==============================================================================
class ElsewhereUIBehaviour : public te::UIBehaviour
{
public:
    /** The edit currently open in the main window (set by MainComponent). */
    static te::Edit*& currentEdit()             { static te::Edit* e = nullptr; return e; }

    te::Edit* getCurrentlyFocusedEdit() override    { return currentEdit(); }
    te::Edit* getLastFocusedEdit() override         { return currentEdit(); }

    /** Used by the engine for exports, bounces and freezes: runs the render job on a
        background thread behind a progress window, blocking until it's done. */
    void runTaskWithProgressBar (te::ThreadPoolJobWithProgress& job) override
    {
        struct RenderWindow : public juce::ThreadWithProgressWindow
        {
            RenderWindow (te::ThreadPoolJobWithProgress& j)
                : juce::ThreadWithProgressWindow (j.getJobName(), true, j.canCancel()), job (j) {}

            void run() override
            {
                while (! threadShouldExit())
                {
                    if (job.runJob() != juce::ThreadPoolJob::jobNeedsRunningAgain)
                        break;

                    setProgress (job.getCurrentTaskProgress());
                }

                if (threadShouldExit())
                    job.signalJobShouldExit();
            }

            te::ThreadPoolJobWithProgress& job;
        };

        RenderWindow window (job);
        window.runThread();
    }

    std::unique_ptr<juce::Component> createPluginWindow (te::PluginWindowState& pws) override
    {
        if (auto ws = dynamic_cast<te::Plugin::WindowState*> (&pws))
            return PluginWindow::create (ws->plugin);

        return {};
    }

    void recreatePluginWindowContentAsync (te::Plugin& p) override
    {
        if (auto w = dynamic_cast<PluginWindow*> (p.windowState->pluginWindow.get()))
            return w->recreateEditorAsync();

        UIBehaviour::recreatePluginWindowContentAsync (p);
    }
};

} // namespace elsewhere
