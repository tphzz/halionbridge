"""Native CI build driver. No HALion instance is loaded by this script."""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

import release


def run(*command, capture=False):
    print("+ " + " ".join(map(str, command)), flush=True)
    result = subprocess.run(list(map(str, command)), check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout or ""


def parallelism(value):
    if not re.fullmatch(r"[1-9][0-9]*", value):
        raise ValueError("CI_BUILD_PARALLEL_LEVEL must be a positive integer")
    return value


def validate_macos_dependencies(output):
    names = [line.strip().split(" ")[0] for line in output.splitlines()[1:] if line.strip()]
    if not names or any(not name.startswith(("/usr/lib/", "/System/Library/Frameworks/")) for name in names):
        raise ValueError("Unexpected macOS dynamic dependency")


def main():
    if sys.version_info < (3, 10):
        raise ValueError("CI helpers require Python 3.10 or newer")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("platform", choices=release.PLATFORMS)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--notarize", action="store_true")
    args = parser.parse_args()
    if args.notarize and args.platform != "macos-universal":
        parser.error("Notarization is only supported on macOS")
    source = Path.cwd()
    preset = "debug" if args.debug else "release"
    configuration = "Debug" if args.debug else "Release"
    os.environ["CONFIGURATION"] = configuration
    os.environ["HALIONBRIDGE_FORBID_PLUGIN_INSTANTIATION"] = "1"
    ctx = release.context()
    jobs = parallelism(os.environ.get("CI_BUILD_PARALLEL_LEVEL") or "2")
    for tool in ("cmake", "ninja", "clang-format", "git"):
        if not shutil.which(tool):
            raise ValueError(f"Required preinstalled tool not found: {tool}")
        run(tool, "--version")
    if run("git", "rev-parse", "HEAD", capture=True).strip() != ctx["sha"]:
        raise ValueError("Checkout does not match dispatch SHA")
    if run("git", "status", "--porcelain", capture=True).strip():
        raise ValueError("CI checkout is not clean")
    files = run("git", "ls-files", "*.cpp", "*.h", ":!:external/*", capture=True).splitlines()
    run("clang-format", "--dry-run", "--Werror", *files)
    configure = ["cmake", "--preset", preset]
    if args.platform == "macos-universal":
        if platform.system() != "Darwin" or platform.machine().lower() != "arm64":
            raise ValueError("macOS Universal requires an Apple Silicon runner")
        run("xcodebuild", "-version")
        run("/usr/bin/arch", "-x86_64", "/usr/bin/true")
        configure += ["-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3"]
    else:
        expected = "arm64" if args.platform.endswith("arm64") else "x64"
        if os.environ.get("VSCMD_ARG_TGT_ARCH", "").lower() != expected or not os.environ.get("VisualStudioVersion", "").startswith("18."):
            raise ValueError("Expected matching Visual Studio 2026 native toolchain")
    run(*configure)
    run("cmake", "--build", "--preset", preset, "--parallel", jobs)
    build = source / ("build" if args.debug else "build-release")
    run("ctest", "--test-dir", build, "--output-on-failure", "--parallel", jobs)
    binary = build / "halionbridge_artefacts" / configuration / ("halionbridge" if args.platform == "macos-universal" else "halionbridge.exe")
    basename = (build / "halionbridge_package_basename.txt").read_text().strip()
    version = run(binary, "--version", capture=True)
    print(version)
    if basename not in version or ctx["sha"][:7] not in version or "source: clean" not in version:
        raise ValueError("Executable version does not match clean source identity")
    scratch = source / "build-ci" / "diagnostics"
    scratch.mkdir(parents=True, exist_ok=True)
    keychain = None
    if args.platform == "macos-universal":
        for arch in ("arm64", "x86_64"):
            run("lipo", binary, "-verify_arch", arch)
            run("/usr/bin/arch", "-" + arch, binary, "--version")
            build_info = run("xcrun", "vtool", "-show-build", "-arch", arch, binary, capture=True)
            if not re.search(r"minos\s+13\.3(?:\s|$)", build_info):
                raise ValueError("Unexpected macOS deployment target")
            validate_macos_dependencies(run("otool", "-arch", arch, "-L", binary, capture=True))
        run("/usr/bin/arch", "-x86_64", build / "halionbridge_tests_artefacts" / configuration / "halionbridge_tests")
        import macos
        keychain = macos.sign(binary, source / "packaging/macos/halionbridge.entitlements", scratch)
        for arch in ("arm64", "x86_64"):
            run("/usr/bin/arch", "-" + arch, binary, "--version")
    else:
        headers = run("dumpbin", "/HEADERS", binary, capture=True)
        machine = "AA64" if args.platform.endswith("arm64") else "8664"
        if not re.search(rf"\b{machine}\s+machine\b", headers, re.I):
            raise ValueError("Unexpected Windows executable architecture")
        dependencies = run("dumpbin", "/DEPENDENTS", binary, capture=True)
        if re.search(r"VCRUNTIME|MSVCP|CONCRT|UCRTBASE|api-ms-win-crt", dependencies, re.I):
            raise ValueError("Unexpected dynamic MSVC runtime dependency")
    archive = release.package(source, binary, source / "build-ci/packages", basename, args.platform, ctx)
    if args.notarize:
        macos.notarize(archive, keychain, scratch)
    release.write_manifest(archive, args.platform, ctx)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"CI build failed: {error}")
