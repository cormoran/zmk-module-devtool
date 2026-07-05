/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/keymap.h>
#include <cormoran/devtool/devtool.pb.h>

#include "devtool_internal.h"

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LAYER_STATE)

/*
 * zmk_keymap_layer_state() is the raw activation bitmask: the default layer
 * is never actually set in it. ZMK instead treats it as always-active via a
 * fallback check in zmk_keymap_layer_active_with_state() ("|| layer ==
 * _zmk_keymap_layer_default"). Reporting the raw bitmask here would make the
 * default layer look inactive, so OR its bit in to match what
 * zmk_keymap_layer_active() actually returns for every layer.
 */
static uint32_t effective_active_layers(void) {
    return zmk_keymap_layer_state() | BIT(zmk_keymap_layer_default());
}

int devtool_handle_get_layer_state(cormoran_devtool_Response *resp) {
    cormoran_devtool_GetLayerStateResponse result =
        cormoran_devtool_GetLayerStateResponse_init_zero;
    result.active_layers = effective_active_layers();
    result.highest_active_layer = zmk_keymap_highest_layer_active();

    resp->which_response_type = cormoran_devtool_Response_get_layer_state_tag;
    resp->response_type.get_layer_state = result;
    return 0;
}

int devtool_handle_set_layer_state(const cormoran_devtool_SetLayerStateRequest *req,
                                   cormoran_devtool_Response *resp) {
    if (req->layer >= UINT8_MAX) {
        devtool_set_error(resp, "Invalid layer");
        return -EINVAL;
    }

    int ret = req->active
                  ? zmk_keymap_layer_activate((zmk_keymap_layer_id_t)req->layer, req->locking)
                  : zmk_keymap_layer_deactivate((zmk_keymap_layer_id_t)req->layer, req->locking);
    if (ret < 0) {
        devtool_set_error(resp, "Failed to set layer state");
        return ret;
    }

    cormoran_devtool_SetLayerStateResponse result =
        cormoran_devtool_SetLayerStateResponse_init_zero;
    result.active_layers = effective_active_layers();

    resp->which_response_type = cormoran_devtool_Response_set_layer_state_tag;
    resp->response_type.set_layer_state = result;
    return 0;
}

int devtool_handle_toggle_layer(const cormoran_devtool_ToggleLayerRequest *req,
                                cormoran_devtool_Response *resp) {
    if (req->layer >= UINT8_MAX) {
        devtool_set_error(resp, "Invalid layer");
        return -EINVAL;
    }

    int ret = zmk_keymap_layer_toggle((zmk_keymap_layer_id_t)req->layer, req->locking);
    if (ret < 0) {
        devtool_set_error(resp, "Failed to toggle layer state");
        return ret;
    }

    cormoran_devtool_ToggleLayerResponse result = cormoran_devtool_ToggleLayerResponse_init_zero;
    result.active_layers = effective_active_layers();

    resp->which_response_type = cormoran_devtool_Response_toggle_layer_tag;
    resp->response_type.toggle_layer = result;
    return 0;
}

#endif /* CONFIG_ZMK_DEVTOOL_LAYER_STATE */
