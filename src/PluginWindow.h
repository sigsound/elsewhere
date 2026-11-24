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

    /** The engine shows every warning as a two-second bubble wherever the mouse happens to be,
        which suits its passing remarks ("can't add that here"). The ones about a recording are
        things to act on, so those stay on screen until they're read. */
    void showWarningMessage (const juce::String& message) override
    {
        if (message.containsIgnoreCase ("disk space"))
            warn ("The disk is nearly full. Stop recording soon and free some space, or the take may be cut short.", "Recording");
        else if (message.containsIgnoreCase ("Couldn't add the new recording"))
            warn ("The take couldn't be added to the project. Its audio is in the project's Samples/Recorded folder: "
                  "drag it onto a track to bring it in.", "Recording");
        else
            UIBehaviour::showWarningMessage (message);
    }

    /** Shows a warning once: the same one isn't stacked up while it's still on screen or was
        just dismissed. Any thread. */
    static void warn (const juce::String& message, const juce::String& title = "Elsewhere")
    {
        // (Always from the message loop, never in the middle of whatever raised it)
        juce::MessageManager::callAsync ([message, title]
        {
            static juce::String lastMessage;
            static juce::uint32 lastTime = 0;
            auto now = juce::Time::getMillisecondCounter();

            if (message == lastMessage && now - lastTime < 30000)
                return;

            lastMessage = message;
            lastTime = now;
            juce::Logger::writeToLog ("Warning shown: " + message);
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, title, message);
        });
    }
};

//==============================================================================
/**
    Keeps a log file (for working out afterwards what went wrong on someone's machine), and tells
    the user about the two recording failures the engine only writes to its log: the disk not
    keeping up, and a write that failed.
*/
class AppLog : public juce::Logger
{
public:
    AppLog (const juce::String& folderName)
        : file (juce::FileLogger::createDefaultAppLogger (folderName, "Elsewhere.log",
                                                          juce::String ("Elsewhere ") + ELSEWHERE_VERSION + ", " + juce::SystemStats::getOperatingSystemName(),
                                                          512 * 1024))
    {
        juce::Logger::setCurrentLogger (this);
    }

    ~AppLog() override      { juce::Logger::setCurrentLogger (nullptr); }

    juce::File getFile() const      { return file != nullptr ? file->getLogFile() : juce::File(); }

private:
    std::unique_ptr<juce::FileLogger> file;

    void logMessage (const juce::String& message) override
    {
        if (file != nullptr)
            file->logMessage (message);

        juce::Logger::outputDebugString (message);

        if (message.contains ("Audio recording can't keep up"))
            ElsewhereUIBehaviour::warn ("The disk isn't keeping up with the recording. Nothing has been lost yet, but stop when you can: "
                                        "record to a faster or less busy disk, or record fewer tracks at once.", "Recording");
        else if (message.contains ("Audio recording failed to write to disk"))
            ElsewhereUIBehaviour::warn ("The recording couldn't be written to disk, so it has stopped. Check that the disk isn't full and "
                                        "that the project's folder can be written to. What was recorded up to now is kept.", "Recording");
    }
};

} // namespace elsewhere
