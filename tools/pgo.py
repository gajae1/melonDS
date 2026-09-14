#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bind GCC core profiles to their source, compiler and compile commands."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
from source_identity import SourceIdentityError, working_source_identity


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def write(path, value):
    text = json.dumps(value, indent=2, sort_keys=True) + "\n"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def split_command(command):
    if os.name != "nt":
        return shlex.split(command)
    split = ctypes.windll.shell32.CommandLineToArgvW
    split.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    split.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    pointer = split(command, ctypes.byref(count))
    if not pointer:
        raise ValueError("Cannot parse the compiler command")
    try:
        return [pointer[i] for i in range(count.value)]
    finally:
        free = ctypes.windll.kernel32.LocalFree
        free.argtypes = [ctypes.c_void_p]
        free(pointer)


def compiler(path):
    path = Path(path).resolve(strict=True)
    return dict(path=str(path), sha256=digest(path),
                version=subprocess.check_output([str(path), "--version"], text=True).strip(),
                target=subprocess.check_output([str(path), "-dumpmachine"], text=True).strip())


def identity(context):
    return dict(schema=1, source=working_source_identity(context["repo"]),
                compilers=[compiler(path) for path in context["compilers"]])


def normalize(value, context):
    value = value.replace("\\", "/")
    for key, token in (("build", "@BUILD@"), ("repo", "@SOURCE@")):
        value = value.replace(Path(context[key]).as_posix(), token)
    return value


def commands(context):
    result = {}
    for entry in read(Path(context["build"]) / "compile_commands.json"):
        args = entry.get("arguments") or split_command(entry["command"])
        if "-o" not in args:
            continue
        output = Path(entry["directory"]) / args[args.index("-o") + 1]
        output = output.resolve()
        try:
            relative = output.relative_to(context["build"]).as_posix()
        except ValueError:
            continue
        if not relative.startswith("src/CMakeFiles/core.dir/") or Path(entry["file"]).suffix not in (".c", ".cpp"):
            continue
        canonical = []
        index = 0
        while index < len(args):
            arg = args[index]
            index += 1
            if arg in ("-o", "-MF", "-MT", "-MQ"):
                index += 1
                continue
            if arg == "-fprofile-update=atomic" or arg.startswith((
                    "-fprofile-generate=", "-fprofile-use=", "-fprofile-prefix-path=",
                    "-Werror=missing-profile", "-Werror=coverage-mismatch")):
                continue
            canonical.append(normalize(arg, context))
        # GCC removes only the final object suffix before mangling the path.
        name = str(Path(relative).with_suffix(".gcda")).replace("\\", "/").replace("/", "#")
        if name in result:
            raise ValueError("Duplicate core profile object identity")
        result[name] = dict(source=normalize(str(Path(entry["file"]).resolve()), context), args=canonical)
    if not result:
        raise ValueError("No GCC core compile commands found")
    return result


def profile_files(profile):
    directory = profile / "data"
    return {p.relative_to(directory).as_posix(): digest(p) for p in sorted(directory.rglob("*.gcda"))}


def verify(context, with_commands=True):
    profile = Path(context["profile"])
    manifest = read(profile / "profile.json")
    if manifest["identity"] != identity(context):
        raise ValueError("PGO source or compiler changed; collect a new profile")
    if context["mode"] == "USE":
        if not manifest.get("sealed"):
            raise ValueError("PGO training is not sealed")
        if manifest["files"] != profile_files(profile):
            raise ValueError("PGO data changed or is missing after sealing")
    elif manifest.get("sealed"):
        raise ValueError("Sealed PGO data cannot be used for further training")
    if with_commands:
        current = commands(context)
        if "commands" not in manifest and context["mode"] == "GENERATE":
            if profile_files(profile):
                raise ValueError("Unattributed profile data exists before the first build")
            manifest["commands"] = current
            write(profile / "profile.json", manifest)
        elif manifest.get("commands") != current:
            raise ValueError("PGO compile commands changed; collect a new profile")
    return manifest


def cmake_quote(value):
    if "]=]" in value:
        raise ValueError("Unsupported CMake path delimiter")
    return "[=[" + value + "]=]"


def configure(args):
    context = dict(repo=str(args.repo.resolve(strict=True)), build=str(args.build.resolve(strict=True)),
                   profile=str(args.profile.resolve()), mode=args.mode,
                   compilers=[str(args.cc.resolve(strict=True)), str(args.cxx.resolve(strict=True))])
    profile = Path(context["profile"])
    if args.mode == "GENERATE" and not profile.exists():
        profile.mkdir(parents=True)
        (profile / "data").mkdir()
        write(profile / "profile.json", dict(identity=identity(context), sealed=False))
    manifest = verify(context, with_commands=False)
    if args.mode == "USE":
        lines = []
        for name in manifest["files"]:
            source = manifest["commands"][name]["source"].replace("@SOURCE@", Path(context["repo"]).as_posix()).replace("@BUILD@", Path(context["build"]).as_posix())
            options = ["-fprofile-use=" + (profile / "data").as_posix(),
                       "-fprofile-prefix-path=" + str(Path(context["build"])),
                       "-Werror=missing-profile", "-Werror=coverage-mismatch"]
            lines.append("set_property(SOURCE " + cmake_quote(source) + " DIRECTORY " +
                         cmake_quote((Path(context["repo"]) / "src").as_posix()) +
                         " APPEND PROPERTY COMPILE_OPTIONS " + " ".join(map(cmake_quote, options)) + ")")
        (args.build / "pgo-use.cmake").write_text("\n".join(lines) + "\n", encoding="utf-8")
    write(args.build / "pgo-context.json", context)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    setup = sub.add_parser("configure")
    setup.add_argument("--repo", type=Path, required=True)
    setup.add_argument("--profile", type=Path, required=True)
    setup.add_argument("--mode", choices=("GENERATE", "USE"), required=True)
    setup.add_argument("--cc", type=Path, required=True)
    setup.add_argument("--cxx", type=Path, required=True)
    for child in (setup, sub.add_parser("verify"), sub.add_parser("seal")):
        child.add_argument("--build", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "configure":
        configure(args)
        return
    context = read(args.build / "pgo-context.json")
    manifest = verify(context)
    if args.command == "seal":
        if context["mode"] != "GENERATE":
            raise ValueError("Seal the training build, not the use build")
        files = profile_files(Path(context["profile"]))
        if not files or not files.keys() <= manifest["commands"].keys():
            raise ValueError("Missing training data or profile files outside the recorded core objects")
        manifest.update(sealed=True, files=files)
        write(Path(context["profile"]) / "profile.json", manifest)
        print(f"Sealed {len(files)} profiled core units; {len(manifest['commands']) - len(files)} units remain unprofiled")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, SourceIdentityError, subprocess.CalledProcessError) as error:
        sys.exit("PGO rejected: " + str(error))
