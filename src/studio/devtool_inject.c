/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/events/position_state_changed.h>
#include <cormoran/devtool/devtool.pb.h>

#include "devtool_internal.h"

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_KEY_INJECTION)

static void inject_position(uint32_t position, bool pressed) {
    raise_zmk_position_state_changed(
        (struct zmk_position_state_changed){.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
                                            .position = position,
                                            .state = pressed,
                                            .timestamp = k_uptime_get()});
}

int devtool_handle_inject_key(const cormoran_devtool_InjectKeyRequest *req,
                              cormoran_devtool_Response *resp) {
    inject_position(req->position, req->pressed);

    cormoran_devtool_InjectKeyResponse result = cormoran_devtool_InjectKeyResponse_init_zero;
    resp->which_response_type = cormoran_devtool_Response_inject_key_tag;
    resp->response_type.inject_key = result;
    return 0;
}

/* Only one tap can be pending at a time -- see TapKeyRequest in devtool.proto. */
static bool tap_pending;
static uint32_t pending_tap_position;

static void tap_release_work_handler(struct k_work *work) {
    ARG_UNUSED(work);
    inject_position(pending_tap_position, false);
    tap_pending = false;
}

K_WORK_DELAYABLE_DEFINE(devtool_tap_release_work, tap_release_work_handler);

int devtool_handle_tap_key(const cormoran_devtool_TapKeyRequest *req,
                           cormoran_devtool_Response *resp) {
    if (tap_pending) {
        devtool_set_error(resp, "A tap_key request is already in flight");
        return -EBUSY;
    }

    tap_pending = true;
    pending_tap_position = req->position;
    inject_position(req->position, true);
    k_work_schedule(&devtool_tap_release_work, K_MSEC(req->hold_ms));

    cormoran_devtool_TapKeyResponse result = cormoran_devtool_TapKeyResponse_init_zero;
    resp->which_response_type = cormoran_devtool_Response_tap_key_tag;
    resp->response_type.tap_key = result;
    return 0;
}

#endif /* CONFIG_ZMK_DEVTOOL_KEY_INJECTION */
