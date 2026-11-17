#!/usr/bin/env python3
"""Prove that a benchmark ``-Game`` path is a private game shadow, not the active installation.

``runner.ps1`` accepts a caller-selected game directory, adds it to the write allowlist and
rewrites ``<game>/gameproject/saved/config/ps4/engine.ini``. A path string cannot show that the
directory is a private copy: the shadow declares itself with ``benchmark-shadow.json`` inside its
root and pins the eboot it contains, and this checker refuses anything that escapes that root,
matches the protected-path policy, or disagrees with the declaration.

Fail closed. Each of these is a reason and an invalid result:

* the game directory, shadow root or eboot does not resolve, is relative, or keeps a ``..``
  component after normalisation;
* the game realpath is not the shadow root and is not inside it (path escape);
* the shadow root is inside a repository checkout, is blocklisted, contains a checkout, or
  contains a blocklisted path (an over-broad "private" declaration);
* ``benchmark-shadow.json`` is missing, is not a JSON object, resolves outside the shadow root,
  does not declare ``private: true``, has no 64-hex ``eboot_sha256`` or has no non-empty
  ``owner``;
* the eboot is not ``<game>/eboot.bin``, cannot be read, or its sha256 differs from the manifest;
* the engine.ini path resolves outside the game directory (for example through a
  ``gameproject/saved`` junction into the active install) - the exact write this checker exists
  to prevent.

The writability of engine.ini is reported, not gated: the runner still has to write it and the
caller decides whether to proceed. The blocklist lives in ``shadow-policy.json`` beside this file.

Standard library only. Python 3.9+.

CLI::

    python game_shadow.py check --game <dir> --shadow-root <dir> --eboot <eboot.bin>
                                [--policy <json>] [--json]

Exit codes: 0 valid, 2 invalid, 1 usage error (bad arguments, unreadable/invalid policy).
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import json
import ntpath
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

MODULE_DIR = Path(__file__).resolve().parent
DEFAULT_POLICY = MODULE_DIR / "shadow-policy.json"
MANIFEST_NAME = "benchmark-shadow.json"
ENGINE_INI_PARTS = ("gameproject", "saved", "config", "ps4", "engine.ini")
HEX64 = re.compile(r"\A[0-9a-fA-F]{64}\Z")

# Windows API constants for the non-mutating directory-write probe.
FILE_ADD_FILE = 0x0002
FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
FILE_SHARE_ALL = 0x00000007
OPEN_EXISTING = 3
FILE_ATTRIBUTE_READONLY = getattr(stat, "FILE_ATTRIBUTE_READONLY", 0x00000001)

GIT_TIMEOUT_SECONDS = 10
_CHUNK = 1024 * 1024


class UsageError(Exception):
    """The command line, the policy file or the environment is unusable."""


# --- path resolution -------------------------------------------------------------------------

def _plain_path(text: str) -> str:
    """Drop the Windows extended-length path prefix so ntpath/realpath see a normal path."""
    if text.startswith("\\\\?\\UNC\\"):
        return "\\\\" + text[len("\\\\?\\UNC\\"):]
    if text.startswith("\\\\?\\"):
        return text[len("\\\\?\\"):]
    return text


def _has_parent_reference(path: str) -> bool:
    return any(part == ".." for part in path.replace("/", "\\").split("\\"))


def _is_absolute_path(path: str) -> bool:
    drive, tail = ntpath.splitdrive(path)
    if drive:
        return tail.startswith(("\\", "/"))
    return os.path.isabs(path)


def _resolve(raw: object, label: str) -> tuple[str | None, str | None]:
    """Resolve one input path; returns (realpath, error). Never raises for a bad path."""
    if not isinstance(raw, str) or not raw.strip():
        return None, f"{label} is empty"
    text = _plain_path(raw.strip())
    if _has_parent_reference(text):
        return None, f"{label} contains a '..' component: {raw!r}"
    if not _is_absolute_path(text):
        return None, f"{label} is not an absolute path: {raw!r}"
    try:
        resolved = os.path.realpath(text)
    except (OSError, ValueError) as error:
        return None, f"{label} does not resolve: {raw!r}: {error}"
    if not resolved or not _is_absolute_path(resolved):
        return None, f"{label} does not resolve to an absolute path: {raw!r}"
    if _has_parent_reference(resolved):
        return None, f"{label} keeps a '..' component after resolution: {resolved!r}"
    return os.path.normpath(resolved), None


def is_inside(child: str, root: str) -> bool:
    """True when ``child`` is ``root`` or below it, at a path-component boundary."""
    child_key = os.path.normcase(os.path.normpath(child))
    root_key = os.path.normcase(os.path.normpath(root))
    if child_key == root_key:
        return True
    prefix = root_key if root_key.endswith(os.sep) else root_key + os.sep
    return child_key.startswith(prefix)


def matches_blocklist_prefix(path: str, prefix: str) -> bool:
    """Blocklist entries are literal prefixes after realpath, so name-squatting variants fail too."""
    return os.path.normcase(os.path.normpath(path)).startswith(
        os.path.normcase(os.path.normpath(prefix)))


# --- repository roots ------------------------------------------------------------------------

def _git(*args: str) -> str | None:
    executable = shutil.which("git")
    if executable is None:
        return None
    try:
        completed = subprocess.run(
            [executable, "-C", str(MODULE_DIR), *args],
            capture_output=True,
            text=True,
            timeout=GIT_TIMEOUT_SECONDS)
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout


def _environment_root(name: str) -> str | None:
    raw = os.environ.get(name)
    if not raw:
        return None
    resolved, error = _resolve(raw, name)
    if error or resolved is None or not os.path.isdir(resolved):
        raise UsageError(f"{name} does not name an existing directory: {raw!r}")
    return resolved


@functools.lru_cache(maxsize=1)
def checkout_root() -> str:
    """The checkout this module runs from: ``%REPO%`` in the policy."""
    override = _environment_root("DERACINE_REPO")
    if override:
        return override
    text = _git("rev-parse", "--show-toplevel")
    if text and text.strip():
        resolved, error = _resolve(text.strip(), "git top level")
        if error is None and resolved and os.path.isdir(resolved):
            return resolved
    return os.path.realpath(str(MODULE_DIR.parents[1]))


@functools.lru_cache(maxsize=1)
def workspace_root() -> str:
    """The private workspace above a checkout: ``%WORKSPACE%`` in the policy."""
    override = _environment_root("DERACINE_WORKSPACE")
    if override:
        return override
    return os.path.dirname(checkout_root())


@functools.lru_cache(maxsize=1)
def repository_roots() -> tuple[str, ...]:
    """Every checkout of this repository; a shadow root inside any of them is not private."""
    roots = [checkout_root()]
    text = _git("worktree", "list", "--porcelain")
    if text:
        for line in text.splitlines():
            if not line.startswith("worktree "):
                continue
            candidate = _plain_path(line[len("worktree "):].strip())
            # WSL-created worktrees are recorded as /mnt/...; only keep paths Windows can resolve.
            if os.path.isdir(candidate):
                roots.append(os.path.realpath(candidate))
    unique: list[str] = []
    seen: set[str] = set()
    for root in roots:
        key = os.path.normcase(root)
        if key not in seen:
            seen.add(key)
            unique.append(root)
    return tuple(unique)


# --- policy ----------------------------------------------------------------------------------

def expand_policy_path(raw: str) -> str:
    if not isinstance(raw, str) or not raw.strip():
        raise UsageError("policy deny entry is empty")
    text = raw.strip()
    for token, value in (("%REPO%", checkout_root()), ("%WORKSPACE%", workspace_root())):
        text = text.replace(token, value)
    token = re.search(r"%[^%]+%", text)
    if token:
        raise UsageError(f"unknown policy token {token.group(0)!r} in {raw!r}")
    resolved, error = _resolve(text, f"policy entry {raw!r}")
    if error or resolved is None:
        raise UsageError(error or f"policy entry does not resolve: {raw!r}")
    return resolved


def load_policy(path: str | os.PathLike[str] | None = None) -> dict:
    """Load and expand ``shadow-policy.json``; a missing or malformed policy is a usage error."""
    policy_path = Path(path) if path is not None else DEFAULT_POLICY
    try:
        with open(policy_path, encoding="utf-8") as handle:
            document = json.load(handle)
    except OSError as error:
        raise UsageError(f"cannot read the shadow policy {str(policy_path)!r}: {error}") from error
    except json.JSONDecodeError as error:
        raise UsageError(
            f"the shadow policy {str(policy_path)!r} is not valid JSON: {error}") from error
    if not isinstance(document, dict):
        raise UsageError(f"the shadow policy {str(policy_path)!r} is not a JSON object")
    raw_entries = document.get("deny")
    if not isinstance(raw_entries, list) or not raw_entries:
        raise UsageError(
            f"the shadow policy {str(policy_path)!r} has no non-empty 'deny' list")
    entries = []
    for raw in raw_entries:
        if not isinstance(raw, str):
            raise UsageError(f"the shadow policy {str(policy_path)!r} has a non-string deny entry")
        entries.append({"raw": raw, "resolved": expand_policy_path(raw)})
    return {
        "path": os.path.realpath(os.path.abspath(os.fspath(policy_path))),
        "deny": [entry["resolved"] for entry in entries],
        "entries": entries,
    }


# --- file inspection -------------------------------------------------------------------------

def _sha256_file(path: str) -> str | None:
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as handle:
            for chunk in iter(lambda: handle.read(_CHUNK), b""):
                digest.update(chunk)
    except OSError:
        return None
    return digest.hexdigest()


def _file_writable(path: str) -> bool:
    try:
        attributes = getattr(os.stat(path), "st_file_attributes", 0)
    except OSError:
        return False
    if attributes & FILE_ATTRIBUTE_READONLY:
        return False
    # Opening for write without O_TRUNC proves access without changing a byte.
    try:
        handle = os.open(path, os.O_WRONLY)
    except OSError:
        return False
    os.close(handle)
    return True


def _windows_directory_can_add_file(path: str) -> bool | None:
    """Non-mutating ACL probe: open the directory for FILE_ADD_FILE and close it."""
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes
    except ImportError:  # pragma: no cover - ctypes is part of the standard library
        return None
    try:
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        create_file = kernel32.CreateFileW
        create_file.restype = wintypes.HANDLE
        create_file.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
            wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
        ]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = create_file(
            str(path), FILE_ADD_FILE, FILE_SHARE_ALL, None, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, None)
    except (AttributeError, OSError, ValueError, ctypes.ArgumentError):
        return None
    invalid = ctypes.c_void_p(-1).value
    if handle is None or handle == invalid:
        return False
    kernel32.CloseHandle(handle)
    return True


def _directory_writable(path: str) -> bool:
    try:
        attributes = getattr(os.stat(path), "st_file_attributes", 0)
    except OSError:
        return False
    if attributes & FILE_ATTRIBUTE_READONLY:
        return False
    probed = _windows_directory_can_add_file(path)
    if probed is not None:
        return probed
    return os.access(path, os.W_OK)


def _nearest_existing_directory(path: str) -> str | None:
    candidate = path
    while True:
        if os.path.isdir(candidate):
            return candidate
        parent = os.path.dirname(candidate)
        if not parent or parent == candidate:
            return None
        candidate = parent


def _writable(path: str) -> bool:
    if os.path.isfile(path):
        return _file_writable(path)
    directory = _nearest_existing_directory(os.path.dirname(path))
    if directory is None:
        return False
    return _directory_writable(directory)


def check_engine_ini(game_dir: str | os.PathLike[str]) -> dict:
    """Report the pinned-preset target before any write: path, existence, hash, writability."""
    game = os.path.abspath(os.fspath(game_dir))
    path = os.path.join(game, *ENGINE_INI_PARTS)
    exists = os.path.isfile(path)
    return {
        "path": path,
        "resolved": os.path.realpath(path),
        "exists": exists,
        "sha256": _sha256_file(path) if exists else None,
        "writable": _writable(path),
    }


# --- the check -------------------------------------------------------------------------------

def _load_manifest(path: str, shadow_root: str, reasons: list[str]) -> dict | None:
    real = os.path.realpath(path)
    if not is_inside(real, shadow_root):
        reasons.append(f"manifest resolves outside the shadow root: {real}")
        return None
    if not os.path.isfile(path):
        reasons.append(f"manifest not found: {path}")
        return None
    try:
        with open(path, encoding="utf-8") as handle:
            document = json.load(handle)
    except OSError as error:
        reasons.append(f"manifest cannot be read: {path}: {error}")
        return None
    except json.JSONDecodeError as error:
        reasons.append(f"manifest is not valid JSON: {path}: {error}")
        return None
    if not isinstance(document, dict):
        reasons.append(f"manifest is not a JSON object: {path}")
        return None

    if document.get("private") is not True:
        reasons.append("manifest 'private' must be the JSON boolean true")
    declared_hash = document.get("eboot_sha256")
    if not isinstance(declared_hash, str) or not HEX64.match(declared_hash):
        reasons.append("manifest 'eboot_sha256' must be a 64-character hex string")
    owner = document.get("owner")
    if not isinstance(owner, str) or not owner.strip():
        reasons.append("manifest 'owner' must be a non-empty string")
    return document


def _first_prefix(path: str, prefixes: list[str]) -> str | None:
    for prefix in prefixes:
        if matches_blocklist_prefix(path, prefix):
            return prefix
    return None


def check(
    game: str,
    shadow_root: str,
    eboot: str,
    policy: str | os.PathLike[str] | None = None,
) -> dict:
    """Return the verdict document for one ``-Game``/shadow/eboot triple."""
    policy_document = load_policy(policy)
    deny = policy_document["deny"]
    repositories = list(repository_roots())
    reasons: list[str] = []

    game_realpath, game_error = _resolve(game, "game directory")
    shadow_realpath, shadow_error = _resolve(shadow_root, "shadow root")
    eboot_realpath, eboot_error = _resolve(eboot, "eboot")
    for error in (game_error, shadow_error, eboot_error):
        if error:
            reasons.append(error)

    engine_ini = check_engine_ini(game_realpath or game)

    if game_realpath:
        if not os.path.isdir(game_realpath):
            reasons.append(f"game directory is not a directory: {game_realpath}")
        blocked = _first_prefix(game_realpath, deny)
        if blocked:
            reasons.append(f"game path is blocklisted (active/shared/protected): {blocked}")
        inside_repo = _first_prefix(game_realpath, repositories)
        if inside_repo:
            reasons.append(f"game path is inside the repository: {inside_repo}")

    manifest: dict | None = None
    manifest_path: str | None = None
    if shadow_realpath:
        if not os.path.isdir(shadow_realpath):
            reasons.append(f"shadow root is not a directory: {shadow_realpath}")
        blocked = _first_prefix(shadow_realpath, deny)
        if blocked:
            reasons.append(f"shadow root is blocklisted: {blocked}")
        inside_repo = _first_prefix(shadow_realpath, repositories)
        if inside_repo:
            reasons.append(f"shadow root is inside the repository: {inside_repo}")
        for entry in deny:
            if is_inside(entry, shadow_realpath):
                reasons.append(f"shadow root contains a blocklisted path: {entry}")
                break
        for root in repositories:
            if is_inside(root, shadow_realpath):
                reasons.append(f"shadow root contains a repository checkout: {root}")
                break

        if game_realpath:
            if game_realpath != shadow_realpath and not is_inside(game_realpath, shadow_realpath):
                reasons.append(
                    f"path escape: game directory {game_realpath} is outside "
                    f"shadow root {shadow_realpath}")
            manifest_path = os.path.join(shadow_realpath, MANIFEST_NAME)
            manifest = _load_manifest(manifest_path, shadow_realpath, reasons)

        if not is_inside(engine_ini["resolved"], shadow_realpath):
            reasons.append(
                f"engine.ini resolves outside the shadow root: {engine_ini['resolved']}")
        if game_realpath and not is_inside(engine_ini["resolved"], game_realpath):
            reasons.append(
                f"engine.ini resolves outside the game directory: {engine_ini['resolved']}")

    eboot_sha256: str | None = None
    if eboot_realpath:
        if not os.path.isfile(eboot_realpath):
            reasons.append(f"eboot is not a file: {eboot_realpath}")
        else:
            eboot_sha256 = _sha256_file(eboot_realpath)
            if eboot_sha256 is None:
                reasons.append(f"eboot cannot be read: {eboot_realpath}")
        if game_realpath:
            expected = os.path.join(game_realpath, "eboot.bin")
            if os.path.normcase(os.path.normpath(eboot_realpath)) != os.path.normcase(
                    os.path.normpath(expected)):
                reasons.append(
                    f"eboot is not the game's declared eboot: {eboot_realpath} "
                    f"(expected {expected})")

    if eboot_sha256 and manifest:
        declared = manifest.get("eboot_sha256")
        if isinstance(declared, str) and HEX64.match(declared):
            declared = declared.lower()
            if eboot_sha256 != declared:
                reasons.append(
                    f"eboot sha256 mismatch: manifest declares {declared}, "
                    f"file is {eboot_sha256}")

    return {
        "schema_version": 1,
        "valid": not reasons,
        "reasons": reasons,
        "shadow_root": shadow_realpath,
        "game_realpath": game_realpath,
        "eboot": eboot_realpath,
        "eboot_sha256": eboot_sha256,
        "manifest": manifest,
        "manifest_path": manifest_path,
        "engine_ini": engine_ini,
        "policy": {"path": policy_document["path"], "deny": deny},
        "repository_roots": repositories,
    }


# --- CLI -------------------------------------------------------------------------------------

class _Parser(argparse.ArgumentParser):
    """argparse exits 2 on a usage error; this tool reserves 2 for an invalid shadow."""

    def error(self, message: str) -> None:  # type: ignore[override]
        self.print_usage(sys.stderr)
        print(f"{self.prog}: error: {message}", file=sys.stderr)
        raise SystemExit(1)


def build_parser() -> argparse.ArgumentParser:
    parser = _Parser(prog="game_shadow.py", description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    check_command = commands.add_parser("check", help="validate one private game shadow")
    check_command.add_argument("--game", required=True, help="the -Game directory to validate")
    check_command.add_argument("--shadow-root", required=True,
                               help="the declared private shadow root")
    check_command.add_argument("--eboot", required=True, help="<game>/eboot.bin")
    check_command.add_argument("--policy", default=None,
                               help="protected-path policy (default: shadow-policy.json)")
    check_command.add_argument("--json", action="store_true", help="print the verdict as JSON")
    return parser


def _print_human(verdict: dict) -> None:
    state = "VALID" if verdict["valid"] else "INVALID"
    print(f"{state} shadow: {verdict['shadow_root']}")
    print(f"  game:   {verdict['game_realpath']}")
    print(f"  eboot:  {verdict['eboot']} ({verdict['eboot_sha256']})")
    print(f"  manifest: {verdict['manifest_path']}")
    engine = verdict["engine_ini"]
    print(f"  engine.ini: exists={engine['exists']} writable={engine['writable']} "
          f"sha256={engine['sha256']}")
    for reason in verdict["reasons"]:
        print(f"  - {reason}")


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(sys.argv[1:] if argv is None else argv)

    try:
        verdict = check(args.game, args.shadow_root, args.eboot, args.policy)
    except UsageError as error:
        if args.json:
            print(json.dumps({"valid": False, "reasons": [str(error)], "error": str(error)}))
        else:
            print(f"usage error: {error}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(verdict, indent=2))
    else:
        _print_human(verdict)
    return 0 if verdict["valid"] else 2


if __name__ == "__main__":
    sys.exit(main())
