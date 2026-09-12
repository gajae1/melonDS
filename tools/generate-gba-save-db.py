#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 melonDS team
"""Generate fixed EEPROM metadata from the pinned, CC0 MAME software list.

Offline usage: generate-gba-save-db.py gba.xml src/GBASaveDatabase.h [--check]
The CLI verifies the original XML bytes before parsing or touching the output.
parse_database() is independent of that pin check for synthetic parser tests.
No ROM data, game-code fallback, or EEPROM protocol inference is used.
"""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import xml.etree.ElementTree as ET


SOURCE_REVISION = "1a326ed01c3629259abcf61a2a6bebf25858d376"
SOURCE_URL = f"https://raw.githubusercontent.com/mamedev/mame/{SOURCE_REVISION}/hash/gba.xml"
SOURCE_SHA256 = "dc14af286739a50307e7d5893cc1fb29946297d1e7983adb2d68f63c999350fe"
SAVE_LENGTHS = {
    "gba_eeprom_4k": 512,
    "gba_eeprom_64k": 8192,
    "gba_yoshiug": 512,
    "gba_boktai": 8192,
}
Entry = tuple[bytes, int, int]  # Full SHA-1, original ROM length, save length.


def number(value: str) -> int:
    """Accept MAME decimal/0x-prefixed integer attributes, including zero."""
    if not re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", value):
        raise ValueError(f"Invalid integer: {value!r}")
    return int(value, 16 if value.lower().startswith("0x") else 10)


def parse_database(xml: bytes) -> tuple[list[Entry], Counter[str]]:
    """Filter complete gba_cart dumps; reject disagreements among eligible rows.

    Counts are per part, except software_records, unique_entries and save_*.
    Each excluded part receives one reason. Generic EEPROM is never sized.
    ElementTree does not load the software list's external DTD.
    """
    root = ET.fromstring(xml)
    if root.tag != "softwarelist" or root.get("name") != "gba":
        raise ValueError("Expected the gba softwarelist root")

    records: dict[bytes, tuple[int, int]] = {}
    counts: Counter[str] = Counter()
    for software in root.findall("software"):
        counts["software_records"] += 1
        for part in software.findall("part"):
            counts["parts"] += 1
            if part.get("name") != "cart" or part.get("interface") != "gba_cart":
                counts["excluded_part"] += 1
                continue
            slots = part.findall("feature[@name='slot']")
            if len(slots) != 1:
                counts["excluded_slot_missing_or_multiple"] += 1
                continue
            slot = slots[0].get("value", "")
            if slot not in SAVE_LENGTHS:
                reason = "unknown_eeprom" if slot == "gba_eeprom" else "other_slot"
                counts[f"excluded_{reason}"] += 1
                continue
            counts[f"slot_{slot}"] += 1

            areas = part.findall("dataarea")
            if len(areas) != 1 or areas[0].get("name") != "rom":
                counts["excluded_region_layout"] += 1
                continue
            area = areas[0]
            if len(area) != 1 or area[0].tag != "rom":
                counts["excluded_rom_layout"] += 1
                continue
            rom = area[0]
            if rom.get("status", "good") != "good":
                counts["excluded_status"] += 1
                continue
            if "loadflag" in rom.attrib:
                counts["excluded_loadflag"] += 1
                continue
            try:
                size = number(rom.get("size", ""))
                area_size = number(area.get("size", ""))
                offset = number(rom.get("offset", "0"))
            except ValueError:
                counts["excluded_numeric_attribute"] += 1
                continue
            if not 0 < size <= 0xFFFFFFFF or size != area_size:
                counts["excluded_size"] += 1
                continue
            if offset != 0:
                counts["excluded_offset"] += 1
                continue
            sha1 = rom.get("sha1", "")
            if not re.fullmatch(r"[0-9a-fA-F]{40}", sha1):
                counts["excluded_sha1"] += 1
                continue

            digest = bytes.fromhex(sha1)
            save_length = SAVE_LENGTHS[slot]
            previous = records.get(digest)
            if previous is not None:
                if previous[0] != size:
                    raise ValueError(f"Inconsistent ROM lengths for SHA-1 {sha1}")
                if previous[1] != save_length:
                    raise ValueError(f"Conflicting save capacities for SHA-1 {sha1}")
                counts["identical_duplicates"] += 1
            records[digest] = (size, save_length)
            counts["eligible_rows"] += 1

    entries = sorted((digest, size, save) for digest, (size, save) in records.items())
    counts["unique_entries"] = len(entries)
    counts.update(f"save_{save}" for _, _, save in entries)
    return entries, counts


def render_header(entries: list[Entry]) -> bytes:
    if not entries:
        raise ValueError("No fixed-capacity EEPROM records found")
    lines = [
        "// Generated by tools/generate-gba-save-db.py; do not edit.",
        "// Save metadata: MAME contributors, CC0-1.0.",
        "// https://creativecommons.org/publicdomain/zero/1.0/",
        f"// Source: {SOURCE_URL}",
        f"// Revision: {SOURCE_REVISION}",
        f"// Source SHA-256: {SOURCE_SHA256}",
        "// Generator and lookup code: GPL-3.0-or-later, melonDS team.",
        "// Only explicit fixed-capacity EEPROM records; unknown returns zero.",
        f"// Unique entries: {len(entries)}",
        "",
        "#ifndef MELONDS_GBA_SAVE_DATABASE_H",
        "#define MELONDS_GBA_SAVE_DATABASE_H",
        "",
        "#include <array>",
        "#include <algorithm>",
        '#include "types.h"',
        "",
        "namespace melonDS::GBACart::SaveDatabase",
        "{",
        "struct Entry",
        "{",
        "    std::array<u8, 20> SHA1;",
        "    u32 ROMLength;",
        "    u32 SaveLength;",
        "};",
        "",
        "inline constexpr Entry Entries[] =",
        "{",
    ]
    for digest, size, save in entries:
        values = ", ".join(f"0x{byte:02X}" for byte in digest)
        lines.append(f"    {{{{{values}}}, {size}, {save}}},")
    lines.extend([
        "};",
        "",
        "inline u32 Lookup(const std::array<u8, 20>& hash, u32 length) noexcept",
        "{",
        "    const auto* end = Entries + sizeof(Entries) / sizeof(Entries[0]);",
        "    const auto* entry = std::lower_bound(Entries, end, hash,",
        "        [](const Entry& item, const std::array<u8, 20>& value)",
        "        { return item.SHA1 < value; });",
        "    if (entry != end && entry->SHA1 == hash && entry->ROMLength == length)",
        "        return entry->SaveLength;",
        "    return 0;",
        "}",
        "}",
        "",
        "#endif // MELONDS_GBA_SAVE_DATABASE_H",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def write_header(output: Path, content: bytes) -> None:
    """Replace only after parsing, pin/conflict checks and rendering succeed."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.",
                                         suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml", type=Path, help="Original pinned MAME gba.xml (offline)")
    parser.add_argument("output", type=Path, help="Generated GBASaveDatabase.h")
    parser.add_argument("--check", action="store_true", help="Compare without writing")
    args = parser.parse_args(argv)
    try:
        if args.xml.resolve() == args.output.resolve():
            raise ValueError("Input and output paths must differ")
        xml = args.xml.read_bytes()
        digest = hashlib.sha256(xml).hexdigest()
        if digest != SOURCE_SHA256:
            raise ValueError(f"Source SHA-256 mismatch: expected {SOURCE_SHA256}, got {digest}")
        entries, counts = parse_database(xml)
        content = render_header(entries)
        if args.check:
            if args.output.read_bytes() != content:
                raise ValueError("Generated header differs; regenerate it from the pinned XML")
        else:
            write_header(args.output, content)
        print(json.dumps({
            "source_revision": SOURCE_REVISION,
            "source_sha256": digest,
            "header_sha256": hashlib.sha256(content).hexdigest(),
            "counts": dict(counts),
        }, sort_keys=True, indent=2))
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"generate-gba-save-db: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
