// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Export.h"
#include "Project.h"

namespace elsewhere
{
//==============================================================================
/**
    Export Several Projects: renders each chosen project with the same export settings, one
    after another, into that project's own Exports folder. Each is opened from what's saved
    (the open project uses what's on screen, saved or not), the whole song every time.
*/
namespace ExportBatch
{
    /** Runs the exports. Needs the message thread turning; tells you when it's done. */
    class Runner
    {
    public:
        Runner (te::Engine& e, juce::Array<juce::File> projects, Export::Settings s, te::Edit* open = nullptr)
            : engine (e), files (std::move (projects)), settings (std::move (s)), openEdit (open)
        {
            settings.range = Export::Range::song;
        }

        ~Runner()
        {
            *alive = false;
            queue.reset();
        }

        /** Each project's files, and what went wrong for the ones that didn't export. */
        juce::Array<juce::File> written;
        juce::StringArray failures, unmarked;
        int numExported = 0;

        std::function<void()> onFinished;

        void start()    { next(); }
        void cancel()   { cancelled = true; if (queue != nullptr) queue->cancelAll(); }

        bool isFinished() const     { return finished; }
        juce::String getStatus() const  { return status; }

        double getProgress() const
        {
            if (files.isEmpty())
                return 1.0;

            auto inThisOne = queue != nullptr ? queue->getTotalProgress() : 0.0;
            return juce::jlimit (0.0, 1.0, (std::max (0, index - 1) + inThisOne) / files.size());
        }

    private:
        te::Engine& engine;
        juce::Array<juce::File> files;
        Export::Settings settings;
        te::Edit* openEdit;
        std::unique_ptr<te::Edit> loaded;
        std::unique_ptr<te::RenderQueue> queue;
        Export::Plan planned;
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
        int index = 0;
        bool cancelled = false, finished = false, thisOneFailed = false;
        juce::String status;

        void later()
        {
            // The queue calls back from inside itself, so it's replaced afterwards
            juce::MessageManager::callAsync ([this, a = alive] { if (*a) next(); });
        }

        void next()
        {
            queue.reset();
            loaded.reset();

            if (cancelled || index >= files.size())
            {
                finished = true;

                if (onFinished)
                    onFinished();

                return;
            }

            auto file = files[index++];
            auto name = file.getFileNameWithoutExtension();
            te::Edit* edit = nullptr;

            if (openEdit != nullptr && openEdit->editFileRetriever && openEdit->editFileRetriever() == file)
            {
                edit = openEdit;
                edit->getTransport().stop (false, true);
            }
            else if ((loaded = Project::openForRendering (engine, file)) != nullptr)
            {
                edit = loaded.get();
            }

            if (edit == nullptr)
            {
                failures.add (name + ": couldn't be opened");
                return later();
            }

            planned = Export::plan (*edit, settings, file.getParentDirectory().getChildFile ("Exports"), name);

            if (planned.jobs.empty())
            {
                failures.add (name + ": " + (planned.problems.isEmpty() ? juce::String ("nothing to export") : planned.problems.joinIntoString ("; ")));
                return later();
            }

            status = "Exporting " + name + " (" + juce::String (index) + " of " + juce::String (files.size()) + ")...";
            thisOneFailed = false;
            planned.folder.createDirectory();
            queue = std::make_unique<te::RenderQueue>();
            queue->addJobs (std::move (planned.jobs));
            queue->onJobFinished = [this, name] (te::RenderQueue::Job& job)
            {
                if (job.getState() == te::RenderQueue::Job::State::failed)
                {
                    failures.add (name + ", " + job.getName() + ": " + job.getError());
                    thisOneFailed = true;
                }
                else if (job.getState() == te::RenderQueue::Job::State::completed)
                {
                    written.add (job.getFile());

                    if (auto problem = Export::finishFile (planned, job.getFile()); problem.isNotEmpty())
                        unmarked.add (job.getFile().getFileName() + ": " + problem);
                }
            };
            queue->onFinished = [this]
            {
                if (! thisOneFailed && ! cancelled)
                    ++numExported;

                later();
            };
            queue->start();
        }
    };

    //==============================================================================
    /** The progress window's content. */
    class Progress : public juce::Component, private juce::Timer
    {
    public:
        explicit Progress (std::unique_ptr<Runner> r) : runner (std::move (r))
        {
            label.setFont (juce::FontOptions (12.5f));
            label.setColour (juce::Label::textColourId, Colours::textDim);
            addAndMakeVisible (label);
            addAndMakeVisible (bar);
            setSize (400, 56);
            runner->onFinished = [this] { stopTimer(); if (onDone) onDone (*runner); };
            runner->start();
            startTimerHz (15);
        }

        std::function<void (Runner&)> onDone;

        void cancel()   { runner->cancel(); }

        void resized() override
        {
            auto r = getLocalBounds();
            label.setBounds (r.removeFromTop (22));
            bar.setBounds (r.removeFromTop (14).reduced (0, 2));
        }

    private:
        std::unique_ptr<Runner> runner;
        juce::Label label;
        double progress = 0.0;
        juce::ProgressBar bar { progress };

        void timerCallback() override
        {
            progress = runner->getProgress();
            label.setText (runner->getStatus(), juce::dontSendNotification);
        }
    };

    /** Asks, then exports the projects chosen. */
    inline void run (te::Engine& engine, juce::Array<juce::File> projects, te::Edit* openEdit)
    {
        if (projects.isEmpty())
            return;

        auto settings = Export::lastSettings();
        auto what = settings.what == Export::What::mix ? juce::String ("the mix")
                  : settings.what == Export::What::stems ? juce::String ("the stems") : juce::String ("the mix and stems");
        auto count = projects.size() == 1 ? juce::String ("1 project") : juce::String (projects.size()) + " projects";

        auto* aw = Theme::newAlertWindow ("Export " + count,
                                          "Exports " + what + " of each, the whole song, as " + Export::describeFormat (settings)
                                          + ", into each project's Exports folder. Change these in Export Audio.");
        aw->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        aw->enterModalState (true, juce::ModalCallbackFunction::create ([&engine, projects, openEdit, settings, count] (int result)
        {
            if (result != 1)
                return;

            auto progress = std::make_shared<Progress> (std::make_unique<Runner> (engine, projects, settings, openEdit));
            auto* pw = Theme::newAlertWindow ("Exporting " + count, {});
            pw->addCustomComponent (progress.get());
            pw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

            juce::Component::SafePointer<juce::AlertWindow> safe (pw);
            progress->onDone = [safe, total = projects.size()] (Runner& r)
            {
                if (safe != nullptr)
                    safe->exitModalState (1);

                juce::String text;

                if (! r.failures.isEmpty())
                    text << "These weren't exported:\n" << r.failures.joinIntoString ("\n") << "\n\n";

                if (! r.unmarked.isEmpty())
                    text << "These were exported without the AI training notice:\n" << r.unmarked.joinIntoString ("\n");

                if (r.failures.isEmpty() && r.unmarked.isEmpty())
                {
                    if (total == 1 && ! r.written.isEmpty())
                        r.written.getFirst().getParentDirectory().revealToUser();
                    else if (r.numExported > 0)
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Exported " + juce::String (r.numExported) + " projects",
                                                                "Each one's files are in its Exports folder.");

                    return;
                }

                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                        "Exported " + juce::String (r.numExported) + " of " + juce::String (total),
                                                        text.trim());
            };

            pw->enterModalState (true, juce::ModalCallbackFunction::create ([progress] (int r)
            {
                if (r == 0)
                    progress->cancel();
            }), true);
        }), true);
    }
}

} // namespace elsewhere
