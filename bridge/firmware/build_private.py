#!/usr/bin/env python3
"""Offline private cross-build. No credential prompts, downloads, flashing or deployment."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name("build-manifest.json")
DEFAULT_SDK = Path("/private/tmp/simplecontroller-pico-sdk-2.3.1")
DEFAULT_TOOLS = Path("/private/tmp/simplecontroller-pico-tools")
ARM_ROOT = "arm-gnu-toolchain-15.2.rel1-darwin-arm64-arm-none-eabi"
BOARD_PLATFORMS = {"pico_w": "rp2040", "pico2_w": "rp2350-arm-s"}
TOOLS = {
    "cmake": "cmake-4.3.4-macos-universal/CMake.app/Contents/bin/cmake",
    "ninja": "ninja/ninja",
    "gcc": ARM_ROOT + "/bin/arm-none-eabi-gcc",
    "g++": ARM_ROOT + "/bin/arm-none-eabi-g++",
    "objcopy": ARM_ROOT + "/bin/arm-none-eabi-objcopy",
    "size": ARM_ROOT + "/bin/arm-none-eabi-size",
    "picotool": "picotool/picotool/picotool",
}
class BuildError(Exception):
    pass

def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()

def run_text(args):
    try:
        return subprocess.check_output([str(x) for x in args], stderr=subprocess.DEVNULL, text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        raise BuildError("A required pinned tool or SDK check failed; see firmware/README.md for the offline requirements.") from None

def check_dependencies(sdk, tools):
    source = json.loads(MANIFEST.read_text())
    for archive in source["tools"]:
        path = tools / archive["name"]
        if not path.is_file() or digest(path) != archive["sha256"]:
            raise BuildError("A required tool archive is missing or has the wrong checksum: " + archive["name"])
    pins = json.loads(Path(__file__).with_name("tool-binaries.json").read_text())
    for relative, expected in pins["sha256"].items():
        path = tools / relative
        if not path.is_file() or digest(path) != expected:
            raise BuildError("An extracted tool does not match the pinned binary: " + relative)
    for path, commit in [(sdk, source["sdk"]["commit"])] + [(sdk / path, commit) for path, commit in source["submodules"].items()]:
        if run_text(["git", "-C", path, "rev-parse", "HEAD"]) != commit:
            raise BuildError("The SDK or a required submodule is not at the pinned revision.")
        # Tracked local modifications must not silently enter a supposedly pinned build.
        if run_text(["git", "-C", path, "status", "--porcelain", "--untracked-files=no", "--ignore-submodules=untracked"]):
            raise BuildError("The SDK or a required submodule has tracked modifications.")
    return source

def exact_keys(value, expected, label):
    if not isinstance(value, dict) or set(value) != set(expected):
        raise BuildError("Invalid " + label + " fields. Use config.example.json; unknown fields are rejected.")

def config_values(raw, countries):
    exact_keys(raw, ["wifi", "usb", "xac_port"], "configuration")
    wifi, usb = raw["wifi"], raw["usb"]
    exact_keys(wifi, ["ssid", "password", "country"], "Wi-Fi")
    exact_keys(usb, ["vid", "pid", "power_ma", "identity_reviewed", "power_reviewed"], "USB")
    for key in ["ssid", "password"]:
        if not isinstance(wifi[key], str) or any(ord(char) < 32 or ord(char) == 127 for char in wifi[key]):
            raise BuildError("Wi-Fi strings must be text without control characters.")
    if not 1 <= len(wifi["ssid"].encode("utf-8")) <= 32:
        raise BuildError("SSID must be 1 through 32 UTF-8 bytes.")
    password = wifi["password"]
    if not (8 <= len(password.encode("utf-8")) <= 63 or re.fullmatch(r"[0-9A-Fa-f]{64}", password)):
        raise BuildError("This prototype supports WPA2 passphrases of 8–63 UTF-8 bytes or a 64-digit hexadecimal PSK.")
    if not isinstance(wifi["country"], str) or wifi["country"] not in countries:
        raise BuildError("Choose an explicit country name supported by the pinned CYW43 SDK.")
    for key in ["vid", "pid"]:
        if not isinstance(usb[key], str) or not re.fullmatch(r"0x[0-9a-fA-F]{4}", usb[key]) or int(usb[key], 16) in [0, 65535]:
            raise BuildError("Private builds require explicit non-placeholder four-digit hexadecimal USB IDs.")
    if type(usb["power_ma"]) is not int or not 2 <= usb["power_ma"] <= 500 or usb["power_ma"] % 2:
        raise BuildError("USB advertised power must be an even integer from 2 through 500 mA.")
    if usb["identity_reviewed"] is not True or usb["power_reviewed"] is not True:
        raise BuildError("USB identity and the power arrangement must be explicitly reviewed before a private build.")
    if raw["xac_port"] not in ["left", "right"]:
        raise BuildError("XAC port must be left or right.")
    return raw

def within(path, parent):
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False

def load_config(path, countries):
    path = path.resolve()
    # Never trust an arbitrary repository file to hold credentials. One ignored location is deliberate.
    if within(path, ROOT) and path != ROOT / "bridge/firmware/local_config.json":
        raise BuildError("Keep credentials outside the repository or in ignored bridge/firmware/local_config.json.")
    metadata = path.stat()
    if not stat.S_ISREG(metadata.st_mode) or metadata.st_mode & 0o077 or metadata.st_uid != os.getuid():
        raise BuildError("The local configuration must be owned by you with private permissions (chmod 600).")
    if metadata.st_size > 8192:
        raise BuildError("The configuration exceeds its 8 KiB limit.")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        raise BuildError("The local configuration is not valid UTF-8 JSON.") from None
    return config_values(raw, countries)

def public_compile_config(test_usb_identity=False):
    # 1209:0001 is the allocator-approved PRIVATE test ID, not a unique product ID.
    # Never distribute a device/image using it: https://pid.codes/1209/0001/
    return {"wifi": {"ssid": "SCX2-COMPILE-ONLY-NETWORK", "password": "SCX2-UNUSED-PLACEHOLDER", "country": "USA"},
            "usb": {"vid": "0x1209" if test_usb_identity else "0xffff", "pid": "0x0001" if test_usb_identity else "0xffff", "power_ma": 500, "identity_reviewed": False, "power_reviewed": False}, "xac_port": "left"}

def c_bytes(value):
    # Fixed-width octal escapes preserve UTF-8 and prevent C/preprocessor injection.
    return '"' + ''.join("\\%03o" % byte for byte in value.encode("utf-8")) + '"'

def header(config):
    return ("#pragma once\n#define SC_WIFI_SSID " + c_bytes(config["wifi"]["ssid"]) +
            "\n#define SC_WIFI_PASSWORD " + c_bytes(config["wifi"]["password"]) +
            "\n#define SC_WIFI_COUNTRY CYW43_COUNTRY_" + config["wifi"]["country"] +
            "\n#define SC_UDP_PORT 42736\n#define SC_XAC_RIGHT_PORT " + str(int(config["xac_port"] == "right")) + "\n")

def redacted_config(config, mode):
    return {"mode": mode, "wifi": "public compile-only placeholders" if mode == "compile-only" else "private; values omitted",
            "country": config["wifi"]["country"], "usb_vid": config["usb"]["vid"], "usb_pid": config["usb"]["pid"],
            "usb_power_ma": config["usb"]["power_ma"], "xac_port": config["xac_port"], "udp_port": 42736}

def public_sources():
    return [ROOT / path for path in ["bridge/include/scx2.hpp", "bridge/include/trigger_output.hpp", "bridge/src/scx2.cpp",
        "bridge/firmware/main.cpp", "bridge/firmware/usb_descriptors.cpp", "bridge/firmware/listener_retry.hpp",
        "bridge/firmware/mcp4251_driver.hpp", "bridge/firmware/pico_trigger_output.hpp",
        "bridge/firmware/lwipopts.h", "bridge/firmware/tusb_config.h", "bridge/firmware/config.example.h",
        "bridge/firmware/CMakeLists.txt", "bridge/firmware/build_private.py", "bridge/firmware/tool-binaries.json"]]

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=sorted(BOARD_PLATFORMS), default="pico_w", help="Exact board; default is RP2040 Pico W")
    parser.add_argument("--test-usb-identity", action="store_true", help="Compile-only: private test ID 1209:0001; never distribute or assume uniqueness")
    parser.add_argument("--mode", choices=["compile-only", "private"], default="compile-only")
    parser.add_argument("--trigger-output", choices=["none", "mcp4251"], default="none", help="mcp4251 requires the complete supervised analog circuit; never connect GPIO directly to XAC")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--sdk", type=Path, default=DEFAULT_SDK)
    parser.add_argument("--tools", type=Path, default=DEFAULT_TOOLS)
    parser.add_argument("--output", type=Path, help="New private directory outside the repository; defaults to /private/tmp")
    parser.add_argument("--validate-only", action="store_true")
    parser.add_argument("--deploy", action="store_true", help="Always refused: this tool never flashes/deploys")
    args = parser.parse_args(argv)
    if args.deploy:
        raise BuildError("Deployment is refused. Placeholder identities are not deployable, and this build-only tool never flashes any image.")
    if (args.mode == "private") != (args.config is not None):
        raise BuildError("Private mode requires --config; compile-only mode refuses credential files.")
    if args.test_usb_identity and args.mode != "compile-only":
        raise BuildError("--test-usb-identity is only for compile-only builds; private configuration must specify its own reviewed identity.")
    source = check_dependencies(args.sdk, args.tools)
    country_text = (args.sdk / "lib/cyw43-driver/src/cyw43_country.h").read_text()
    countries = set(re.findall(r"^#define CYW43_COUNTRY_([A-Z_]+)\s+CYW43_COUNTRY", country_text, re.MULTILINE))
    config = load_config(args.config, countries) if args.config else public_compile_config(args.test_usb_identity)
    if args.validate_only:
        print("Validated pinned dependencies and " + args.mode + " configuration. No files built, deployed or downloaded.")
        return 0
    os.umask(0o077)
    if args.output:
        output = args.output.resolve()
        if within(output, ROOT) or output.exists():
            raise BuildError("Output must be a new directory outside the repository; existing builds are never reused.")
        output.mkdir(mode=0o700, parents=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="simplecontroller-private-build-", dir="/private/tmp"))
    private_header = output / "private-config.h"
    private_header.write_text(header(config)); private_header.chmod(0o600)
    build = output / "build"
    sources = public_sources()
    source_before = {str(p.relative_to(ROOT)): digest(p) for p in sources}
    private_header_before = digest(private_header)  # Compare only; never publish a credential-file hash.
    commands = [[args.tools / TOOLS["cmake"], "-S", ROOT / "bridge/firmware", "-B", build, "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_MAKE_PROGRAM=" + str(args.tools / TOOLS["ninja"]),
                 "-DPICO_SDK_PATH=" + str(args.sdk), "-DPICO_BOARD=" + args.board, "-DPICO_TOOLCHAIN_PATH=" + str(args.tools / ARM_ROOT),
                 "-Dpicotool_DIR=" + str(args.tools / "picotool/picotool"), "-DSC_USB_VID=" + config["usb"]["vid"],
                 "-DSC_USB_PID=" + config["usb"]["pid"], "-DSC_USB_POWER_MA=" + str(config["usb"]["power_ma"]),
                 "-DSC_TRIGGER_MCP4251=" + ("ON" if args.trigger_output == "mcp4251" else "OFF"),
                 "-DSC_BRIDGE_CONFIG=" + str(private_header)],
                [args.tools / TOOLS["cmake"], "--build", build, "--parallel", "4"]]
    with (output / "build.log").open("w") as log:
        for command in commands:
            result = subprocess.run([str(x) for x in command], stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise BuildError("Cross-build failed. Private diagnostics: " + str(output / "build.log"))
    if source_before != {str(p.relative_to(ROOT)): digest(p) for p in sources} or digest(private_header) != private_header_before:
        raise BuildError("Build inputs changed during compilation. Success and manifest publication are refused; use a fresh build directory.")
    artifacts = {}
    for suffix in ["elf", "bin", "uf2"]:
        file = build / ("simplecontroller_bridge." + suffix)
        artifacts[file.name] = {"bytes": file.stat().st_size, "sha256": digest(file)}
    manifest = {"status": "Cross-built only; NOT hardware-proven or cleared for deployment", "board": args.board, "platform": BOARD_PLATFORMS[args.board],
                "sdk": source["sdk"], "submodules": source["submodules"], "tools": source["tools"],
                "configuration": {**redacted_config(config, args.mode), "trigger_output": args.trigger_output}, "artifacts": artifacts,
                "source_sha256": source_before,
                "credential_warning": "Private headers, build logs and firmware images may contain credentials. Do not share them.",
                "usb_identity_notice": "1209:0001 is a shared PRIVATE testing ID; never distribute/sell/manufacture with it. Check for another same-ID device before testing." if (config["usb"]["vid"].lower(), config["usb"]["pid"].lower()) == ("0x1209", "0x0001") else "No USB assignment/compliance is inferred from these values.",
                "hardware_proven": False, "deployable": False}
    (output / "redacted-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Cross-build passed: " + str(output))
    print("Redacted metadata: " + str(output / "redacted-manifest.json"))
    if args.test_usb_identity:
        print("PRIVATE TEST identity only: 1209:0001 is not unique. No redistribution; check for same-ID devices before bench use.")
    print("No deployment occurred. USB identity, power, provisioning/reset and XAC proof remain required.")
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (BuildError, OSError) as error:
        print("Build refused: " + str(error), file=sys.stderr)
        sys.exit(2)
