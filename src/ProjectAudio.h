// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Project.h"

namespace elsewhere
{
//==============================================================================
/**
    Keeps a project's audio inside its folder, and finds audio that has gone missing.

    A project only survives being moved (to iCloud, Dropbox, another Mac) if every file it
    plays is inside its folder. A recording or file that lives elsewhere is stored as a path
    like "../../Other Song Project/Samples/x.wav", which breaks the moment the project moves.
*/
namespace ProjectAudio
{
    /** Every place the edit refers to an audio file: clips and their takes. */
    inline juce::Array<juce::ValueTree> findSourceNodes (const juce::ValueTree& v)
    {
        juce::Array<juce::ValueTree> nodes;

        std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& t)
        {
            if (t.hasProperty (te::IDs::source) && (t.hasType (te::IDs::AUDIOCLIP) || t.hasType (te::IDs::TAKE)))
                if (t[te::IDs::source].toString().isNotEmpty())
                    nodes.add (t);

            for (auto c : t)
                walk (c);
        };

        walk (v);
        return nodes;
    }

    inline juce::File resolve (te::Edit& edit, const juce::String& path)
    {
        return edit.filePathResolver ? edit.filePathResolver (path) : juce::File (path);
    }

    /** Points a clip (or take) at a file inside the project, stored relative to the project folder. */
    inline void relink (juce::ValueTree node, Project& project, const juce::File& file)
    {
        auto path = file.isAChildOf (project.getFolder()) ? file.getRelativePathFrom (project.getFolder()).replaceCharacter ('\\', '/')
                                                          : file.getFullPathName();
        node.setProperty (te::IDs::source, path, nullptr);
    }

    /** Copies audio the project uses from outside its folder into Samples/Imported, and points the
        clips at the copies. Returns how many files were copied. */
    inline int collectExternal (Project& project)
    {
        auto& edit = project.getEdit();
        std::map<juce::String, juce::File> copied;   // original path -> copy

        for (auto node : findSourceNodes (edit.state))
        {
            auto file = resolve (edit, node[te::IDs::source].toString());

            if (! file.existsAsFile() || file.isAChildOf (project.getFolder()))
                continue;

            auto key = file.getFullPathName();

            if (copied.count (key) == 0)
                copied[key] = project.importAudioFile (file);

            if (copied[key].existsAsFile())
                relink (node, project, copied[key]);
        }

        return (int) copied.size();
    }

    /** Files the project refers to that aren't where it says (by file name, each listed once). */
    inline juce::StringArray findMissing (Project& project)
    {
        auto& edit = project.getEdit();
        juce::StringArray names;

        for (auto node : findSourceNodes (edit.state))
        {
            auto path = node[te::IDs::source].toString();

            if (! resolve (edit, path).existsAsFile())
                names.addIfNotAlreadyThere (juce::File::createFileWithoutCheckingPath (path.replaceCharacter ('\\', '/')).getFileName());
        }

        return names;
    }

    /** Looks for the missing files by name under a folder (and its subfolders). */
    inline std::map<juce::String, juce::File> search (const juce::File& folder, const juce::StringArray& names, int maxFiles = 200000)
    {
        std::map<juce::String, juce::File> found;
        int seen = 0;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            if (++seen > maxFiles || found.size() == (size_t) names.size())
                break;

            auto f = entry.getFile();

            if (names.contains (f.getFileName()) && found.count (f.getFileName()) == 0)
                found[f.getFileName()] = f;
        }

        return found;
    }

    /** Relinks every missing file found under a folder: copies it into the project if it's outside.
        Returns how many of the names were found. */
    inline int relinkFrom (Project& project, const juce::File& folder)
    {
        auto& edit = project.getEdit();
        auto missing = findMissing (project);

        if (missing.isEmpty())
            return 0;

        auto found = search (folder, missing);
        std::map<juce::String, juce::File> imported;

        for (auto node : findSourceNodes (edit.state))
        {
            auto path = node[te::IDs::source].toString();

            if (resolve (edit, path).existsAsFile())
                continue;

            auto name = juce::File::createFileWithoutCheckingPath (path.replaceCharacter ('\\', '/')).getFileName();
            auto it = found.find (name);

            if (it == found.end())
                continue;

            if (imported.count (name) == 0)
                imported[name] = project.importAudioFile (it->second);

            relink (node, project, imported[name]);
        }

        return (int) found.size();
    }
}

} // namespace elsewhere
