/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <cormoran/fast-keymap/fast_keymap.pb.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/physical_layouts.h>
#include <zmk/studio/rpc.h>

#include "../fingerprint.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_STUDIO_RPC_TEST)

/*
 * Drives the registered cormoran__fast_keymap RPC handler directly (no
 * transport), the same pattern used by zmk-module-devtool's
 * src/test/devtool_rpc_test.c / zmk-feature-watchdog's
 * src/test/watchdog_rpc_test.c: look up the handler in the
 * zmk_rpc_custom_subsystem iterable section and call it with an encoded
 * Request, exercising the exact function Studio's RPC dispatch would call.
 *
 * Needs CONFIG_ZMK_STUDIO (a physical-layout devicetree, see tests/studio/
 * via ../test.dtsi). The edited-diff test additionally needs
 * CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER=y (tests/studio/native_sim.conf sets
 * it), which is what gates the non-upstream
 * zmk_stock_keymap_get_layer_binding_at_idx() accessor; without it that test
 * is compiled out (see test_edited_diff_and_default).
 *
 * tests/studio/native_sim.keymap defines two layers over the 4-key physical
 * layout from ../test.dtsi:
 *   layer 0 "default_layer": kp A, kp B, mo 1, trans
 *   layer 1 "layer_1":       kp C, kp D, trans, trans
 */

static custom_subsystem_handler *find_fast_keymap_rpc_handler(void) {
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);

    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "cormoran__fast_keymap") == 0) {
            return subsys->handler;
        }
    }
    return NULL;
}

struct call_response_payload_capture {
    uint8_t buf[CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE + 512];
    size_t size;
};

static bool decode_call_response_payload(pb_istream_t *stream, const pb_field_t *field,
                                         void **arg) {
    ARG_UNUSED(field);
    struct call_response_payload_capture *capture = *arg;

    if (stream->bytes_left > sizeof(capture->buf)) {
        LOG_ERR("CallResponse payload too large for test capture buffer: %u",
                (unsigned int)stream->bytes_left);
        return false;
    }

    capture->size = stream->bytes_left;
    return pb_read(stream, capture->buf, capture->size);
}

/*
 * `out` must already be zero-initialized by the caller, with any callback
 * fields it wants populated (repeated/string fields decode as CALLBACK --
 * see DESIGN.md SS3/SS7 -- so fields nobody wires a decode callback into are
 * silently skipped by nanopb rather than crashing).
 */
static bool call_fast_keymap_rpc(const cormoran_fast_keymap_Request *req,
                                 cormoran_fast_keymap_Response *out) {
    custom_subsystem_handler *handler = find_fast_keymap_rpc_handler();
    if (!handler) {
        LOG_ERR("fast_keymap RPC subsystem not registered");
        return false;
    }

    static zmk_custom_CallRequest raw_request;
    raw_request = (zmk_custom_CallRequest){0};
    pb_ostream_t req_stream =
        pb_ostream_from_buffer(raw_request.payload.bytes, sizeof(raw_request.payload.bytes));
    if (!pb_encode(&req_stream, cormoran_fast_keymap_Request_fields, req)) {
        LOG_ERR("Failed to encode fast_keymap request: %s", PB_GET_ERROR(&req_stream));
        return false;
    }
    raw_request.payload.size = req_stream.bytes_written;

    zmk_custom_CallResponse response = zmk_custom_CallResponse_init_zero;
    bool ok = handler(&raw_request, &response.payload);
    if (!ok || !response.payload.funcs.encode) {
        LOG_ERR("fast_keymap RPC handler did not produce a response encoder");
        return false;
    }

    static uint8_t call_resp_buf[CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE + 512];
    pb_ostream_t call_resp_stream = pb_ostream_from_buffer(call_resp_buf, sizeof(call_resp_buf));
    if (!pb_encode(&call_resp_stream, zmk_custom_CallResponse_fields, &response)) {
        LOG_ERR("Failed to encode CallResponse: %s", PB_GET_ERROR(&call_resp_stream));
        return false;
    }

    static struct call_response_payload_capture capture;
    capture = (struct call_response_payload_capture){0};

    zmk_custom_CallResponse decoded_call_resp = zmk_custom_CallResponse_init_zero;
    decoded_call_resp.payload.funcs.decode = decode_call_response_payload;
    decoded_call_resp.payload.arg = &capture;

    pb_istream_t call_resp_istream =
        pb_istream_from_buffer(call_resp_buf, call_resp_stream.bytes_written);
    if (!pb_decode(&call_resp_istream, zmk_custom_CallResponse_fields, &decoded_call_resp)) {
        LOG_ERR("Failed to decode CallResponse: %s", PB_GET_ERROR(&call_resp_istream));
        return false;
    }

    /* NOINIT: plain pb_decode() starts by resetting the WHOLE dest_struct to
     * defaults (pb_message_set_to_defaults()), which would wipe out any
     * which_response_type/callback funcs the caller pre-populated on `out`
     * before this call -- see this function's doc comment. With NOINIT that
     * initial reset is skipped, and nanopb's oneof handling only resets a
     * *specific* oneof arm if the incoming wire tag differs from the
     * pre-set which_field, so a pre-configured callback on the arm that
     * actually matches the wire data survives intact. */
    pb_istream_t resp_istream = pb_istream_from_buffer(capture.buf, capture.size);
    if (!pb_decode_noinit(&resp_istream, cormoran_fast_keymap_Response_fields, out)) {
        LOG_ERR("Failed to decode fast_keymap response: %s", PB_GET_ERROR(&resp_istream));
        return false;
    }

    return true;
}

/* -- small reusable decode helpers for callback (repeated/string) fields -- */

struct str_capture {
    char buf[48];
};

static bool decode_str_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    struct str_capture *cap = *arg;
    size_t len = stream->bytes_left;
    if (len >= sizeof(cap->buf)) {
        len = sizeof(cap->buf) - 1;
    }
    if (!pb_read(stream, (uint8_t *)cap->buf, len)) {
        return false;
    }
    cap->buf[len] = '\0';
    return true;
}

#define CAPTURE_ARRAY(type, cap)                                                                   \
    struct {                                                                                       \
        type entries[cap];                                                                         \
        size_t count;                                                                              \
    }

struct layer_fp_entry {
    uint32_t id;
    uint32_t binding_count;
    uint32_t fp;
    uint32_t default_fp;
    uint32_t edited_count;
};
typedef CAPTURE_ARRAY(struct layer_fp_entry, ZMK_KEYMAP_LAYERS_LEN) layer_fp_capture_t;

static bool decode_layer_fp_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    layer_fp_capture_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many layers in snapshot for test capture");
        return false;
    }

    cormoran_fast_keymap_LayerFingerprint tmp = cormoran_fast_keymap_LayerFingerprint_init_zero;
    /* name left uncallbacked on purpose -- nanopb skips it, we don't need it here */
    if (!pb_decode(stream, cormoran_fast_keymap_LayerFingerprint_fields, &tmp)) {
        return false;
    }

    struct layer_fp_entry *e = &captured->entries[captured->count++];
    e->id = tmp.id;
    e->binding_count = tmp.binding_count;
    e->fp = tmp.fp;
    e->default_fp = tmp.default_fp;
    e->edited_count = tmp.edited_count;
    return true;
}

static const struct layer_fp_entry *find_layer_fp(const layer_fp_capture_t *cap, uint32_t id) {
    for (size_t i = 0; i < cap->count; i++) {
        if (cap->entries[i].id == id) {
            return &cap->entries[i];
        }
    }
    return NULL;
}

typedef CAPTURE_ARRAY(cormoran_fast_keymap_BehaviorBinding, ZMK_KEYMAP_LEN) binding_capture_t;

static bool decode_binding_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    binding_capture_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many bindings for test capture");
        return false;
    }

    cormoran_fast_keymap_BehaviorBinding tmp = cormoran_fast_keymap_BehaviorBinding_init_zero;
    if (!pb_decode(stream, cormoran_fast_keymap_BehaviorBinding_fields, &tmp)) {
        return false;
    }
    captured->entries[captured->count++] = tmp;
    return true;
}

/* One entry per Layer in a GetLayersResponse: its id plus its decoded
 * bindings (each Layer submessage decoded with decode_binding_cb into its own
 * capture). */
struct layers_entry {
    uint32_t id;
    binding_capture_t bindings;
};
typedef CAPTURE_ARRAY(struct layers_entry, ZMK_KEYMAP_LAYERS_LEN) layers_capture_t;

static bool decode_layers_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    layers_capture_t *cap = *arg;
    if (cap->count >= ARRAY_SIZE(cap->entries)) {
        LOG_ERR("too many layers in get_layers response for test capture");
        return false;
    }

    struct layers_entry *e = &cap->entries[cap->count];
    e->bindings = (binding_capture_t){0};

    cormoran_fast_keymap_Layer lyr = cormoran_fast_keymap_Layer_init_zero;
    lyr.bindings.funcs.decode = decode_binding_cb;
    lyr.bindings.arg = &e->bindings;
    if (!pb_decode(stream, cormoran_fast_keymap_Layer_fields, &lyr)) {
        return false;
    }

    e->id = lyr.id;
    cap->count++;
    return true;
}

struct edited_entry {
    uint32_t position;
    cormoran_fast_keymap_BehaviorBinding binding;
};
typedef CAPTURE_ARRAY(struct edited_entry, ZMK_KEYMAP_LEN) edited_capture_t;

static bool decode_edited_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    edited_capture_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many edited bindings for test capture");
        return false;
    }

    cormoran_fast_keymap_EditedBinding tmp = cormoran_fast_keymap_EditedBinding_init_zero;
    if (!pb_decode(stream, cormoran_fast_keymap_EditedBinding_fields, &tmp)) {
        return false;
    }
    struct edited_entry *e = &captured->entries[captured->count++];
    e->position = tmp.position;
    e->binding = tmp.binding;
    return true;
}

/* -- BehaviorDetails decode helpers (alias/display_name/metadata) --
 *
 * Small bounded captures (this module's test keymap only has a handful of
 * distinct behavior devices) used to decode a BehaviorDetails submessage
 * wherever it's nested: BehaviorSummary.details (list_behaviors) and
 * BehaviorEntry.details (get_behaviors).
 */

struct value_desc_capture {
    char name[32];
    uint8_t type; /* 0=nil 1=constant 2=range 3=hid_usage 4=layer_id, DESIGN.md SS5 */
    uint32_t a;
    uint32_t b;
};
typedef CAPTURE_ARRAY(struct value_desc_capture, 4) value_desc_capture_t;

static bool decode_value_desc_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    value_desc_capture_t *cap = *arg;
    if (cap->count >= ARRAY_SIZE(cap->entries)) {
        LOG_ERR("too many parameter values for test capture");
        return false;
    }

    cormoran_fast_keymap_BehaviorParameterValueDescription tmp =
        cormoran_fast_keymap_BehaviorParameterValueDescription_init_zero;
    struct str_capture name_cap = {0};
    tmp.name.funcs.decode = decode_str_cb;
    tmp.name.arg = &name_cap;

    if (!pb_decode(stream, cormoran_fast_keymap_BehaviorParameterValueDescription_fields, &tmp)) {
        return false;
    }

    struct value_desc_capture *e = &cap->entries[cap->count++];
    strncpy(e->name, name_cap.buf, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';

    switch (tmp.which_value_type) {
    case cormoran_fast_keymap_BehaviorParameterValueDescription_nil_tag:
        e->type = 0;
        e->a = 0;
        e->b = 0;
        break;
    case cormoran_fast_keymap_BehaviorParameterValueDescription_constant_tag:
        e->type = 1;
        e->a = tmp.value_type.constant;
        e->b = 0;
        break;
    case cormoran_fast_keymap_BehaviorParameterValueDescription_range_tag:
        e->type = 2;
        e->a = (uint32_t)tmp.value_type.range.min;
        e->b = (uint32_t)tmp.value_type.range.max;
        break;
    case cormoran_fast_keymap_BehaviorParameterValueDescription_hid_usage_tag:
        e->type = 3;
        e->a = tmp.value_type.hid_usage.keyboard_max;
        e->b = tmp.value_type.hid_usage.consumer_max;
        break;
    case cormoran_fast_keymap_BehaviorParameterValueDescription_layer_id_tag:
        e->type = 4;
        e->a = 0;
        e->b = 0;
        break;
    default:
        LOG_ERR("unexpected value_type tag %d", tmp.which_value_type);
        return false;
    }

    return true;
}

struct param_set_capture {
    value_desc_capture_t param1;
    value_desc_capture_t param2;
};
typedef CAPTURE_ARRAY(struct param_set_capture, 2) param_set_capture_t;

static bool decode_param_set_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    param_set_capture_t *cap = *arg;
    if (cap->count >= ARRAY_SIZE(cap->entries)) {
        LOG_ERR("too many parameter sets for test capture");
        return false;
    }

    struct param_set_capture *e = &cap->entries[cap->count++];
    *e = (struct param_set_capture){0};

    cormoran_fast_keymap_BehaviorBindingParametersSet tmp =
        cormoran_fast_keymap_BehaviorBindingParametersSet_init_zero;
    tmp.param1.funcs.decode = decode_value_desc_cb;
    tmp.param1.arg = &e->param1;
    tmp.param2.funcs.decode = decode_value_desc_cb;
    tmp.param2.arg = &e->param2;

    return pb_decode(stream, cormoran_fast_keymap_BehaviorBindingParametersSet_fields, &tmp);
}

/* Independent (test-local) reimplementation of DESIGN.md SS4/SS5's def_fp
 * canonicalization, deliberately NOT calling into fingerprint.c -- this is
 * the "does the web-side recompute match the firmware's reported def_fp"
 * property, standing in for the real web/TS reimplementation. */
static uint32_t test_crc_u8(uint32_t seed, uint8_t v) { return crc32_ieee_update(seed, &v, 1); }

static uint32_t test_crc_u16le(uint32_t seed, uint16_t v) {
    uint8_t buf[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    return crc32_ieee_update(seed, buf, sizeof(buf));
}

static uint32_t test_crc_u32le(uint32_t seed, uint32_t v) {
    uint8_t buf[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return crc32_ieee_update(seed, buf, sizeof(buf));
}

static uint32_t test_crc_str16(uint32_t seed, const char *s) {
    size_t len = s ? strlen(s) : 0;
    seed = test_crc_u16le(seed, (uint16_t)len);
    if (len) {
        seed = crc32_ieee_update(seed, (const uint8_t *)s, len);
    }
    return seed;
}

static uint32_t test_crc_str8(uint32_t seed, const char *s) {
    size_t len = s ? strlen(s) : 0;
    seed = test_crc_u8(seed, (uint8_t)len);
    if (len) {
        seed = crc32_ieee_update(seed, (const uint8_t *)s, len);
    }
    return seed;
}

/* The width the device serves def_fp at (Snapshot.def_fp_bits /
 * fk_def_fp_bits()); recompute_def_fp() masks its full result to this so it
 * matches BehaviorSummary.def_fp exactly. Refreshed by
 * sync_served_def_fp_bits() before each test that recomputes. */
static uint8_t g_served_def_fp_bits = 16;

static uint32_t mask_to_bits(uint32_t v, uint8_t bits) {
    return bits >= 32 ? v : (v & ((1u << bits) - 1u));
}

static void sync_served_def_fp_bits(void) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_get_snapshot_tag;
    req.request_type.get_snapshot = true;

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    if (call_fast_keymap_rpc(&req, &resp) &&
        resp.which_response_type == cormoran_fast_keymap_Response_snapshot_tag) {
        uint32_t bits = resp.response_type.snapshot.def_fp_bits;
        g_served_def_fp_bits = bits ? (uint8_t)bits : 16;
    }
}

static uint32_t recompute_def_fp(const char *alias, const char *display_name,
                                 const param_set_capture_t *metadata) {
    uint32_t seed = 0;
    seed = test_crc_u8(seed, (uint8_t)metadata->count);

    for (size_t i = 0; i < metadata->count; i++) {
        const struct param_set_capture *set = &metadata->entries[i];
        seed = test_crc_u8(seed, (uint8_t)set->param1.count);
        seed = test_crc_u8(seed, (uint8_t)set->param2.count);

        for (int pass = 0; pass < 2; pass++) {
            const value_desc_capture_t *values = pass == 0 ? &set->param1 : &set->param2;
            for (size_t v = 0; v < values->count; v++) {
                const struct value_desc_capture *val = &values->entries[v];
                seed = test_crc_u8(seed, val->type);
                seed = test_crc_u32le(seed, val->a);
                seed = test_crc_u32le(seed, val->b);
                seed = test_crc_str8(seed, val->name);
            }
        }
    }

    seed = test_crc_str16(seed, alias);
    seed = test_crc_str16(seed, display_name);
    /* def_fp is served truncated to the device-adaptive width
     * (fk_def_fp_bits() / Snapshot.def_fp_bits); mirror that here so this
     * stand-in for the web recompute matches BehaviorSummary.def_fp. */
    return mask_to_bits(seed, g_served_def_fp_bits);
}

struct behavior_summary_entry {
    uint32_t local_id;
    uint32_t def_fp;
    bool has_details;
    char alias[48];
    char display_name[48];
    param_set_capture_t metadata;
};
typedef CAPTURE_ARRAY(struct behavior_summary_entry, 16) behavior_summary_capture_t;

static bool decode_behavior_summary_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    behavior_summary_capture_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many behaviors for test capture");
        return false;
    }

    cormoran_fast_keymap_BehaviorSummary tmp = cormoran_fast_keymap_BehaviorSummary_init_zero;
    struct str_capture alias_cap = {0};
    struct str_capture display_cap = {0};
    param_set_capture_t metadata_cap = {0};
    tmp.details.alias.funcs.decode = decode_str_cb;
    tmp.details.alias.arg = &alias_cap;
    tmp.details.display_name.funcs.decode = decode_str_cb;
    tmp.details.display_name.arg = &display_cap;
    tmp.details.metadata.funcs.decode = decode_param_set_cb;
    tmp.details.metadata.arg = &metadata_cap;

    if (!pb_decode(stream, cormoran_fast_keymap_BehaviorSummary_fields, &tmp)) {
        return false;
    }

    struct behavior_summary_entry *e = &captured->entries[captured->count++];
    e->local_id = tmp.local_id;
    e->def_fp = tmp.def_fp;
    e->has_details = tmp.has_details;
    strncpy(e->alias, alias_cap.buf, sizeof(e->alias) - 1);
    e->alias[sizeof(e->alias) - 1] = '\0';
    strncpy(e->display_name, display_cap.buf, sizeof(e->display_name) - 1);
    e->display_name[sizeof(e->display_name) - 1] = '\0';
    e->metadata = metadata_cap;
    return true;
}

static const struct behavior_summary_entry *
find_behavior_by_alias(const behavior_summary_capture_t *cap, const char *alias) {
    for (size_t i = 0; i < cap->count; i++) {
        if (strcmp(cap->entries[i].alias, alias) == 0) {
            return &cap->entries[i];
        }
    }
    return NULL;
}

/* -- get_behaviors (batch) decode helpers -------------------------------- */

struct behavior_entry_capture {
    uint32_t id;
    char alias[48];
    char display_name[48];
    param_set_capture_t metadata;
};
typedef CAPTURE_ARRAY(struct behavior_entry_capture, 16) behavior_entry_capture_array_t;

static bool decode_behavior_entry_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    behavior_entry_capture_array_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many behavior entries for test capture");
        return false;
    }

    cormoran_fast_keymap_BehaviorEntry tmp = cormoran_fast_keymap_BehaviorEntry_init_zero;
    struct str_capture alias_cap = {0};
    struct str_capture display_cap = {0};
    param_set_capture_t metadata_cap = {0};
    tmp.details.alias.funcs.decode = decode_str_cb;
    tmp.details.alias.arg = &alias_cap;
    tmp.details.display_name.funcs.decode = decode_str_cb;
    tmp.details.display_name.arg = &display_cap;
    tmp.details.metadata.funcs.decode = decode_param_set_cb;
    tmp.details.metadata.arg = &metadata_cap;

    if (!pb_decode(stream, cormoran_fast_keymap_BehaviorEntry_fields, &tmp)) {
        return false;
    }

    struct behavior_entry_capture *e = &captured->entries[captured->count++];
    e->id = tmp.id;
    strncpy(e->alias, alias_cap.buf, sizeof(e->alias) - 1);
    e->alias[sizeof(e->alias) - 1] = '\0';
    strncpy(e->display_name, display_cap.buf, sizeof(e->display_name) - 1);
    e->display_name[sizeof(e->display_name) - 1] = '\0';
    e->metadata = metadata_cap;
    return true;
}

static const struct behavior_entry_capture *
find_behavior_entry_by_id(const behavior_entry_capture_array_t *cap, uint32_t id) {
    for (size_t i = 0; i < cap->count; i++) {
        if (cap->entries[i].id == id) {
            return &cap->entries[i];
        }
    }
    return NULL;
}

/* -- get_physical_layouts / get_physical_layout decode helpers ---------- */

struct key_attrs_capture {
    int32_t width, height, x, y, r, rx, ry;
};
typedef CAPTURE_ARRAY(struct key_attrs_capture, 8) key_attrs_capture_t;

static bool decode_key_attrs_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    key_attrs_capture_t *cap = *arg;
    if (cap->count >= ARRAY_SIZE(cap->entries)) {
        LOG_ERR("too many keys for test capture");
        return false;
    }

    cormoran_fast_keymap_KeyPhysicalAttrs tmp = cormoran_fast_keymap_KeyPhysicalAttrs_init_zero;
    if (!pb_decode(stream, cormoran_fast_keymap_KeyPhysicalAttrs_fields, &tmp)) {
        return false;
    }

    struct key_attrs_capture *e = &cap->entries[cap->count++];
    e->width = tmp.width;
    e->height = tmp.height;
    e->x = tmp.x;
    e->y = tmp.y;
    e->r = tmp.r;
    e->rx = tmp.rx;
    e->ry = tmp.ry;
    return true;
}

struct layout_summary_capture {
    char name[48];
    uint32_t fp;
    key_attrs_capture_t keys;
};
typedef CAPTURE_ARRAY(struct layout_summary_capture, 4) layout_summary_capture_t;

static bool decode_layout_summary_cb(pb_istream_t *stream, const pb_field_t *field, void **arg) {
    ARG_UNUSED(field);
    layout_summary_capture_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many physical layouts for test capture");
        return false;
    }

    cormoran_fast_keymap_PhysicalLayoutSummary tmp =
        cormoran_fast_keymap_PhysicalLayoutSummary_init_zero;
    struct str_capture name_cap = {0};
    key_attrs_capture_t keys_cap = {0};
    tmp.name.funcs.decode = decode_str_cb;
    tmp.name.arg = &name_cap;
    tmp.keys.funcs.decode = decode_key_attrs_cb;
    tmp.keys.arg = &keys_cap;

    if (!pb_decode(stream, cormoran_fast_keymap_PhysicalLayoutSummary_fields, &tmp)) {
        return false;
    }

    struct layout_summary_capture *e = &captured->entries[captured->count++];
    strncpy(e->name, name_cap.buf, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';
    e->fp = tmp.fp;
    e->keys = keys_cap;
    return true;
}

/* -- tests ------------------------------------------------------------- */

static int test_snapshot_fingerprint_stable(void) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_get_snapshot_tag;
    req.request_type.get_snapshot = true;

    cormoran_fast_keymap_Response resp1 = cormoran_fast_keymap_Response_init_zero;
    cormoran_fast_keymap_Response resp2 = cormoran_fast_keymap_Response_init_zero;

    if (!call_fast_keymap_rpc(&req, &resp1) || !call_fast_keymap_rpc(&req, &resp2)) {
        return -EINVAL;
    }

    if (resp1.which_response_type != cormoran_fast_keymap_Response_snapshot_tag ||
        resp2.which_response_type != cormoran_fast_keymap_Response_snapshot_tag) {
        LOG_ERR("get_snapshot did not return a snapshot: %d / %d", resp1.which_response_type,
                resp2.which_response_type);
        return -EINVAL;
    }

    const cormoran_fast_keymap_Snapshot *s1 = &resp1.response_type.snapshot;
    const cormoran_fast_keymap_Snapshot *s2 = &resp2.response_type.snapshot;

    if (s1->definition_version != FK_DEFINITION_VERSION) {
        LOG_ERR("unexpected definition_version %u", s1->definition_version);
        return -EINVAL;
    }

    if (s1->behaviors_fp != s2->behaviors_fp || s1->default_keymap_fp != s2->default_keymap_fp ||
        s1->keymap_fp != s2->keymap_fp || s1->physical_layout_fp != s2->physical_layout_fp ||
        s1->behavior_count != s2->behavior_count || s1->available_layers != s2->available_layers ||
        s1->active_layout_index != s2->active_layout_index) {
        LOG_ERR("snapshot fingerprints not stable across calls");
        return -EINVAL;
    }

    if (s1->behavior_count == 0 || s1->behaviors_fp == 0 || s1->keymap_fp == 0) {
        LOG_ERR("unexpectedly zero-valued fingerprint/count (behavior_count=%u behaviors_fp=%u "
                "keymap_fp=%u)",
                s1->behavior_count, s1->behaviors_fp, s1->keymap_fp);
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_snapshot_fingerprint_stable");
    return 0;
}

static int test_get_layer_matches_live_keymap(void) {
    for (uint8_t layer_id = 0; layer_id < 2; layer_id++) {
        cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
        req.which_request_type = cormoran_fast_keymap_Request_get_layer_tag;
        req.request_type.get_layer.layer_id = layer_id;

        cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
        binding_capture_t captured = {0};
        /* Pre-set which_field to the expected tag: nanopb memsets a oneof
         * submessage (wiping any pre-set callback funcs) whenever the tag
         * being decoded differs from which_field's current value. */
        resp.which_response_type = cormoran_fast_keymap_Response_get_layer_tag;
        /* Explicitly zero this specific oneof arm's struct -- Response_init_zero
         * only zeros the union up to its smallest listed member, not up to
         * whichever arm we're about to use, so untouched fields here (e.g.
         * `name`) would otherwise be uninitialized stack garbage. */
        resp.response_type.get_layer =
            (cormoran_fast_keymap_Layer)cormoran_fast_keymap_Layer_init_zero;
        resp.response_type.get_layer.bindings.funcs.decode = decode_binding_cb;
        resp.response_type.get_layer.bindings.arg = &captured;

        if (!call_fast_keymap_rpc(&req, &resp)) {
            return -EINVAL;
        }
        if (resp.which_response_type != cormoran_fast_keymap_Response_get_layer_tag) {
            LOG_ERR("get_layer(%d) unexpected response type %d", layer_id,
                    resp.which_response_type);
            return -EINVAL;
        }
        if (captured.count != ZMK_KEYMAP_LEN) {
            LOG_ERR("get_layer(%d) returned %u bindings, expected %d", layer_id,
                    (unsigned int)captured.count, ZMK_KEYMAP_LEN);
            return -EINVAL;
        }

        for (uint16_t b = 0; b < ZMK_KEYMAP_LEN; b++) {
            const struct zmk_behavior_binding *live =
                zmk_keymap_get_layer_binding_at_idx(layer_id, b);
            int32_t want_id = -1;
            uint32_t want_p1 = 0, want_p2 = 0;
            if (live && live->behavior_dev) {
                want_id = zmk_behavior_get_local_id(live->behavior_dev);
                want_p1 = live->param1;
                want_p2 = live->param2;
            }

            const cormoran_fast_keymap_BehaviorBinding *got = &captured.entries[b];
            if (got->behavior_id != want_id || got->param1 != want_p1 || got->param2 != want_p2) {
                LOG_ERR("get_layer(%d) binding %d mismatch: got id=%d p1=%u p2=%u, want id=%d "
                        "p1=%u p2=%u",
                        layer_id, b, got->behavior_id, got->param1, got->param2, want_id, want_p1,
                        want_p2);
                return -EINVAL;
            }
        }
    }

    LOG_INF("PASS: fast_keymap_get_layer_matches_live_keymap");
    return 0;
}

/* get_layers batches several layers' effective bindings in one response
 * (DESIGN.md SS7): request layers 0 and 1 plus an invalid id, and assert the
 * batch returns exactly the two valid layers (invalid skipped) with bindings
 * identical to the live keymap -- the same property test_get_layer_matches_
 * live_keymap() checks per single-layer call. */
static int test_get_layers_batch(void) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_get_layers_tag;
    req.request_type.get_layers =
        (cormoran_fast_keymap_GetLayersRequest)cormoran_fast_keymap_GetLayersRequest_init_zero;
    req.request_type.get_layers.layer_ids_count = 3;
    req.request_type.get_layers.layer_ids[0] = 0;
    req.request_type.get_layers.layer_ids[1] = 1;
    req.request_type.get_layers.layer_ids[2] = 250; /* invalid -> skipped */

    static layers_capture_t cap;
    cap = (layers_capture_t){0};

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    resp.which_response_type = cormoran_fast_keymap_Response_get_layers_tag;
    resp.response_type.get_layers =
        (cormoran_fast_keymap_GetLayersResponse)cormoran_fast_keymap_GetLayersResponse_init_zero;
    resp.response_type.get_layers.layers.funcs.decode = decode_layers_cb;
    resp.response_type.get_layers.layers.arg = &cap;

    if (!call_fast_keymap_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_get_layers_tag) {
        LOG_ERR("get_layers unexpected response type %d", resp.which_response_type);
        return -EINVAL;
    }
    if (cap.count != 2) {
        LOG_ERR("get_layers returned %u layers, expected 2 (invalid id skipped)",
                (unsigned int)cap.count);
        return -EINVAL;
    }

    for (size_t i = 0; i < cap.count; i++) {
        const struct layers_entry *e = &cap.entries[i];
        if (e->id != 0 && e->id != 1) {
            LOG_ERR("get_layers returned unexpected layer id %u", e->id);
            return -EINVAL;
        }
        if (e->bindings.count != ZMK_KEYMAP_LEN) {
            LOG_ERR("get_layers layer %u returned %u bindings, expected %d", e->id,
                    (unsigned int)e->bindings.count, ZMK_KEYMAP_LEN);
            return -EINVAL;
        }

        for (uint16_t b = 0; b < ZMK_KEYMAP_LEN; b++) {
            const struct zmk_behavior_binding *live = zmk_keymap_get_layer_binding_at_idx(e->id, b);
            int32_t want_id = -1;
            uint32_t want_p1 = 0, want_p2 = 0;
            if (live && live->behavior_dev) {
                want_id = zmk_behavior_get_local_id(live->behavior_dev);
                want_p1 = live->param1;
                want_p2 = live->param2;
            }

            const cormoran_fast_keymap_BehaviorBinding *got = &e->bindings.entries[b];
            if (got->behavior_id != want_id || got->param1 != want_p1 || got->param2 != want_p2) {
                LOG_ERR("get_layers layer %u binding %d mismatch: got id=%d p1=%u p2=%u", e->id, b,
                        got->behavior_id, got->param1, got->param2);
                return -EINVAL;
            }
        }
    }

    LOG_INF("PASS: fast_keymap_get_layers_batch");
    return 0;
}

static bool call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode mode,
                                behavior_summary_capture_t *captured) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_list_behaviors_tag;
    req.request_type.list_behaviors = (cormoran_fast_keymap_ListBehaviorsRequest)
        cormoran_fast_keymap_ListBehaviorsRequest_init_zero;
    req.request_type.list_behaviors.details = mode;
    /* Ask for the alias too (opt-in) so the def_fp encoder-consistency checks
     * that recompute from the served alias have it; display_name is on by
     * default. The dedicated field-selection test covers omitting them. */
    req.request_type.list_behaviors.include_alias = true;

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    resp.which_response_type = cormoran_fast_keymap_Response_list_behaviors_tag;
    resp.response_type.list_behaviors = (cormoran_fast_keymap_ListBehaviorsResponse)
        cormoran_fast_keymap_ListBehaviorsResponse_init_zero;
    resp.response_type.list_behaviors.behaviors.funcs.decode = decode_behavior_summary_cb;
    resp.response_type.list_behaviors.behaviors.arg = captured;

    if (!call_fast_keymap_rpc(&req, &resp)) {
        return false;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_list_behaviors_tag) {
        LOG_ERR("list_behaviors(mode=%d) unexpected response type %d", mode,
                resp.which_response_type);
        return false;
    }
    return true;
}

/* Like call_list_behaviors but with explicit include_alias / exclude_display_name
 * flags, for the field-selection test. */
static bool issue_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode mode, bool include_alias,
                                 bool exclude_display_name, behavior_summary_capture_t *captured) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_list_behaviors_tag;
    req.request_type.list_behaviors = (cormoran_fast_keymap_ListBehaviorsRequest)
        cormoran_fast_keymap_ListBehaviorsRequest_init_zero;
    req.request_type.list_behaviors.details = mode;
    req.request_type.list_behaviors.include_alias = include_alias;
    req.request_type.list_behaviors.exclude_display_name = exclude_display_name;

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    resp.which_response_type = cormoran_fast_keymap_Response_list_behaviors_tag;
    resp.response_type.list_behaviors = (cormoran_fast_keymap_ListBehaviorsResponse)
        cormoran_fast_keymap_ListBehaviorsResponse_init_zero;
    resp.response_type.list_behaviors.behaviors.funcs.decode = decode_behavior_summary_cb;
    resp.response_type.list_behaviors.behaviors.arg = captured;

    if (!call_fast_keymap_rpc(&req, &resp)) {
        return false;
    }
    return resp.which_response_type == cormoran_fast_keymap_Response_list_behaviors_tag;
}

static const struct behavior_summary_entry *
find_summary_by_local_id(const behavior_summary_capture_t *cap, uint32_t local_id) {
    for (size_t i = 0; i < cap->count; i++) {
        if (cap->entries[i].local_id == local_id) {
            return &cap->entries[i];
        }
    }
    return NULL;
}

/* alias is opt-in and display_name opt-out (ListBehaviorsRequest.include_alias
 * / exclude_display_name): assert each flag actually controls whether that
 * field rides the wire, and that def_fp is unaffected (it hashes the true
 * alias/display_name regardless of what's served). */
static int test_list_behaviors_field_selection(void) {
    static behavior_summary_capture_t no_alias;
    static behavior_summary_capture_t no_name;
    no_alias = (behavior_summary_capture_t){0};
    no_name = (behavior_summary_capture_t){0};

    /* ALL mode so every summary carries inline details. */
    if (!issue_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL,
                              /*include_alias=*/false, /*exclude_display_name=*/false, &no_alias) ||
        !issue_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL,
                              /*include_alias=*/true, /*exclude_display_name=*/true, &no_name)) {
        return -EINVAL;
    }
    if (no_alias.count == 0 || no_alias.count != no_name.count) {
        LOG_ERR("field-selection list_behaviors counts differ (%u vs %u)",
                (unsigned int)no_alias.count, (unsigned int)no_name.count);
        return -EINVAL;
    }

    for (size_t i = 0; i < no_alias.count; i++) {
        const struct behavior_summary_entry *a = &no_alias.entries[i];
        if (!a->has_details) {
            LOG_ERR("ALL mode did not inline details for local_id %u", a->local_id);
            return -EINVAL;
        }
        /* include_alias=false -> alias omitted; display_name still present. */
        if (a->alias[0] != '\0') {
            LOG_ERR("include_alias=false but local_id %u served alias '%s'", a->local_id, a->alias);
            return -EINVAL;
        }
        if (a->display_name[0] == '\0') {
            LOG_ERR("exclude_display_name=false but local_id %u served no display_name",
                    a->local_id);
            return -EINVAL;
        }

        /* exclude_display_name=true -> display_name omitted; alias present. */
        const struct behavior_summary_entry *b = find_summary_by_local_id(&no_name, a->local_id);
        if (!b) {
            LOG_ERR("local_id %u missing from the second (no-display-name) list", a->local_id);
            return -EINVAL;
        }
        if (b->alias[0] == '\0') {
            LOG_ERR("include_alias=true but local_id %u served no alias", b->local_id);
            return -EINVAL;
        }
        if (b->display_name[0] != '\0') {
            LOG_ERR("exclude_display_name=true but local_id %u served display_name '%s'",
                    b->local_id, b->display_name);
            return -EINVAL;
        }

        /* Field selection must not change the fingerprint. */
        if (a->def_fp != b->def_fp) {
            LOG_ERR("def_fp for local_id %u changed with field selection (%u vs %u)", a->local_id,
                    a->def_fp, b->def_fp);
            return -EINVAL;
        }
    }

    LOG_INF("PASS: fast_keymap_list_behaviors_field_selection");
    return 0;
}

/* Covers DESIGN.md SS5's three BehaviorDetailsMode semantics, def_fp
 * determinism, and the encoder-consistency property: recompute def_fp from
 * the served alias/display_name/metadata bytes (recompute_def_fp(), a
 * from-scratch reimplementation of SS4's canonicalization, standing in for
 * the real web/TS side) and assert it equals what BehaviorSummary.def_fp
 * reports -- including for a behavior whose devicetree node has no label,
 * so the device-name alias fallback (fk_behavior_served_alias()) is
 * exercised on both the fingerprint and the wire encoder. */
static int test_list_behaviors_modes_and_def_fp(void) {
    static behavior_summary_capture_t none_cap;
    static behavior_summary_capture_t all_cap;
    static behavior_summary_capture_t custom_cap;
    none_cap = (behavior_summary_capture_t){0};
    all_cap = (behavior_summary_capture_t){0};
    custom_cap = (behavior_summary_capture_t){0};

    sync_served_def_fp_bits();

    if (!call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_NONE,
                             &none_cap) ||
        !call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL,
                             &all_cap) ||
        !call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_CUSTOM,
                             &custom_cap)) {
        return -EINVAL;
    }

    if (none_cap.count == 0 || none_cap.count != all_cap.count ||
        none_cap.count != custom_cap.count) {
        LOG_ERR("list_behaviors returned inconsistent behavior counts across modes (none=%u "
                "all=%u custom=%u)",
                (unsigned int)none_cap.count, (unsigned int)all_cap.count,
                (unsigned int)custom_cap.count);
        return -EINVAL;
    }

    /* def_fp determinism under caching: a second, independent list_behaviors
     * call must report exactly the same per-behavior def_fp as the first --
     * this is the property the memoized fk_fp_behavior_def() cache
     * (fingerprint.c) must preserve, since a stale/inconsistent cached
     * def_fp would silently break the web's cache-key comparisons. */
    static behavior_summary_capture_t none_cap2;
    none_cap2 = (behavior_summary_capture_t){0};
    if (!call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_NONE,
                             &none_cap2)) {
        return -EINVAL;
    }
    if (none_cap2.count != none_cap.count) {
        LOG_ERR("list_behaviors(NONE) behavior count not stable across calls (%u vs %u)",
                (unsigned int)none_cap.count, (unsigned int)none_cap2.count);
        return -EINVAL;
    }
    for (size_t i = 0; i < none_cap.count; i++) {
        const struct behavior_summary_entry *e = &none_cap.entries[i];
        bool found = false;
        for (size_t j = 0; j < none_cap2.count; j++) {
            if (none_cap2.entries[j].local_id == e->local_id) {
                found = true;
                if (none_cap2.entries[j].def_fp != e->def_fp) {
                    LOG_ERR("def_fp for local_id %u not stable across list_behaviors calls (%u "
                            "vs %u)",
                            e->local_id, e->def_fp, none_cap2.entries[j].def_fp);
                    return -EINVAL;
                }
                break;
            }
        }
        if (!found) {
            LOG_ERR("local_id %u missing from second list_behaviors(NONE) call", e->local_id);
            return -EINVAL;
        }
    }

    /* NONE: no summary carries inline details. */
    for (size_t i = 0; i < none_cap.count; i++) {
        if (none_cap.entries[i].has_details) {
            LOG_ERR("BEHAVIOR_DETAILS_NONE: local_id %u unexpectedly has details",
                    none_cap.entries[i].local_id);
            return -EINVAL;
        }
    }

    /* ALL: every summary carries inline details, and every one's def_fp
     * matches an independent recomputation from the served fields -- the
     * encoder-consistency property. def_fp itself must also match the
     * mode-independent value reported under NONE. */
    for (size_t i = 0; i < all_cap.count; i++) {
        const struct behavior_summary_entry *e = &all_cap.entries[i];
        if (!e->has_details) {
            LOG_ERR("BEHAVIOR_DETAILS_ALL: local_id %u missing details", e->local_id);
            return -EINVAL;
        }

        uint32_t recomputed = recompute_def_fp(e->alias, e->display_name, &e->metadata);
        if (recomputed != e->def_fp) {
            LOG_ERR("def_fp mismatch for local_id %u alias='%s': reported=%u recomputed=%u",
                    e->local_id, e->alias, e->def_fp, recomputed);
            return -EINVAL;
        }

        for (size_t j = 0; j < none_cap.count; j++) {
            if (none_cap.entries[j].local_id == e->local_id &&
                none_cap.entries[j].def_fp != e->def_fp) {
                LOG_ERR("def_fp for local_id %u (alias='%s') differs between NONE (%u) and ALL "
                        "(%u) modes",
                        e->local_id, e->alias, none_cap.entries[j].def_fp, e->def_fp);
                return -EINVAL;
            }
        }
    }

    /* CUSTOM: standard behaviors (kp/mo/trans) have no inline details;
     * custom ones (myht, and the label-less hold-tap) do, and their alias
     * matches the DT node label / device-name fallback respectively. */
    static const char *const expect_standard[] = {"kp", "mo", "trans"};
    for (size_t i = 0; i < ARRAY_SIZE(expect_standard); i++) {
        const struct behavior_summary_entry *e =
            find_behavior_by_alias(&all_cap, expect_standard[i]);
        if (!e) {
            LOG_ERR("list_behaviors did not resolve alias '%s' (count=%u)", expect_standard[i],
                    (unsigned int)all_cap.count);
            return -EINVAL;
        }
        const struct behavior_summary_entry *custom_e = NULL;
        for (size_t j = 0; j < custom_cap.count; j++) {
            if (custom_cap.entries[j].local_id == e->local_id) {
                custom_e = &custom_cap.entries[j];
                break;
            }
        }
        if (!custom_e || custom_e->has_details) {
            LOG_ERR("BEHAVIOR_DETAILS_CUSTOM: standard behavior '%s' should NOT have details",
                    expect_standard[i]);
            return -EINVAL;
        }
    }

    /* tests/studio/native_sim.keymap defines a custom (non-built-in)
     * hold-tap `myht: my_hold_tap {...}`. Its device name ("my_hold_tap")
     * isn't in any hardcoded table -- proving this resolves via the
     * compile-time devicetree node-label walk, not a fixed list of ZMK
     * built-ins. */
    const struct behavior_summary_entry *myht = find_behavior_by_alias(&all_cap, "myht");
    if (!myht) {
        LOG_ERR("list_behaviors did not resolve alias 'myht' for the custom hold-tap (count=%u)",
                (unsigned int)all_cap.count);
        return -EINVAL;
    }
    const struct behavior_summary_entry *myht_custom = NULL;
    for (size_t j = 0; j < custom_cap.count; j++) {
        if (custom_cap.entries[j].local_id == myht->local_id) {
            myht_custom = &custom_cap.entries[j];
            break;
        }
    }
    if (!myht_custom || !myht_custom->has_details) {
        LOG_ERR("BEHAVIOR_DETAILS_CUSTOM: custom behavior 'myht' should have details");
        return -EINVAL;
    }

    /* The label-less custom hold-tap: alias falls back to its device name. */
    const struct behavior_summary_entry *nolabel =
        find_behavior_by_alias(&all_cap, "my_hold_tap_nolabel");
    if (!nolabel) {
        LOG_ERR("list_behaviors did not resolve the label-less custom hold-tap by its "
                "device-name alias fallback (count=%u)",
                (unsigned int)all_cap.count);
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_list_behaviors_modes_and_def_fp");
    return 0;
}

/* get_behaviors batch (DESIGN.md SS5 miss-repair): request a couple of
 * known ids plus one unknown id in the same call, and assert each known id
 * gets a BehaviorEntry with the right alias/display_name/metadata while the
 * unknown id yields no entry (this handler's chosen "skip unknown ids"
 * behavior -- the response still succeeds for the ids that do resolve). */
static int test_get_behaviors_batch(void) {
    sync_served_def_fp_bits();

    static behavior_summary_capture_t list_cap;
    list_cap = (behavior_summary_capture_t){0};
    if (!call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL,
                             &list_cap)) {
        return -EINVAL;
    }

    const struct behavior_summary_entry *kp = find_behavior_by_alias(&list_cap, "kp");
    const struct behavior_summary_entry *myht = find_behavior_by_alias(&list_cap, "myht");
    if (!kp || !myht) {
        LOG_ERR("could not find 'kp'/'myht' in list_behaviors to seed get_behaviors batch test");
        return -EINVAL;
    }

    static const uint32_t unknown_id = 0xBEEF;

    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_get_behaviors_tag;
    req.request_type.get_behaviors = (cormoran_fast_keymap_GetBehaviorsRequest)
        cormoran_fast_keymap_GetBehaviorsRequest_init_zero;
    req.request_type.get_behaviors.behavior_ids_count = 3;
    req.request_type.get_behaviors.behavior_ids[0] = kp->local_id;
    req.request_type.get_behaviors.behavior_ids[1] = myht->local_id;
    req.request_type.get_behaviors.behavior_ids[2] = unknown_id;
    req.request_type.get_behaviors.include_alias = true; /* recompute needs the alias */

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    static behavior_entry_capture_array_t entries_cap;
    entries_cap = (behavior_entry_capture_array_t){0};
    resp.which_response_type = cormoran_fast_keymap_Response_get_behaviors_tag;
    resp.response_type.get_behaviors = (cormoran_fast_keymap_GetBehaviorsResponse)
        cormoran_fast_keymap_GetBehaviorsResponse_init_zero;
    resp.response_type.get_behaviors.behaviors.funcs.decode = decode_behavior_entry_cb;
    resp.response_type.get_behaviors.behaviors.arg = &entries_cap;

    if (!call_fast_keymap_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_get_behaviors_tag) {
        LOG_ERR("get_behaviors unexpected response type %d", resp.which_response_type);
        return -EINVAL;
    }

    if (entries_cap.count != 2) {
        LOG_ERR("get_behaviors: expected exactly 2 entries (unknown id skipped), got %u",
                (unsigned int)entries_cap.count);
        return -EINVAL;
    }
    if (find_behavior_entry_by_id(&entries_cap, unknown_id)) {
        LOG_ERR("get_behaviors returned an entry for the unknown id %u", unknown_id);
        return -EINVAL;
    }

    const struct behavior_entry_capture *kp_entry =
        find_behavior_entry_by_id(&entries_cap, kp->local_id);
    const struct behavior_entry_capture *myht_entry =
        find_behavior_entry_by_id(&entries_cap, myht->local_id);
    if (!kp_entry || strcmp(kp_entry->alias, "kp") != 0) {
        LOG_ERR("get_behaviors: kp entry missing or has wrong alias ('%s')",
                kp_entry ? kp_entry->alias : "(missing)");
        return -EINVAL;
    }
    if (!myht_entry || strcmp(myht_entry->alias, "myht") != 0) {
        LOG_ERR("get_behaviors: myht entry missing or has wrong alias ('%s')",
                myht_entry ? myht_entry->alias : "(missing)");
        return -EINVAL;
    }
    if (recompute_def_fp(kp_entry->alias, kp_entry->display_name, &kp_entry->metadata) !=
        kp->def_fp) {
        LOG_ERR("get_behaviors: kp entry's fields don't recompute to list_behaviors' def_fp");
        return -EINVAL;
    }
    if (recompute_def_fp(myht_entry->alias, myht_entry->display_name, &myht_entry->metadata) !=
        myht->def_fp) {
        LOG_ERR("get_behaviors: myht entry's fields don't recompute to list_behaviors' def_fp");
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_get_behaviors_batch");
    return 0;
}

/* Covers DESIGN.md SS6's slim-summary + on-demand-geometry physical layouts
 * API: NONE mode omits `keys` entirely, ALL mode inlines it, a single
 * get_physical_layout(index) fetch returns the same fp/name/keys as the
 * ALL-mode summary, per-layout fp is deterministic across calls, an
 * out-of-range index yields ERR_INVALID_LAYOUT, and the Snapshot's whole-set
 * physical_layout_fp stays stable across all these read-only calls.
 * tests/studio/native_sim.keymap's physical layout ("Custom Layout") has
 * exactly one layout with 4 keys -- see tests/test.dtsi. */
static int test_physical_layouts(void) {
    /* NONE (default): summaries only, no keys. */
    cormoran_fast_keymap_Request none_req = cormoran_fast_keymap_Request_init_zero;
    none_req.which_request_type = cormoran_fast_keymap_Request_get_physical_layouts_tag;
    none_req.request_type.get_physical_layouts = (cormoran_fast_keymap_GetPhysicalLayoutsRequest)
        cormoran_fast_keymap_GetPhysicalLayoutsRequest_init_zero;
    none_req.request_type.get_physical_layouts.details =
        cormoran_fast_keymap_LayoutDetailsMode_LAYOUT_DETAILS_NONE;

    cormoran_fast_keymap_Response none_resp = cormoran_fast_keymap_Response_init_zero;
    static layout_summary_capture_t none_cap;
    none_cap = (layout_summary_capture_t){0};
    none_resp.which_response_type = cormoran_fast_keymap_Response_get_physical_layouts_tag;
    none_resp.response_type.get_physical_layouts = (cormoran_fast_keymap_PhysicalLayoutsResponse)
        cormoran_fast_keymap_PhysicalLayoutsResponse_init_zero;
    none_resp.response_type.get_physical_layouts.layouts.funcs.decode = decode_layout_summary_cb;
    none_resp.response_type.get_physical_layouts.layouts.arg = &none_cap;

    if (!call_fast_keymap_rpc(&none_req, &none_resp)) {
        return -EINVAL;
    }
    if (none_resp.which_response_type != cormoran_fast_keymap_Response_get_physical_layouts_tag) {
        LOG_ERR("get_physical_layouts(NONE) unexpected response type %d",
                none_resp.which_response_type);
        return -EINVAL;
    }
    if (none_cap.count != 1) {
        LOG_ERR("get_physical_layouts(NONE) expected 1 layout (test.dtsi defines one), got %u",
                (unsigned int)none_cap.count);
        return -EINVAL;
    }
    if (strlen(none_cap.entries[0].name) == 0) {
        LOG_ERR("get_physical_layouts(NONE) layout name is empty");
        return -EINVAL;
    }
    if (none_cap.entries[0].fp == 0) {
        LOG_ERR("get_physical_layouts(NONE) layout fp is zero");
        return -EINVAL;
    }
    if (none_cap.entries[0].keys.count != 0) {
        LOG_ERR("get_physical_layouts(NONE) unexpectedly included keys (count=%u)",
                (unsigned int)none_cap.entries[0].keys.count);
        return -EINVAL;
    }
    if (none_resp.response_type.get_physical_layouts.active_layout_index !=
        zmk_physical_layouts_get_selected()) {
        LOG_ERR("get_physical_layouts active_layout_index mismatch");
        return -EINVAL;
    }

    /* ALL: same summary, plus keys. */
    cormoran_fast_keymap_Request all_req = cormoran_fast_keymap_Request_init_zero;
    all_req.which_request_type = cormoran_fast_keymap_Request_get_physical_layouts_tag;
    all_req.request_type.get_physical_layouts = (cormoran_fast_keymap_GetPhysicalLayoutsRequest)
        cormoran_fast_keymap_GetPhysicalLayoutsRequest_init_zero;
    all_req.request_type.get_physical_layouts.details =
        cormoran_fast_keymap_LayoutDetailsMode_LAYOUT_DETAILS_ALL;

    cormoran_fast_keymap_Response all_resp = cormoran_fast_keymap_Response_init_zero;
    static layout_summary_capture_t all_cap;
    all_cap = (layout_summary_capture_t){0};
    all_resp.which_response_type = cormoran_fast_keymap_Response_get_physical_layouts_tag;
    all_resp.response_type.get_physical_layouts = (cormoran_fast_keymap_PhysicalLayoutsResponse)
        cormoran_fast_keymap_PhysicalLayoutsResponse_init_zero;
    all_resp.response_type.get_physical_layouts.layouts.funcs.decode = decode_layout_summary_cb;
    all_resp.response_type.get_physical_layouts.layouts.arg = &all_cap;

    if (!call_fast_keymap_rpc(&all_req, &all_resp)) {
        return -EINVAL;
    }
    if (all_cap.count != 1) {
        LOG_ERR("get_physical_layouts(ALL) expected 1 layout, got %u", (unsigned int)all_cap.count);
        return -EINVAL;
    }
    if (all_cap.entries[0].keys.count == 0) {
        LOG_ERR("get_physical_layouts(ALL) unexpectedly missing keys");
        return -EINVAL;
    }
    if (all_cap.entries[0].fp != none_cap.entries[0].fp) {
        LOG_ERR("per-layout fp differs between NONE (%u) and ALL (%u) modes",
                none_cap.entries[0].fp, all_cap.entries[0].fp);
        return -EINVAL;
    }

    /* fp determinism: repeating the ALL-mode request yields the same fp. */
    static layout_summary_capture_t all_cap2;
    all_cap2 = (layout_summary_capture_t){0};
    cormoran_fast_keymap_Response all_resp2 = cormoran_fast_keymap_Response_init_zero;
    all_resp2.which_response_type = cormoran_fast_keymap_Response_get_physical_layouts_tag;
    all_resp2.response_type.get_physical_layouts = (cormoran_fast_keymap_PhysicalLayoutsResponse)
        cormoran_fast_keymap_PhysicalLayoutsResponse_init_zero;
    all_resp2.response_type.get_physical_layouts.layouts.funcs.decode = decode_layout_summary_cb;
    all_resp2.response_type.get_physical_layouts.layouts.arg = &all_cap2;
    if (!call_fast_keymap_rpc(&all_req, &all_resp2)) {
        return -EINVAL;
    }
    if (all_cap2.count != 1 || all_cap2.entries[0].fp != all_cap.entries[0].fp) {
        LOG_ERR("per-layout fp not deterministic across calls");
        return -EINVAL;
    }

    /* get_physical_layout(0): single-layout geometry fetch must agree with
     * the ALL-mode summary's name/fp/keys. */
    cormoran_fast_keymap_Request single_req = cormoran_fast_keymap_Request_init_zero;
    single_req.which_request_type = cormoran_fast_keymap_Request_get_physical_layout_tag;
    single_req.request_type.get_physical_layout.layout_index = 0;

    cormoran_fast_keymap_Response single_resp = cormoran_fast_keymap_Response_init_zero;
    static struct str_capture single_name_cap;
    single_name_cap = (struct str_capture){0};
    static key_attrs_capture_t single_keys_cap;
    single_keys_cap = (key_attrs_capture_t){0};
    single_resp.which_response_type = cormoran_fast_keymap_Response_get_physical_layout_tag;
    single_resp.response_type.get_physical_layout = (cormoran_fast_keymap_PhysicalLayoutResponse)
        cormoran_fast_keymap_PhysicalLayoutResponse_init_zero;
    single_resp.response_type.get_physical_layout.layout.name.funcs.decode = decode_str_cb;
    single_resp.response_type.get_physical_layout.layout.name.arg = &single_name_cap;
    single_resp.response_type.get_physical_layout.layout.keys.funcs.decode = decode_key_attrs_cb;
    single_resp.response_type.get_physical_layout.layout.keys.arg = &single_keys_cap;

    if (!call_fast_keymap_rpc(&single_req, &single_resp)) {
        return -EINVAL;
    }
    if (single_resp.which_response_type != cormoran_fast_keymap_Response_get_physical_layout_tag) {
        LOG_ERR("get_physical_layout(0) unexpected response type %d",
                single_resp.which_response_type);
        return -EINVAL;
    }
    if (single_resp.response_type.get_physical_layout.index != 0) {
        LOG_ERR("get_physical_layout(0) unexpected index %u",
                single_resp.response_type.get_physical_layout.index);
        return -EINVAL;
    }
    if (single_resp.response_type.get_physical_layout.layout.fp != all_cap.entries[0].fp) {
        LOG_ERR("get_physical_layout(0) fp (%u) != get_physical_layouts summary fp (%u)",
                single_resp.response_type.get_physical_layout.layout.fp, all_cap.entries[0].fp);
        return -EINVAL;
    }
    if (single_keys_cap.count != all_cap.entries[0].keys.count) {
        LOG_ERR("get_physical_layout(0) returned %u keys, expected %u",
                (unsigned int)single_keys_cap.count, (unsigned int)all_cap.entries[0].keys.count);
        return -EINVAL;
    }
    if (strcmp(single_name_cap.buf, none_cap.entries[0].name) != 0) {
        LOG_ERR("get_physical_layout(0) name '%s' != get_physical_layouts summary name '%s'",
                single_name_cap.buf, none_cap.entries[0].name);
        return -EINVAL;
    }

    /* Out-of-range layout_index -> ERR_INVALID_LAYOUT. */
    cormoran_fast_keymap_Request bad_req = cormoran_fast_keymap_Request_init_zero;
    bad_req.which_request_type = cormoran_fast_keymap_Request_get_physical_layout_tag;
    bad_req.request_type.get_physical_layout.layout_index = 250;

    cormoran_fast_keymap_Response bad_resp = cormoran_fast_keymap_Response_init_zero;
    if (!call_fast_keymap_rpc(&bad_req, &bad_resp)) {
        return -EINVAL;
    }
    if (bad_resp.which_response_type != cormoran_fast_keymap_Response_error_tag ||
        bad_resp.response_type.error.code != cormoran_fast_keymap_ErrorCode_ERR_INVALID_LAYOUT) {
        LOG_ERR("get_physical_layout with an out-of-range index should return ERR_INVALID_LAYOUT "
                "(got type=%d code=%d)",
                bad_resp.which_response_type, bad_resp.response_type.error.code);
        return -EINVAL;
    }

    /* Snapshot's whole-set physical_layout_fp stays stable across all the
     * read-only physical-layout RPCs above. */
    cormoran_fast_keymap_Request snap_req = cormoran_fast_keymap_Request_init_zero;
    snap_req.which_request_type = cormoran_fast_keymap_Request_get_snapshot_tag;
    snap_req.request_type.get_snapshot = true;

    cormoran_fast_keymap_Response snap1 = cormoran_fast_keymap_Response_init_zero;
    cormoran_fast_keymap_Response snap2 = cormoran_fast_keymap_Response_init_zero;
    if (!call_fast_keymap_rpc(&snap_req, &snap1) || !call_fast_keymap_rpc(&snap_req, &snap2)) {
        return -EINVAL;
    }
    if (snap1.response_type.snapshot.physical_layout_fp !=
        snap2.response_type.snapshot.physical_layout_fp) {
        LOG_ERR("Snapshot physical_layout_fp not stable across get_physical_layout* calls");
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_physical_layouts");
    return 0;
}

/* -- dedup + mutable-fp-cache value preservation -----------------------
 *
 * Independent (test-local) recomputation of the three MUTABLE opaque fps
 * (keymap_fp, per-layer current fp, physical_layout_fp), from the live ZMK
 * keymap/layout data, using DESIGN.md SS4's canonical byte layout -- the same
 * straightforward per-fp walk the firmware did BEFORE the dedup/cache refactor
 * (i.e. exactly what the cache-OFF path must produce). Asserting the served
 * fps equal these proves the single shared walk + the cache preserve the
 * values byte-for-byte (cache on == cache off == pre-refactor). */
static uint32_t test_crc_i16le(uint32_t seed, int16_t v) {
    return test_crc_u16le(seed, (uint16_t)v);
}
static uint32_t test_crc_i32le(uint32_t seed, int32_t v) {
    return test_crc_u32le(seed, (uint32_t)v);
}

static uint32_t test_crc_binding(uint32_t seed, const struct zmk_behavior_binding *b) {
    int32_t local_id = -1;
    uint32_t p1 = 0, p2 = 0;
    if (b && b->behavior_dev) {
        local_id = (int32_t)zmk_behavior_get_local_id(b->behavior_dev);
        p1 = b->param1;
        p2 = b->param2;
    }
    seed = test_crc_i32le(seed, local_id);
    seed = test_crc_u32le(seed, p1);
    seed = test_crc_u32le(seed, p2);
    return seed;
}

static uint32_t ref_keymap_fp(void) {
    uint32_t seed = 0;
    seed = test_crc_u8(seed, (uint8_t)zmk_physical_layouts_get_selected());
    uint16_t binding_count = fk_binding_count();

    for (zmk_keymap_layer_index_t idx = 0; idx < ZMK_KEYMAP_LAYERS_LEN; idx++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(idx);
        if (layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            break;
        }
        seed = test_crc_u8(seed, layer_id);
        seed = test_crc_str16(seed, zmk_keymap_layer_name(layer_id));
        for (uint16_t b = 0; b < binding_count; b++) {
            seed = test_crc_binding(seed, zmk_keymap_get_layer_binding_at_idx(layer_id, b));
        }
    }
    return seed & 0xFFFFu;
}

static uint32_t ref_layer_current_fp(zmk_keymap_layer_id_t layer_id) {
    uint32_t seed = 0;
    uint16_t binding_count = fk_binding_count();
    for (uint16_t b = 0; b < binding_count; b++) {
        seed = test_crc_binding(seed, zmk_keymap_get_layer_binding_at_idx(layer_id, b));
    }
    return seed & 0xFFFFu;
}

static uint32_t ref_physical_layout_fp(void) {
    uint32_t seed = 0;
    seed = test_crc_u8(seed, (uint8_t)zmk_physical_layouts_get_selected());

    struct zmk_physical_layout const *const *layouts;
    size_t n = zmk_physical_layouts_get_list(&layouts);
    for (size_t i = 0; i < n; i++) {
        const struct zmk_physical_layout *l = layouts[i];
        seed = test_crc_str16(seed, l->display_name);
        seed = test_crc_u16le(seed, (uint16_t)l->keys_len);
        for (size_t k = 0; k < l->keys_len; k++) {
            const struct zmk_key_physical_attrs *a = &l->keys[k];
            seed = test_crc_i16le(seed, a->width);
            seed = test_crc_i16le(seed, a->height);
            seed = test_crc_i16le(seed, a->x);
            seed = test_crc_i16le(seed, a->y);
#if IS_ENABLED(CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION)
            seed = test_crc_i16le(seed, a->r);
            seed = test_crc_i16le(seed, a->rx);
            seed = test_crc_i16le(seed, a->ry);
#else
            seed = test_crc_i16le(seed, 0);
            seed = test_crc_i16le(seed, 0);
            seed = test_crc_i16le(seed, 0);
#endif
        }
    }
    return seed & 0xFFFFu;
}

/* Fetch a snapshot, capturing its per-layer fingerprints. */
static bool fetch_snapshot(cormoran_fast_keymap_Response *resp, layer_fp_capture_t *layers) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_get_snapshot_tag;
    req.request_type.get_snapshot = true;

    *resp = (cormoran_fast_keymap_Response)cormoran_fast_keymap_Response_init_zero;
    *layers = (layer_fp_capture_t){0};
    resp->which_response_type = cormoran_fast_keymap_Response_snapshot_tag;
    resp->response_type.snapshot =
        (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;
    resp->response_type.snapshot.layers.funcs.decode = decode_layer_fp_cb;
    resp->response_type.snapshot.layers.arg = layers;
    if (!call_fast_keymap_rpc(&req, resp)) {
        return false;
    }
    return resp->which_response_type == cormoran_fast_keymap_Response_snapshot_tag;
}

/* (i) served mutable fps match an independent recompute (dedup preserves the
 * exact values, and the cached path equals the cache-off/direct path);
 * (ii) after an edit + a cache invalidation, keymap_fp + the edited layer's
 * current fp change, while def_fp/behaviors_fp stay constant; (iii) a warm
 * read of the same state returns identical values. Runs before
 * test_edited_diff_and_default() and restores the keymap it touches, so it
 * leaves state clean for later tests. */
static int test_dedup_and_cache_values(void) {
    cormoran_fast_keymap_Response snap;
    layer_fp_capture_t layers;
    if (!fetch_snapshot(&snap, &layers)) {
        return -EINVAL;
    }

    /* (i) keymap_fp / physical_layout_fp / each per-layer current fp equal the
     * independent recomputation. */
    uint32_t want_keymap = ref_keymap_fp();
    if (snap.response_type.snapshot.keymap_fp != want_keymap) {
        LOG_ERR("keymap_fp %u != independent recompute %u (dedup altered the value)",
                snap.response_type.snapshot.keymap_fp, want_keymap);
        return -EINVAL;
    }
    uint32_t want_phys = ref_physical_layout_fp();
    if (snap.response_type.snapshot.physical_layout_fp != want_phys) {
        LOG_ERR("physical_layout_fp %u != independent recompute %u",
                snap.response_type.snapshot.physical_layout_fp, want_phys);
        return -EINVAL;
    }
    for (size_t i = 0; i < layers.count; i++) {
        uint32_t want = ref_layer_current_fp(layers.entries[i].id);
        if (layers.entries[i].fp != want) {
            LOG_ERR("layer %u current fp %u != independent recompute %u (dedup altered the value)",
                    layers.entries[i].id, layers.entries[i].fp, want);
            return -EINVAL;
        }
    }

    /* (iii) a warm read (same state, no edits) returns byte-identical values. */
    cormoran_fast_keymap_Response snap2;
    layer_fp_capture_t layers2;
    if (!fetch_snapshot(&snap2, &layers2)) {
        return -EINVAL;
    }
    if (snap2.response_type.snapshot.keymap_fp != snap.response_type.snapshot.keymap_fp ||
        snap2.response_type.snapshot.physical_layout_fp !=
            snap.response_type.snapshot.physical_layout_fp ||
        snap2.response_type.snapshot.behaviors_fp != snap.response_type.snapshot.behaviors_fp) {
        LOG_ERR("warm snapshot read returned different fingerprints");
        return -EINVAL;
    }

    /* (ii) edit a binding, invalidate via the REAL Studio RPC notification
     * (the production invalidation path -- trigger #1), and assert keymap_fp +
     * the edited layer's current fp change while behaviors_fp stays constant.
     * Edit layer 1 position 0 and restore it afterwards so this test is
     * order-independent. */
    const zmk_keymap_layer_id_t edit_layer = 1;
    const struct zmk_behavior_binding *orig = zmk_keymap_get_layer_binding_at_idx(edit_layer, 0);
    struct zmk_behavior_binding saved = orig ? *orig : (struct zmk_behavior_binding){0};

    struct zmk_behavior_binding edited = {
        .behavior_dev = "key_press", .param1 = 0x1234, .param2 = 0};
    if (zmk_keymap_set_layer_binding_at_idx(edit_layer, 0, edited) < 0) {
        LOG_ERR("dedup test: set_layer_binding_at_idx(edit) failed");
        return -EINVAL;
    }
    /* Model what a real Studio edit does: raise the notification the cache
     * listens on (no-op when the FP cache is compiled out -- the handler
     * force-recomputes each snapshot in that case). */
    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){0});

    cormoran_fast_keymap_Response after;
    layer_fp_capture_t after_layers;
    if (!fetch_snapshot(&after, &after_layers)) {
        return -EINVAL;
    }

    if (after.response_type.snapshot.keymap_fp == snap.response_type.snapshot.keymap_fp) {
        LOG_ERR("keymap_fp did not change after an edit + invalidation");
        return -EINVAL;
    }
    if (after.response_type.snapshot.behaviors_fp != snap.response_type.snapshot.behaviors_fp) {
        LOG_ERR("behaviors_fp changed after a keymap edit (must be immutable)");
        return -EINVAL;
    }
    const struct layer_fp_entry *before_l1 = find_layer_fp(&layers, edit_layer);
    const struct layer_fp_entry *after_l1 = find_layer_fp(&after_layers, edit_layer);
    if (!before_l1 || !after_l1 || before_l1->fp == after_l1->fp) {
        LOG_ERR("edited layer's current fp did not change");
        return -EINVAL;
    }
    /* Served fp still matches an independent recompute of the EDITED keymap. */
    if (after_l1->fp != ref_layer_current_fp(edit_layer) ||
        after.response_type.snapshot.keymap_fp != ref_keymap_fp()) {
        LOG_ERR("post-edit fps don't match independent recompute");
        return -EINVAL;
    }

    /* Restore the original binding + invalidate, leaving a clean state. */
    if (zmk_keymap_set_layer_binding_at_idx(edit_layer, 0, saved) < 0) {
        LOG_ERR("dedup test: set_layer_binding_at_idx(restore) failed");
        return -EINVAL;
    }
    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){0});

    cormoran_fast_keymap_Response restored;
    layer_fp_capture_t restored_layers;
    if (!fetch_snapshot(&restored, &restored_layers)) {
        return -EINVAL;
    }
    if (restored.response_type.snapshot.keymap_fp != snap.response_type.snapshot.keymap_fp) {
        LOG_ERR("keymap_fp not restored to original after undoing the edit (%u vs %u)",
                restored.response_type.snapshot.keymap_fp, snap.response_type.snapshot.keymap_fp);
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_dedup_and_cache_values");
    return 0;
}

static int test_edited_diff_and_default(void) {
#if !IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
    /* Without CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER the stock-keymap accessor
     * isn't compiled in and get_default_layer/get_edited_layer degrade to
     * "default == current" (no diff); this test asserts the real diff, so it
     * only runs when the feature is enabled (tests/studio/native_sim.conf sets
     * it). */
    return 0;
#else
    cormoran_fast_keymap_Request snap_req = cormoran_fast_keymap_Request_init_zero;
    snap_req.which_request_type = cormoran_fast_keymap_Request_get_snapshot_tag;
    snap_req.request_type.get_snapshot = true;

    cormoran_fast_keymap_Response before = cormoran_fast_keymap_Response_init_zero;
    layer_fp_capture_t before_layers = {0};
    before.which_response_type = cormoran_fast_keymap_Response_snapshot_tag;
    before.response_type.snapshot =
        (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;
    before.response_type.snapshot.layers.funcs.decode = decode_layer_fp_cb;
    before.response_type.snapshot.layers.arg = &before_layers;
    if (!call_fast_keymap_rpc(&snap_req, &before)) {
        return -EINVAL;
    }

    const struct layer_fp_entry *before_l0 = find_layer_fp(&before_layers, 0);
    if (!before_l0 || before_l0->edited_count != 0) {
        LOG_ERR("expected layer 0 to start with edited_count 0 (got %s)",
                before_l0 ? "nonzero" : "missing");
        return -EINVAL;
    }

    /* Capture every behavior's def_fp before the edit -- def_fp/behaviors_fp
     * are memoized (fingerprint.c) because they're immutable (compile-time
     * behavior metadata, independent of the keymap), so editing a keymap
     * binding below must NOT perturb them. */
    static behavior_summary_capture_t before_behaviors;
    before_behaviors = (behavior_summary_capture_t){0};
    if (!call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL,
                             &before_behaviors)) {
        return -EINVAL;
    }

    /* Edit layer 0, position 0 (originally `&kp A`) to a different keycode
     * value directly through the ZMK keymap API -- this module is read-only
     * and never edits the keymap itself, but the underlying RPC-editable
     * ZMK core API is exactly what a real edit (via the official studio
     * keymap subsystem) would call. */
    struct zmk_behavior_binding new_binding = {
        .behavior_dev = "key_press",
        .param1 = 99,
        .param2 = 0,
    };
    int ret = zmk_keymap_set_layer_binding_at_idx(0, 0, new_binding);
    if (ret < 0) {
        LOG_ERR("zmk_keymap_set_layer_binding_at_idx failed: %d", ret);
        return -EINVAL;
    }
    /* A real Studio edit goes through the official keymap subsystem, which
     * raises this notification; the mutable-fp cache (fingerprint.c) listens
     * on it to invalidate. This test edits the core keymap API directly, so it
     * must raise the notification itself for keymap_fp to reflect the edit
     * under CONFIG_ZMK_FAST_KEYMAP_FP_CACHE. */
    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){0});

    cormoran_fast_keymap_Response after = cormoran_fast_keymap_Response_init_zero;
    layer_fp_capture_t after_layers = {0};
    after.which_response_type = cormoran_fast_keymap_Response_snapshot_tag;
    after.response_type.snapshot =
        (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;
    after.response_type.snapshot.layers.funcs.decode = decode_layer_fp_cb;
    after.response_type.snapshot.layers.arg = &after_layers;
    if (!call_fast_keymap_rpc(&snap_req, &after)) {
        return -EINVAL;
    }

    const struct layer_fp_entry *after_l0 = find_layer_fp(&after_layers, 0);
    if (!after_l0) {
        LOG_ERR("layer 0 missing from snapshot after edit");
        return -EINVAL;
    }

    /* Cache-correctness property: behaviors_fp and every behavior's def_fp
     * are immutable (they don't depend on the keymap), so they must NOT
     * change just because the memoized def_fp/behaviors_fp caches
     * (fingerprint.c) happen to have been filled before this edit. A stale
     * OR an incorrectly-invalidated cache would both show up here: stale ->
     * wrong value if a real change should have propagated (not applicable
     * to these truly-immutable fps); incorrectly invalidated -> value
     * changes when it shouldn't. */
    if (before.response_type.snapshot.behaviors_fp != after.response_type.snapshot.behaviors_fp) {
        LOG_ERR("behaviors_fp changed after editing a keymap binding (should be immutable): "
                "before=%u after=%u",
                before.response_type.snapshot.behaviors_fp,
                after.response_type.snapshot.behaviors_fp);
        return -EINVAL;
    }

    static behavior_summary_capture_t after_behaviors;
    after_behaviors = (behavior_summary_capture_t){0};
    if (!call_list_behaviors(cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL,
                             &after_behaviors)) {
        return -EINVAL;
    }
    if (after_behaviors.count != before_behaviors.count) {
        LOG_ERR("list_behaviors behavior count changed after editing a keymap binding (%u vs %u)",
                (unsigned int)before_behaviors.count, (unsigned int)after_behaviors.count);
        return -EINVAL;
    }
    for (size_t i = 0; i < before_behaviors.count; i++) {
        const struct behavior_summary_entry *b = &before_behaviors.entries[i];
        bool found = false;
        for (size_t j = 0; j < after_behaviors.count; j++) {
            if (after_behaviors.entries[j].local_id == b->local_id) {
                found = true;
                if (after_behaviors.entries[j].def_fp != b->def_fp) {
                    LOG_ERR("def_fp for local_id %u changed after editing a keymap binding "
                            "(should be immutable): before=%u after=%u",
                            b->local_id, b->def_fp, after_behaviors.entries[j].def_fp);
                    return -EINVAL;
                }
                break;
            }
        }
        if (!found) {
            LOG_ERR("local_id %u missing from list_behaviors after editing a keymap binding",
                    b->local_id);
            return -EINVAL;
        }
    }

    if (before.response_type.snapshot.keymap_fp == after.response_type.snapshot.keymap_fp) {
        LOG_ERR("keymap_fp did not change after editing a binding");
        return -EINVAL;
    }
    if (before.response_type.snapshot.default_keymap_fp !=
        after.response_type.snapshot.default_keymap_fp) {
        LOG_ERR("default_keymap_fp changed after editing the CURRENT keymap (should be stable)");
        return -EINVAL;
    }
    if (before_l0->fp == after_l0->fp) {
        LOG_ERR("layer 0 fp did not change after editing a binding on it");
        return -EINVAL;
    }
    if (before_l0->default_fp != after_l0->default_fp) {
        LOG_ERR("layer 0 default_fp changed after editing the CURRENT binding (should be stable)");
        return -EINVAL;
    }
    if (after_l0->edited_count != 1) {
        LOG_ERR("expected exactly 1 edited binding on layer 0, got %u", after_l0->edited_count);
        return -EINVAL;
    }

    /* get_edited_layer should report exactly the one edited position. */
    cormoran_fast_keymap_Request edited_req = cormoran_fast_keymap_Request_init_zero;
    edited_req.which_request_type = cormoran_fast_keymap_Request_get_edited_layer_tag;
    edited_req.request_type.get_edited_layer.layer_id = 0;

    cormoran_fast_keymap_Response edited_resp = cormoran_fast_keymap_Response_init_zero;
    edited_capture_t edited_captured = {0};
    edited_resp.which_response_type = cormoran_fast_keymap_Response_get_edited_layer_tag;
    edited_resp.response_type.get_edited_layer =
        (cormoran_fast_keymap_EditedLayer)cormoran_fast_keymap_EditedLayer_init_zero;
    edited_resp.response_type.get_edited_layer.bindings.funcs.decode = decode_edited_cb;
    edited_resp.response_type.get_edited_layer.bindings.arg = &edited_captured;
    if (!call_fast_keymap_rpc(&edited_req, &edited_resp)) {
        return -EINVAL;
    }
    if (edited_resp.which_response_type != cormoran_fast_keymap_Response_get_edited_layer_tag) {
        LOG_ERR("get_edited_layer unexpected response type %d", edited_resp.which_response_type);
        return -EINVAL;
    }
    if (edited_captured.count != 1 || edited_captured.entries[0].position != 0 ||
        edited_captured.entries[0].binding.param1 != 99) {
        LOG_ERR("get_edited_layer did not return exactly {position=0, param1=99} (count=%u)",
                (unsigned int)edited_captured.count);
        return -EINVAL;
    }

    /* get_default_layer must still report the ORIGINAL (DT stock) binding. */
    const struct zmk_behavior_binding *stock = zmk_stock_keymap_get_layer_binding_at_idx(0, 0);

    cormoran_fast_keymap_Request default_req = cormoran_fast_keymap_Request_init_zero;
    default_req.which_request_type = cormoran_fast_keymap_Request_get_default_layer_tag;
    default_req.request_type.get_default_layer.layer_id = 0;

    cormoran_fast_keymap_Response default_resp = cormoran_fast_keymap_Response_init_zero;
    binding_capture_t default_captured = {0};
    default_resp.which_response_type = cormoran_fast_keymap_Response_get_default_layer_tag;
    default_resp.response_type.get_default_layer =
        (cormoran_fast_keymap_Layer)cormoran_fast_keymap_Layer_init_zero;
    default_resp.response_type.get_default_layer.bindings.funcs.decode = decode_binding_cb;
    default_resp.response_type.get_default_layer.bindings.arg = &default_captured;
    if (!call_fast_keymap_rpc(&default_req, &default_resp)) {
        return -EINVAL;
    }
    if (default_captured.count != ZMK_KEYMAP_LEN) {
        LOG_ERR("get_default_layer returned %u bindings, expected %d",
                (unsigned int)default_captured.count, ZMK_KEYMAP_LEN);
        return -EINVAL;
    }
    uint32_t want_default_p1 = (stock && stock->behavior_dev) ? stock->param1 : 0;
    if (default_captured.entries[0].param1 != want_default_p1 ||
        default_captured.entries[0].param1 == 99) {
        LOG_ERR("get_default_layer position 0 should still be the DT stock binding (param1=%u), "
                "got param1=%u",
                want_default_p1, default_captured.entries[0].param1);
        return -EINVAL;
    }

    /* get_layer (current) must reflect the edit. */
    cormoran_fast_keymap_Request current_req = cormoran_fast_keymap_Request_init_zero;
    current_req.which_request_type = cormoran_fast_keymap_Request_get_layer_tag;
    current_req.request_type.get_layer.layer_id = 0;

    cormoran_fast_keymap_Response current_resp = cormoran_fast_keymap_Response_init_zero;
    binding_capture_t current_captured = {0};
    current_resp.which_response_type = cormoran_fast_keymap_Response_get_layer_tag;
    current_resp.response_type.get_layer =
        (cormoran_fast_keymap_Layer)cormoran_fast_keymap_Layer_init_zero;
    current_resp.response_type.get_layer.bindings.funcs.decode = decode_binding_cb;
    current_resp.response_type.get_layer.bindings.arg = &current_captured;
    if (!call_fast_keymap_rpc(&current_req, &current_resp)) {
        return -EINVAL;
    }
    if (current_captured.count != ZMK_KEYMAP_LEN || current_captured.entries[0].param1 != 99) {
        LOG_ERR("get_layer position 0 should reflect the edit (param1=99), got param1=%u",
                current_captured.count ? current_captured.entries[0].param1 : 0);
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_edited_diff_and_default");
    return 0;
#endif /* CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER */
}

static int test_invalid_requests(void) {
    cormoran_fast_keymap_Request bad_layer = cormoran_fast_keymap_Request_init_zero;
    bad_layer.which_request_type = cormoran_fast_keymap_Request_get_layer_tag;
    bad_layer.request_type.get_layer.layer_id = 250;

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    if (!call_fast_keymap_rpc(&bad_layer, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_error_tag ||
        resp.response_type.error.code != cormoran_fast_keymap_ErrorCode_ERR_INVALID_LAYER) {
        LOG_ERR("get_layer with an out-of-range layer_id should return ERR_INVALID_LAYER "
                "(got type=%d code=%d)",
                resp.which_response_type, resp.response_type.error.code);
        return -EINVAL;
    }

    /* get_behaviors with only unknown ids should still succeed with an empty
     * batch (this handler's chosen "skip unknown ids" behavior -- see
     * test_get_behaviors_batch() for the mixed known/unknown case). */
    cormoran_fast_keymap_Request bad_behavior = cormoran_fast_keymap_Request_init_zero;
    bad_behavior.which_request_type = cormoran_fast_keymap_Request_get_behaviors_tag;
    bad_behavior.request_type.get_behaviors = (cormoran_fast_keymap_GetBehaviorsRequest)
        cormoran_fast_keymap_GetBehaviorsRequest_init_zero;
    bad_behavior.request_type.get_behaviors.behavior_ids_count = 1;
    bad_behavior.request_type.get_behaviors.behavior_ids[0] = 0xBEEF;

    resp = (cormoran_fast_keymap_Response)cormoran_fast_keymap_Response_init_zero;
    static behavior_entry_capture_array_t bad_behavior_cap;
    bad_behavior_cap = (behavior_entry_capture_array_t){0};
    resp.which_response_type = cormoran_fast_keymap_Response_get_behaviors_tag;
    resp.response_type.get_behaviors = (cormoran_fast_keymap_GetBehaviorsResponse)
        cormoran_fast_keymap_GetBehaviorsResponse_init_zero;
    resp.response_type.get_behaviors.behaviors.funcs.decode = decode_behavior_entry_cb;
    resp.response_type.get_behaviors.behaviors.arg = &bad_behavior_cap;
    if (!call_fast_keymap_rpc(&bad_behavior, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_get_behaviors_tag ||
        bad_behavior_cap.count != 0) {
        LOG_ERR("get_behaviors with only an unknown id should return an empty batch (got "
                "type=%d count=%u)",
                resp.which_response_type, (unsigned int)bad_behavior_cap.count);
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_invalid_requests");
    return 0;
}

static int fast_keymap_rpc_test_init(void) {
    int ret;

    /* This test runs from a SYS_INIT (APPLICATION priority), which runs
     * BEFORE main()'s settings_subsys_init()/settings_load(). Behavior local
     * IDs (CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE) are only
     * assigned by the "behavior" settings subtree's commit handler, which
     * fires when that subtree is loaded/committed; until then every
     * behavior's local_id is stuck at its zero-initialized default, making
     * zmk_behavior_get_local_id()/zmk_behavior_find_behavior_name_from_local_id()
     * and this module's local_id-keyed RPCs (list_behaviors, get_behaviors)
     * all resolve to id 0 and therefore ambiguous between behaviors. Load it
     * explicitly here, same fix as zmk-feature-custom-settings's RPC test. */
    ret = settings_subsys_init();
    if (ret < 0) {
        return ret;
    }
    ret = settings_load_subtree("behavior");
    if (ret < 0) {
        return ret;
    }

    /* Order matters: test_edited_diff_and_default() mutates layer 0's
     * runtime keymap, so run the read-only checks first. */
    ret = test_snapshot_fingerprint_stable();
    if (ret < 0) {
        return ret;
    }

    ret = test_get_layer_matches_live_keymap();
    if (ret < 0) {
        return ret;
    }

    ret = test_get_layers_batch();
    if (ret < 0) {
        return ret;
    }

    ret = test_list_behaviors_modes_and_def_fp();
    if (ret < 0) {
        return ret;
    }

    ret = test_list_behaviors_field_selection();
    if (ret < 0) {
        return ret;
    }

    ret = test_get_behaviors_batch();
    if (ret < 0) {
        return ret;
    }

    ret = test_physical_layouts();
    if (ret < 0) {
        return ret;
    }

    ret = test_invalid_requests();
    if (ret < 0) {
        return ret;
    }

    ret = test_dedup_and_cache_values();
    if (ret < 0) {
        return ret;
    }

    ret = test_edited_diff_and_default();
    if (ret < 0) {
        return ret;
    }

    return 0;
}

SYS_INIT(fast_keymap_rpc_test_init, APPLICATION, 99);

#endif /* CONFIG_ZMK_FAST_KEYMAP_STUDIO_RPC_TEST */
