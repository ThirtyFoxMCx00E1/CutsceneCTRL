#!/usr/bin/env bash
# Verifies every symbol CutsceneCtrl resolves at runtime actually exists in
# a given libGTASA.so. Run this against a new game version before trusting
# the mod against it.
#
# Usage: ./verify_symbols.sh /path/to/libGTASA.so
set -euo pipefail

LIB="${1:?Usage: $0 /path/to/libGTASA.so}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIST="$HERE/symbol_list.txt"

if ! command -v readelf >/dev/null; then
    echo "readelf not found (install binutils)"; exit 1
fi

echo "Checking $(wc -l < "$LIST") symbols against: $LIB"
readelf --dyn-syms -W "$LIB" 2>/dev/null | awk '{print $8}' > /tmp/_cutscenectrl_syms.txt
readelf -s -W "$LIB" 2>/dev/null | awk '{print $8}' >> /tmp/_cutscenectrl_syms.txt

missing=0
while read -r sym; do
    [ -z "$sym" ] && continue
    if grep -qxF "$sym" /tmp/_cutscenectrl_syms.txt; then
        echo "  OK   $sym"
    else
        echo "  MISSING  $sym"
        missing=$((missing+1))
    fi
done < "$LIST"

echo ""
if [ "$missing" -eq 0 ]; then
    echo "All symbols present."
else
    echo "$missing symbol(s) missing -- the corresponding feature(s) will log a"
    echo "warning and disable themselves at runtime rather than crash."
fi
