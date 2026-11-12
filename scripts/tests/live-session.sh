#!/bin/bash
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
# Two-person live session test. Two Elsewhere instances ("Alice", "Bob") open their own copy of a
# shared project (kept in sync by a fake cloud service), go live, connect peer-to-peer through
# the folder handshake, play (with the master fader muted) and exchange intervals for ~45 s.
# Needs a working audio device. Usage: scripts/tests/live-session.sh <project folder> [Debug|Release]
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
( for i in $(seq 1 60); do sleep 1.5
    rsync -a --update "$PA/Collab/" "$PB/Collab/"; rsync -a --update "$PB/Collab/" "$PA/Collab/"
  done ) 2>/dev/null &
SYNC=$!
ELSEWHERE_USER_ID=aaaaaaaaaaaaaaaa ELSEWHERE_USER_NAME=Alice "$APP" --open "$EDIT" --live-test A "$T/out" > "$T/A.log" 2>&1 &
A_PID=$!
ELSEWHERE_USER_ID=bbbbbbbbbbbbbbbb ELSEWHERE_USER_NAME=Bob "$APP" --open "$EDITB" --live-test B "$T/out" > "$T/B.log" 2>&1 &
B_PID=$!
wait $A_PID $B_PID; kill $SYNC 2>/dev/null; wait $SYNC 2>/dev/null
grep -h "^LIVE" "$T/A.log" "$T/B.log"
grep -h "Assertion" "$T/A.log" "$T/B.log" | sort | uniq -c
echo "Logs and screenshots: $T"
