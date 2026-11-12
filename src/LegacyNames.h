#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere was called Tandem until version 0.17. Projects, presets, templates and settings
    saved under the old name still open: these helpers translate the old names as they're read.

    A project keeps the extension it was created with (an old "Song.tandem" goes on saving
    backups and collaboration snapshots as .tandem), so a shared folder stays consistent.
    Collaborators all need a version from after the rename, though: the device and property
    names inside the file changed.
*/
namespace Legacy
{
    inline constexpr const char* projectExtension = ".tandem";
    inline constexpr const char* presetExtension  = ".tandempreset";

    /** The new spelling of a Tandem-era identifier, or an empty string if it isn't one. */
    inline juce::String upgradedName (const juce::String& name)
    {
        if (name.startsWith ("tandem"))    return "elsewhere" + name.substring (6);
        if (name.startsWith ("TANDEM_"))   return "ELSEWHERE_" + name.substring (7);
        return {};
    }

    /** Renames Tandem-era identifiers in a saved edit or preset: node types ("TANDEM_COMMENTS"),
        properties ("tandemSavedBy") and device types ("tandemDrums"). Returns the upgraded
        tree, which is a new one if its own type had to change. */
    inline juce::ValueTree upgradeIdentifiers (juce::ValueTree v)
    {
        if (auto newType = upgradedName (v.getType().toString()); newType.isNotEmpty())
        {
            juce::ValueTree renamed (newType);
            renamed.copyPropertiesAndChildrenFrom (v, nullptr);

            if (auto parent = v.getParent(); parent.isValid())
            {
                auto index = parent.indexOf (v);
                parent.removeChild (index, nullptr);
                parent.addChild (renamed, index, nullptr);
            }

            v = renamed;
        }

        for (int i = v.getNumProperties(); --i >= 0;)
        {
            auto name = v.getPropertyName (i);

            if (auto newName = upgradedName (name.toString()); newName.isNotEmpty())
            {
                v.setProperty (newName, v[name], nullptr);
                v.removeProperty (name, nullptr);
            }
        }

        if (auto newType = upgradedName (v[te::IDs::type].toString()); newType.isNotEmpty())
            v.setProperty (te::IDs::type, newType, nullptr);

        for (int i = 0; i < v.getNumChildren(); ++i)
            upgradeIdentifiers (v.getChild (i));

        return v;
    }

    /** An attribute of a project file read without loading it, under its new or old name. */
    inline juce::String getAttribute (const juce::XmlElement& xml, const juce::String& name)
    {
        if (xml.hasAttribute (name))
            return xml.getStringAttribute (name);

        return xml.getStringAttribute ("tandem" + name.fromFirstOccurrenceOf ("elsewhere", false, false));
    }

    /** The folder to use: the new one, unless only the Tandem-era one exists (so existing
        projects, templates and presets stay where people put them). */
    inline juce::File preferExisting (const juce::File& folder, const juce::File& legacyFolder)
    {
        return (! folder.exists() && legacyFolder.isDirectory()) ? legacyFolder : folder;
    }

    /** "*.elsewhere;*.tandem": project files, including ones saved before the rename. */
    inline juce::String projectWildcard (const juce::String& extension)
    {
        return "*" + extension + ";*" + projectExtension;
    }

    /** Moves the app's own data (settings, plug-in list, downloaded sound packs) from the Tandem
        folders to the Elsewhere ones, the first time Elsewhere starts. */
    inline void moveAppFolders()
    {
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

        auto move = [] (const juce::File& from, const juce::File& to)
        {
            if (from.isDirectory() && ! to.exists())
                from.moveFileTo (to);
        };

        move (base.getChildFile ("Tandem"), base.getChildFile ("Elsewhere"));
       #if JUCE_MAC
        move (base.getChildFile ("Application Support/Tandem"), base.getChildFile ("Application Support/Elsewhere"));
       #endif
    }
}

} // namespace elsewhere
