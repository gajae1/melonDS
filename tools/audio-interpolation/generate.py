#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate candidate banks offline; never install them in the emulator."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import struct
import subprocess
import sys
import urllib.request
import zipfile

os.environ["OPENBLAS_NUM_THREADS"] = "1"
import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def source_tree(directory, output, manifest):
    if directory is None:
        archive = output / "r8brain.zip"
        urllib.request.urlretrieve(manifest["url"], archive)
        if sha(archive) != manifest["archive_sha256"]:
            raise ValueError("Pinned r8brain archive checksum mismatch")
        destination = output / "upstream"
        with zipfile.ZipFile(archive) as z:
            for member in z.infolist():
                if not (destination / member.filename).resolve().is_relative_to(destination.resolve()):
                    raise ValueError("Archive path escapes source directory")
            z.extractall(destination)
        directory = destination / ("r8brain-free-src-" + manifest["commit"])
    directory = directory.resolve(strict=True)
    expected = {entry["file"]: entry["sha256"] for entry in manifest["files"]}
    actual = {p.relative_to(directory).as_posix(): sha(p)
              for p in directory.rglob("*") if p.is_file() and ".git" not in p.relative_to(directory).parts}
    if actual != expected:
        raise ValueError("r8brain source tree differs from the pinned snapshot")
    return directory


def responses(path, mix):
    data = path.read_bytes()
    if struct.unpack_from("<3I", data) != (0x31524648, mix, mix):
        raise ValueError("Invalid reference header")
    offset, result = 12, []
    for period in range(1, mix + 1):
        p, length = struct.unpack_from("<2I", data, offset)
        offset += 8
        if p != period or not 2 <= length <= 50000:
            raise ValueError("Invalid reference record")
        values = np.frombuffer(data, dtype="<f8", count=length, offset=offset)
        offset += 8 * length
        if not np.isfinite(values).all():
            raise ValueError("Nonfinite reference")
        result.append(values)
    if offset != len(data):
        raise ValueError("Trailing reference data")
    return result


def solve(basis, targets):
    coefficients, _, rank, _ = np.linalg.lstsq(basis, targets, rcond=None)
    if rank != 16 or not np.isfinite(coefficients).all():
        raise ValueError("Rank-deficient or nonfinite fit")
    return coefficients


def fit(reference, mix, output):
    # This stage has no access to the frozen banks or historical fit outputs.
    kernels = responses(reference, mix)
    bases = {span: np.polynomial.chebyshev.chebvander(1 - 2 * np.arange(span) / span, 15)
             for span in (mix, 256)}
    inverses = {span: solve(basis, np.eye(span)) for span, basis in bases.items()}
    targets = []
    for response in kernels[:32]:
        rows = (len(response) + mix - 1) // mix + 1
        residual = np.zeros(rows * mix)
        residual[:len(response) - 1] = 1 - response[:-1]
        targets.append(residual.reshape(rows, mix).T)
    dense = solve(bases[mix], np.concatenate(targets, axis=1))
    bank = bytearray(struct.pack("<3I", 0x31425148, mix, mix))
    column = 0
    for p, response in enumerate(kernels, start=1):
        length = len(response)
        if p == mix:
            bank += struct.pack("<3I", p, length, 0) + response.astype("<f8").tobytes()
            continue
        span = mix if p <= 256 else 256
        rows = (length + span - 1) // span + (p <= 32)
        if p <= 32:
            coefficients = dense[:, column:column + rows].T
            column += rows
        else:
            residual = np.zeros(rows * span)
            residual[:length - 1] = 1 - response[:-1]
            coefficients = (inverses[span] @ residual.reshape(rows, span).T).T
        if not np.isfinite(coefficients).all():
            raise ValueError("Nonfinite coefficient")
        bank += struct.pack("<3I", p, length, rows) + coefficients.astype("<f8").tobytes()
    with output.open("xb") as stream:
        stream.write(bank)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path, help="New directory for candidates and report")
    parser.add_argument("--compiler", default="g++", help="GCC or Clang C++ compiler")
    parser.add_argument("--r8brain-dir", type=Path, help="Exact pinned source tree; otherwise download verified archive")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.is_relative_to(ROOT / "src"):
        raise ValueError("Output must be outside the product source directory")
    output.mkdir(parents=True, exist_ok=False)
    manifest = json.loads((HERE / "upstream.json").read_text())
    upstream = source_tree(args.r8brain_dir, output, manifest)
    helper = output / ("reference.exe" if os.name == "nt" else "reference")
    command = [args.compiler, "-std=c++20", "-O3", "-I" + str(upstream), "-I" + str(ROOT / "src"),
               str(HERE / "reference.cpp"), str(ROOT / "src/AudioInterpolationBank.cpp"), "-o", str(helper)]
    subprocess.run(command, check=True)
    report = dict(python=platform.python_version(), numpy=np.__version__,
                  compiler=run(args.compiler, "--version"), compile_command=command,
                  upstream=manifest, tool_sha256={p.name: sha(p) for p in (HERE / "generate.py", HERE / "reference.cpp")},
                  tolerance=1e-12, banks=[])
    for mix in (352, 512):
        raw = output / f"reference-{mix}.bin"
        generated = output / f"bank-{mix}.coeff"
        reference_report = json.loads(run(helper, "kernels", mix, raw))
        fit(raw, mix, generated)
        # Read the frozen bank only after independent generation is complete.
        frozen = ROOT / "src/audio_interpolation" / generated.name
        digest = sha(frozen)
        comparison = json.loads(run(helper, "compare", generated, frozen))
        if digest != sha(frozen):
            raise ValueError("Frozen bank changed during comparison")
        entry = dict(reference=reference_report, comparison=comparison, bytes=generated.stat().st_size,
                     generated_sha256=sha(generated), frozen_sha256=digest, bit_exact=sha(generated) == digest)
        report["banks"].append(entry)
        print(json.dumps(entry), flush=True)
    report["limitations"] = ("Numerical regeneration comparison only; not a new audio quality or stream-error bound. "
                             "Does not recover the historical metadata writer revision or install candidate banks.")
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
