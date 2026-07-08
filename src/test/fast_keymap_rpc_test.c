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
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <cormoran/fast-keymap/fast_keymap.pb.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

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
 * via ../test.dtsi) -- CONFIG_ZMK_STUDIO_RPC (implied) in turn selects
 * CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE (see src/studio/Kconfig in the zmk
 * dependency), so zmk_keymap_set_layer_binding_at_idx() /
 * zmk_stock_keymap_get_layer_binding_at_idx() are available for the
 * edited-diff test below.
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
        type entries[cap];                                                                          \
        size_t count;                                                                               \
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

struct behavior_summary_entry {
    uint32_t local_id;
    char alias[48];
    uint32_t metadata_fp;
    bool standard;
};
typedef CAPTURE_ARRAY(struct behavior_summary_entry, 64) behavior_summary_capture_t;

static bool decode_behavior_summary_cb(pb_istream_t *stream, const pb_field_t *field,
                                       void **arg) {
    ARG_UNUSED(field);
    behavior_summary_capture_t *captured = *arg;
    if (captured->count >= ARRAY_SIZE(captured->entries)) {
        LOG_ERR("too many behaviors for test capture");
        return false;
    }

    cormoran_fast_keymap_BehaviorSummary tmp = cormoran_fast_keymap_BehaviorSummary_init_zero;
    struct str_capture alias_cap = {0};
    tmp.alias.funcs.decode = decode_str_cb;
    tmp.alias.arg = &alias_cap;
    /* display_name intentionally left uncallbacked -- not needed here */

    if (!pb_decode(stream, cormoran_fast_keymap_BehaviorSummary_fields, &tmp)) {
        return false;
    }

    struct behavior_summary_entry *e = &captured->entries[captured->count++];
    e->local_id = tmp.local_id;
    strncpy(e->alias, alias_cap.buf, sizeof(e->alias) - 1);
    e->alias[sizeof(e->alias) - 1] = '\0';
    e->metadata_fp = tmp.metadata_fp;
    e->standard = tmp.standard;
    return true;
}

static const struct behavior_summary_entry *find_behavior_by_alias(
    const behavior_summary_capture_t *cap, const char *alias) {
    for (size_t i = 0; i < cap->count; i++) {
        if (strcmp(cap->entries[i].alias, alias) == 0) {
            return &cap->entries[i];
        }
    }
    return NULL;
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
        resp.response_type.get_layer = (cormoran_fast_keymap_Layer)cormoran_fast_keymap_Layer_init_zero;
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
            if (got->behavior_id != want_id || got->param1 != want_p1 ||
                got->param2 != want_p2) {
                LOG_ERR("get_layer(%d) binding %d mismatch: got id=%d p1=%u p2=%u, want id=%d "
                        "p1=%u p2=%u",
                        layer_id, b, got->behavior_id, got->param1, got->param2, want_id,
                        want_p1, want_p2);
                return -EINVAL;
            }
        }
    }

    LOG_INF("PASS: fast_keymap_get_layer_matches_live_keymap");
    return 0;
}

static int test_alias_resolution(void) {
    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;
    req.which_request_type = cormoran_fast_keymap_Request_list_behaviors_tag;
    req.request_type.list_behaviors = true;

    cormoran_fast_keymap_Response resp = cormoran_fast_keymap_Response_init_zero;
    behavior_summary_capture_t captured = {0};
    resp.which_response_type = cormoran_fast_keymap_Response_list_behaviors_tag;
    resp.response_type.list_behaviors =
        (cormoran_fast_keymap_ListBehaviorsResponse)cormoran_fast_keymap_ListBehaviorsResponse_init_zero;
    resp.response_type.list_behaviors.behaviors.funcs.decode = decode_behavior_summary_cb;
    resp.response_type.list_behaviors.behaviors.arg = &captured;

    if (!call_fast_keymap_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_list_behaviors_tag) {
        LOG_ERR("list_behaviors unexpected response type %d", resp.which_response_type);
        return -EINVAL;
    }

    static const char *const expect_standard[] = {"kp", "mo", "trans"};
    for (size_t i = 0; i < ARRAY_SIZE(expect_standard); i++) {
        const struct behavior_summary_entry *e = find_behavior_by_alias(&captured, expect_standard[i]);
        if (!e) {
            LOG_ERR("list_behaviors did not resolve alias '%s' (count=%u)", expect_standard[i],
                    (unsigned int)captured.count);
            return -EINVAL;
        }
        if (!e->standard) {
            LOG_ERR("behavior '%s' should be reported as standard", expect_standard[i]);
            return -EINVAL;
        }
    }

    LOG_INF("PASS: fast_keymap_alias_resolution");
    return 0;
}

static int test_edited_diff_and_default(void) {
    cormoran_fast_keymap_Request snap_req = cormoran_fast_keymap_Request_init_zero;
    snap_req.which_request_type = cormoran_fast_keymap_Request_get_snapshot_tag;
    snap_req.request_type.get_snapshot = true;

    cormoran_fast_keymap_Response before = cormoran_fast_keymap_Response_init_zero;
    layer_fp_capture_t before_layers = {0};
    before.which_response_type = cormoran_fast_keymap_Response_snapshot_tag;
    before.response_type.snapshot = (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;
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

    cormoran_fast_keymap_Response after = cormoran_fast_keymap_Response_init_zero;
    layer_fp_capture_t after_layers = {0};
    after.which_response_type = cormoran_fast_keymap_Response_snapshot_tag;
    after.response_type.snapshot = (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;
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
    current_resp.response_type.get_layer = (cormoran_fast_keymap_Layer)cormoran_fast_keymap_Layer_init_zero;
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

    cormoran_fast_keymap_Request bad_behavior = cormoran_fast_keymap_Request_init_zero;
    bad_behavior.which_request_type = cormoran_fast_keymap_Request_get_behavior_details_tag;
    bad_behavior.request_type.get_behavior_details.behavior_id = 0xBEEF;

    resp = (cormoran_fast_keymap_Response)cormoran_fast_keymap_Response_init_zero;
    if (!call_fast_keymap_rpc(&bad_behavior, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_fast_keymap_Response_error_tag ||
        resp.response_type.error.code != cormoran_fast_keymap_ErrorCode_ERR_INVALID_BEHAVIOR) {
        LOG_ERR("get_behavior_details with an unknown id should return ERR_INVALID_BEHAVIOR "
                "(got type=%d code=%d)",
                resp.which_response_type, resp.response_type.error.code);
        return -EINVAL;
    }

    LOG_INF("PASS: fast_keymap_invalid_requests");
    return 0;
}

static int fast_keymap_rpc_test_init(void) {
    int ret;

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

    ret = test_alias_resolution();
    if (ret < 0) {
        return ret;
    }

    ret = test_invalid_requests();
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
