/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_WA_
#define _XE_WA_

#include "xe_assert.h"

struct drm_printer;
struct xe_gt;
struct xe_hw_engine;
struct xe_tile;

int xe_wa_init(struct xe_gt *gt);
void xe_wa_process_oob(struct xe_gt *gt);
void xe_wa_process_gt(struct xe_gt *gt);
void xe_wa_process_engine(struct xe_hw_engine *hwe);
void xe_wa_process_lrc(struct xe_hw_engine *hwe);
void xe_wa_apply_tile_workarounds(struct xe_tile *tile);
void xe_wa_dump(struct xe_gt *gt, struct drm_printer *p);

/**
 * XE_WA - Out-of-band workarounds, that don't fit the lifecycle any
 *         other more specific type
 * @gt__: gt instance
 * @id__: XE_OOB_<id__>, as generated by build system in generated/xe_wa_oob.h
 */
#define XE_WA(gt__, id__) ({						\
	xe_gt_assert(gt__, (gt__)->wa_active.oob_initialized);		\
	test_bit(XE_WA_OOB_ ## id__, (gt__)->wa_active.oob);		\
})

#endif