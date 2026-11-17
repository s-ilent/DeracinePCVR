#!/usr/bin/env python3
"""Allowlist/denylist guard for automated file operations.

The harness is one Windows process among many under one user token, so this guard cannot
sandbox arbitrary tools. It is preventive for the harness's own copy/delete/write calls: the
runner invokes it before every destructive operation, and it fails closed. A separate
preflight/postflight detects changes to protected files, which is the only barrier that also
covers operations this guard never sees.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import ntpath

FILE_ATTRIBUTE_REPARSE_POINT = 0x400


class GuardError(Exception):
    pass


def normalise(path: str) -> str:
    if not isinstance(path, str) or not path.strip():
        raise GuardError("empty path")
    raw = path.strip()
    if raw.startswith("\\\\?\\"):
        raw = raw[4:]
    if not is_absolute(raw):
        raise GuardError(f"relative path refused: {raw!r}")
    if any(part == ".." for part in raw.replace("/", "\\").split("\\")):
        raise GuardError(f"path contains '..': {raw!r}")
    return os.path.normpath(raw)


def is_absolute(path: str) -> bool:
    return ntpath.isabs(path) or os.path.isabs(path)


def _is_reparse_point(path: str) -> bool:
    try:
        stat = os.lstat(path)
    except OSError:
        return False
    return bool(getattr(stat, "st_file_attributes", 0) & FILE_ATTRIBUTE_REPARSE_POINT)


def reparse_escape(path: str, root: str) -> str | None:
    """Returns the first reparse point below `root` that resolves outside `root`."""
    resolved_root = os.path.realpath(root)
    current = root
    relative = os.path.relpath(path, root)
    if relative == ".":
        return None
    for part in relative.split(os.sep):
        current = os.path.join(current, part)
        if not os.path.exists(current) and not os.path.islink(current):
            break
        if _is_reparse_point(current):
            resolved = os.path.realpath(current)
            if not is_under(resolved, resolved_root):
                return current
    return None


def is_under(path: str, root: str) -> bool:
    path = os.path.normcase(os.path.normpath(path))
    root = os.path.normcase(os.path.normpath(root))
    if path == root:
        return True
    return path.startswith(root + os.sep)


def check(path: str, op: str, config: dict) -> dict:
    target = normalise(path)
    resolved = os.path.realpath(target)
    allow_write = [os.path.realpath(p) for p in config.get("allow_write", [])]
    allow_read = [os.path.realpath(p) for p in config.get("allow_read", [])]
    deny = [os.path.realpath(p) for p in config.get("deny", [])]

    allow = allow_write if op in ("write", "delete", "mkdir", "copy_dest") else allow_write + allow_read
    if not allow:
        raise GuardError("config has no allow roots; failing closed")

    lexical_ok = any(is_under(target, root) for root in allow)
    resolved_ok = any(is_under(resolved, root) for root in allow)
    denied = any(is_under(target, root) or is_under(resolved, root) for root in deny)

    escaped = None
    for root in allow:
        if is_under(target, root):
            escaped = reparse_escape(target, root)
            if escaped is not None:
                break

    verdict = {
        "path": target,
        "resolved": resolved,
        "op": op,
        "allowed": lexical_ok and resolved_ok and not denied and escaped is None,
        "reasons": [],
    }
    if not lexical_ok:
        verdict["reasons"].append("outside every allow root")
    if not resolved_ok:
        verdict["reasons"].append("resolves outside every allow root")
    if denied:
        verdict["reasons"].append("under a deny root")
    if escaped is not None:
        verdict["reasons"].append(f"reparse point escapes the allow root: {escaped}")
    return verdict


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="guard-roots JSON written by preflight")
    parser.add_argument("--op", required=True,
                        choices=["write", "delete", "mkdir", "copy_dest", "copy_source", "read"])
    parser.add_argument("--path", required=True)
    parser.add_argument("--json", action="store_true", help="print the verdict as JSON")
    args = parser.parse_args(argv)

    try:
        with open(args.config, encoding="utf-8") as handle:
            config = json.load(handle)
        if not isinstance(config, dict):
            raise GuardError("config is not an object")
        verdict = check(args.path, args.op, config)
    except (GuardError, OSError, ValueError, json.JSONDecodeError) as error:
        print(json.dumps({"allowed": False, "error": str(error)}))
        return 2

    if args.json:
        print(json.dumps(verdict))
    else:
        state = "ALLOW" if verdict["allowed"] else "DENY"
        print(f"{state} {verdict['op']} {verdict['path']} -> {verdict['resolved']}")
    return 0 if verdict["allowed"] else 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
