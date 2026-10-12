"""Bounded, line-oriented state for long-running native guest transactions."""
from __future__ import annotations

import json
import os
import re
import time
from pathlib import Path

STAGES = (
    "image-preparation", "guest-boot", "network-readiness", "source-fetch",
    "source-verification", "extraction", "configure", "compile", "tests",
    "package-creation", "install", "lifecycle", "recovery",
)
_SAFE_NAME = re.compile(r"^[a-z0-9][a-z0-9+-]{0,63}$")


def safe_state_dir(root: Path, package: str) -> Path:
    """Return a package state directory without permitting path traversal."""
    if not _SAFE_NAME.fullmatch(package):
        raise ValueError("unsafe package name")
    root = Path(root).resolve()
    result = (root / package).resolve()
    if result.parent != root:
        raise ValueError("state path escapes root")
    return result


def write_checkpoint(state: Path, stage: str, status: str, detail: str = "") -> None:
    if stage not in STAGES:
        raise ValueError(f"unknown stage: {stage}")
    if status not in {"running", "passed", "failed", "timeout", "skipped"}:
        raise ValueError(f"unknown checkpoint status: {status}")
    state.mkdir(parents=True, exist_ok=True)
    record = {
        "timestamp": int(time.time()), "stage": stage, "status": status,
        "detail": str(detail)[:512],
    }
    line = json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n"
    tmp = state / "current.json.tmp"
    tmp.write_text(line, encoding="utf-8")
    os.replace(tmp, state / "current.json")
    with (state / "checkpoints.jsonl").open("a", encoding="utf-8") as stream:
        stream.write(line)


def read_checkpoint(state: Path) -> dict | None:
    path = Path(state) / "current.json"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get("stage") in STAGES else None


def resume_stage(state: Path) -> str | None:
    """Return the stage to rerun, or None when the transaction is complete."""
    checkpoint = read_checkpoint(state)
    if not checkpoint:
        return STAGES[0]
    if checkpoint.get("status") != "passed":
        return str(checkpoint["stage"])
    if checkpoint["stage"] == "lifecycle":
        return None
    index = STAGES.index(checkpoint["stage"]) + 1
    return STAGES[index] if index < len(STAGES) else None


def tail(path: Path, limit: int = 2000) -> str:
    """Read a bounded UTF-8 tail without loading an unbounded guest log."""
    if limit < 1:
        raise ValueError("tail limit must be positive")
    try:
        with Path(path).open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            stream.seek(max(0, stream.tell() - limit))
            return stream.read(limit).decode("utf-8", "replace")
    except FileNotFoundError:
        return ""


def format_status(state: Path, log: Path) -> str:
    checkpoint = read_checkpoint(state)
    if not checkpoint:
        return "stage=unknown status=not-started output=none"
    output = tail(log, 600).replace("\n", "\\n")
    return "stage={stage} status={status} detail={detail} output={output}".format(
        output=output, **checkpoint
    )
