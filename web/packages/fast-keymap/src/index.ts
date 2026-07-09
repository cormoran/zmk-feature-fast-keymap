// Public API of the `zmk-feature-fast-keymap` package -- a transport-free
// client for the `cormoran__fast_keymap` custom ZMK Studio RPC subsystem.
// Consumers supply their own `call(request): Promise<Response | null>` (see
// `FastKeymapCall`) -- this package owns the RPC-data-handling, caching, and
// fingerprint-canonicalization logic only, not the transport. See README.md
// for a usage example.

export {
  loadFastKeymap,
  indexBehaviorsById,
  formatBindingToken,
  exportKeymapText,
  FastKeymapCacheKeys,
  type FastKeymapCall,
  type FastKeymapBinding,
  type BehaviorResolvedFrom,
  type FastKeymapBehavior,
  type FastKeymapLayer,
  type FastKeymapLayoutKey,
  type FastKeymapLayout,
  type FastKeymapLoadStats,
  type FastKeymapModel,
  type LoadFastKeymapOptions,
} from "./fastKeymap";

export {
  FK_DEFINITION_VERSION,
  STANDARD_BEHAVIORS,
  bundledMetadataFp,
  type StandardBehaviorDef,
} from "./standardBehaviors";

export { crc32Update, metadataFp } from "./fingerprint";

export type {
  ErrorResponse,
  Snapshot,
  LayerFingerprint,
  ListBehaviorsResponse,
  BehaviorSummary,
  GetBehaviorDetailsRequest,
  GetBehaviorDetailsResponse,
  GetLayerRequest,
  BehaviorBindingParametersSet,
  BehaviorParameterValueDescription,
  BehaviorParameterValueDescriptionRange,
  BehaviorParameterNil,
  BehaviorParameterLayerId,
  BehaviorParameterHidUsage,
  BehaviorBinding,
  Layer,
  EditedBinding,
  EditedLayer,
  PhysicalLayouts,
  PhysicalLayout,
  KeyPhysicalAttrs,
} from "./proto/cormoran/fast-keymap/fast_keymap";

export {
  Request,
  Response,
  ErrorCode,
} from "./proto/cormoran/fast-keymap/fast_keymap";
