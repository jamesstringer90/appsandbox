#!/usr/bin/env python3
"""
Cross-check src/core/ source files against the project files in the
Windows and macOS build systems. Prints anything that is on disk but
not listed in a project file, or listed in a project file but missing
from disk.

Usage:
    python tools/check-sources.py

Exit code 0 on success (no drift), 1 if drift detected.
"""

import os
import re
import sys

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CORE_DIR = os.path.join(REPO_ROOT, "src", "core")
VCXPROJ = os.path.join(REPO_ROOT, "AppSandboxCore.vcxproj")
XCODEPROJ = os.path.join(REPO_ROOT, "app", "mac", "AppSandbox.xcodeproj", "project.pbxproj")


def find_core_files():
    result = []
    if not os.path.isdir(CORE_DIR):
        return result
    for entry in sorted(os.listdir(CORE_DIR)):
        if entry.endswith(".c") or entry.endswith(".h"):
            result.append(entry)
    return result


def parse_vcxproj(path):
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    matches = re.findall(r'Include="src\\core\\([^"]+)"', text)
    return set(matches)


def parse_xcodeproj(path):
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    matches = re.findall(r'path\s*=\s*[^\s;]*src/core/([^\s;/]+\.[ch])', text)
    return set(matches)


def report(label, on_disk, in_proj):
    if in_proj is None:
        print(f"[skip] {label}: project file not present")
        return 0
    missing = on_disk - in_proj
    stale = in_proj - on_disk
    drift = 0
    if missing:
        drift += len(missing)
        print(f"[drift] {label}: files on disk but not in project:")
        for f in sorted(missing):
            print(f"    {f}")
    if stale:
        drift += len(stale)
        print(f"[drift] {label}: files in project but not on disk:")
        for f in sorted(stale):
            print(f"    {f}")
    if drift == 0:
        print(f"[ok] {label}: {len(on_disk)} core files match")
    return drift


def main():
    on_disk = set(find_core_files())
    total_drift = 0
    total_drift += report("AppSandboxCore.vcxproj", on_disk, parse_vcxproj(VCXPROJ))
    total_drift += report("app/mac/AppSandbox.xcodeproj", on_disk, parse_xcodeproj(XCODEPROJ))
    if total_drift > 0:
        print(f"\n{total_drift} drift(s) detected")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
