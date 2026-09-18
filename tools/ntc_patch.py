#!/usr/bin/env python3
"""NTC assertion-skip binary patch for BES2800BP (best1700_ep) firmware.

The EVB has no NTC thermistor, so pmu_open() hits a udiv-by-zero assertion
at boot. This patch locates `pmu_open` SYMBOL-LEVEL via arm-none-eabi-nm
(no hardcoded offsets) and rewrites the `cbnz r5` (0xb955) at
pmu_open+0x1a78 into an unconditional branch skipping the assertion and
the divide path.

Safety: before writing, the instruction at the computed offset is verified
to be 0xb955; anything else aborts the patch (layout drift guard).

Usage (from the openvela workspace root, WSL or Linux):
    python3 contest2026_416_dianzinongmingong/tools/ntc_patch.py

Reads : cmake_out/best1700_ep/aos_evb/out/nuttx_ap.elf
Writes: cmake_out/best1700_ep/aos_evb/out/nuttx_ap.bin  (objcopy regen)
        flash/vela_2800bp/vela_2800bp/nuttx_ap.bin      (flash-ready copy)
"""
import os
import struct
import subprocess
import sys

PMU_OPEN_OFFSET = 0x1A78      # cbnz r5 inside pmu_open
PMU_OPEN_DEST = 0x1A9E        # ldr r4, [pc, #472] (skip assert + udiv)
EXPECT_ORIGINAL = 0xB955      # cbnz r5


def workspace_root():
    # tools/ntc_patch.py -> contest repo -> workspace root
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def find_tool(root, name):
    cand = os.path.join(root, "prebuilts", "gcc", "linux-x86_64",
                        "arm-none-eabi", "bin", name)
    return cand if os.path.isfile(cand) else name  # fall back to $PATH


def main():
    root = workspace_root()
    out_dir = os.path.join(root, "cmake_out", "best1700_ep", "aos_evb", "out")
    elf = os.path.join(out_dir, "nuttx_ap.elf")
    bin_path = os.path.join(out_dir, "nuttx_ap.bin")
    flash_bin = os.path.join(root, "flash", "vela_2800bp", "vela_2800bp",
                             "nuttx_ap.bin")
    if not os.path.isfile(elf):
        print(f"ERROR: ELF not found: {elf} (build first: bash "
              f"vendor/bes/readme/1700_ap.sh)")
        return 1

    with open(elf, "rb") as f:
        data = bytearray(f.read())

    nm = find_tool(root, "arm-none-eabi-nm")
    try:
        res = subprocess.run([nm, elf], capture_output=True, text=True,
                             check=True)
    except (OSError, subprocess.CalledProcessError) as e:
        print(f"ERROR: cannot run {nm}: {e}")
        return 1

    pmu_addr = None
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == "pmu_open" and parts[1] in ("T", "t"):
            pmu_addr = int(parts[0], 16)
            break
    if pmu_addr is None:
        print("ERROR: symbol pmu_open not found in ELF")
        return 1

    target_vaddr = pmu_addr + PMU_OPEN_OFFSET
    dest_vaddr = pmu_addr + PMU_OPEN_DEST
    print(f"pmu_open @ 0x{pmu_addr:08x}, patch target 0x{target_vaddr:08x}")

    e_phoff = struct.unpack_from("<I", data, 28)[0]
    e_phentsize = struct.unpack_from("<H", data, 42)[0]
    e_phnum = struct.unpack_from("<H", data, 44)[0]

    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if struct.unpack_from("<I", data, off)[0] != 1:  # PT_LOAD
            continue
        p_offset = struct.unpack_from("<I", data, off + 4)[0]
        p_vaddr = struct.unpack_from("<I", data, off + 8)[0]
        p_filesz = struct.unpack_from("<I", data, off + 16)[0]
        if not (p_vaddr <= target_vaddr < p_vaddr + p_filesz):
            continue

        file_offset = p_offset + (target_vaddr - p_vaddr)
        instr = struct.unpack_from("<H", data, file_offset)[0]
        if instr != EXPECT_ORIGINAL:
            print(f"ABORT: instruction at 0x{target_vaddr:08x} is "
                  f"0x{instr:04x}, expected 0x{EXPECT_ORIGINAL:04x} "
                  f"(layout drifted, refusing to patch)")
            return 2

        rel = (dest_vaddr - target_vaddr - 4) >> 1
        new_instr = 0xE000 | (rel & 0x7FF)
        print(f"Patching: 0x{instr:04x} -> 0x{new_instr:04x} "
              f"(jump 0x{target_vaddr:08x} -> 0x{dest_vaddr:08x})")
        struct.pack_into("<H", data, file_offset, new_instr)

        with open(elf, "wb") as f:
            f.write(data)

        objcopy = find_tool(root, "arm-none-eabi-objcopy")
        subprocess.run([objcopy, "-O", "binary", elf, bin_path], check=True)
        print(f"Regenerated: {bin_path} ({os.path.getsize(bin_path)} bytes)")

        os.makedirs(os.path.dirname(flash_bin), exist_ok=True)
        with open(bin_path, "rb") as fi, open(flash_bin, "wb") as fo:
            fo.write(fi.read())
        print(f"Flash-ready: {flash_bin} ({os.path.getsize(flash_bin)} bytes)")
        return 0

    print("ERROR: no PT_LOAD segment covers the target address")
    return 1


if __name__ == "__main__":
    sys.exit(main())
