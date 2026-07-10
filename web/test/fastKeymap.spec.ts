import {
  BehaviorDetailsMode,
  LayoutDetailsMode,
  exportKeymapText,
  FastKeymapCacheKeys,
  loadFastKeymap,
  loadPhysicalLayoutGeometry,
  FK_DEFINITION_VERSION,
  STANDARD_BEHAVIORS,
  defFp,
  type BehaviorDetails,
  type BehaviorSummary,
  type EditedBinding,
  type FastKeymapCall,
  type PhysicalLayoutsResponse,
  type Request,
  type Snapshot,
} from "zmk-feature-fast-keymap";

// -- fixture: a 3-behavior (1 bundled/kp, 1 custom, 1 def_fp-miss), 2-layer
// device --------------------------------------------------------------
//
// Layer "Base" (id 0): fp === defaultFp (no edits) -> exercised via
// get_layer.
// Layer "Fn" (id 1): fp !== defaultFp (one edited position) -> exercised
// via get_default_layer + get_edited_layer, overlaid.

interface BehaviorFixture {
  localId: number;
  alias: string;
  displayName: string;
  metadata: BehaviorDetails["metadata"];
  defFp: number;
  /** Mirrors the firmware's fk_behavior_is_standard() classification --
   * independent of whether the web bundle happens to recognize this
   * behavior's def_fp (that's MISS_BEHAVIOR's whole point below). */
  isStandard: boolean;
}

const KP_METADATA = STANDARD_BEHAVIORS.kp.metadata;
const KP_BEHAVIOR: BehaviorFixture = {
  localId: 1,
  alias: STANDARD_BEHAVIORS.kp.alias,
  displayName: STANDARD_BEHAVIORS.kp.displayName,
  metadata: KP_METADATA,
  defFp: defFp(
    STANDARD_BEHAVIORS.kp.alias,
    STANDARD_BEHAVIORS.kp.displayName,
    KP_METADATA
  ),
  isStandard: true,
};

const CUSTOM_METADATA: BehaviorDetails["metadata"] = [
  { param1: [{ name: "Value", constant: 1 }], param2: [] },
];
const CUSTOM_BEHAVIOR: BehaviorFixture = {
  localId: 2,
  alias: "my_custom",
  displayName: "My Custom",
  metadata: CUSTOM_METADATA,
  defFp: defFp("my_custom", "My Custom", CUSTOM_METADATA),
  isStandard: false,
};

// A behavior the FIRMWARE classifies as "standard" (so DETAILS_CUSTOM never
// inlines it) but whose def_fp the web bundle does NOT recognize (an
// unbundled/Kconfig-dependent standard behavior, DESIGN.md SS5) -- this is
// a genuine miss that only the get_behaviors batch can resolve.
const MISS_METADATA: BehaviorDetails["metadata"] = [];
const MISS_BEHAVIOR: BehaviorFixture = {
  localId: 3,
  alias: "mystery",
  displayName: "Mystery",
  metadata: MISS_METADATA,
  defFp: defFp("mystery", "Mystery", MISS_METADATA),
  isStandard: true,
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

// Two layouts (fp 900 active/"Default", fp 901 inactive/"Split") so tests
// can assert the active layout's geometry is fetched eagerly while the
// inactive one's stays lazy (DESIGN.md SS6).
const DEFAULT_LAYOUT_FP = 900;
const SPLIT_LAYOUT_FP = 901;
const DEFAULT_LAYOUT_KEYS = [
  { width: 100, height: 100, x: 0, y: 0, r: 0, rx: 0, ry: 0 },
];
const SPLIT_LAYOUT_KEYS = [
  { width: 100, height: 100, x: 0, y: 0, r: 0, rx: 0, ry: 0 },
  { width: 100, height: 100, x: 100, y: 0, r: 0, rx: 0, ry: 0 },
];

const LAYOUT_SUMMARIES: PhysicalLayoutsResponse = {
  activeLayoutIndex: 0,
  layouts: [
    { name: "Default", fp: DEFAULT_LAYOUT_FP, keys: [] },
    { name: "Split", fp: SPLIT_LAYOUT_FP, keys: [] },
  ],
};

const LAYOUT_GEOMETRY_BY_FP: Record<
  number,
  { name: string; keys: typeof DEFAULT_LAYOUT_KEYS }
> = {
  [DEFAULT_LAYOUT_FP]: { name: "Default", keys: DEFAULT_LAYOUT_KEYS },
  [SPLIT_LAYOUT_FP]: { name: "Split", keys: SPLIT_LAYOUT_KEYS },
};

// Single-layout fixture used by the tests that don't care about the
// lazy/inactive-layout behavior (unchanged shape from before this
// protocol's slim-summary rework, just carrying a `fp` now).
const LAYOUTS: PhysicalLayoutsResponse = {
  activeLayoutIndex: 0,
  layouts: [{ name: "Default", fp: DEFAULT_LAYOUT_FP, keys: [] }],
};

function makeSnapshot(overrides: Partial<Snapshot> = {}): Snapshot {
  return {
    definitionVersion: FK_DEFINITION_VERSION,
    behaviorsFp: 555,
    defaultKeymapFp: 777,
    keymapFp: 888,
    physicalLayoutFp: 300,
    behaviorCount: 2,
    maxLayerNameLength: 16,
    availableLayers: 2,
    activeLayoutIndex: 0,
    // The fixtures below carry full 32-bit defFp() values, so the mocked
    // device reports 32-bit width (identity mask). def_fp truncation math is
    // covered by standardBehaviors.spec.
    defFpBits: 32,
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
    ...overrides,
  };
}

const SNAPSHOT = makeSnapshot();

/** Builds the inline `details` a real device would serve for `fixture`
 * under `mode`, mirroring the firmware's inlining rule (SS5):
 * ALL -> always; CUSTOM -> only non-standard; NONE -> never. */
function summaryFor(
  fixture: BehaviorFixture,
  mode: BehaviorDetailsMode
): BehaviorSummary {
  const shouldInline =
    mode === BehaviorDetailsMode.BEHAVIOR_DETAILS_ALL ||
    (mode === BehaviorDetailsMode.BEHAVIOR_DETAILS_CUSTOM &&
      !fixture.isStandard);

  return {
    localId: fixture.localId,
    defFp: fixture.defFp,
    details: shouldInline
      ? {
          alias: fixture.alias,
          displayName: fixture.displayName,
          metadata: fixture.metadata,
        }
      : undefined,
  };
}

/** A fake FastKeymapCall backed by the fixtures above, recording every
 * request it receives (in typed form -- no wire encoding involved, since
 * loadFastKeymap() talks to `call` directly) so tests can assert exactly
 * which RPCs were (not) made.
 *
 * `behaviors` controls which fixtures list_behaviors reports (in this
 * order); the mock computes each one's inline `details` from the
 * request's mode, exactly like the firmware would. */
function makeDeviceCall(
  behaviors: BehaviorFixture[] = [KP_BEHAVIOR, CUSTOM_BEHAVIOR],
  snapshot: Snapshot = SNAPSHOT,
  layoutSummaries: PhysicalLayoutsResponse = LAYOUTS
): { call: FastKeymapCall; requests: Request[] } {
  const requests: Request[] = [];
  const byLocalId = new Map(behaviors.map((b) => [b.localId, b] as const));

  const call: FastKeymapCall = async (request) => {
    requests.push(request);

    if (request.getSnapshot) {
      return { snapshot };
    }
    if (request.listBehaviors) {
      return {
        listBehaviors: {
          behaviors: behaviors.map((b) =>
            summaryFor(b, request.listBehaviors!.details)
          ),
        },
      };
    }
    if (request.getBehaviors) {
      return {
        getBehaviors: {
          behaviors: request.getBehaviors.behaviorIds.flatMap((id) => {
            const fixture = byLocalId.get(id);
            if (!fixture) return [];
            return [
              {
                id,
                details: {
                  alias: fixture.alias,
                  displayName: fixture.displayName,
                  metadata: fixture.metadata,
                },
              },
            ];
          }),
        },
      };
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
    if (request.getLayers) {
      // Batch effective/current bindings per requested id (SS7): layer 0 is
      // unedited (effective == default), layer 1 is edited (effective ==
      // default overlaid with its edits). Unknown ids are skipped.
      const effective: Record<number, { name: string; bindings: unknown[] }> = {
        0: { name: "Base", bindings: BASE_DEFAULT_BINDINGS },
        1: { name: "Fn", bindings: FN_EFFECTIVE_BINDINGS },
      };
      return {
        getLayers: {
          layers: request.getLayers.layerIds.flatMap((id) => {
            const l = effective[id];
            return l ? [{ id, name: l.name, bindings: l.bindings }] : [];
          }),
        },
      };
    }
    if (request.getPhysicalLayouts) {
      // Mirrors the firmware: NONE mode never inlines `keys` (SS6) --
      // strip any test-fixture geometry so a bug that regresses to always
      // requesting geometry would surface in the request assertions below,
      // not just be silently masked by this fixture.
      expect(request.getPhysicalLayouts.details).toBe(
        LayoutDetailsMode.LAYOUT_DETAILS_NONE
      );
      return {
        getPhysicalLayouts: {
          activeLayoutIndex: layoutSummaries.activeLayoutIndex,
          layouts: layoutSummaries.layouts.map((l) => ({
            name: l.name,
            fp: l.fp,
            keys: [],
            defaultWidth: 0,
            defaultHeight: 0,
          })),
        },
      };
    }
    if (request.getPhysicalLayout) {
      const index = request.getPhysicalLayout.layoutIndex;
      const summary = layoutSummaries.layouts[index];
      if (!summary) {
        throw new Error(`unexpected get_physical_layout(${index})`);
      }
      const geometry = LAYOUT_GEOMETRY_BY_FP[summary.fp];
      if (!geometry) {
        throw new Error(`no geometry fixture for fp ${summary.fp}`);
      }
      return {
        getPhysicalLayout: {
          index,
          layout: {
            name: geometry.name,
            fp: summary.fp,
            keys: geometry.keys,
            defaultWidth: 0,
            defaultHeight: 0,
          },
        },
      };
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

    // get_snapshot, list_behaviors(CUSTOM, first connect -> empty cache),
    // (kp resolves from bundle, my_custom is inlined by CUSTOM mode -- no
    // get_behaviors needed), a single batched get_layers([0,1]) for both
    // layers' effective bindings (no cached defaults to diff against),
    // get_physical_layouts(NONE) + get_physical_layout for the active (only)
    // layout, whose geometry isn't cached yet.
    expect(requestKinds(requests)).toEqual([
      "getSnapshot",
      "listBehaviors",
      "getLayers",
      "getPhysicalLayouts",
      "getPhysicalLayout",
    ]);

    expect(model.behaviors).toHaveLength(2);
    const kp = model.behaviors.find((b) => b.localId === 1);
    const custom = model.behaviors.find((b) => b.localId === 2);
    expect(kp?.resolvedFrom).toBe("bundle");
    expect(kp?.alias).toBe("kp");
    expect(custom?.resolvedFrom).toBe("device");
    expect(custom?.metadata).toEqual(CUSTOM_METADATA);

    expect(model.layers).toHaveLength(2);
    const base = model.layers.find((l) => l.id === 0)!;
    const fn = model.layers.find((l) => l.id === 1)!;
    expect(base.bindings).toEqual(BASE_DEFAULT_BINDINGS);
    expect(base.loadedFromCache).toBe(false);
    expect(fn.bindings).toEqual(FN_EFFECTIVE_BINDINGS);
    expect(fn.loadedFromCache).toBe(false);

    expect(model.layouts).toEqual([
      { name: "Default", fp: DEFAULT_LAYOUT_FP, keys: DEFAULT_LAYOUT_KEYS },
    ]);
    expect(model.activeLayoutIndex).toBe(0);

    expect(model.stats).toEqual({
      behaviorsFromCache: false,
      behaviorsBundled: 1,
      behaviorsCached: 0,
      behaviorsFetched: 1,
      layersFromCache: 0,
      layersFetchedFull: 2,
      layersFetchedDiff: 0,
      physicalLayoutsFromCache: false,
    });
  });

  it("behaviors_fp cache hit: skips list_behaviors and every get_behaviors round-trip", async () => {
    localStorage.setItem(
      FastKeymapCacheKeys.behaviors(DEVICE_KEY),
      JSON.stringify({
        fp: SNAPSHOT.behaviorsFp,
        behaviors: [
          {
            localId: 1,
            alias: "kp",
            displayName: "Key Press",
            defFp: KP_BEHAVIOR.defFp,
            metadata: KP_METADATA,
          },
          {
            localId: 2,
            alias: "my_custom",
            displayName: "My Custom",
            defFp: CUSTOM_BEHAVIOR.defFp,
            metadata: CUSTOM_METADATA,
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
    expect(requests.some((r) => r.listBehaviors)).toBe(false);
    expect(requests.some((r) => r.getBehaviors)).toBe(false);

    expect(model.stats).toEqual({
      behaviorsFromCache: true,
      behaviorsBundled: 0,
      behaviorsCached: 0,
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

  it("cold cache, all bundled-standard: list_behaviors(CUSTOM) with no inline details, resolved from bundle, zero get_behaviors calls", async () => {
    const { call, requests } = makeDeviceCall([KP_BEHAVIOR]);

    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    const listRequest = requests.find((r) => r.listBehaviors);
    expect(listRequest?.listBehaviors?.details).toBe(
      BehaviorDetailsMode.BEHAVIOR_DETAILS_CUSTOM
    );
    expect(requests.some((r) => r.getBehaviors)).toBe(false);

    expect(model.behaviors).toHaveLength(1);
    expect(model.behaviors[0].resolvedFrom).toBe("bundle");
    expect(model.behaviors[0].alias).toBe("kp");
    expect(model.stats.behaviorsBundled).toBe(1);
    expect(model.stats.behaviorsFetched).toBe(0);
  });

  it("cold cache with a custom behavior: its details are inlined by CUSTOM mode, resolved as device, no batch needed", async () => {
    const { call, requests } = makeDeviceCall([KP_BEHAVIOR, CUSTOM_BEHAVIOR]);

    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    expect(requests.some((r) => r.getBehaviors)).toBe(false);

    const custom = model.behaviors.find((b) => b.localId === 2);
    expect(custom?.resolvedFrom).toBe("device");
    expect(custom?.alias).toBe("my_custom");
    expect(custom?.metadata).toEqual(CUSTOM_METADATA);
    expect(model.stats.behaviorsFetched).toBe(1);
  });

  it("a def_fp miss (neither inlined nor bundled) is resolved by exactly one get_behaviors batch", async () => {
    const { call, requests } = makeDeviceCall([KP_BEHAVIOR, MISS_BEHAVIOR]);

    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    const batchRequests = requests.filter((r) => r.getBehaviors);
    expect(batchRequests).toHaveLength(1);
    expect(batchRequests[0].getBehaviors?.behaviorIds).toEqual([3]);

    const mystery = model.behaviors.find((b) => b.localId === 3);
    expect(mystery?.resolvedFrom).toBe("device");
    expect(mystery?.alias).toBe("mystery");
    expect(mystery?.metadata).toEqual(MISS_METADATA);
    expect(model.stats.behaviorsFetched).toBe(1);
  });

  it("definition_version mismatch: requests list_behaviors(ALL)", async () => {
    const mismatchedSnapshot = makeSnapshot({
      definitionVersion: FK_DEFINITION_VERSION + 1,
    });
    const { call, requests } = makeDeviceCall(
      [KP_BEHAVIOR, CUSTOM_BEHAVIOR],
      mismatchedSnapshot
    );

    const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

    const listRequest = requests.find((r) => r.listBehaviors);
    expect(listRequest?.listBehaviors?.details).toBe(
      BehaviorDetailsMode.BEHAVIOR_DETAILS_ALL
    );
    // Every behavior arrives inlined -- no bundle trust, no batch needed.
    expect(requests.some((r) => r.getBehaviors)).toBe(false);
    expect(model.behaviors.every((b) => b.resolvedFrom === "device")).toBe(
      true
    );
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

  describe("physical layouts (slim summary + on-demand geometry, DESIGN.md SS6)", () => {
    it("cold path: get_physical_layouts(NONE) has no keys, only the active layout's geometry is fetched, never a non-active one", async () => {
      const { call, requests } = makeDeviceCall(
        [KP_BEHAVIOR, CUSTOM_BEHAVIOR],
        SNAPSHOT,
        LAYOUT_SUMMARIES
      );

      const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

      const listRequests = requests.filter((r) => r.getPhysicalLayouts);
      expect(listRequests).toHaveLength(1);
      expect(listRequests[0].getPhysicalLayouts?.details).toBe(
        LayoutDetailsMode.LAYOUT_DETAILS_NONE
      );

      const geometryRequests = requests.filter((r) => r.getPhysicalLayout);
      expect(geometryRequests).toHaveLength(1);
      expect(geometryRequests[0].getPhysicalLayout?.layoutIndex).toBe(
        LAYOUT_SUMMARIES.activeLayoutIndex
      );

      // The inactive ("Split", index 1) layout must NOT have had its
      // geometry fetched.
      expect(
        geometryRequests.some((r) => r.getPhysicalLayout?.layoutIndex === 1)
      ).toBe(false);

      expect(model.layouts).toEqual([
        { name: "Default", fp: DEFAULT_LAYOUT_FP, keys: DEFAULT_LAYOUT_KEYS },
        { name: "Split", fp: SPLIT_LAYOUT_FP, keys: undefined },
      ]);
      expect(model.activeLayoutIndex).toBe(0);
    });

    it("per-layout geometry cache: a cached fp for the active layout skips get_physical_layout entirely", async () => {
      localStorage.setItem(
        FastKeymapCacheKeys.layoutGeometry(DEVICE_KEY, DEFAULT_LAYOUT_FP),
        JSON.stringify({
          name: "Default",
          fp: DEFAULT_LAYOUT_FP,
          keys: DEFAULT_LAYOUT_KEYS,
        })
      );

      const { call, requests } = makeDeviceCall(
        [KP_BEHAVIOR, CUSTOM_BEHAVIOR],
        SNAPSHOT,
        LAYOUT_SUMMARIES
      );

      const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

      expect(requests.some((r) => r.getPhysicalLayout)).toBe(false);
      expect(
        model.layouts.find((l) => l.fp === DEFAULT_LAYOUT_FP)?.keys
      ).toEqual(DEFAULT_LAYOUT_KEYS);
    });

    it("whole-set cache hit: physical_layout_fp match issues zero layout RPCs at all", async () => {
      localStorage.setItem(
        FastKeymapCacheKeys.layouts(DEVICE_KEY),
        JSON.stringify({
          fp: SNAPSHOT.physicalLayoutFp,
          activeLayoutIndex: 0,
          layouts: [
            {
              name: "Default",
              fp: DEFAULT_LAYOUT_FP,
              keys: DEFAULT_LAYOUT_KEYS,
            },
            { name: "Split", fp: SPLIT_LAYOUT_FP, keys: undefined },
          ],
        })
      );

      const { call, requests } = makeDeviceCall(
        [KP_BEHAVIOR, CUSTOM_BEHAVIOR],
        SNAPSHOT,
        LAYOUT_SUMMARIES
      );

      const model = await loadFastKeymap(call, { deviceKey: DEVICE_KEY });

      expect(requests.some((r) => r.getPhysicalLayouts)).toBe(false);
      expect(requests.some((r) => r.getPhysicalLayout)).toBe(false);
      expect(model.stats.physicalLayoutsFromCache).toBe(true);
      expect(model.layouts).toEqual([
        { name: "Default", fp: DEFAULT_LAYOUT_FP, keys: DEFAULT_LAYOUT_KEYS },
        { name: "Split", fp: SPLIT_LAYOUT_FP, keys: undefined },
      ]);
    });
  });
});

describe("loadPhysicalLayoutGeometry", () => {
  it("fetches a non-active layout's geometry with one get_physical_layout call and caches it by fp", async () => {
    const { call, requests } = makeDeviceCall(
      [KP_BEHAVIOR, CUSTOM_BEHAVIOR],
      SNAPSHOT,
      LAYOUT_SUMMARIES
    );

    const keys = await loadPhysicalLayoutGeometry(call, 1, {
      deviceKey: DEVICE_KEY,
    });

    const geometryRequests = requests.filter((r) => r.getPhysicalLayout);
    expect(geometryRequests).toHaveLength(1);
    expect(geometryRequests[0].getPhysicalLayout?.layoutIndex).toBe(1);
    expect(keys).toEqual(SPLIT_LAYOUT_KEYS);

    const cached = localStorage.getItem(
      FastKeymapCacheKeys.layoutGeometry(DEVICE_KEY, SPLIT_LAYOUT_FP)
    );
    expect(cached).not.toBeNull();
    expect(JSON.parse(cached!)).toEqual({
      name: "Split",
      fp: SPLIT_LAYOUT_FP,
      keys: SPLIT_LAYOUT_KEYS,
    });
  });

  it("restores width/height hoisted to default_width/default_height (SS6)", async () => {
    // A device that hoisted the common 100x100 size: keys matching it carry
    // width/height 0, non-matching keys carry their own (a 200-wide key).
    const call: FastKeymapCall = async (request) => {
      if (request.getPhysicalLayout) {
        return {
          getPhysicalLayout: {
            index: 0,
            layout: {
              name: "Hoisted",
              fp: 4242,
              defaultWidth: 100,
              defaultHeight: 100,
              keys: [
                { width: 0, height: 0, x: 0, y: 0, r: 0, rx: 0, ry: 0 },
                { width: 200, height: 0, x: 100, y: 0, r: 0, rx: 0, ry: 0 },
              ],
            },
          },
        };
      }
      throw new Error(`unexpected request: ${JSON.stringify(request)}`);
    };

    const keys = await loadPhysicalLayoutGeometry(call, 0, {
      deviceKey: DEVICE_KEY,
    });

    expect(keys).toEqual([
      { width: 100, height: 100, x: 0, y: 0, r: 0, rx: 0, ry: 0 },
      { width: 200, height: 100, x: 100, y: 0, r: 0, rx: 0, ry: 0 },
    ]);
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
