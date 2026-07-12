import { render, screen } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { StackUsageSection, SUBSYSTEM_IDENTIFIER } from "../src/App";

describe("StackUsageSection Component", () => {
  it("should render the refresh control when the subsystem is found", () => {
    const mockZMKApp = createConnectedMockZMKApp({
      deviceName: "Test Device",
      subsystems: [SUBSYSTEM_IDENTIFIER],
    });

    render(
      <ZMKAppProvider value={mockZMKApp}>
        <StackUsageSection />
      </ZMKAppProvider>
    );

    expect(
      screen.getByRole("heading", { name: "Stack Usage" })
    ).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Refresh" })).toBeInTheDocument();
    expect(
      screen.getByText(/CONFIG_ZMK_DEVTOOL_STACK_USAGE/i)
    ).toBeInTheDocument();
  });

  it("should not render when the subsystem is absent", () => {
    const mockZMKApp = createConnectedMockZMKApp({ subsystems: [] });

    const { container } = render(
      <ZMKAppProvider value={mockZMKApp}>
        <StackUsageSection />
      </ZMKAppProvider>
    );

    expect(container.firstChild).toBeNull();
  });

  it("should not render without a ZMKAppContext", () => {
    const { container } = render(<StackUsageSection />);

    expect(container.firstChild).toBeNull();
  });
});
