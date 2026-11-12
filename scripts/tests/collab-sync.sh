#!/bin/bash
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
# Two-person collaboration test. Two Elsewhere instances ("Alice" and "Bob") each open their own
# copy of a project, kept in sync by a fake cloud service that copies the change logs every 1.5 s
# and audio files only every ~4.5 s (so audio arrives after the edits that use it). Both edit at
# the same time, including conflicting edits. Passes if both end up with identical projects.
#
# Usage: scripts/tests/collab-sync.sh <project folder containing a .elsewhere with audio> [Debug|Release]
set -uo pipefail
cd "$(dirname "$0")/../.."
SRC="${1:?project folder}"; CONFIG="${2:-Release}"
if [ "$CONFIG" = "Debug" ]; then APP=build-xcode/Elsewhere_artefacts/Debug/Elsewhere.app/Contents/MacOS/Elsewhere
else APP=build/Elsewhere_artefacts/Release/Elsewhere.app/Contents/MacOS/Elsewhere; fi
APP="$PWD/$APP"
T=$(mktemp -d); mkdir -p "$T/a" "$T/b" "$T/out"
NAME=$(basename "$SRC"); cp -R "$SRC" "$T/a/" || { echo "Couldn't copy $SRC"; exit 1; }; PA="$T/a/$NAME"; rm -rf "$PA/.elsewhere-lock" "$PA/Collab"
EDIT=$(ls "$PA"/*.elsewhere | head -1); grep -q 'elsewhereCollab="1"' "$EDIT" || sed -i '' 's/<EDIT /<EDIT elsewhereCollab="1" /' "$EDIT"
cp -R "$PA" "$T/b/"; PB="$T/b/$NAME"; EDITB="$PB/$(basename "$EDIT")"
NEW=$(ls "$PA"/Samples/Imported/*.wav | head -1); cp "$NEW" "$T/newstem.wav"
sync_dir() { rsync -a --update "$1/" "$2/" 2>/dev/null; }
( for i in $(seq 1 40); do sleep 1.5
    sync_dir "$PA/Collab" "$PB/Collab"; sync_dir "$PB/Collab" "$PA/Collab"
    if [ $((i % 3)) -eq 0 ]; then sync_dir "$PA/Samples" "$PB/Samples"; sync_dir "$PB/Samples" "$PA/Samples"; fi
  done ) 2>/dev/null &
SYNC=$!
ELSEWHERE_TEST_NEW_AUDIO="$T/newstem.wav" ELSEWHERE_USER_ID=aaaaaaaaaaaaaaaa ELSEWHERE_USER_NAME=Alice "$APP" --no-audio --open "$EDIT" --collab-test A "$T/out" > "$T/A.log" 2>&1 &
PA_PID=$!
ELSEWHERE_USER_ID=bbbbbbbbbbbbbbbb ELSEWHERE_USER_NAME=Bob "$APP" --no-audio --open "$EDITB" --collab-test B "$T/out" > "$T/B.log" 2>&1 &
PB_PID=$!
wait $PA_PID $PB_PID; kill $SYNC 2>/dev/null; wait $SYNC 2>/dev/null
grep -h COLLAB "$T/A.log" "$T/B.log"
grep -h "Assertion" "$T/A.log" "$T/B.log" | sort | uniq -c
if cmp -s "$T/out/A.xml" "$T/out/B.xml"; then echo "PASS: both collaborators ended with identical projects"; rm -rf "$T"; exit 0
else echo "FAIL: projects differ (see $T)"; diff <(xmllint --format "$T/out/A.xml") <(xmllint --format "$T/out/B.xml") | head -40; exit 1; fi
