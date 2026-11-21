#!/usr/bin/env python3
"""Allowlist/denylist guard for automated file operations.

The harness is one Windows process among many under one user token, so this guard cannot
sandbox arbitrary tools. It is preventive for the harness's own copy/delete/write calls: the
runner invokes it before every destructive operation, and it fails closed. A separate
preflight/postflight detects changes to protected files, which is the only barrier that also
covers operations this guard never sees.

Every mutating operation additionally denies the shipped protected-path policy
(``shadow-policy.json``): the active installation, shared game images, the shared harness and
evidence stores and any checkout of this repository stay denied even when the caller's config
would allow them. Reads are unaffected, because preflight hashes those paths legitimately. A
missing or malformed policy is a failure, not a bypass.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import ntpath
from pathlib import Path

FILE_ATTRIBUTE_REPARSE_POINT = 0x400

TOOLS_DIR = Path(__file__).resolve().parent
# Active installations, shared game images, the shared harness/evidence stores and every checkout
# of this repository. Automated writes to these fail closed even when the caller's config would
# allow them; game_shadow.py enforces the same list for the game shadow.
POLICY_PATH = TOOLS_DIR / "shadow-policy.json"
MUTATING_OPS = frozenset({"write", "delete", "mkdir", "copy_dest"})


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


def protected_deny() -> list[str]:
    """The shipped protected-path policy, resolved exactly as game_shadow.py resolves it.

    A missing or malformed shadow-policy.json is not ignored: automated writes fail closed. The
    policy is loaded through game_shadow.py so one file and one expansion rule govern both the
    shadow proof and the write guard.
    """
    try:
        import game_shadow
    except ImportError as error:  # pragma: no cover - only when this file is imported out of place
        raise GuardError(f"protected-path policy is unavailable: {error}") from error
    try:
        return list(game_shadow.load_policy(POLICY_PATH)["deny"])
    except game_shadow.UsageError as error:
        raise GuardError(f"protected-path policy is unusable: {error}") from error


def _first_deny(path: str, resolved: str, roots: list[str]) -> str | None:
    for root in roots:
        if is_under(path, root) or is_under(resolved, root):
            return root
    return None


def _roots(config: dict, key: str) -> list[str]:
    """Resolve one root list, rejecting non-string entries instead of raising TypeError."""
    value = config.get(key, [])
    if not isinstance(value, list):
        raise GuardError(f"config '{key}' is not a list")
    roots: list[str] = []
    for entry in value:
        if not isinstance(entry, str):
            raise GuardError(f"config '{key}' contains a non-string entry: {entry!r}")
        roots.append(os.path.realpath(entry))
    return roots


def check(path: str, op: str, config: dict) -> dict:
    target = normalise(path)
    resolved = os.path.realpath(target)
    allow_write = _roots(config, "allow_write")
    allow_read = _roots(config, "allow_read")
    deny = _roots(config, "deny")
    # The protected-path policy covers mutations only: reading the active install is legitimate
    # (preflight hashes it), writing or deleting it never is.
    policy_deny = protected_deny() if op in MUTATING_OPS else []

    allow = allow_write if op in ("write", "delete", "mkdir", "copy_dest") else allow_write + allow_read
    if not allow:
        raise GuardError("config has no allow roots; failing closed")

    lexical_ok = any(is_under(target, root) for root in allow)
    resolved_ok = any(is_under(resolved, root) for root in allow)
    config_denied = _first_deny(target, resolved, deny)
    policy_denied = _first_deny(target, resolved, policy_deny)

    escaped = None
    for root in allow:
        if is_under(target, root):
            escaped = reparse_escape(target, root)
            if escaped is not None:
                break

    allowed = (
        lexical_ok
        and resolved_ok
        and config_denied is None
        and policy_denied is None
        and escaped is None
    )
    verdict = {
        "path": target,
        "resolved": resolved,
        "op": op,
        "allowed": allowed,
        "reasons": [],
        "policy": {"path": str(POLICY_PATH), "entries": len(policy_deny)},
    }
    if not lexical_ok:
        verdict["reasons"].append("outside every allow root")
    if not resolved_ok:
        verdict["reasons"].append("resolves outside every allow root")
    if config_denied is not None:
        verdict["reasons"].append("under a deny root")
    if policy_denied is not None:
        verdict["reasons"].append(
            f"under a protected-path policy entry: {policy_denied}")
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
