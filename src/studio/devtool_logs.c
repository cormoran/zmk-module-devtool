/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log_msg.h>
#include <zephyr/logging/log_output.h>

#include <cormoran/devtool/devtool.pb.h>

#include "devtool_internal.h"

/*
 * Deliberately no LOG_* calls anywhere in this file (see design principle in
 * docs/feature-ideas.md #1): this backend's own process() runs on every log
 * message including ones it might emit itself, so logging here would be a
 * direct self-feedback loop. The primary defense is
 * CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_DEFAULT_LEVEL defaulting to INF, below the
 * Studio RPC dispatch/transport's DBG-level chatter -- this file having zero
 * log calls makes that a certainty rather than a convention for at least this
 * one source.
 */

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE)

#define LOG_RING_CAPACITY CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_BUFFER_SIZE

/*
 * ~77 bytes/record (timestamp + level + source[16] + message[48], each with
 * tag/len overhead) * the 3 records/response cap from devtool.options, plus
 * next_cursor/dropped_count and CallResponse wrapper overhead.
 */
#define DEVTOOL_LOGS_RESPONSE_ESTIMATED_MAX_SIZE 260
BUILD_ASSERT(DEVTOOL_LOGS_RESPONSE_ESTIMATED_MAX_SIZE + 64 <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
             "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE too small for GetLogsResponse; see "
             "devtool.options' records max_count comment");

static cormoran_devtool_LogRecord log_ring[LOG_RING_CAPACITY];
static uint32_t log_ring_total_written;
static struct k_spinlock log_ring_lock;

/* Built up by devtool_log_backend_process()/char_out() below. The logging
 * subsystem processes messages one at a time on a single thread (deferred
 * mode) or inline at the emitting call site (immediate mode), never
 * concurrently, so a single scratch record is safe without its own lock;
 * only the final push into log_ring needs one, to guard against a concurrent
 * get_logs/clear_logs RPC call. */
static cormoran_devtool_LogRecord pending_record;
static size_t pending_message_len;

static int char_out(uint8_t *data, size_t length, void *ctx) {
    ARG_UNUSED(ctx);

    size_t capacity = sizeof(pending_record.message) - 1;
    size_t copy_len = length;
    if (pending_message_len + copy_len > capacity) {
        copy_len = capacity - pending_message_len;
    }
    if (copy_len > 0) {
        memcpy(&pending_record.message[pending_message_len], data, copy_len);
        pending_message_len += copy_len;
    }
    return length;
}

#define LOG_OUTPUT_SCRATCH_SIZE 32
static uint8_t log_output_scratch_buf[LOG_OUTPUT_SCRATCH_SIZE];
LOG_OUTPUT_DEFINE(devtool_log_output, char_out, log_output_scratch_buf,
                  sizeof(log_output_scratch_buf));

static cormoran_devtool_LogLevel to_proto_level(uint8_t zephyr_level) {
    switch (zephyr_level) {
    case LOG_LEVEL_ERR:
        return cormoran_devtool_LogLevel_LOG_LEVEL_ERR;
    case LOG_LEVEL_WRN:
        return cormoran_devtool_LogLevel_LOG_LEVEL_WRN;
    case LOG_LEVEL_INF:
        return cormoran_devtool_LogLevel_LOG_LEVEL_INF;
    case LOG_LEVEL_DBG:
        return cormoran_devtool_LogLevel_LOG_LEVEL_DBG;
    default:
        return cormoran_devtool_LogLevel_LOG_LEVEL_UNSPECIFIED;
    }
}

static void devtool_log_backend_process(const struct log_backend *const backend,
                                        union log_msg_generic *msg) {
    ARG_UNUSED(backend);
    struct log_msg *log_msg = &msg->log;

    pending_record = (cormoran_devtool_LogRecord)cormoran_devtool_LogRecord_init_zero;
    pending_message_len = 0;

    pending_record.timestamp_ms = (uint32_t)k_uptime_get();
    pending_record.level = to_proto_level(log_msg_get_level(log_msg));

    int16_t source_id = log_msg_get_source_id(log_msg);
    const char *source_name = source_id >= 0 ? log_source_name_get(0, (uint32_t)source_id) : NULL;
    if (source_name) {
        strncpy(pending_record.source, source_name, sizeof(pending_record.source) - 1);
    }

    /* SKIP_SOURCE + CRLF_NONE, no TIMESTAMP/LEVEL/COLORS: this yields just the
     * formatted (args-substituted) message body, no "source: " prefix or
     * trailing newline -- source/level/timestamp are already captured above
     * from the message's own fields. */
    log_output_msg_process(&devtool_log_output, log_msg,
                           LOG_OUTPUT_FLAG_SKIP_SOURCE | LOG_OUTPUT_FLAG_CRLF_NONE);
    pending_record.message[pending_message_len] = '\0';

    K_SPINLOCK(&log_ring_lock) {
        log_ring[log_ring_total_written % LOG_RING_CAPACITY] = pending_record;
        log_ring_total_written++;
    }
}

static void devtool_log_backend_init(const struct log_backend *const backend) {
    log_backend_enable(backend, NULL, CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_DEFAULT_LEVEL);
}

static void devtool_log_backend_panic(const struct log_backend *const backend) {
    ARG_UNUSED(backend);
}

const struct log_backend_api devtool_log_backend_api = {
    .process = devtool_log_backend_process,
    .init = devtool_log_backend_init,
    .panic = devtool_log_backend_panic,
};

LOG_BACKEND_DEFINE(devtool_log_backend, devtool_log_backend_api, true);

int devtool_handle_get_logs(const cormoran_devtool_GetLogsRequest *req,
                            cormoran_devtool_Response *resp) {
    cormoran_devtool_GetLogsResponse result = cormoran_devtool_GetLogsResponse_init_zero;

    uint32_t start, count, dropped, total_written;
    K_SPINLOCK(&log_ring_lock) {
        total_written = log_ring_total_written;
        devtool_ring_window(req->cursor, total_written, LOG_RING_CAPACITY,
                            ARRAY_SIZE(result.records), &start, &count, &dropped);
        for (uint32_t i = 0; i < count; i++) {
            result.records[i] = log_ring[(start + i) % LOG_RING_CAPACITY];
        }
    }

    result.records_count = count;
    result.next_cursor = start + count;
    result.dropped_count = dropped;

    resp->which_response_type = cormoran_devtool_Response_get_logs_tag;
    resp->response_type.get_logs = result;
    return 0;
}

int devtool_handle_clear_logs(cormoran_devtool_Response *resp) {
    K_SPINLOCK(&log_ring_lock) { log_ring_total_written = 0; }

    cormoran_devtool_ClearLogsResponse result = cormoran_devtool_ClearLogsResponse_init_zero;
    resp->which_response_type = cormoran_devtool_Response_clear_logs_tag;
    resp->response_type.clear_logs = result;
    return 0;
}

int devtool_handle_set_log_capture_filter(const cormoran_devtool_SetLogCaptureFilterRequest *req,
                                          cormoran_devtool_Response *resp) {
    if (req->min_level == cormoran_devtool_LogLevel_LOG_LEVEL_UNSPECIFIED) {
        devtool_set_error(resp, "Invalid log level");
        return -EINVAL;
    }

    if (req->source[0] == '\0') {
        log_backend_enable(&devtool_log_backend, NULL, req->min_level);
    } else {
        int source_id = log_source_id_get(req->source);
        if (source_id < 0) {
            devtool_set_error(resp, "Unknown log source");
            return -EINVAL;
        }
        log_filter_set(&devtool_log_backend, 0, (int16_t)source_id, req->min_level);
    }

    cormoran_devtool_SetLogCaptureFilterResponse result =
        cormoran_devtool_SetLogCaptureFilterResponse_init_zero;
    resp->which_response_type = cormoran_devtool_Response_set_log_capture_filter_tag;
    resp->response_type.set_log_capture_filter = result;
    return 0;
}

#endif /* CONFIG_ZMK_DEVTOOL_LOG_CAPTURE */
