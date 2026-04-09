// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 * Copyright (c) 2025 Lindroid Authors
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#ifndef __UAPI_EVDI_DRM_H__
#define __UAPI_EVDI_DRM_H__

#ifdef __KERNEL__
#include <linux/types.h>
#include <drm/drm.h>
#else
#include <stdint.h>
#include <drm/drm.h>
#endif

enum poll_event_type { none = 0, disp_pwr = 1, destroy_buf = 2, swap_to = 3 };

struct drm_evdi_connect {
	int32_t connected;
	int32_t dev_index;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_rate;
	uint32_t display_id;
};

struct drm_evdi_poll {
	enum poll_event_type event;
	int poll_id;
	void *data;
};

struct drm_evdi_get_fd {
	int display_id;
	int buffer_id;
	int num_fds;
	int fds[4];
};

struct drm_evdi_flipped {
	__u32 display_id;
};

#define DRM_EVDI_CONNECT 0x00
#define DRM_EVDI_GRABPIX 0x02 /* Unused by create-disp */
#define DRM_EVDI_ENABLE_CURSOR_EVENTS 0x03 /* Unused by create-disp */
#define DRM_EVDI_POLL 0x04
#define DRM_EVDI_GET_FD 0x05 /* Unused by create-disp */
#define DRM_EVDI_DESTROY_BUFF_CALLBACK 0x09
#define DRM_EVDI_FLIPPED 0x0E

#define DRM_IOCTL_EVDI_CONNECT                                                 \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_CONNECT, struct drm_evdi_connect)

#define DRM_IOCTL_EVDI_POLL                                                    \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_POLL, struct drm_evdi_poll)

#define DRM_IOCTL_EVDI_GET_FD                                                  \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_EVDI_GET_FD, struct drm_evdi_get_fd)

#define DRM_IOCTL_EVDI_FLIPPED                                                 \
	DRM_IOW(DRM_COMMAND_BASE + DRM_EVDI_FLIPPED, struct drm_evdi_flipped)

#endif /* __UAPI_EVDI_DRM_H__ */
