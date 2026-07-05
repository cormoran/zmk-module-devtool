/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <cormoran/devtool/devtool.pb.h>

#include "devtool_internal.h"

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_EVENT_TAP)

struct devtool_event_record {
    uint32_t timestamp_ms;
    cormoran_devtool_DevtoolEventType type;
    union {
        struct {
            uint32_t position;
            bool pressed;
        } position_state_changed;
        struct {
            uint32_t usage_page;
            uint32_t keycode;
            bool pressed;
            uint32_t implicit_modifiers;
            uint32_t explicit_modifiers;
        } keycode_state_changed;
        struct {
            uint32_t layer;
            bool active;
            bool locked;
        } layer_state_changed;
        struct {
            uint32_t modifiers;
            bool pressed;
        } modifiers_state_changed;
    } data;
};

#define EVENT_RING_CAPACITY CONFIG_ZMK_DEVTOOL_EVENT_TAP_BUFFER_SIZE

/*
 * ~37 bytes/event (timestamp + type + the largest oneof variant,
 * KeycodeStateChangedData, plus repeated-field tag/len overhead) * the 6
 * events/response cap from devtool.options, plus next_cursor/dropped_count
 * and CallResponse wrapper overhead.
 */
#define DEVTOOL_EVENTS_RESPONSE_ESTIMATED_MAX_SIZE 260
BUILD_ASSERT(DEVTOOL_EVENTS_RESPONSE_ESTIMATED_MAX_SIZE + 64 <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
             "CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE too small for GetEventsResponse; see "
             "devtool.options' events max_count comment");

static struct devtool_event_record event_ring[EVENT_RING_CAPACITY];
static uint32_t event_ring_total_written;
static uint32_t event_type_mask;
static struct k_spinlock event_ring_lock;

static bool event_type_subscribed(cormoran_devtool_DevtoolEventType type) {
    return (event_type_mask & BIT(type)) != 0;
}

static void push_event(const struct devtool_event_record *record) {
    K_SPINLOCK(&event_ring_lock) {
        event_ring[event_ring_total_written % EVENT_RING_CAPACITY] = *record;
        event_ring_total_written++;
    }
}

static int devtool_events_listener(const zmk_event_t *eh) {
    if (event_type_mask == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct devtool_event_record record = {.timestamp_ms = (uint32_t)k_uptime_get()};

    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev &&
        event_type_subscribed(
            cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_POSITION_STATE_CHANGED)) {
        record.type = cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_POSITION_STATE_CHANGED;
        record.data.position_state_changed.position = pos_ev->position;
        record.data.position_state_changed.pressed = pos_ev->state;
        push_event(&record);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_keycode_state_changed *kc_ev = as_zmk_keycode_state_changed(eh);
    if (kc_ev && event_type_subscribed(
                     cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_KEYCODE_STATE_CHANGED)) {
        record.type = cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_KEYCODE_STATE_CHANGED;
        record.data.keycode_state_changed.usage_page = kc_ev->usage_page;
        record.data.keycode_state_changed.keycode = kc_ev->keycode;
        record.data.keycode_state_changed.pressed = kc_ev->state;
        record.data.keycode_state_changed.implicit_modifiers = kc_ev->implicit_modifiers;
        record.data.keycode_state_changed.explicit_modifiers = kc_ev->explicit_modifiers;
        push_event(&record);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev && event_type_subscribed(
                        cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_LAYER_STATE_CHANGED)) {
        record.type = cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_LAYER_STATE_CHANGED;
        record.data.layer_state_changed.layer = layer_ev->layer;
        record.data.layer_state_changed.active = layer_ev->state;
        record.data.layer_state_changed.locked = layer_ev->locked;
        push_event(&record);
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_modifiers_state_changed *mod_ev = as_zmk_modifiers_state_changed(eh);
    if (mod_ev &&
        event_type_subscribed(
            cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_MODIFIERS_STATE_CHANGED)) {
        record.type = cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_MODIFIERS_STATE_CHANGED;
        record.data.modifiers_state_changed.modifiers = mod_ev->modifiers;
        record.data.modifiers_state_changed.pressed = mod_ev->state;
        push_event(&record);
        return ZMK_EV_EVENT_BUBBLE;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(devtool_events, devtool_events_listener);
ZMK_SUBSCRIPTION(devtool_events, zmk_position_state_changed);
ZMK_SUBSCRIPTION(devtool_events, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(devtool_events, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(devtool_events, zmk_modifiers_state_changed);

int devtool_handle_subscribe_events(const cormoran_devtool_SubscribeEventsRequest *req,
                                    cormoran_devtool_Response *resp) {
    event_type_mask = req->event_type_mask;
    if (event_type_mask == 0) {
        /* Disabling drops whatever was buffered: nothing new will be captured with an empty
         * mask, so stale records from a previous subscription would otherwise linger. */
        K_SPINLOCK(&event_ring_lock) { event_ring_total_written = 0; }
    }

    cormoran_devtool_SubscribeEventsResponse result =
        cormoran_devtool_SubscribeEventsResponse_init_zero;
    result.event_type_mask = event_type_mask;

    resp->which_response_type = cormoran_devtool_Response_subscribe_events_tag;
    resp->response_type.subscribe_events = result;
    return 0;
}

int devtool_handle_get_events(const cormoran_devtool_GetEventsRequest *req,
                              cormoran_devtool_Response *resp) {
    cormoran_devtool_GetEventsResponse result = cormoran_devtool_GetEventsResponse_init_zero;

    uint32_t start, count, dropped;
    uint32_t total_written;
    struct devtool_event_record snapshot[ARRAY_SIZE(result.events)];

    K_SPINLOCK(&event_ring_lock) {
        total_written = event_ring_total_written;
        devtool_ring_window(req->cursor, total_written, EVENT_RING_CAPACITY,
                            ARRAY_SIZE(result.events), &start, &count, &dropped);
        for (uint32_t i = 0; i < count; i++) {
            snapshot[i] = event_ring[(start + i) % EVENT_RING_CAPACITY];
        }
    }

    result.events_count = count;
    for (uint32_t i = 0; i < count; i++) {
        cormoran_devtool_DevtoolEvent *out = &result.events[i];
        out->timestamp_ms = snapshot[i].timestamp_ms;
        out->type = snapshot[i].type;
        switch (snapshot[i].type) {
        case cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_POSITION_STATE_CHANGED:
            out->which_data = cormoran_devtool_DevtoolEvent_position_state_changed_tag;
            out->data.position_state_changed.position =
                snapshot[i].data.position_state_changed.position;
            out->data.position_state_changed.pressed =
                snapshot[i].data.position_state_changed.pressed;
            break;
        case cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_KEYCODE_STATE_CHANGED:
            out->which_data = cormoran_devtool_DevtoolEvent_keycode_state_changed_tag;
            out->data.keycode_state_changed.usage_page =
                snapshot[i].data.keycode_state_changed.usage_page;
            out->data.keycode_state_changed.keycode =
                snapshot[i].data.keycode_state_changed.keycode;
            out->data.keycode_state_changed.pressed =
                snapshot[i].data.keycode_state_changed.pressed;
            out->data.keycode_state_changed.implicit_modifiers =
                snapshot[i].data.keycode_state_changed.implicit_modifiers;
            out->data.keycode_state_changed.explicit_modifiers =
                snapshot[i].data.keycode_state_changed.explicit_modifiers;
            break;
        case cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_LAYER_STATE_CHANGED:
            out->which_data = cormoran_devtool_DevtoolEvent_layer_state_changed_tag;
            out->data.layer_state_changed.layer = snapshot[i].data.layer_state_changed.layer;
            out->data.layer_state_changed.active = snapshot[i].data.layer_state_changed.active;
            out->data.layer_state_changed.locked = snapshot[i].data.layer_state_changed.locked;
            break;
        case cormoran_devtool_DevtoolEventType_DEVTOOL_EVENT_TYPE_MODIFIERS_STATE_CHANGED:
            out->which_data = cormoran_devtool_DevtoolEvent_modifiers_state_changed_tag;
            out->data.modifiers_state_changed.modifiers =
                snapshot[i].data.modifiers_state_changed.modifiers;
            out->data.modifiers_state_changed.pressed =
                snapshot[i].data.modifiers_state_changed.pressed;
            break;
        default:
            break;
        }
    }
    result.next_cursor = start + count;
    result.dropped_count = dropped;

    resp->which_response_type = cormoran_devtool_Response_get_events_tag;
    resp->response_type.get_events = result;
    return 0;
}

int devtool_handle_clear_events(cormoran_devtool_Response *resp) {
    K_SPINLOCK(&event_ring_lock) { event_ring_total_written = 0; }

    cormoran_devtool_ClearEventsResponse result = cormoran_devtool_ClearEventsResponse_init_zero;
    resp->which_response_type = cormoran_devtool_Response_clear_events_tag;
    resp->response_type.clear_events = result;
    return 0;
}

#endif /* CONFIG_ZMK_DEVTOOL_EVENT_TAP */
