import {
  STANDARD_BEHAVIORS,
  bundledByDefFp,
  crc32Update,
  defFp,
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
    // seed=0; crc(u8 num_sets=0); crc(str16 "a"); crc(str16 "b")
    expect(defFp("a", "b", [])).toBe(0x77d4f6fe);
  });

  it("matches an independently computed golden vector (kp-shaped metadata)", () => {
    // 1 set: param1=[HID_USAGE "Key" keyboardMax=255 consumerMax=4095], param2=[]
    // then str16 "kp", then str16 "Key Press".
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

describe("bundledByDefFp", () => {
  it("resolves a known standard behavior (kp) by its computed def_fp", () => {
    const kp = STANDARD_BEHAVIORS.kp;
    const fp = defFp(kp.alias, kp.displayName, kp.metadata);
    expect(bundledByDefFp(fp)).toEqual(kp);
  });

  it("misses (returns undefined) for a def_fp that doesn't match any bundled behavior", () => {
    const unknownFp = defFp("some_custom_user_behavior", "Custom", []);
    expect(bundledByDefFp(unknownFp)).toBeUndefined();
  });

  it("resolves every bundled entry by its own def_fp", () => {
    for (const def of Object.values(STANDARD_BEHAVIORS)) {
      const fp = defFp(def.alias, def.displayName, def.metadata);
      expect(bundledByDefFp(fp)).toEqual(def);
    }
  });
});
