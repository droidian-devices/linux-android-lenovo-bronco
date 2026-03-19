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

#include <linux/slab.h>
#include <linux/shmem_fs.h>
#include <linux/dma-buf.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/version.h>

#include <drm/drm_gem.h>
#include <drm/drm_prime.h>

#include "evdi_drv.h"

static int evdi_export_id_as_fd(int id, uint32_t flags, int *out_fd)
{
	struct file *f;
	int fd_flags = (flags & DRM_CLOEXEC) ? O_CLOEXEC : 0;
	int newfd;
	loff_t pos = 0;
	ssize_t wr;

	if (!out_fd || id <= 0)
		return -EINVAL;

	f = shmem_file_setup("evdi-bufid", sizeof(id), 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	newfd = get_unused_fd_flags(fd_flags);
	if (newfd < 0) {
		fput(f);
		return newfd;
	}

	wr = kernel_write(f, &id, sizeof(id), &pos);
	if (wr != sizeof(id)) {
		put_unused_fd(newfd);
		fput(f);
		return (wr < 0) ? (int)wr : -EIO;
	}

	fd_install(newfd, f);
	*out_fd = newfd;
	return 0;
}

int evdi_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv,
			    uint32_t handle, uint32_t flags, int *prime_fd)
{
	return evdi_export_id_as_fd((int)handle, flags, prime_fd);
}

int evdi_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv,
			    int prime_fd, uint32_t *handle)
{
	struct evdi_gem_object *obj;
	struct file *dmabuf_file;
	int ret;

	if (!handle || prime_fd < 0)
		return -EINVAL;

	dmabuf_file = fget(prime_fd);
	if (!dmabuf_file) {
		return -EBADF;
	}

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj) {
		fput(dmabuf_file);
		return -ENOMEM;
	}

	ret = drm_gem_object_init(dev, &obj->base, PAGE_SIZE);
	if (ret) {
		kfree(obj);
		fput(dmabuf_file);
		return ret;
	}

	obj->dmabuf_file = dmabuf_file;

	ret = drm_gem_handle_create(file_priv, &obj->base, handle);
	if (ret) {
		drm_gem_object_release(&obj->base);
		kfree(obj);
		fput(dmabuf_file);
		return ret;
	}

	drm_gem_object_put(&obj->base);

	return 0;
}