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
#include <zephyr/sys/atomic.h>

#include <cormoran/devtool/devtool.pb.h>

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING)
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#endif

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

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING)
/* Streaming state. streaming_enabled/suppress are only ever read/written with
 * atomics; log_stream_cursor is guarded by log_ring_lock (shared with the ring
 * math). See the streaming thread and devtool_handle_set_log_streaming below. */
static atomic_t log_streaming_enabled = ATOMIC_INIT(0);
static atomic_t log_stream_suppress = ATOMIC_INIT(0);
static uint32_t log_stream_cursor;
static K_SEM_DEFINE(log_stream_sem, 0, 1);
#endif

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

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING)
    /* Drop anything logged as a side effect of the streaming thread's own
     * notification send (e.g. the "zmk"-source "Encoding custom response" line),
     * so streaming cannot re-trigger itself. Fully effective in immediate log
     * mode where those logs run inline on the streaming thread; see the
     * self-feedback note in the streaming thread and README for the deferred-mode
     * caveat and the recommended zmk_studio source filter. */
    if (atomic_get(&log_stream_suppress)) {
        return;
    }
#endif

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

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING)
    /* Wake the streaming thread to encode+send on its own low-priority thread;
     * the logging subsystem's thread must never do RPC/protobuf work itself. */
    if (atomic_get(&log_streaming_enabled)) {
        k_sem_give(&log_stream_sem);
    }
#endif
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

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING)

/* Encodes the devtool Notification as the CustomNotification payload bytes,
 * same wire shape as a CallResponse payload. Runs synchronously inside
 * raise_zmk_studio_custom_notification(), so `arg` may point at the streaming
 * thread's stack. */
static bool encode_stream_notification_payload(pb_ostream_t *stream, const pb_field_t *field,
                                               void *const *arg) {
    const cormoran_devtool_Notification *notification = *arg;
    return zmk_rpc_custom_subsystem_encode_response_payload(
        stream, field, cormoran_devtool_Notification_fields, notification);
}

/* Drains the log ring from log_stream_cursor and pushes it to the connected
 * client as one or more LogStreamNotifications. All of the encode + transport
 * work happens here, on this dedicated low-priority thread, so the logging
 * subsystem's own thread only ever does the cheap ring-buffer append above. */
static void log_stream_thread(void) {
    for (;;) {
        k_sem_take(&log_stream_sem, K_FOREVER);

        while (atomic_get(&log_streaming_enabled)) {
            cormoran_devtool_LogStreamNotification batch =
                cormoran_devtool_LogStreamNotification_init_zero;
            uint32_t start, count, dropped, total_written;

            K_SPINLOCK(&log_ring_lock) {
                total_written = log_ring_total_written;
                devtool_ring_window(log_stream_cursor, total_written, LOG_RING_CAPACITY,
                                    ARRAY_SIZE(batch.records), &start, &count, &dropped);
                for (uint32_t i = 0; i < count; i++) {
                    batch.records[i] = log_ring[(start + i) % LOG_RING_CAPACITY];
                }
                log_stream_cursor = start + count;
            }

            /* Nothing new and nothing lost: caught up, wait for the next log. */
            if (count == 0 && dropped == 0) {
                break;
            }

            batch.records_count = count;
            batch.dropped_count = dropped;

            cormoran_devtool_Notification notification = cormoran_devtool_Notification_init_zero;
            notification.which_notification_type = cormoran_devtool_Notification_log_stream_tag;
            notification.notification_type.log_stream = batch;

            struct zmk_studio_custom_notification ev = {
                .subsystem_index = devtool_custom_subsystem_index(),
                .encode_payload = {.funcs = {.encode = encode_stream_notification_payload},
                                   .arg = &notification},
            };

            /* Any log emitted while this send runs (the Studio subsystem's own
             * "Encoding custom response" DBG line, transport chatter, ...) is
             * dropped by devtool_log_backend_process() via log_stream_suppress,
             * so streaming can't feed itself. */
            atomic_set(&log_stream_suppress, 1);
            raise_zmk_studio_custom_notification(ev);
            atomic_set(&log_stream_suppress, 0);
        }
    }
}

K_THREAD_DEFINE(devtool_log_stream_thread, CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAM_STACK_SIZE,
                log_stream_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

int devtool_handle_set_log_streaming(const cormoran_devtool_SetLogStreamingRequest *req,
                                     cormoran_devtool_Response *resp) {
    if (req->enabled) {
        /* Start streaming from now (skip whatever was already buffered); use
         * get_logs to pull existing backlog. */
        K_SPINLOCK(&log_ring_lock) { log_stream_cursor = log_ring_total_written; }
        atomic_set(&log_streaming_enabled, 1);
        k_sem_give(&log_stream_sem);
    } else {
        atomic_set(&log_streaming_enabled, 0);
    }

    cormoran_devtool_SetLogStreamingResponse result =
        cormoran_devtool_SetLogStreamingResponse_init_zero;
    result.enabled = req->enabled;
    resp->which_response_type = cormoran_devtool_Response_set_log_streaming_tag;
    resp->response_type.set_log_streaming = result;
    return 0;
}

#endif /* CONFIG_ZMK_DEVTOOL_LOG_CAPTURE_STREAMING */

#endif /* CONFIG_ZMK_DEVTOOL_LOG_CAPTURE */
