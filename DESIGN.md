# zmk-feature-fast-keymap — Design

A **read-only** ZMK Studio custom-RPC subsystem that serves the behavior list
and keymap data, optimized to load **fast over slow BLE**. It is a faster,
cache-friendly alternative to ZMK's official `behaviors` + `keymap`
subsystems, plus a few export-oriented extensions.

This document is the source of truth for implementation. Implement in the
phase order in §9. Read `skills/zmk-module-design/SKILL.md` and
`skills/zmk-module-dev/SKILL.md` before touching proto/firmware/web/tests.

## 1. Scope

- **Feature**, not a driver. No `dts/`, no `boards/`, no hardware nodes.
- **Read-only.** No keymap editing, no persistence, no
  `zmk-feature-custom-settings` dependency (drop it from this module).
- Subsystem identifier (C token + web string): **`cormoran__fast_keymap`**.
- Security: **`ZMK_STUDIO_RPC_HANDLER_UNSECURED`** (same as the official
  `list_all_behaviors`; reading a keymap is not sensitive and unsecured keeps
  load fast — no unlock round-trip). The web template already handles the
  unlock flow if we ever change this.
- Out of scope: writing bindings, adding/removing/moving layers, saving,
  layer name editing, sensor bindings (v1 ignores `sensor_bindings`).

### Why (three speed levers + extensions)

Over BLE the official flow is slow because it re-transfers the full behavior
metadata and the full effective keymap every connect. We attack that by
**sending less**, not by re-encoding bytes:

1. **Fingerprint cache keys** — the client asks for cheap CRC32 fingerprints
   of each data group first; if a fingerprint matches its local cache it skips
   fetching that group entirely.
2. **Bundled standard definitions** — standard ZMK behaviors' metadata rarely
   changes and is shipped inside the web app, versioned by
   `definition_version`. The firmware only needs to transfer metadata for
   behaviors the client cannot resolve from its bundle.
3. **Default keymap + edited-only diff** — the compiled-in ("stock") keymap
   changes only on reflash, so the client caches it by fingerprint and then
   fetches only the small set of positions the user actually edited.

The wire format itself deliberately **mimics ZMK's official proto** (§3) — no
bespoke byte packing (protobuf already omits zero/default fields) and no
app-level response chunking (the RPC response streams). Simplicity over a
marginal packing win; the wins above dominate.

Extensions requested: expose each behavior's **keymap alias** (`kp` for
`&kp`) for `.keymap` export, and expose the **default keymap** and the
**edited-only diff** as first-class reads.

## 2. Key ZMK facts this design relies on (verified in deps)

- `zmk_keymap_get_layer_binding_at_idx(layer_id, binding_idx)`
  (`zmk/keymap.h`) returns the **current/effective** binding; `binding_idx` is
  a physical-layout index mapped to storage via the selected position map.
- `zmk_physical_layouts_get_selected_to_stock_position_map(&map)`
  (`zmk/physical_layouts.h`, **public**) returns that map and its length. You
  do NOT need to call it directly — both the current and default accessors
  below already apply it internally, so passing the same `binding_idx` to both
  yields aligned bindings for the diff.
- **Default ("stock") keymap accessor.** Use the public
  `zmk_stock_keymap_get_layer_binding_at_idx(layer_id, binding_idx)`
  (`zmk/keymap.h`) — added in the ZMK branch
  `pr/main+custom-studio-protocol/expose-stock-keymap` (the west manifest
  already pins it). It mirrors `zmk_keymap_get_layer_binding_at_idx` but
  returns the compiled-in DT default binding (same position mapping applied),
  so the edited-diff is simply: for each `binding_idx`, compare
  `zmk_keymap_get_layer_binding_at_idx(...)` (current) vs
  `zmk_stock_keymap_get_layer_binding_at_idx(...)` (default). **Do NOT
  reconstruct the stock keymap in-module** — there is no `default_keymap.c`.
  The accessor is guarded by `#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)`
  (declared only then). When that config is off, the keymap is `const` and
  cannot be edited, so treat default == current: `get_default_layer_bindings`
  returns the same bytes as `get_layer_bindings`, `default_keymap_fp ==
  keymap_fp`, and `edited_count`/`get_edited_bindings` are always empty. Guard
  the default/edited code paths with the same `#if` and provide that fallback.
- Behavior identity:
  - `zmk_behavior_local_id_t` is **`uint16`** (packs in 2 bytes).
  - Iterate behaviors at runtime via
    `STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh)` → `beh->local_id`,
    `beh->device`.
  - `zmk_behavior_find_behavior_name_from_local_id(id)` → the device **name**
    (e.g. `"key_press"`, the DT node *full name*, NOT the `kp` label).
  - `struct zmk_behavior_ref` (`drivers/behavior.h`) has `.device` and
    `.metadata.display_name` (e.g. `"Key Press"`); iterate with
    `STRUCT_SECTION_FOREACH(zmk_behavior_ref, ...)` to match a device.
  - Param metadata: `behavior_get_parameter_metadata(device, &desc)` fills
    `struct behavior_parameter_metadata { sets_len; sets[] }`; each set has
    `param1_values[]/param2_values[]` of
    `struct behavior_parameter_value_metadata { type; value/range/...; display_name }`.
    Types: `VALUE` (constant), `RANGE`, `NIL`, `HID_USAGE`, `LAYER_ID`.
    HID usage maxima: `ZMK_HID_KEYBOARD_MAX_USAGE`, `ZMK_HID_CONSUMER_MAX_USAGE`.
  See `dependencies/zmk/app/src/studio/{behavior_subsystem,keymap_subsystem}.c`
  for the canonical encoders to mirror.

### 2.1 Behavior alias (`&kp`) resolution

The `kp` token is the DT **node label**, not the device name. Resolve it at
compile time with `DT_FOREACH_NODELABEL` / `DT_NODELABEL_STRING_ARRAY`
(available in this Zephyr) to build a `device* → primary node label` table,
then match `beh->device` at runtime.

Enumeration is best-effort. Build the alias table over the ZMK built-in
behavior compatibles (list them from
`dependencies/zmk/app/dts/bindings/behaviors/*.yaml`) using
`DT_FOREACH_STATUS_OKAY(<compat>, FN)`, plus children of `DT_PATH(behaviors)`
and `DT_PATH(macros)` to catch user-defined behaviors. For each node, alias =
first entry of `DT_NODELABEL_STRING_ARRAY(node)`. **Fallback** when a device
has no resolvable label: report the device name from
`zmk_behavior_find_behavior_name_from_local_id`. Document that the browser
bundle is the authoritative source of canonical aliases for standard
behaviors; the firmware alias is a convenience/override for custom behaviors.

Keep this in its own file (`src/behavior_alias.c` + small header) so its
verbosity does not pollute the handler, and unit-test it in native_sim
(the test keymap uses `&kp`, `&mo`, `&trans`, etc.).

## 3. Wire format: mimic the official proto; no packing, no chunking

Direction (decided): **do not** hand-pack bindings and **do not** chunk
responses. The savings come from the fingerprints (§4) and from skipping
well-known/default data (bundled standard behaviors, cached default keymap),
not from a bespoke byte layout. protobuf already omits zero/default fields, and
the Studio RPC transport **streams** the response, so a whole keymap in one
message is fine and simpler than app-level paging.

- **Message shapes mimic ZMK's official `zmk.keymap` / `zmk.behaviors`**
  (`BehaviorBinding{sint32 behavior_id; uint32 param1; uint32 param2}`, a
  `Layer` with `repeated BehaviorBinding bindings`, the official
  `BehaviorBindingParametersSet` / `BehaviorParameterValueDescription`
  metadata tree, the official `PhysicalLayouts`). Copy the encoders from
  `dependencies/zmk/app/src/studio/{keymap,behavior}_subsystem.c` almost
  verbatim.
- **Streaming, not buffering.** Encode `repeated`/`string` fields with nanopb
  **callback fields** (`pb_callback_t` + `.funcs.encode`), exactly like the
  official subsystems — NOT the template's plain-struct/`max_count` style.
  Verified why this works and is unbounded: the custom-subsystem payload is
  encoded by `zmk_rpc_custom_subsystem_encode_response_payload`
  (`custom_subsystem.c`) as a size-pass (`pb_get_encoded_size`) + a length
  prefix + `pb_encode` **directly into the transport ostream**, and that
  ostream (`rpc.c` `rpc_tx_buffer_write`) is a ring buffer that **flushes to
  the transport as it fills**. So the response is never materialized whole in
  RAM and is not capped by `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE`.
- **Consequence for handlers:** the encoders run **twice** (size pass + encode
  pass). They must be deterministic and read stable data across both passes.
  Reading the live ZMK keymap/behaviors is safe (RPCs are serialized, so state
  is stable for the call). For the Snapshot fingerprints, compute all fps once
  into a static array in the handler before returning, and have the layer
  callback read that array (don't recompute mid-encode).

## 4. Fingerprints (cache keys) — internal, independent of wire format

Compute with `crc32_ieee_update` (`zephyr/sys/crc.h`, seed 0) over a **canonical
little-endian byte serialization** of each group. This canonical form is an
INTERNAL hashing input — it is not the wire format and never leaves the device
except as the resulting `u32`. Computed on demand (cheap; a few hundred
bindings = microseconds), no persistence.

Canonicalization must be **identical in firmware and in the web bundle** for
the groups the web reproduces locally (`behaviors_fp` and per-behavior
`def_fp`). Freeze the exact byte layout here and bump `FK_DEFINITION_VERSION`
on any change.

Canonical binding bytes (used by the keymap fps): `i32 behavior_local_id`
(LE; `-1` when `behavior_dev == NULL`), `u32 param1` (LE), `u32 param2` (LE).

Canonical **string** bytes: `str16` = `u16 length` (LE) + raw UTF-8 bytes.
Behavior alias/display_name always use the **as-served** strings (the exact
strings the RPC returns: alias = first DT node label, falling back to the
device name when the node has no label; display_name = metadata display name,
falling back to the device name) — so the web can recompute a behavior's fp
from a details response.

Groups:

- **`def_fp`** (per behavior, the identity + validator; §5) — over the
  behavior's canonical metadata bytes (§5) + `str16 alias` + `str16
  display_name` (as-served). Because it covers metadata AND the strings, a
  `def_fp` match proves the client's bundled/cached copy is byte-identical to
  what the device serves, and it uniquely distinguishes behaviors that share
  metadata (`mo`/`tog`/`to`/`sl`). The web keys its standard-behavior bundle
  (and its per-device cache) by `def_fp` — no `alias` needs to ride the wire.
  (Replaces the old per-behavior `metadata_fp`, which could not detect a
  renamed alias and could not identify which behavior a shared metadata hash
  belonged to.)
- **`behaviors_fp`** — over all behaviors in ascending `local_id` order; for
  each append `u16 local_id`, then that behavior's `def_fp`-preimage bytes
  (metadata + `str16 alias` + `str16 display_name`, as-served). Whole-set cache
  key: a match lets the client skip the entire behavior fetch.
- **`default_keymap_fp`** — over every layer (index order): `u8 layer_id` then,
  for each `binding_idx` in `0..binding_count`, the canonical binding bytes of
  `zmk_stock_keymap_get_layer_binding_at_idx(...)`. Changes only on reflash.
- **per-layer `fp` / `default_fp`** (in the Snapshot `LayerFingerprint`) — the
  same, for one layer's current vs default bindings, so the client fetches only
  layers whose `fp` changed.
- **`keymap_fp`** — over all layers' current bindings + layer names + active
  physical-layout index. One "did anything in the keymap change" key.
- **`physical_layout_fp`** — over the canonical layouts bytes (active index,
  then per layout: name + per-key `i16 width,height,x,y,r,rx,ry`).

## 5. Behaviors: slim list + on-demand details (minimize redundant transport)

`list_behaviors` returns one `BehaviorSummary { local_id, def_fp, details }`
per behavior. On the wire the summary is normally just `local_id + def_fp`
(~10 B) — proto3 omits the empty `details` submessage for free. The client
resolves each behavior's alias/display_name/metadata **locally** by `def_fp`
(§4): from its versioned standard-behavior **bundle** or its **per-device
cache**; only genuine misses are transferred. `local_id` is per-device
(SETTINGS_TABLE), so it keys the per-device cache only — the portable identity
is `def_fp`.

`details` is a `BehaviorDetails { display_name, alias, metadata }` where
`metadata` is the **official** tree (`repeated BehaviorBindingParametersSet` of
`BehaviorParameterValueDescription`, callback-encoded — copy the official
encoder). It carries no fingerprint; the client recomputes `def_fp` from the
served fields (guaranteeing both sides agree on the canonicalization).

`list_behaviors` takes a `BehaviorDetailsMode`:

- `NONE` — slim summaries only (warm-ish: the client has a per-device cache).
- `CUSTOM` — inline `details` for **non-standard** behaviors only (the cold
  path; the client fills standard behaviors from its bundle). "Standard" =
  `fk_behavior_is_standard()` (the firmware builtin classification, §2.1).
- `ALL` — inline `details` for everything (bundle untrusted, e.g. a
  `FK_DEFINITION_VERSION` mismatch).

Miss-repair batch: `get_behaviors { repeated behavior_ids }` →
`repeated BehaviorEntry { id, details }` fetches exactly the unresolved
behaviors in **one** round-trip (bundle gaps, Kconfig-variant `def_fp` misses
for `kp`/`mt`/`lt`/`sk`/`bt`, stale caches). The request must fit
`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128`, so `behavior_ids` is a bounded
`max_count` array (≤32) and the client chunks at 32.

The metadata canonical bytes (input to `def_fp`, §4) — freeze, versioned by
`FK_DEFINITION_VERSION`:

```
u8  num_sets
repeat num_sets:
  u8 num_param1_values ; u8 num_param2_values
  repeat (all param1 values, then all param2 values):
    u8  value_type      ; 0=nil 1=constant 2=range 3=hid_usage 4=layer_id
    u32 a               ; constant | range.min | hid keyboard_max   (0 if unused)
    u32 b               ; 0        | range.max | hid consumer_max
    u8  name_len ; name bytes (value display_name, may be empty)
```

The web bundle must cover **all** firmware-"standard" behaviors so `CUSTOM`
actually reaches the device in one round-trip (an unbundled-but-standard
behavior would otherwise need a `get_behaviors` follow-up). Expanding the
bundle is web-only and additive (no version bump).

## 6. Physical layouts

Secondary (geometry for export). Return the **official** `PhysicalLayouts`
message (callback-encoded, copy the official encoder). Gate refetch by
`physical_layout_fp` (§4).

## 7. RPC API (proto — adjust field numbers to satisfy nanopb)

One `Request`/`Response` oneof pair. **No paging: each call returns its whole
result in one streamed response.** Encode `repeated`/`string` fields as nanopb
**callback fields** (leave them WITHOUT `max_size`/`max_count` so nanopb
generates callback types — this is the official-subsystem style and is required
for streaming; see §3). Fixed scalar fields need no options. No 64-bit fields.
For any `has_<sub>` submessage field you assign, set `has_<sub>=true`.

```
Request.oneof:
  bool                              get_snapshot           = 1
  ListBehaviorsRequest              list_behaviors         = 2
  GetBehaviorsRequest               get_behaviors          = 3   // batch details
  GetLayerRequest{u32 layer_id}     get_layer              = 4   // current/effective
  GetLayerRequest{u32 layer_id}     get_default_layer      = 5   // DT stock default
  GetLayerRequest{u32 layer_id}     get_edited_layer       = 6   // diff current vs default
  bool                              get_physical_layouts   = 7

Response.oneof:
  ErrorResponse{ErrorCode code}     error                  = 1
  Snapshot                          snapshot               = 2
  ListBehaviorsResponse             list_behaviors         = 3
  GetBehaviorsResponse              get_behaviors          = 4
  Layer                             get_layer              = 5
  Layer                             get_default_layer      = 6
  EditedLayer                       get_edited_layer       = 7
  PhysicalLayouts                   get_physical_layouts   = 8

Snapshot:
  u32 definition_version, behaviors_fp, default_keymap_fp, keymap_fp,
      physical_layout_fp, behavior_count, max_layer_name_length,
      available_layers, active_layout_index
  repeated LayerFingerprint layers            // callback-encoded (streams)
LayerFingerprint:
  u32 id; string name; u32 binding_count; u32 fp; u32 default_fp; u32 edited_count

enum BehaviorDetailsMode: NONE = 0; CUSTOM = 1; ALL = 2   // §5
ListBehaviorsRequest:
  BehaviorDetailsMode details
ListBehaviorsResponse:
  repeated BehaviorSummary behaviors          // callback-encoded
BehaviorSummary:                              // slim: normally just local_id + def_fp
  u32 local_id; u32 def_fp; BehaviorDetails details   // details present only per mode (§5)

// batch miss-repair (§5)
GetBehaviorsRequest:
  repeated u32 behavior_ids                   // .options max_count:32 (fits RX buf)
GetBehaviorsResponse:
  repeated BehaviorEntry behaviors            // callback-encoded
BehaviorEntry:
  u32 id; BehaviorDetails details

// official details tree factored into a submessage (no fingerprint on the wire)
BehaviorDetails:
  string display_name; string alias;
  repeated BehaviorBindingParametersSet metadata
BehaviorBindingParametersSet / BehaviorParameterValueDescription /
  BehaviorParameterValueDescriptionRange / BehaviorParameterHidUsage /
  BehaviorParameterNil / BehaviorParameterLayerId
    -- copy verbatim from dependencies/.../proto/zmk/behaviors.proto

BehaviorBinding:  sint32 behavior_id; uint32 param1; uint32 param2   // official
Layer:            u32 id; string name; repeated BehaviorBinding bindings
EditedBinding:    u32 position; BehaviorBinding binding
EditedLayer:      u32 id; repeated EditedBinding bindings

// official shape
PhysicalLayouts:  u32 active_layout_index; repeated PhysicalLayout layouts
PhysicalLayout:   string name; repeated KeyPhysicalAttrs keys
KeyPhysicalAttrs: sint32 width,height,x,y,r,rx,ry

ErrorResponse:    ErrorCode code
ErrorCode enum:   ERR_GENERIC=0; ERR_INVALID_LAYER=1; ERR_INVALID_BEHAVIOR=2
```

`behavior_id` in `BehaviorBinding` follows the official convention (the
behavior's local id; `sint32`, and the encoder mirrors the official one for a
missing binding). Invalid `layer_id`/`behavior_id` → `ErrorResponse` with the
specific `ErrorCode`. `get_edited_layer`/`get_default_layer` and the
`default_fp`/`edited_count` fields degrade gracefully when
`CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE` is off (default == current; see §2).

## 8. Config / buffers

- **ZMK branch requirement.** This module calls
  `zmk_stock_keymap_get_layer_binding_at_idx()`, which currently exists only on
  the ZMK branch `pr/main+custom-studio-protocol/expose-stock-keymap` (the test
  west manifest pins it). The consumer setup docs (README, Phase D) must tell
  users to point their `config/west.yml` ZMK project at that branch until it
  merges into `main+custom-studio-protocol`.
- Kconfig: `ZMK_FAST_KEYMAP` (bool, main switch) and
  `ZMK_FAST_KEYMAP_STUDIO_RPC` (default y, `depends on ZMK_STUDIO`). The
  handler compiles only under the latter. Depends only on ZMK core keymap +
  studio — no hardware, so native_sim builds and tests it.
- `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128` (requests are tiny; this is the ZMK
  Studio default-ish bump). Requests must fit RX whole.
- `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE`: this is only the streaming **staging
  ring buffer** (§3) — it does NOT cap response size, so no fit-`BUILD_ASSERT`
  is needed. Leave it at the ZMK default unless throughput profiling says
  otherwise; a modest bump (e.g. 256) means fewer transport flushes. Document
  it as a throughput knob, not a correctness limit.
- `FK_DEFINITION_VERSION` C macro = **1**, mirrored as a TS constant in the
  web bundle.

## 9. Phase plan (delegate B/C/D to Sonnet; verify between phases)

- **A — template init** (DONE): placeholders replaced, remotes wired, west
  updated. Baseline `python3 -m unittest` + web tests green before changes.
- **B — firmware + proto + firmware tests.** Proto (§7, official-style,
  callback-encoded, no chunking); handler `src/studio/fast_keymap_handler.c`
  split into helpers (`src/fingerprint.c`, `src/behavior_alias.c` — NO
  `binding_pack.c`, NO `default_keymap.c`; bindings use the official
  `BehaviorBinding` message and the ZMK stock-keymap accessor §2). native_sim
  unit tests under `tests/` covering: snapshot fields/fingerprint stability,
  behavior list + alias, `get_layer` bindings match
  `zmk_keymap_get_layer_binding_at_idx`, edited-diff (edit a binding via
  `zmk_keymap_set_layer_binding_at_idx` in a test hook, assert the diff),
  default vs current. Build tests in `tests/zmk-config/` (add a
  `fast_keymap`-enabled artifact to `build.yaml` + assert in `test.py`).
  Renode functional test: fetch a snapshot end-to-end.
  Gate: `python3 -m unittest` green.
- **C — web UI + web tests.** A fast-load client (`web/src/fastKeymap.ts`):
  snapshot → cache-diff → fetch only changed layers / unbundled behaviors →
  assemble (default layer from cache + edited-diff applied). A bundled standard
  definitions module (`web/src/standardBehaviors.ts`, versioned by
  `FK_DEFINITION_VERSION`, keyed by alias + metadata_fp). A view that shows the
  loaded keymap and behaviors and a **`.keymap` export** using aliases. Cache
  in `localStorage` keyed by fingerprints. Tests with
  `createConnectedMockZMKApp`. Gate: `npm test`, `npm run build`, `npm run lint`.
- **D — docs + finalize.** README rewrite (what/why/setup incl. the ZMK branch
  requirement/how the fast load works/limitations), `docs/` note on the
  fingerprint canonicalization contract and the `definition_version` bump
  procedure. Full `pre-commit` (`SKIP=prettier,eslint,jest,web-build` inside
  nix; run web hooks via npm). PR to origin.

## 10. Testing checklist

- native_sim: fingerprints are deterministic across calls; editing a binding
  changes `keymap_fp` and the matching `layer.fp` but NOT `default_keymap_fp`;
  edited-diff returns exactly the edited positions; alias table resolves
  `kp/mo/lt/trans`; `get_layer` bindings decode to the same values as
  `zmk_keymap_get_layer_binding_at_idx`.
- build tests: a config with `CONFIG_ZMK_FAST_KEYMAP=y` builds for the RPC
  board; artifact/CONFIG names match between `build.yaml` and `test.py`.
- web: snapshot cache hit skips fetches (assert no extra RPC calls when
  fingerprints match a seeded cache); custom behavior (fp miss) triggers a
  metadata fetch; `.keymap` export string is correct for a known mock keymap.
- Renode: boot + `get_snapshot` round-trip decodes.

## 11. Non-goals / risks

- Keymap **editing** stays with the official subsystem; this module is a fast
  read/export path that coexists with it.
- Alias enumeration is best-effort (see §2.1); the web bundle covers standard
  behaviors regardless.
- `definition_version` is the compatibility contract for §3/§5 canonical
  bytes — never change those layouts without bumping it.
</content>
</invoke>
