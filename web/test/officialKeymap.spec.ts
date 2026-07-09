import { createMockConnection } from "@cormoran/zmk-studio-react-hook/testing";
import { isUnlockRequiredError } from "@cormoran/zmk-studio-react-hook";
import { loadOfficialKeymap, OfficialKeymapError } from "../src/officialKeymap";

// Mock the ZMK client's call_rpc so we can assert the exact sequence of
// official (core) RPC requests loadOfficialKeymap() issues, and control
// its responses/failures directly -- same pattern as
// FastKeymapSection.spec.tsx / App.spec.tsx's fixtures.
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
  // Real `Response` (not mocked) -- loadOfficialKeymap()'s readBytes
  // tracking re-encodes each response via `Response.encode()`, so this
  // needs the actual generated protobuf encoder, not a jest.fn() stub.
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

// eslint-disable-next-line @typescript-eslint/no-require-imports
const zmkClient = require("@zmkfirmware/zmk-studio-ts-client");

interface OfficialRequest {
  behaviors?: {
    listAllBehaviors?: boolean;
    getBehaviorDetails?: { behaviorId: number };
  };
  keymap?: { getKeymap?: boolean };
}

describe("loadOfficialKeymap", () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  it("issues list_all_behaviors, one get_behavior_details per id, then get_keymap, and parses the responses", async () => {
    zmkClient.call_rpc.mockImplementation(
      (_connection: unknown, req: OfficialRequest) => {
        if (req.behaviors?.listAllBehaviors) {
          return Promise.resolve({
            behaviors: { listAllBehaviors: { behaviors: [1, 2] } },
          });
        }
        if (req.behaviors?.getBehaviorDetails) {
          const id = req.behaviors.getBehaviorDetails.behaviorId;
          return Promise.resolve({
            behaviors: {
              getBehaviorDetails: {
                id,
                displayName: `Behavior ${id}`,
                metadata: [],
              },
            },
          });
        }
        if (req.keymap?.getKeymap) {
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

    const connection = createMockConnection();
    const result = await loadOfficialKeymap(connection);

    // Exact call sequence: list_all_behaviors, then get_behavior_details
    // per id (in id order), then get_keymap.
    expect(zmkClient.call_rpc).toHaveBeenCalledTimes(4);
    expect(zmkClient.call_rpc.mock.calls[0][1]).toEqual({
      behaviors: { listAllBehaviors: true },
    });
    expect(zmkClient.call_rpc.mock.calls[1][1]).toEqual({
      behaviors: { getBehaviorDetails: { behaviorId: 1 } },
    });
    expect(zmkClient.call_rpc.mock.calls[2][1]).toEqual({
      behaviors: { getBehaviorDetails: { behaviorId: 2 } },
    });
    expect(zmkClient.call_rpc.mock.calls[3][1]).toEqual({
      keymap: { getKeymap: true },
    });

    expect(result.rpcCalls).toBe(4);
    expect(result.readBytes).toBeGreaterThan(0);

    // Per-RPC-method breakdown: method names + call counts match the calls
    // actually made above -- list_all_behaviors and get_keymap once each,
    // get_behavior_details once per id (two ids here).
    const byMethod = new Map(result.byMethod.map((m) => [m.method, m]));
    expect(byMethod.get("list_all_behaviors")?.calls).toBe(1);
    expect(byMethod.get("get_behavior_details")?.calls).toBe(2);
    expect(byMethod.get("get_keymap")?.calls).toBe(1);
    const sumCalls = result.byMethod.reduce((n, m) => n + m.calls, 0);
    const sumBytes = result.byMethod.reduce((n, m) => n + m.bytes, 0);
    expect(sumCalls).toBe(result.rpcCalls);
    expect(sumBytes).toBe(result.readBytes);
    for (const stat of result.byMethod) {
      expect(typeof stat.ms).toBe("number");
      expect(stat.ms).toBeGreaterThanOrEqual(0);
    }

    expect(result.behaviors).toEqual([
      { id: 1, displayName: "Behavior 1", metadata: [] },
      { id: 2, displayName: "Behavior 2", metadata: [] },
    ]);
    expect(result.layers).toEqual([
      {
        id: 0,
        name: "Base",
        bindings: [{ behaviorId: 1, param1: 4, param2: 0 }],
      },
    ]);
  });

  it("wraps an UNLOCK_REQUIRED failure in OfficialKeymapError, preserving the round-trip count so far", async () => {
    zmkClient.call_rpc.mockImplementation(
      (_connection: unknown, req: OfficialRequest) => {
        if (req.behaviors?.listAllBehaviors) {
          return Promise.resolve({
            behaviors: { listAllBehaviors: { behaviors: [1] } },
          });
        }
        if (req.behaviors?.getBehaviorDetails) {
          return Promise.reject(new zmkClient.MetaError(UNLOCK_REQUIRED));
        }
        return Promise.reject(new Error("unexpected call_rpc request"));
      }
    );

    const connection = createMockConnection();

    let caught: unknown;
    try {
      await loadOfficialKeymap(connection);
    } catch (err) {
      caught = err;
    }

    expect(caught).toBeInstanceOf(OfficialKeymapError);
    const error = caught as OfficialKeymapError;
    // list_all_behaviors succeeded (1), get_behavior_details was attempted
    // and failed (2) -- get_keymap was never reached.
    expect(error.rpcCalls).toBe(2);
    // The list_all_behaviors response was read before the failure, so
    // readBytes reflects that partial read (not zero), even though the
    // load as a whole failed.
    expect(error.readBytes).toBeGreaterThan(0);
    expect(isUnlockRequiredError(error.cause)).toBe(true);

    // byMethod reflects the calls made before the failure: the successful
    // list_all_behaviors call is recorded, and get_behavior_details is NOT
    // recorded for its failed attempt (it only records completed calls --
    // rpcCalls above still counts the attempt, so byMethod's calls sum is
    // one less than rpcCalls here).
    const byMethod = new Map(error.byMethod.map((m) => [m.method, m]));
    expect(byMethod.get("list_all_behaviors")?.calls).toBe(1);
    expect(byMethod.has("get_behavior_details")).toBe(false);
    const sumCalls = error.byMethod.reduce((n, m) => n + m.calls, 0);
    const sumBytes = error.byMethod.reduce((n, m) => n + m.bytes, 0);
    expect(sumCalls).toBe(1);
    expect(sumBytes).toBe(error.readBytes);
  });
});
