# Aurora OS: testing an original microkernel in QEMU

This video captures a live QEMU session running Aurora OS 0.2's self-test build.
The guest display is recorded directly through QMP. The surrounding titles and
test information are editorial overlays, labeled **HOST TEST RUNNER**.

Aurora's small privileged kernel handles memory protection, preemptive
scheduling, traps, capability checks, and inter-process communication (IPC).
The desktop and built-in apps, input/platform driver, and framebuffer service
run as three separate user-mode processes with private address spaces.

## What the recording shows

- Booting the original 64-bit OS.
- Running 28 microkernel checks against actual guest state.
- Containing six deliberate faults: kernel-memory write, foreign-memory write,
  direct port I/O, execution from stack data, executable-code write, and an
  invalid opcode.
- Preempting a test process that spins without yielding. The displayed timer,
  IPC, and preemption counters are sampled from guest memory during recording.
- Running 12 GUI checks, followed by paced demonstrations of note typing,
  palette selection, window dragging, and terminal interaction.

The terminal and notes share the desktop process; they are not separate
isolated applications. Fault containment does not imply automatic service
recovery: restarting failed services is not implemented. Aurora is a static,
single-core hobby OS with RAM-only notes, no filesystem, and no networking.

## Package

- `aurora-tests-vertical.mp4`: captioned 1080 × 1920 video for vertical feeds.
- `aurora-tests-landscape.mp4`: captioned 1920 × 1080 video with the entire
  guest display kept at native size.
- `aurora-qemu-original.mp4`: uncaptioned 1024 × 768 source capture.
- `aurora-poster.jpg`: vertical cover image.
- `aurora-landscape-poster.jpg`: landscape cover image.
- `caption.md`: ready-to-post caption and short alternative.
- `capture-timeline.json`: capture timestamps and sampled guest counters.
- `test-microkernel-output.txt`, `test-smoke-output.txt`: captured test output.
- `microkernel-results.json`, `test-results.json`, `serial.log`: test evidence.

The videos are approximately 44 seconds long and use H.264, 15 fps, and no audio. Captions carry the explanation;
no music or synthetic narration is included. The QEMU source capture runs at
real-time speed. The test scripts automate the mouse and keyboard interactions.

## Reproduce

Run `record-social.py` from the workspace with Python, Pillow, and the local
`imageio-ffmpeg` package in `tools/media-python`. It rebuilds the self-test image,
launches a dedicated headless QEMU instance, records the test session, and
renders both layouts. QMP ports 4444 and 4445 must be free.

To re-render an existing capture without rerunning QEMU:

```powershell
python record-social.py --render-only
```
