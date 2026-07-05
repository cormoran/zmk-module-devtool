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
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/studio/custom.h>
#include <cormoran/devtool/devtool.pb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_STUDIO_RPC_TEST)

/*
 * Studio RPC tests for src/studio/devtool_layer.c, devtool_inject.c,
 * devtool_events.c and devtool_logs.c. Drives the registered handler
 * directly (no transport), the same pattern used by
 * zmk-feature-watchdog/src/test/watchdog_rpc_test.c and
 * zmk-feature-kscan-diagnostics/src/test/kscan_diagnostics_rpc_test.c: look
 * up cormoran__devtool in the zmk_rpc_custom_subsystem iterable section and
 * call its handler with an encoded Request, exercising the exact function
 * Studio's RPC dispatch would call.
 *
 * Needs CONFIG_ZMK_STUDIO, which in turn needs a physical-layout devicetree
 * -- see tests/studio/ (via ../test.dtsi) vs tests/test/.
 */

static custom_subsystem_handler *find_devtool_rpc_handler(void) {
    size_t count;
    STRUCT_SECTION_COUNT(zmk_rpc_custom_subsystem, &count);

    for (size_t i = 0; i < count; i++) {
        struct zmk_rpc_custom_subsystem *subsys;
        STRUCT_SECTION_GET(zmk_rpc_custom_subsystem, i, &subsys);
        if (strcmp(subsys->identifier, "cormoran__devtool") == 0) {
            return subsys->handler;
        }
    }
    return NULL;
}

struct call_response_payload_capture {
    uint8_t buf[CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE + 16];
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

static bool call_devtool_rpc(const cormoran_devtool_Request *req, cormoran_devtool_Response *out) {
    custom_subsystem_handler *handler = find_devtool_rpc_handler();
    if (!handler) {
        LOG_ERR("devtool RPC subsystem not registered");
        return false;
    }

    static zmk_custom_CallRequest raw_request;
    raw_request = (zmk_custom_CallRequest){0};
    pb_ostream_t req_stream =
        pb_ostream_from_buffer(raw_request.payload.bytes, sizeof(raw_request.payload.bytes));
    if (!pb_encode(&req_stream, cormoran_devtool_Request_fields, req)) {
        LOG_ERR("Failed to encode devtool request: %s", PB_GET_ERROR(&req_stream));
        return false;
    }
    raw_request.payload.size = req_stream.bytes_written;

    zmk_custom_CallResponse response = zmk_custom_CallResponse_init_zero;
    bool ok = handler(&raw_request, &response.payload);
    if (!ok || !response.payload.funcs.encode) {
        LOG_ERR("devtool RPC handler did not produce a response encoder");
        return false;
    }

    static uint8_t call_resp_buf[CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE + 16];
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

    *out = (cormoran_devtool_Response)cormoran_devtool_Response_init_zero;
    pb_istream_t resp_istream = pb_istream_from_buffer(capture.buf, capture.size);
    if (!pb_decode(&resp_istream, cormoran_devtool_Response_fields, out)) {
        LOG_ERR("Failed to decode devtool response: %s", PB_GET_ERROR(&resp_istream));
        return false;
    }

    return true;
}

/*
 * tests/studio/native_sim.keymap (via ../test.dtsi) defines a single layer
 * (id 0, the default layer) bound to &kp A on every position, so there is no
 * second layer to activate here -- this test instead exercises the
 * always-active-default-layer rule and the out-of-range error path.
 */
static int test_rpc_layer_state(void) {
    cormoran_devtool_Request req = cormoran_devtool_Request_init_zero;
    req.which_request_type = cormoran_devtool_Request_get_layer_state_tag;

    cormoran_devtool_Response resp;
    if (!call_devtool_rpc(&req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_devtool_Response_get_layer_state_tag ||
        !(resp.response_type.get_layer_state.active_layers & BIT(0)) ||
        resp.response_type.get_layer_state.highest_active_layer != 0) {
        LOG_ERR("unexpected get_layer_state: type=%d active=%u highest=%u",
                resp.which_response_type, resp.response_type.get_layer_state.active_layers,
                resp.response_type.get_layer_state.highest_active_layer);
        return -EINVAL;
    }

    /* Layer 1 does not exist (ZMK_KEYMAP_LAYERS_LEN == 1): expect an error. */
    cormoran_devtool_Request set_req = cormoran_devtool_Request_init_zero;
    set_req.which_request_type = cormoran_devtool_Request_set_layer_state_tag;
    set_req.request_type.set_layer_state.layer = 1;
    set_req.request_type.set_layer_state.active = true;

    if (!call_devtool_rpc(&set_req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_devtool_Response_error_tag) {
        LOG_ERR("expected error activating out-of-range layer, got type=%d",
                resp.which_response_type);
        return -EINVAL;
    }

    /* Deactivating the default layer is a documented no-op: it must remain active. */
    cormoran_devtool_Request toggle_req = cormoran_devtool_Request_init_zero;
    toggle_req.which_request_type = cormoran_devtool_Request_toggle_layer_tag;
    toggle_req.request_type.toggle_layer.layer = 0;

    if (!call_devtool_rpc(&toggle_req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_devtool_Response_toggle_layer_tag ||
        !(resp.response_type.toggle_layer.active_layers & BIT(0))) {
        LOG_ERR("default layer should remain active after toggle: type=%d active=%u",
                resp.which_response_type, resp.response_type.toggle_layer.active_layers);
        return -EINVAL;
    }

    LOG_INF("PASS: devtool_rpc_layer_state");
    return 0;
}

/*
 * Ties inject_key (devtool_inject.c) together with the event tap
 * (devtool_events.c): subscribes to position+keycode events, injects a
 * press at position 0 (bound to &kp A), and verifies both the raw position
 * event and the resulting HID keycode event show up -- the exact
 * "did pressing this key do what I expect" loop the event tap exists for.
 */
static int test_rpc_inject_key_and_event_tap(void) {
    cormoran_devtool_Request sub_req = cormoran_devtool_Request_init_zero;
    sub_req.which_request_type = cormoran_devtool_Request_subscribe_events_tag;
    sub_req.request_type.subscribe_events.event_type_mask =
        BIT(cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_POSITION_STATE_CHANGED) |
        BIT(cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_KEYCODE_STATE_CHANGED);

    cormoran_devtool_Response resp;
    if (!call_devtool_rpc(&sub_req, &resp) ||
        resp.which_response_type != cormoran_devtool_Response_subscribe_events_tag) {
        LOG_ERR("subscribe_events failed: type=%d", resp.which_response_type);
        return -EINVAL;
    }

    cormoran_devtool_Request clear_req = cormoran_devtool_Request_init_zero;
    clear_req.which_request_type = cormoran_devtool_Request_clear_events_tag;
    if (!call_devtool_rpc(&clear_req, &resp)) {
        return -EINVAL;
    }

    cormoran_devtool_Request inject_req = cormoran_devtool_Request_init_zero;
    inject_req.which_request_type = cormoran_devtool_Request_inject_key_tag;
    inject_req.request_type.inject_key.position = 0;
    inject_req.request_type.inject_key.pressed = true;
    if (!call_devtool_rpc(&inject_req, &resp) ||
        resp.which_response_type != cormoran_devtool_Response_inject_key_tag) {
        LOG_ERR("inject_key(press) failed: type=%d", resp.which_response_type);
        return -EINVAL;
    }

    inject_req.request_type.inject_key.pressed = false;
    if (!call_devtool_rpc(&inject_req, &resp) ||
        resp.which_response_type != cormoran_devtool_Response_inject_key_tag) {
        LOG_ERR("inject_key(release) failed: type=%d", resp.which_response_type);
        return -EINVAL;
    }

    cormoran_devtool_Request get_req = cormoran_devtool_Request_init_zero;
    get_req.which_request_type = cormoran_devtool_Request_get_events_tag;
    get_req.request_type.get_events.cursor = 0;
    if (!call_devtool_rpc(&get_req, &resp) ||
        resp.which_response_type != cormoran_devtool_Response_get_events_tag) {
        LOG_ERR("get_events failed: type=%d", resp.which_response_type);
        return -EINVAL;
    }

    const cormoran_devtool_GetEventsResponse *events = &resp.response_type.get_events;
    if (events->dropped_count != 0) {
        LOG_ERR("unexpected dropped events: %u", events->dropped_count);
        return -EINVAL;
    }

    bool saw_position_press = false;
    bool saw_keycode_press = false;
    for (size_t i = 0; i < events->events_count; i++) {
        const cormoran_devtool_DevtoolEvent *ev = &events->events[i];
        if (ev->which_data == cormoran_devtool_DevtoolEvent_position_state_changed_tag &&
            ev->data.position_state_changed.position == 0 &&
            ev->data.position_state_changed.pressed) {
            saw_position_press = true;
        }
        if (ev->which_data == cormoran_devtool_DevtoolEvent_keycode_state_changed_tag &&
            ev->data.keycode_state_changed.pressed) {
            saw_keycode_press = true;
        }
    }
    if (!saw_position_press || !saw_keycode_press) {
        LOG_ERR("event tap missed injected key: position=%d keycode=%d", saw_position_press,
                saw_keycode_press);
        return -EINVAL;
    }

    /* Disabling the tap (empty mask) drops the buffered content. */
    sub_req.request_type.subscribe_events.event_type_mask = 0;
    if (!call_devtool_rpc(&sub_req, &resp)) {
        return -EINVAL;
    }
    if (!call_devtool_rpc(&get_req, &resp) || resp.response_type.get_events.events_count != 0) {
        LOG_ERR("expected empty event tap after unsubscribe");
        return -EINVAL;
    }

    LOG_INF("PASS: devtool_rpc_inject_key_and_event_tap");
    return 0;
}

static int test_rpc_tap_key_busy(void) {
    cormoran_devtool_Request tap_req = cormoran_devtool_Request_init_zero;
    tap_req.which_request_type = cormoran_devtool_Request_tap_key_tag;
    tap_req.request_type.tap_key.position = 0;
    tap_req.request_type.tap_key.hold_ms = 5;

    cormoran_devtool_Response resp;
    if (!call_devtool_rpc(&tap_req, &resp) ||
        resp.which_response_type != cormoran_devtool_Response_tap_key_tag) {
        LOG_ERR("first tap_key failed: type=%d", resp.which_response_type);
        return -EINVAL;
    }

    /* A second tap while the first is still pending must be rejected. */
    if (!call_devtool_rpc(&tap_req, &resp)) {
        return -EINVAL;
    }
    if (resp.which_response_type != cormoran_devtool_Response_error_tag) {
        LOG_ERR("expected busy error for overlapping tap_key, got type=%d",
                resp.which_response_type);
        return -EINVAL;
    }

    /* Let the scheduled release fire before the next test reuses position 0. */
    k_sleep(K_MSEC(20));

    LOG_INF("PASS: devtool_rpc_tap_key_busy");
    return 0;
}

static int test_rpc_log_capture(void) {
    cormoran_devtool_Request clear_req = cormoran_devtool_Request_init_zero;
    clear_req.which_request_type = cormoran_devtool_Request_clear_logs_tag;

    cormoran_devtool_Response resp;
    if (!call_devtool_rpc(&clear_req, &resp)) {
        return -EINVAL;
    }

    LOG_INF("devtool_rpc_log_capture_marker");
    /* Deferred-mode log messages are queued for a separate logging thread;
     * drain them synchronously so the marker above is in the ring buffer
     * before get_logs below reads it. */
    while (log_process()) {
    }

    cormoran_devtool_Request get_req = cormoran_devtool_Request_init_zero;
    get_req.which_request_type = cormoran_devtool_Request_get_logs_tag;
    get_req.request_type.get_logs.cursor = 0;
    if (!call_devtool_rpc(&get_req, &resp) ||
        resp.which_response_type != cormoran_devtool_Response_get_logs_tag) {
        LOG_ERR("get_logs failed: type=%d", resp.which_response_type);
        return -EINVAL;
    }

    const cormoran_devtool_GetLogsResponse *logs = &resp.response_type.get_logs;
    bool saw_marker = false;
    for (size_t i = 0; i < logs->records_count; i++) {
        if (strstr(logs->records[i].message, "devtool_rpc_log_capture_marker") != NULL) {
            saw_marker = true;
            if (logs->records[i].level != cormoran_devtool_LogLevel_LOG_LEVEL_INF) {
                LOG_ERR("marker captured with unexpected level %d", logs->records[i].level);
                return -EINVAL;
            }
        }
    }
    if (!saw_marker) {
        LOG_ERR("get_logs did not capture the marker (records_count=%u)",
                (unsigned int)logs->records_count);
        return -EINVAL;
    }

    LOG_INF("PASS: devtool_rpc_log_capture");
    return 0;
}

static int devtool_rpc_test_init(void) {
    int ret = test_rpc_layer_state();
    if (ret < 0) {
        return ret;
    }

    ret = test_rpc_inject_key_and_event_tap();
    if (ret < 0) {
        return ret;
    }

    ret = test_rpc_tap_key_busy();
    if (ret < 0) {
        return ret;
    }

    ret = test_rpc_log_capture();
    if (ret < 0) {
        return ret;
    }

    return 0;
}

SYS_INIT(devtool_rpc_test_init, APPLICATION, 99);

#endif /* CONFIG_ZMK_DEVTOOL_STUDIO_RPC_TEST */
