/*
 * ptformat_c - a plain C entry point to libptformat, for Elsewhere
 *
 * Copyright (C) 2026 Fuzz LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */
#define PTFORMAT_C_BUILD 1
#include "ptformat_c.h"
#include "ptformat/ptformat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
    void appendString (std::string& out, const std::string& s)
    {
        out += '"';

        for (unsigned char c : s)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20)
                    {
                        char buf[8];
                        std::snprintf (buf, sizeof (buf), "\\u%04x", c);
                        out += buf;
                    }
                    else
                    {
                        out += (char) c;    // names are passed through as UTF-8
                    }
            }
        }

        out += '"';
    }

    void appendKey (std::string& out, const char* key)
    {
        appendString (out, key);
        out += ':';
    }

    void appendNumber (std::string& out, long long v)
    {
        out += std::to_string (v);
    }
}

extern "C" int ptformat_c_abi_version (void)
{
    return PTFORMAT_C_ABI_VERSION;
}

extern "C" char* ptformat_c_load (const char* path, int* error)
{
    PTFFormat ptf;

    // Positions are scaled to the rate asked for: ask for the session's own, so they're exact
    int result = ptf.load (path, 48000);

    if (result == 0 && ptf.sessionrate() > 0 && ptf.sessionrate() != 48000)
        result = ptf.load (path, ptf.sessionrate());

    if (error != nullptr)
        *error = result;

    if (result != 0)
        return nullptr;

    std::string out = "{";
    appendKey (out, "version");       appendNumber (out, ptf.version());       out += ',';
    appendKey (out, "sessionRate");   appendNumber (out, ptf.sessionrate());   out += ',';

    appendKey (out, "audioFiles");
    out += '[';
    bool first = true;

    for (auto& w : ptf.audiofiles())
    {
        if (! first) out += ',';
        first = false;
        out += '{';
        appendKey (out, "index");   appendNumber (out, w.index);     out += ',';
        appendKey (out, "name");    appendString (out, w.filename);  out += ',';
        appendKey (out, "length");  appendNumber (out, w.length);
        out += '}';
    }

    out += "],";
    appendKey (out, "tracks");
    out += '[';
    first = true;

    for (auto& t : ptf.tracks())
    {
        if (! first) out += ',';
        first = false;
        out += '{';
        appendKey (out, "index");   appendNumber (out, t.index);               out += ',';
        appendKey (out, "name");    appendString (out, t.name);                out += ',';
        appendKey (out, "region");  appendString (out, t.reg.name);            out += ',';
        appendKey (out, "file");    appendString (out, t.reg.wave.filename);   out += ',';
        appendKey (out, "start");   appendNumber (out, t.reg.startpos);        out += ',';
        appendKey (out, "offset");  appendNumber (out, t.reg.sampleoffset);    out += ',';
        appendKey (out, "length");  appendNumber (out, t.reg.length);
        out += '}';
    }

    out += "],";
    appendKey (out, "midiTracks");
    out += '[';
    first = true;

    for (auto& t : ptf.miditracks())
    {
        if (! first) out += ',';
        first = false;
        out += '{';
        appendKey (out, "index");   appendNumber (out, t.index);          out += ',';
        appendKey (out, "name");    appendString (out, t.name);           out += ',';
        appendKey (out, "region");  appendString (out, t.reg.name);       out += ',';
        appendKey (out, "start");   appendNumber (out, t.reg.startpos);   out += ',';
        appendKey (out, "notes");
        out += '[';

        bool firstNote = true;
        for (auto& n : t.reg.midi)
        {
            if (! firstNote) out += ',';
            firstNote = false;
            out += '[';
            appendNumber (out, (long long) n.pos);     out += ',';
            appendNumber (out, (long long) n.length);  out += ',';
            appendNumber (out, n.note);                out += ',';
            appendNumber (out, n.velocity);
            out += ']';
        }

        out += "]}";
    }

    out += "]}";

    auto* result_json = static_cast<char*> (std::malloc (out.size() + 1));

    if (result_json != nullptr)
        std::memcpy (result_json, out.c_str(), out.size() + 1);

    return result_json;
}

extern "C" void ptformat_c_free (char* json)
{
    std::free (json);
}
