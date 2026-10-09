#!/usr/bin/env python3
"""Host regressions for the guarded AURMOD1 activation boundary."""
import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(value)
    return value


fmt = load("module-format")
runtime = load("module-runtime")


def rejects(action):
    try:
        action()
    except runtime.ActivationError:
        return
    raise AssertionError("activation request was accepted")


def main():
    payload = b"service-image"
    manager = runtime.ModuleManager(allowed_capabilities={"net"})
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "probe.mod"
        path.write_bytes(fmt.pack_module(payload, entry=0, text_end=4, ro_end=len(payload)))

        record = manager.activate("probe", path, requested_capabilities={"net"},
                                  granted_capabilities={"net"})
        assert record.state is runtime.ModuleState.ACTIVE
        assert manager.operation_allowed("probe", "net")
        rejects(lambda: manager.operation_allowed("probe", "storage") or
                manager.begin_call("probe", "storage"))
        manager.begin_call("probe", "net")
        rejects(lambda: manager.unload("probe"))
        assert manager.active("probe").state is runtime.ModuleState.ACTIVE
        manager.end_call("probe")
        rejects(lambda: manager.activate("probe", path, granted_capabilities={"net"}))
        rejects(lambda: manager.activate("second", path, requested_capabilities={"net"},
                                          granted_capabilities={"net"}))
        assert manager.active("probe") is record

        rejects(lambda: manager.activate("denied", path, requested_capabilities={"net"},
                                          granted_capabilities=set()))
        assert manager.failed("denied").state is runtime.ModuleState.FAILED
        rejects(lambda: manager.activate("unsupported", path, requested_capabilities={"disk"},
                                          granted_capabilities={"disk"}))

        corrupt = bytearray(path.read_bytes())
        corrupt[-1] ^= 1
        rejects(lambda: manager.activate("corrupt", corrupt, granted_capabilities=set()))
        oversized = fmt.HEADER.pack(fmt.MAGIC, fmt.ABI_VERSION, fmt.KIND_USER_SERVICE,
                                     fmt.ABI_VERSION, fmt.FLAG_IPC, fmt.MAX_PAYLOAD + 1,
                                     0, 1, 1, 0, 0, 0, b"0" * 32)
        rejects(lambda: manager.activate("oversized", oversized, granted_capabilities=set()))

        def fail_registration(name, value):
            raise RuntimeError("registration failed")

        rejects(lambda: manager.activate("rollback", path, register=fail_registration))
        assert manager.active("rollback") is None

        unloadable_payload = b"unloadable-image"
        unloadable = fmt.pack_module(unloadable_payload, entry=0, text_end=4,
                                     ro_end=len(unloadable_payload),
                                     flags=fmt.FLAG_IPC | fmt.FLAG_MAY_UNLOAD)
        manager.activate("unloadable", unloadable, granted_capabilities=set())
        manager.unload("unloadable")
        assert manager.active("unloadable") is None
    print("PASS: guarded module activation, capability enforcement and rollback")


if __name__ == "__main__":
    main()
