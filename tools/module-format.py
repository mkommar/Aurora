#!/usr/bin/env python3
"""Create and validate Aurora's bounded loadable user-service module format.

This is a transport/validation format, not a ring-0 loader.  Modules contain
already-linked flat ring-3 service images; relocation and direct device access
are deliberately outside ABI version 1.
"""
import argparse
import hashlib
import struct
from pathlib import Path

MAGIC = b"AURMOD1\0"
ABI_VERSION = 1
KIND_USER_SERVICE = 1
FLAG_MAY_UNLOAD = 1
FLAG_IPC = 2
HEADER = struct.Struct("<8sHHHHIIIIIII32s")
MAX_PAYLOAD = 512 * 1024
PAGE = 4096


def pack_module(payload, *, entry, text_end, ro_end, flags=FLAG_IPC):
    validate_fields(len(payload), entry, text_end, ro_end, flags)
    digest = hashlib.sha256(payload).digest()
    return HEADER.pack(MAGIC, ABI_VERSION, KIND_USER_SERVICE, ABI_VERSION,
                       flags, len(payload), entry, text_end, ro_end, 0, 0, 0,
                       digest) + payload


def validate_fields(payload_size, entry, text_end, ro_end, flags):
    if not 0 < payload_size <= MAX_PAYLOAD:
        raise ValueError("module payload exceeds bounded module limit")
    if flags & ~(FLAG_MAY_UNLOAD | FLAG_IPC):
        raise ValueError("unknown module flags")
    if not 0 <= entry < text_end <= ro_end <= payload_size:
        raise ValueError("module entry and RX/RO boundaries are invalid")


def read_module(path):
    data = Path(path).read_bytes()
    if len(data) < HEADER.size:
        raise ValueError("module is shorter than its header")
    fields = HEADER.unpack_from(data)
    magic, version, kind, abi, flags, size, entry, text_end, ro_end = fields[:9]
    digest = fields[-1]
    if magic != MAGIC or version != ABI_VERSION or kind != KIND_USER_SERVICE:
        raise ValueError("unsupported Aurora module ABI or kind")
    if len(data) != HEADER.size + size:
        raise ValueError("module payload length does not match header")
    validate_fields(size, entry, text_end, ro_end, flags)
    payload = data[HEADER.size:]
    if hashlib.sha256(payload).digest() != digest:
        raise ValueError("module payload hash mismatch")
    return {
        "version": version,
        "kind": kind,
        "flags": flags,
        "payload_size": size,
        "entry": entry,
        "text_end": text_end,
        "ro_end": ro_end,
        "payload": payload,
    }


def unload_allowed(module, active_calls):
    """Return whether a future loader may unload a module safely.

    ABI v1 only permits unload for modules explicitly marked unloadable and
    with no active calls.  The current kernel has no activation path, so this
    policy is a guard for the eventual service manager rather than a claim of
    runtime unloading.
    """
    return bool(module["flags"] & FLAG_MAY_UNLOAD) and active_calls == 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create")
    create.add_argument("--input", required=True)
    create.add_argument("--output", required=True)
    create.add_argument("--entry", type=lambda value: int(value, 0), default=0)
    create.add_argument("--text-end", type=lambda value: int(value, 0), required=True)
    create.add_argument("--ro-end", type=lambda value: int(value, 0), required=True)
    create.add_argument("--may-unload", action="store_true")
    check = sub.add_parser("validate")
    check.add_argument("module")
    args = parser.parse_args()
    if args.command == "create":
        payload = Path(args.input).read_bytes()
        flags = FLAG_IPC | (FLAG_MAY_UNLOAD if args.may_unload else 0)
        Path(args.output).write_bytes(pack_module(payload, entry=args.entry,
                                                   text_end=args.text_end,
                                                   ro_end=args.ro_end,
                                                   flags=flags))
    else:
        module = read_module(args.module)
        print(f"valid Aurora module: {module['payload_size']} bytes, entry {module['entry']}")


if __name__ == "__main__":
    main()
