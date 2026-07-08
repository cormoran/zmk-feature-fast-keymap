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

### Why (the three speed levers + extensions)

Over BLE the official flow is slow because it always transfers the full
behavior metadata and the full effective keymap, with per-binding protobuf
submessage overhead, every connect. We attack that with:

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
4. **Optimized byte packing** — bindings travel as fixed-width packed bytes in
   one `bytes` field, not one protobuf submessage per binding.

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

## 3. Fingerprints (cache keys)

Compute with `crc32_ieee_update` (`zephyr/sys/crc.h`, seed 0) over a **canonical
little-endian byte serialization** of each group. Fingerprints are computed on
demand (cheap: a few hundred bindings = microseconds); no persistence.

Canonicalization must be **identical in firmware and in the web bundle** for
the groups the web reproduces locally (behaviors_fp and per-behavior
metadata_fp). Freeze the exact byte layout in this doc and bump
`FK_DEFINITION_VERSION` on any change.

Groups:

- **`behaviors_fp`** — over all behaviors in ascending `local_id` order; for
  each: `u16 local_id`, `u16 metadata_fp_low`… no — simpler: append
  `u16 local_id` then the behavior's canonical metadata bytes (see
  metadata_fp input) then the alias bytes and display_name bytes. This makes
  `behaviors_fp` change if any behavior, its metadata, alias, or name changes.
- **`metadata_fp`** (per behavior) — over the packed metadata blob defined in
  §5 (the exact bytes `get_behavior_metadata` returns). The web bundle stores
  each standard behavior's expected `metadata_fp`; a match means "use my
  bundled copy."
- **`default_keymap_fp`** — over every layer (index order), each layer:
  `u8 layer_id`, then for each storage position the packed default binding
  (§4). Changes only on reflash.
- **layer `fp` / `default_fp`** (per layer, in `LayerInfo`) — same packing as
  above but for a single layer's current vs default bindings, so the client
  can fetch only changed layers.
- **`keymap_fp`** — over all layers' current bindings + layer names + active
  physical layout index. A single "did anything in the keymap change" key.
- **`physical_layout_fp`** — over the packed layouts blob (§6) + active index.

## 4. Binding packing (the wire format for keymap data)

Fixed-width little-endian, no protobuf per-binding overhead:

- **Packed binding** = 10 bytes: `u16 behavior_local_id`, `u32 param1`,
  `u32 param2`. An empty/`&none` binding encodes `local_id = 0xFFFF`
  (sentinel) — mirror the official encoder, which emits `behavior_id = 0`/
  zeros for a missing binding; pick one sentinel and document it. Recommended:
  use the real local id (`zmk_behavior_get_local_id(binding->behavior_dev)`)
  and `0xFFFF` only when `behavior_dev == NULL`.
- **Edited entry** = 12 bytes: `u16 binding_idx` + packed binding. Used only by
  `get_edited_bindings`; `binding_idx` is the same index space as
  `zmk_keymap_get_layer_binding_at_idx` (so the client applies each edit
  directly onto its cached default layer). A position is "edited" when the
  current packed binding differs from the default packed binding at that
  `binding_idx`.

Chunk `bytes` fields carry an integer number of entries; `max_size` = **240**
(24 bindings or 20 edited entries per chunk). Client reassembles by `offset`.

## 5. Behavior metadata packing

`get_behavior_metadata` returns a packed blob (in a `bytes` field, chunkable)
describing param metadata. Define a compact TLV, e.g.:

```
u8  num_sets
repeat num_sets:
  u8 num_param1_values ; u8 num_param2_values
  repeat (param1 then param2) values:
    u8  value_type      ; 0=nil 1=constant 2=range 3=hid_usage 4=layer_id
    u32 a               ; constant value | range.min | hid keyboard_max
    u32 b               ; 0 | range.max | hid consumer_max
    u8  name_len ; name bytes (display_name, may be empty)
```

The web bundle carries the same standard behaviors decoded from this exact
format, so `metadata_fp` matches. Keep names ≤ some cap; total blob chunked at
240 bytes. (Standard behaviors are normally resolved from the bundle and never
transferred; only custom behaviors hit this path.)

## 6. Physical layouts

Secondary (needed for geometry-aware export). Mirror the official
`get_physical_layouts` but pack into a `bytes` blob: active index, count, then
per layout: name + key count + per key `i16 width,height,x,y,r,rx,ry`. Gate by
`physical_layout_fp`. Chunk at 240 bytes.

## 7. RPC API (proto shape — adjust field numbers/sizes to satisfy nanopb)

One `Request`/`Response` oneof pair. All string/bytes fields need a `.options`
`max_size`. No 64-bit fields. Set `has_<sub>=true` for every submessage
assignment. Response data lives in **static** buffers (encoding runs after the
handler returns). One shared static response buffer sized by the largest
member — keep every message small; the packed `bytes` chunks (240) dominate.

```
Request.oneof:
  bool                        get_snapshot          = 1
  ListBehaviorsRequest{u32 offset}                  = 2  // offset = behavior index
  GetBehaviorMetadataRequest{u32 local_id; u32 offset} = 3
  GetBindingsRequest{u32 layer_id; u32 offset}      = 4  // current/effective, offset in binding units
  GetBindingsRequest (get_default_layer_bindings)   = 5  // DT stock defaults
  GetBindingsRequest (get_edited_bindings)          = 6  // diff current vs default
  GetLayoutsRequest{u32 offset}                     = 7

Response.oneof:
  ErrorResponse{ErrorCode code}                     = 1
  Snapshot                                          = 2
  BehaviorList                                      = 3
  BehaviorMetadata                                  = 4
  BindingsChunk                                     = 5
  LayoutsChunk                                      = 6

Snapshot:
  u32 definition_version, behaviors_fp, default_keymap_fp, keymap_fp,
      physical_layout_fp, behavior_count, max_layer_name_length,
      available_layers, active_layout_index
  repeated LayerInfo layers    // bounded; see note

LayerInfo:
  u32 id; string name(max 16); u32 binding_count; u32 fp; u32 default_fp;
  u32 edited_count

BehaviorList:
  repeated BehaviorSummary behaviors; bool has_more; u32 next_offset
BehaviorSummary:
  u32 local_id; string alias(max 16); string display_name(max 32);
  u32 metadata_fp; bool standard

BehaviorMetadata:
  u32 local_id; bytes data(max 240); u32 offset; u32 total_size; bool has_more

BindingsChunk:
  u32 layer_id; u32 offset; u32 total; bytes data(max 240); bool has_more

LayoutsChunk:
  u32 active_index; u32 layout_count; u32 offset; u32 total_size;
  bytes data(max 240); bool has_more
```

**`Snapshot.layers` sizing:** `LayerInfo` is ~40 bytes. Bound the encoded
Snapshot with a `BUILD_ASSERT` against `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE`.
Most keyboards have ≤16 layers; if a build has more, the assert tells the
integrator to raise the TX buffer. (If this proves limiting, a follow-up can
page the layer list; not needed for v1.)

### Paging protocol

`list_behaviors`, `get_*_bindings`, `get_behavior_metadata`, and
`get_physical_layouts` all page via `offset` + `has_more`/`next_offset`. The
client loops until `!has_more`. Invalid `layer_id`/`local_id`/`offset` →
`ErrorResponse` with the specific `ErrorCode`.

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
- `CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128`.
- `CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE`: pick to fit the largest response
  (≈ 240 chunk + Snapshot layer list + framing). Start **512** and back it
  with a `BUILD_ASSERT` next to the response buffer. Document the RAM cost.
- `FK_DEFINITION_VERSION` C macro = **1**, mirrored as a TS constant in the
  web bundle.

## 9. Phase plan (delegate B/C/D to Sonnet; verify between phases)

- **A — template init** (DONE): placeholders replaced, remotes wired, west
  updated. Baseline `python3 -m unittest` + web tests green before changes.
- **B — firmware + proto + firmware tests.** Proto (§7); handler
  `src/studio/fast_keymap_handler.c` split into helpers
  (`src/fingerprint.c`, `src/binding_pack.c`, `src/behavior_alias.c`); Kconfig;
  buffers. (No `default_keymap.c` — use the ZMK stock-keymap accessor, §2.) native_sim unit tests under
  `tests/` covering: snapshot fields/fingerprint stability, behavior list +
  alias, packed bindings round-trip, edited-diff (edit a binding via
  `zmk_keymap_set_layer_binding_at_idx` in a test hook, assert the diff),
  default vs current. Build tests in `tests/zmk-config/` (add a
  `fast_keymap`-enabled artifact to `build.yaml` + assert in `test.py`).
  Renode functional test: fetch a snapshot end-to-end.
  Gate: `python3 -m unittest` green.
- **C — web UI + web tests.** A fast-load client (`web/src/fastKeymap.ts`):
  snapshot → cache-diff → paged fetch → reassemble. A bundled standard
  definitions module (`web/src/standardBehaviors.ts`, versioned by
  `FK_DEFINITION_VERSION`, keyed by alias + metadata_fp). A view that shows the
  loaded keymap and behaviors and a **`.keymap` export** using aliases. Cache
  in `localStorage` keyed by fingerprints. Tests with
  `createConnectedMockZMKApp`. Gate: `npm test`, `npm run build`, `npm run lint`.
- **D — docs + finalize.** README rewrite (what/why/setup/how the fast load
  works/limitations), `docs/` note on the fingerprint/packing contract and the
  `definition_version` bump procedure. Full `pre-commit`
  (`SKIP=prettier,eslint,jest,web-build` inside nix; run web hooks via npm).
  PR to origin.

## 10. Testing checklist

- native_sim: fingerprints are deterministic across calls; editing a binding
  changes `keymap_fp` and the matching `layer.fp` but NOT `default_keymap_fp`;
  edited-diff returns exactly the edited positions; alias table resolves
  `kp/mo/lt/trans`; packed-binding decode matches
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
