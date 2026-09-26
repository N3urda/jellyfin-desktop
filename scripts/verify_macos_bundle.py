#!/usr/bin/env python3
"""Fail packaging when a macOS bundle depends on files outside the app or OS.

Run after macdeployqt/fixup and before signing. This checks linked dependencies
and the known QtWebEngine/SDL runtime files; it does not replace a launch test.
"""

import argparse
import configparser
import plistlib
from pathlib import Path
import re
import subprocess
import sys


MACHO_MAGICS = {
    bytes.fromhex(value)
    for value in ("feedface", "cefaedfe", "feedfacf", "cffaedfe",
                  "cafebabe", "bebafeca", "cafebabf", "bfbafeca")
}


def run(*command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)


def is_macho(path):
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) in MACHO_MAGICS


def inside(path, root):
    return path.resolve().is_relative_to(root)


def system_path(value):
    return value.startswith(("/usr/lib/", "/System/Library/"))


def version(value):
    return tuple(int(part) for part in value.split(".")) + (0,) * (3 - len(value.split(".")))


def load_commands(path):
    output = run("otool", "-l", str(path))
    rpaths = re.findall(r"cmd LC_RPATH\s+cmdsize \d+\s+path (.*?) \(offset", output)
    minimums = re.findall(r"cmd LC_BUILD_VERSION[\s\S]*?\bminos ([\d.]+)", output)
    minimums += re.findall(r"cmd LC_VERSION_MIN_MACOSX\s+cmdsize \d+\s+version ([\d.]+)", output)
    return rpaths, minimums


def expand(value, loader, executable):
    if value == "@loader_path" or value.startswith("@loader_path/"):
        return loader / value[len("@loader_path"):].lstrip("/")
    if value == "@executable_path" or value.startswith("@executable_path/"):
        return executable / value[len("@executable_path"):].lstrip("/")
    if value.startswith("/"):
        return Path(value)
    return None


def verify(app, arch=None, max_macos=None):
    app = app.resolve()
    errors = []
    contents = app / "Contents"
    plist_path = contents / "Info.plist"
    if not plist_path.is_file():
        return ["Missing Contents/Info.plist"], 0
    with plist_path.open("rb") as stream:
        plist = plistlib.load(stream)
    main = contents / "MacOS" / plist["CFBundleExecutable"]
    frameworks = contents / "Frameworks"
    webengine = frameworks / "QtWebEngineCore.framework"
    helper = webengine / "Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
    resources = webengine / "Resources"
    required = [main, helper, webengine / "QtWebEngineCore",
                contents / "PlugIns/platforms/libqcocoa.dylib",
                resources / "icudtl.dat", resources / "qtwebengine_resources.pak",
                resources / "qtwebengine_resources_100p.pak",
                resources / "qtwebengine_resources_200p.pak",
                contents / "Resources/qml/QtWebEngine/qmldir"]
    for path in required:
        if not path.is_file():
            errors.append(f"Missing runtime file: {path.relative_to(app)}")
    if not list((resources / "qtwebengine_locales").glob("*.pak")):
        errors.append("Missing QtWebEngine locale packs")
    if not list(resources.glob("v8_context_snapshot*.bin")):
        errors.append("Missing QtWebEngine V8 context snapshot")
    qt_conf = contents / "Resources/qt.conf"
    if qt_conf.is_file():
        parser = configparser.ConfigParser()
        parser.read(qt_conf)
        if parser.has_section("Paths"):
            for key, value in parser.items("Paths"):
                if value.startswith("/"):
                    errors.append(f"qt.conf {key} references an absolute build path: {value}")

    deployment = plist.get("LSMinimumSystemVersion", "")
    if not re.fullmatch(r"\d+(\.\d+){0,2}", deployment):
        errors.append("Info.plist has no valid LSMinimumSystemVersion")
    elif max_macos and version(deployment) > version(max_macos):
        errors.append(f"Info.plist requires macOS {deployment}, above release target {max_macos}")

    binaries = sorted({path.resolve() for path in app.rglob("*") if is_macho(path)})
    if not binaries:
        errors.append("No Mach-O binaries found")
    commands = {path: load_commands(path) for path in binaries}
    main_rpaths = commands.get(main.resolve(), ([], []))[0]
    helper_rpaths = commands.get(helper.resolve(), ([], []))[0]
    highest_minimum = "0"

    for binary in binaries:
        if not inside(binary, app):
            errors.append(f"Executable symlink escapes app: {binary}")
            continue
        label = binary.relative_to(app)
        if arch and arch not in run("lipo", "-archs", str(binary)).split():
            errors.append(f"{label}: missing {arch} architecture")
        rpaths, minimums = commands[binary]
        for minimum in minimums:
            if version(minimum) > version(highest_minimum):
                highest_minimum = minimum
            if max_macos and version(minimum) > version(max_macos):
                errors.append(f"{label}: requires macOS {minimum}, above target {max_macos}")
            if deployment and re.fullmatch(r"\d+(\.\d+){0,2}", deployment):
                if version(minimum) > version(deployment):
                    errors.append(f"{label}: requires macOS {minimum}, but Info.plist advertises {deployment}")

        is_helper = "QtWebEngineProcess.app" in binary.parts
        executable = helper.resolve().parent if is_helper else main.parent
        inherited = helper_rpaths if is_helper else main_rpaths
        search = []
        for value in rpaths:
            candidate = expand(value, binary.parent, executable)
            if candidate is not None:
                search.append(candidate)
        for value in inherited:
            candidate = expand(value, executable, executable)
            if candidate is not None:
                search.append(candidate)
        output = run("otool", "-L", str(binary))
        for dependency in re.findall(r"^\s+(.+?) \(compatibility version", output, re.MULTILINE):
            if system_path(dependency):
                continue
            if dependency.startswith("/"):
                errors.append(f"{label}: external absolute dependency {dependency}")
                continue
            if dependency.startswith("@rpath/"):
                candidates = [path / dependency[len("@rpath/"):] for path in search]
            else:
                candidate = expand(dependency, binary.parent, executable)
                candidates = [candidate] if candidate is not None else []
            if not any(path.is_file() and inside(path, app) for path in candidates):
                errors.append(f"{label}: dependency is not resolvable inside app: {dependency}")

    sdl2 = frameworks / "libSDL2-2.0.0.dylib"
    if sdl2.is_file() and b"libSDL3.dylib" in sdl2.read_bytes():
        if not (frameworks / "libSDL3.dylib").is_file():
            errors.append("sdl2-compat requires bundled libSDL3.dylib")
    print(f"Checked {len(binaries)} Mach-O files; highest minimum macOS: {highest_minimum}")
    return errors, len(binaries)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--arch", help="Require this architecture in every Mach-O file")
    parser.add_argument("--max-macos", help="Fail if any binary requires a newer macOS")
    args = parser.parse_args()
    try:
        errors, _ = verify(args.app, args.arch, args.max_macos)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"Bundle verification failed: {error}", file=sys.stderr)
        return 1
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print("macOS bundle dependency verification passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
