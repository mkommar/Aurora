#!/usr/bin/env python3
"""Host regression tests for the bounded Aurora user-service module format."""
import importlib.util
import tempfile
from pathlib import Path

spec = importlib.util.spec_from_file_location("module_format", "tools/module-format.py")
module_format = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module_format)


def expect_failure(action):
    try:
        action()
    except ValueError:
        return
    raise AssertionError("malformed module was accepted")


def main():
    payload = b"text" + b"\0" * 4092 + b"read-only"
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "probe.mod"
        path.write_bytes(module_format.pack_module(payload, entry=0,
                                                    text_end=4096,
                                                    ro_end=len(payload)))
        parsed = module_format.read_module(path)
        assert parsed["payload"] == payload
        assert not module_format.unload_allowed(parsed, 1)
        assert not module_format.unload_allowed(parsed, 0)

        unloadable = module_format.pack_module(payload, entry=0, text_end=4096,
                                               ro_end=len(payload),
                                               flags=module_format.FLAG_MAY_UNLOAD)
        path.write_bytes(unloadable)
        parsed = module_format.read_module(path)
        assert module_format.unload_allowed(parsed, 0)

        corrupted = bytearray(unloadable)
        corrupted[-1] ^= 1
        path.write_bytes(corrupted)
        expect_failure(lambda: module_format.read_module(path))

        expect_failure(lambda: module_format.pack_module(payload, entry=8,
                                                          text_end=4,
                                                          ro_end=len(payload)))
        expect_failure(lambda: module_format.pack_module(b"", entry=0,
                                                          text_end=0, ro_end=0))
    print("PASS: Aurora module format ABI, bounds, hash and unload policy")


if __name__ == "__main__":
    main()
