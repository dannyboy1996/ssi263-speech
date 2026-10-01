#!/bin/sh
# blazie_files on Linux, a saved unit's files round trip (src/apps/blazie/blazie_files.c): export the state to a disk
# image, unpack the image into a folder, add a file there, pack the folder into a new image, import it into the state;
# the unit then lists the new file, its bytes extract exactly, and every file it had before is still there, byte for
# byte.  The state is copied first: the given one is never changed.
#
#   tools/blazie_files_roundtrip.sh BLAZIE_FILES STATE
#
# Exit 0 and "round trip: ok" when it all holds; 1 and the first thing that did not.
TOOL="$1"
STATE="$2"
T="$(mktemp -d)" || exit 1
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL $*"; echo "round trip: FAILED"; exit 1; }
cp "$STATE" "$T/unit.state" || fail "cannot copy $STATE"
"$TOOL" extract "$T/unit.state" "$T/before" >/dev/null || fail "extract (before)"
"$TOOL" export "$T/unit.state" "$T/a.img" >/dev/null || fail "export"
"$TOOL" unpack "$T/a.img" "$T/dir" >/dev/null || fail "unpack"
first="$(find "$T/dir" -mindepth 1 -maxdepth 1 -type d | sort | head -1)"
[ -n "$first" ] || fail "the image has no folder"
printf 'from linux\r' > "$first/linux.txt"
"$TOOL" pack "$T/dir" "$T/b.img" >/dev/null || fail "pack"
"$TOOL" import "$T/unit.state" "$T/b.img" >"$T/import.log" || { cat "$T/import.log"; fail "import"; }
"$TOOL" list "$T/unit.state" | grep -q '^linux\.txt  *11 bytes' || fail "the unit does not list linux.txt, 11 bytes"
"$TOOL" extract "$T/unit.state" "$T/after" >/dev/null || fail "extract (after)"
got="$(find "$T/after" -name linux.txt)"
[ -n "$got" ] && cmp -s "$got" "$first/linux.txt" || fail "linux.txt's bytes differ"
rm -f "$got"
find "$T/before" "$T/after" -type d -empty -delete
diff -r "$T/before" "$T/after" >/dev/null || fail "a file the unit had changed"
"$TOOL" check "$T/unit.state" >/dev/null || fail "the file system's rules"
echo "round trip: ok (export, unpack, a new file, pack, import; $(find "$T/before" -type f | wc -l) files kept)"
