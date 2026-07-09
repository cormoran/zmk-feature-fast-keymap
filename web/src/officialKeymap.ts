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
  readonly cause: unknown;

  constructor(rpcCalls: number, readBytes: number, cause: unknown) {
    super(cause instanceof Error ? cause.message : String(cause));
    this.name = "OfficialKeymapError";
    this.rpcCalls = rpcCalls;
    this.readBytes = readBytes;
    this.cause = cause;
    Object.setPrototypeOf(this, OfficialKeymapError.prototype);
  }
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

  const call = async (request: Omit<Request, "requestId">) => {
    rpcCalls++;
    const response = await call_rpc(connection, request);
    // Outer count: `call_rpc()` returns the decoded `RequestResponse`; re-wrap
    // it in the full `zmk.studio.Response` frame it travels in, so this matches
    // the fast-keymap path's whole-`zmk.studio.Response` count (benchmark.ts).
    // requestId zeroed (omitted on the wire) to match the fast-keymap side.
    readBytes += StudioResponse.encode({
      requestResponse: { ...response, requestId: 0 },
    }).finish().length;
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

    return { behaviors, layers, rpcCalls, readBytes };
  } catch (err) {
    throw new OfficialKeymapError(rpcCalls, readBytes, err);
  }
}
