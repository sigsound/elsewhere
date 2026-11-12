/*
 * ptformat_c - a plain C entry point to libptformat, for Elsewhere
 *
 * Copyright (C) 2026 Fuzz LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * It's compiled into the same shared library as libptformat
 * (libs/ptformat, LGPL-2.1+), so that library can be rebuilt or replaced
 * independently of Elsewhere. Only C strings cross the library boundary.
 */
#ifndef PTFORMAT_C_H
#define PTFORMAT_C_H

#if defined(_WIN32)
 #if defined(PTFORMAT_C_BUILD)
  #define PTFORMAT_C_API __declspec(dllexport)
 #else
  #define PTFORMAT_C_API __declspec(dllimport)
 #endif
#else
 #define PTFORMAT_C_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped if the JSON layout changes incompatibly. */
#define PTFORMAT_C_ABI_VERSION 1
PTFORMAT_C_API int ptformat_c_abi_version (void);

/* Reads a Pro Tools session (.ptx, .ptf, .pts, .pt5) and describes it as UTF-8 JSON:

     { "version": 12, "sessionRate": 48000,
       "audioFiles": [ { "index", "name", "length" } ],
       "tracks":     [ { "index", "name", "region", "file", "start", "offset", "length" } ],
       "midiTracks": [ { "index", "name", "region", "start", "notes" } ] }

   Positions and lengths are in samples at the session rate; MIDI note times are
   in Pro Tools ticks. Returns NULL on failure, with *error set to libptformat's
   load() result (-1 decrypt, -2 version, -3 unsupported version, -4 parse).
   Free the result with ptformat_c_free. */
PTFORMAT_C_API char* ptformat_c_load (const char* utf8Path, int* error);

PTFORMAT_C_API void ptformat_c_free (char* json);

#ifdef __cplusplus
}
#endif

#endif
