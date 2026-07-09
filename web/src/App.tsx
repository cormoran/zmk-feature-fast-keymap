import { useCallback, useContext, useEffect, useState } from "react";
import "./App.css";
import { connect as gattConnect } from "@zmkfirmware/zmk-studio-ts-client/transport/gatt";
import {
  ZMKConnection,
  ZMKAppContext,
  useStudioLockState,
  isUnlockRequiredError,
  isWebSerialSupported,
  isWebBluetoothSupported,
  useCustomSubsystem,
  connectSerial,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  exportKeymapText,
  formatBindingToken,
  indexBehaviorsById,
  loadFastKeymap,
  type FastKeymapBehavior,
  type FastKeymapLayer,
  type FastKeymapModel,
} from "zmk-feature-fast-keymap";

export const SUBSYSTEM_IDENTIFIER = "cormoran__fast_keymap";

// Template placeholder: `scripts/init_module.py` rewrites this literal to
// `{owner}/{repo}`. Never write the full
// `...-with-custom-studio-rpc` repo name in a URL built from this constant --
// the replacement targets this exact string first, which would otherwise
// leave the owner unreplaced.
export const GITHUB_REPO = "cormoran/zmk-fast-keymap";

// Unlike GITHUB_REPO above, this always credits the original template
// project, regardless of which repo this module was forked into. The
// trailing comment is scripts/init_module.py's IGNORE_MARKER: it keeps this
// line from being rewritten (like GITHUB_REPO is) or flagged as a leftover
// placeholder once initialized.
export const TEMPLATE_CREDIT_REPO = "cormoran/zmk-module-template"; // zmk-module-template:keep

function App() {
  return (
    <div className="app">
      <header className="app-header">
        <h1>⚡ zmk-fast-keymap</h1>
        <p>
          Fast, cache-friendly keymap &amp; behavior viewer / .keymap exporter
        </p>
      </header>

      <ZMKConnection
        autoReconnect
        renderDisconnected={({ connect, isLoading, error }) => (
          <section className="card">
            <h2>Device Connection</h2>
            {isLoading && <p>⏳ Connecting...</p>}
            {error && (
              <div className="error-message">
                <p>🚨 {error}</p>
              </div>
            )}
            {!isLoading && (
              <>
                <div className="connect-buttons">
                  {isWebSerialSupported() && (
                    <button
                      className="btn btn-primary"
                      onClick={() => connect(connectSerial)}
                    >
                      🔌 Connect USB
                    </button>
                  )}
                  {isWebBluetoothSupported() && (
                    <button
                      className="btn btn-primary"
                      onClick={() => connect(gattConnect)}
                    >
                      📶 Connect Bluetooth
                    </button>
                  )}
                  {!isWebSerialSupported() && !isWebBluetoothSupported() && (
                    <div className="warning-message">
                      <p>
                        ⚠️ Web Serial and Web Bluetooth are unavailable here.
                        Use a Chromium-based browser (Chrome, Edge, ...) over
                        HTTPS or localhost to connect to your keyboard.
                      </p>
                    </div>
                  )}
                </div>
                {isWebBluetoothSupported() && (
                  <p className="hint-message">
                    📶 Not showing up? Some firmware only advertises the Studio
                    Bluetooth service once unlocked — press the unlock key (
                    <code>&amp;studio_unlock</code> behavior) on your keyboard,
                    then try connecting again.
                  </p>
                )}
              </>
            )}
          </section>
        )}
        renderConnected={({ disconnect, deviceName }) => (
          <>
            <section className="card">
              <h2>Device Connection</h2>
              <div className="device-info">
                <h3>✅ Connected to: {deviceName}</h3>
              </div>
              <button className="btn btn-secondary" onClick={disconnect}>
                Disconnect
              </button>
            </section>

            <FastKeymapSection />
          </>
        )}
      />

      <footer className="app-footer">
        <p>
          <strong>zmk-fast-keymap</strong> — read-only ZMK Studio keymap loader
          &amp; exporter
        </p>
        <p>
          <a
            href={`https://github.com/${GITHUB_REPO}`}
            target="_blank"
            rel="noreferrer"
          >
            {GITHUB_REPO}
          </a>
        </p>
        <p className="template-credit">
          Built from{" "}
          <a
            href={`https://github.com/${TEMPLATE_CREDIT_REPO}`}
            target="_blank"
            rel="noreferrer"
          >
            {TEMPLATE_CREDIT_REPO}
          </a>{" "}
          - AI ready ZMK module template by{" "}
          <a
            href="https://github.com/cormoran"
            target="_blank"
            rel="noreferrer"
          >
            @cormoran
          </a>
        </p>
      </footer>
    </div>
  );
}

/** Renders a `FastKeymapLoadStats` boolean as a small "loaded from cache"
 * vs. "fetched from device" badge -- makes the fast-load caching visible in
 * the UI (DESIGN.md SS9 "C" asks for this explicitly). */
function CacheBadge({ cached, label }: { cached: boolean; label: string }) {
  return (
    <span className={`badge ${cached ? "badge-cache" : "badge-fetch"}`}>
      {cached ? "⚡" : "📡"} {label}
    </span>
  );
}

function BehaviorList({ behaviors }: { behaviors: FastKeymapBehavior[] }) {
  return (
    <ul className="behavior-list">
      {behaviors.map((behavior) => (
        <li key={behavior.localId}>
          <code>&amp;{behavior.alias || `behavior_${behavior.localId}`}</code> —{" "}
          {behavior.displayName || "(unnamed)"}{" "}
          <span className="badge-source">
            {behavior.resolvedFrom === "bundle" && "bundled"}
            {behavior.resolvedFrom === "cache" && "cached"}
            {behavior.resolvedFrom === "device" && "fetched"}
          </span>
        </li>
      ))}
    </ul>
  );
}

function LayerList({
  layers,
  behaviors,
}: {
  layers: FastKeymapLayer[];
  behaviors: FastKeymapBehavior[];
}) {
  const behaviorsById = indexBehaviorsById(behaviors);

  return (
    <div className="layer-list">
      {layers.map((layer) => (
        <div className="layer-block" key={layer.id}>
          <h4>
            {layer.name || `Layer ${layer.id}`}{" "}
            <CacheBadge cached={layer.loadedFromCache} label="keymap" />
          </h4>
          <p className="layer-bindings">
            {layer.bindings
              .map((binding) => formatBindingToken(binding, behaviorsById))
              .join("  ")}
          </p>
        </div>
      ))}
    </div>
  );
}

function ExportPanel({ model }: { model: FastKeymapModel }) {
  const [copyStatus, setCopyStatus] = useState<"idle" | "copied" | "failed">(
    "idle"
  );
  const keymapText = exportKeymapText(model);

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(keymapText);
      setCopyStatus("copied");
    } catch (error) {
      console.error("Failed to copy .keymap text:", error);
      setCopyStatus("failed");
    }
  };

  return (
    <div className="export-panel">
      <h3>Export .keymap</h3>
      <button className="btn btn-secondary" onClick={() => void copy()}>
        📋 Copy .keymap
      </button>
      {copyStatus === "copied" && <span> Copied!</span>}
      {copyStatus === "failed" && (
        <span> Copy failed -- select the text below manually.</span>
      )}
      <textarea
        readOnly
        aria-label=".keymap export"
        className="keymap-export-text"
        value={keymapText}
        rows={12}
      />
    </div>
  );
}

export function FastKeymapSection() {
  const zmkApp = useContext(ZMKAppContext);
  const { ready, subsystem, call } = useCustomSubsystem(SUBSYSTEM_IDENTIFIER, {
    encode: (r: Request) => Request.encode(r).finish(),
    decode: Response.decode,
  });
  const { locked } = useStudioLockState();
  const [model, setModel] = useState<FastKeymapModel | null>(null);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [awaitingUnlock, setAwaitingUnlock] = useState(false);

  // Fast-load caching is keyed per-device (DESIGN.md SS9 "C") -- fall back
  // to a fixed key when the device name isn't known yet so a first load
  // still works (it just may share a cache with other unnamed devices).
  const deviceKey = zmkApp?.state.deviceInfo?.name;

  const load = useCallback(async () => {
    if (!ready) return;

    setIsLoading(true);
    setError(null);

    try {
      const loaded = await loadFastKeymap(call, { deviceKey });
      setAwaitingUnlock(false);
      setModel(loaded);
    } catch (err) {
      if (isUnlockRequiredError(err)) {
        setAwaitingUnlock(true);
      } else {
        console.error("Fast keymap load failed:", err);
        setError(err instanceof Error ? err.message : "Unknown error");
      }
    } finally {
      setIsLoading(false);
    }
  }, [ready, call, deviceKey]);

  // Load automatically once the subsystem is ready -- this is the whole
  // point of the fast-load client: no manual "fetch" step needed. `load()`
  // sets state asynchronously (after its awaits), not synchronously within
  // the effect body itself, but the linter can't see through that --
  // suppress both rules it flags here (same rationale as the unlock-retry
  // effect below).
  useEffect(() => {
    if (ready) {
      // eslint-disable-next-line react-hooks/set-state-in-effect
      void load();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready]);

  // Auto-retry once the device reports it's unlocked again -- covers the
  // common case where the user presses &studio_unlock after seeing the
  // prompt below without needing to click "Retry" themselves. (This
  // subsystem is unsecured by default -- see DESIGN.md SS1 -- but the web
  // template's unlock flow is kept working in case a fork secures it.)
  useEffect(() => {
    if (awaitingUnlock && !locked) {
      // This mirrors an external system (the device's lock state) rather
      // than deriving from props/state, so a direct setState here is
      // intentional -- see react-hooks/set-state-in-effect's rationale (same
      // pattern used by useStudioLockState itself).
      // eslint-disable-next-line react-hooks/set-state-in-effect
      setAwaitingUnlock(false);
      void load();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [locked]);

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the fast-keymap module. See the{" "}
            <a href={`https://github.com/${GITHUB_REPO}#readme`}>
              module README
            </a>{" "}
            for firmware setup.
          </p>
        </div>
      </section>
    );
  }

  return (
    <section className="card">
      <h2>Fast Keymap</h2>

      {locked && (
        <div className="locked-banner">
          <p>🔒 ZMK Studio is locked.</p>
        </div>
      )}

      {isLoading && <p>⏳ Loading keymap...</p>}

      {error && (
        <div className="error-message">
          <p>🚨 {error}</p>
        </div>
      )}

      {awaitingUnlock && (
        <div className="unlock-prompt card">
          <p>
            🔒 ZMK Studio is locked. Press the unlock key (
            <code>&amp;studio_unlock</code> behavior) on your keyboard — the
            request will retry automatically.
          </p>
          <button className="btn btn-secondary" onClick={() => void load()}>
            Retry
          </button>
        </div>
      )}

      <button
        className="btn btn-primary"
        disabled={isLoading || locked}
        onClick={() => void load()}
      >
        🔄 Reload
      </button>

      {model && (
        <>
          <div className="stats-badges">
            <CacheBadge
              cached={model.stats.behaviorsFromCache}
              label={
                model.stats.behaviorsFromCache
                  ? "behaviors"
                  : `behaviors (${model.stats.behaviorsBundled} bundled, ${model.stats.behaviorsFetched} fetched)`
              }
            />
            <CacheBadge
              cached={model.stats.physicalLayoutsFromCache}
              label="physical layouts"
            />
            <span className="badge">
              {model.stats.layersFromCache} / {model.layers.length} layers from
              cache
            </span>
          </div>

          <h3>Behaviors ({model.behaviors.length})</h3>
          <BehaviorList behaviors={model.behaviors} />

          <h3>Layers ({model.layers.length})</h3>
          <LayerList layers={model.layers} behaviors={model.behaviors} />

          <ExportPanel model={model} />
        </>
      )}
    </section>
  );
}

export default App;
