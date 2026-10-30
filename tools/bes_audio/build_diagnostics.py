#!/usr/bin/env python3
"""Verify or rebuild the two historical BES audio diagnostics; never flash a board."""

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tarfile


REPO = Path(__file__).resolve().parents[2]
SOURCES = REPO / "diagnostics" / "bes_audio"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def checked_bytes(path, expected):
    data = path.read_bytes()
    if len(data) != expected["size"] or sha256(data) != expected["sha256"]:
        raise ValueError("size or SHA256 mismatch: " + str(path))
    return data


def patch_image(base, payload, layout):
    """Apply one replacement only to the exact archived AP image."""
    if len(base) != layout["image_size"] or sha256(base) != layout["base_sha256"]:
        raise ValueError("unsupported base image")
    if struct.unpack_from("<I", base, len(base) - 4)[0] != layout["flash_address"]:
        raise ValueError("unexpected AP footer")
    capacity = layout["capacity"]
    offset = layout["entry_address"] - layout["runtime_base"]
    if capacity % 2 or not payload or len(payload) > capacity or len(payload) % 2:
        raise ValueError("payload must fit the even-sized replacement area")
    if offset < 0 or offset + capacity > len(base) - 4:
        raise ValueError("replacement area crosses image boundary")
    replacement = payload + b"\x00\xbf" * ((capacity - len(payload)) // 2)
    return base[:offset] + replacement + base[offset + capacity:]


def read_bundle(metadata, output):
    archive = REPO / metadata["bundle"]["path"]
    checked_bytes(archive, metadata["bundle"])
    destination = output / "baseline"
    destination.mkdir(parents=True, exist_ok=True)
    expected = metadata["bundle_members"]
    with tarfile.open(archive, "r:xz") as bundle:
        members = bundle.getmembers()
        if len(members) != len(expected) or {m.name for m in members} != set(expected):
            raise ValueError("unexpected archive members")
        for member in members:
            if not member.isfile() or member.size != expected[member.name]["size"]:
                raise ValueError("unexpected archive member type or size")
            stream = bundle.extractfile(member)
            if stream is None:
                raise ValueError("unreadable archive member")
            data = stream.read()
            if sha256(data) != expected[member.name]["sha256"]:
                raise ValueError("archive member SHA256 mismatch: " + member.name)
            # No extractall: only the exact flat names in the checked manifest are written.
            if Path(member.name).name != member.name or "/" in member.name or "\\" in member.name:
                raise ValueError("archive member must have a flat filename")
            (destination / member.name).write_bytes(data)
    return destination


def run(command, output):
    result = subprocess.run(command, capture_output=True, text=True)
    output.append({"command": command, "returncode": result.returncode,
                   "stdout": result.stdout, "stderr": result.stderr})
    if result.returncode:
        raise RuntimeError("command failed: " + command[0] + "\n" + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--toolchain-prefix", default="arm-none-eabi-")
    parser.add_argument("--output", type=Path, default=REPO / "out" / "bes_audio")
    args = parser.parse_args()
    metadata = json.loads((SOURCES / "baseline.json").read_text(encoding="utf-8"))
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    report = {"board_operations": False, "commands": [], "artifacts": []}
    try:
        for relative, expected in metadata["sources"].items():
            checked_bytes(REPO / relative, expected)
        baseline = read_bundle(metadata, output)
        base = (baseline / metadata["base_image"]).read_bytes()
        for variant, item in metadata["variants"].items():
            archived = (baseline / item["image"]).read_bytes()
            offset = metadata["layout"]["entry_address"] - metadata["layout"]["runtime_base"]
            payload = archived[offset:offset + item["payload_size"]]
            if patch_image(base, payload, metadata["layout"]) != archived:
                raise ValueError("archived image changes bytes outside the diagnostic: " + variant)
        report["archive_verified"] = True
        if args.verify_only:
            report["result"] = "archive and sources verified; no compilation or board operation"
        else:
            compiler = args.toolchain_prefix + "gcc"
            objcopy = args.toolchain_prefix + "objcopy"
            report["compiler"] = run([compiler, "--version"], report["commands"]).splitlines()[0]
            for variant, item in metadata["variants"].items():
                elf = output / (variant + ".elf")
                payload_path = output / (variant + ".payload.bin")
                command = [compiler, "-mthumb", "-mcpu=cortex-m33", "-Os",
                           "-ffreestanding", "-fno-builtin", "-fno-unwind-tables",
                           "-fno-asynchronous-unwind-tables", "-nostdlib",
                           "-Wl,--build-id=none", "-Wl,--just-symbols=" + str(baseline / "base_symbols.elf"),
                           "-Wl,-T," + str(SOURCES / "probe.ld"), str(REPO / item["source"]),
                           "-o", str(elf)]
                run(command, report["commands"])
                run([objcopy, "-O", "binary", "--only-section=.text", str(elf), str(payload_path)],
                    report["commands"])
                payload = payload_path.read_bytes()
                if len(payload) != item["payload_size"] or sha256(payload) != item["payload_sha256"]:
                    raise ValueError("payload differs from the verified build: " + variant)
                image = patch_image(base, payload, metadata["layout"])
                if sha256(image) != item["sha256"]:
                    raise ValueError("rebuilt image differs from the verified build: " + variant)
                target = output / item["image"]
                target.write_bytes(image)
                report["artifacts"].append({"variant": variant, "filename": target.name,
                                            "size": len(image), "sha256": sha256(image),
                                            "matches_verified_image": True})
            report["result"] = "both rebuilt images match the archived verified images byte for byte"
    except Exception as error:
        report["result"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        (output / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "commands"}, indent=2))


if __name__ == "__main__":
    main()
