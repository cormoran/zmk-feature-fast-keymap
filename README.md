# zmk-feature-fast-keymap

![ZMK Version](https://img.shields.io/badge/ZMK-master-blue)
[![Test](https://github.com/cormoran/zmk-feature-fast-keymap/actions/workflows/zmk-module.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-fast-keymap/actions/workflows/zmk-module.yml) [![Devcontainer](https://github.com/cormoran/zmk-feature-fast-keymap/actions/workflows/devcontainer.yml/badge.svg?branch=main)](https://github.com/cormoran/zmk-feature-fast-keymap/actions/workflows/devcontainer.yml)

A **read-only** ZMK [Studio](https://zmk.dev/docs/features/studio) module that
serves your keyboard's behavior list and keymap over a custom
([unofficial](https://github.com/cormoran/zmk)) Studio RPC subsystem, built to
**load fast over slow BLE**. It is a cache-friendly companion to ZMK's official
`keymap`/`behaviors` subsystems, plus a couple of export-oriented extras.

It does not change how your keyboard works and it never writes to your keymap —
it only reads it (e.g. to display or to export a `.keymap` file).

## Why it's fast

The official flow re-transfers the full behavior metadata and the full keymap on
every connect. This module instead **sends less**:

1. **Fingerprint cache keys.** `get_snapshot` returns cheap CRC32 fingerprints
   for each data group (all behaviors, each layer's current + default bindings,
   physical layouts). The web client caches by fingerprint and skips re-fetching
   any group whose fingerprint still matches.
2. **Bundled standard behaviors.** Metadata for standard ZMK behaviors rarely
   changes, so the web app ships it, versioned by a definition version. The
   firmware only transfers metadata for behaviors the client can't resolve from
   its bundle.
3. **Default keymap + edited-only diff.** The compiled-in ("stock") keymap only
   changes when you reflash, so the client caches it and then fetches just the
   handful of positions you actually edited (`get_edited_layer`).
4. **Batched cold load.** Over slow BLE the initial load is dominated by
   per-request round-trips, so on a cold cache the client pulls every layer's
   bindings in one `get_layers` batch instead of one request per layer.

The wire format deliberately mimics ZMK's official protobuf messages (no bespoke
byte-packing; the RPC response streams, so there is no app-level chunking). The
speed comes from the caching above, not from re-encoding bytes.

Extras for tooling/export:

- **Behavior aliases** — each behavior reports its keymap alias (e.g. `kp` for
  `&kp`, resolved from the devicetree node label, including your custom
  behaviors), so a client can reconstruct `.keymap` text.
- **Default vs. edited keymap** — read the pristine compiled-in keymap, the
  effective keymap, or only the edited positions.

The bundled web app (behaviors browser, keymap viewer, and a **`.keymap`
exporter**) is published at
`https://cormoran.github.io/zmk-feature-fast-keymap/`.

## Module User Guide

1. Add the module to your `config/west.yml` (uses the standard
   `main+custom-studio-protocol` ZMK branch — no extra patches needed for the
   default feature set).

   ```yml
   manifest:
     remotes:
       - name: cormoran
         url-base: https://github.com/cormoran
     projects:
       - name: zmk-feature-fast-keymap
         remote: cormoran
         revision: main
       # Patched ZMK: the unofficial custom Studio RPC protocol
       - name: zmk
         remote: cormoran
         revision: main+custom-studio-protocol
         import:
           file: app/west.yml
   ```

2. Enable the flags in your `config/<shield>.conf`.

   ```conf
   CONFIG_ZMK_STUDIO=y
   CONFIG_ZMK_FAST_KEYMAP=y
   CONFIG_ZMK_FAST_KEYMAP_STUDIO_RPC=y

   # Recommended: the requests are tiny, but the default RX buffer (30 B) is
   # smaller than the framed custom-subsystem request envelope.
   CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=128
   ```

   `CONFIG_ZMK_FAST_KEYMAP_STUDIO_RPC` is `y` by default once `CONFIG_ZMK_STUDIO`
   and `CONFIG_ZMK_FAST_KEYMAP` are set. Responses stream, so no TX-buffer
   change is required.

   > **Optional — default keymap / edited-only diff.** The
   > `get_default_layer` / `get_edited_layer` methods (which let a client cache
   > the compiled-in default keymap and fetch only edited positions) read the
   > default keymap through ZMK's non-upstream
   > `zmk_stock_keymap_get_layer_binding_at_idx()` accessor. They are **off by
   > default**. To use them, set `CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER=y` **and**
   > point the `zmk` project at a branch that has the accessor —
   > `main+dya` or `pr/main+custom-studio-protocol/expose-stock-keymap`. With
   > the default branch, `get_default_layer` returns the current keymap (no
   > diff) and everything else works normally.

   > **Optional — unsaved (pending) positions.** The `get_pending` method
   > returns, per layer, the positions whose binding has an unsaved,
   > in-memory-only edit (changed since the last save) so a client can flag
   > which keys aren't persisted yet. It reads ZMK core's pending state through
   > the non-upstream `zmk_keymap_layer_binding_at_idx_is_pending()` accessor
   > (cormoran/zmk PR #30). Point the `zmk` project at a branch that has it; on
   > a branch without it the method reports nothing pending.

3. Flash, open the web app above, connect (USB or BLE), and it fast-loads your
   keymap and behaviors. Use "Export .keymap" to copy a `.keymap`-style dump.

### Limitations

- Read-only: editing/saving stays with ZMK's official `keymap` subsystem. This
  module coexists with it.
- `.keymap` export renders binding parameters as raw numbers (e.g. `&kp 4`), not
  keycode names — enough to reconstruct structure, not a pretty-printer.
- Standard-behavior aliases/metadata are authoritative in the web bundle;
  behaviors it doesn't bundle are fetched from the device on demand.

### Reusable client library

The RPC-data-handling and fingerprint/caching logic is published as a
transport-agnostic npm package, **[`zmk-feature-fast-keymap`](./web/packages/fast-keymap)**,
so other web apps can reuse it for consistent caching. It provides the
fingerprint canonicalization, the bundled standard-behavior definitions, and
`loadFastKeymap()` (snapshot → cache-diff → assemble); the app supplies its own
`call(request)` transport. See its [README](./web/packages/fast-keymap/README.md).

The bundled web app also includes a **benchmark** that compares fast-keymap vs.
ZMK's official `keymap`/`behaviors` RPC for initial and cached (2nd) loads.

### Protocol / contributor notes

The fingerprint canonicalization and the definition-version bump procedure (the
firmware↔web contract) are documented in [docs/protocol.md](./docs/protocol.md);
`DESIGN.md` has the full rationale and API table.

### Web UI

See [web/README.md](./web/README.md) for web UI development instructions.

### Publishing Web UI

**GitHub Pages**: Merge a pull request into `main` to deploy to
`https://<account>.github.io/<repo>/`.

**Cloudflare Workers (PR previews)**: Configure `CLOUDFLARE_API_TOKEN` and
`CLOUDFLARE_ACCOUNT_ID` secrets.

## Module Development Guide

### Initialize from template

Right after creating a repository from this template, run the initialization
script and follow the checklist in [AGENTS.md](./AGENTS.md):

```bash
python3 scripts/init_module.py --namespace <your-github-name> --module <feature-name>
```

It replaces every template placeholder (identifiers, paths, URLs, artifact
names) and verifies nothing is left (`--verify-only` re-checks at any time).

### Setup for running test

#### Option0: Dev container (recommended)

Open this repository in VS Code with the [Dev Containers extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-containers). The container automatically initializes the west workspace using the isolated layout.

#### Option1: west workspace directory layout

Set west topdir as parent of repository root and download dependencies under `../`.
This layout is useful to reduce disk usage by sharing dependencies with other zephyr modules.
The build result is located in `../build`.

```bash
mkdir west-workspace
cd west-workspace # this directory becomes west workspace root (topdir)
git clone <this repository>
# rm -r .west # if exists to reset workspace
west init -l . --mf west/west-test-workspace.yml
west update --narrow
west zephyr-export
```

#### Option2: isolated directory layout

Set west topdir as repository root and download dependencies under `./dependencies`.
This layout is useful if you don't want to share dependencies to other zephyr modules.
Dev container and github actions uses this layout.
The build result is located in `./build`.

```bash
git clone <this repository>
cd <cloned directory>
west init -l west --mf west-test-isolated.yml
west update --narrow
west zephyr-export
```

### Pre-commit

Every commit need to pass pre-commit verification. The verification contains formatting code and running tests.

```
pip install pre-commit
pre-commit install

# Run pre-commit manually
pre-commit run --all-files
# Run for git staged files
pre-commit run
```

### Running Test

```bash
# Run unit test + build test and verify the results
python3 -m unittest
# Run build test directly
west zmk-build tests/zmk-config
# Run unit test directly
west zmk-test tests -m .
# Run web tests
cd web && npm test
```

### Hardware-free Renode testing

CI also boots this module's firmware in the [Renode](https://renode.io/)
emulator (no physical board needed) and exercises it functionally: the real
ZMK boot banner, a core Studio RPC `GetDeviceInfo` round trip, and this
module's own custom Studio RPC subsystem. This is a *step* in the `Build`
job in `.github/workflows/zmk-module.yml` (not a separate job) -- it tests
the `renode_smoke_test` artifact `python3 -m unittest -v` already built
(see `tests/zmk-config/build.yaml`) using a reusable action,
[`cormoran/zmk-workspace`'s
`zmk-renode-test`](https://github.com/cormoran/zmk-workspace/tree/main/.github/actions/zmk-renode-test).
**That action does not build firmware** -- this module's own build flow
does, using the `renode-studio-uart` Zephyr snippet that `zmk-workspace`
provides as a west dependency (see
`west/west-dependency/west-test-dependency.yml`); the action only boots the
resulting ELF and runs tests against it.

To reproduce locally (after the usual `west update`, which also fetches
`zmk-workspace` into `dependencies/zmk-workspace`):

```bash
# 1. Build the Renode-testable artifact (Studio-RPC-over-UART overlay + the
#    Renode-only transport that bypasses the USB-gated real one -- real
#    hardware still uses the studio-rpc-usb-uart snippet as normal). This
#    builds every tests/zmk-config/build.yaml artifact; -af filters to just
#    the Renode one by (substring) artifact name.
west zmk-build tests/zmk-config -af renode
# (equivalent to letting the full `python3 -m unittest -v` build sweep run)

# 2. Generic smoke test (boot banner + core Studio RPC).
python3 dependencies/zmk-workspace/skills/test-zmk-renode/scripts/renode_smoke.py \
  --elf build/renode_smoke_test/zephyr/zmk.elf --west-topdir "$PWD"

# 3. This module's own Renode test (custom Studio RPC subsystem). PYTHONPATH
#    is optional -- tests/renode/renode_test.py falls back to
#    dependencies/zmk-workspace/skills/test-zmk-renode/scripts automatically.
ZMK_RENODE_ELF="$PWD/build/renode_smoke_test/zephyr/zmk.elf" \
python3 tests/renode/renode_test.py -v
```

To adapt `tests/renode/renode_test.py` for your own module: it imports
`renode_harness` (from the `zmk-workspace` west dependency, via
`PYTHONPATH` in CI or the automatic fallback locally) for all the
Renode/RPC plumbing, reads the built ELF's path from `ZMK_RENODE_ELF`, and
only needs to know your module's own custom-subsystem identifier and proto
messages -- see the file's own module docstring for how the `zmk.custom`
envelope (subsystem discovery/addressing) works.

### Sync changes from template

Run `Actions > Sync Changes in Template > Run workflow` to get the latest template changes as a pull request.

If the template contains changes in `.github/workflows/*`, register a GitHub personal access token as `GH_TOKEN` repository secret (`repo` + `workflow` scopes).

### Coding agent on actions

Actions for github copilot and claude are available.

- Mention `@copilot`
- Setup `ANTHROPIC_API_KEY` secret and mention `@claude`
  - Or fix [claude.yml](./github/workflows/claude.yml) to use `CLAUDE_CODE_OAUTH_TOKEN`
