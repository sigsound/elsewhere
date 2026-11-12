// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Aaf.h"

namespace elsewhere::Aaf
{
namespace
{
    juce::uint16 u16 (const void* p)    { return juce::ByteOrder::littleEndianShort (p); }
    juce::uint32 u32 (const void* p)    { return juce::ByteOrder::littleEndianInt (p); }
    juce::uint64 u64 (const void* p)    { return juce::ByteOrder::littleEndianInt64 (p); }

    constexpr juce::uint32 noEntry = 0xffffffff;

    //==============================================================================
    /** Microsoft Compound File Binary (MS-CFB): the "file system inside a file" AAF is stored in. */
    class Cfb
    {
    public:
        struct Entry
        {
            juce::String name;
            int type = 0;                       // 1 storage, 2 stream, 5 root
            std::array<juce::uint8, 16> clsid {};
            juce::uint32 left = noEntry, right = noEntry, child = noEntry, start = 0;
            juce::uint64 size = 0;
        };

        explicit Cfb (const juce::File& f) : file (f)
        {
            juce::FileInputStream in (file);
            juce::uint8 h[512];

            if (! in.openedOk() || in.read (h, 512) != 512 || u64 (h) != 0xe11ab1a1e011cfd0ull)
                return;

            auto major = u16 (h + 0x1a);
            sectorSize = 1u << u16 (h + 0x1e);
            miniSectorSize = 1u << u16 (h + 0x20);
            miniCutoff = u32 (h + 0x38);

            if (sectorSize != 512 && sectorSize != 4096)
                return;

            // The FAT's own sectors are listed in the header, then in a chain of DIFAT sectors
            std::vector<juce::uint32> fatSectors;
            for (int i = 0; i < 109; ++i)
                if (auto s = u32 (h + 0x4c + i * 4); s < 0xfffffffa)
                    fatSectors.push_back (s);

            juce::HeapBlock<juce::uint8> sector (sectorSize);
            auto perSector = sectorSize / 4;

            for (auto s = u32 (h + 0x44), n = u32 (h + 0x48); s < 0xfffffffa && n-- > 0;)
            {
                if (! readSector (in, s, sector))
                    return;

                for (juce::uint32 i = 0; i + 1 < perSector; ++i)
                    if (auto f = u32 (sector + i * 4); f < 0xfffffffa)
                        fatSectors.push_back (f);

                s = u32 (sector + (perSector - 1) * 4);
            }

            for (auto s : fatSectors)
            {
                if (! readSector (in, s, sector))
                    return;

                for (juce::uint32 i = 0; i < perSector; ++i)
                    fat.push_back (u32 (sector + i * 4));
            }

            for (auto s : chain (u32 (h + 0x3c), fat))
            {
                if (! readSector (in, s, sector))
                    return;

                for (juce::uint32 i = 0; i < perSector; ++i)
                    miniFat.push_back (u32 (sector + i * 4));
            }

            for (auto s : chain (u32 (h + 0x30), fat))
            {
                if (! readSector (in, s, sector))
                    return;

                for (juce::uint32 i = 0; i < sectorSize / 128; ++i)
                {
                    auto* d = sector + i * 128;
                    Entry e;
                    auto nameBytes = std::min<int> (u16 (d + 0x40), 64);
                    e.name = juce::String (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (d)),
                                           (size_t) std::max (0, nameBytes / 2 - 1));
                    e.type = d[0x42];
                    std::copy (d + 0x50, d + 0x60, e.clsid.begin());
                    e.left = u32 (d + 0x44);
                    e.right = u32 (d + 0x48);
                    e.child = u32 (d + 0x4c);
                    e.start = u32 (d + 0x74);
                    e.size = major == 3 ? u32 (d + 0x78) : u64 (d + 0x78);
                    entries.push_back (e);
                }
            }

            if (! entries.empty() && entries[0].type == 5)
                miniStream = chain (entries[0].start, fat);
        }

        bool isValid() const                            { return ! entries.empty() && entries[0].type == 5; }
        const Entry& entry (juce::uint32 i) const       { return entries[i]; }

        /** A child of a storage, by name. */
        juce::uint32 find (juce::uint32 storage, const juce::String& name) const
        {
            auto& names = childrenOf (storage);

            if (auto it = names.find (name); it != names.end())
                return it->second;

            for (auto& [n, i] : names)
                if (n.equalsIgnoreCase (name))
                    return i;

            return noEntry;
        }

        std::unique_ptr<juce::InputStream> open (juce::uint32 i) const
        {
            if (i >= entries.size() || entries[i].type != 2)
                return {};

            auto& e = entries[i];
            bool mini = e.size < miniCutoff;
            return std::make_unique<Stream> (*this, chain (e.start, mini ? miniFat : fat), mini, (juce::int64) e.size);
        }

        juce::MemoryBlock read (juce::uint32 i) const
        {
            juce::MemoryBlock m;

            if (auto in = open (i))
                in->readIntoMemoryBlock (m);

            return m;
        }

    private:
        struct Stream : public juce::InputStream
        {
            Stream (const Cfb& c, std::vector<juce::uint32> s, bool m, juce::int64 len)
                : cfb (c), in (c.file), sectors (std::move (s)), mini (m), length (len) {}

            juce::int64 getTotalLength() override       { return length; }
            bool isExhausted() override                 { return pos >= length; }
            juce::int64 getPosition() override          { return pos; }
            bool setPosition (juce::int64 p) override   { pos = juce::jlimit ((juce::int64) 0, length, p); return true; }

            int read (void* dest, int bytes) override
            {
                auto* out = static_cast<char*> (dest);
                int done = 0;
                auto block = (juce::int64) (mini ? cfb.miniSectorSize : cfb.sectorSize);

                while (done < bytes && pos < length)
                {
                    auto index = (size_t) (pos / block);

                    if (index >= sectors.size())
                        break;

                    auto within = pos % block;
                    auto n = (int) std::min<juce::int64> ({ block - within, length - pos, (juce::int64) (bytes - done) });
                    juce::int64 at;

                    if (mini)
                    {
                        auto miniPos = (juce::int64) sectors[index] * block + within;
                        auto big = (size_t) (miniPos / cfb.sectorSize);

                        if (big >= cfb.miniStream.size())
                            break;

                        at = ((juce::int64) cfb.miniStream[big] + 1) * cfb.sectorSize + miniPos % cfb.sectorSize;
                    }
                    else
                    {
                        at = ((juce::int64) sectors[index] + 1) * cfb.sectorSize + within;
                    }

                    if (! in.setPosition (at) || in.read (out + done, n) != n)
                        break;

                    done += n;
                    pos += n;
                }

                return done;
            }

            const Cfb& cfb;
            juce::FileInputStream in;
            std::vector<juce::uint32> sectors;
            bool mini;
            juce::int64 length, pos = 0;
        };

        bool readSector (juce::FileInputStream& in, juce::uint32 s, juce::uint8* dest) const
        {
            return in.setPosition (((juce::int64) s + 1) * sectorSize) && in.read (dest, (int) sectorSize) == (int) sectorSize;
        }

        std::vector<juce::uint32> chain (juce::uint32 start, const std::vector<juce::uint32>& table) const
        {
            std::vector<juce::uint32> result;

            for (auto s = start; s < table.size() && result.size() <= table.size(); s = table[s])
                result.push_back (s);

            return result;
        }

        const std::map<juce::String, juce::uint32>& childrenOf (juce::uint32 storage) const
        {
            auto& names = children[storage];

            if (names.empty() && storage < entries.size())
            {
                // Siblings form a red-black tree under the storage's child
                std::vector<juce::uint32> stack { entries[storage].child };
                std::set<juce::uint32> seen;

                while (! stack.empty())
                {
                    auto i = stack.back();
                    stack.pop_back();

                    if (i >= entries.size() || ! seen.insert (i).second)
                        continue;

                    names[entries[i].name] = i;
                    stack.push_back (entries[i].left);
                    stack.push_back (entries[i].right);
                }
            }

            return names;
        }

        juce::File file;
        juce::uint32 sectorSize = 512, miniSectorSize = 64, miniCutoff = 4096;
        std::vector<juce::uint32> fat, miniFat, miniStream;
        std::vector<Entry> entries;
        mutable std::map<juce::uint32, std::map<juce::String, juce::uint32>> children;
    };

    //==============================================================================
    using Guid = std::array<juce::uint8, 16>;

    /** "0d010101-0101-3500-..." as stored on disk (the first three fields little-endian). */
    Guid guid (const char* text)
    {
        Guid b {};
        juce::String hex = juce::String (text).removeCharacters ("-");

        for (int i = 0; i < 16; ++i)
            b[(size_t) i] = (juce::uint8) hex.substring (i * 2, i * 2 + 2).getHexValue32();

        return { b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15] };
    }

    namespace Class
    {
        const Guid compositionMob = guid ("0d010101-0101-3500-060e-2b3402060101"),
                   masterMob      = guid ("0d010101-0101-3600-060e-2b3402060101"),
                   sourceMob      = guid ("0d010101-0101-3700-060e-2b3402060101"),
                   timelineSlot   = guid ("0d010101-0101-3b00-060e-2b3402060101"),
                   sequence       = guid ("0d010101-0101-0f00-060e-2b3402060101"),
                   sourceClip     = guid ("0d010101-0101-1100-060e-2b3402060101"),
                   filler         = guid ("0d010101-0101-0900-060e-2b3402060101"),
                   transition     = guid ("0d010101-0101-1700-060e-2b3402060101"),
                   operationGroup = guid ("0d010101-0101-0a00-060e-2b3402060101"),
                   selector       = guid ("0d010101-0101-0e00-060e-2b3402060101"),
                   essenceGroup   = guid ("0d010101-0101-0500-060e-2b3402060101"),
                   nestedScope    = guid ("0d010101-0101-0b00-060e-2b3402060101"),
                   constantValue  = guid ("0d010101-0101-3d00-060e-2b3402060101"),
                   varyingValue   = guid ("0d010101-0101-3e00-060e-2b3402060101"),
                   fileDescriptor = guid ("0d010101-0101-2500-060e-2b3402060101"),
                   waveDescriptor = guid ("0d010101-0101-2c00-060e-2b3402060101"),
                   aifcDescriptor = guid ("0d010101-0101-2600-060e-2b3402060101"),
                   soundDescriptor = guid ("0d010101-0101-4200-060e-2b3402060101"),
                   multipleDescriptor = guid ("0d010101-0101-4400-060e-2b3402060101"),
                   networkLocator = guid ("0d010101-0101-3200-060e-2b3402060101");

        /** Child -> parent for the classes we look at, so subclasses written by other tools still match. */
        const std::vector<std::pair<const char*, const char*>> hierarchy {
            { "0d010101-0101-3500-060e-2b3402060101", "0d010101-0101-3400-060e-2b3402060101" },   // CompositionMob : Mob
            { "0d010101-0101-3600-060e-2b3402060101", "0d010101-0101-3400-060e-2b3402060101" },   // MasterMob
            { "0d010101-0101-3700-060e-2b3402060101", "0d010101-0101-3400-060e-2b3402060101" },   // SourceMob
            { "0d010101-0101-3b00-060e-2b3402060101", "0d010101-0101-3800-060e-2b3402060101" },   // TimelineMobSlot : MobSlot
            { "0d010101-0101-0300-060e-2b3402060101", "0d010101-0101-0200-060e-2b3402060101" },   // Segment : Component
            { "0d010101-0101-0f00-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // Sequence : Segment
            { "0d010101-0101-1000-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // SourceReference : Segment
            { "0d010101-0101-1100-060e-2b3402060101", "0d010101-0101-1000-060e-2b3402060101" },   // SourceClip : SourceReference
            { "0d010101-0101-0900-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // Filler
            { "0d010101-0101-1700-060e-2b3402060101", "0d010101-0101-0200-060e-2b3402060101" },   // Transition : Component
            { "0d010101-0101-0a00-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // OperationGroup
            { "0d010101-0101-0e00-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // Selector
            { "0d010101-0101-0500-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // EssenceGroup
            { "0d010101-0101-0b00-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // NestedScope
            { "0d010101-0101-1400-060e-2b3402060101", "0d010101-0101-0300-060e-2b3402060101" },   // Timecode
            { "0d010101-0101-3d00-060e-2b3402060101", "0d010101-0101-3c00-060e-2b3402060101" },   // ConstantValue : Parameter
            { "0d010101-0101-3e00-060e-2b3402060101", "0d010101-0101-3c00-060e-2b3402060101" },   // VaryingValue
            { "0d010101-0101-2500-060e-2b3402060101", "0d010101-0101-2400-060e-2b3402060101" },   // FileDescriptor : EssenceDescriptor
            { "0d010101-0101-2c00-060e-2b3402060101", "0d010101-0101-2500-060e-2b3402060101" },   // WAVEDescriptor
            { "0d010101-0101-2600-060e-2b3402060101", "0d010101-0101-2500-060e-2b3402060101" },   // AIFCDescriptor
            { "0d010101-0101-4200-060e-2b3402060101", "0d010101-0101-2500-060e-2b3402060101" },   // SoundDescriptor
            { "0d010101-0101-4800-060e-2b3402060101", "0d010101-0101-4200-060e-2b3402060101" },   // PCMDescriptor
            { "0d010101-0101-4400-060e-2b3402060101", "0d010101-0101-2500-060e-2b3402060101" },   // MultipleDescriptor
            { "0d010101-0101-3200-060e-2b3402060101", "0d010101-0101-3100-060e-2b3402060101" },   // NetworkLocator : Locator
        };
    }

    namespace Def
    {
        const Guid sound       = guid ("01030202-0200-0000-060e-2b3404010101"),
                   legacySound = guid ("78e1ebe1-6cef-11d2-807d-006008143e6f"),
                   monoGain    = guid ("9d2ea894-0968-11d3-8a38-0050040ef7d2"),
                   stereoGain  = guid ("9d2ea895-0968-11d3-8a38-0050040ef7d2"),
                   monoPan     = guid ("9d2ea893-0968-11d3-8a38-0050040ef7d2"),
                   level       = guid ("e4962320-2267-11d3-8a4c-0050040ef7d2"),
                   amplitude   = guid ("e4962321-2267-11d3-8a4c-0050040ef7d2"),
                   pan         = guid ("e4962322-2267-11d3-8a4c-0050040ef7d2"),
                   topLevel    = guid ("0d010102-0101-0700-060e-2b3404010101"),
                   rational    = guid ("03010100-0000-0000-060e-2b3401040101"),
                   int32       = guid ("01010700-0000-0000-060e-2b3401040101"),
                   uint32      = guid ("01010300-0000-0000-060e-2b3401040101"),
                   int64       = guid ("01010800-0000-0000-060e-2b3401040101");
    }

    // Property IDs (AAF Object Specification)
    namespace Pid
    {
        enum : juce::uint16
        {
            rootMetaDictionary = 0x0001, rootHeader = 0x0002,
            metaClassDefinitions = 0x0003, metaIdentification = 0x0005, metaParentClass = 0x0008,
            headerContent = 0x3b03, headerIdentifications = 0x3b06,
            productName = 0x3c02,
            contentMobs = 0x1901, contentEssenceData = 0x1902,
            mobId = 0x4401, mobName = 0x4402, mobSlots = 0x4403, mobUsage = 0x4408,
            sourceMobDescriptor = 0x4701,
            slotId = 0x4801, slotName = 0x4802, slotSegment = 0x4803,
            editRate = 0x4b01, origin = 0x4b02,
            dataDefinition = 0x0201, length = 0x0202,
            components = 0x1001,
            sourceId = 0x1101, sourceSlotId = 0x1102,
            startTime = 0x1201, fadeInLength = 0x1202, fadeOutLength = 0x1204,
            operation = 0x0b01, inputSegments = 0x0b02, parameters = 0x0b03,
            selected = 0x0f01, choices = 0x0501, nestedSlots = 0x0c01,
            parameterDefinition = 0x4c01, constantValue = 0x4d01, pointList = 0x4e02, pointValue = 0x1a02,
            locators = 0x2f01, sampleRate = 0x3001, fileDescriptors = 0x3f01,
            waveSummary = 0x3801, aifcSummary = 0x3101,
            quantizationBits = 0x3d01, audioSamplingRate = 0x3d03, channels = 0x3d07,
            url = 0x4001,
            essenceMobId = 0x2701, essenceData = 0x2702
        };
    }

    enum Form : juce::uint16
    {
        data = 0x82, dataStream = 0x42, strongRef = 0x22, strongRefVector = 0x32, strongRefSet = 0x3a,
        weakRef = 0x02, weakRefStoredId = 0x03
    };

    //==============================================================================
    /** One AAF object: a storage holding a "properties" stream, and its children. */
    struct Object
    {
        juce::uint32 dir = noEntry;
        Guid cls {};
        std::map<juce::uint16, std::pair<juce::uint16, juce::MemoryBlock>> props;   // pid -> (stored form, bytes)

        const juce::MemoryBlock* get (juce::uint16 pid) const
        {
            auto it = props.find (pid);
            return it != props.end() ? &it->second.second : nullptr;
        }

        juce::uint16 formOf (juce::uint16 pid) const
        {
            auto it = props.find (pid);
            return it != props.end() ? it->second.first : 0;
        }
    };

    juce::String utf16 (const juce::MemoryBlock& m, size_t from = 0)
    {
        if (m.getSize() <= from + 1)
            return {};

        auto* p = reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (static_cast<const char*> (m.getData()) + from);
        auto chars = (m.getSize() - from) / 2;
        size_t n = 0;

        while (n < chars && p[n] != 0)
            ++n;

        return juce::String (juce::CharPointer_UTF16 (p), n);
    }

    class File
    {
    public:
        explicit File (const juce::File& f) : cfb (f)
        {
            if (! cfb.isValid())
                return;

            root = load (0);

            for (auto& [child, parent] : Class::hierarchy)
                parents[guid (child)] = guid (parent);

            // Classes other tools added (Avid's, for example) are described in the meta dictionary
            if (auto* meta = ref (*root, Pid::rootMetaDictionary))
                for (auto* c : refs (*meta, Pid::metaClassDefinitions))
                    if (auto id = auid (*c, Pid::metaIdentification))
                        if (auto parent = weakKey (*c, Pid::metaParentClass); parent && *parent != *id)
                            parents.insert ({ *id, *parent });
        }

        Cfb cfb;
        Object* root = nullptr;

        Object* load (juce::uint32 dir)
        {
            if (dir == noEntry || dir >= 0x7fffffff)
                return nullptr;

            if (auto it = objects.find (dir); it != objects.end())
                return it->second.get();

            auto obj = std::make_unique<Object>();
            obj->dir = dir;
            obj->cls = cfb.entry (dir).clsid;

            auto bytes = cfb.read (cfb.find (dir, "properties"));
            auto* p = static_cast<const juce::uint8*> (bytes.getData());

            if (bytes.getSize() >= 4 && p[0] == 0x4c)
            {
                auto count = u16 (p + 2);
                size_t at = 4 + (size_t) count * 6;

                for (int i = 0; i < count && 4 + (size_t) (i + 1) * 6 <= bytes.getSize(); ++i)
                {
                    auto* e = p + 4 + i * 6;
                    auto size = u16 (e + 4);

                    if (at + size > bytes.getSize())
                        break;

                    obj->props[u16 (e)] = { u16 (e + 2), juce::MemoryBlock (p + at, size) };
                    at += size;
                }
            }

            auto* raw = obj.get();
            objects[dir] = std::move (obj);
            return raw;
        }

        bool isA (const Object& o, const Guid& cls) const
        {
            auto c = o.cls;

            for (int depth = 0; depth < 32; ++depth)
            {
                if (c == cls)
                    return true;

                auto it = parents.find (c);

                if (it == parents.end())
                    return false;

                c = it->second;
            }

            return false;
        }

        //==============================================================================
        Object* ref (const Object& o, juce::uint16 pid)
        {
            if (o.formOf (pid) != strongRef)
                return nullptr;

            return load (cfb.find (o.dir, utf16 (*o.get (pid))));
        }

        /** A strong reference vector or set, in order. */
        std::vector<Object*> refs (const Object& o, juce::uint16 pid)
        {
            std::vector<Object*> result;
            auto form = o.formOf (pid);

            if (form != strongRefVector && form != strongRefSet)
                return result;

            auto name = utf16 (*o.get (pid));
            auto index = cfb.read (cfb.find (o.dir, name + " index"));
            auto* p = static_cast<const juce::uint8*> (index.getData());

            if (index.getSize() < 12)
                return result;

            auto count = u32 (p);
            size_t at = 12, stride = 4;

            if (form == strongRefSet)
            {
                if (index.getSize() < 15)
                    return result;

                stride = 8 + p[14];
                at = 15;
            }

            for (juce::uint32 i = 0; i < count && at + 4 <= index.getSize(); ++i, at += stride)
                if (auto* child = load (cfb.find (o.dir, name + "{" + juce::String::toHexString ((int) u32 (p + at)) + "}")))
                    result.push_back (child);

            return result;
        }

        std::optional<Guid> weakKey (const Object& o, juce::uint16 pid) const
        {
            auto form = o.formOf (pid);
            auto* m = o.get (pid);

            if ((form != weakRef && form != weakRefStoredId) || m->getSize() < 5 + 16)
                return {};

            auto* p = static_cast<const juce::uint8*> (m->getData());

            if (p[4] != 16)
                return {};

            Guid g;
            std::copy (p + 5, p + 21, g.begin());
            return g;
        }

        static std::optional<Guid> auid (const Object& o, juce::uint16 pid)
        {
            auto* m = o.get (pid);

            if (m == nullptr || m->getSize() < 16)
                return {};

            Guid g;
            std::copy (static_cast<const juce::uint8*> (m->getData()), static_cast<const juce::uint8*> (m->getData()) + 16, g.begin());
            return g;
        }

        static std::optional<juce::int64> integer (const Object& o, juce::uint16 pid)
        {
            auto* m = o.get (pid);

            if (m == nullptr)
                return {};

            auto* p = m->getData();

            switch (m->getSize())
            {
                case 1:  return (juce::int64) *static_cast<const juce::int8*> (p);
                case 2:  return (juce::int64) (juce::int16) u16 (p);
                case 4:  return (juce::int64) (juce::int32) u32 (p);
                case 8:  return (juce::int64) u64 (p);
                default: return {};
            }
        }

        static double rational (const Object& o, juce::uint16 pid, double fallback)
        {
            auto* m = o.get (pid);

            if (m == nullptr || m->getSize() != 8)
                return fallback;

            auto num = (juce::int32) u32 (m->getData()), den = (juce::int32) u32 (static_cast<const char*> (m->getData()) + 4);
            return den != 0 && num != 0 ? (double) num / den : fallback;
        }

        static juce::String string (const Object& o, juce::uint16 pid)
        {
            auto* m = o.get (pid);
            return m != nullptr ? utf16 (*m) : juce::String();
        }

        static juce::String mobId (const Object& o, juce::uint16 pid)
        {
            auto* m = o.get (pid);
            return m != nullptr && m->getSize() == 32 ? juce::String::toHexString (m->getData(), 32, 0) : juce::String();
        }

        /** A Rational, Int32 or Int64 stored as an "indirect" value: byte order, type, value. */
        static std::optional<double> indirect (const Object& o, juce::uint16 pid)
        {
            auto* m = o.get (pid);

            if (m == nullptr || m->getSize() < 17)
                return {};

            auto* p = static_cast<const juce::uint8*> (m->getData());
            Guid type;
            std::copy (p + 1, p + 17, type.begin());
            auto size = m->getSize() - 17;

            if (type == Def::rational && size == 8)
            {
                auto den = (juce::int32) u32 (p + 21);
                return den != 0 ? (double) (juce::int32) u32 (p + 17) / den : 0.0;
            }

            if ((type == Def::int32 || type == Def::uint32) && size == 4)
                return (double) (type == Def::int32 ? (juce::int32) u32 (p + 17) : u32 (p + 17));

            if (type == Def::int64 && size == 8)
                return (double) (juce::int64) u64 (p + 17);

            return {};
        }

        /** The stream a data-stream property points at (inside the object's storage). */
        juce::uint32 stream (const Object& o, juce::uint16 pid) const
        {
            auto* m = o.get (pid);

            if (m == nullptr || o.formOf (pid) != dataStream || m->getSize() < 3)
                return noEntry;

            return cfb.find (o.dir, utf16 (*m, 1));
        }

    private:
        std::map<juce::uint32, std::unique_ptr<Object>> objects;
        std::map<Guid, Guid> parents;
    };

    //==============================================================================
    /** A WAV header for raw PCM (16/24/32-bit integer, little-endian). */
    juce::MemoryBlock wavHeader (int channels, double rate, int bits, juce::int64 dataBytes)
    {
        juce::MemoryOutputStream h;
        auto blockAlign = channels * bits / 8;
        h.write ("RIFF", 4);
        h.writeInt ((int) std::min<juce::int64> (0xffffffffll, 36 + dataBytes));
        h.write ("WAVEfmt ", 8);
        h.writeInt (16);
        h.writeShort (1);
        h.writeShort ((short) channels);
        h.writeInt ((int) rate);
        h.writeInt ((int) rate * blockAlign);
        h.writeShort ((short) blockAlign);
        h.writeShort ((short) bits);
        h.write ("data", 4);
        h.writeInt ((int) std::min<juce::int64> (0xffffffffll, dataBytes));
        return h.getMemoryBlock();
    }
}

//==============================================================================
struct Importer::Impl
{
    explicit Impl (const juce::File& f) : source (f), aaf (f) {}

    juce::File source;
    File aaf;
    juce::String error, applicationName;

    struct Essence
    {
        juce::String name, clipName;            // the file source mob's name; the first clip's that uses it
        juce::uint32 stream = noEntry;          // embedded
        juce::File external;
        bool rawPcm = false;
        int channels = 1, bits = 16;
        double rate = 48000.0;
        juce::String missingName;               // set if it can't be found
        juce::File local;                       // the copy in the Elsewhere project
    };

    struct Clip
    {
        double start, end, sourceStart, gainDb = 0.0, fadeIn = 0.0, fadeOut = 0.0;
        juce::String essence, name;
    };

    struct Track
    {
        juce::String name;
        std::vector<Clip> clips;
        double volumeDb = 0.0;
        std::optional<double> pan;              // -1..1
    };

    std::map<juce::String, Object*> mobs;
    std::map<juce::String, Object*> essenceData;
    std::map<juce::String, Essence> essences;   // by file source mob ID
    std::vector<Track> tracks;
    std::map<juce::String, juce::File> nearbyFiles;
    juce::StringArray missing;
    bool flattenedAutomation = false, unsupportedMedia = false;

    //==============================================================================
    void read()
    {
        if (! aaf.cfb.isValid() || aaf.root == nullptr)
        {
            error = "\"" + source.getFileName() + "\" isn't an AAF file, or it's damaged.";
            return;
        }

        auto* header = aaf.ref (*aaf.root, Pid::rootHeader);
        auto* content = header != nullptr ? aaf.ref (*header, Pid::headerContent) : nullptr;

        if (content == nullptr)
        {
            error = "\"" + source.getFileName() + "\" couldn't be read: it has no content.";
            return;
        }

        // The most recent tool to save the file is listed last
        for (auto* id : aaf.refs (*header, Pid::headerIdentifications))
            if (auto name = File::string (*id, Pid::productName); name.isNotEmpty())
                applicationName = name;

        for (auto* m : aaf.refs (*content, Pid::contentMobs))
            if (auto id = File::mobId (*m, Pid::mobId); id.isNotEmpty())
                mobs[id] = m;

        for (auto* e : aaf.refs (*content, Pid::contentEssenceData))
            if (auto id = File::mobId (*e, Pid::essenceMobId); id.isNotEmpty())
                essenceData[id] = e;

        auto* composition = findComposition();

        if (composition == nullptr)
        {
            error = "\"" + source.getFileName() + "\" doesn't contain a sequence (composition) to import.";
            return;
        }

        for (auto* slot : aaf.refs (*composition, Pid::mobSlots))
        {
            if (! aaf.isA (*slot, Class::timelineSlot))
                continue;

            auto* segment = aaf.ref (*slot, Pid::slotSegment);

            if (segment == nullptr || ! isSound (*segment))
                continue;

            Track t;
            t.name = File::string (*slot, Pid::slotName);

            if (t.name.isEmpty())
                t.name = "Audio " + juce::String ((int) tracks.size() + 1);

            auto rate = File::rational (*slot, Pid::editRate, 48000.0);
            walk (*segment, 0.0, rate, 0.0, t, true);
            tracks.push_back (std::move (t));
        }

        if (tracks.empty())
            error = "\"" + source.getFileName() + "\" has no audio tracks.";
    }

    /** The top-level sequence: marked as such, or else the one with the most audio tracks. */
    Object* findComposition()
    {
        Object* best = nullptr;
        int bestSlots = -1;

        for (auto& [id, m] : mobs)
        {
            if (! aaf.isA (*m, Class::compositionMob))
                continue;

            if (File::auid (*m, Pid::mobUsage) == Def::topLevel)
                return m;

            int sound = 0;
            for (auto* slot : aaf.refs (*m, Pid::mobSlots))
                if (auto* seg = aaf.ref (*slot, Pid::slotSegment); seg != nullptr && isSound (*seg))
                    ++sound;

            if (sound > bestSlots)
            {
                best = m;
                bestSlots = sound;
            }
        }

        return best;
    }

    bool isSound (const Object& component) const
    {
        auto def = aaf.weakKey (component, Pid::dataDefinition);
        return def == Def::sound || def == Def::legacySound;
    }

    //==============================================================================
    /** Lays out a segment starting at `position` (edit units). Operation groups wrapping a whole
        track are its fader and pan; inside a sequence they're clip gain. */
    void walk (Object& seg, double position, double rate, double gainDb, Track& track, bool wholeTrack)
    {
        auto length = (double) File::integer (seg, Pid::length).value_or (0);

        if (aaf.isA (seg, Class::sequence))
        {
            double at = position, pendingFadeIn = 0.0;
            int lastClip = -1;

            for (auto* c : aaf.refs (seg, Pid::components))
            {
                auto len = (double) File::integer (*c, Pid::length).value_or (0);

                // A transition overlaps the end of the previous clip and the start of the next
                if (aaf.isA (*c, Class::transition))
                {
                    at -= len;

                    if (lastClip >= 0)
                        track.clips[(size_t) lastClip].fadeOut = len / rate;

                    pendingFadeIn = len / rate;
                    continue;
                }

                auto before = track.clips.size();
                walk (*c, at, rate, gainDb, track, false);

                if (track.clips.size() > before)
                {
                    auto& first = track.clips[before];
                    first.fadeIn = std::max (first.fadeIn, pendingFadeIn);
                    lastClip = (int) track.clips.size() - 1;
                }
                else
                {
                    lastClip = -1;
                }

                pendingFadeIn = 0.0;
                at += len;
            }
        }
        else if (aaf.isA (seg, Class::sourceClip))
        {
            auto sourceMob = File::mobId (seg, Pid::sourceId);
            auto slot = (juce::uint32) File::integer (seg, Pid::sourceSlotId).value_or (0);
            auto start = (double) File::integer (seg, Pid::startTime).value_or (0);

            if (sourceMob.isEmpty() || sourceMob.containsOnly ("0") || length <= 0.0)
                return;

            juce::String essence;
            double sourceSeconds = 0.0;

            if (! resolve (sourceMob, slot, start / rate, essence, sourceSeconds, 0))
            {
                if (auto it = mobs.find (sourceMob); it != mobs.end())
                    missing.addIfNotAlreadyThere (File::string (*it->second, Pid::mobName));
                return;
            }

            Clip c { position / rate, (position + length) / rate, sourceSeconds, gainDb };
            c.essence = essence;

            if (auto it = mobs.find (sourceMob); it != mobs.end())
                c.name = File::string (*it->second, Pid::mobName);

            if (auto& e = essences[essence]; e.clipName.isEmpty())
                e.clipName = c.name;

            if (auto f = File::integer (seg, Pid::fadeInLength))   c.fadeIn = (double) *f / rate;
            if (auto f = File::integer (seg, Pid::fadeOutLength))  c.fadeOut = (double) *f / rate;

            track.clips.push_back (c);
        }
        else if (aaf.isA (seg, Class::operationGroup))
        {
            auto inputs = aaf.refs (seg, Pid::inputSegments);
            auto op = aaf.weakKey (seg, Pid::operation);

            if (op == Def::monoGain || op == Def::stereoGain)
            {
                if (auto g = parameter (seg, { Def::amplitude, Def::level }); g && *g > 0.0)
                {
                    auto db = juce::Decibels::gainToDecibels (*g, -100.0);

                    if (wholeTrack)
                        track.volumeDb += db;
                    else
                        gainDb += db;
                }
            }
            else if (op == Def::monoPan && wholeTrack)
            {
                if (auto p = parameter (seg, { Def::pan }))
                    track.pan = juce::jlimit (-1.0, 1.0, *p * 2.0 - 1.0);
            }

            // Effects we don't know still play their (first) input
            if (! inputs.empty())
                walk (*inputs.front(), position, rate, gainDb, track, wholeTrack);
        }
        else if (aaf.isA (seg, Class::selector))
        {
            if (auto* selected = aaf.ref (seg, Pid::selected))
                walk (*selected, position, rate, gainDb, track, wholeTrack);
        }
        else if (aaf.isA (seg, Class::essenceGroup))
        {
            if (auto choices = aaf.refs (seg, Pid::choices); ! choices.empty())
                walk (*choices.front(), position, rate, gainDb, track, wholeTrack);
        }
        else if (aaf.isA (seg, Class::nestedScope))
        {
            if (auto slots = aaf.refs (seg, Pid::nestedSlots); ! slots.empty())
                walk (*slots.back(), position, rate, gainDb, track, wholeTrack);
        }
    }

    /** A parameter's value: constant, or the first point of an automation curve. */
    std::optional<double> parameter (Object& group, std::initializer_list<Guid> which)
    {
        for (auto* p : aaf.refs (group, Pid::parameters))
        {
            auto def = File::auid (*p, Pid::parameterDefinition);

            if (! def || std::find (which.begin(), which.end(), *def) == which.end())
                continue;

            if (aaf.isA (*p, Class::constantValue))
                return File::indirect (*p, Pid::constantValue);

            if (aaf.isA (*p, Class::varyingValue))
            {
                auto points = aaf.refs (*p, Pid::pointList);

                if (points.size() > 1)
                    flattenedAutomation = true;

                if (! points.empty())
                    return File::indirect (*points.front(), Pid::pointValue);
            }
        }

        return {};
    }

    //==============================================================================
    /** Follows a reference through master and source mobs to a file's audio. `seconds` is
        measured from the referenced slot's origin. */
    bool resolve (const juce::String& id, juce::uint32 slotId, double seconds, juce::String& essence, double& sourceSeconds, int depth)
    {
        auto it = mobs.find (id);

        if (it == mobs.end() || depth > 16)
            return false;

        auto* mob = it->second;
        Object* slot = nullptr;

        for (auto* s : aaf.refs (*mob, Pid::mobSlots))
            if ((juce::uint32) File::integer (*s, Pid::slotId).value_or (0) == slotId)
                slot = s;

        auto rate = slot != nullptr ? File::rational (*slot, Pid::editRate, 48000.0) : 48000.0;
        auto origin = slot != nullptr ? (double) File::integer (*slot, Pid::origin).value_or (0) : 0.0;

        // A file source mob: this is the audio
        if (aaf.isA (*mob, Class::sourceMob))
        {
            if (auto* descriptor = aaf.ref (*mob, Pid::sourceMobDescriptor); descriptor != nullptr && aaf.isA (*descriptor, Class::fileDescriptor))
            {
                if (! addEssence (id, *mob, *descriptor))
                    return false;

                essence = id;
                sourceSeconds = seconds + origin / rate;
                return true;
            }
        }

        if (slot == nullptr)
            return false;

        // Find the clip at that point of the slot and carry on through it
        auto* seg = aaf.ref (*slot, Pid::slotSegment);
        auto position = seconds * rate + origin;

        for (int guard = 0; seg != nullptr && guard < 64; ++guard)
        {
            if (aaf.isA (*seg, Class::sourceClip))
            {
                auto next = File::mobId (*seg, Pid::sourceId);
                auto nextSlot = (juce::uint32) File::integer (*seg, Pid::sourceSlotId).value_or (0);
                auto start = (double) File::integer (*seg, Pid::startTime).value_or (0);

                if (next.isEmpty() || next.containsOnly ("0"))
                    return false;

                return resolve (next, nextSlot, (start + position) / rate, essence, sourceSeconds, depth + 1);
            }

            if (aaf.isA (*seg, Class::sequence))
            {
                Object* found = nullptr;
                double at = 0.0;

                for (auto* c : aaf.refs (*seg, Pid::components))
                {
                    auto len = (double) File::integer (*c, Pid::length).value_or (0);

                    if (aaf.isA (*c, Class::transition))
                    {
                        at -= len;
                        continue;
                    }

                    if (position < at + len)
                    {
                        found = c;
                        position -= at;
                        break;
                    }

                    at += len;
                }

                seg = found;
            }
            else if (aaf.isA (*seg, Class::operationGroup))
            {
                auto inputs = aaf.refs (*seg, Pid::inputSegments);
                seg = inputs.empty() ? nullptr : inputs.front();
            }
            else if (aaf.isA (*seg, Class::essenceGroup))
            {
                auto choices = aaf.refs (*seg, Pid::choices);
                seg = choices.empty() ? nullptr : choices.front();
            }
            else if (aaf.isA (*seg, Class::selector))
            {
                seg = aaf.ref (*seg, Pid::selected);
            }
            else
            {
                return false;
            }
        }

        return false;
    }

    bool addEssence (const juce::String& id, Object& mob, Object& descriptor)
    {
        if (auto it = essences.find (id); it != essences.end())
            return it->second.missingName.isEmpty();

        Essence e;
        e.name = File::string (mob, Pid::mobName);

        // Several files in one: use the first audio one
        Object* d = &descriptor;

        if (aaf.isA (descriptor, Class::multipleDescriptor))
            for (auto* sub : aaf.refs (descriptor, Pid::fileDescriptors))
                if (aaf.isA (*sub, Class::soundDescriptor) || aaf.isA (*sub, Class::waveDescriptor) || aaf.isA (*sub, Class::aifcDescriptor))
                {
                    d = sub;
                    break;
                }

        e.rate = File::rational (*d, Pid::audioSamplingRate, File::rational (*d, Pid::sampleRate, 48000.0));
        e.channels = (int) File::integer (*d, Pid::channels).value_or (1);
        e.bits = (int) File::integer (*d, Pid::quantizationBits).value_or (16);

        // A WAVE summary is the file's header: read the format from it
        if (auto* summary = d->get (Pid::waveSummary); summary != nullptr && summary->getSize() >= 36
             && std::memcmp (summary->getData(), "RIFF", 4) == 0)
        {
            auto* p = static_cast<const juce::uint8*> (summary->getData());
            e.channels = u16 (p + 22);
            e.rate = (double) u32 (p + 24);
            e.bits = u16 (p + 34);
        }

        if (auto data = essenceData.find (id); data != essenceData.end())
        {
            e.stream = aaf.stream (*data->second, Pid::essenceData);

            if (auto in = aaf.cfb.open (e.stream))
            {
                char magic[4] {};
                in->read (magic, 4);
                e.rawPcm = std::memcmp (magic, "RIFF", 4) != 0 && std::memcmp (magic, "FORM", 4) != 0;

                // Raw AIFC sample data is big-endian, which our WAV header can't describe
                if (e.rawPcm && aaf.isA (*d, Class::aifcDescriptor))
                {
                    e.missingName = e.name + " (AIFF data)";
                    unsupportedMedia = true;
                }
            }
        }
        else
        {
            juce::StringArray urls;

            for (auto* loc : aaf.refs (descriptor, Pid::locators))
                if (aaf.isA (*loc, Class::networkLocator))
                    urls.add (File::string (*loc, Pid::url));

            for (auto& url : urls)
            {
                auto path = juce::URL::removeEscapeChars (url.fromFirstOccurrenceOf ("file://", false, true));

                if (path.startsWithIgnoreCase ("localhost/"))
                    path = path.substring (9);

                // file:///C:/... on Windows
                if (path.length() > 3 && path[0] == '/' && path[2] == ':')
                    path = path.substring (1);

                auto name = path.fromLastOccurrenceOf ("/", false, false).fromLastOccurrenceOf ("\\", false, false);

                if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
                    e.external = juce::File (path);
                else
                    e.external = SessionImport::findFileNear (source.getParentDirectory(), name, nearbyFiles);

                if (e.external.existsAsFile())
                {
                    if (e.external.hasFileExtension ("mxf"))
                    {
                        unsupportedMedia = true;
                        e.missingName = e.external.getFileName();
                    }

                    break;
                }

                e.missingName = name;
            }

            if (urls.isEmpty())
                e.missingName = e.name.isNotEmpty() ? e.name : "audio file";
        }

        if (e.missingName.isNotEmpty())
            missing.addIfNotAlreadyThere (e.missingName);

        auto ok = e.missingName.isEmpty();
        essences[id] = e;
        return ok;
    }

    //==============================================================================
    bool extract (const juce::File& dest, const std::function<bool (double)>& progress)
    {
        dest.createDirectory();
        juce::int64 total = 0, done = 0;

        for (auto& [id, e] : essences)
        {
            if (e.missingName.isNotEmpty())
                continue;

            if (e.stream != noEntry)
                total += (juce::int64) aaf.cfb.entry (e.stream).size;
            else
                total += e.external.getSize();
        }

        juce::HeapBlock<char> buffer (1 << 20);

        for (auto& [id, e] : essences)
        {
            if (e.missingName.isNotEmpty())
                continue;

            // Files keep their own names; embedded audio is named after its clip
            auto base = juce::File::createLegalFileName (e.stream == noEntry ? e.external.getFileNameWithoutExtension()
                                                                             : e.clipName.isNotEmpty() ? e.clipName : e.name);
            std::unique_ptr<juce::InputStream> in;
            juce::String ext;

            if (e.stream != noEntry)
            {
                in = aaf.cfb.open (e.stream);
                char magic[4] {};

                if (in != nullptr)
                {
                    in->read (magic, 4);
                    in->setPosition (0);
                }

                ext = std::memcmp (magic, "FORM", 4) == 0 ? ".aif" : ".wav";
            }
            else
            {
                in = e.external.createInputStream();
                ext = e.external.getFileExtension();
            }

            if (in == nullptr)
                continue;

            if (base.isEmpty())
                base = "Audio";

            auto local = dest.getNonexistentChildFile (base, ext, false);
            juce::FileOutputStream out (local);

            if (! out.openedOk())
                continue;

            if (e.rawPcm)
            {
                auto header = wavHeader (e.channels, e.rate, e.bits, in->getTotalLength());
                out.write (header.getData(), header.getSize());
            }

            for (int n; (n = in->read (buffer, 1 << 20)) > 0;)
            {
                out.write (buffer, (size_t) n);
                done += n;

                if (progress && ! progress (total > 0 ? (double) done / (double) total : 1.0))
                    return false;
            }

            out.flush();
            e.local = local;
        }

        return true;
    }

    //==============================================================================
    SessionImport::Summary build (te::Edit& edit)
    {
        SessionImport::Summary summary;
        summary.applicationName = applicationName;
        summary.missingFiles = missing;

        SessionImport::removePlaceholderTracks (edit);

        te::Track* preceding = nullptr;
        juce::Array<juce::File> used;

        for (size_t i = 0; i < tracks.size(); ++i)
        {
            auto& t = tracks[i];
            auto at = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr);

            if (at == nullptr)
                continue;

            preceding = at.get();
            ++summary.tracks;

            // Mono tracks of a stereo pair: pan them apart (by the file names) unless the AAF says
            auto pan = t.pan;

            if (! pan && ! t.clips.empty())
            {
                std::optional<double> side;
                bool consistent = true;

                for (auto& c : t.clips)
                {
                    auto s = SessionImport::stereoSideFromName (essences[c.essence].local.getFileName());
                    consistent = consistent && s.has_value() && (! side || *side == *s);
                    side = s;
                }

                if (consistent)
                    pan = side;
            }

            auto name = t.name;
            bool sameAsNeighbour = (i > 0 && tracks[i - 1].name == t.name) || (i + 1 < tracks.size() && tracks[i + 1].name == t.name);

            if (sameAsNeighbour && pan && std::abs (*pan) > 0.99)
                name << (*pan < 0 ? " L" : " R");

            at->setName (name);
            at->setColour (EngineHelpers::trackColourFor (te::getAudioTracks (edit).indexOf (at.get())));

            if (auto* volume = at->getVolumePlugin())
            {
                volume->setVolumeDb ((float) t.volumeDb);

                if (pan)
                    volume->setPan ((float) *pan);
            }

            for (auto& c : t.clips)
            {
                auto& e = essences[c.essence];

                if (! e.local.existsAsFile())
                    continue;

                used.addIfNotAlreadyThere (e.local);

                if (auto clip = SessionImport::addAudioClip (*at, c.name.isNotEmpty() ? c.name : e.local.getFileNameWithoutExtension(),
                                                              e.local, c.start, c.end, c.sourceStart))
                {
                    clip->setGainDB ((float) c.gainDb);
                    clip->setFadeIn (te::TimeDuration::fromSeconds (std::min (c.fadeIn, c.end - c.start)));
                    clip->setFadeOut (te::TimeDuration::fromSeconds (std::min (c.fadeOut, c.end - c.start)));
                    ++summary.audioClips;
                }
            }
        }

        SessionImport::finish (edit, {}, used, summary);

        summary.notes.add ("AAF files don't include the tempo. Set it in the transport bar: the audio stays where it is.");

        if (flattenedAutomation)
            summary.notes.add ("Volume and pan automation was set to where it starts.");

        if (unsupportedMedia)
            summary.notes.add ("Some media is in a format Elsewhere can't read yet (MXF, or raw AIFF data). Export the AAF with WAV audio instead.");

        summary.notes.add ("Plug-ins and sends aren't brought across.");
        return summary;
    }
};

//==============================================================================
Importer::Importer (const juce::File& f) : impl (std::make_unique<Impl> (f))
{
    impl->read();
}

Importer::~Importer() = default;

juce::String Importer::getError() const     { return impl->error; }

bool Importer::extractAudio (const juce::File& destDir, std::function<bool (double)> progress)
{
    return impl->error.isEmpty() && impl->extract (destDir, progress);
}

SessionImport::Summary Importer::build (te::Edit& edit)
{
    return impl->build (edit);
}

} // namespace elsewhere::Aaf
