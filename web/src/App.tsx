import { useContext, useState } from "react";
import "./App.css";
import { connect as serial_connect } from "@zmkfirmware/zmk-studio-ts-client/transport/serial";
import {
  ZMKConnection,
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  StudioLockState,
} from "./proto/cormoran/devtool/devtool";

export const SUBSYSTEM_IDENTIFIER = "cormoran__devtool";

type Operation = "unlock" | "lock" | "bootloader" | "reboot";

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
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );

      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);

      if (!responsePayload) {
        setStatus("No response payload");
        return;
      }

      const resp = Response.decode(responsePayload);

      if (resp.error) {
        setStatus(`Error: ${resp.error.message}`);
      } else if (resp.setStudioLockState) {
        setStatus(`Studio is ${lockStateLabel(resp.setStudioLockState.state)}`);
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

export default App;
