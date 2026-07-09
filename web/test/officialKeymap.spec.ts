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
    expect(isUnlockRequiredError(error.cause)).toBe(true);
  });
});
