/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/fast-keymap/fast_keymap.pb.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/physical_layouts.h>

#include "../behavior_alias.h"
#include "../fingerprint.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta fast_keymap_subsystem_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-fast-keymap/"),
    // Read-only data (behavior list + keymap); reading it isn't sensitive
    // and unsecured keeps the fast-load path fast -- no unlock round-trip.
    // The web template already implements the unlock prompt/retry flow (see
    // web/src/App.tsx), so switching this to ZMK_STUDIO_RPC_HANDLER_SECURED
    // requires no web changes.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__fast_keymap, &fast_keymap_subsystem_meta,
                         fast_keymap_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__fast_keymap, cormoran_fast_keymap_Response);

static void set_error(cormoran_fast_keymap_Response *resp, cormoran_fast_keymap_ErrorCode code) {
    resp->which_response_type = cormoran_fast_keymap_Response_error_tag;
    resp->response_type.error = (cormoran_fast_keymap_ErrorResponse){.code = code};
}

/* -- get_snapshot ---------------------------------------------------- */

/*
 * Per-layer fingerprints/edited-count are relatively expensive (a CRC pass
 * over every binding) and the response is encoded twice (size pass + real
 * pass, see DESIGN.md SS3) -- compute them exactly once into this static
 * array before returning, and have the callback just read it back. The
 * array lives in static storage so pointers into it stay valid across both
 * encode passes.
 */
struct fk_snapshot_layer_entry {
    uint8_t id;
    uint16_t binding_count;
    uint32_t fp;
    uint32_t default_fp;
    uint16_t edited_count;
};

static struct fk_snapshot_layer_entry fk_snapshot_layers[ZMK_KEYMAP_LAYERS_LEN];
static size_t fk_snapshot_layer_count;

static bool encode_snapshot_layer_name(pb_ostream_t *stream, const pb_field_t *field,
                                       void *const *arg) {
    uint8_t layer_id = *(uint8_t *)*arg;
    const char *name = zmk_keymap_layer_name(layer_id);

    if (!name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, name, strlen(name));
}

static bool encode_snapshot_layers(pb_ostream_t *stream, const pb_field_t *field,
                                   void *const *arg) {
    ARG_UNUSED(arg);

    for (size_t i = 0; i < fk_snapshot_layer_count; i++) {
        struct fk_snapshot_layer_entry *entry = &fk_snapshot_layers[i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_LayerFingerprint lf = cormoran_fast_keymap_LayerFingerprint_init_zero;
        lf.id = entry->id;
        lf.binding_count = entry->binding_count;
        lf.fp = entry->fp;
        lf.default_fp = entry->default_fp;
        lf.edited_count = entry->edited_count;
        lf.name.funcs.encode = encode_snapshot_layer_name;
        lf.name.arg = &entry->id;

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_LayerFingerprint_msg, &lf)) {
            return false;
        }
    }

    return true;
}

static void compute_snapshot_layers(void) {
    fk_snapshot_layer_count = 0;

    uint8_t layer_count = fk_layer_count();
    uint16_t binding_count = fk_binding_count();

    for (uint8_t idx = 0; idx < layer_count && idx < ARRAY_SIZE(fk_snapshot_layers); idx++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);

        struct fk_snapshot_layer_entry *entry = &fk_snapshot_layers[fk_snapshot_layer_count++];
        entry->id = id;
        entry->binding_count = binding_count;
        entry->fp = fk_fp_layer_current(id);
        entry->default_fp = fk_fp_layer_default(id);
        entry->edited_count = fk_layer_edited_count(id);
    }
}

static int handle_get_snapshot(cormoran_fast_keymap_Response *resp) {
#if !IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_FP_CACHE)
    /* Cache compiled out: no invalidation listener exists, so drop the shared
     * mutable-fp state at the start of every snapshot. The first read below
     * then repopulates it once (Part A dedup still applies WITHIN this
     * snapshot -- one binding walk total -- while nothing persists across
     * snapshots). With the cache on, invalidation is event/layout driven (see
     * fingerprint.c) so this force-drop is unnecessary. */
    fk_fp_mutable_invalidate();
#endif

    compute_snapshot_layers();

    cormoran_fast_keymap_Snapshot *snap = &resp->response_type.snapshot;
    *snap = (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;

    snap->definition_version = FK_DEFINITION_VERSION;
    snap->behaviors_fp = fk_fp_behaviors();
    snap->def_fp_bits = fk_def_fp_bits();
    snap->default_keymap_fp = fk_fp_default_keymap();
    snap->keymap_fp = fk_fp_keymap();
    snap->physical_layout_fp = fk_fp_physical_layout();
    snap->behavior_count = fk_behavior_count();
    snap->max_layer_name_length = CONFIG_ZMK_KEYMAP_LAYER_NAME_MAX_LEN;
    snap->available_layers = fk_available_layers();
    snap->active_layout_index = zmk_physical_layouts_get_selected();
    snap->layers.funcs.encode = encode_snapshot_layers;

    resp->which_response_type = cormoran_fast_keymap_Response_snapshot_tag;
    return 0;
}

/* -- behavior details (BehaviorDetails submessage) ---------------------
 *
 * Shared by list_behaviors' inline `details` (DESIGN.md SS5 modes) and
 * get_behaviors' batch BehaviorEntry.details -- both encode the exact same
 * alias/display_name/metadata for a device, via fill_behavior_details()
 * below. BehaviorDetails carries no fingerprint; the client recomputes
 * def_fp from these served fields (SS4/SS5).
 */

struct fk_metadata_encode_state {
    const struct behavior_parameter_metadata_set *sets;
    size_t sets_len;
    size_t i;
};

/* Reused sequentially (never concurrently -- RPC handling and nanopb's
 * synchronous depth-first submessage encoding never overlap two behaviors'
 * encodes) across every behavior a given response fills details for, same
 * pattern as the layer-fingerprint static array in handle_get_snapshot(). */
static struct fk_metadata_encode_state fk_metadata_state;

static bool encode_behavior_alias(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct device *dev = (const struct device *)*arg;
    /* MUST be the exact same served-alias helper fk_fp_behavior_def() uses
     * (fingerprint.c) -- see DESIGN.md SS4: def_fp is only a trustworthy
     * cache key if a client can recompute it byte-for-byte from what this
     * encoder actually serves. */
    const char *alias = fk_behavior_served_alias(dev);

    if (!alias) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, alias, strlen(alias));
}

static bool encode_behavior_display_name(pb_ostream_t *stream, const pb_field_t *field,
                                         void *const *arg) {
    const struct device *dev = (const struct device *)*arg;
    const char *name = fk_behavior_display_name(dev);

    if (!name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, name, strlen(name));
}

static bool encode_value_description_name(pb_ostream_t *stream, const pb_field_t *field,
                                          void *const *arg) {
    const struct behavior_parameter_value_metadata *val =
        (const struct behavior_parameter_value_metadata *)*arg;

    if (!val->display_name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, val->display_name, strlen(val->display_name));
}

static bool encode_value_description(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    struct fk_metadata_encode_state *state = (struct fk_metadata_encode_state *)*arg;
    const struct behavior_parameter_metadata_set *set = &state->sets[state->i];

    bool is_param1 = field->tag == cormoran_fast_keymap_BehaviorBindingParametersSet_param1_tag;
    size_t values_len = is_param1 ? set->param1_values_len : set->param2_values_len;
    const struct behavior_parameter_value_metadata *values =
        is_param1 ? set->param1_values : set->param2_values;

    for (size_t val_i = 0; val_i < values_len; val_i++) {
        const struct behavior_parameter_value_metadata *val = &values[val_i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_BehaviorParameterValueDescription desc =
            cormoran_fast_keymap_BehaviorParameterValueDescription_init_zero;
        desc.name.funcs.encode = encode_value_description_name;
        desc.name.arg = (void *)val;

        switch (val->type) {
        case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
            desc.which_value_type =
                cormoran_fast_keymap_BehaviorParameterValueDescription_constant_tag;
            desc.value_type.constant = val->value;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
            desc.which_value_type =
                cormoran_fast_keymap_BehaviorParameterValueDescription_range_tag;
            desc.value_type.range.min = val->range.min;
            desc.value_type.range.max = val->range.max;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
            desc.which_value_type = cormoran_fast_keymap_BehaviorParameterValueDescription_nil_tag;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
            desc.which_value_type =
                cormoran_fast_keymap_BehaviorParameterValueDescription_hid_usage_tag;
            desc.value_type.hid_usage.consumer_max = ZMK_HID_CONSUMER_MAX_USAGE;
            desc.value_type.hid_usage.keyboard_max = ZMK_HID_KEYBOARD_MAX_USAGE;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
            desc.which_value_type =
                cormoran_fast_keymap_BehaviorParameterValueDescription_layer_id_tag;
            break;
        default:
            LOG_ERR("Unknown value description type %d", val->type);
            return false;
        }

        if (!pb_encode_submessage(
                stream, &cormoran_fast_keymap_BehaviorParameterValueDescription_msg, &desc)) {
            return false;
        }
    }

    return true;
}

static bool encode_metadata_sets(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct fk_metadata_encode_state *state = (struct fk_metadata_encode_state *)*arg;

    for (size_t i = 0; i < state->sets_len; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        state->i = i;
        cormoran_fast_keymap_BehaviorBindingParametersSet msg =
            cormoran_fast_keymap_BehaviorBindingParametersSet_init_zero;
        msg.param1.funcs.encode = encode_value_description;
        msg.param1.arg = state;
        msg.param2.funcs.encode = encode_value_description;
        msg.param2.arg = state;

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_BehaviorBindingParametersSet_msg,
                                  &msg)) {
            return false;
        }
    }

    return true;
}

/* Fills a zero-initialized BehaviorDetails message's callback fields for
 * `dev` into `*details`, using `*state` as the metadata-set iteration
 * cursor. `state` must be static/persistent storage: nanopb reads it back
 * later (once for the size pass, once for the encode pass, both after this
 * function returns) via details->metadata.arg. */
static void fill_behavior_details(cormoran_fast_keymap_BehaviorDetails *details,
                                  const struct device *dev, struct fk_metadata_encode_state *state,
                                  bool include_alias, bool exclude_display_name) {
    struct behavior_parameter_metadata desc = {0};
    int ret = behavior_get_parameter_metadata(dev, &desc);
    if (ret < 0) {
        LOG_DBG("Failed to fetch metadata for behavior device %s: %d", dev ? dev->name : "(null)",
                ret);
    }

    state->sets = desc.sets;
    state->sets_len = desc.sets_len;
    state->i = 0;

    *details = (cormoran_fast_keymap_BehaviorDetails)cormoran_fast_keymap_BehaviorDetails_init_zero;
    /* alias is opt-in and display_name opt-out (ListBehaviorsRequest/
     * GetBehaviorsRequest.include_alias / exclude_display_name): leaving a
     * callback unset omits that field on the wire. def_fp still hashes the
     * true alias + display_name regardless -- this only affects the BYTES, not
     * the fingerprint. */
    if (!exclude_display_name) {
        details->display_name.funcs.encode = encode_behavior_display_name;
        details->display_name.arg = (void *)dev;
    }
    if (include_alias) {
        details->alias.funcs.encode = encode_behavior_alias;
        details->alias.arg = (void *)dev;
    }
    details->metadata.funcs.encode = encode_metadata_sets;
    details->metadata.arg = state;
}

/* -- list_behaviors ---------------------------------------------------- */

/* Copied out of the request into static storage before returning: the
 * callback below runs after handle_list_behaviors() returns (SS3's
 * size-pass/encode-pass), so it cannot read from a stack-local mode
 * variable. */
static cormoran_fast_keymap_BehaviorDetailsMode fk_list_behaviors_mode;
static bool fk_list_behaviors_include_alias;
static bool fk_list_behaviors_exclude_display_name;

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    cormoran_fast_keymap_BehaviorDetailsMode mode =
        *(const cormoran_fast_keymap_BehaviorDetailsMode *)*arg;

    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_BehaviorSummary sum = cormoran_fast_keymap_BehaviorSummary_init_zero;
        sum.local_id = beh->local_id;
        sum.def_fp = fk_fp_behavior_def(beh->device);

        /* DESIGN.md SS5: NONE -> no details; CUSTOM -> details only for
         * non-standard behaviors (the client's bundle covers standard
         * ones); ALL -> details on every summary. */
        bool want_details =
            mode == cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_ALL ||
            (mode == cormoran_fast_keymap_BehaviorDetailsMode_BEHAVIOR_DETAILS_CUSTOM &&
             !fk_behavior_is_standard(beh->device));
        if (want_details) {
            sum.has_details = true;
            fill_behavior_details(&sum.details, beh->device, &fk_metadata_state,
                                  fk_list_behaviors_include_alias,
                                  fk_list_behaviors_exclude_display_name);
        }

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_BehaviorSummary_msg, &sum)) {
            return false;
        }
    }

    return true;
}

static int handle_list_behaviors(const cormoran_fast_keymap_ListBehaviorsRequest *req,
                                 cormoran_fast_keymap_Response *resp) {
    fk_list_behaviors_mode = req->details;
    fk_list_behaviors_include_alias = req->include_alias;
    fk_list_behaviors_exclude_display_name = req->exclude_display_name;

    cormoran_fast_keymap_ListBehaviorsResponse *out = &resp->response_type.list_behaviors;
    *out = (cormoran_fast_keymap_ListBehaviorsResponse)
        cormoran_fast_keymap_ListBehaviorsResponse_init_zero;
    out->behaviors.funcs.encode = encode_behavior_summaries;
    out->behaviors.arg = &fk_list_behaviors_mode;

    resp->which_response_type = cormoran_fast_keymap_Response_list_behaviors_tag;
    return 0;
}

/* -- get_behaviors (batch miss-repair, DESIGN.md SS5) ------------------ */

/* GetBehaviorsRequest.behavior_ids is a bounded plain-struct field
 * (.options max_count:32, NOT a callback -- it's a request field decoded
 * from the small RX buffer), so it's safe/cheap to copy the whole struct by
 * value into static storage for the callback below to read after
 * handle_get_behaviors() returns (SS3). */
static cormoran_fast_keymap_GetBehaviorsRequest fk_get_behaviors_req;

static bool encode_behavior_entries(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    const cormoran_fast_keymap_GetBehaviorsRequest *req =
        (const cormoran_fast_keymap_GetBehaviorsRequest *)*arg;

    for (pb_size_t i = 0; i < req->behavior_ids_count; i++) {
        uint32_t id = req->behavior_ids[i];
        const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(id);
        const struct device *dev = behavior_name ? zmk_behavior_get_binding(behavior_name) : NULL;

        if (!dev) {
            /* Unknown id: skip it rather than failing the whole batch, so a
             * client's request mostly succeeds even if one id in it (e.g. a
             * stale cache entry) no longer resolves. */
            LOG_WRN("get_behaviors: no behavior for id %u, skipping", (unsigned int)id);
            continue;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_BehaviorEntry entry = cormoran_fast_keymap_BehaviorEntry_init_zero;
        entry.id = id;
        entry.has_details = true;
        fill_behavior_details(&entry.details, dev, &fk_metadata_state, req->include_alias,
                              req->exclude_display_name);

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_BehaviorEntry_msg, &entry)) {
            return false;
        }
    }

    return true;
}

static int handle_get_behaviors(const cormoran_fast_keymap_GetBehaviorsRequest *req,
                                cormoran_fast_keymap_Response *resp) {
    fk_get_behaviors_req = *req;

    cormoran_fast_keymap_GetBehaviorsResponse *out = &resp->response_type.get_behaviors;
    *out = (cormoran_fast_keymap_GetBehaviorsResponse)
        cormoran_fast_keymap_GetBehaviorsResponse_init_zero;
    out->behaviors.funcs.encode = encode_behavior_entries;
    out->behaviors.arg = &fk_get_behaviors_req;

    resp->which_response_type = cormoran_fast_keymap_Response_get_behaviors_tag;
    return 0;
}

/* -- get_layer / get_default_layer / get_edited_layer ----------------- */

struct fk_layer_encode_arg {
    zmk_keymap_layer_id_t layer_id;
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
    bool use_default;
#endif
};

static struct fk_layer_encode_arg fk_layer_arg;

static bool encode_layer_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    uint8_t layer_id = *(uint8_t *)*arg;
    const char *name = zmk_keymap_layer_name(layer_id);

    if (!name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, name, strlen(name));
}

static void encode_binding_fields(cormoran_fast_keymap_BehaviorBinding *bb,
                                  const struct zmk_behavior_binding *binding) {
    *bb = (cormoran_fast_keymap_BehaviorBinding)cormoran_fast_keymap_BehaviorBinding_init_zero;
    if (binding && binding->behavior_dev) {
        bb->behavior_id = zmk_behavior_get_local_id(binding->behavior_dev);
        bb->param1 = binding->param1;
        bb->param2 = binding->param2;
    } else {
        bb->behavior_id = -1;
    }
}

static bool encode_layer_bindings(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct fk_layer_encode_arg *a = (const struct fk_layer_encode_arg *)*arg;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        const struct zmk_behavior_binding *binding;
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
        binding = a->use_default ? zmk_stock_keymap_get_layer_binding_at_idx(a->layer_id, b)
                                 : zmk_keymap_get_layer_binding_at_idx(a->layer_id, b);
#else
        binding = zmk_keymap_get_layer_binding_at_idx(a->layer_id, b);
#endif

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_BehaviorBinding bb;
        encode_binding_fields(&bb, binding);

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_BehaviorBinding_msg, &bb)) {
            return false;
        }
    }

    return true;
}

static bool layer_id_valid(uint32_t layer_id) { return layer_id < ZMK_KEYMAP_LAYERS_LEN; }

static int handle_get_layer_common(uint32_t layer_id, bool want_default,
                                   cormoran_fast_keymap_Response *resp) {
    if (!layer_id_valid(layer_id)) {
        set_error(resp, cormoran_fast_keymap_ErrorCode_ERR_INVALID_LAYER);
        return 0;
    }

    fk_layer_arg.layer_id = (zmk_keymap_layer_id_t)layer_id;
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
    fk_layer_arg.use_default = want_default;
#else
    ARG_UNUSED(want_default);
#endif

    cormoran_fast_keymap_Layer *out =
        want_default ? &resp->response_type.get_default_layer : &resp->response_type.get_layer;
    *out = (cormoran_fast_keymap_Layer)cormoran_fast_keymap_Layer_init_zero;
    out->id = layer_id;
    out->name.funcs.encode = encode_layer_name;
    out->name.arg = &fk_layer_arg.layer_id;
    out->bindings.funcs.encode = encode_layer_bindings;
    out->bindings.arg = &fk_layer_arg;

    resp->which_response_type = want_default ? cormoran_fast_keymap_Response_get_default_layer_tag
                                             : cormoran_fast_keymap_Response_get_layer_tag;
    return 0;
}

static bool encode_edited_bindings(pb_ostream_t *stream, const pb_field_t *field,
                                   void *const *arg) {
    const struct fk_layer_encode_arg *a = (const struct fk_layer_encode_arg *)*arg;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        if (!fk_binding_is_edited(a->layer_id, b)) {
            continue;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        const struct zmk_behavior_binding *binding =
            zmk_keymap_get_layer_binding_at_idx(a->layer_id, b);

        cormoran_fast_keymap_EditedBinding eb = cormoran_fast_keymap_EditedBinding_init_zero;
        eb.position = b;
        eb.has_binding = true;
        encode_binding_fields(&eb.binding, binding);

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_EditedBinding_msg, &eb)) {
            return false;
        }
    }

    return true;
}

static int handle_get_edited_layer(uint32_t layer_id, cormoran_fast_keymap_Response *resp) {
    if (!layer_id_valid(layer_id)) {
        set_error(resp, cormoran_fast_keymap_ErrorCode_ERR_INVALID_LAYER);
        return 0;
    }

    fk_layer_arg.layer_id = (zmk_keymap_layer_id_t)layer_id;

    cormoran_fast_keymap_EditedLayer *out = &resp->response_type.get_edited_layer;
    *out = (cormoran_fast_keymap_EditedLayer)cormoran_fast_keymap_EditedLayer_init_zero;
    out->id = layer_id;
    out->bindings.funcs.encode = encode_edited_bindings;
    out->bindings.arg = &fk_layer_arg;

    resp->which_response_type = cormoran_fast_keymap_Response_get_edited_layer_tag;
    return 0;
}

/* -- get_layers (batch current/effective, DESIGN.md SS7) --------------- */

/* Copied out of the request into static storage before returning, same
 * reason as fk_get_behaviors_req: encode_layers() runs after
 * handle_get_layers() returns (SS3's size-pass/encode-pass). */
static cormoran_fast_keymap_GetLayersRequest fk_get_layers_req;

/* One persistent fk_layer_encode_arg per requested layer, so each Layer
 * submessage's bindings callback reads the right layer_id across both encode
 * passes (nanopb keeps the arg pointer, like fk_snapshot_layers). */
static struct fk_layer_encode_arg fk_get_layers_args[ARRAY_SIZE(fk_get_layers_req.layer_ids)];

static bool encode_layers(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const cormoran_fast_keymap_GetLayersRequest *req =
        (const cormoran_fast_keymap_GetLayersRequest *)*arg;

    for (pb_size_t i = 0; i < req->layer_ids_count; i++) {
        uint32_t layer_id = req->layer_ids[i];
        if (!layer_id_valid(layer_id)) {
            /* Skip unknown/invalid ids rather than failing the whole batch,
             * same as get_behaviors. */
            LOG_WRN("get_layers: invalid layer_id %u, skipping", (unsigned int)layer_id);
            continue;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        fk_get_layers_args[i].layer_id = (zmk_keymap_layer_id_t)layer_id;
#if IS_ENABLED(CONFIG_ZMK_FAST_KEYMAP_DEFAULT_LAYER)
        fk_get_layers_args[i].use_default = false; /* effective/current bindings */
#endif

        cormoran_fast_keymap_Layer lyr = cormoran_fast_keymap_Layer_init_zero;
        lyr.id = layer_id;
        lyr.name.funcs.encode = encode_layer_name;
        lyr.name.arg = &fk_get_layers_args[i].layer_id;
        lyr.bindings.funcs.encode = encode_layer_bindings;
        lyr.bindings.arg = &fk_get_layers_args[i];

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_Layer_msg, &lyr)) {
            return false;
        }
    }

    return true;
}

static int handle_get_layers(const cormoran_fast_keymap_GetLayersRequest *req,
                             cormoran_fast_keymap_Response *resp) {
    fk_get_layers_req = *req;

    cormoran_fast_keymap_GetLayersResponse *out = &resp->response_type.get_layers;
    *out = (cormoran_fast_keymap_GetLayersResponse)cormoran_fast_keymap_GetLayersResponse_init_zero;
    out->layers.funcs.encode = encode_layers;
    out->layers.arg = &fk_get_layers_req;

    resp->which_response_type = cormoran_fast_keymap_Response_get_layers_tag;
    return 0;
}

/* -- get_pending (all layers' unsaved positions) ---------------------- */

static bool fk_binding_is_pending(zmk_keymap_layer_id_t layer_id, uint16_t binding_idx) {
    return zmk_keymap_layer_binding_at_idx_is_pending(layer_id, binding_idx) == 1;
}

static bool fk_layer_has_pending(zmk_keymap_layer_id_t layer_id) {
    uint16_t binding_count = fk_binding_count();
    for (uint16_t b = 0; b < binding_count; b++) {
        if (fk_binding_is_pending(layer_id, b)) {
            return true;
        }
    }
    return false;
}

/* One persistent layer id per layer slot, so each PendingLayer submessage's
 * positions callback reads the right layer across both encode passes (nanopb
 * keeps the arg pointer, like fk_get_layers_args). */
static zmk_keymap_layer_id_t fk_pending_layer_ids[ZMK_KEYMAP_LAYERS_LEN];

static bool encode_pending_positions(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    zmk_keymap_layer_id_t layer_id = *(zmk_keymap_layer_id_t *)*arg;
    uint16_t binding_count = fk_binding_count();

    for (uint16_t b = 0; b < binding_count; b++) {
        if (!fk_binding_is_pending(layer_id, b)) {
            continue;
        }

        /* Unpacked repeated uint32 (tag per element): valid proto3, decoders
         * accept it as well as the packed form. */
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }
        if (!pb_encode_varint(stream, b)) {
            return false;
        }
    }

    return true;
}

static bool encode_pending_layers(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);
    uint8_t layer_count = fk_layer_count();

    for (uint8_t idx = 0; idx < layer_count && idx < ARRAY_SIZE(fk_pending_layer_ids); idx++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(idx);

        if (!fk_layer_has_pending(id)) {
            continue;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        fk_pending_layer_ids[idx] = id;

        cormoran_fast_keymap_PendingLayer pl = cormoran_fast_keymap_PendingLayer_init_zero;
        pl.id = id;
        pl.positions.funcs.encode = encode_pending_positions;
        pl.positions.arg = &fk_pending_layer_ids[idx];

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_PendingLayer_msg, &pl)) {
            return false;
        }
    }

    return true;
}

static int handle_get_pending(cormoran_fast_keymap_Response *resp) {
    cormoran_fast_keymap_PendingResponse *out = &resp->response_type.get_pending;
    *out = (cormoran_fast_keymap_PendingResponse)cormoran_fast_keymap_PendingResponse_init_zero;
    out->layers.funcs.encode = encode_pending_layers;

    resp->which_response_type = cormoran_fast_keymap_Response_get_pending_tag;
    return 0;
}

/* -- get_physical_layouts ------------------------------------------- */

static bool encode_layout_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_physical_layout *layout = (const struct zmk_physical_layout *)*arg;

    if (!layout->display_name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, layout->display_name, strlen(layout->display_name));
}

/* The width/height shared by most keys (see PhysicalLayoutSummary.default_*
 * in the proto): the first key's size, which is the common case on any
 * uniform board. Keys matching it encode width/height as 0 (omitted) and the
 * client fills them back in from these defaults. */
static void fk_layout_default_size(const struct zmk_physical_layout *l, int32_t *dw, int32_t *dh) {
    if (l->keys_len > 0) {
        *dw = l->keys[0].width;
        *dh = l->keys[0].height;
    } else {
        *dw = 0;
        *dh = 0;
    }
}

static bool encode_layout_keys(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_physical_layout *layout = (const struct zmk_physical_layout *)*arg;

    int32_t dw, dh;
    fk_layout_default_size(layout, &dw, &dh);

    for (size_t kp = 0; kp < layout->keys_len; kp++) {
        const struct zmk_key_physical_attrs *k = &layout->keys[kp];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        /* Omit width/height (encode 0) when they match the layout default;
         * the client restores them from PhysicalLayoutSummary.default_*. */
        cormoran_fast_keymap_KeyPhysicalAttrs msg = {
            .width = k->width == dw ? 0 : k->width,
            .height = k->height == dh ? 0 : k->height,
            .x = k->x,
            .y = k->y,
#if IS_ENABLED(CONFIG_ZMK_PHYSICAL_LAYOUT_KEY_ROTATION)
            .r = k->r,
            .rx = k->rx,
            .ry = k->ry,
#endif
        };

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_KeyPhysicalAttrs_msg, &msg)) {
            return false;
        }
    }

    return true;
}

/* Copied out of the request into static storage before returning, same
 * reason as fk_list_behaviors_mode above: encode_physical_layout_summaries()
 * runs after handle_get_physical_layouts() returns (SS3's size-pass/encode-
 * pass), so it cannot read from a stack-local mode variable. */
static cormoran_fast_keymap_LayoutDetailsMode fk_physical_layouts_mode;

static bool encode_physical_layout_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                             void *const *arg) {
    cormoran_fast_keymap_LayoutDetailsMode mode =
        *(const cormoran_fast_keymap_LayoutDetailsMode *)*arg;

    struct zmk_physical_layout const *const *layouts;
    size_t layout_count = zmk_physical_layouts_get_list(&layouts);

    for (size_t i = 0; i < layout_count; i++) {
        const struct zmk_physical_layout *l = layouts[i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_PhysicalLayoutSummary summary =
            cormoran_fast_keymap_PhysicalLayoutSummary_init_zero;
        summary.name.funcs.encode = encode_layout_name;
        summary.name.arg = (void *)l;
        summary.fp = fk_fp_physical_layout_one(l);

        /* DESIGN.md SS6: NONE -> summary only (name + fp, keys omitted for
         * free by proto3); ALL -> inline every layout's geometry (plus the
         * default_width/height the per-key omission relies on). */
        if (mode == cormoran_fast_keymap_LayoutDetailsMode_LAYOUT_DETAILS_ALL) {
            summary.keys.funcs.encode = encode_layout_keys;
            summary.keys.arg = (void *)l;
            fk_layout_default_size(l, &summary.default_width, &summary.default_height);
        }

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_PhysicalLayoutSummary_msg,
                                  &summary)) {
            return false;
        }
    }

    return true;
}

static int handle_get_physical_layouts(cormoran_fast_keymap_LayoutDetailsMode mode,
                                       cormoran_fast_keymap_Response *resp) {
    fk_physical_layouts_mode = mode;

    cormoran_fast_keymap_PhysicalLayoutsResponse *out = &resp->response_type.get_physical_layouts;
    *out = (cormoran_fast_keymap_PhysicalLayoutsResponse)
        cormoran_fast_keymap_PhysicalLayoutsResponse_init_zero;
    out->active_layout_index = zmk_physical_layouts_get_selected();
    out->layouts.funcs.encode = encode_physical_layout_summaries;
    out->layouts.arg = &fk_physical_layouts_mode;

    resp->which_response_type = cormoran_fast_keymap_Response_get_physical_layouts_tag;
    return 0;
}

/* -- get_physical_layout (single layout, geometry always populated) ---- */

/* The pointer target is a static/DT-generated struct owned by ZMK's physical
 * layout list (see zmk_physical_layouts_get_list()), so it stays valid
 * across both encode passes without needing to copy anything -- unlike
 * fk_layer_arg/fk_get_behaviors_req above, this is just a pointer, not
 * request data that must be preserved. */
static const struct zmk_physical_layout *fk_selected_physical_layout;

static int handle_get_physical_layout(uint32_t layout_index, cormoran_fast_keymap_Response *resp) {
    struct zmk_physical_layout const *const *layouts;
    size_t layout_count = zmk_physical_layouts_get_list(&layouts);

    if (layout_index >= layout_count) {
        set_error(resp, cormoran_fast_keymap_ErrorCode_ERR_INVALID_LAYOUT);
        return 0;
    }

    fk_selected_physical_layout = layouts[layout_index];

    cormoran_fast_keymap_PhysicalLayoutResponse *out = &resp->response_type.get_physical_layout;
    *out = (cormoran_fast_keymap_PhysicalLayoutResponse)
        cormoran_fast_keymap_PhysicalLayoutResponse_init_zero;
    out->index = layout_index;
    out->has_layout = true;
    out->layout.name.funcs.encode = encode_layout_name;
    out->layout.name.arg = (void *)fk_selected_physical_layout;
    out->layout.fp = fk_fp_physical_layout_one(fk_selected_physical_layout);
    out->layout.keys.funcs.encode = encode_layout_keys;
    out->layout.keys.arg = (void *)fk_selected_physical_layout;
    fk_layout_default_size(fk_selected_physical_layout, &out->layout.default_width,
                           &out->layout.default_height);

    resp->which_response_type = cormoran_fast_keymap_Response_get_physical_layout_tag;
    return 0;
}

/* -- dispatch ---------------------------------------------------------- */

static bool fast_keymap_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                           pb_callback_t *encode_response) {
    cormoran_fast_keymap_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__fast_keymap, encode_response);

    cormoran_fast_keymap_Request req = cormoran_fast_keymap_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_fast_keymap_Request_fields, &req)) {
        LOG_WRN("Failed to decode fast_keymap request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, cormoran_fast_keymap_ErrorCode_ERR_GENERIC);
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case cormoran_fast_keymap_Request_get_snapshot_tag:
        rc = handle_get_snapshot(resp);
        break;
    case cormoran_fast_keymap_Request_list_behaviors_tag:
        rc = handle_list_behaviors(&req.request_type.list_behaviors, resp);
        break;
    case cormoran_fast_keymap_Request_get_behaviors_tag:
        rc = handle_get_behaviors(&req.request_type.get_behaviors, resp);
        break;
    case cormoran_fast_keymap_Request_get_layer_tag:
        rc = handle_get_layer_common(req.request_type.get_layer.layer_id, false, resp);
        break;
    case cormoran_fast_keymap_Request_get_default_layer_tag:
        rc = handle_get_layer_common(req.request_type.get_default_layer.layer_id, true, resp);
        break;
    case cormoran_fast_keymap_Request_get_edited_layer_tag:
        rc = handle_get_edited_layer(req.request_type.get_edited_layer.layer_id, resp);
        break;
    case cormoran_fast_keymap_Request_get_layers_tag:
        rc = handle_get_layers(&req.request_type.get_layers, resp);
        break;
    case cormoran_fast_keymap_Request_get_pending_tag:
        rc = handle_get_pending(resp);
        break;
    case cormoran_fast_keymap_Request_get_physical_layouts_tag:
        rc = handle_get_physical_layouts(req.request_type.get_physical_layouts.details, resp);
        break;
    case cormoran_fast_keymap_Request_get_physical_layout_tag:
        rc = handle_get_physical_layout(req.request_type.get_physical_layout.layout_index, resp);
        break;
    default:
        LOG_WRN("Unsupported fast_keymap request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        set_error(resp, cormoran_fast_keymap_ErrorCode_ERR_GENERIC);
    }
    return true;
}
