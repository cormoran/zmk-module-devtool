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

### Out of scope

Covered by sibling modules:

- [zmk-feature-device-info](https://github.com/cormoran/zmk-feature-device-info):
  read-only diagnostics — build/git info, hwinfo, reset cause, flash/SRAM
  size, compile-time ZMK config flags, Zephyr device list, uptime.
- [zmk-feature-zephyr-setting-expose](https://github.com/cormoran/zmk-feature-zephyr-setting-expose):
  Zephyr settings (NVS) list/read/write/delete, storage stats, GC, clear-all.
- Endpoint/BLE runtime actions, crash/fault capture, and timing metrics are
  also already handled by other modules in the same family.

Anything that is "read static/diagnostic info" belongs in device-info, and
anything that is "manipulate persisted settings" belongs in setting-expose.
Devtool should own *runtime interaction*: injecting inputs and observing live
events.

Considered but rejected as overkill for this module:

- Pointer input injection/observation (virtual input device).
- I2C/SPI register access and bus scan.

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
  observe keystrokes must carry a prominent "development firmware only"
  warning in Kconfig help and README.

## Tier 1 — close the verify loop

### 1. Log capture over RPC

**Motivation.** Getting Zephyr logs today needs a J-Link/RTT rig or a second
USB CDC console; neither is available in constrained/sandboxed agent
environments. Logs are the single most useful debugging signal, and pulling
them over the *same* Studio RPC transport removes a whole class of setup pain.

**Sketch.**

- A custom Zephyr log backend writing log records into a RAM ring buffer
  (size via Kconfig, e.g. 2–8 KB).
- `get_logs { cursor }` → `{ records[], next_cursor, dropped_count }` —
  cursor based so the agent can poll incrementally; report how many records
  were dropped on overflow. Records are structured
  `(source_id, level, timestamp, text)`, not preformatted strings, so hosts
  can filter mechanically.
- `clear_logs {}`, `set_capture_filter { source, level }`, and optionally
  `set_log_level { module, level }` using Zephyr runtime log filtering to
  turn on debug logs for one driver without reflashing.

**The self-feedback problem.** Serving `get_logs` itself makes the RPC
transport and Studio subsystem emit logs, which land in the buffer, so every
poll generates fresh content — an infinite feedback loop that also drowns the
useful logs. The design must break this structurally, not cosmetically:

1. **Per-backend runtime filtering with an allow-list.** Zephyr's
   `CONFIG_LOG_RUNTIME_FILTERING` + `log_filter_set()` configure levels *per
   backend per source*. The capture backend defaults every source to OFF and
   enables only an allow-list (typically: the module under development).
   Transport/RPC logs then never enter the buffer, so recursion is
   structurally impossible, while RTT/console backends keep seeing
   everything. Allow-list defaults come from Kconfig and are adjustable at
   runtime via `set_capture_filter`.
2. **Split ZMK's log sources in the fork.** The catch: ZMK registers almost
   everything under the single `zmk` log module (`LOG_MODULE_DECLARE(zmk)`),
   so source-level filtering cannot separate Studio-transport logs from
   useful keymap/behavior logs. Since this module already depends on the
   `cormoran/zmk` fork (`main+custom-studio-protocol`), add a small patch
   there registering the Studio RPC/transport files under a dedicated module
   (e.g. `zmk_studio`), which the capture backend deny-lists by default.
   Then capturing the rest of `zmk` becomes safe.
3. **No logging in the capture path.** Devtool's own backend, ring buffer,
   and RPC handlers use no `LOG_*` calls at all (enforced by convention and
   its own log level pinned OFF) — the one part guaranteed at compile time.
4. **Safety net: snapshot + bounded drain.** `get_logs` snapshots the write
   cursor on entry and returns only records up to it. Even if some
   self-generated log slips through the filters, each poll's growth is
   bounded and visible (via `dropped_count` and per-record `source_id`), and
   the host can discard it — degraded noise, never an infinite loop.

(A thread-based suppression — drop records emitted by the RPC thread while a
busy flag is set — only works reliably in `LOG_MODE_IMMEDIATE`, where the
backend runs in the emitting thread's context. ZMK builds normally use
deferred mode, so this is not the primary mechanism.)

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
size down. Unlike log capture, the event tap has no self-feedback problem:
serving an RPC produces no ZMK input/layer/HID events.

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

## Tier 2 — split keyboards

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

Ideas 1–3 share infrastructure (RAM ring buffer + cursor-based chunked
reads), so implementing log capture first also builds the plumbing that the
event tap reuses.
