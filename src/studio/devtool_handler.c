#include <errno.h>

#include <dt-bindings/zmk/reset.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/core.h>
#include <zmk/studio/custom.h>
#include <cormoran/devtool/devtool.pb.h>

#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
#include <zephyr/retention/bootmode.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define REBOOT_DELAY_MS 250

enum devtool_reboot_target {
    DEVTOOL_REBOOT_TARGET_SYSTEM,
    DEVTOOL_REBOOT_TARGET_BOOTLOADER,
};

static struct zmk_rpc_custom_subsystem_meta devtool_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-module-devtool/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__devtool, &devtool_meta, devtool_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__devtool, cormoran_devtool_Response);

static enum devtool_reboot_target pending_reboot_target = DEVTOOL_REBOOT_TARGET_SYSTEM;

static cormoran_devtool_StudioLockState to_proto_lock_state(void) {
    switch (zmk_studio_core_get_lock_state()) {
    case ZMK_STUDIO_CORE_LOCK_STATE_LOCKED:
        return cormoran_devtool_StudioLockState_STUDIO_LOCK_STATE_LOCKED;
    case ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED:
        return cormoran_devtool_StudioLockState_STUDIO_LOCK_STATE_UNLOCKED;
    default:
        return cormoran_devtool_StudioLockState_STUDIO_LOCK_STATE_UNSPECIFIED;
    }
}

static void devtool_reboot_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (pending_reboot_target == DEVTOOL_REBOOT_TARGET_BOOTLOADER) {
#if IS_ENABLED(CONFIG_RETENTION_BOOT_MODE)
        int ret = bootmode_set(BOOT_MODE_TYPE_BOOTLOADER);
        if (ret < 0) {
            LOG_ERR("Failed to set bootloader mode (%d)", ret);
            return;
        }

        sys_reboot(SYS_REBOOT_WARM);
#else
        sys_reboot(RST_UF2);
#endif
        return;
    }

    sys_reboot(SYS_REBOOT_WARM);
}

K_WORK_DELAYABLE_DEFINE(devtool_reboot_work, devtool_reboot_work_handler);

static void schedule_reboot(enum devtool_reboot_target target) {
    pending_reboot_target = target;
    k_work_schedule(&devtool_reboot_work, K_MSEC(REBOOT_DELAY_MS));
}

static void set_error(cormoran_devtool_Response *resp, const char *message) {
    cormoran_devtool_ErrorResponse err = cormoran_devtool_ErrorResponse_init_zero;

    snprintf(err.message, sizeof(err.message), "%s", message);
    resp->which_response_type = cormoran_devtool_Response_error_tag;
    resp->response_type.error = err;
}

static int
handle_set_studio_lock_state_request(const cormoran_devtool_SetStudioLockStateRequest *req,
                                     cormoran_devtool_Response *resp) {
    switch (req->state) {
    case cormoran_devtool_StudioLockState_STUDIO_LOCK_STATE_LOCKED:
        zmk_studio_core_lock();
        break;
    case cormoran_devtool_StudioLockState_STUDIO_LOCK_STATE_UNLOCKED:
        zmk_studio_core_unlock();
        break;
    default:
        set_error(resp, "Invalid Studio lock state");
        return -EINVAL;
    }

    cormoran_devtool_SetStudioLockStateResponse result =
        cormoran_devtool_SetStudioLockStateResponse_init_zero;
    result.state = to_proto_lock_state();

    resp->which_response_type = cormoran_devtool_Response_set_studio_lock_state_tag;
    resp->response_type.set_studio_lock_state = result;
    return 0;
}

static int handle_enter_bootloader_request(cormoran_devtool_Response *resp) {
    cormoran_devtool_EnterBootloaderResponse result =
        cormoran_devtool_EnterBootloaderResponse_init_zero;

    resp->which_response_type = cormoran_devtool_Response_enter_bootloader_tag;
    resp->response_type.enter_bootloader = result;

    schedule_reboot(DEVTOOL_REBOOT_TARGET_BOOTLOADER);
    return 0;
}

static int handle_reboot_request(cormoran_devtool_Response *resp) {
    cormoran_devtool_RebootResponse result = cormoran_devtool_RebootResponse_init_zero;

    resp->which_response_type = cormoran_devtool_Response_reboot_tag;
    resp->response_type.reboot = result;

    schedule_reboot(DEVTOOL_REBOOT_TARGET_SYSTEM);
    return 0;
}

static bool devtool_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                       pb_callback_t *encode_response) {
    cormoran_devtool_Response *resp =
        ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran__devtool, encode_response);

    cormoran_devtool_Request req = cormoran_devtool_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_devtool_Request_fields, &req)) {
        LOG_WRN("Failed to decode devtool request: %s", PB_GET_ERROR(&req_stream));
        set_error(resp, "Failed to decode request");
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case cormoran_devtool_Request_set_studio_lock_state_tag:
        rc = handle_set_studio_lock_state_request(&req.request_type.set_studio_lock_state, resp);
        break;
    case cormoran_devtool_Request_enter_bootloader_tag:
        rc = handle_enter_bootloader_request(resp);
        break;
    case cormoran_devtool_Request_reboot_tag:
        rc = handle_reboot_request(resp);
        break;
    default:
        LOG_WRN("Unsupported devtool request type: %d", req.which_request_type);
        rc = -ENOTSUP;
    }

    if (rc != 0 && resp->which_response_type != cormoran_devtool_Response_error_tag) {
        set_error(resp, "Failed to process request");
    }

    return true;
}
