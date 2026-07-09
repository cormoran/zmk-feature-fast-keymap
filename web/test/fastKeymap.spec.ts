import {
  exportKeymapText,
  FastKeymapCacheKeys,
  loadFastKeymap,
  FK_DEFINITION_VERSION,
  bundledMetadataFp,
  type FastKeymapCall,
  type BehaviorSummary,
  type EditedBinding,
  type GetBehaviorDetailsResponse,
  type PhysicalLayouts,
  type Request,
  type Snapshot,
} from "zmk-feature-fast-keymap";

// -- fixture: a 2-behavior (1 bundled/kp, 1 custom), 2-layer device --------
//
// Layer "Base" (id 0): fp === defaultFp (no edits) -> exercised via
// get_layer.
// Layer "Fn" (id 1): fp !== defaultFp (one edited position) -> exercised
// via get_default_layer + get_edited_layer, overlaid.

const KP_BEHAVIOR: BehaviorSummary = {
  localId: 1,
  alias: "kp",
  displayName: "Key Press",
  metadataFp: bundledMetadataFp("kp")!,
  standard: true,
};

const CUSTOM_BEHAVIOR: BehaviorSummary = {
  localId: 2,
  alias: "my_custom",
  displayName: "My Custom",
  metadataFp: 999999, // does not match any bundled alias
  standard: false,
};

const CUSTOM_BEHAVIOR_DETAILS: GetBehaviorDetailsResponse = {
  id: 2,
  alias: "my_custom",
  displayName: "My Custom",
  metadataFp: CUSTOM_BEHAVIOR.metadataFp,
  metadata: [{ param1: [{ name: "Value", constant: 1 }], param2: [] }],
};

const BASE_DEFAULT_BINDINGS = [
  { behaviorId: 1, param1: 4, param2: 0 },
  { behaviorId: 1, param1: 5, param2: 0 },
];

const FN_DEFAULT_BINDINGS = [
  { behaviorId: 1, param1: 6, param2: 0 },
  { behaviorId: 1, param1: 7, param2: 0 },
];

const FN_EDITS: EditedBinding[] = [
  { position: 1, binding: { behaviorId: 2, param1: 9, param2: 0 } },
];

const FN_EFFECTIVE_BINDINGS = [
  { behaviorId: 1, param1: 6, param2: 0 },
  { behaviorId: 2, param1: 9, param2: 0 },
];

const LAYOUTS: PhysicalLayouts = {
  activeLayoutIndex: 0,
  layouts: [
    {
      name: "Default",
      keys: [{ width: 100, height: 100, x: 0, y: 0, r: 0, rx: 0, ry: 0 }],
    },
  ],
};

const SNAPSHOT: Snapshot = {
  definitionVersion: FK_DEFINITION_VERSION,
  behaviorsFp: 555,
  defaultKeymapFp: 777,
  keymapFp: 888,
  physicalLayoutFp: 300,
  behaviorCount: 2,
  maxLayerNameLength: 16,
  availableLayers: 2,
  activeLayoutIndex: 0,
  layers: [
    {
      id: 0,
      name: "Base",
      bindingCount: 2,
      fp: 100,
      defaultFp: 100,
      editedCount: 0,
    },
    {
      id: 1,
      name: "Fn",
      bindingCount: 2,
      fp: 200,
      defaultFp: 201,
      editedCount: 1,
    },
  ],
};

/** A fake FastKeymapCall backed by the fixture above, recording every
 * request it receives (in typed form -- no wire encoding involved, since
 * loadFastKeymap() talks to `call` directly) so tests can assert exactly
 * which RPCs were (not) made. */
function makeDeviceCall(): { call: FastKeymapCall; requests: Request[] } {
  const requests: Request[] = [];

  const call: FastKeymapCall = async (request) => {
    requests.push(request);

    if (request.getSnapshot) {
      return { snapshot: SNAPSHOT };
    }
    if (request.listBehaviors) {
      return { listBehaviors: { behaviors: [KP_BEHAVIOR, CUSTOM_BEHAVIOR] } };
    }
    if (request.getBehaviorDetails) {
      if (request.getBehaviorDetails.behaviorId === 2) {
        return { getBehaviorDetails: CUSTOM_BEHAVIOR_DETAILS };
      }
      throw new Error(
        `unexpected get_behavior_details(${request.getBehaviorDetails.behaviorId})`
      );
    }
    if (request.getLayer) {
      if (request.getLayer.layerId === 0) {
        return {
          getLayer: { id: 0, name: "Base", bindings: BASE_DEFAULT_BINDINGS },
        };
      }
      throw new Error(`unexpected get_layer(${request.getLayer.layerId})`);
    }
    if (request.getDefaultLayer) {
      if (request.getDefaultLayer.layerId === 1) {
        return {
          getDefaultLayer: { id: 1, name: "Fn", bindings: FN_DEFAULT_BINDINGS },
        };
      }
      throw new Error(
        `unexpected get_default_layer(${request.getDefaultLayer.layerId})`
      );
    }
    if (request.getEditedLayer) {
      if (request.getEditedLayer.layerId === 1) {
        return { getEditedLayer: { id: 1, bindings: FN_EDITS } };
      }
      throw new Error(
        `unexpected get_edited_layer(${request.getEditedLayer.layerId})`
      );
    }
    if (request.getPhysicalLayouts) {
      return { getPhysicalLayouts: LAYOUTS };
    }

    throw new Error(`unhandled request: ${JSON.stringify(request)}`);
  };

  return { call, requests };
}

function requestKinds(requests: Request[]): string[] {
  return requests.map(
    (r) =>
      Object.keys(r).find(
        (k) => (r as Record<string, unknown>)[k] !== undefined
      ) ?? "?"
  );
}

const DEVICE_KEY = "Test Device";

beforeEach(() => {
  localStorage.clear();
});

describe("loadFastKeymap", () => {
  it("cold cache: fetches everything and assembles the correct model", async () => {
    const { call, requests } = makeDeviceCall();

    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    // get_snapshot, list_behaviors, get_behavior_details(custom only),
    // get_layer(0), get_default_layer(1), get_edited_layer(1),
    // get_physical_layouts.
    expect(requestKinds(requests)).toEqual([
      "getSnapshot",
      "listBehaviors",
      "getBehaviorDetails",
      "getLayer",
      "getDefaultLayer",
      "getEditedLayer",
      "getPhysicalLayouts",
    ]);

    expect(model.behaviors).toHaveLength(2);
    const kp = model.behaviors.find((b) => b.localId === 1);
    const custom = model.behaviors.find((b) => b.localId === 2);
    expect(kp?.resolvedFrom).toBe("bundle");
    expect(kp?.alias).toBe("kp");
    expect(custom?.resolvedFrom).toBe("device");
    expect(custom?.metadata).toEqual(CUSTOM_BEHAVIOR_DETAILS.metadata);

    expect(model.layers).toHaveLength(2);
    const base = model.layers.find((l) => l.id === 0)!;
    const fn = model.layers.find((l) => l.id === 1)!;
    expect(base.bindings).toEqual(BASE_DEFAULT_BINDINGS);
    expect(base.loadedFromCache).toBe(false);
    expect(fn.bindings).toEqual(FN_EFFECTIVE_BINDINGS);
    expect(fn.loadedFromCache).toBe(false);

    expect(model.layouts).toEqual(LAYOUTS.layouts);
    expect(model.activeLayoutIndex).toBe(0);

    expect(model.stats).toEqual({
      behaviorsFromCache: false,
      behaviorsBundled: 1,
      behaviorsFetched: 1,
      layersFromCache: 0,
      layersFetchedFull: 1,
      layersFetchedDiff: 1,
      physicalLayoutsFromCache: false,
    });
  });

  it("bundled skip: does not fetch get_behavior_details for a bundled, fp-matching behavior", async () => {
    const { call, requests } = makeDeviceCall();
    await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    const detailsRequests = requests.filter((r) => r.getBehaviorDetails);
    expect(detailsRequests).toHaveLength(1);
    expect(detailsRequests[0].getBehaviorDetails?.behaviorId).toBe(2); // only the custom one
  });

  it("snapshot cache hit: a warm cache makes only get_snapshot, no other RPCs", async () => {
    // Seed localStorage directly (rather than depending on a prior load) so
    // this test is self-contained and asserts the cache-hit path in
    // isolation.
    localStorage.setItem(
      FastKeymapCacheKeys.behaviors(DEVICE_KEY),
      JSON.stringify({
        fp: SNAPSHOT.behaviorsFp,
        behaviors: [
          {
            localId: 1,
            alias: "kp",
            displayName: "Key Press",
            metadataFp: KP_BEHAVIOR.metadataFp,
            standard: true,
            metadata: [],
          },
          {
            localId: 2,
            alias: "my_custom",
            displayName: "My Custom",
            metadataFp: CUSTOM_BEHAVIOR.metadataFp,
            standard: false,
            metadata: CUSTOM_BEHAVIOR_DETAILS.metadata,
          },
        ],
      })
    );
    localStorage.setItem(
      FastKeymapCacheKeys.layer(DEVICE_KEY, 100),
      JSON.stringify(BASE_DEFAULT_BINDINGS)
    );
    localStorage.setItem(
      FastKeymapCacheKeys.layer(DEVICE_KEY, 200),
      JSON.stringify(FN_EFFECTIVE_BINDINGS)
    );
    localStorage.setItem(
      FastKeymapCacheKeys.layouts(DEVICE_KEY),
      JSON.stringify({
        fp: SNAPSHOT.physicalLayoutFp,
        activeLayoutIndex: 0,
        layouts: LAYOUTS.layouts,
      })
    );

    const { call, requests } = makeDeviceCall();
    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    expect(requestKinds(requests)).toEqual(["getSnapshot"]);

    expect(model.stats).toEqual({
      behaviorsFromCache: true,
      behaviorsBundled: 0,
      behaviorsFetched: 0,
      layersFromCache: 2,
      layersFetchedFull: 0,
      layersFetchedDiff: 0,
      physicalLayoutsFromCache: true,
    });
    expect(model.layers.find((l) => l.id === 1)?.bindings).toEqual(
      FN_EFFECTIVE_BINDINGS
    );
    expect(model.behaviors.every((b) => b.resolvedFrom === "cache")).toBe(true);
  });

  it("edited-diff assembly: a cached default layer + get_edited_layer overlays correctly", async () => {
    // Seed only the default-layer cache for layer 1 (as if a previous load
    // already cached it, e.g. via another layer sharing the same
    // default_fp) -- current-layer and behaviors/layouts caches are cold.
    localStorage.setItem(
      FastKeymapCacheKeys.defaultLayer(DEVICE_KEY, 201),
      JSON.stringify(FN_DEFAULT_BINDINGS)
    );

    const { call, requests } = makeDeviceCall();
    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    // get_default_layer(1) must NOT be called -- the cached default is
    // reused; only the edited diff is fetched.
    expect(requests.some((r) => r.getDefaultLayer)).toBe(false);
    expect(requests.filter((r) => r.getEditedLayer)).toHaveLength(1);

    const fn = model.layers.find((l) => l.id === 1)!;
    expect(fn.bindings).toEqual(FN_EFFECTIVE_BINDINGS);
    expect(fn.loadedFromCache).toBe(false);
    expect(model.stats.layersFetchedDiff).toBe(1);
  });

  it("throws a descriptive error when the device returns an error response", async () => {
    const call: FastKeymapCall = async (request) => {
      if (request.getSnapshot) {
        return { error: { code: 0 } };
      }
      throw new Error("unexpected request");
    };

    await expect(
      loadFastKeymap(call, { deviceKey: DEVICE_KEY })
    ).rejects.toThrow(/ERR_GENERIC/);
  });
});

describe("exportKeymapText", () => {
  it("renders each layer as a bindings block using resolved aliases", async () => {
    const { call } = makeDeviceCall();
    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    const text = exportKeymapText(model);

    expect(text).toBe(
      [
        "keymap {",
        '    compatible = "zmk,keymap";',
        "",
        "    base {",
        "        bindings = < &kp 4 &kp 5 >;",
        "    };",
        "",
        "    fn {",
        "        bindings = < &kp 6 &my_custom 9 >;",
        "    };",
        "};",
      ].join("\n")
    );
  });
});
