// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    Version history: every saved version of a project, kept compressed in one hidden file
    beside it ("My Song.elsewhere" keeps ".My Song.history"), newest last. Saving adds a
    checkpoint; unsaved changes set aside after a crash, and a damaged file that was replaced,
    go in too. The oldest go once there are more than `keep`.

    The file is a header and then one checkpoint after another, each written on the end, so
    a save never rewrites what's there (except when the oldest are dropped). A checkpoint cut
    short (a sync that hasn't finished) ends the list there; everything before it still reads.
    A copy a sync service makes when two computers write at once is read too.
*/
namespace History
{
    constexpr int keep = 200;

    enum class Kind { saved, unsaved, damaged };

    struct Checkpoint
    {
        juce::Time time;
        Kind kind = Kind::saved;
        juce::String savedBy, label, hash;
        juce::File file;            // the history file it's in
        juce::int64 offset = 0;     // where its compressed data starts
        juce::int64 size = 0, compressedSize = 0;
    };

    //==============================================================================
    inline juce::File fileFor (const juce::File& editFile)
    {
        return editFile.getSiblingFile ("." + editFile.getFileNameWithoutExtension() + ".history");
    }

    /** 64-bit FNV-1a of the uncompressed project, to tell whether a version is already kept. */
    inline juce::String hashOf (const void* data, size_t size)
    {
        juce::uint64 h = 14695981039346656037ull;
        auto* p = static_cast<const juce::uint8*> (data);

        for (size_t i = 0; i < size; ++i)
            h = (h ^ p[i]) * 1099511628211ull;

        return juce::String::toHexString ((juce::int64) h).paddedLeft ('0', 16);
    }

    namespace detail
    {
        constexpr int fileMagic = 0x54534845;   // "EHST"
        constexpr int entryMagic = 0x504b4345;  // "ECKP"
        constexpr int version = 1;

        inline juce::String kindName (Kind k)   { return k == Kind::unsaved ? "unsaved" : k == Kind::damaged ? "damaged" : "saved"; }
        inline Kind kindFrom (const juce::String& s)  { return s == "unsaved" ? Kind::unsaved : s == "damaged" ? Kind::damaged : Kind::saved; }

        /** Who saved it, from the project's own header, without parsing it all. */
        inline juce::String savedByIn (const juce::MemoryBlock& content)
        {
            auto head = juce::String::fromUTF8 (static_cast<const char*> (content.getData()), (int) std::min<size_t> (content.getSize(), 4000));

            for (auto attr : { "elsewhereSavedBy=\"", "tandemSavedBy=\"" })
                if (head.contains (attr))
                    return head.fromFirstOccurrenceOf (attr, false, false).upToFirstOccurrenceOf ("\"", false, false)
                               .replace ("&quot;", "\"").replace ("&lt;", "<").replace ("&gt;", ">").replace ("&apos;", "'").replace ("&amp;", "&");

            return {};
        }

        inline juce::Array<Checkpoint> readOne (const juce::File& f)
        {
            juce::Array<Checkpoint> list;
            juce::FileInputStream in (f);

            if (! in.openedOk() || in.readInt() != fileMagic || in.readInt() > version)
                return list;

            const auto total = in.getTotalLength();

            while (in.getPosition() + 8 < total)
            {
                if (in.readInt() != entryMagic)
                    break;

                Checkpoint c;
                c.file = f;
                c.time = juce::Time (in.readInt64());
                c.kind = kindFrom (in.readString());
                c.savedBy = in.readString();
                c.label = in.readString();
                c.hash = in.readString();
                c.size = in.readInt64();
                c.compressedSize = in.readInt64();
                c.offset = in.getPosition();

                if (c.compressedSize < 0 || c.offset + c.compressedSize > total)
                    break;      // cut short

                list.add (c);
                in.setPosition (c.offset + c.compressedSize);
            }

            return list;
        }

        inline void writeEntry (juce::OutputStream& out, const Checkpoint& c, const void* compressed, size_t compressedSize)
        {
            out.writeInt (entryMagic);
            out.writeInt64 (c.time.toMilliseconds());
            out.writeString (kindName (c.kind));
            out.writeString (c.savedBy);
            out.writeString (c.label);
            out.writeString (c.hash);
            out.writeInt64 (c.size);
            out.writeInt64 ((juce::int64) compressedSize);
            out.write (compressed, compressedSize);
        }

        inline juce::MemoryBlock compress (const juce::MemoryBlock& data)
        {
            juce::MemoryOutputStream mo;

            {
                juce::GZIPCompressorOutputStream gz (mo, 9);
                gz.write (data.getData(), data.getSize());
            }

            return mo.getMemoryBlock();
        }

        inline juce::MemoryBlock readCompressed (const Checkpoint& c)
        {
            juce::MemoryBlock block;
            juce::FileInputStream in (c.file);

            if (in.openedOk() && in.setPosition (c.offset))
                in.readIntoMemoryBlock (block, (ssize_t) c.compressedSize);

            return block;
        }
    }

    //==============================================================================
    /** Every checkpoint kept for a project, oldest first. */
    inline juce::Array<Checkpoint> read (const juce::File& editFile)
    {
        auto main = fileFor (editFile);
        auto list = detail::readOne (main);

        // Copies a sync service made of it when two computers wrote at once: what's only there counts too
        auto stem = main.getFileNameWithoutExtension();

        for (auto& other : editFile.getParentDirectory().findChildFiles (juce::File::findFiles, false, stem + "*.history", juce::File::FollowSymlinks::no))
        {
            if (other == main)
                continue;

            for (auto& c : detail::readOne (other))
            {
                bool have = false;

                for (auto& existing : list)
                    have = have || (existing.hash == c.hash && existing.kind == c.kind);

                if (! have)
                    list.add (c);
            }
        }

        std::stable_sort (list.begin(), list.end(), [] (const Checkpoint& a, const Checkpoint& b) { return a.time < b.time; });
        return list;
    }

    /** The project as it was at a checkpoint (empty if it can't be read). */
    inline juce::MemoryBlock load (const Checkpoint& c)
    {
        auto compressed = detail::readCompressed (c);
        juce::MemoryInputStream mi (compressed, false);
        juce::GZIPDecompressorInputStream gz (mi);
        juce::MemoryBlock out;
        gz.readIntoMemoryBlock (out);

        if ((juce::int64) out.getSize() != c.size || hashOf (out.getData(), out.getSize()) != c.hash)
            return {};

        return out;
    }

    /** The project without the engine's note of when it last changed, which moves even when nothing else did. */
    inline juce::String withoutChangeTime (const juce::MemoryBlock& content)
    {
        auto text = content.toString();
        auto at = text.indexOf (" lastSignificantChange=\"");

        if (at < 0)
            return text;

        auto end = text.indexOfChar (at + 25, '"');
        return end < 0 ? text : text.replaceSection (at, end + 1 - at, {});
    }

    /** True if the newest checkpoint of this kind already holds this project, as good as unchanged. */
    inline bool isLatest (const juce::File& editFile, const juce::MemoryBlock& content, Kind kind = Kind::saved)
    {
        auto list = read (editFile);

        for (int i = list.size(); --i >= 0;)
        {
            auto& c = list.getReference (i);

            if (c.kind != kind)
                continue;

            if (c.hash == hashOf (content.getData(), content.getSize()))
                return true;

            auto kept = load (c);
            return ! kept.isEmpty() && withoutChangeTime (kept) == withoutChangeTime (content);
        }

        return false;
    }

    /** True if any checkpoint holds exactly this. */
    inline bool contains (const juce::File& editFile, const juce::String& hash)
    {
        for (auto& c : read (editFile))
            if (c.hash == hash)
                return true;

        return false;
    }

    /** Adds a checkpoint. Returns false if it couldn't be written. */
    inline bool add (const juce::File& editFile, const juce::MemoryBlock& content, Kind kind,
                     juce::Time time = juce::Time::getCurrentTime(), const juce::String& label = {})
    {
        if (content.isEmpty())
            return false;

        auto f = fileFor (editFile);
        Checkpoint c;
        c.time = time;
        c.kind = kind;
        c.savedBy = detail::savedByIn (content);
        c.label = label;
        c.hash = hashOf (content.getData(), content.getSize());
        c.size = (juce::int64) content.getSize();
        auto compressed = detail::compress (content);
        auto existing = detail::readOne (f);

        if (existing.size() >= keep || (f.existsAsFile() && existing.isEmpty() && f.getSize() > 0))
        {
            // Drop the oldest (or start again from what can still be read): rewrite it whole
            juce::TemporaryFile temp (f);

            {
                juce::FileOutputStream out (temp.getFile());

                if (! out.openedOk())
                    return false;

                out.writeInt (detail::fileMagic);
                out.writeInt (detail::version);

                for (int i = std::max (0, existing.size() - (keep - 1)); i < existing.size(); ++i)
                {
                    auto& old = existing.getReference (i);
                    auto data = detail::readCompressed (old);
                    detail::writeEntry (out, old, data.getData(), data.getSize());
                }

                detail::writeEntry (out, c, compressed.getData(), compressed.getSize());
                out.flush();

                if (out.getStatus().failed())
                    return false;
            }

            return temp.overwriteTargetFileWithTemporary();
        }

        // A checkpoint cut short at the end goes: the new one is written in its place
        auto end = existing.isEmpty() ? (juce::int64) 8 : existing.getLast().offset + existing.getLast().compressedSize;
        juce::FileOutputStream out (f);

        if (! out.openedOk())
            return false;

        if (f.getSize() == 0)
        {
            out.writeInt (detail::fileMagic);
            out.writeInt (detail::version);
        }
        else if (out.getPosition() != end)
        {
            out.setPosition (end);
            out.truncate();
        }

        detail::writeEntry (out, c, compressed.getData(), compressed.getSize());
        out.flush();
        return out.getStatus().wasOk();
    }

    inline bool addFile (const juce::File& editFile, const juce::File& source, Kind kind, juce::Time time = juce::Time::getCurrentTime())
    {
        juce::MemoryBlock content;
        return source.loadFileAsData (content) && add (editFile, content, kind, time);
    }

    /** Moves an older project's Backups folder into its history, then to the bin. Leaves it
        where it is if anything in it couldn't be kept. */
    inline void importBackups (const juce::File& editFile)
    {
        auto backups = editFile.getParentDirectory().getChildFile ("Backups");

        if (! backups.isDirectory())
            return;

        struct Old { juce::File file; juce::Time time; Kind kind; };
        std::vector<Old> old;
        auto name = editFile.getFileNameWithoutExtension();

        for (auto& f : backups.findChildFiles (juce::File::findFiles, false))
        {
            auto base = f.getFileNameWithoutExtension();
            auto kind = base.endsWith (" unsaved") ? Kind::unsaved : (base.endsWith (" damaged") || f.hasFileExtension ("damaged")) ? Kind::damaged : Kind::saved;
            auto stamp = base.fromLastOccurrenceOf (name + " ", false, false).upToFirstOccurrenceOf (" unsaved", false, false)
                             .upToFirstOccurrenceOf (" damaged", false, false);

            // "2026-11-12 14-03-55", as the name says; the file's own time if it doesn't
            auto time = f.getLastModificationTime();
            auto parts = juce::StringArray::fromTokens (stamp.replaceCharacter (' ', '-'), "-", {});

            if (parts.size() >= 6)
                time = juce::Time (parts[0].getIntValue(), parts[1].getIntValue() - 1, parts[2].getIntValue(),
                                   parts[3].getIntValue(), parts[4].getIntValue(), parts[5].getIntValue(), 0, true);

            if (f.getFileName().startsWithChar ('.'))
                continue;

            old.push_back ({ f, time, kind });
        }

        std::sort (old.begin(), old.end(), [] (const Old& a, const Old& b) { return a.time < b.time; });
        bool allKept = true;

        for (auto& o : old)
        {
            juce::MemoryBlock content;

            if (! o.file.loadFileAsData (content))
            {
                allKept = false;
                continue;
            }

            auto hash = hashOf (content.getData(), content.getSize());

            if (! contains (editFile, hash) && ! add (editFile, content, o.kind, o.time))
                allKept = false;
        }

        if (allKept)
            backups.moveToTrash();
    }
}

} // namespace elsewhere
