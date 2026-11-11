#!/usr/bin/env python3
"""Let the chiron loader take its iOS root volume from the "darwin" partition.

The installed phone has no host, so "oem ramdisk" (a 2.4 GB USB upload on every boot) cannot
be how the volume arrives. This reads the recorded volume from the phone's own flash instead,
into the same resident area the USB path fills, so the kernel side is unchanged: run_kernel
already treats resident_rd_size at win_base + RESIDENT_RD_OFFSET as RAMDisk (md0).

Nothing here may write a partition. The volume is put on the phone from the computer
(tools/darwin-write.py or EDL + fh_loader), never by the loader.
"""

import hashlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
INC = HERE / "chiron_root_ufs.inc"


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"source context changed: {old[:100]}")
    return text.replace(old, new, 1)


def apply(work):
    source = Path(work)
    loader = source / "xnuloader/loader.c"
    target = source / "xnuloader/chiron_root_ufs.inc"
    inc = INC.read_text()
    target.write_text(inc)
    text = loader.read_text()
    # The include goes where everything it uses is already declared: just before efi_main.
    text = replace_once(
        text,
        "efi_status EFIAPI\nefi_main(efi_handle image",
        inc + "\nefi_status EFIAPI\nefi_main(efi_handle image",
    )
    # One attempt, after the exception hook exists so that a fault is logged, before the kernel.
    text = replace_once(
        text,
        "\tinstall_exception_hook();\n\twdt_arm();",
        "\tinstall_exception_hook();\n"
        "\tif (chiron_mode) {\n"
        "\t\t/* the root is on the phone already: read it instead of waiting for the host */\n"
        "\t\tchiron_root_from_ufs();\n"
        "\t}\n"
        "\twdt_arm();",
    )
    # With the volume resident there is nothing to wait for: the image runs one-shot, so a
    # phone booted from its own boot/recovery partition never needs a host at all. When the
    # read failed, resident_rd_size is 0 and the USB channel starts exactly as before.
    text = replace_once(
        text,
        "\tif (have_payload && pi.usb_mode && usb_start()) {",
        "\tif (have_payload && pi.usb_mode && !resident_rd_size && usb_start()) {",
    )
    loader.write_text(text)
    return {
        "loader_source_sha256": hashlib.sha256(loader.read_bytes()).hexdigest(),
        "rootufs_include_sha256": hashlib.sha256(inc.encode()).hexdigest(),
        "root_partition": "darwin",
        "root_bytes": 2492219392,
        "writes_partitions": False,
    }
