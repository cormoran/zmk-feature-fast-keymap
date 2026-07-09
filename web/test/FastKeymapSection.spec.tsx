import { act, render, screen, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { FastKeymapSection, SUBSYSTEM_IDENTIFIER } from "../src/App";
import {
  Request,
  Response,
  FK_DEFINITION_VERSION,
  STANDARD_BEHAVIORS,
  defFp,
  type Snapshot,
} from "zmk-feature-fast-keymap";
import { LockState } from "@zmkfirmware/zmk-studio-ts-client/core";

// Mock the ZMK client so we can control call_rpc responses directly: both
// useStudioLockState's initial getLockState query and useCustomSubsystem's
// callRPC go through this module. Same pattern as App.spec.tsx /
// fastKeymap.spec.ts's fixture, just exercised over the wire (encode/decode)
// this time since we're testing the full component.
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
  // Real `Response` (not mocked) -- officialKeymap.ts's readBytes tracking
  // (exercised here through the Benchmark panel's "Run Benchmark" click)
  // re-encodes each response via `Response.encode()`, so this needs the
  // actual generated protobuf encoder, not a jest.fn() stub.
  Response: jest.requireActual("@zmkfirmware/zmk-studio-ts-client").Response,
  MetaError: class MetaError extends Error {
    condition: number;
    constructor(condition: number) {
      super(`meta error: ${condition}`);
      this.condition = condition;
      Object.setPrototypeOf(this, MetaError.prototype);
    }
  },
}));

const UNLOCK_REQUIRED = 1; // zmk.meta.ErrorConditions.UNLOCK_REQUIRED

// A minimal 1-behavior (bundled &kp), 1-layer (no edits) device fixture.
// The interesting cache/diff-assembly paths are covered directly against
// loadFastKeymap() in fastKeymap.spec.ts; this file only needs to prove the
// component wires the client up correctly end-to-end (over real
// encode/decode) and that the unlock flow still works.
const SNAPSHOT: Snapshot = {
  definitionVersion: FK_DEFINITION_VERSION,
  behaviorsFp: 1,
  defaultKeymapFp: 2,
  keymapFp: 3,
  physicalLayoutFp: 4,
  behaviorCount: 1,
  maxLayerNameLength: 8,
  availableLayers: 1,
  activeLayoutIndex: 0,
  defFpBits: 32, // fixture serves full defFp() values (identity mask)
  layers: [
    {
      id: 0,
      name: "Base",
      bindingCount: 1,
      fp: 10,
      defaultFp: 10,
      editedCount: 0,
    },
  ],
};

function buildResponse(request: Request): Response {
  if (request.getSnapshot) {
    return { snapshot: SNAPSHOT };
  }
  if (request.listBehaviors) {
    // Mirrors the firmware: &kp is genuinely standard, so DETAILS_CUSTOM
    // (the mode a cold per-device cache requests) never inlines its
    // details -- the component resolves it from the bundle by def_fp.
    return {
      listBehaviors: {
        behaviors: [
          {
            localId: 1,
            defFp: defFp(
              STANDARD_BEHAVIORS.kp.alias,
              STANDARD_BEHAVIORS.kp.displayName,
              STANDARD_BEHAVIORS.kp.metadata
            ),
            details: undefined,
          },
        ],
      },
    };
  }
  if (request.getLayer) {
    return {
      getLayer: {
        id: 0,
        name: "Base",
        bindings: [{ behaviorId: 1, param1: 4, param2: 0 }],
      },
    };
  }
  if (request.getPhysicalLayouts) {
    return {
      getPhysicalLayouts: {
        activeLayoutIndex: 0,
        layouts: [
          {
            name: "Default",
            fp: 20,
            keys: [],
            defaultWidth: 0,
            defaultHeight: 0,
          },
        ],
      },
    };
  }
  if (request.getPhysicalLayout) {
    return {
      getPhysicalLayout: {
        index: request.getPhysicalLayout.layoutIndex,
        layout: {
          name: "Default",
          fp: 20,
          keys: [],
          defaultWidth: 0,
          defaultHeight: 0,
        },
      },
    };
  }
  throw new Error(
    `FastKeymapSection.spec fixture: unhandled request ${JSON.stringify(request)}`
  );
}

describe("FastKeymapSection", () => {
  beforeEach(() => {
    localStorage.clear();
  });

  describe("Without ZMKAppContext", () => {
    it("should not render", () => {
      const { container } = render(<FastKeymapSection />);
      expect(container.firstChild).toBeNull();
    });
  });

  describe("Without subsystem", () => {
    it("shows a warning when the subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__fast_keymap" not found/i)
      ).toBeInTheDocument();
      const link = screen.getByRole("link", { name: /module README/i });
      expect(link).toHaveAttribute(
        "href",
        "https://github.com/cormoran/zmk-feature-fast-keymap#readme"
      );
    });
  });

  describe("Successful load", () => {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const zmkClient = require("@zmkfirmware/zmk-studio-ts-client");

    beforeEach(() => {
      jest.clearAllMocks();
      zmkClient.call_rpc.mockImplementation(
        (
          _connection: unknown,
          req: {
            core?: { getLockState?: boolean };
            custom?: { call?: { payload: Uint8Array } };
          }
        ) => {
          if (req.core?.getLockState) {
            return Promise.resolve({
              core: {
                getLockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
              },
            });
          }
          if (req.custom?.call) {
            const decoded = Request.decode(req.custom.call.payload);
            const response = buildResponse(decoded);
            return Promise.resolve({
              custom: { call: { payload: Response.encode(response).finish() } },
            });
          }
          return Promise.reject(new Error("unexpected call_rpc request"));
        }
      );
    });

    it("loads automatically on connect and renders behaviors, layers and the .keymap export", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/Fast Keymap/i)).toBeInTheDocument();

      await waitFor(() => {
        expect(screen.getByText(/Behaviors \(1\)/i)).toBeInTheDocument();
      });

      expect(screen.getByText("&kp")).toBeInTheDocument();
      expect(screen.getByText(/Key Press/i)).toBeInTheDocument();
      // Exact match: the stats badge above also contains the substring
      // "bundled" (e.g. "behaviors (1 bundled, 0 fetched)"), so a loose
      // regex would be ambiguous -- this asserts the per-behavior source
      // tag specifically.
      expect(screen.getByText("bundled")).toBeInTheDocument();

      expect(screen.getByText(/Layers \(1\)/i)).toBeInTheDocument();
      expect(screen.getByText("Base")).toBeInTheDocument();
      expect(screen.getByText("&kp 4")).toBeInTheDocument();

      const exportTextarea = screen.getByLabelText(
        /\.keymap export/i
      ) as HTMLTextAreaElement;
      expect(exportTextarea.value).toContain("&kp 4");
      expect(exportTextarea.value).toContain("base {");
    });

    it("shows a cache-hit indicator on a second load once the cache is warm", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText(/Behaviors \(1\)/i)).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.click(screen.getByText(/Reload/i));

      await waitFor(() => {
        expect(
          screen.getByText(/1 \/ 1 layers from cache/i)
        ).toBeInTheDocument();
      });
    });
  });

  describe("Unlock flow", () => {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const zmkClient = require("@zmkfirmware/zmk-studio-ts-client");

    beforeEach(() => {
      jest.clearAllMocks();
    });

    function mockCallRpc({
      lockState,
      customResult,
    }: {
      lockState: LockState;
      customResult: "unlock-required" | "success";
    }) {
      zmkClient.call_rpc.mockImplementation(
        (
          _connection: unknown,
          req: {
            core?: { getLockState?: boolean };
            custom?: { call?: { payload: Uint8Array } };
          }
        ) => {
          if (req.core?.getLockState) {
            return Promise.resolve({ core: { getLockState: lockState } });
          }
          if (req.custom?.call) {
            if (customResult === "unlock-required") {
              return Promise.reject(new zmkClient.MetaError(UNLOCK_REQUIRED));
            }
            const decoded = Request.decode(req.custom.call.payload);
            const response = buildResponse(decoded);
            return Promise.resolve({
              custom: { call: { payload: Response.encode(response).finish() } },
            });
          }
          return Promise.reject(new Error("unexpected call_rpc request"));
        }
      );
    }

    it("shows the unlock prompt when the initial load is rejected with UNLOCK_REQUIRED", async () => {
      mockCallRpc({
        lockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
        customResult: "unlock-required",
      });

      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText(/ZMK Studio is locked/i)).toBeInTheDocument();
      });
      expect(screen.getByText("Retry")).toBeInTheDocument();
    });

    it("auto-retries and renders once a lockStateChanged notification reports unlocked", async () => {
      mockCallRpc({
        lockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
        customResult: "unlock-required",
      });

      let coreCallback:
        ((notification: { lockStateChanged?: LockState }) => void) | undefined;
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });
      mockZMKApp.onNotification = jest.fn((subscription) => {
        if (subscription.type === "core") {
          coreCallback = subscription.callback;
        }
        return () => {};
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText("Retry")).toBeInTheDocument();
      });

      mockCallRpc({
        lockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
        customResult: "success",
      });

      expect(coreCallback).toBeDefined();
      await act(async () => {
        coreCallback?.({
          lockStateChanged: LockState.ZMK_STUDIO_CORE_LOCK_STATE_LOCKED,
        });
      });
      await act(async () => {
        coreCallback?.({
          lockStateChanged: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
        });
      });

      await waitFor(() => {
        expect(screen.getByText(/Behaviors \(1\)/i)).toBeInTheDocument();
      });
    });

    it("retries manually via the Retry button and renders the result", async () => {
      mockCallRpc({
        lockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
        customResult: "unlock-required",
      });

      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText("Retry")).toBeInTheDocument();
      });

      mockCallRpc({
        lockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
        customResult: "success",
      });

      const user = userEvent.setup();
      await user.click(screen.getByText("Retry"));

      await waitFor(() => {
        expect(screen.getByText(/Behaviors \(1\)/i)).toBeInTheDocument();
      });
    });
  });

  describe("Benchmark panel", () => {
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const zmkClient = require("@zmkfirmware/zmk-studio-ts-client");

    beforeEach(() => {
      jest.clearAllMocks();
    });

    interface CallRpcRequest {
      core?: { getLockState?: boolean };
      custom?: { call?: { payload: Uint8Array } };
      behaviors?: {
        listAllBehaviors?: boolean;
        getBehaviorDetails?: { behaviorId: number };
      };
      keymap?: { getKeymap?: boolean };
    }

    // Handles both the fast-keymap custom-subsystem traffic (reusing this
    // file's `buildResponse` fixture) and the official core `behaviors` /
    // `keymap` RPCs the benchmark panel's official path drives directly --
    // see src/officialKeymap.ts.
    function mockCallRpc({ officialLocked }: { officialLocked: boolean }) {
      zmkClient.call_rpc.mockImplementation(
        (_connection: unknown, req: CallRpcRequest) => {
          if (req.core?.getLockState) {
            return Promise.resolve({
              core: {
                getLockState: LockState.ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED,
              },
            });
          }
          if (req.custom?.call) {
            const decoded = Request.decode(req.custom.call.payload);
            const response = buildResponse(decoded);
            return Promise.resolve({
              custom: { call: { payload: Response.encode(response).finish() } },
            });
          }
          if (req.behaviors?.listAllBehaviors) {
            return Promise.resolve({
              behaviors: { listAllBehaviors: { behaviors: [1] } },
            });
          }
          if (req.behaviors?.getBehaviorDetails) {
            if (officialLocked) {
              return Promise.reject(new zmkClient.MetaError(UNLOCK_REQUIRED));
            }
            return Promise.resolve({
              behaviors: {
                getBehaviorDetails: {
                  id: 1,
                  displayName: "Key Press",
                  metadata: [],
                },
              },
            });
          }
          if (req.keymap?.getKeymap) {
            if (officialLocked) {
              return Promise.reject(new zmkClient.MetaError(UNLOCK_REQUIRED));
            }
            return Promise.resolve({
              keymap: {
                getKeymap: {
                  layers: [
                    {
                      id: 0,
                      name: "Base",
                      bindings: [{ behaviorId: 1, param1: 4, param2: 0 }],
                    },
                  ],
                  availableLayers: 4,
                  maxLayerNameLength: 8,
                },
              },
            });
          }
          return Promise.reject(new Error("unexpected call_rpc request"));
        }
      );
    }

    it("renders the fast-keymap vs. official comparison table after a run", async () => {
      mockCallRpc({ officialLocked: false });
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText(/Behaviors \(1\)/i)).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.click(screen.getByText(/Run Benchmark/i));

      await waitFor(() => {
        expect(
          screen.getByRole("cell", { name: "⚡ fast-keymap" })
        ).toBeInTheDocument();
        expect(
          screen.getByRole("cell", { name: "📡 official RPC" })
        ).toBeInTheDocument();
      });

      // The "Read bytes" column renders too, with human-readable, nonzero
      // byte counts for both loaders. (Matched by its "(initial / 2nd)"
      // suffix -- the per-RPC breakdown tables below add their own,
      // differently-labeled "Read bytes" columnheaders.)
      expect(
        screen.getByRole("columnheader", { name: /Read bytes \(initial/i })
      ).toBeInTheDocument();
      expect(
        screen.getAllByText(/\d+(\.\d+)? (B|KB) \/ \d+(\.\d+)? (B|KB)/)
      ).not.toHaveLength(0);

      // The collapsible per-RPC breakdown section renders too.
      expect(screen.getByText(/Per-RPC breakdown/i)).toBeInTheDocument();
    });

    it("surfaces the unlock message instead of crashing when the official path is locked", async () => {
      mockCallRpc({ officialLocked: true });
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <FastKeymapSection />
        </ZMKAppProvider>
      );

      await waitFor(() => {
        expect(screen.getByText(/Behaviors \(1\)/i)).toBeInTheDocument();
      });

      const user = userEvent.setup();
      await user.click(screen.getByText(/Run Benchmark/i));

      await waitFor(() => {
        expect(
          screen.getByText(/needs ZMK Studio to be unlocked/i)
        ).toBeInTheDocument();
      });

      // The fast-keymap side of the panel still succeeded -- only the
      // official path's two cells are reported as locked.
      expect(screen.getAllByText(/🔒 locked/i)).toHaveLength(2);
    });
  });
});
