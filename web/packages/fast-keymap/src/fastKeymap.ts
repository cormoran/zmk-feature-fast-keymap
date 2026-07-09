// Fast-load client for the cormoran__fast_keymap custom Studio RPC
// subsystem -- see DESIGN.md SS9 phase C. Loads the behavior list and the
// effective keymap using the three "send less" cache levers the firmware
// exposes (SS4): a `get_snapshot` fingerprint round-trip first, then only
// fetching whatever changed since the last load (cached in localStorage,
// keyed by fingerprint) -- skipping behavior metadata that matches a
// bundled standard definition (standardBehaviors.ts) and preferring the
// default+edited-diff path over a whole-layer fetch.

import {
  ErrorCode,
  type BehaviorBindingParametersSet,
  type EditedBinding,
  type Request,
  type Response,
  type Snapshot,
} from "./proto/cormoran/fast-keymap/fast_keymap";
import {
  FK_DEFINITION_VERSION,
  STANDARD_BEHAVIORS,
  bundledMetadataFp,
} from "./standardBehaviors";

/** Matches `useCustomSubsystem(...).call` -- see web/src/App.tsx. */
export type FastKeymapCall = (request: Request) => Promise<Response | null>;

export interface FastKeymapBinding {
  /** The behavior's local_id, or -1 when the device reports no bound
   * behavior at this position (BehaviorBinding.behavior_id's official
   * "missing binding" convention -- see DESIGN.md SS7). */
  behaviorId: number;
  param1: number;
  param2: number;
}

export type BehaviorResolvedFrom = "bundle" | "device" | "cache";

export interface FastKeymapBehavior {
  localId: number;
  alias: string;
  displayName: string;
  metadataFp: number;
  standard: boolean;
  metadata: BehaviorBindingParametersSet[];
  /** How this behavior's metadata was obtained -- "bundle" and "cache" mean
   * `get_behavior_details` was NOT called for it. Purely informational
   * (drives the "fast load" indicator in the UI). */
  resolvedFrom: BehaviorResolvedFrom;
}

export interface FastKeymapLayer {
  id: number;
  name: string;
  bindingCount: number;
  editedCount: number;
  bindings: FastKeymapBinding[];
  /** True when this layer's whole effective binding list came straight
   * from the localStorage cache (its `fp` matched) -- zero RPCs for this
   * layer. */
  loadedFromCache: boolean;
}

export interface FastKeymapLayoutKey {
  width: number;
  height: number;
  x: number;
  y: number;
  r: number;
  rx: number;
  ry: number;
}

export interface FastKeymapLayout {
  name: string;
  keys: FastKeymapLayoutKey[];
}

export interface FastKeymapLoadStats {
  /** `behaviors_fp` matched the cache: `list_behaviors` was skipped
   * entirely (and so was every `get_behavior_details`). */
  behaviorsFromCache: boolean;
  /** Behaviors resolved from the bundled standard-definitions table
   * (metadata_fp matched) without a `get_behavior_details` round-trip.
   * Only meaningful when `behaviorsFromCache` is false. */
  behaviorsBundled: number;
  /** Behaviors that required a `get_behavior_details` round-trip. */
  behaviorsFetched: number;
  /** Layers whose whole effective binding list came from the cache (`fp`
   * match, zero RPCs). */
  layersFromCache: number;
  /** Layers fetched whole via `get_layer` (no edits vs. default, or no
   * usable default cache -- see loadLayers()). */
  layersFetchedFull: number;
  /** Layers assembled from a (possibly cached) default layer plus a
   * `get_edited_layer` diff. */
  layersFetchedDiff: number;
  /** `physical_layout_fp` matched the cache: `get_physical_layouts` was
   * skipped entirely. */
  physicalLayoutsFromCache: boolean;
}

export interface FastKeymapModel {
  definitionVersion: number;
  behaviors: FastKeymapBehavior[];
  layers: FastKeymapLayer[];
  layouts: FastKeymapLayout[];
  activeLayoutIndex: number;
  stats: FastKeymapLoadStats;
}

export interface LoadFastKeymapOptions {
  /** Stable cache-key prefix for this device -- pass the connected device
   * name when available (DESIGN.md SS9: "cache in localStorage keyed by a
   * stable device key"). Defaults to a fixed key, which is fine for a
   * single-device workflow but will share a cache across different
   * physical devices. */
  deviceKey?: string;
  /** Defaults to `window.localStorage`. Overridable for tests / for
   * environments without a global localStorage. */
  storage?: Storage;
}

function defaultStorage(): Storage | undefined {
  return typeof localStorage !== "undefined" ? localStorage : undefined;
}

/** localStorage key builders -- the single source of truth for the cache
 * key format, shared by loadFastKeymap() and (for cache-seeding/inspection)
 * tests. Keyed by fingerprint, as described in DESIGN.md SS9 "C". */
export const FastKeymapCacheKeys = {
  prefix(deviceKey: string): string {
    return `fastKeymap:v${FK_DEFINITION_VERSION}:${deviceKey}`;
  },
  behaviors(deviceKey: string): string {
    return `${FastKeymapCacheKeys.prefix(deviceKey)}:behaviors`;
  },
  layer(deviceKey: string, fp: number): string {
    return `${FastKeymapCacheKeys.prefix(deviceKey)}:layer:${fp}`;
  },
  defaultLayer(deviceKey: string, defaultFp: number): string {
    return `${FastKeymapCacheKeys.prefix(deviceKey)}:defaultLayer:${defaultFp}`;
  },
  layouts(deviceKey: string): string {
    return `${FastKeymapCacheKeys.prefix(deviceKey)}:layouts`;
  },
};

/**
 * Loads the full behavior list + effective keymap from a connected device
 * as fast as the firmware's caching levers allow, per DESIGN.md SS9 "C".
 */
export async function loadFastKeymap(
  call: FastKeymapCall,
  options: LoadFastKeymapOptions = {}
): Promise<FastKeymapModel> {
  const storage = options.storage ?? defaultStorage();
  const deviceKey = options.deviceKey ?? "default-device";

  const snapshotResponse = await callChecked(call, { getSnapshot: true });
  const snapshot = snapshotResponse.snapshot;
  if (!snapshot) {
    throw new Error(
      "get_snapshot: device returned a response with no snapshot"
    );
  }

  // Only trust the bundle when the firmware's canonicalization contract
  // version matches ours -- see standardBehaviors.ts's FK_DEFINITION_VERSION
  // doc comment. A mismatch just means every behavior gets fetched.
  const bundleTrusted = snapshot.definitionVersion === FK_DEFINITION_VERSION;

  const behaviorsResult = await loadBehaviors(
    call,
    storage,
    deviceKey,
    snapshot,
    bundleTrusted
  );
  const layersResult = await loadLayers(call, storage, deviceKey, snapshot);
  const layoutsResult = await loadPhysicalLayouts(
    call,
    storage,
    deviceKey,
    snapshot
  );

  return {
    definitionVersion: snapshot.definitionVersion,
    behaviors: behaviorsResult.behaviors,
    layers: layersResult.layers,
    layouts: layoutsResult.layouts,
    activeLayoutIndex: layoutsResult.activeLayoutIndex,
    stats: {
      behaviorsFromCache: behaviorsResult.fromCache,
      behaviorsBundled: behaviorsResult.bundledCount,
      behaviorsFetched: behaviorsResult.fetchedCount,
      layersFromCache: layersResult.fromCacheCount,
      layersFetchedFull: layersResult.fetchedFullCount,
      layersFetchedDiff: layersResult.fetchedDiffCount,
      physicalLayoutsFromCache: layoutsResult.fromCache,
    },
  };
}

// `ErrorCode` is generated as an `as const` object (buf.gen.yaml's
// `enumsAsLiterals=true` -- ts-proto's default `export enum` output isn't
// erasable syntax, which tsconfig.app.json's `erasableSyntaxOnly` forbids),
// so it has no built-in reverse (number -> name) mapping like a real TS
// enum would. Build one for error messages.
const ERROR_CODE_NAMES: Record<number, string> = Object.fromEntries(
  Object.entries(ErrorCode).map(([name, code]) => [code, name])
);

async function callChecked(
  call: FastKeymapCall,
  request: Request
): Promise<Response> {
  const response = await call(request);
  if (!response) {
    throw new Error("fast_keymap RPC: device sent no response payload");
  }
  if (response.error) {
    const name =
      ERROR_CODE_NAMES[response.error.code] ?? String(response.error.code);
    throw new Error(`fast_keymap RPC error: ${name}`);
  }
  return response;
}

// ---------------------------------------------------------------------
// localStorage cache helpers -- caching is a pure optimization, never a
// correctness requirement, so any storage failure (quota, private
// browsing, no `Storage` at all) just falls back to "not cached".
// ---------------------------------------------------------------------

function readCache<T>(
  storage: Storage | undefined,
  key: string
): T | undefined {
  if (!storage) return undefined;
  try {
    const raw = storage.getItem(key);
    if (!raw) return undefined;
    return JSON.parse(raw) as T;
  } catch {
    return undefined;
  }
}

function writeCache(
  storage: Storage | undefined,
  key: string,
  value: unknown
): void {
  if (!storage) return;
  try {
    storage.setItem(key, JSON.stringify(value));
  } catch {
    // Ignore -- see file header.
  }
}

// -- behaviors ------------------------------------------------------------

interface SerializedBehavior {
  localId: number;
  alias: string;
  displayName: string;
  metadataFp: number;
  standard: boolean;
  metadata: BehaviorBindingParametersSet[];
}

function serializeBehavior(behavior: FastKeymapBehavior): SerializedBehavior {
  return {
    localId: behavior.localId,
    alias: behavior.alias,
    displayName: behavior.displayName,
    metadataFp: behavior.metadataFp,
    standard: behavior.standard,
    metadata: behavior.metadata,
  };
}

async function loadBehaviors(
  call: FastKeymapCall,
  storage: Storage | undefined,
  deviceKey: string,
  snapshot: Snapshot,
  bundleTrusted: boolean
): Promise<{
  behaviors: FastKeymapBehavior[];
  fromCache: boolean;
  bundledCount: number;
  fetchedCount: number;
}> {
  const cacheKey = FastKeymapCacheKeys.behaviors(deviceKey);
  const cached = readCache<{ fp: number; behaviors: SerializedBehavior[] }>(
    storage,
    cacheKey
  );

  if (cached && cached.fp === snapshot.behaviorsFp) {
    return {
      behaviors: cached.behaviors.map((b) => ({
        ...b,
        resolvedFrom: "cache" as const,
      })),
      fromCache: true,
      bundledCount: 0,
      fetchedCount: 0,
    };
  }

  const listResponse = await callChecked(call, { listBehaviors: true });
  const summaries = listResponse.listBehaviors?.behaviors ?? [];

  const behaviors: FastKeymapBehavior[] = [];
  let bundledCount = 0;
  let fetchedCount = 0;

  for (const summary of summaries) {
    const bundled = bundleTrusted
      ? STANDARD_BEHAVIORS[summary.alias]
      : undefined;
    const bundledFp = bundled ? bundledMetadataFp(summary.alias) : undefined;

    if (
      bundled &&
      bundledFp !== undefined &&
      bundledFp === summary.metadataFp
    ) {
      bundledCount++;
      behaviors.push({
        localId: summary.localId,
        alias: summary.alias,
        displayName: summary.displayName || bundled.displayName,
        metadataFp: summary.metadataFp,
        standard: summary.standard,
        metadata: bundled.metadata,
        resolvedFrom: "bundle",
      });
      continue;
    }

    fetchedCount++;
    const detailsResponse = await callChecked(call, {
      getBehaviorDetails: { behaviorId: summary.localId },
    });
    const details = detailsResponse.getBehaviorDetails;
    behaviors.push({
      localId: summary.localId,
      alias: details?.alias || summary.alias,
      displayName: details?.displayName || summary.displayName,
      metadataFp: details?.metadataFp ?? summary.metadataFp,
      standard: summary.standard,
      metadata: details?.metadata ?? [],
      resolvedFrom: "device",
    });
  }

  writeCache(storage, cacheKey, {
    fp: snapshot.behaviorsFp,
    behaviors: behaviors.map(serializeBehavior),
  });

  return { behaviors, fromCache: false, bundledCount, fetchedCount };
}

// -- keymap (layers) --------------------------------------------------------

function toFastBindings(
  bindings: { behaviorId: number; param1: number; param2: number }[]
): FastKeymapBinding[] {
  return bindings.map((b) => ({
    behaviorId: b.behaviorId,
    param1: b.param1,
    param2: b.param2,
  }));
}

function overlayEdits(
  defaultBindings: FastKeymapBinding[],
  edits: EditedBinding[]
): FastKeymapBinding[] {
  const result = defaultBindings.map((b) => ({ ...b }));
  for (const edit of edits) {
    if (!edit.binding) continue;
    if (edit.position < 0 || edit.position >= result.length) continue;
    result[edit.position] = {
      behaviorId: edit.binding.behaviorId,
      param1: edit.binding.param1,
      param2: edit.binding.param2,
    };
  }
  return result;
}

async function loadLayers(
  call: FastKeymapCall,
  storage: Storage | undefined,
  deviceKey: string,
  snapshot: Snapshot
): Promise<{
  layers: FastKeymapLayer[];
  fromCacheCount: number;
  fetchedFullCount: number;
  fetchedDiffCount: number;
}> {
  const layers: FastKeymapLayer[] = [];
  let fromCacheCount = 0;
  let fetchedFullCount = 0;
  let fetchedDiffCount = 0;

  for (const layerFingerprint of snapshot.layers) {
    const layerKey = FastKeymapCacheKeys.layer(deviceKey, layerFingerprint.fp);
    const defaultKey = FastKeymapCacheKeys.defaultLayer(
      deviceKey,
      layerFingerprint.defaultFp
    );

    const cachedCurrent = readCache<FastKeymapBinding[]>(storage, layerKey);
    if (cachedCurrent) {
      fromCacheCount++;
      layers.push({
        id: layerFingerprint.id,
        name: layerFingerprint.name,
        bindingCount: layerFingerprint.bindingCount,
        editedCount: layerFingerprint.editedCount,
        bindings: cachedCurrent,
        loadedFromCache: true,
      });
      continue;
    }

    let bindings: FastKeymapBinding[];

    if (layerFingerprint.fp === layerFingerprint.defaultFp) {
      // Current == default content (no edits on this layer): a cached
      // default (possibly written while loading a different layer that
      // shares the same default_fp) can serve directly; otherwise a single
      // get_layer call is cheaper than get_default_layer + get_edited_layer.
      const cachedDefault = readCache<FastKeymapBinding[]>(storage, defaultKey);
      if (cachedDefault) {
        bindings = cachedDefault;
      } else {
        const response = await callChecked(call, {
          getLayer: { layerId: layerFingerprint.id },
        });
        bindings = toFastBindings(response.getLayer?.bindings ?? []);
        writeCache(storage, defaultKey, bindings);
        fetchedFullCount++;
      }
    } else {
      let defaultBindings = readCache<FastKeymapBinding[]>(storage, defaultKey);
      if (!defaultBindings) {
        const response = await callChecked(call, {
          getDefaultLayer: { layerId: layerFingerprint.id },
        });
        defaultBindings = toFastBindings(
          response.getDefaultLayer?.bindings ?? []
        );
        writeCache(storage, defaultKey, defaultBindings);
      }

      const editedResponse = await callChecked(call, {
        getEditedLayer: { layerId: layerFingerprint.id },
      });
      bindings = overlayEdits(
        defaultBindings,
        editedResponse.getEditedLayer?.bindings ?? []
      );
      fetchedDiffCount++;
    }

    writeCache(storage, layerKey, bindings);
    layers.push({
      id: layerFingerprint.id,
      name: layerFingerprint.name,
      bindingCount: layerFingerprint.bindingCount,
      editedCount: layerFingerprint.editedCount,
      bindings,
      loadedFromCache: false,
    });
  }

  return { layers, fromCacheCount, fetchedFullCount, fetchedDiffCount };
}

// -- physical layouts -------------------------------------------------------

interface SerializedLayouts {
  fp: number;
  activeLayoutIndex: number;
  layouts: FastKeymapLayout[];
}

async function loadPhysicalLayouts(
  call: FastKeymapCall,
  storage: Storage | undefined,
  deviceKey: string,
  snapshot: Snapshot
): Promise<{
  layouts: FastKeymapLayout[];
  activeLayoutIndex: number;
  fromCache: boolean;
}> {
  const cacheKey = FastKeymapCacheKeys.layouts(deviceKey);
  const cached = readCache<SerializedLayouts>(storage, cacheKey);

  if (cached && cached.fp === snapshot.physicalLayoutFp) {
    return {
      layouts: cached.layouts,
      activeLayoutIndex: cached.activeLayoutIndex,
      fromCache: true,
    };
  }

  const response = await callChecked(call, { getPhysicalLayouts: true });
  const data = response.getPhysicalLayouts;
  const layouts: FastKeymapLayout[] = (data?.layouts ?? []).map((l) => ({
    name: l.name,
    keys: l.keys.map((k) => ({
      width: k.width,
      height: k.height,
      x: k.x,
      y: k.y,
      r: k.r,
      rx: k.rx,
      ry: k.ry,
    })),
  }));
  const activeLayoutIndex =
    data?.activeLayoutIndex ?? snapshot.activeLayoutIndex;

  writeCache(storage, cacheKey, {
    fp: snapshot.physicalLayoutFp,
    activeLayoutIndex,
    layouts,
  });

  return { layouts, activeLayoutIndex, fromCache: false };
}

// ---------------------------------------------------------------------
// `.keymap` export (DESIGN.md SS9 "C"). Renders each layer as a devicetree
// `bindings = < ... >;` block using each binding's resolved alias (falling
// back to `behavior_<local_id>` when the behavior can't be resolved at
// all). Params are always rendered as raw numbers -- resolving `&kp`
// numeric usage IDs back to ZMK keycode names (e.g. 30 -> "N1") is out of
// scope for v1.
// ---------------------------------------------------------------------

/** How many of a binding's two params to render, inferred from the
 * resolved behavior's metadata (first parameter set): both param1/param2
 * present -> 2, only param1 -> 1, neither (or metadata unresolved) -> 0.
 * Best-effort -- behaviors with multiple sets of differing arity (e.g.
 * `&bt`) are only checked against their first set. */
function behaviorParamArity(behavior: FastKeymapBehavior | undefined): number {
  const firstSet = behavior?.metadata[0];
  if (!firstSet) return 0;
  if (firstSet.param2.length > 0) return 2;
  if (firstSet.param1.length > 0) return 1;
  return 0;
}

/** Builds a `local_id -> behavior` lookup for `formatBindingToken()` /
 * `exportKeymapText()`; also handy for UI code rendering a single layer. */
export function indexBehaviorsById(
  behaviors: FastKeymapBehavior[]
): Map<number, FastKeymapBehavior> {
  return new Map(behaviors.map((b) => [b.localId, b] as const));
}

/** Renders one binding as a `.keymap`-style token, e.g. `&kp 30` or
 * `&mo 1` -- shared by exportKeymapText() and the UI's per-binding
 * display. See exportKeymapText()'s doc comment for the params/arity
 * caveats. */
export function formatBindingToken(
  binding: FastKeymapBinding,
  behaviorsById: Map<number, FastKeymapBehavior>
): string {
  return renderBindingToken(binding, behaviorsById);
}

function renderBindingToken(
  binding: FastKeymapBinding,
  behaviorsById: Map<number, FastKeymapBehavior>
): string {
  if (binding.behaviorId < 0) {
    return "&none";
  }

  const behavior = behaviorsById.get(binding.behaviorId);
  const alias = behavior?.alias || `behavior_${binding.behaviorId}`;
  const arity = behaviorParamArity(behavior);

  const tokens = [`&${alias}`];
  if (arity >= 1) tokens.push(String(binding.param1));
  if (arity >= 2) tokens.push(String(binding.param2));
  return tokens.join(" ");
}

/** Devicetree node names must start with a lowercase letter/underscore and
 * contain only `[a-z0-9_]` (roughly) -- sanitize a (possibly
 * user-authored) layer name into one, falling back to `layer_<id>` for an
 * empty/unusable name. */
function layerNodeName(name: string, id: number): string {
  const sanitized = name
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_]+/g, "_")
    .replace(/^_+|_+$/g, "");
  if (!sanitized || /^[0-9]/.test(sanitized)) {
    return `layer_${id}${sanitized ? `_${sanitized}` : ""}`;
  }
  return sanitized;
}

/** Renders `model` as `.keymap`-style `keymap { <layer> { bindings = < ... >; }; ... };`
 * text, suitable for pasting into a ZMK keymap file's `keymap` node. */
export function exportKeymapText(model: FastKeymapModel): string {
  const behaviorsById = indexBehaviorsById(model.behaviors);

  const layerBlocks = model.layers.map((layer) => {
    const tokens = layer.bindings.map((binding) =>
      formatBindingToken(binding, behaviorsById)
    );
    const bindingsInner = tokens.length > 0 ? ` ${tokens.join(" ")} ` : " ";
    return `    ${layerNodeName(layer.name, layer.id)} {\n        bindings = <${bindingsInner}>;\n    };`;
  });

  return `keymap {\n    compatible = "zmk,keymap";\n\n${layerBlocks.join("\n\n")}\n};`;
}
