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
import type { RpcConnection } from "@zmkfirmware/zmk-studio-ts-client";
import { loadOfficialKeymap, OfficialKeymapError } from "./officialKeymap";

export interface BenchmarkMeasurement {
  ms: number;
  rpcCalls: number;
  /** Total size, in bytes, of the RPC responses received during this load.
   * ≈ response payload bytes: each decoded response is re-encoded (protobuf
   * encoding is deterministic, and both paths omit default/empty fields the
   * same way) and summed -- a faithful, consistent proxy for what actually
   * crossed the wire, excluding RPC framing. Measured the same way on both
   * the fast-keymap path (below) and the official path
   * (officialKeymap.ts's `loadOfficialKeymap`), so the comparison is fair. */
  readBytes: number;
  /** Set when this measurement's load failed -- the benchmark still
   * reports the other measurements rather than aborting entirely. */
  error?: string;
  /** True when `error` is specifically an UNLOCK_REQUIRED MetaError --
   * lets the UI show "unlock to benchmark the official path" instead of a
   * generic error message. */
  unlockRequired?: boolean;
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
}

function errorMessage(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
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
  deviceKey: string
): Promise<BenchmarkMeasurement> {
  let rpcCalls = 0;
  let readBytes = 0;
  const countingCall: FastKeymapCall = async (request) => {
    rpcCalls++;
    const response = await call(request);
    if (response) {
      readBytes += FastKeymapResponse.encode(response).finish().length;
    }
    return response;
  };

  const start = performance.now();
  try {
    await loadFastKeymap(countingCall, { deviceKey, storage });
    return { ms: performance.now() - start, rpcCalls, readBytes };
  } catch (err) {
    return {
      ms: performance.now() - start,
      rpcCalls,
      readBytes,
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
    };
  } catch (err) {
    const ms = performance.now() - start;
    if (err instanceof OfficialKeymapError) {
      return {
        ms,
        rpcCalls: err.rpcCalls,
        readBytes: err.readBytes,
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
  const storage = new MemoryStorage();

  const fastInitial = await measureFastKeymap(
    fastKeymapCall,
    storage,
    deviceKey
  );
  const fastSecond = await measureFastKeymap(
    fastKeymapCall,
    storage,
    deviceKey
  );

  const officialInitial = await measureOfficial(connection);
  const officialSecond = await measureOfficial(connection);

  return {
    fastKeymap: { initial: fastInitial, second: fastSecond },
    official: { initial: officialInitial, second: officialSecond },
  };
}
