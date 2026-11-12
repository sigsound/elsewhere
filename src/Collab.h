// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Project.h"

namespace elsewhere
{
//==============================================================================
/**
    Collaboration over a shared folder (Dropbox, iCloud Drive, Google Drive...).

    Instead of two people saving the same .elsewhere file (which makes cloud services create
    "conflicted copies"), every change you make is recorded as a small operation and
    appended to *your own* log file in the project folder:

        My Song Project/Collab/ops/<your-id>.jsonl
        My Song Project/Collab/presence/<your-id>.json

    Each person only ever writes their own files, so the cloud service never sees a
    conflict. Elsewhere reads the other logs as they sync in and applies those operations
    to your open project, so both copies converge while you both work.

    Operations are ValueTree edits (set property / add child / remove child / move child)
    addressed by item IDs, merged with last-writer-wins using Lamport clocks. The same
    operations can later be sent over a network connection for live sessions (Phase 3).
*/
namespace Collab
{
    //==============================================================================
    struct Identity
    {
        juce::String userId, name;
        juce::Colour colour;

        /** The short tag used in file names, e.g. recordings. */
        juce::String getShortTag() const    { return userId.substring (0, 6); }

        static juce::Colour colourFor (const juce::String& userId)
        {
            static const juce::uint32 palette[] = { 0xffff6b6b, 0xff4fb3ff, 0xff9ad64b, 0xffe06cd8, 0xfff3d34a, 0xff4cc9a6, 0xffff9a3c, 0xff8f7cff };
            return juce::Colour (palette[(size_t) (userId.hashCode64() & 0x7fffffff) % std::size (palette)]);
        }

        static Identity load (juce::PropertiesFile& props)
        {
            Identity id;
            id.userId = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_USER_ID", props.getValue ("collabUserId"));

            if (id.userId.isEmpty())
            {
                id.userId = juce::Uuid().toDashedString().removeCharacters ("-").substring (0, 16);
                props.setValue ("collabUserId", id.userId);
            }

            auto defaultName = juce::SystemStats::getFullUserName().upToFirstOccurrenceOf (" ", false, false);
            id.name = juce::SystemStats::getEnvironmentVariable ("ELSEWHERE_USER_NAME", props.getValue ("collabName", defaultName));
            id.colour = colourFor (id.userId);
            return id;
        }
    };

    inline const juce::Identifier collabEnabledProp { "elsewhereCollab" };
    inline const juce::Identifier collabClockProp   { "elsewhereCollabClock" };

    inline const juce::Identifier cidProp           { "cid" };   // string IDs for Elsewhere's own items (comments)

    inline bool isEnabled (const te::Edit& edit)    { return (bool) edit.state[collabEnabledProp]; }

    //==============================================================================
    /** What another collaborator is doing, read from their presence file. */
    struct Presence
    {
        juce::String userId, name;
        juce::Colour colour;
        te::EditItemID focusedTrack;
        double playheadSeconds = 0.0;
        bool playing = false;
        juce::Time lastSeen;

        bool isOnline() const   { return juce::Time::getCurrentTime() - lastSeen < juce::RelativeTime::seconds (30.0); }
    };

    //==============================================================================
    class Session : private juce::ValueTree::Listener,
                    private juce::Timer
    {
    public:
        Session (Project& p, Identity me) : project (p), edit (p.getEdit()), identity (std::move (me))
        {
            if (isEnabled (edit))
                start();
        }

        ~Session() override
        {
            stop();
        }

        const Identity& getIdentity() const         { return identity; }
        bool isActive() const                       { return active; }

        /** True while a collaborator's changes are being applied (not a local edit). */
        bool isApplyingRemote() const               { return applyingRemote; }

        /** Turns collaboration on for this project (stored in the project, so it syncs). */
        void enable()
        {
            if (active)
                return;

            edit.state.setProperty (collabEnabledProp, true, nullptr);
            start();
            flush();
            project.save();
        }

        /** Returns the people who have this project open right now. */
        std::vector<Presence> getOthersOnline() const
        {
            std::vector<Presence> result;

            for (auto& [uid, p] : others)
                if (p.isOnline())
                    result.push_back (p);

            return result;
        }

        /** Our own presence details, published to the others. */
        te::EditItemID focusedTrack;

        /** Called on the message thread after remote changes were applied. */
        std::function<void()> onRemoteChanges;

        /** Writes pending ops and records how far each log has been applied (call before saving). */
        void prepareForSave()
        {
            if (! active)
                return;

            flush();

            juce::DynamicObject::Ptr clock = new juce::DynamicObject();
            for (auto& [uid, seq] : appliedSeq)
                clock->setProperty (juce::Identifier (uid), (juce::int64) seq);

            // Not sent as an op: every user writes their own view of the clock
            juce::ScopedValueSetter<bool> svs (applyingRemote, true);
            edit.state.setProperty (collabClockProp, juce::JSON::toString (juce::var (clock.get()), true), nullptr);
        }

        juce::File getFolder() const        { return project.getFolder().getChildFile ("Collab"); }

        /** Writes our own copy of the project, which only we ever write (see Project::chooseFileToLoad). */
        void writeSnapshot()
        {
            if (! active)
                return;

            auto folder = getFolder().getChildFile ("snapshots");
            folder.createDirectory();

            if (auto xml = edit.state.createXml())
                xml->writeTo (folder.getChildFile (identity.userId + project.getEditFile().getFileExtension()));
        }

        //==============================================================================
        // Exposed for the self-tests
        int getNumPendingRemote() const     { return (int) pendingRemote.size(); }
        void pollNow()                      { readRemoteOps(); retryPending(); }
        void flushNow()                     { flush(); }

    private:
        //==============================================================================
        struct Op
        {
            juce::String user;
            juce::int64 seq = 0, lamport = 0;
            juce::var data;             // { k: "set"|"add"|"remove"|"move", p: path, ... }
            juce::int64 firstTried = 0; // ms, for ops waiting on something that hasn't synced yet
        };

        struct LastWrite { juce::int64 lamport = 0; juce::String user; };

        Project& project;
        te::Edit& edit;
        Identity identity;
        bool active = false, applyingRemote = false;

        juce::int64 lamport = 0, mySeq = 0;
        std::map<juce::String, juce::int64> appliedSeq;       // highest seq applied per user
        std::map<juce::String, juce::int64> readOffsets;      // bytes read per remote log
        std::map<juce::String, LastWrite> lastWrites;         // LWW register per node+property
        std::map<juce::String, juce::var> lastRemoteValues;   // to avoid echoing values we just received
        std::map<juce::String, LastWrite> insertStamps;       // when each item was inserted (for ordering)
        std::map<juce::String, Presence> others;

        juce::Array<juce::var> pendingLocal;
        std::map<int, juce::ValueTree> pendingSubtrees;   // index in pendingLocal -> added subtree
        std::vector<Op> pendingRemote;
        int tick = 0;

        //==============================================================================
        juce::File getOpsFolder() const             { return getFolder().getChildFile ("ops"); }
        juce::File getPresenceFolder() const        { return getFolder().getChildFile ("presence"); }
        juce::File getMyLog() const                 { return getOpsFolder().getChildFile (identity.userId + ".jsonl"); }

        void start()
        {
            active = true;
            getOpsFolder().createDirectory();
            getPresenceFolder().createDirectory();

            claimIDRange();

            // Where the saved file is up to, per user
            if (auto clock = juce::JSON::parse (edit.state[collabClockProp].toString()); auto* obj = clock.getDynamicObject())
                for (auto& prop : obj->getProperties())
                    appliedSeq[prop.name.toString()] = (juce::int64) prop.value;

            // Replay everything newer than the saved file, including our own unsaved changes
            readRemoteOps (true);
            retryPending();

            mySeq = appliedSeq[identity.userId];

            edit.state.addListener (this);
            startTimer (250);
            writePresence();
        }

        void stop()
        {
            if (! active)
                return;

            flush();
            stopTimer();
            edit.state.removeListener (this);
            getPresenceFolder().getChildFile (identity.userId + ".json").deleteFile();
            active = false;
        }

        /** Each collaborator allocates new item IDs from their own range, so things created
            at the same time on two machines can never get the same ID. */
        void claimIDRange()
        {
            const juce::uint64 slot = 1 + ((juce::uint64) (identity.userId.hashCode64() & 0x7fffffff) % 4000);
            const juce::uint64 base = slot << 40, top = base + (1ull << 40);
            juce::uint64 highest = base;

            for (auto id : te::EditItemID::findAllIDs (edit.state))
                if (id.getRawID() >= base && id.getRawID() < top)
                    highest = std::max (highest, id.getRawID());

            edit.setNextItemIDRaw (highest + 1);
        }

        //==============================================================================
        // Addressing nodes: a path of steps from the edit root. Items with an "id" are
        // found by ID, anything else by its index among siblings of the same type.
        static juce::var stepFor (const juce::ValueTree& node)
        {
            juce::DynamicObject::Ptr step = new juce::DynamicObject();
            step->setProperty ("t", node.getType().toString());

            if (node.hasProperty (te::IDs::id))
            {
                step->setProperty ("id", node[te::IDs::id]);
            }
            else if (node.hasProperty (cidProp))
            {
                step->setProperty ("cid", node[cidProp]);
            }
            else
            {
                auto parent = node.getParent();
                int n = 0;

                for (int i = 0; i < parent.getNumChildren(); ++i)
                {
                    auto c = parent.getChild (i);
                    if (c == node) break;
                    if (c.getType() == node.getType()) ++n;
                }

                step->setProperty ("n", n);
            }

            return juce::var (step.get());
        }

        juce::var pathTo (juce::ValueTree node) const
        {
            juce::Array<juce::var> steps;

            while (node.isValid() && node != edit.state)
            {
                steps.insert (0, stepFor (node));
                node = node.getParent();
            }

            if (node != edit.state)
                return {};   // not part of this edit

            return steps;
        }

        static juce::ValueTree findChild (const juce::ValueTree& parent, const juce::var& step)
        {
            juce::Identifier type (step["t"].toString());

            if (step.hasProperty ("id"))
            {
                auto id = step["id"].toString();

                for (int i = 0; i < parent.getNumChildren(); ++i)
                    if (auto c = parent.getChild (i); c.hasType (type) && c[te::IDs::id].toString() == id)
                        return c;

                return {};
            }

            if (step.hasProperty ("cid"))
            {
                auto cid = step["cid"].toString();

                for (int i = 0; i < parent.getNumChildren(); ++i)
                    if (auto c = parent.getChild (i); c.hasType (type) && c[cidProp].toString() == cid)
                        return c;

                return {};
            }

            int n = (int) step["n"];

            for (int i = 0; i < parent.getNumChildren(); ++i)
                if (auto c = parent.getChild (i); c.hasType (type) && n-- == 0)
                    return c;

            return {};
        }

        juce::ValueTree resolve (const juce::var& path) const
        {
            auto node = edit.state;

            if (auto* steps = path.getArray())
                for (auto& step : *steps)
                    if (! (node = findChild (node, step)).isValid())
                        return {};

            return node;
        }

        /** Identifies an item (anything with an engine ID or an Elsewhere cid) across machines. */
        static juce::String itemKey (const juce::ValueTree& node)
        {
            if (node.hasProperty (te::IDs::id))   return node.getType().toString() + ":" + node[te::IDs::id].toString();
            if (node.hasProperty (cidProp))       return node.getType().toString() + ":" + node[cidProp].toString();
            return {};
        }

        // Values are written with their type and full precision, so a fader at 0.5488116145133972
        // arrives as exactly that (JSON would round it)
        static juce::var encodeValue (const juce::var& v)
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();

            if (v.isDouble())        { o->setProperty ("d", juce::String::formatted ("%.17g", (double) v)); }
            else if (v.isInt64())    { o->setProperty ("l", juce::String ((juce::int64) v)); }
            else if (v.isInt())      { o->setProperty ("i", (int) v); }
            else if (v.isBool())     { o->setProperty ("b", (bool) v); }
            else                     { o->setProperty ("s", v.toString()); }

            return juce::var (o.get());
        }

        static juce::var decodeValue (const juce::var& e)
        {
            if (e.hasProperty ("d"))  return e["d"].toString().getDoubleValue();
            if (e.hasProperty ("l"))  return e["l"].toString().getLargeIntValue();
            if (e.hasProperty ("i"))  return (int) e["i"];
            if (e.hasProperty ("b"))  return (bool) e["b"];
            return e["s"].toString();
        }

        /** Subtrees are sent as XML, which stores every value as text; parsing it back gives
            the same strings the engine itself would read from a saved file. */
        static juce::String encodeTree (const juce::ValueTree& v)
        {
            return v.toXmlString (juce::XmlElement::TextFormat().singleLine());
        }

        static juce::String keyFor (const juce::var& path, const juce::String& prop)
        {
            return juce::JSON::toString (path, true) + "#" + prop;
        }

        //==============================================================================
        // What not to sync: things that are per-machine or per-session
        static bool isExcludedTree (juce::ValueTree v)
        {
            for (; v.isValid(); v = v.getParent())
            {
                if (v.hasType (te::IDs::INPUTDEVICES) || v.hasType (te::IDs::TRANSPORT)
                     || (v.hasType (te::IDs::PLUGIN) && v[te::IDs::type].toString() == "elsewhereLiveTap"))   // per-machine
                    return true;

                // A drum section's notes are derived from its settings: every machine generates
                // them itself (deterministically), so only the settings are shared. Otherwise two
                // people changing a section at once would merge both sets of notes.
                if (v.hasType (te::IDs::SEQUENCE) && (bool) v.getParent().getProperty ("tdSection", false))
                    return true;
            }

            return false;
        }

        /** Child types that only ever appear once under their parent. */
        static bool isSingletonType (const juce::Identifier& type)
        {
            static const juce::StringArray singletons { "MODIFIERS", "MODIFIERASSIGNMENTS", "MACROPARAMETERS", "CLIPSLOTS", "OUTPUTDEVICES",
                                                        "SEQUENCE", "LOOPINFO", "WARPTIME", "EFFECTS", "PATTERNGENERATOR", "ELSEWHERE_COMMENTS" };
            return singletons.contains (type.toString());
        }

        static bool isExcludedProperty (const juce::Identifier& prop)
        {
            static const juce::StringArray excluded { "lastSignificantChange", "modifiedBy", "windowX", "windowY", "windowLocked",
                                                      "elsewhereCollabClock", "creationTime", "appVersion", "elsewhereSavedBy",
                                                      "playSlotClips" };   // Session vs Arrangement playback is per performer
            return excluded.contains (prop.toString());
        }

        //==============================================================================
        // Capturing local changes
        void addLocal (juce::DynamicObject::Ptr op)
        {
            // Coalesce rapid changes to the same property (e.g. dragging a fader)
            if (op->getProperty ("k") == "set" && ! pendingLocal.isEmpty())
            {
                auto& last = pendingLocal.getReference (pendingLocal.size() - 1);

                if (last["k"] == "set" && last["key"] == op->getProperty ("key"))
                {
                    last = juce::var (op.get());
                    return;
                }
            }

            pendingLocal.add (juce::var (op.get()));
        }

        void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& prop) override
        {
            if (applyingRemote || isExcludedProperty (prop) || isExcludedTree (v))
                return;

            auto path = pathTo (v);
            if (path.isVoid())
                return;

            auto key = keyFor (path, prop.toString());
            auto value = v.getProperty (prop);

            // Don't bounce back a value we just received from someone else
            if (auto it = lastRemoteValues.find (key); it != lastRemoteValues.end() && it->second == value)
                return;

            juce::DynamicObject::Ptr op = new juce::DynamicObject();
            op->setProperty ("k", "set");
            op->setProperty ("p", path);
            op->setProperty ("prop", prop.toString());
            op->setProperty ("key", key);

            if (v.hasProperty (prop))
                op->setProperty ("v", encodeValue (value));
            else
                op->setProperty ("del", true);

            addLocal (op);
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
        {
            if (applyingRemote || isExcludedTree (child))
                return;

            auto path = pathTo (parent);
            if (path.isVoid())
                return;

            juce::DynamicObject::Ptr op = new juce::DynamicObject();
            op->setProperty ("k", "add");
            op->setProperty ("p", path);
            op->setProperty ("i", parent.indexOf (child));

            // Position relative to the previous sibling, so concurrent inserts order consistently
            auto index = parent.indexOf (child);
            if (index > 0)
                op->setProperty ("after", stepFor (parent.getChild (index - 1)));

            if (auto key = itemKey (child); key.isNotEmpty())
                op->setProperty ("item", key);
            // The XML is written at flush time: the engine often fills in new items (e.g. a track's
            // clip slots) right after adding them, and those contents must travel with the item
            addLocal (op);
            pendingSubtrees[pendingLocal.size() - 1] = child;
        }

        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int index) override
        {
            if (applyingRemote || isExcludedTree (parent))
                return;

            auto path = pathTo (parent);
            if (path.isVoid())
                return;

            juce::DynamicObject::Ptr step = new juce::DynamicObject();
            step->setProperty ("t", child.getType().toString());

            if (child.hasProperty (te::IDs::id))
            {
                step->setProperty ("id", child[te::IDs::id]);
            }
            else if (child.hasProperty (cidProp))
            {
                step->setProperty ("cid", child[cidProp]);
            }
            else
            {
                // Index among same-type siblings at the time it was removed
                int n = 0;
                for (int i = 0; i < index && i < parent.getNumChildren(); ++i)
                    if (parent.getChild (i).hasType (child.getType()))
                        ++n;
                step->setProperty ("n", n);
            }

            juce::DynamicObject::Ptr op = new juce::DynamicObject();
            op->setProperty ("k", "remove");
            op->setProperty ("p", path);
            op->setProperty ("c", juce::var (step.get()));
            addLocal (op);
        }

        void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int newIndex) override
        {
            if (applyingRemote || isExcludedTree (parent))
                return;

            auto path = pathTo (parent);
            auto child = parent.getChild (newIndex);

            if (path.isVoid() || ! child.isValid())
                return;

            juce::DynamicObject::Ptr op = new juce::DynamicObject();
            op->setProperty ("k", "move");
            op->setProperty ("p", path);
            op->setProperty ("c", stepFor (child));
            op->setProperty ("i", newIndex);

            if (newIndex > 0)
                op->setProperty ("after", stepFor (parent.getChild (newIndex - 1)));

            addLocal (op);
        }

        //==============================================================================
        void flush()
        {
            if (pendingLocal.isEmpty())
                return;

            juce::FileOutputStream out (getMyLog());   // appends

            if (! out.openedOk())
                return;

            for (int opIndex = 0; opIndex < pendingLocal.size(); ++opIndex)
            {
                auto& opData = pendingLocal.getReference (opIndex);

                // Added items are described as they are now: the engine often assigns IDs and
                // fills in contents just after adding them
                if (auto it = pendingSubtrees.find (opIndex); it != pendingSubtrees.end())
                {
                    if (auto* obj = opData.getDynamicObject())
                    {
                        auto& child = it->second;
                        obj->setProperty ("xml", encodeTree (child));

                        if (auto key = itemKey (child); key.isNotEmpty())
                            obj->setProperty ("item", key);

                        auto parent = child.getParent();
                        auto index = parent.isValid() ? parent.indexOf (child) : -1;

                        if (index > 0)
                            obj->setProperty ("after", stepFor (parent.getChild (index - 1)));
                    }
                }

                ++lamport;
                ++mySeq;

                juce::DynamicObject::Ptr line = new juce::DynamicObject();
                line->setProperty ("u", identity.userId);
                line->setProperty ("s", mySeq);
                line->setProperty ("t", lamport);
                line->setProperty ("o", opData);

                if (opData["k"] == "set")
                    lastWrites[opData["key"].toString()] = { lamport, identity.userId };
                else if (opData["k"] == "add" && opData["item"].toString().isNotEmpty())
                    insertStamps[opData["item"].toString()] = { lamport, identity.userId };

                out << juce::JSON::toString (juce::var (line.get()), true) << "\n";
            }

            out.flush();
            appliedSeq[identity.userId] = mySeq;
            pendingLocal.clear();
            pendingSubtrees.clear();
        }

        //==============================================================================
        // Reading and applying other people's changes
        void readRemoteOps (bool includeOwnLog = false)
        {
            // Our own pending changes must be stamped first, so concurrent inserts from others
            // are ordered against them the same way on every machine
            if (! includeOwnLog)
                flush();

            std::vector<Op> incoming;

            for (auto f : getOpsFolder().findChildFiles (juce::File::findFiles, false, "*.jsonl"))
            {
                auto user = f.getFileNameWithoutExtension();

                if (user == identity.userId && ! includeOwnLog)
                    continue;

                // Only read new bytes, and only whole lines (the file may be mid-sync)
                auto& offset = readOffsets[user];
                auto size = f.getSize();

                if (size <= offset)
                    continue;

                juce::FileInputStream in (f);
                if (! in.openedOk())
                    continue;

                in.setPosition (offset);
                auto chunk = in.readString();
                auto lastNewline = chunk.lastIndexOfChar ('\n');

                if (lastNewline < 0)
                    continue;

                offset += (juce::int64) chunk.substring (0, lastNewline + 1).getNumBytesAsUTF8();

                for (auto& line : juce::StringArray::fromLines (chunk.substring (0, lastNewline)))
                {
                    auto json = juce::JSON::parse (line);

                    if (! json.isObject())
                        continue;

                    Op op { json["u"].toString(), (juce::int64) json["s"], (juce::int64) json["t"], json["o"] };

                    if (op.seq > appliedSeq[op.user])
                        incoming.push_back (std::move (op));

                    lamport = std::max (lamport, (juce::int64) json["t"]);
                }
            }

            // Apply in causal order across users
            std::sort (incoming.begin(), incoming.end(), [] (const Op& a, const Op& b)
            {
                return a.lamport != b.lamport ? a.lamport < b.lamport : a.user < b.user;
            });

            if (incoming.empty())
                return;

            bool anyApplied = false;

            {
                juce::ScopedValueSetter<bool> svs (applyingRemote, true);

                for (auto& op : incoming)
                {
                    if (apply (op))
                    {
                        anyApplied = true;
                        appliedSeq[op.user] = std::max (appliedSeq[op.user], op.seq);
                    }
                    else
                    {
                        // Something it refers to hasn't arrived yet (different files sync at different times)
                        op.firstTried = juce::Time::currentTimeMillis();
                        pendingRemote.push_back (op);
                    }
                }
            }

            if (anyApplied && onRemoteChanges)
                onRemoteChanges();
        }

        void retryPending()
        {
            if (pendingRemote.empty())
                return;

            auto now = juce::Time::currentTimeMillis();
            std::vector<Op> stillWaiting;
            bool anyApplied = false;

            {
                juce::ScopedValueSetter<bool> svs (applyingRemote, true);

                for (auto& op : pendingRemote)
                {
                    if (apply (op))
                    {
                        anyApplied = true;
                        appliedSeq[op.user] = std::max (appliedSeq[op.user], op.seq);
                    }
                    else if (now - op.firstTried < 60000)
                    {
                        stillWaiting.push_back (op);
                    }
                    else
                    {
                        // Refers to something that no longer exists (e.g. deleted meanwhile)
                        appliedSeq[op.user] = std::max (appliedSeq[op.user], op.seq);
                    }
                }
            }

            pendingRemote = std::move (stillWaiting);

            if (anyApplied && onRemoteChanges)
                onRemoteChanges();
        }

        /** Returns false if the op can't be applied yet (its target doesn't exist). */
        bool apply (const Op& op)
        {
            auto& d = op.data;
            auto kind = d["k"].toString();
            auto target = resolve (d["p"]);

            if (! target.isValid())
                return false;

            if (kind == "set")
            {
                auto key = d["key"].toString();
                auto& lw = lastWrites[key];

                // Last writer wins: ignore changes older than what we already have
                if (op.lamport < lw.lamport || (op.lamport == lw.lamport && op.user < lw.user))
                    return true;

                lw = { op.lamport, op.user };
                juce::Identifier prop (d["prop"].toString());

                if (d["del"])
                {
                    target.removeProperty (prop, nullptr);
                }
                else
                {
                    auto value = decodeValue (d["v"]);
                    lastRemoteValues[key] = value;
                    target.setProperty (prop, value, nullptr);
                }

                return true;
            }

            if (kind == "add")
            {
                auto child = Legacy::upgradeIdentifiers (juce::ValueTree::fromXml (d["xml"].toString()));

                if (! child.isValid())
                    return true;   // malformed: skip

                // Already there? Items with IDs match by ID. For ID-less nodes (the engine
                // sometimes adds these itself, e.g. MODIFIERS), skip identical duplicates.
                for (int i = 0; i < target.getNumChildren(); ++i)
                {
                    auto existing = target.getChild (i);

                    if (! existing.hasType (child.getType()))
                        continue;

                    if (child.hasProperty (te::IDs::id) || child.hasProperty (cidProp))
                    {
                        if ((child.hasProperty (te::IDs::id) && existing[te::IDs::id] == child[te::IDs::id])
                             || (child.hasProperty (cidProp) && existing[cidProp] == child[cidProp]))
                            return true;
                    }
                    else if (existing.isEquivalentTo (child) || isSingletonType (child.getType()))
                    {
                        return true;
                    }
                }

                // Insert after the same sibling it followed on the other machine. If several
                // items were inserted there concurrently, newer inserts go first (by Lamport
                // time, then user ID) so every copy ends up with the same order.
                int pos = juce::jlimit (0, target.getNumChildren(), (int) d["i"]);

                if (d.hasProperty ("after"))
                {
                    auto after = findChild (target, d["after"]);
                    pos = after.isValid() ? target.indexOf (after) + 1 : pos;
                }
                else if (d.hasProperty ("item"))
                {
                    pos = 0;
                }

                if (d.hasProperty ("item"))
                {
                    while (pos < target.getNumChildren())
                    {
                        auto it = insertStamps.find (itemKey (target.getChild (pos)));

                        if (it == insertStamps.end())
                            break;

                        bool existingIsNewer = it->second.lamport > op.lamport
                                                || (it->second.lamport == op.lamport && it->second.user > op.user);
                        if (! existingIsNewer)
                            break;

                        ++pos;
                    }

                    insertStamps[d["item"].toString()] = { op.lamport, op.user };
                }

                target.addChild (child, pos, nullptr);
                return true;
            }

            if (kind == "remove")
            {
                auto child = findChild (target, d["c"]);

                if (child.isValid())
                    target.removeChild (child, nullptr);

                return true;   // already gone is fine
            }

            if (kind == "move")
            {
                auto child = findChild (target, d["c"]);

                if (! child.isValid())
                    return true;

                // Move to just after the same sibling as on the other machine
                int newIndex = 0;

                if (d.hasProperty ("after"))
                {
                    auto after = findChild (target, d["after"]);

                    if (! after.isValid() || after == child)
                        return true;

                    newIndex = target.indexOf (after) + 1;
                }

                auto current = target.indexOf (child);

                if (current < newIndex)
                    --newIndex;

                if (current != newIndex)
                    target.moveChild (current, juce::jlimit (0, target.getNumChildren() - 1, newIndex), nullptr);

                return true;
            }

            return true;
        }

        //==============================================================================
        // Presence
        void writePresence()
        {
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty ("name", identity.name);
            o->setProperty ("colour", identity.colour.toString());
            o->setProperty ("track", focusedTrack.toVar());
            o->setProperty ("playhead", edit.getTransport().getPosition().inSeconds());
            o->setProperty ("playing", edit.getTransport().isPlaying());
            o->setProperty ("time", juce::Time::currentTimeMillis());
            getPresenceFolder().getChildFile (identity.userId + ".json").replaceWithText (juce::JSON::toString (juce::var (o.get())));
        }

        void readPresence()
        {
            for (auto f : getPresenceFolder().findChildFiles (juce::File::findFiles, false, "*.json"))
            {
                auto uid = f.getFileNameWithoutExtension();

                if (uid == identity.userId)
                    continue;

                auto json = juce::JSON::parse (f.loadFileAsString());

                if (! json.isObject())
                    continue;

                Presence p;
                p.userId = uid;
                p.name = json["name"].toString();
                p.colour = juce::Colour::fromString (json["colour"].toString());
                p.focusedTrack = te::EditItemID::fromVar (json["track"]);
                p.playheadSeconds = (double) json["playhead"];
                p.playing = (bool) json["playing"];
                p.lastSeen = juce::Time ((juce::int64) json["time"]);
                others[uid] = p;
            }
        }

        //==============================================================================
        /** Audio files can arrive (via the cloud service) after the operation that uses them.
            Once a missing file shows up, tell the engine so the clip starts playing. */
        std::set<te::EditItemID> clipsWaitingForMedia;

        void refreshArrivedMedia()
        {
            bool changed = false;

            juce::Array<te::Clip*> clips;

            for (auto ct : te::getClipTracks (edit))
            {
                clips.addArray (ct->getClips());

                if (auto at = dynamic_cast<te::AudioTrack*> (ct))
                    for (auto slot : at->getClipSlotList().getClipSlots())
                        if (auto c = slot->getClip())
                            clips.add (c);
            }

            for (auto clip : clips)
            {
                auto ac = dynamic_cast<te::AudioClipBase*> (clip);

                if (ac == nullptr)
                    continue;

                auto file = ac->getOriginalFile();
                bool exists = file.existsAsFile();

                if (! exists)
                {
                    clipsWaitingForMedia.insert (ac->itemID);
                }
                else if (clipsWaitingForMedia.erase (ac->itemID) > 0)
                {
                    te::AudioFile af (edit.engine, file);
                    edit.engine.getAudioFileManager().checkFileForChanges (af);
                    ac->sourceMediaChanged();
                    changed = true;
                }
            }

            if (changed)
            {
                edit.restartPlayback();

                if (onRemoteChanges)
                    onRemoteChanges();
            }
        }

        //==============================================================================
        void timerCallback() override
        {
            ++tick;
            flush();                            // every 250 ms

            if (tick % 4 == 0)                  // every second
            {
                readRemoteOps();
                retryPending();
            }

            if (tick % 12 == 0)                 // every 3 seconds
            {
                writePresence();
                readPresence();
                refreshArrivedMedia();
            }
        }
    };
}

} // namespace elsewhere
