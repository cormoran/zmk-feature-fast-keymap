// Load-time benchmark: fast-keymap vs. ZMK Studio's official core RPC, for
// an initial load and a second (repeat) load. See DESIGN.md "Why" -- the
// point being demonstrated is that fast-keymap's *second* load is
// near-instant on a fingerprint cache hit, while the official path has no
// client-side cache at all and re-fetches everything every time.
//
// Lives in the app (not the zmk-feature-fast-keymap package) because it
// drives the transport-coupled official loader (officialKeymap.ts).

import {
  loadFastKeymap,
  Response as FastKeymapResponse,
  type FastKeymapCall,
} from "zmk-feature-fast-keymap";
import { isUnlockRequiredError } from "@cormoran/zmk-studio-react-hook";
import {
  Response as StudioResponse,
  type RpcConnection,
} from "@zmkfirmware/zmk-studio-ts-client";
import { loadOfficialKeymap, OfficialKeymapError } from "./officialKeymap";

export interface BenchmarkMeasurement {
  ms: number;
  rpcCalls: number;
  /** Total size, in bytes, of the responses read during this load, measured
   * at the OUTER transport level: the full `zmk.studio.Response` frame each
   * reply travels in. The official path re-encodes the `RequestResponse`
   * `call_rpc()` returns; the fast-keymap path re-wraps its decoded response
   * as the custom-subsystem payload inside the same `zmk.studio.Response`
   * envelope (see measureFastKeymap). Both are therefore counted at the same
   * (whole-message) layer -- a faithful, consistent proxy for what actually
   * crosses the wire, excluding only the low-level SOF/EOF framing. */
  readBytes: number;
  /** Per-RPC-method breakdown of this measurement's calls -- one entry per
   * distinct method name, `calls`/`bytes`/`ms` summing to `rpcCalls`/
   * `readBytes`/`ms` above. Lets the UI's "Per-RPC breakdown" section show
   * not just how many bytes crossed the wire but which calls actually cost
   * the time -- see benchmark.ts's module doc comment for why that matters
   * (fast-keymap's per-RPC cost, not total bytes, is what makes its initial
   * load slower on real hardware). Sorted by descending `ms`. */
  byMethod: BenchmarkMethodStat[];
  /** Set when this measurement's load failed -- the benchmark still
   * reports the other measurements rather than aborting entirely. */
  error?: string;
  /** True when `error` is specifically an UNLOCK_REQUIRED MetaError --
   * lets the UI show "unlock to benchmark the official path" instead of a
   * generic error message. */
  unlockRequired?: boolean;
}

/** One RPC method's aggregated cost within a `BenchmarkMeasurement`: how
 * many times it was called, how many (outer, `zmk.studio.Response`-frame)
 * bytes its responses totaled, and how much wall-clock time those calls
 * took. `method` is a readable, proto-method-shaped label -- the fast-keymap
 * side uses the request oneof's own camelCase field name (e.g.
 * `"getSnapshot"`); the official side uses the proto method's snake_case
 * name (e.g. `"get_behavior_details"`), matching each side's own generated
 * naming convention. */
export interface BenchmarkMethodStat {
  method: string;
  calls: number;
  bytes: number;
  ms: number;
}

export interface BenchmarkRun {
  initial: BenchmarkMeasurement;
  second: BenchmarkMeasurement;
}

export interface BenchmarkResult {
  fastKeymap: BenchmarkRun;
  official: BenchmarkRun;
}

export interface RunBenchmarkOptions {
  /** Cache-key prefix for the fast-keymap runs -- see
   * `LoadFastKeymapOptions.deviceKey`. Only affects the *contents* of the
   * isolated benchmark storage (see below), not the real cache. */
  deviceKey?: string;
  /** The connected fast-keymap custom-subsystem index (from
   * `useCustomSubsystem().subsystem.index`). Used only to reconstruct the
   * outer `zmk.studio.Response` envelope for the byte count; defaults to 0
   * (its almost-always value, and a zero index is omitted on the wire). */
  subsystemIndex?: number;
  /** Fast-keymap field-selection options the benchmark loads WITH, so the UI
   * can compare payload sizes with/without a behavior's alias / display_name
   * (see `LoadFastKeymapOptions`). Both default to `true`, matching a normal
   * `loadFastKeymap()` call. */
  includeAliases?: boolean;
  includeDisplayNames?: boolean;
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

/** Returns the single set key of a oneof-shaped request object -- e.g. a
 * fast-keymap `Request`'s `getSnapshot`/`listBehaviors`/... field -- used to
 * derive each call's RPC method name for the per-method breakdown
 * (`BenchmarkMethodStat`). `officialKeymap.ts` has its own copy of this same
 * one-liner (see its header comment) -- kept duplicated rather than shared
 * so the two loaders stay independent modules. */
function firstDefinedKey<T extends object>(obj: T): string | undefined {
  return Object.keys(obj).find(
    (key) => (obj as Record<string, unknown>)[key] !== undefined
  );
}

/** Records one RPC call's cost against its method's running totals in
 * `byMethod` (creating the entry on first use) -- the per-method breakdown
 * shown in the UI's "Per-RPC breakdown" section. */
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
 * time -- the slowest methods (usually the ones worth investigating, see
 * this file's header comment) come first. */
function sortedByMethod(
  byMethod: Map<string, BenchmarkMethodStat>
): BenchmarkMethodStat[] {
  return [...byMethod.values()].sort((a, b) => b.ms - a.ms);
}

/** Minimal in-memory `Storage` implementation used for the fast-keymap
 * runs instead of the real `localStorage`. Two benefits: (1) the "initial"
 * run always starts from a guaranteed-empty cache without needing to
 * enumerate `FastKeymapCacheKeys`' dynamic per-fingerprint keys, and (2)
 * running a benchmark never disturbs the user's actual warm cache -- the
 * one `FastKeymapSection` relies on for its own fast loads. */
class MemoryStorage implements Storage {
  private store = new Map<string, string>();

  get length(): number {
    return this.store.size;
  }

  clear(): void {
    this.store.clear();
  }

  getItem(key: string): string | null {
    return this.store.has(key) ? this.store.get(key)! : null;
  }

  key(index: number): string | null {
    return Array.from(this.store.keys())[index] ?? null;
  }

  removeItem(key: string): void {
    this.store.delete(key);
  }

  setItem(key: string, value: string): void {
    this.store.set(key, value);
  }
}

async function measureFastKeymap(
  call: FastKeymapCall,
  storage: Storage,
  deviceKey: string,
  subsystemIndex: number,
  loadOptions: { includeAliases: boolean; includeDisplayNames: boolean }
): Promise<BenchmarkMeasurement> {
  let rpcCalls = 0;
  let readBytes = 0;
  const byMethod = new Map<string, BenchmarkMethodStat>();
  const countingCall: FastKeymapCall = async (request) => {
    rpcCalls++;
    // The oneof's own set field IS the method name (e.g. "getSnapshot",
    // "listBehaviors") -- see fast_keymap.proto's `Request`.
    const method = firstDefinedKey(request) ?? "unknown";
    const callStart = performance.now();
    const response = await call(request);
    const callMs = performance.now() - callStart;
    let bytes = 0;
    if (response) {
      // Outer count: the fast-keymap payload rides inside a custom-subsystem
      // CallResponse inside a zmk.studio.Response -- re-wrap it the same way
      // so this matches the official path's whole-`zmk.studio.Response` count.
      const payload = FastKeymapResponse.encode(response).finish();
      bytes = StudioResponse.encode({
        // requestId 0 (omitted on the wire) here and on the official side, so
        // the per-call request-id framing doesn't skew the comparison.
        requestResponse: {
          requestId: 0,
          custom: { call: { subsystemIndex, payload } },
        },
      }).finish().length;
      readBytes += bytes;
    }
    recordMethodStat(byMethod, method, bytes, callMs);
    return response;
  };

  const start = performance.now();
  try {
    await loadFastKeymap(countingCall, {
      deviceKey,
      storage,
      includeAliases: loadOptions.includeAliases,
      includeDisplayNames: loadOptions.includeDisplayNames,
    });
    return {
      ms: performance.now() - start,
      rpcCalls,
      readBytes,
      byMethod: sortedByMethod(byMethod),
    };
  } catch (err) {
    return {
      ms: performance.now() - start,
      rpcCalls,
      readBytes,
      byMethod: sortedByMethod(byMethod),
      error: errorMessage(err),
      unlockRequired: isUnlockRequiredError(err),
    };
  }
}

async function measureOfficial(
  connection: RpcConnection
): Promise<BenchmarkMeasurement> {
  const start = performance.now();
  try {
    const result = await loadOfficialKeymap(connection);
    return {
      ms: performance.now() - start,
      rpcCalls: result.rpcCalls,
      readBytes: result.readBytes,
      byMethod: result.byMethod,
    };
  } catch (err) {
    const ms = performance.now() - start;
    if (err instanceof OfficialKeymapError) {
      return {
        ms,
        rpcCalls: err.rpcCalls,
        readBytes: err.readBytes,
        byMethod: err.byMethod,
        error: errorMessage(err.cause),
        unlockRequired: isUnlockRequiredError(err.cause),
      };
    }
    // Shouldn't happen -- loadOfficialKeymap() always wraps failures in
    // OfficialKeymapError -- but don't let an unexpected throw shape abort
    // the rest of the benchmark either.
    return {
      ms,
      rpcCalls: 0,
      readBytes: 0,
      byMethod: [],
      error: errorMessage(err),
      unlockRequired: isUnlockRequiredError(err),
    };
  }
}

/**
 * Runs the fast-keymap-vs-official load-time benchmark: fast-keymap
 * initial (cold cache) + second (warm cache) load, then the official core
 * RPC path's initial + second load (it has no client cache, so both are
 * full loads -- that contrast with fast-keymap's near-instant second load
 * is the whole point, see DESIGN.md "Why").
 *
 * Robust to any individual measurement failing (most commonly: the
 * official path needs Studio to be unlocked) -- each measurement is caught
 * independently, so a failure surfaces as that measurement's
 * `error`/`unlockRequired` fields rather than aborting the whole benchmark
 * or throwing.
 */
export async function runBenchmark(
  fastKeymapCall: FastKeymapCall,
  connection: RpcConnection,
  options: RunBenchmarkOptions = {}
): Promise<BenchmarkResult> {
  const deviceKey = options.deviceKey ?? "benchmark-device";
  const subsystemIndex = options.subsystemIndex ?? 0;
  const loadOptions = {
    includeAliases: options.includeAliases ?? true,
    includeDisplayNames: options.includeDisplayNames ?? true,
  };
  const storage = new MemoryStorage();

  const fastInitial = await measureFastKeymap(
    fastKeymapCall,
    storage,
    deviceKey,
    subsystemIndex,
    loadOptions
  );
  const fastSecond = await measureFastKeymap(
    fastKeymapCall,
    storage,
    deviceKey,
    subsystemIndex,
    loadOptions
  );

  const officialInitial = await measureOfficial(connection);
  const officialSecond = await measureOfficial(connection);

  return {
    fastKeymap: { initial: fastInitial, second: fastSecond },
    official: { initial: officialInitial, second: officialSecond },
  };
}
