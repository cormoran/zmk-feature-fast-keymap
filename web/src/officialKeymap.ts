// Official-RPC keymap loader -- loads behaviors + the effective keymap the
// way ZMK Studio officially does it, via the CORE `behaviors`/`keymap` RPC
// oneofs (not a custom subsystem, and with no client-side cache). This
// exists purely as a comparison baseline for the fast-keymap benchmark (see
// benchmark.ts, DESIGN.md "Why"): it deliberately re-fetches everything on
// every call, which is exactly the behavior fast-keymap's fingerprint cache
// avoids on a repeat load.
//
// This is intentionally transport-coupled (drives a live `RpcConnection`
// directly) and lives in the app, not the zmk-feature-fast-keymap package,
// which stays transport-agnostic.

import {
  call_rpc,
  Response as StudioResponse,
  type Request,
  type RpcConnection,
} from "@zmkfirmware/zmk-studio-ts-client";
import type { BehaviorBindingParametersSet } from "@zmkfirmware/zmk-studio-ts-client/behaviors";
// Type-only -- erased by `verbatimModuleSyntax` at compile time, so this
// doesn't introduce a real runtime circular import even though benchmark.ts
// imports this module's `loadOfficialKeymap`/`OfficialKeymapError` values.
import type { BenchmarkMethodStat } from "./benchmark";

export interface OfficialBehavior {
  id: number;
  displayName: string;
  metadata: BehaviorBindingParametersSet[];
}

export interface OfficialBinding {
  behaviorId: number;
  param1: number;
  param2: number;
}

export interface OfficialLayer {
  id: number;
  name: string;
  bindings: OfficialBinding[];
}

export interface OfficialKeymapResult {
  behaviors: OfficialBehavior[];
  layers: OfficialLayer[];
  /** Number of `call_rpc()` round-trips this load made: always
   * `2 + behaviors.length` (`list_all_behaviors` + `get_keymap` + one
   * `get_behavior_details` per behavior) -- the official flow has no
   * fingerprinting or bundling to let it skip any of them, unlike
   * fast-keymap (see DESIGN.md "Why"). */
  rpcCalls: number;
  /** Total size, in bytes, of the responses read during this load, measured
   * at the OUTER transport level: each decoded `call_rpc()` response re-encoded
   * as the full `zmk.studio.Response` frame it travels in, summed. Measured at
   * the same whole-message layer as the fast-keymap path -- see benchmark.ts's
   * matching comment -- so the comparison is fair. */
  readBytes: number;
  /** Per-RPC-method breakdown of this load's calls -- see
   * `BenchmarkMethodStat` (benchmark.ts) for the shape and why it exists. */
  byMethod: BenchmarkMethodStat[];
}

/**
 * Thrown by `loadOfficialKeymap()` on any failure (most commonly an
 * `UNLOCK_REQUIRED` `MetaError` from the SECURED `get_behavior_details` /
 * `get_keymap` RPCs -- see this file's header comment) so callers --
 * notably benchmark.ts -- still learn how many round-trips completed
 * before the failure, instead of losing that count along with the error.
 */
export class OfficialKeymapError extends Error {
  readonly rpcCalls: number;
  readonly readBytes: number;
  readonly byMethod: BenchmarkMethodStat[];
  readonly cause: unknown;

  constructor(
    rpcCalls: number,
    readBytes: number,
    byMethod: BenchmarkMethodStat[],
    cause: unknown
  ) {
    super(cause instanceof Error ? cause.message : String(cause));
    this.name = "OfficialKeymapError";
    this.rpcCalls = rpcCalls;
    this.readBytes = readBytes;
    this.byMethod = byMethod;
    this.cause = cause;
    Object.setPrototypeOf(this, OfficialKeymapError.prototype);
  }
}

/** Returns the single set key of a oneof-shaped request object -- e.g.
 * `{ behaviors: { listAllBehaviors: true } }`'s outer `"behaviors"` key, or
 * that inner object's own `"listAllBehaviors"` key. `benchmark.ts` has its
 * own copy of this same one-liner (see its header comment) -- kept
 * duplicated rather than shared so the two loaders stay independent
 * modules. */
function firstDefinedKey<T extends object>(obj: T): string | undefined {
  return Object.keys(obj).find(
    (key) => (obj as Record<string, unknown>)[key] !== undefined
  );
}

/** camelCase -> snake_case, e.g. `"getBehaviorDetails"` ->
 * `"get_behavior_details"` -- matches the official proto method names
 * (`zmk.behaviors.get_behavior_details` etc.), unlike the field's own
 * generated camelCase spelling. */
function camelToSnake(name: string): string {
  return name.replace(/[A-Z]/g, (c) => `_${c.toLowerCase()}`);
}

/** Derives a readable, proto-method-shaped label for one official RPC
 * request -- e.g. `{ behaviors: { getBehaviorDetails: {...} } }` ->
 * `"get_behavior_details"`, `{ keymap: { getKeymap: true } }` ->
 * `"get_keymap"` -- for the per-method breakdown (`BenchmarkMethodStat`). */
function officialMethodName(request: Omit<Request, "requestId">): string {
  const outerKey = firstDefinedKey(request);
  if (!outerKey) return "unknown";
  const inner = (request as unknown as Record<string, unknown>)[outerKey];
  if (inner && typeof inner === "object") {
    const innerKey = firstDefinedKey(inner);
    if (innerKey) return camelToSnake(innerKey);
  }
  return camelToSnake(outerKey);
}

/** Records one RPC call's cost against its method's running totals in
 * `byMethod` (creating the entry on first use). */
function recordMethodStat(
  byMethod: Map<string, BenchmarkMethodStat>,
  method: string,
  bytes: number,
  ms: number
): void {
  const existing = byMethod.get(method);
  if (existing) {
    existing.calls++;
    existing.bytes += bytes;
    existing.ms += ms;
  } else {
    byMethod.set(method, { method, calls: 1, bytes, ms });
  }
}

/** Snapshots a per-method breakdown map as an array sorted by descending
 * time -- matches benchmark.ts's own `sortedByMethod()`. */
function sortedByMethod(
  byMethod: Map<string, BenchmarkMethodStat>
): BenchmarkMethodStat[] {
  return [...byMethod.values()].sort((a, b) => b.ms - a.ms);
}

/**
 * Loads the full behavior list + effective keymap the way ZMK Studio
 * officially does it: `behaviors.list_all_behaviors`, then
 * `behaviors.get_behavior_details` per id, then `keymap.get_keymap` -- via
 * the CORE RPC (not a custom subsystem). No client-side cache: every call
 * redoes the full round-trip set, which is the baseline benchmark.ts
 * compares fast-keymap's fingerprint-cached load against.
 *
 * `get_behavior_details` and `get_keymap` are SECURED on stock ZMK Studio
 * firmware -- while the keyboard is locked, `call_rpc()` rejects with a
 * `MetaError` whose `condition` is `ErrorConditions.UNLOCK_REQUIRED` (see
 * `@cormoran/zmk-studio-react-hook`'s `isUnlockRequiredError`).
 * `list_all_behaviors` is unsecured, so a locked device typically fails on
 * the first `get_behavior_details` (or, with zero behaviors, on
 * `get_keymap`). This function doesn't special-case that itself -- it just
 * wraps whatever `call_rpc()` throws in an `OfficialKeymapError` carrying
 * the round-trip count so far; callers should check
 * `isUnlockRequiredError(error.cause)`.
 */
export async function loadOfficialKeymap(
  connection: RpcConnection
): Promise<OfficialKeymapResult> {
  let rpcCalls = 0;
  let readBytes = 0;
  const byMethod = new Map<string, BenchmarkMethodStat>();

  const call = async (request: Omit<Request, "requestId">) => {
    rpcCalls++;
    const method = officialMethodName(request);
    const callStart = performance.now();
    const response = await call_rpc(connection, request);
    const callMs = performance.now() - callStart;
    // Outer count: `call_rpc()` returns the decoded `RequestResponse`; re-wrap
    // it in the full `zmk.studio.Response` frame it travels in, so this matches
    // the fast-keymap path's whole-`zmk.studio.Response` count (benchmark.ts).
    // requestId zeroed (omitted on the wire) to match the fast-keymap side.
    const bytes = StudioResponse.encode({
      requestResponse: { ...response, requestId: 0 },
    }).finish().length;
    readBytes += bytes;
    recordMethodStat(byMethod, method, bytes, callMs);
    return response;
  };

  try {
    const listResponse = await call({
      behaviors: { listAllBehaviors: true },
    });
    const ids = listResponse.behaviors?.listAllBehaviors?.behaviors ?? [];

    const behaviors: OfficialBehavior[] = [];
    for (const id of ids) {
      const detailsResponse = await call({
        behaviors: { getBehaviorDetails: { behaviorId: id } },
      });
      const details = detailsResponse.behaviors?.getBehaviorDetails;
      behaviors.push({
        id: details?.id ?? id,
        displayName: details?.displayName ?? "",
        metadata: details?.metadata ?? [],
      });
    }

    const keymapResponse = await call({ keymap: { getKeymap: true } });
    const layers: OfficialLayer[] = (
      keymapResponse.keymap?.getKeymap?.layers ?? []
    ).map((layer) => ({
      id: layer.id,
      name: layer.name,
      bindings: layer.bindings.map((binding) => ({
        behaviorId: binding.behaviorId,
        param1: binding.param1,
        param2: binding.param2,
      })),
    }));

    return {
      behaviors,
      layers,
      rpcCalls,
      readBytes,
      byMethod: sortedByMethod(byMethod),
    };
  } catch (err) {
    throw new OfficialKeymapError(
      rpcCalls,
      readBytes,
      sortedByMethod(byMethod),
      err
    );
  }
}
