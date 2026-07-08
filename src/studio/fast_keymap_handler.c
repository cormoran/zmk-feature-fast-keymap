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
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-fast-keymap/"),
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
    compute_snapshot_layers();

    cormoran_fast_keymap_Snapshot *snap = &resp->response_type.snapshot;
    *snap = (cormoran_fast_keymap_Snapshot)cormoran_fast_keymap_Snapshot_init_zero;

    snap->definition_version = FK_DEFINITION_VERSION;
    snap->behaviors_fp = fk_fp_behaviors();
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

/* -- list_behaviors ---------------------------------------------------- */

static bool encode_behavior_alias(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct device *dev = (const struct device *)*arg;
    const char *alias = fk_behavior_alias(dev);

    if (!alias) {
        /* Best-effort fallback: report the device name (DESIGN.md SS2.1). */
        alias = dev ? dev->name : NULL;
    }
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

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    ARG_UNUSED(arg);

    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_BehaviorSummary sum = cormoran_fast_keymap_BehaviorSummary_init_zero;
        sum.local_id = beh->local_id;
        sum.alias.funcs.encode = encode_behavior_alias;
        sum.alias.arg = (void *)beh->device;
        sum.display_name.funcs.encode = encode_behavior_display_name;
        sum.display_name.arg = (void *)beh->device;
        sum.metadata_fp = fk_fp_behavior_metadata(beh->device);
        sum.standard = fk_behavior_is_standard(beh->device);

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_BehaviorSummary_msg, &sum)) {
            return false;
        }
    }

    return true;
}

static int handle_list_behaviors(cormoran_fast_keymap_Response *resp) {
    cormoran_fast_keymap_ListBehaviorsResponse *out = &resp->response_type.list_behaviors;
    *out = (cormoran_fast_keymap_ListBehaviorsResponse)
        cormoran_fast_keymap_ListBehaviorsResponse_init_zero;
    out->behaviors.funcs.encode = encode_behavior_summaries;

    resp->which_response_type = cormoran_fast_keymap_Response_list_behaviors_tag;
    return 0;
}

/* -- get_behavior_details ------------------------------------------- */

struct fk_metadata_encode_state {
    const struct behavior_parameter_metadata_set *sets;
    size_t sets_len;
    size_t i;
};

static struct fk_metadata_encode_state fk_metadata_state;

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

static int handle_get_behavior_details(uint32_t behavior_id, cormoran_fast_keymap_Response *resp) {
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);

    if (!behavior_name) {
        LOG_WRN("No behavior found for ID %d", behavior_id);
        set_error(resp, cormoran_fast_keymap_ErrorCode_ERR_INVALID_BEHAVIOR);
        return 0;
    }

    const struct device *dev = zmk_behavior_get_binding(behavior_name);

    struct behavior_parameter_metadata desc = {0};
    int ret = behavior_get_parameter_metadata(dev, &desc);
    if (ret < 0) {
        LOG_DBG("Failed to fetch metadata for behavior %d: %d", behavior_id, ret);
    }

    fk_metadata_state.sets = desc.sets;
    fk_metadata_state.sets_len = desc.sets_len;

    cormoran_fast_keymap_GetBehaviorDetailsResponse *out =
        &resp->response_type.get_behavior_details;
    *out = (cormoran_fast_keymap_GetBehaviorDetailsResponse)
        cormoran_fast_keymap_GetBehaviorDetailsResponse_init_zero;
    out->id = behavior_id;
    out->display_name.funcs.encode = encode_behavior_display_name;
    out->display_name.arg = (void *)dev;
    out->alias.funcs.encode = encode_behavior_alias;
    out->alias.arg = (void *)dev;
    out->metadata_fp = fk_fp_behavior_metadata(dev);
    out->metadata.funcs.encode = encode_metadata_sets;
    out->metadata.arg = &fk_metadata_state;

    resp->which_response_type = cormoran_fast_keymap_Response_get_behavior_details_tag;
    return 0;
}

/* -- get_layer / get_default_layer / get_edited_layer ----------------- */

struct fk_layer_encode_arg {
    zmk_keymap_layer_id_t layer_id;
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
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
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
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
#if IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE)
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

static bool encode_layout_keys(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const struct zmk_physical_layout *layout = (const struct zmk_physical_layout *)*arg;

    for (size_t kp = 0; kp < layout->keys_len; kp++) {
        const struct zmk_key_physical_attrs *k = &layout->keys[kp];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_KeyPhysicalAttrs msg = {
            .width = k->width,
            .height = k->height,
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

static bool encode_layouts(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    struct zmk_physical_layout const *const *layouts;
    size_t layout_count = zmk_physical_layouts_get_list(&layouts);

    for (size_t i = 0; i < layout_count; i++) {
        const struct zmk_physical_layout *l = layouts[i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        cormoran_fast_keymap_PhysicalLayout layout = cormoran_fast_keymap_PhysicalLayout_init_zero;
        layout.name.funcs.encode = encode_layout_name;
        layout.name.arg = (void *)l;
        layout.keys.funcs.encode = encode_layout_keys;
        layout.keys.arg = (void *)l;

        if (!pb_encode_submessage(stream, &cormoran_fast_keymap_PhysicalLayout_msg, &layout)) {
            return false;
        }
    }

    return true;
}

static int handle_get_physical_layouts(cormoran_fast_keymap_Response *resp) {
    cormoran_fast_keymap_PhysicalLayouts *out = &resp->response_type.get_physical_layouts;
    *out = (cormoran_fast_keymap_PhysicalLayouts)cormoran_fast_keymap_PhysicalLayouts_init_zero;
    out->active_layout_index = zmk_physical_layouts_get_selected();
    out->layouts.funcs.encode = encode_layouts;

    resp->which_response_type = cormoran_fast_keymap_Response_get_physical_layouts_tag;
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
        rc = handle_list_behaviors(resp);
        break;
    case cormoran_fast_keymap_Request_get_behavior_details_tag:
        rc = handle_get_behavior_details(req.request_type.get_behavior_details.behavior_id, resp);
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
    case cormoran_fast_keymap_Request_get_physical_layouts_tag:
        rc = handle_get_physical_layouts(resp);
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
