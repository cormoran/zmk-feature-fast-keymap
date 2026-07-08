import {
  STANDARD_BEHAVIORS,
  bundledMetadataFp,
  crc32Update,
  metadataFp,
} from "../src/standardBehaviors";

describe("crc32Update", () => {
  it("matches the standard CRC-32/ISO-HDLC check value for '123456789'", () => {
    // The well-known check value for this polynomial/seed (used to verify
    // basically every CRC32 implementation), see e.g.
    // https://reveng.sourceforge.io/crc-catalogue/17plus.htm#crc.cat.crc-32-iso-hdlc
    const bytes = new TextEncoder().encode("123456789");
    expect(crc32Update(0, bytes)).toBe(0xcbf43926);
  });

  it("is chainable: hashing in two calls equals hashing the concatenation in one", () => {
    const a = new TextEncoder().encode("hello ");
    const b = new TextEncoder().encode("world");
    const ab = new TextEncoder().encode("hello world");

    const chained = crc32Update(crc32Update(0, a), b);
    const single = crc32Update(0, ab);

    expect(chained).toBe(single);
  });
});

describe("metadataFp", () => {
  it("is deterministic for the same metadata", () => {
    const metadata = STANDARD_BEHAVIORS.kp.metadata;
    expect(metadataFp(metadata)).toBe(metadataFp(metadata));
  });

  it("differs when a value's display name changes", () => {
    const a = metadataFp([{ param1: [{ name: "Key" }], param2: [] }]);
    const b = metadataFp([{ param1: [{ name: "Other" }], param2: [] }]);
    expect(a).not.toBe(b);
  });

  it("differs between nil/constant/range/hidUsage/layerId value types", () => {
    const nil = metadataFp([{ param1: [{ name: "x" }], param2: [] }]);
    const constant = metadataFp([
      { param1: [{ name: "x", constant: 1 }], param2: [] },
    ]);
    const range = metadataFp([
      { param1: [{ name: "x", range: { min: 0, max: 1 } }], param2: [] },
    ]);
    const hidUsage = metadataFp([
      {
        param1: [{ name: "x", hidUsage: { keyboardMax: 1, consumerMax: 2 } }],
        param2: [],
      },
    ]);
    const layerId = metadataFp([
      { param1: [{ name: "x", layerId: {} }], param2: [] },
    ]);

    const values = [nil, constant, range, hidUsage, layerId];
    expect(new Set(values).size).toBe(values.length);
  });

  it("returns a fixed fp (single 'u8 num_sets=0' byte hash) for empty metadata", () => {
    expect(metadataFp([])).toBe(crc32Update(0, Uint8Array.of(0)));
  });
});

describe("bundledMetadataFp", () => {
  it("returns a stable fp for a bundled alias", () => {
    const first = bundledMetadataFp("kp");
    const second = bundledMetadataFp("kp");
    expect(first).toBeDefined();
    expect(first).toBe(second);
  });

  it("returns undefined for an alias that isn't bundled", () => {
    expect(bundledMetadataFp("some_custom_user_behavior")).toBeUndefined();
  });

  it("is exactly metadataFp() of the bundled definition", () => {
    // Note: several bundled aliases intentionally share an identical
    // metadata shape (e.g. kp/kt/sk are all "one HID_USAGE param1"), so
    // their metadataFp legitimately collides -- that's harmless because
    // bundle selection is keyed by alias first (STANDARD_BEHAVIORS[alias]),
    // with metadataFp only used to confirm THAT alias's bundled copy still
    // matches the device's.
    for (const alias of Object.keys(STANDARD_BEHAVIORS)) {
      expect(bundledMetadataFp(alias)).toBe(
        metadataFp(STANDARD_BEHAVIORS[alias].metadata)
      );
    }
  });
});
