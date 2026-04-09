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

#include "evdi_drv.h"
#include <drm/drm_framebuffer.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <linux/slab.h>
#include <linux/overflow.h>

static void evdi_fb_destroy(struct drm_framebuffer *fb)
{
	struct evdi_framebuffer *efb = to_evdi_fb(fb);
	struct evdi_device *evdi = fb->dev->dev_private;
	int i;
	
	for (i = 0; i < efb->gem_count; i++)
		drm_gem_object_put(efb->gem_objs[i]);

	evdi_queue_destroy_buf_event(evdi, efb->gralloc_buf_id, efb->owner);

	drm_framebuffer_cleanup(fb);
	kfree(efb);
}

const struct drm_framebuffer_funcs evdifb_funcs = {
	.destroy = evdi_fb_destroy,
};

static unsigned int evdi_fb_cpp(u32 format)
{
	const struct drm_format_info *info = drm_format_info(format);
	if (!info || info->num_planes != 1)
		return 0;
	return info->cpp[0];
}

static int evdi_fb_calc_size(const struct drm_mode_fb_cmd2 *mode_cmd,
			     u32 *out_pitch, size_t *out_size)
{
	u32 cpp, pitch;
	size_t total;

	cpp = evdi_fb_cpp(mode_cmd->pixel_format);
	if (!cpp || !mode_cmd->width || !mode_cmd->height)
		return -EINVAL;

	pitch = mode_cmd->pitches[0] ? mode_cmd->pitches[0] :
				       cpp * mode_cmd->width;
	if (check_mul_overflow((size_t)mode_cmd->height, (size_t)pitch, &total))
		return -EOVERFLOW;

	*out_pitch = pitch;
	*out_size = total;
	return 0;
}

static int evdi_fb_init_core(struct drm_device *dev,
			     struct evdi_framebuffer *efb,
			     const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct drm_framebuffer *fb = &efb->base;
	const struct drm_format_info *info =
		drm_format_info(mode_cmd->pixel_format);
	int ret;

	if (!info)
		return -EINVAL;

	fb->dev = dev;
	fb->format = info;
	fb->width = mode_cmd->width;
	fb->height = mode_cmd->height;
	fb->pitches[0] =
		mode_cmd->pitches[0] ?
			mode_cmd->pitches[0] :
			evdi_fb_cpp(mode_cmd->pixel_format) * mode_cmd->width;
	fb->offsets[0] = mode_cmd->offsets[0];
	fb->modifier = DRM_FORMAT_MOD_LINEAR;
	fb->flags = 0;
	fb->funcs = &evdifb_funcs;

	ret = drm_framebuffer_init(dev, fb, &evdifb_funcs);
	return ret;
}

struct drm_framebuffer *
evdi_fb_user_fb_create(struct drm_device *dev, struct drm_file *file,
		       const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct evdi_device *evdi = dev->dev_private;
	struct evdi_framebuffer *efb;
	struct drm_gem_object *gem_obj;
	int i, ret;

	efb = kzalloc(sizeof(*efb), GFP_KERNEL);
	if (!efb)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < 4; i++) {
		if (mode_cmd->handles[i] == 0)
			continue;

		gem_obj = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!gem_obj) {
			evdi_err("GEM lookup failed for handle %u",
				 mode_cmd->handles[i]);
			ret = -ENOENT;
			goto err;
		}
		efb->gem_objs[i] = gem_obj;
		efb->gem_count++;
	}

	efb->owner = file;
	efb->active = true;
	efb->gralloc_buf_id = atomic_inc_return(&evdi->buf_id_counter);

	ret = evdi_fb_init_core(dev, efb, mode_cmd);
	if (ret)
		goto err;

	return &efb->base;

err:
	for (i = 0; i < efb->gem_count; i++) {
		if (efb->gem_objs[i])
			drm_gem_object_put(efb->gem_objs[i]);
	}
	kfree(efb);
	return ERR_PTR(ret);
}