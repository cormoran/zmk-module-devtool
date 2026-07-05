# Devtool Feature Ideas

This document proposes new features for `zmk-module-devtool`. The module's
goal is to make ZMK keyboard development and debugging easy to drive from a
coding agent: everything should be scriptable over Studio RPC (serial/BLE)
without physical access to the keyboard.

## Where the gaps are

A typical agent-driven development loop looks like:

```
edit code → build → flash → exercise the keyboard → observe results → debug
```

Today the module covers the *flash* step well (unlock Studio, reboot, enter
bootloader), but the *exercise*, *observe*, and *debug* steps still require a
human pressing keys, a host-side HID sniffer, or a J-Link/RTT rig. The ideas
below focus on closing that loop.

### Out of scope (covered by sibling modules)

- [zmk-feature-device-info](https://github.com/cormoran/zmk-feature-device-info):
  read-only diagnostics — build/git info, hwinfo, reset cause, flash/SRAM
  size, compile-time ZMK config flags, Zephyr device list, uptime.
- [zmk-feature-zephyr-setting-expose](https://github.com/cormoran/zmk-feature-zephyr-setting-expose):
  Zephyr settings (NVS) list/read/write/delete, storage stats, GC, clear-all.

Anything that is "read static/diagnostic info" belongs in device-info, and
anything that is "manipulate persisted settings" belongs in setting-expose.
Devtool should own *runtime interaction*: injecting inputs, observing live
events, and dev-only hardware pokes.

## Design principles

- One Kconfig option per feature (e.g. `ZMK_DEVTOOL_INPUT_INJECTION`,
  `ZMK_DEVTOOL_LOG_CAPTURE`) so users only compile in what they need; flash
  and RAM on nRF52-class MCUs are tight.
- Follow the repo convention: proto definition → firmware handler → web UI,
  and extend the host-side CLI (`zmk-studio-rpc devtool …`) at the same time —
  for coding agents the CLI subcommand *is* the feature.
- Every feature must be testable on `native_sim` (unit test) and included in a
  `tests/zmk-config` build artifact.
- Mind the known constraints: no 64-bit proto fields (nanopb + Studio),
  static response buffers, and `ZMK_STUDIO_RPC_TX_BUF_SIZE` sizing for large
  or streamed responses.
- The subsystem is intentionally unsecured for automation. Features that can
  observe keystrokes or touch hardware buses must carry a prominent
  "development firmware only" warning in Kconfig help and README, and may
  deserve a shared `ZMK_DEVTOOL_ALLOW_DANGEROUS` gate.

## Tier 1 — close the verify loop

### 1. Log capture over RPC

**Motivation.** Getting Zephyr logs today needs a J-Link/RTT rig or a second
USB CDC console; neither is available in constrained/sandboxed agent
environments. Logs are the single most useful debugging signal, and pulling
them over the *same* Studio RPC transport removes a whole class of setup pain.

**Sketch.**

- A custom Zephyr log backend writing formatted lines into a RAM ring buffer
  (size via Kconfig, e.g. 2–8 KB).
- `get_logs { cursor }` → `{ lines[], next_cursor, dropped_count }` — cursor
  based so the agent can poll incrementally; report how many lines were
  dropped on overflow.
- `clear_logs {}`, and optionally `set_log_level { module, level }` using
  Zephyr runtime log filtering to turn on debug logs for one driver without
  reflashing.

**Notes.** Response chunking must respect the TX buffer size. Log capture
should be disabled while Studio RPC's own logging would recurse (filter the
RPC module out of the backend).

### 2. Virtual key input injection

**Motivation.** Lets an agent test keymaps, combos, macros, tap-dance, and
custom behaviors end-to-end on real hardware without touching a key. Combined
with idea 3 this turns any physical keyboard into a self-testing rig.

**Sketch.**

- `inject_key { position, pressed }` — raise
  `zmk_position_state_changed` as if the matrix reported it.
- `tap_key { position, hold_ms }` convenience (press → delay → release) so
  timing-sensitive behaviors (hold-tap!) can be exercised deterministically.
- Optionally `invoke_behavior { binding }` to trigger a behavior directly
  (e.g. `&kp A`, `&mo 1`) bypassing the keymap, useful for behavior-module
  development.

**Notes.** Position-level injection reuses the exact event path of real key
presses, so everything downstream (combos, hold-tap decision, HID) is
covered. Needs a work-queue hop so injection does not run in the RPC thread.

### 3. Event tap (streamed or polled observation)

**Motivation.** Injection without observation is half a test. An agent needs
to confirm "pressing position 12 activated layer 2 and sent usage 0x04".
Host-side HID capture (usbmon) or BLE sniffing is often unavailable, so the
firmware itself should report what it did.

**Sketch.**

- Subscribe/unsubscribe RPC with an event-type bitmask: position changed,
  keycode state changed, layer state changed, HID keyboard/consumer/mouse
  report sent, endpoint changed, activity state changed, split peripheral
  (dis)connected.
- Events are timestamped (`uint32` ms) and delivered either as Studio RPC
  notifications or written to a ring buffer drained by
  `get_events { cursor }` — the polled path is the more robust default and
  matches the log-capture design; notifications can come later.

**Notes.** This is effectively a keylogger — Kconfig warning mandatory, and
it is the flagship reason for the "development firmware only" framing.
Sharing the ring-buffer + cursor plumbing between ideas 1 and 3 keeps code
size down.

### 4. Layer state inspection & control

**Motivation.** Official Studio RPC edits the keymap but does not expose
*runtime* layer state. Agents debugging layer logic currently have no way to
ask "which layers are active right now?".

**Sketch.**

- `get_layer_state {}` → `{ active_layers[], highest_active }`.
- `set_layer_state { layer, active }` for momentary activate/deactivate and
  `toggle_layer { layer }`.

**Notes.** Small, low-risk, and pairs naturally with ideas 2/3 for keymap
testing. Layer *names* are already available via official keymap RPC.

## Tier 2 — split keyboards and pointing devices

### 5. Split peripheral relay

**Motivation.** On split keyboards only the central runs Studio RPC; the
peripheral half is unreachable. "Put the *peripheral* into bootloader" today
means physically double-tapping its reset button — a frequent, annoying,
manual step in the flash loop.

**Sketch.**

- `peripheral_command { index, command }` where command ∈ {reboot,
  bootloader} relayed over the split transport (custom split data channel).
- `get_peripheral_status {}` → per-peripheral connectivity, and battery/RSSI
  if cheaply available.

**Notes.** Requires a devtool component on the peripheral build too. The
split transport channel work is the main cost; once it exists, later features
(e.g. peripheral log forwarding) can reuse it. Highest wall-clock savings for
split-keyboard developers of anything in this list.

### 6. Pointer input injection & observation

**Motivation.** For pointing-device work (e.g. the PMW3610 trackball driver)
the interesting code path is Zephyr `input_report` → ZMK input processors →
HID mouse report. A virtual input device that injects REL/ABS events lets an
agent test input-processor chains (scaling, rotation, scroll layers) without
spinning a physical trackball; the event tap (idea 3) then verifies the
resulting mouse reports.

**Sketch.**

- A devtool virtual `input` device declared via devicetree overlay.
- `inject_input { type, code, value, sync }` mirroring Zephyr's input event
  triple.
- Input events (from *any* device, real sensors included) appear in the
  event tap stream, giving driver developers a live view of sensor output.

### 7. Endpoint & BLE runtime actions

**Motivation.** Testing multi-host setups needs "switch to BLE profile 2",
"unpair profile 0", "prefer USB" as scriptable actions, plus live connection
state to verify the result. setting-expose can poke the persisted bytes but
has no semantics; device-info only reports compile-time flags.

**Sketch.**

- `select_ble_profile { index }`, `unpair_ble_profile { index }`,
  `set_preferred_endpoint { usb | ble }`.
- `get_connection_state {}` → active endpoint, per-profile
  bonded/connected/address, USB state.

**Notes.** Boundary case: the read-only half could arguably live in
device-info; keeping action + matching state query together in devtool makes
the CLI story simpler ("switch, then confirm").

## Tier 3 — hardware bring-up and crash forensics

### 8. I2C/SPI register access & bus scan

**Motivation.** Sensor driver bring-up (PMW3610 again) is dominated by "what
does register 0x02 actually read?" questions that today require RTT printf
cycles. An I2C bus scan plus raw register read/write over RPC turns those
into one-second CLI calls, including live register dumps while the sensor
runs.

**Sketch.**

- `i2c_scan { bus }` → present addresses.
- `reg_read { device, reg, count }` / `reg_write { device, reg, data }`
  addressing devices by devicetree node label, routed through the existing
  driver's bus handle where possible.

**Notes.** Clearly dangerous (can misconfigure hardware) — gate behind
`ZMK_DEVTOOL_ALLOW_DANGEROUS`. SPI support can piggyback on per-driver hooks
where a generic implementation is awkward (sensors with custom protocols like
PMW3610's 3-wire SPI).

### 9. GPIO peek/poke

**Motivation.** First-boot bring-up of a new PCB: verify matrix wiring, check
a rotary encoder's pins, confirm an LED gate — without writing throwaway
firmware. `gpio_read { port, pin }` / `gpio_write { port, pin, value }` /
`gpio_configure { … }`.

**Notes.** Same dangerous-gate as idea 8. Refuse pins already claimed by
kscan unless explicitly forced, to avoid fighting live drivers.

### 10. Crash forensics: last-fault info & coredump retrieval

**Motivation.** device-info exposes the reset *cause*; when firmware hard
faults, the agent still cannot see *where*. Zephyr can persist fault details
(faulting PC/LR, thread name) or a full coredump to a flash partition; a
retrieval RPC (chunked) lets the agent pull it after reboot and symbolize it
against the ELF on the host — post-mortem debugging with no debug probe.

**Sketch.**

- `get_last_fault {}` → compact fault record (cause, PC, LR, thread,
  timestamp), cleared with `clear_last_fault {}`.
- Optional full coredump: enable Zephyr's coredump flash backend, expose
  `read_coredump { offset, count }`.

**Notes.** Compact fault record first — it is a few dozen bytes and covers
most needs; full coredump is a follow-up. Needs a small retained-RAM or flash
region; interacts with board partition layout, so keep it opt-in.

### 11. Timing & health metrics

**Motivation.** "Is my input processor making the scan loop slow?" and "is
the work queue backing up?" are common performance questions with no current
answer. A small metrics RPC (event-queue high-water mark, work-queue max
latency, matrix scan rate, per-thread stack high-water mark) plus an `echo`
RPC with device timestamp for transport-latency measurement gives agents
numbers instead of guesses.

**Notes.** Borderline with device-info's diagnostics; proposed here because
the interesting values are dev-only instrumentation (needs hooks compiled
into hot paths), not always-on info. Could migrate to device-info if it grows
an "instrumentation" story instead.

## Cross-cutting work

- **CLI first-class support** — every new RPC gets a
  `zmk-studio-rpc devtool <op>` subcommand with `--json` output; that is the
  interface coding agents actually use. Web UI panels follow for humans.
- **Web UI: event/log console** — a live console panel (ideas 1/3) makes the
  web UI genuinely useful for manual debugging, not just a button board.
- **README security section** — as observation features land, document the
  threat model explicitly: unsecured subsystem + event tap = anyone with the
  USB/BLE link can read keystrokes. Recommend dev-only firmware and Kconfig
  defaults that keep observation features off.

## Suggested order

| Order | Feature                          | Why first                                             |
| ----- | -------------------------------- | ----------------------------------------------------- |
| 1     | Log capture (idea 1)             | Biggest debugging win; useful for developing the rest |
| 2     | Key injection + event tap (2, 3) | Together they enable closed-loop E2E testing          |
| 3     | Layer state (4)                  | Small; rounds out keymap testing                      |
| 4     | Split relay (5)                  | Big quality-of-life for split boards                  |
| 5     | Pointer injection (6)            | Unblocks input-processor/driver testing               |
| 6     | Endpoint/BLE actions (7)         | Multi-host test automation                            |
| 7     | Bus/GPIO access (8, 9)           | Bring-up power tools, dangerous-gated                 |
| 8     | Fault capture (10), metrics (11) | Forensics and performance polish                      |

Ideas 1–3 share infrastructure (RAM ring buffer + cursor-based chunked
reads), so implementing log capture first also builds the plumbing that the
event tap reuses.
