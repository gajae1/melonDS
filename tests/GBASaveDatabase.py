# SPDX-License-Identifier: GPL-3.0-or-later
"""ROM-free acceptance for the offline GBA database generator."""

import contextlib
import copy
import hashlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
EVIDENCE = Path(__file__).resolve().parent
ROOT = EVIDENCE.parent
TEMP_ROOT = Path(tempfile.gettempdir()).resolve()
GENERATOR = ROOT / "tools/generate-gba-save-db.py"
spec = importlib.util.spec_from_file_location("gba_save_generator", GENERATOR)
db = importlib.util.module_from_spec(spec)
spec.loader.exec_module(db)


def software(name="sample", digest="10" * 20, slot="gba_eeprom_4k", size=4096):
    item = ET.Element("software", name=name)
    part = ET.SubElement(item, "part", name="cart", interface="gba_cart")
    ET.SubElement(part, "feature", name="slot", value=slot)
    area = ET.SubElement(part, "dataarea", name="rom", size=str(size))
    ET.SubElement(area, "rom", name="public-metadata-only.gba", size=str(size), sha1=digest)
    return item


def xml(*items):
    root = ET.Element("softwarelist", name="gba")
    root.extend(items)
    return ET.tostring(root)


class GeneratorAcceptance(unittest.TestCase):
    def test_four_capacities_full_hash_original_length_and_dedup(self):
        # The last-byte difference catches accidental truncation to a short hash.
        hashes = ["10" * 19 + "01", "10" * 19 + "02", "00" * 20, "ff" * 20]
        slots = ["gba_eeprom_4k", "gba_eeprom_64k", "gba_yoshiug", "gba_boktai"]
        items = [software(str(i), digest, slot, 12345 + i)
                 for i, (digest, slot) in enumerate(zip(hashes, slots))]
        items[3].find("part/dataarea/rom").set("status", "good")
        duplicate = copy.deepcopy(items[3])
        duplicate.set("name", "same-dump-other-label")
        duplicate.find("part/dataarea/rom").set("sha1", hashes[3].upper())
        entries, counts = db.parse_database(xml(*items, duplicate))
        self.assertEqual(entries, sorted([
            (bytes.fromhex(hashes[0]), 12345, 512),
            (bytes.fromhex(hashes[1]), 12346, 8192),
            (bytes.fromhex(hashes[2]), 12347, 512),
            (bytes.fromhex(hashes[3]), 12348, 8192),
        ]))
        self.assertEqual(counts["eligible_rows"], 5)
        self.assertEqual(counts["identical_duplicates"], 1)

    def test_filtering_cannot_create_a_fixed_capacity(self):
        invalid = []

        def change(path, attribute, value):
            item = software()
            item.find(path).set(attribute, value)
            invalid.append(item)

        change("part", "name", "not-cart")
        change("part", "interface", "gba_cart_extra")
        change("part/feature", "value", "gba_eeprom")
        change("part/feature", "value", "gba_sram")
        change("part/dataarea", "name", "not-rom")
        change("part/dataarea", "size", "8192")
        for value in ("baddump", "nodump", "unknown"):
            change("part/dataarea/rom", "status", value)
        change("part/dataarea/rom", "loadflag", "reload")
        change("part/dataarea/rom", "offset", "0x10")
        for value in ("-1", "0", "4294967296", "four thousand"):
            item = software(size=value)
            invalid.append(item)
        change("part/dataarea/rom", "sha1", "ab" * 19)
        change("part/dataarea/rom", "sha1", "zz" * 20)
        missing_slot = software()
        missing_slot.find("part").remove(missing_slot.find("part/feature"))
        invalid.append(missing_slot)
        for path, tag, attrs in (
            ("part", "feature", {"name": "slot", "value": "gba_eeprom_64k"}),
            ("part", "dataarea", {"name": "rom", "size": "4096"}),
            ("part/dataarea", "rom", {"size": "4096", "sha1": "20" * 20}),
        ):
            item = software()
            ET.SubElement(item.find(path), tag, attrs)
            invalid.append(item)

        valid = software(digest="30" * 20, slot="gba_boktai")
        valid.find("part/dataarea/rom").set("offset", "0x0")
        valid.find("part/dataarea").set("size", "0x1000")
        entries, counts = db.parse_database(xml(*invalid, valid))
        self.assertEqual(entries, [(bytes.fromhex("30" * 20), 4096, 8192)])
        self.assertEqual(sum(v for k, v in counts.items() if k.startswith("excluded_")),
                         len(invalid))
        self.assertEqual(counts["excluded_unknown_eeprom"], 1)

    def test_duplicate_capacity_and_length_conflicts_are_errors(self):
        first = software()
        with self.assertRaisesRegex(ValueError, "Conflicting save capacities"):
            db.parse_database(xml(first, software(slot="gba_eeprom_64k")))
        with self.assertRaisesRegex(ValueError, "Inconsistent ROM lengths"):
            db.parse_database(xml(first, software(size=8192)))

    def test_malformed_document_and_empty_output_are_rejected(self):
        with self.assertRaises(ET.ParseError):
            db.parse_database(b"<softwarelist")
        with self.assertRaisesRegex(ValueError, "softwarelist root"):
            db.parse_database(b'<softwarelist name="other"/>')
        with self.assertRaisesRegex(ValueError, "No fixed-capacity"):
            db.render_header(db.parse_database(xml(software(slot="gba_eeprom")))[0])

    def test_output_is_independent_of_record_order(self):
        items = [software(digest="ff" * 20), software(digest="00" * 20, size=8192)]
        first = db.render_header(db.parse_database(xml(*items))[0])
        second = db.render_header(db.parse_database(xml(*reversed(items)))[0])
        self.assertEqual(first, second)
        self.assertNotIn(b"\r", first)

    def test_cli_pin_and_validation_failures_preserve_existing_output(self):
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            case = Path(directory).resolve()
            self.assertEqual(case.parent, TEMP_ROOT)
            source, output = case / "input.xml", case / "existing.h"
            sentinel = b"existing output must survive\n"
            source.write_bytes(xml(software()))
            output.write_bytes(sentinel)
            result = subprocess.run([sys.executable, "-B", str(GENERATOR),
                                     str(source), str(output)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("Source SHA-256 mismatch", result.stderr)
            self.assertEqual(output.read_bytes(), sentinel)

            # In-process pin injection exercises the post-pin failure boundary.
            # The production CLI exposes no way to bypass its source pin.
            for content in (b"<invalid", xml(software(), software(slot="gba_eeprom_64k"))):
                source.write_bytes(content)
                with mock.patch.object(db, "SOURCE_SHA256", hashlib.sha256(content).hexdigest()):
                    with contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(db.main([str(source), str(output)]), 1)
                self.assertEqual(output.read_bytes(), sentinel)
            self.assertEqual(sorted(p.name for p in case.iterdir()), ["existing.h", "input.xml"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
