/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <cormoran/devtool/devtool.pb.h>

/* Shared ErrorResponse setter, defined in devtool_handler.c. */
void devtool_set_error(cormoran_devtool_Response *resp, const char *message);

/*
 * Shared cursor math for the event tap and log capture ring buffers: both
 * are append-only, fixed-capacity, monotonically-numbered record streams
 * drained by a `get_*(cursor)` RPC. Given how many records have ever been
 * written (total_written) and the capacity of the backing ring, compute the
 * window [*out_start, *out_start + *out_count) to return, clamping
 * requested_cursor forward if the caller fell behind and got overwritten
 * records (reported via *out_dropped) and capping the batch at max_batch to
 * fit the RPC TX buffer.
 *
 * total_written can go backwards relative to a cursor a caller already holds
 * (clear_events/clear_logs resets it to 0 without changing the caller's
 * cursor). Clamp start to total_written too, not just to oldest_available,
 * so `total_written - start` below can never underflow.
 */
static inline void devtool_ring_window(uint32_t requested_cursor, uint32_t total_written,
                                       uint32_t capacity, uint32_t max_batch, uint32_t *out_start,
                                       uint32_t *out_count, uint32_t *out_dropped) {
    uint32_t oldest_available = total_written > capacity ? total_written - capacity : 0;
    uint32_t start = requested_cursor < oldest_available ? oldest_available : requested_cursor;
    if (start > total_written) {
        start = total_written;
    }

    /* Saturating: start can end up below requested_cursor (not just above) when
     * total_written moved backwards, e.g. requested_cursor from before a clear. */
    *out_dropped = start > requested_cursor ? start - requested_cursor : 0;
    uint32_t available = total_written - start;
    *out_count = available < max_batch ? available : max_batch;
    *out_start = start;
}

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LAYER_STATE)
int devtool_handle_get_layer_state(cormoran_devtool_Response *resp);
int devtool_handle_set_layer_state(const cormoran_devtool_SetLayerStateRequest *req,
                                   cormoran_devtool_Response *resp);
int devtool_handle_toggle_layer(const cormoran_devtool_ToggleLayerRequest *req,
                                cormoran_devtool_Response *resp);
#endif

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_KEY_INJECTION)
int devtool_handle_inject_key(const cormoran_devtool_InjectKeyRequest *req,
                              cormoran_devtool_Response *resp);
int devtool_handle_tap_key(const cormoran_devtool_TapKeyRequest *req,
                           cormoran_devtool_Response *resp);
#endif

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_EVENT_TAP)
int devtool_handle_subscribe_events(const cormoran_devtool_SubscribeEventsRequest *req,
                                    cormoran_devtool_Response *resp);
int devtool_handle_get_events(const cormoran_devtool_GetEventsRequest *req,
                              cormoran_devtool_Response *resp);
int devtool_handle_clear_events(cormoran_devtool_Response *resp);
#endif

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE)
int devtool_handle_get_logs(const cormoran_devtool_GetLogsRequest *req,
                            cormoran_devtool_Response *resp);
int devtool_handle_clear_logs(cormoran_devtool_Response *resp);
int devtool_handle_set_log_capture_filter(const cormoran_devtool_SetLogCaptureFilterRequest *req,
                                          cormoran_devtool_Response *resp);
#endif

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_STACK_USAGE)
int devtool_handle_get_stack_usage(const cormoran_devtool_GetStackUsageRequest *req,
                                   cormoran_devtool_Response *resp);
#endif

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING)
int devtool_handle_set_log_streaming(const cormoran_devtool_SetLogStreamingRequest *req,
                                     cormoran_devtool_Response *resp);

/*
 * Index of this module's custom subsystem within the zmk_rpc_custom_subsystem
 * iterable section, needed to address custom notifications back to the client.
 * Resolved once and cached; defined in devtool_handler.c where the subsystem
 * registration is visible.
 */
uint8_t devtool_custom_subsystem_index(void);
#endif
