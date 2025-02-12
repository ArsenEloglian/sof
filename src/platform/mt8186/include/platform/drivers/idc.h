/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright(c) 2022 MediaTek. All rights reserved.
 *
 * Author: Allen-KH Cheng <allen-kh.cheng@mediatek.com>
 *         Tinghan Shen <tinghan.shen@mediatek.com>
 */

#ifdef __SOF_DRIVERS_IDC_H__

#ifndef __PLATFORM_DRIVERS_IDC_H__
#define __PLATFORM_DRIVERS_IDC_H__

#include <stdint.h>

struct idc_msg;

static inline int idc_send_msg(struct idc_msg *msg, uint32_t mode)
{
	return 0;
}

static inline int idc_init(void)
{
	return 0;
}

#endif /* __PLATFORM_DRIVERS_IDC_H__ */

#else

#error "This file shouldn't be included from outside of sof/drivers/idc.h"

#endif /* __SOF_DRIVERS_IDC_H__ */
