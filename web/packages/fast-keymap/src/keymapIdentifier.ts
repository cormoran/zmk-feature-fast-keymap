import type { FastKeymapCall } from "./fastKeymap";
import { ErrorCode } from "./proto/cormoran/fast-keymap/fast_keymap";

const BASE64URL =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
const IDENTIFIER_PATTERN = /^[A-Za-z0-9_-]{4}$/;

/** Hash the exact UTF-8 source string into the device's four-character ID. */
export async function hashKeymapIdentifier(source: string): Promise<string> {
  const digest = new Uint8Array(
    await crypto.subtle.digest("SHA-256", new TextEncoder().encode(source))
  );
  return (
    BASE64URL[digest[0] >> 2] +
    BASE64URL[((digest[0] & 3) << 4) | (digest[1] >> 4)] +
    BASE64URL[((digest[1] & 15) << 2) | (digest[2] >> 6)] +
    BASE64URL[digest[2] & 63]
  );
}

export async function getKeymapIdentifier(
  call: FastKeymapCall
): Promise<string> {
  const response = await call({ getKeymapIdentifier: true });
  if (!response) throw new Error("keymap identifier RPC returned no response");
  if (response.error) {
    throw new Error(`keymap identifier RPC error: ${response.error.code}`);
  }
  if (!response.keymapIdentifier) {
    throw new Error("keymap identifier RPC returned the wrong response type");
  }
  return response.keymapIdentifier.identifier;
}

/** Empty string clears the ID. Studio must be unlocked to change it. */
export async function setKeymapIdentifier(
  call: FastKeymapCall,
  identifier: string
): Promise<void> {
  if (identifier !== "" && !IDENTIFIER_PATTERN.test(identifier)) {
    throw new Error("keymap identifier must be four Base64url characters");
  }
  const response = await call({ setKeymapIdentifier: { identifier } });
  if (!response) throw new Error("keymap identifier RPC returned no response");
  if (response.error) {
    if (response.error.code === ErrorCode.ERR_UNLOCK_REQUIRED) {
      throw new Error("unlock ZMK Studio before setting the keymap identifier");
    }
    throw new Error(`keymap identifier RPC error: ${response.error.code}`);
  }
  if (!response.setKeymapIdentifier) {
    throw new Error("keymap identifier RPC returned the wrong response type");
  }
}
