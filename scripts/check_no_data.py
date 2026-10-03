#!/usr/bin/env python3
"""Fails if ROM files or other data were ever committed.

Checks every file in every commit reachable from REV (default HEAD), so data that was committed and deleted again is
caught too (it would stay in the history). A file is rejected when:
  - its extension is .bin, .img, .wav, .mid, .syx or .mp3 (any letter case), or
  - it is an image or font (.png, .jpg, .jpeg, .gif, .tga, .bmp, .webp, .psd, .ttf, .otf): skins and their art are
    never committed, or
  - it is a skin file (.rml, .rcss) anywhere but the text-only test skin in plugin/skin/tests/skins/, or
  - it is larger than 1 MB (1,048,576 bytes), or
  - its MD5 equals the OS image's or the native wave image's (a renamed ROM of any size).

Usage: scripts/check_no_data.py [REV]
Needs the full history (CI: actions/checkout with fetch-depth: 0).
"""
import hashlib
import subprocess
import sys

FORBIDDEN_EXTENSIONS = (".bin", ".img", ".wav", ".mid", ".syx", ".mp3")
SKIN_ASSET_EXTENSIONS = (".png", ".jpg", ".jpeg", ".gif", ".tga", ".bmp", ".webp", ".psd", ".ttf", ".otf")
SKIN_EXTENSIONS = (".rml", ".rcss")
TEST_SKINS = "plugin/skin/tests/skins/"
MAX_BYTES = 1024 * 1024
ROM_MD5 = {"8de06f48bfb0d847cacab05b763e22c5": "the OS image",
           "42a974e31e48b05815d07554d0063171": "the native wave image"}


def git(*args):
    return subprocess.run(["git", *args], check=True, capture_output=True).stdout


def main():
    rev = sys.argv[1] if len(sys.argv) > 1 else "HEAD"
    commits = git("rev-list", rev).decode().split()
    checked = {}                     # blob -> reason or None
    problems = []
    for commit in commits:
        # Files added or changed by this commit (against its first parent; the root commit lists everything).
        out = git("diff-tree", "-r", "-z", "--root", "--no-commit-id", "--no-renames", "--diff-filter=AMT", commit)
        fields = out.split(b"\0")
        i = 0
        while i + 1 < len(fields) and fields[i]:
            meta, path = fields[i].decode(), fields[i + 1].decode("utf-8", "replace")
            i += 2
            new_mode, new_blob = meta.split()[1], meta.split()[3]
            if new_mode == "160000":         # submodule link, no file content
                continue
            reasons = []
            if path.lower().endswith(FORBIDDEN_EXTENSIONS):
                reasons.append("data file type " + path[path.rfind("."):])
            if path.lower().endswith(SKIN_ASSET_EXTENSIONS):
                reasons.append("image or font file " + path[path.rfind("."):] + " (skin art is never committed)")
            if path.lower().endswith(SKIN_EXTENSIONS) and not path.startswith(TEST_SKINS):
                reasons.append("skin file outside the test skin (skins live in ~/Documents/Phyzo/skins/)")
            if new_blob not in checked:
                size = int(git("cat-file", "-s", new_blob))
                why = []
                if size > MAX_BYTES:
                    why.append("%d bytes (over 1 MB)" % size)
                md5 = hashlib.md5(git("cat-file", "blob", new_blob)).hexdigest()
                if md5 in ROM_MD5:
                    why.append("content is " + ROM_MD5[md5])
                checked[new_blob] = why
            reasons += checked[new_blob]
            if reasons:
                problems.append("%s  %s: %s" % (commit[:10], path, "; ".join(reasons)))
    if problems:
        print("Data files found in the history (ROMs and other data must never be committed):")
        print("\n".join(problems))
        print("\nRemoving the file in a new commit is not enough: the commit that added it must be removed from the")
        print("branch before it is merged. Ask for help with this before merging.")
        return 1
    print("check_no_data: %d commits, %d files checked: no data files" % (len(commits), len(checked)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
