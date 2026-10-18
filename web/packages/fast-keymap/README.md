# zmk-feature-fast-keymap

Transport-free client for the ZMK Studio `cormoran__fast_keymap` custom RPC
subsystem. This package provides:

- Fingerprint (CRC32) canonicalization primitives (`crc32Update`,
  `metadataFp`) that reproduce the firmware's `metadata_fp` /
  `behaviors_fp` / layer-fp calculations byte-for-byte, so a client can tell
  whether cached data is still valid without re-fetching it.
- A bundle of ZMK's standard (built-in) behaviors (`STANDARD_BEHAVIORS`,
  `bundledMetadataFp`) so common behaviors (`&kp`, `&mo`, `&bt`, ...) never
  need a `get_behavior_details` round-trip.
- `loadFastKeymap()`: assembles the full behavior list + effective keymap
  using the firmware's "send less" caching levers (snapshot fingerprint
  round-trip, localStorage cache keyed by fingerprint, default+edited-diff
  layer assembly, bundle-first behavior resolution).
- `exportKeymapText()` / `formatBindingToken()` / `indexBehaviorsById()`:
  render a loaded model back out as `.keymap`-style devicetree text.
- `hashKeymapIdentifier()` / `getKeymapIdentifier()` /
  `setKeymapIdentifier()`: derive a four-character keymap type ID from an
  exact UTF-8 source string and read or persist it over RPC. The setter
  requires ZMK Studio unlock.

This package intentionally does **not** own the RPC transport (WebSerial,
WebBluetooth, or anything else) -- each consuming app supplies its own
`call(request): Promise<Response | null>` implementation, typically by
encoding/decoding the re-exported `Request`/`Response` protobuf messages
over whatever ZMK Studio transport it already has (e.g.
`@cormoran/zmk-studio-react-hook`'s `useCustomSubsystem`).

## Usage

```ts
import {
  loadFastKeymap,
  Request,
  Response,
  exportKeymapText,
} from "zmk-feature-fast-keymap";

// Supplied by your app's transport layer -- must encode/decode the
// `cormoran.fast_keymap.Request`/`Response` protobuf messages over the
// `cormoran__fast_keymap` custom Studio RPC subsystem.
const call = async (request: Request): Promise<Response | null> => {
  const bytes = Request.encode(request).finish();
  const responseBytes = await mySubsystem.call(bytes); // your transport
  return responseBytes ? Response.decode(responseBytes) : null;
};

const model = await loadFastKeymap(call, { deviceKey: "my-keyboard" });
console.log(exportKeymapText(model));
```

See the parent repo's `DESIGN.md` / `docs/protocol.md` for the full RPC and
fingerprint contract this package implements.
