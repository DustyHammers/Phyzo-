#!/usr/bin/env bash
# Fetches the pinned dependencies from scripts/deps.env into work/deps/ (repository root).
# Usage: scripts/fetch_deps.sh [musashi] [juce]   (no arguments: both)
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=deps.env
source "$root/scripts/deps.env"
mkdir -p "$root/work/deps"

fetch() {   # name url commit
    local dir="$root/work/deps/$1" url="$2" commit="$3"
    if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ]; then
        echo "$1: $commit (already present)"; return
    fi
    rm -rf "$dir"
    git init -q "$dir"
    git -C "$dir" fetch -q --depth 1 "$url" "$commit"
    git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
    [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || { echo "$1: wrong commit" >&2; exit 1; }
    echo "$1: $commit"
}

want="${*:-musashi juce}"
for d in $want; do
    case "$d" in
        musashi) fetch Musashi "$MUSASHI_URL" "$MUSASHI_COMMIT" ;;
        juce)    fetch JUCE "$JUCE_URL" "$JUCE_COMMIT" ;;
        *) echo "unknown dependency $d" >&2; exit 2 ;;
    esac
done
