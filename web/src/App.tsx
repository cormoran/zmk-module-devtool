import { useContext, useEffect, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import type { UseZMKAppReturn } from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  Notification,
  StudioLockState,
  DevtoolEventType,
  DevtoolEvent,
  LogLevel,
  LogRecord,
} from "./proto/cormoran/devtool/devtool";

export const SUBSYSTEM_IDENTIFIER = "cormoran__devtool";

type Operation = "unlock" | "lock" | "getLockState" | "bootloader" | "reboot";

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>ZMK Devtool</h1>
        <p>Custom Studio RPC controls for development firmware.</p>
      </header>

      <ZMKConnection
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>{error}</p>
              </div>
            )}
            {!isLoading && (
              <button
                className="btn btn-primary"
                onClick={() => connect(serial_connect)}
              >
                Connect Serial
              </button>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <RPCTestSection />
            <LayerStateSection />
            <KeyInjectionSection />
            <EventTapSection />
            <LogCaptureSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>ZMK Devtool</strong> custom subsystem: {SUBSYSTEM_IDENTIFIER}
        </p>
      </footer>
    </div>
  );
}

/** Looks up the devtool custom subsystem and calls it with an encoded Request,
 * returning the decoded Response. Shared by every section below. */
async function callDevtoolRpc(
  zmkApp: UseZMKAppReturn,
  subsystemIndex: number,
  request: Request
): Promise<Response> {
  const service = new ZMKCustomSubsystem(
    zmkApp.state.connection!,
    subsystemIndex
  );
  const payload = Request.encode(request).finish();
  const responsePayload = await service.callRPC(payload);
  if (!responsePayload) {
    throw new Error("No response payload");
  }
  return Response.decode(responsePayload);
}

/** Shared "find the subsystem or render a warning" pattern used by every
 * section below (they only run once the subsystem is confirmed present). */
function useDevtoolSubsystem() {
  const zmkApp = useContext(ZMKAppContext);
  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER);
  return { zmkApp, subsystem };
}

function lockStateLabel(state: StudioLockState | undefined): string {
  switch (state) {
    case StudioLockState.STUDIO_LOCK_STATE_LOCKED:
      return "locked";
    case StudioLockState.STUDIO_LOCK_STATE_UNLOCKED:
      return "unlocked";
    default:
      return "unknown";
  }
}

export function RPCTestSection() {
  const zmkApp = useContext(ZMKAppContext);
  const [status, setStatus] = useState<string | null>(null);
  const [activeOperation, setActiveOperation] = useState<Operation | null>(
    null
  );

  if (!zmkApp) return null;

  const subsystem = zmkApp.findSubsystem(SUBSYSTEM_IDENTIFIER);

  const callDevtool = async (operation: Operation, request: Request) => {
    if (!zmkApp.state.connection || !subsystem) return;

    setActiveOperation(operation);
    setStatus(null);

    try {
      const resp = await callDevtoolRpc(zmkApp, subsystem.index, request);

      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
      } else if (resp.setStudioLockState) {
        setStatus(`Studio is ${lockStateLabel(resp.setStudioLockState.state)}`);
      } else if (resp.getStudioLockState) {
        setStatus(`Studio is ${lockStateLabel(resp.getStudioLockState.state)}`);
      } else if (resp.enterBootloader) {
        setStatus("Bootloader request accepted");
      } else if (resp.reboot) {
        setStatus("Reboot request accepted");
      } else {
        setStatus("Request completed");
      }
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setActiveOperation(null);
    }
  };

  const setStudioLockState = (state: StudioLockState) =>
    Request.create({
      setStudioLockState: {
        state,
      },
    });

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the ZMK Devtool module.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>Devtool RPC</h2>

      <div className="button-grid">
        <button
          className="btn btn-primary"
          disabled={activeOperation !== null}
          onClick={() =>
            callDevtool(
              "unlock",
              setStudioLockState(StudioLockState.STUDIO_LOCK_STATE_UNLOCKED)
            )
          }
        >
          {activeOperation === "unlock" ? "Unlocking..." : "Unlock Studio"}
        </button>

        <button
          className="btn btn-secondary"
          disabled={activeOperation !== null}
          onClick={() =>
            callDevtool(
              "lock",
              setStudioLockState(StudioLockState.STUDIO_LOCK_STATE_LOCKED)
            )
          }
        >
          {activeOperation === "lock" ? "Locking..." : "Lock Studio"}
        </button>

        <button
          className="btn btn-secondary"
          disabled={activeOperation !== null}
          onClick={() =>
            callDevtool(
              "getLockState",
              Request.create({ getStudioLockState: {} })
            )
          }
        >
          {activeOperation === "getLockState" ? "Reading..." : "Get Lock State"}
        </button>

        <button
          className="btn btn-danger"
          disabled={activeOperation !== null}
          onClick={() =>
            callDevtool("bootloader", Request.create({ enterBootloader: {} }))
          }
        >
          {activeOperation === "bootloader"
            ? "Requesting..."
            : "Enter Bootloader"}
        </button>

        <button
          className="btn btn-danger"
          disabled={activeOperation !== null}
          onClick={() => callDevtool("reboot", Request.create({ reboot: {} }))}
        >
          {activeOperation === "reboot" ? "Rebooting..." : "Reboot"}
        </button>
      </div>

      {status && (
        <div className="response-box">
          <h3>Result</h3>
          <pre>{status}</pre>
        </div>
      )}
    </section>
  );
}

function formatLayerMask(mask: number): string {
  const layers: number[] = [];
  for (let i = 0; i < 32; i++) {
    if (mask & (1 << i)) layers.push(i);
  }
  return layers.length ? layers.join(", ") : "(none)";
}

export function LayerStateSection() {
  const { zmkApp, subsystem } = useDevtoolSubsystem();
  const [layer, setLayer] = useState("0");
  const [locking, setLocking] = useState(false);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);

  if (!zmkApp || !subsystem) return null;

  const run = async (label: string, request: Request) => {
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(zmkApp, subsystem.index, request);
      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
      } else if (resp.getLayerState) {
        setStatus(
          `${label}: active=[${formatLayerMask(resp.getLayerState.activeLayers)}] highest=${resp.getLayerState.highestActiveLayer}`
        );
      } else if (resp.setLayerState) {
        setStatus(
          `${label}: active=[${formatLayerMask(resp.setLayerState.activeLayers)}]`
        );
      } else if (resp.toggleLayer) {
        setStatus(
          `${label}: active=[${formatLayerMask(resp.toggleLayer.activeLayers)}]`
        );
      } else {
        setStatus(`${label}: completed`);
      }
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const layerNum = Number(layer) || 0;

  return (
    <section className="card">
      <h2>Layer State</h2>
      <div className="button-grid">
        <label>
          Layer
          <input
            type="number"
            min={0}
            value={layer}
            onChange={(e) => setLayer(e.target.value)}
          />
        </label>
        <label>
          <input
            type="checkbox"
            checked={locking}
            onChange={(e) => setLocking(e.target.checked)}
          />
          Locking
        </label>
      </div>
      <div className="button-grid">
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={() => run("Get", Request.create({ getLayerState: {} }))}
        >
          Get Layer State
        </button>
        <button
          className="btn btn-primary"
          disabled={busy}
          onClick={() =>
            run(
              "Activate",
              Request.create({
                setLayerState: { layer: layerNum, active: true, locking },
              })
            )
          }
        >
          Activate
        </button>
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={() =>
            run(
              "Deactivate",
              Request.create({
                setLayerState: { layer: layerNum, active: false, locking },
              })
            )
          }
        >
          Deactivate
        </button>
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={() =>
            run(
              "Toggle",
              Request.create({ toggleLayer: { layer: layerNum, locking } })
            )
          }
        >
          Toggle
        </button>
      </div>
      {status && (
        <div className="response-box">
          <pre>{status}</pre>
        </div>
      )}
    </section>
  );
}

export function KeyInjectionSection() {
  const { zmkApp, subsystem } = useDevtoolSubsystem();
  const [position, setPosition] = useState("0");
  const [holdMs, setHoldMs] = useState("100");
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);

  if (!zmkApp || !subsystem) return null;

  const run = async (label: string, request: Request) => {
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(zmkApp, subsystem.index, request);
      setStatus(resp.error ? `Error: ${resp.error.message}` : `${label}: ok`);
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const positionNum = Number(position) || 0;
  const holdMsNum = Number(holdMs) || 0;

  return (
    <section className="card">
      <h2>Key Injection</h2>
      <p>
        Simulates a real matrix key press by raising the same event a physical
        keypress would -- combos, hold-taps and behaviors all see it.
      </p>
      <div className="button-grid">
        <label>
          Position
          <input
            type="number"
            min={0}
            value={position}
            onChange={(e) => setPosition(e.target.value)}
          />
        </label>
        <label>
          Tap hold (ms)
          <input
            type="number"
            min={0}
            value={holdMs}
            onChange={(e) => setHoldMs(e.target.value)}
          />
        </label>
      </div>
      <div className="button-grid">
        <button
          className="btn btn-primary"
          disabled={busy}
          onClick={() =>
            run(
              "Press",
              Request.create({
                injectKey: { position: positionNum, pressed: true },
              })
            )
          }
        >
          Press
        </button>
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={() =>
            run(
              "Release",
              Request.create({
                injectKey: { position: positionNum, pressed: false },
              })
            )
          }
        >
          Release
        </button>
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={() =>
            run(
              "Tap",
              Request.create({
                tapKey: { position: positionNum, holdMs: holdMsNum },
              })
            )
          }
        >
          Tap
        </button>
      </div>
      {status && (
        <div className="response-box">
          <pre>{status}</pre>
        </div>
      )}
    </section>
  );
}

const EVENT_TYPE_OPTIONS: { type: DevtoolEventType; label: string }[] = [
  {
    type: DevtoolEventType.DEVTOOL_EVENT_TYPE_POSITION_STATE_CHANGED,
    label: "Position",
  },
  {
    type: DevtoolEventType.DEVTOOL_EVENT_TYPE_KEYCODE_STATE_CHANGED,
    label: "Keycode",
  },
  {
    type: DevtoolEventType.DEVTOOL_EVENT_TYPE_LAYER_STATE_CHANGED,
    label: "Layer",
  },
  {
    type: DevtoolEventType.DEVTOOL_EVENT_TYPE_MODIFIERS_STATE_CHANGED,
    label: "Modifiers",
  },
];

function formatEvent(ev: DevtoolEvent): string {
  const t = `[${ev.timestampMs}ms]`;
  if (ev.positionStateChanged) {
    return `${t} position ${ev.positionStateChanged.position} ${ev.positionStateChanged.pressed ? "pressed" : "released"}`;
  }
  if (ev.keycodeStateChanged) {
    const k = ev.keycodeStateChanged;
    return `${t} keycode 0x${k.usagePage.toString(16)}/0x${k.keycode.toString(16)} ${k.pressed ? "pressed" : "released"}`;
  }
  if (ev.layerStateChanged) {
    const l = ev.layerStateChanged;
    return `${t} layer ${l.layer} ${l.active ? "active" : "inactive"}${l.locked ? " (locked)" : ""}`;
  }
  if (ev.modifiersStateChanged) {
    const m = ev.modifiersStateChanged;
    return `${t} modifiers 0x${m.modifiers.toString(16)} ${m.pressed ? "pressed" : "released"}`;
  }
  return `${t} (unknown event)`;
}

export function EventTapSection() {
  const { zmkApp, subsystem } = useDevtoolSubsystem();
  const [selected, setSelected] = useState<Set<DevtoolEventType>>(new Set());
  const [subscribed, setSubscribed] = useState(false);
  const [cursor, setCursor] = useState(0);
  const [events, setEvents] = useState<string[]>([]);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);

  if (!zmkApp || !subsystem) return null;

  const toggleType = (type: DevtoolEventType) => {
    const next = new Set(selected);
    if (next.has(type)) next.delete(type);
    else next.add(type);
    setSelected(next);
  };

  const maskFromSelected = () =>
    [...selected].reduce(
      (mask: number, type) => mask | (1 << (type as number)),
      0
    );

  const subscribe = async (mask: number) => {
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({ subscribeEvents: { eventTypeMask: mask } })
      );
      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
        return;
      }
      setSubscribed(mask !== 0);
      if (mask === 0) {
        setCursor(0);
        setEvents([]);
      }
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const poll = async () => {
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({ getEvents: { cursor } })
      );
      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
        return;
      }
      const page = resp.getEvents!;
      setCursor(page.nextCursor);
      setEvents((prev) =>
        [...prev, ...page.events.map(formatEvent)].slice(-200)
      );
      if (page.droppedCount > 0) {
        setStatus(
          `Warning: ${page.droppedCount} event(s) dropped (buffer overflow between polls)`
        );
      }
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const clear = async () => {
    setBusy(true);
    setStatus(null);
    try {
      await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({ clearEvents: {} })
      );
      setCursor(0);
      setEvents([]);
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  return (
    <section className="card">
      <h2>Event Tap</h2>
      <div className="warning-message">
        <p>
          Records the selected event types (including keycodes) into a RAM ring
          buffer for polling below. Development/test firmware only.
        </p>
      </div>
      <div className="button-grid">
        {EVENT_TYPE_OPTIONS.map(({ type, label }) => (
          <label key={type}>
            <input
              type="checkbox"
              checked={selected.has(type)}
              onChange={() => toggleType(type)}
            />
            {label}
          </label>
        ))}
      </div>
      <div className="button-grid">
        <button
          className="btn btn-primary"
          disabled={busy}
          onClick={() => subscribe(maskFromSelected())}
        >
          {subscribed ? "Update Subscription" : "Subscribe"}
        </button>
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={() => subscribe(0)}
        >
          Unsubscribe
        </button>
        <button
          className="btn btn-secondary"
          disabled={busy || !subscribed}
          onClick={poll}
        >
          Poll Now
        </button>
        <button className="btn btn-secondary" disabled={busy} onClick={clear}>
          Clear
        </button>
      </div>
      {status && (
        <div className="response-box">
          <pre>{status}</pre>
        </div>
      )}
      {events.length > 0 && (
        <div className="response-box log-list">
          <pre>{events.join("\n")}</pre>
        </div>
      )}
    </section>
  );
}

function levelLabel(level: LogLevel): string {
  switch (level) {
    case LogLevel.LOG_LEVEL_ERR:
      return "ERR";
    case LogLevel.LOG_LEVEL_WRN:
      return "WRN";
    case LogLevel.LOG_LEVEL_INF:
      return "INF";
    case LogLevel.LOG_LEVEL_DBG:
      return "DBG";
    default:
      return "?";
  }
}

function formatLogRecord(record: LogRecord): string {
  return `[${record.timestampMs}ms] <${levelLabel(record.level)}> ${record.source}: ${record.message}`;
}

const FILTER_LEVEL_OPTIONS: { value: LogLevel; label: string }[] = [
  { value: LogLevel.LOG_LEVEL_ERR, label: "ERR" },
  { value: LogLevel.LOG_LEVEL_WRN, label: "WRN" },
  { value: LogLevel.LOG_LEVEL_INF, label: "INF" },
  { value: LogLevel.LOG_LEVEL_DBG, label: "DBG" },
];

export function LogCaptureSection() {
  const { zmkApp, subsystem } = useDevtoolSubsystem();
  const [cursor, setCursor] = useState(0);
  const [records, setRecords] = useState<string[]>([]);
  const [filterSource, setFilterSource] = useState("");
  const [filterLevel, setFilterLevel] = useState<LogLevel>(
    LogLevel.LOG_LEVEL_INF
  );
  const [streaming, setStreaming] = useState(false);
  // Accumulated across the current streaming session: droppedCount is a
  // per-notification delta, summed here.
  const [streamDropped, setStreamDropped] = useState(0);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState<string | null>(null);

  // While streaming is on, push each LogStreamNotification straight into the
  // list. React state setters are stable, so this only re-subscribes when the
  // streaming toggle or the connection/subsystem changes.
  const subscriptionIndex = subsystem?.index;
  const onNotification = zmkApp?.onNotification;
  useEffect(() => {
    if (!streaming || subscriptionIndex === undefined || !onNotification)
      return;
    return onNotification({
      type: "custom",
      subsystemIndex: subscriptionIndex,
      callback: (n) => {
        if (!n.payload) return;
        const ls = Notification.decode(n.payload).logStream;
        if (!ls) return;
        setRecords((prev) =>
          [...prev, ...ls.records.map(formatLogRecord)].slice(-200)
        );
        if (ls.droppedCount > 0) {
          setStreamDropped((prev) => prev + ls.droppedCount);
        }
      },
    });
  }, [streaming, subscriptionIndex, onNotification]);

  if (!zmkApp || !subsystem) return null;

  const poll = async () => {
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({ getLogs: { cursor } })
      );
      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
        return;
      }
      const page = resp.getLogs!;
      setCursor(page.nextCursor);
      setRecords((prev) =>
        [...prev, ...page.records.map(formatLogRecord)].slice(-200)
      );
      if (page.droppedCount > 0) {
        setStatus(
          `Warning: ${page.droppedCount} record(s) dropped (buffer overflow between polls)`
        );
      }
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const toggleStreaming = async () => {
    const next = !streaming;
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({ setLogStreaming: { enabled: next } })
      );
      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
        return;
      }
      const enabled = resp.setLogStreaming?.enabled ?? next;
      if (enabled) {
        setStreamDropped(0);
      }
      setStreaming(enabled);
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const clear = async () => {
    setBusy(true);
    setStatus(null);
    try {
      await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({ clearLogs: {} })
      );
      setCursor(0);
      setRecords([]);
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  const applyFilter = async () => {
    setBusy(true);
    setStatus(null);
    try {
      const resp = await callDevtoolRpc(
        zmkApp,
        subsystem.index,
        Request.create({
          setLogCaptureFilter: { source: filterSource, minLevel: filterLevel },
        })
      );
      setStatus(
        resp.error
          ? `Error: ${resp.error.message}`
          : `Filter applied${filterSource ? ` for "${filterSource}"` : " (default)"}`
      );
    } catch (error) {
      setStatus(
        `Failed: ${error instanceof Error ? error.message : "Unknown error"}`
      );
    } finally {
      setBusy(false);
    }
  };

  return (
    <section className="card">
      <h2>Log Capture</h2>
      <div className="button-grid">
        <label>
          Source (empty = default level)
          <input
            type="text"
            value={filterSource}
            onChange={(e) => setFilterSource(e.target.value)}
            placeholder="e.g. my_driver"
          />
        </label>
        <label>
          Min level
          <select
            value={filterLevel}
            onChange={(e) => setFilterLevel(Number(e.target.value) as LogLevel)}
          >
            {FILTER_LEVEL_OPTIONS.map((opt) => (
              <option key={opt.value} value={opt.value}>
                {opt.label}
              </option>
            ))}
          </select>
        </label>
      </div>
      <div className="button-grid">
        <button
          className="btn btn-secondary"
          disabled={busy}
          onClick={applyFilter}
        >
          Apply Filter
        </button>
        <button
          className="btn btn-primary"
          disabled={busy || streaming}
          onClick={poll}
        >
          Poll Now
        </button>
        <button
          className={streaming ? "btn btn-primary" : "btn btn-secondary"}
          disabled={busy}
          onClick={toggleStreaming}
        >
          {streaming ? "Stop Streaming" : "Start Streaming"}
        </button>
        <button className="btn btn-secondary" disabled={busy} onClick={clear}>
          Clear
        </button>
      </div>
      {streaming && (
        <p className="hint">
          Streaming: new log records are pushed live (no need to poll). Dropped:{" "}
          {streamDropped} (buffer overflow)
        </p>
      )}
      {status && (
        <div className="response-box">
          <pre>{status}</pre>
        </div>
      )}
      {records.length > 0 && (
        <div className="response-box log-list">
          <pre>{records.join("\n")}</pre>
        </div>
      )}
    </section>
  );
}

export default App;
