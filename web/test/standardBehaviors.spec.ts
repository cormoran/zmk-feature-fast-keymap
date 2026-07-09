import {
  STANDARD_BEHAVIORS,
  bundledByDefFp,
  crc32Update,
  defFp,
  maskDefFp,
} from "zmk-feature-fast-keymap";

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

describe("defFp", () => {
  it("is deterministic for the same inputs", () => {
    const kp = STANDARD_BEHAVIORS.kp;
    expect(defFp(kp.alias, kp.displayName, kp.metadata)).toBe(
      defFp(kp.alias, kp.displayName, kp.metadata)
    );
  });

  it("differs when the alias changes (same metadata/display_name)", () => {
    const a = defFp("kp", "Key Press", []);
    const b = defFp("kt", "Key Press", []);
    expect(a).not.toBe(b);
  });

  it("differs when the display_name changes (same alias/metadata)", () => {
    const a = defFp("kp", "Key Press", []);
    const b = defFp("kp", "Something Else", []);
    expect(a).not.toBe(b);
  });

  it("differs when a metadata value's display name changes", () => {
    const a = defFp("x", "X", [{ param1: [{ name: "Key" }], param2: [] }]);
    const b = defFp("x", "X", [{ param1: [{ name: "Other" }], param2: [] }]);
    expect(a).not.toBe(b);
  });

  it("differs between nil/constant/range/hidUsage/layerId value types", () => {
    const nil = defFp("x", "X", [{ param1: [{ name: "x" }], param2: [] }]);
    const constant = defFp("x", "X", [
      { param1: [{ name: "x", constant: 1 }], param2: [] },
    ]);
    const range = defFp("x", "X", [
      { param1: [{ name: "x", range: { min: 0, max: 1 } }], param2: [] },
    ]);
    const hidUsage = defFp("x", "X", [
      {
        param1: [{ name: "x", hidUsage: { keyboardMax: 1, consumerMax: 2 } }],
        param2: [],
      },
    ]);
    const layerId = defFp("x", "X", [
      { param1: [{ name: "x", layerId: {} }], param2: [] },
    ]);

    const values = [nil, constant, range, hidUsage, layerId];
    expect(new Set(values).size).toBe(values.length);
  });

  // Golden vectors: computed independently (Python's zlib.crc32, chained
  // over the exact SS4/SS5 canonical byte layout) so this test catches a
  // regression in the byte layout itself, not just self-consistency against
  // this file's own implementation.
  it("matches an independently computed golden vector (empty metadata)", () => {
    // seed=0; crc(u8 num_sets=0); crc(str16 "a"); crc(str16 "b"). defFp() is
    // the FULL 32-bit value; the device serves it truncated to
    // Snapshot.def_fp_bits (see maskDefFp tests below).
    expect(defFp("a", "b", [])).toBe(0x77d4f6fe);
  });

  it("matches an independently computed golden vector (kp-shaped metadata)", () => {
    // 1 set: param1=[HID_USAGE "Key" keyboardMax=255 consumerMax=4095], param2=[]
    // then str16 "kp", then str16 "Key Press" (full 32-bit).
    expect(
      defFp("kp", "Key Press", [
        {
          param1: [
            { name: "Key", hidUsage: { keyboardMax: 255, consumerMax: 4095 } },
          ],
          param2: [],
        },
      ])
    ).toBe(0x8f0abb1f);
  });
});

describe("maskDefFp", () => {
  it("masks a full def_fp to 16/24/32-bit widths (and treats 0/absent as 16)", () => {
    const full = 0x8f0abb1f;
    expect(maskDefFp(full, 16)).toBe(0xbb1f);
    expect(maskDefFp(full, 24)).toBe(0x0abb1f);
    expect(maskDefFp(full, 32)).toBe(0x8f0abb1f);
    expect(maskDefFp(full, 0)).toBe(0xbb1f); // 0 -> 16 (legacy firmware)
  });
});

describe("bundledByDefFp", () => {
  it("resolves a known standard behavior (kp) at the device's served width", () => {
    const kp = STANDARD_BEHAVIORS.kp;
    const full = defFp(kp.alias, kp.displayName, kp.metadata);
    // The device serves def_fp masked to Snapshot.def_fp_bits, so the lookup
    // key is the masked value; the bundle is masked to the same width.
    for (const bits of [16, 24, 32]) {
      expect(bundledByDefFp(maskDefFp(full, bits), bits)).toEqual(kp);
    }
  });

  it("misses (returns undefined) for a def_fp that doesn't match any bundled behavior", () => {
    const unknownFull = defFp("some_custom_user_behavior", "Custom", []);
    expect(bundledByDefFp(maskDefFp(unknownFull, 16), 16)).toBeUndefined();
  });

  it("resolves every bundled entry by its own def_fp at each width", () => {
    for (const def of Object.values(STANDARD_BEHAVIORS)) {
      const full = defFp(def.alias, def.displayName, def.metadata);
      for (const bits of [16, 24, 32]) {
        expect(bundledByDefFp(maskDefFp(full, bits), bits)).toEqual(def);
      }
    }
  });
});
