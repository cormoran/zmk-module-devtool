/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <cormoran/devtool/devtool.pb.h>

#include "devtool_internal.h"

#if IS_ENABLED(CONFIG_ZMK_DEVTOOL_STACK_USAGE)

/*
 * Per-thread stack high-water inspection over Studio RPC.
 *
 * k_thread_foreach() walks the kernel thread list (CONFIG_THREAD_MONITOR) and
 * for each thread k_thread_stack_space_get() scans its stack for the unused
 * INIT_STACKS sentinel (CONFIG_INIT_STACKS) to report peak usage against the
 * stack bounds recorded in stack_info (CONFIG_THREAD_STACK_INFO). This is the
 * same measurement Zephyr's thread_analyzer performs; all four Kconfigs are
 * selected by CONFIG_ZMK_DEVTOOL_STACK_USAGE.
 *
 * The response is paginated (cursor = number of threads to skip) so an
 * arbitrary thread count fits the fixed GetStackUsageResponse.stacks array.
 */

struct stack_collect_ctx {
    cormoran_devtool_GetStackUsageResponse *resp;
    uint32_t skip;
    uint32_t total;
    bool overflow;
};

static void stack_collect_cb(const struct k_thread *thread, void *user_data) {
    struct stack_collect_ctx *ctx = user_data;
    uint32_t index = ctx->total++;

    /* Already returned on an earlier page. */
    if (index < ctx->skip) {
        return;
    }
    /* Page is full: remember that more threads remain so we hand back a cursor. */
    if (ctx->resp->stacks_count >= ARRAY_SIZE(ctx->resp->stacks)) {
        ctx->overflow = true;
        return;
    }

    cormoran_devtool_StackInfo *info = &ctx->resp->stacks[ctx->resp->stacks_count];
    *info = (cormoran_devtool_StackInfo)cormoran_devtool_StackInfo_init_zero;

    const char *name = k_thread_name_get((k_tid_t)thread);
    if (name != NULL && name[0] != '\0') {
        snprintf(info->name, sizeof(info->name), "%s", name);
    } else {
        snprintf(info->name, sizeof(info->name), "%p", (const void *)thread);
    }

    uint32_t size = (uint32_t)thread->stack_info.size;
    info->size = size;

    size_t unused = 0;
    if (k_thread_stack_space_get(thread, &unused) == 0) {
        if (unused > size) {
            unused = size;
        }
        info->unused = (uint32_t)unused;
        info->used = size - (uint32_t)unused;
    }

    ctx->resp->stacks_count++;
}

int devtool_handle_get_stack_usage(const cormoran_devtool_GetStackUsageRequest *req,
                                   cormoran_devtool_Response *resp) {
    cormoran_devtool_GetStackUsageResponse result =
        cormoran_devtool_GetStackUsageResponse_init_zero;

    struct stack_collect_ctx ctx = {
        .resp = &result,
        .skip = req->cursor,
    };

    k_thread_foreach(stack_collect_cb, &ctx);

    result.next_cursor = ctx.overflow ? ctx.skip + result.stacks_count : 0;
    result.total = ctx.total;

    resp->which_response_type = cormoran_devtool_Response_get_stack_usage_tag;
    resp->response_type.get_stack_usage = result;
    return 0;
}

#endif /* CONFIG_ZMK_DEVTOOL_STACK_USAGE */
