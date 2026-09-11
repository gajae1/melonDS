#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compute a verifiable content identity for the melonDS source tree.

Identity schema 1 is a SHA-256 digest over a fixed schema prefix and, for
every included file in deterministic order, the relative path (UTF-8,
forward slashes), a NUL byte, the Git blob OID of the current content, and
a NUL byte. Working-tree identity hashes actual file content through Git's
own check-in filters (hash-object without -w), so line-ending normalization
always matches what Git would commit. Committed identity reads the HEAD
tree (ls-tree) with the same documentation-exclusion policy, so
documentation-only commits keep the same identity and a source ZIP made
from 'git archive HEAD' can be bound to the built binary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SCHEMA = 1
SCHEMA_PREFIX = b"melonds-source-identity-v1"
SOURCE_ID_RE = re.compile(r"[0-9a-f]{64}")
# Git blob OIDs are SHA-1 (40 hex) or SHA-256 (64 hex) depending on the
# repository's object format; the final source identity is always SHA-256.
GIT_OID_RE = re.compile(r"[0-9a-f]{40}|[0-9a-f]{64}")
VERSION_RE = re.compile(r"[0-9]+(\.[0-9]+)*")
# Documentation policy: only these root files and Markdown under plans/ are
# excluded. Source-side Markdown, tools, tests, CMake and vendored code stay
# part of the identity.
DOC_EXCLUDED_ROOT_FILES = ("README.md", "BUILD.md", "CONTRIBUTING.md")
DOC_EXCLUDED_POLICY = list(DOC_EXCLUDED_ROOT_FILES) + ["plans/**/*.md"]


class SourceIdentityError(Exception):
    """A clear, actionable identity computation or verification failure."""


def is_documentation_path(rel: str) -> bool:
    if "/" not in rel:
        return rel in DOC_EXCLUDED_ROOT_FILES
    return rel.startswith("plans/") and rel.endswith(".md")


def _git(repo: Path, args: list[str], input_bytes: bytes | None = None) -> bytes:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise SourceIdentityError(f"git {' '.join(args[:2])} failed in {repo}: {detail}")
    return result.stdout


def _resolve_repo(path: str | os.PathLike) -> Path:
    path = Path(path).resolve()
    try:
        top = _git(path, ["rev-parse", "--show-toplevel"]).decode("utf-8", "surrogateescape").strip()
    except SourceIdentityError as exc:
        raise SourceIdentityError(f"{path} is not inside a Git worktree") from exc
    return Path(top)


def _fail_unsupported_entry(mode: str | None, path: Path, rel: str) -> None:
    """Fail clearly on submodules and symlinks instead of following them."""
    if mode == "160000":
        raise SourceIdentityError(f"Git submodule not supported by source identity: {rel}")
    if mode == "120000":
        raise SourceIdentityError(f"Symlink not supported by source identity: {rel}")
    if path.is_symlink():
        raise SourceIdentityError(f"Symlink not supported by source identity: {rel}")


def _sort_key(rel: str) -> bytes:
    return rel.encode("utf-8", "surrogateescape")


def _c_quoted_path(rel: str) -> bytes:
    """Encode one path as an ASCII Git C-quoted string.

    'git hash-object --stdin-paths' reads one path per line and unquotes
    lines that start with a double quote; quoting every path uniformly
    keeps spaces, control bytes, leading dashes and non-UTF-8-safe names
    unambiguous without any per-file subprocess calls.
    """
    out = bytearray(b'"')
    for byte in rel.encode("utf-8", "surrogateescape"):
        if byte in (0x22, 0x5C):  # double quote and backslash
            out += b"\\" + bytes([byte])
        elif 0x20 <= byte <= 0x7E:
            out.append(byte)
        else:
            out += f"\\{byte:03o}".encode("ascii")
    out += b'"'
    return bytes(out)


def working_source_files(repo: str | os.PathLike) -> list[str]:
    """Relative paths of all tracked and nonignored untracked regular files.

    Tracked files deleted from the working tree are removed from the set;
    submodule and symlink entries fail loudly.
    """
    repo = _resolve_repo(repo)
    paths: set[str] = set()

    cached = _git(repo, ["ls-files", "-s", "-z", "--cached"])
    for record in cached.split(b"\0"):
        if not record:
            continue
        meta, _, raw_path = record.partition(b"\t")
        mode = meta.split(b" ")[0].decode("ascii")
        rel = raw_path.decode("utf-8", "surrogateescape")
        if is_documentation_path(rel):
            continue
        _fail_unsupported_entry(mode, repo / rel, rel)
        if not (repo / rel).is_file():
            continue  # tracked file deleted (or replaced) in the working tree
        _fail_unsupported_entry(None, repo / rel, rel)
        paths.add(rel)

    others = _git(repo, ["ls-files", "-z", "--others", "--exclude-standard"])
    for raw_path in others.split(b"\0"):
        if not raw_path:
            continue
        rel = raw_path.decode("utf-8", "surrogateescape")
        if is_documentation_path(rel):
            continue
        _fail_unsupported_entry(None, repo / rel, rel)
        if not (repo / rel).is_file():
            raise SourceIdentityError(f"Untracked entry is not a regular file: {rel}")
        paths.add(rel)

    return sorted(paths, key=_sort_key)


def _hash_working_blobs(repo: Path, rel_paths: list[str]) -> dict[str, str]:
    """Hash files through Git's own check-in filters (no -w).

    --stdin-paths is line-delimited (git has no NUL mode for it), so every
    path is sent in Git C-quoted form; control and non-ASCII bytes become
    octal escapes, which keeps the payload one quoted path per line.
    """
    if not rel_paths:
        return {}
    oids: dict[str, str] = {}
    payload = b"".join(_c_quoted_path(rel) + b"\n" for rel in rel_paths)
    out = _git(repo, ["hash-object", "--stdin-paths"], payload)
    lines = out.decode("ascii").split("\n")
    if len(lines) != len(rel_paths) + 1 or lines[-1] != "":
        raise SourceIdentityError("hash-object returned an unexpected OID stream")
    for rel, oid in zip(rel_paths, lines[:-1]):
        if not GIT_OID_RE.fullmatch(oid):
            raise SourceIdentityError(f"hash-object returned a malformed OID for {rel}")
        oids[rel] = oid
    return oids


def committed_source_entries(repo: str | os.PathLike) -> list[tuple[str, str]]:
    repo = _resolve_repo(repo)
    out = _git(repo, ["ls-tree", "-r", "-z", "HEAD"])
    entries: list[tuple[str, str]] = []
    for record in out.split(b"\0"):
        if not record:
            continue
        meta, _, raw_path = record.partition(b"\t")
        mode, kind, oid = meta.split(b" ")
        rel = raw_path.decode("utf-8", "surrogateescape")
        if is_documentation_path(rel):
            continue
        if kind != b"blob":
            raise SourceIdentityError(f"Unsupported HEAD entry ({kind.decode('ascii')}): {rel}")
        _fail_unsupported_entry(mode.decode("ascii"), repo / rel, rel)
        entries.append((rel, oid.decode("ascii")))
    entries.sort(key=lambda entry: _sort_key(entry[0]))
    return entries


def _digest(entries: list[tuple[str, str]]) -> str:
    hasher = hashlib.sha256()
    hasher.update(SCHEMA_PREFIX + b"\0")
    for rel, oid in entries:
        hasher.update(_sort_key(rel))
        hasher.update(b"\0")
        hasher.update(oid.encode("ascii"))
        hasher.update(b"\0")
    return hasher.hexdigest()


def working_source_identity(repo: str | os.PathLike) -> str:
    repo = _resolve_repo(repo)
    paths = working_source_files(repo)
    oids = _hash_working_blobs(repo, paths)
    return _digest([(rel, oids[rel]) for rel in paths])


def committed_source_identity(repo: str | os.PathLike) -> str:
    return _digest(committed_source_entries(repo))


HEADER_TEMPLATE = """/* Generated by tools/source_identity.py or cmake/PackageIdentity.cmake.
 * Do not edit; the file is rewritten only when the identity changes. */
#ifndef PACKAGE_IDENTITY_H
#define PACKAGE_IDENTITY_H

#define MELONDS_PACKAGE_IDENTITY_SCHEMA {schema}
#define MELONDS_PACKAGE_IDENTITY_SOURCE_ID "{source_id}"

#endif // PACKAGE_IDENTITY_H
"""


def render_header(source_id: str) -> str:
    return HEADER_TEMPLATE.format(schema=SCHEMA, source_id=source_id)


def _write_if_changed(path: Path, content: str) -> bool:
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(content, encoding="utf-8", newline="\n")
    temp.replace(path)
    return True


def generate(repo: str | os.PathLike, header: Path, report: Path) -> str:
    """Recompute the working identity and refresh header/report if changed."""
    source_id = working_source_identity(repo)
    _write_if_changed(header, render_header(source_id))
    _write_if_changed(report, json.dumps({
        "schema": SCHEMA,
        "source_id": source_id,
        "identity": "working-content",
        "documentation_exclusions": DOC_EXCLUDED_POLICY,
    }, indent=2) + "\n")
    return source_id


def parse_build_info(text: str) -> dict:
    """Strictly parse the single JSON object printed by 'melonDS --build-info'."""
    try:
        obj = json.loads(text)
    except json.JSONDecodeError as exc:
        raise SourceIdentityError(f"build info is not valid JSON: {exc}") from exc
    if not isinstance(obj, dict) or set(obj) != {"schema", "source_id", "version"}:
        raise SourceIdentityError("build info must contain exactly schema, source_id and version")
    if type(obj["schema"]) is not int or obj["schema"] != SCHEMA:
        raise SourceIdentityError(f"unsupported build info schema: {obj['schema']!r}")
    source_id, version = obj["source_id"], obj["version"]
    if not isinstance(source_id, str) or not (source_id == "" or SOURCE_ID_RE.fullmatch(source_id)):
        raise SourceIdentityError(f"malformed build info source_id: {source_id!r}")
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise SourceIdentityError(f"malformed build info version: {version!r}")
    return obj


def verify_build_info(build_info: dict, repo: str | os.PathLike, expected_version: str | None = None) -> str:
    """Bind an executable's embedded identity to the committed source.

    Rejects identity-less binaries, version drift and stale binaries whose
    embedded identity no longer matches the committed source identity.
    Returns the verified committed identity.
    """
    if expected_version is not None and build_info["version"] != expected_version:
        raise SourceIdentityError(
            f"EXE reports version {build_info['version']!r}, expected {expected_version!r}")
    source_id = build_info["source_id"]
    if not source_id:
        raise SourceIdentityError("EXE has no embedded source identity (pre-identity or OFF build)")
    committed = committed_source_identity(repo)
    if source_id != committed:
        raise SourceIdentityError(
            f"EXE source identity {source_id} does not match committed identity {committed}; "
            "rebuild before packaging")
    return committed


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name, help_text in (
        ("working", "print the working-tree content identity"),
        ("committed", "print the committed (HEAD) content identity"),
    ):
        child = sub.add_parser(name, help=help_text)
        child.add_argument("--repo", default=".")
    child = sub.add_parser("generate", help="regenerate package_identity.h and package-source.json")
    child.add_argument("--repo", default=".")
    child.add_argument("--header", type=Path, required=True)
    child.add_argument("--json", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "working":
            print(working_source_identity(args.repo))
        elif args.command == "committed":
            print(committed_source_identity(args.repo))
        else:
            print(generate(args.repo, args.header, args.json))
    except SourceIdentityError as exc:
        parser.exit(2, f"source-identity error: {exc}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
