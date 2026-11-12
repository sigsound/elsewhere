// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Crash-safe plug-in scanning.

    Each plug-in is loaded in a separate short-lived copy of Elsewhere
    (`Elsewhere --scan-plugin <format> <resultFile> <identifierFile>`), so a plug-in that
    crashes or hangs during scanning only takes down that helper process. The plug-in
    is then reported as failed and skipped, and Elsewhere keeps running.
*/
namespace PluginScanner
{
    static constexpr const char* scanArg = "--scan-plugin";
    static constexpr int timeoutMs = 60000;

    /** Runs in the helper process: scans one plug-in and writes its descriptions to resultFile. */
    inline int runChildScan (const juce::StringArray& args)
    {
        auto index = args.indexOf (scanArg);

        if (index < 0 || args.size() < index + 4)
            return 1;

        auto formatName = args[index + 1];
        auto resultFile = juce::File (args[index + 2]);
        auto identifier = juce::File (args[index + 3]).loadFileAsString();

        juce::AudioPluginFormatManager formats;
        juce::addDefaultFormatsToManager (formats);

        for (auto* format : formats.getFormats())
        {
            if (format->getName() != formatName)
                continue;

            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile (found, identifier);

            juce::XmlElement xml ("PLUGINS");
            for (auto* d : found)
                xml.addChildElement (d->createXml().release());

            resultFile.replaceWithText (xml.toString());
            return found.isEmpty() ? 2 : 0;
        }

        return 3;
    }

    //==============================================================================
    /** Runs in the main app: hands each plug-in to a helper process. */
    class OutOfProcessScanner : public juce::KnownPluginList::CustomScanner
    {
    public:
        bool findPluginTypesFor (juce::AudioPluginFormat& format,
                                 juce::OwnedArray<juce::PluginDescription>& result,
                                 const juce::String& fileOrIdentifier) override
        {
            auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
            juce::TemporaryFile idFile (".txt"), resultFile (".xml");
            idFile.getFile().replaceWithText (fileOrIdentifier);

            juce::ChildProcess child;
            juce::StringArray cmd { exe.getFullPathName(), scanArg, format.getName(),
                                    resultFile.getFile().getFullPathName(), idFile.getFile().getFullPathName() };

            if (! child.start (cmd, 0))
                return false;

            // Poll so the scan can be cancelled from the plug-in manager
            for (int waited = 0; waited < timeoutMs; waited += 100)
            {
                if (child.waitForProcessToFinish (100))
                    break;

                if (shouldExit())
                {
                    child.kill();
                    return true;
                }
            }

            if (child.isRunning())
            {
                child.kill();   // hung: treat as failed
                return false;
            }

            if (auto xml = juce::XmlDocument::parse (resultFile.getFile()))
            {
                for (auto* e : xml->getChildIterator())
                {
                    auto d = std::make_unique<juce::PluginDescription>();

                    if (d->loadFromXml (*e))
                        result.add (d.release());
                }
            }

            // Crashed helper -> non-zero exit code and no results -> reported as failed
            return child.getExitCode() == 0 || ! result.isEmpty();
        }
    };
}

} // namespace elsewhere
