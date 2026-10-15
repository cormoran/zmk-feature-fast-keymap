/** @jest-environment node */

import {
  getKeymapIdentifier,
  hashKeymapIdentifier,
  setKeymapIdentifier,
  type FastKeymapCall,
} from "zmk-feature-fast-keymap";

test("hashes exact UTF-8 bytes to four Base64url characters", async () => {
  expect(await hashKeymapIdentifier("abc")).toBe("ungW");
  expect(await hashKeymapIdentifier("abyss:abc")).not.toBe("ungW");
});

test("reads, writes, and clears the identifier over RPC", async () => {
  let stored = "";
  const call: FastKeymapCall = async (request) => {
    if (request.getKeymapIdentifier) {
      return { keymapIdentifier: { identifier: stored } };
    }
    if (request.setKeymapIdentifier) {
      stored = request.setKeymapIdentifier.identifier;
      return { setKeymapIdentifier: {} };
    }
    throw new Error("unexpected request");
  };

  expect(await getKeymapIdentifier(call)).toBe("");
  await setKeymapIdentifier(call, "A0-_");
  expect(await getKeymapIdentifier(call)).toBe("A0-_");
  await setKeymapIdentifier(call, "");
  expect(await getKeymapIdentifier(call)).toBe("");
  await expect(setKeymapIdentifier(call, "ab+c")).rejects.toThrow(
    "four Base64url characters"
  );
});
