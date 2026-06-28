import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { RPCTestSection, SUBSYSTEM_IDENTIFIER } from "../src/App";

describe("RPCTestSection Component", () => {
  describe("With Subsystem", () => {
    it("should render RPC controls when subsystem is found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/Devtool RPC/i)).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Unlock Studio" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Lock Studio" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Get Lock State" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Enter Bootloader" })
      ).toBeInTheDocument();
      expect(
        screen.getByRole("button", { name: "Reboot" })
      ).toBeInTheDocument();
    });

    it("should not show a result before an RPC is sent", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(screen.queryByText(/Result/i)).not.toBeInTheDocument();
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <RPCTestSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__devtool" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the ZMK Devtool module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<RPCTestSection />);

      expect(container.firstChild).toBeNull();
    });
  });
});
