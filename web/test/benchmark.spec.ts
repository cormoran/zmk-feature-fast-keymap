import { createMockConnection } from "@cormoran/zmk-studio-react-hook/testing";
import {
  Request,
  Response,
  FK_DEFINITION_VERSION,
  STANDARD_BEHAVIORS,
  defFp,
  type FastKeymapCall,
  type Snapshot,
} from "zmk-feature-fast-keymap";
import { runBenchmark } from "../src/benchmark";

// Mock the ZMK client's call_rpc -- only the official loader
// (officialKeymap.ts, exercised through runBenchmark()) talks to it
// directly; the fast-keymap side is driven through a plain FastKeymapCall
// fixture below, same pattern as fastKeymap.spec.ts.
jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  create_rpc_connection: jest.fn(),
  call_rpc: jest.fn(),
  // Real `Response` (not mocked) -- officialKeymap.ts's readBytes tracking
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

// eslint-disable-next-line @typescript-eslint/no-require-imports
const zmkClient = require("@zmkfirmware/zmk-studio-ts-client");

interface OfficialRequest {
  behaviors?: {
    listAllBehaviors?: boolean;
    getBehaviorDetails?: { behaviorId: number };
  };
  keymap?: { getKeymap?: boolean };
}

// -- fast-keymap fixture: a 1-behavior (bundled &kp), 1-layer (no edits)
// device -- deliberately minimal, mirroring FastKeymapSection.spec.tsx's
// fixture. What matters here is only the *shape* of the result and that a
// warm cache makes the 2nd load issue fewer custom RPC calls than the
// initial one -- not the exact call count.
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
  defFpBits: 32, // fixture uses full defFp() values (identity mask)
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

function buildFastKeymapResponse(request: Request): Response {
  if (request.getSnapshot) {
    return { snapshot: SNAPSHOT };
  }
  if (request.listBehaviors) {
    // &kp is genuinely standard, so a cold-cache DETAILS_CUSTOM request
    // never inlines its details -- fast-keymap resolves it from the bundle
    // by def_fp instead (mirrors FastKeymapSection.spec.tsx's fixture).
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
  if (request.getLayers) {
    // Cold load batches every layer's effective bindings in one call (SS7).
    return {
      getLayers: {
        layers: request.getLayers.layerIds.map((id) => ({
          id,
          name: "Base",
          bindings: [{ behaviorId: 1, param1: 4, param2: 0 }],
        })),
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
    `benchmark.spec fixture: unhandled fast-keymap request ${JSON.stringify(request)}`
  );
}

const fastKeymapCall: FastKeymapCall = async (request) =>
  buildFastKeymapResponse(request);

function mockOfficialSuccess() {
  zmkClient.call_rpc.mockImplementation(
    (_connection: unknown, req: OfficialRequest) => {
      if (req.behaviors?.listAllBehaviors) {
        // Two ids -- exercises the get_behavior_details per-behavior
        // fan-out (benchmark.spec.ts's byMethod assertion below wants
        // calls > 1 for it), same as officialKeymap.spec.ts's fixture.
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
}

function mockOfficialLocked() {
  zmkClient.call_rpc.mockImplementation(
    (_connection: unknown, req: OfficialRequest) => {
      if (req.behaviors?.listAllBehaviors) {
        return Promise.resolve({
          behaviors: { listAllBehaviors: { behaviors: [1] } },
        });
      }
      // get_behavior_details (and get_keymap) are SECURED -- reject with
      // the UNLOCK_REQUIRED MetaError, as the real device would while
      // locked.
      return Promise.reject(new zmkClient.MetaError(UNLOCK_REQUIRED));
    }
  );
}

describe("runBenchmark", () => {
  beforeEach(() => {
    jest.clearAllMocks();
  });

  it("runs all four measurements and shows fast-keymap's 2nd load hitting the cache (fewer RPC calls)", async () => {
    mockOfficialSuccess();
    const connection = createMockConnection();

    const result = await runBenchmark(fastKeymapCall, connection, {
      deviceKey: "test-device",
    });

    // Shape: four measurements, each with a numeric ms, rpcCalls, and
    // readBytes, no errors on this happy path.
    for (const measurement of [
      result.fastKeymap.initial,
      result.fastKeymap.second,
      result.official.initial,
      result.official.second,
    ]) {
      expect(typeof measurement.ms).toBe("number");
      expect(typeof measurement.rpcCalls).toBe("number");
      expect(typeof measurement.readBytes).toBe("number");
      expect(measurement.error).toBeUndefined();

      // Per-RPC-method breakdown: non-empty, and its per-method sums equal
      // this measurement's own aggregates -- byMethod is a faithful
      // decomposition of rpcCalls/readBytes, not a separate/divergent count.
      expect(measurement.byMethod.length).toBeGreaterThan(0);
      const sumCalls = measurement.byMethod.reduce((n, m) => n + m.calls, 0);
      const sumBytes = measurement.byMethod.reduce((n, m) => n + m.bytes, 0);
      expect(sumCalls).toBe(measurement.rpcCalls);
      expect(sumBytes).toBe(measurement.readBytes);
      for (const stat of measurement.byMethod) {
        expect(typeof stat.ms).toBe("number");
        expect(stat.ms).toBeGreaterThanOrEqual(0);
      }
    }

    // fast-keymap's initial (cold-cache) load fetches the snapshot plus at
    // least one layer/behavior method -- see fastKeymap.ts's loadFastKeymap().
    const fastMethods = result.fastKeymap.initial.byMethod.map((m) => m.method);
    expect(fastMethods).toContain("getSnapshot");
    expect(
      fastMethods.some((m) =>
        ["listBehaviors", "getBehaviors", "getLayer"].includes(m)
      )
    ).toBe(true);

    // The official path's per-behavior fan-out: get_behavior_details is
    // called once per behavior id (mockOfficialSuccess() below lists two),
    // so its breakdown entry shows more than one call.
    const officialDetails = result.official.initial.byMethod.find(
      (m) => m.method === "get_behavior_details"
    );
    expect(officialDetails).toBeDefined();
    expect(officialDetails!.calls).toBeGreaterThan(1);

    // The cache-hit assertion: fast-keymap's initial load has to talk to
    // the device (get_snapshot + list_behaviors + get_layer +
    // get_physical_layouts here), while its 2nd load, now warm, should
    // need strictly fewer custom RPC round-trips -- ideally just the
    // get_snapshot fingerprint check.
    expect(result.fastKeymap.initial.rpcCalls).toBeGreaterThan(0);
    expect(result.fastKeymap.second.rpcCalls).toBeGreaterThan(0);
    expect(result.fastKeymap.second.rpcCalls).toBeLessThan(
      result.fastKeymap.initial.rpcCalls
    );

    // The same cache-hit effect, in bytes: fast-keymap's 2nd load reads
    // strictly fewer response bytes than its initial (cold) load, since it
    // skips most of the round-trips the initial load needed.
    expect(result.fastKeymap.initial.readBytes).toBeGreaterThan(0);
    expect(result.fastKeymap.second.readBytes).toBeGreaterThan(0);
    expect(result.fastKeymap.second.readBytes).toBeLessThan(
      result.fastKeymap.initial.readBytes
    );

    // The official path has no client cache: both loads should cost the
    // same number of round-trips (list_all_behaviors + 2x
    // get_behavior_details + get_keymap = 4 here), unlike fast-keymap's 2nd
    // load above.
    expect(result.official.initial.rpcCalls).toBe(4);
    expect(result.official.second.rpcCalls).toBe(4);

    // ...and, since it re-fetches the exact same data every time, the exact
    // same number of response bytes too -- unlike fast-keymap's 2nd load
    // above.
    expect(result.official.initial.readBytes).toBeGreaterThan(0);
    expect(result.official.initial.readBytes).toBe(
      result.official.second.readBytes
    );
  });

  it("reports the official path's unlock requirement without failing the fast-keymap measurements", async () => {
    mockOfficialLocked();
    const connection = createMockConnection();

    const result = await runBenchmark(fastKeymapCall, connection);

    expect(result.official.initial.error).toBeDefined();
    expect(result.official.initial.unlockRequired).toBe(true);
    expect(result.official.second.unlockRequired).toBe(true);

    // The benchmark doesn't abort -- fast-keymap's own (unsecured) path
    // still ran and succeeded, and still shows the cache-hit effect.
    expect(result.fastKeymap.initial.error).toBeUndefined();
    expect(result.fastKeymap.second.error).toBeUndefined();
    expect(result.fastKeymap.second.rpcCalls).toBeLessThan(
      result.fastKeymap.initial.rpcCalls
    );
  });

  it("forwards the field-selection options to the fast-keymap load requests", async () => {
    mockOfficialSuccess();
    const connection = createMockConnection();
    const requests: Request[] = [];
    const recordingCall: FastKeymapCall = async (request) => {
      requests.push(request);
      return buildFastKeymapResponse(request);
    };

    await runBenchmark(recordingCall, connection, {
      deviceKey: "field-selection",
      includeAliases: false,
      includeDisplayNames: false,
    });

    const listReq = requests.find((r) => r.listBehaviors)?.listBehaviors;
    expect(listReq?.includeAlias).toBe(false);
    expect(listReq?.excludeDisplayName).toBe(true);
  });
});
