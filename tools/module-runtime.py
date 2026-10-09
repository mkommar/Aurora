#!/usr/bin/env python3
"""Bounded activation policy for already-linked AURMOD1 user services.

The manager stages an image and its service registration. It does not execute
the payload or grant hardware mappings. Capabilities are supplied by the
caller, never read from the module image.
"""
from dataclasses import dataclass
from enum import Enum
import hashlib
import importlib.util
from pathlib import Path
import tempfile


_FORMAT_PATH = Path(__file__).with_name("module-format.py")
_SPEC = importlib.util.spec_from_file_location("aurora_module_format", _FORMAT_PATH)
module_format = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(module_format)


class ModuleState(Enum):
    NEW = "new"
    VALIDATED = "validated"
    AUTHORIZED = "authorized"
    ACTIVE = "active"
    FAILED = "failed"
    UNLOAD_REJECTED = "unload-rejected"
    UNLOADED = "unloaded"


class ActivationError(ValueError):
    """A request failed before it could become active."""


@dataclass
class ModuleRecord:
    name: str
    digest: bytes
    payload: bytes
    capabilities: frozenset
    flags: int = 0
    state: ModuleState = ModuleState.NEW
    active_calls: int = 0
    registration: object = None
    failure: str = ""


class ModuleManager:
    """Validate, authorize, stage, and retire bounded user-service modules."""

    def __init__(self, *, allowed_capabilities=()):
        self.allowed_capabilities = frozenset(allowed_capabilities)
        self._active = {}
        self._failed = {}

    def activate(self, name, source, *, requested_capabilities=(), granted_capabilities=(), register=None):
        if not isinstance(name, str) or not name or len(name) > 64 or "/" in name or "\\" in name:
            raise ActivationError("invalid module name")
        if name in self._active:
            raise ActivationError("duplicate module activation")
        try:
            requested = frozenset(requested_capabilities)
            granted = frozenset(granted_capabilities)
        except (TypeError, ValueError) as error:
            raise ActivationError("invalid module capability request") from error
        record = ModuleRecord(name, b"", b"", requested)
        temporary = None
        try:
            if not requested <= granted or not granted <= self.allowed_capabilities:
                raise ActivationError("module capability grant denied")
            if isinstance(source, (str, Path)):
                parsed = module_format.read_module(source)
            else:
                temporary = tempfile.NamedTemporaryFile(delete=False)
                temporary.write(bytes(source))
                temporary.close()
                parsed = module_format.read_module(temporary.name)
            record.state = ModuleState.VALIDATED
            record.digest = hashlib.sha256(parsed["payload"]).digest()
            if record.digest in (item.digest for item in self._active.values()):
                raise ActivationError("duplicate module image")
            record.payload = bytes(parsed["payload"])
            record.flags = parsed["flags"]
            record.state = ModuleState.AUTHORIZED
            # Registration is the only externally visible mutation. Commit the
            # record only after it succeeds so failures cannot leak state.
            record.registration = register(name, record) if register else {"name": name}
            self._active[name] = record
            record.state = ModuleState.ACTIVE
            return record
        except Exception as error:
            record.state = ModuleState.FAILED
            record.failure = str(error)
            record.payload = b""
            record.registration = None
            self._failed[name] = record
            if isinstance(error, ActivationError):
                raise
            raise ActivationError(str(error)) from error
        finally:
            if temporary is not None:
                Path(temporary.name).unlink(missing_ok=True)

    def operation_allowed(self, name, capability):
        record = self._active.get(name)
        return bool(record and record.state is ModuleState.ACTIVE and capability in record.capabilities)

    def begin_call(self, name, capability):
        if not self.operation_allowed(name, capability):
            raise ActivationError("privileged operation denied")
        self._active[name].active_calls += 1

    def end_call(self, name):
        record = self._active.get(name)
        if not record or record.active_calls <= 0:
            raise ActivationError("invalid active call")
        record.active_calls -= 1

    def unload(self, name):
        record = self._active.get(name)
        if not record:
            raise ActivationError("module is not active")
        if not (record.state is ModuleState.ACTIVE and record.active_calls == 0 and
                module_format.unload_allowed({"flags": record.flags}, 0)):
            record.failure = "module unload is not allowed"
            raise ActivationError("module unload is not allowed")
        registration = record.registration
        self._active.pop(name)
        record.registration = None
        record.payload = b""
        record.state = ModuleState.UNLOADED
        return registration

    def active(self, name):
        return self._active.get(name)

    def failed(self, name):
        return self._failed.get(name)
