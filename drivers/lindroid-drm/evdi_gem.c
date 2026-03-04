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

static int evdi_read_id_from_fd(int fd_u32, int *out_id)
{
	struct file *memfd_file;
	loff_t pos = 0;
	ssize_t rd;
	int id;

	if (!out_id || fd_u32 <= 0 || fd_u32 > INT_MAX)
		return -EINVAL;

	memfd_file = fget(fd_u32);
	if (!memfd_file)
		return -EINVAL;

	rd = kernel_read(memfd_file, &id, sizeof(id), &pos);
	fput(memfd_file);
	if (rd != sizeof(id))
		return -EINVAL;

	*out_id = id;
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
	int id, ret;
	if (!handle)
		return -EINVAL;

	ret = evdi_read_id_from_fd(prime_fd, &id);
	if (ret)
		return ret;

	*handle = (uint32_t)id;
	return 0;
}