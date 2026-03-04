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
#include "uapi/evdi_drm.h"
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/prefetch.h>
#include <linux/completion.h>
#include <linux/compat.h>
#include <linux/sched/signal.h>
#include <linux/errno.h>

enum evdi_buffer_op { EVDI_BUF_TRACK, EVDI_BUF_UNTRACK };

struct evdi_gralloc_buf_stack {
	struct evdi_gralloc_buf_user buf;
	int installed_fds[EVDI_MAX_FDS];
};

static int evdi_queue_event(struct evdi_device *evdi, enum poll_event_type type,
			    const void *payload, size_t payload_size,
			    struct drm_file *owner, int poll_id)
{
	struct evdi_event *event = evdi_event_alloc(
		evdi, type, poll_id, (void *)payload, payload_size, owner);
	if (!event)
		return -ENOMEM;

	evdi_event_queue(evdi, event);
	return 0;
}

static inline int evdi_queue_event_autoid(struct evdi_device *evdi,
					  enum poll_event_type type,
					  const void *payload,
					  size_t payload_size,
					  struct drm_file *owner)
{
	int poll_id = atomic_inc_return(&evdi->events.next_poll_id);
	return evdi_queue_event(evdi, type, payload, payload_size, owner,
				poll_id);
}

static struct evdi_inflight_req *evdi_inflight_take(struct evdi_device *evdi,
						    int id)
{
	struct evdi_inflight_req *req;

	spin_lock(&evdi->inflight_lock);
	req = idr_remove(&evdi->inflight_idr, id);
	spin_unlock(&evdi->inflight_lock);

	return req;
}

static int evdi_wait_req(struct evdi_inflight_req *req)
{
	long ret = wait_for_completion_interruptible_timeout(&req->done,
							     EVDI_WAIT_TIMEOUT);
	if (ret <= 0)
		return ret ? (int)ret : -ETIMEDOUT;
	return 0;
}

static inline void evdi_inflight_cancel(struct evdi_device *evdi,
					struct evdi_inflight_req *req,
					int poll_id)
{
	struct evdi_inflight_req *tmp = evdi_inflight_take(evdi, poll_id);
	if (tmp)
		evdi_inflight_req_put(tmp);
	evdi_inflight_req_put(req);
}

static inline int evdi_get_unused_fds_batch(int n, int flags, int *fds)
{
	int i;

	for (i = 0; i < n; i++) {
		fds[i] = get_unused_fd_flags(flags);
		if (fds[i] < 0)
			goto err;
	}

	return 0;

err:
	while (i--)
		put_unused_fd(fds[i]);
	return fds[i];
}

static int
evdi_process_gralloc_buffer(struct evdi_inflight_req *req, int *installed_fds,
			    struct evdi_gralloc_buf_user *gralloc_buf)
{
	int i, ret;
	struct evdi_gralloc_data *gralloc =
		req->reply.get_buf.gralloc_buf.gralloc;

	if (!gralloc || gralloc->numFds < 0 || gralloc->numFds > EVDI_MAX_FDS ||
	    gralloc->numInts < 0 || gralloc->numInts > EVDI_MAX_INTS)
		return -EINVAL;

	gralloc_buf->version = gralloc->version;
	gralloc_buf->numFds = gralloc->numFds;
	gralloc_buf->numInts = gralloc->numInts;

	memcpy(&gralloc_buf->data[gralloc_buf->numFds], gralloc->data_ints,
	       sizeof(*gralloc->data_ints) * gralloc_buf->numInts);

	ret = evdi_get_unused_fds_batch(gralloc_buf->numFds, O_RDWR,
					installed_fds);
	if (ret < 0)
		return ret;

	for (i = 0; i < gralloc_buf->numFds; i++)
		gralloc_buf->data[i] = installed_fds[i];

	return 0;
}

static inline struct evdi_inflight_req *
evdi_inflight_alloc(struct evdi_device *evdi, struct drm_file *owner, int type,
		    int *out_id)
{
	int id;
	struct evdi_inflight_req *req;

	req = evdi_inflight_req_alloc(evdi);
	if (!req)
		return NULL;

	req->type = type;
	req->owner = owner;

	idr_preload(GFP_KERNEL);
	spin_lock(&evdi->inflight_lock);
	id = idr_alloc(&evdi->inflight_idr, req, 1, EVDI_MAX_INFLIGHT_REQUESTS,
		       GFP_NOWAIT);
	spin_unlock(&evdi->inflight_lock);
	idr_preload_end();

	if (id < 0) {
		evdi_inflight_req_put(req);
		return NULL;
	}

	evdi_inflight_req_get(req);
	*out_id = id;
	return req;
}

void evdi_inflight_discard_owner(struct evdi_device *evdi,
				 struct drm_file *owner)
{
	struct evdi_inflight_req *req;
	struct evdi_inflight_req *batch[16];
	int nr, i, id = 0;

	if (unlikely(!evdi || !owner))
		return;

	{
		do {
			nr = 0;

			spin_lock(&evdi->inflight_lock);
			while (nr < ARRAY_SIZE(batch)) {
				req = idr_get_next(&evdi->inflight_idr, &id);
				if (!req)
					break;

				if (req->owner == owner) {
					idr_remove(&evdi->inflight_idr, id);
					batch[nr++] = req;
				}
				id++;
			}
			spin_unlock(&evdi->inflight_lock);

			for (i = 0; i < nr; i++) {
				complete_all(&batch[i]->done);
				evdi_inflight_req_put(batch[i]);
				cond_resched();
			}
		} while (nr == ARRAY_SIZE(batch));
	}
}

static inline size_t evdi_event_serialize_payload(struct evdi_event *e,
						  void *out, size_t max)
{
	size_t sz = min_t(size_t, e->payload_size, max);

	if (sz)
		memcpy(out, e->payload, sz);

	return sz;
}

int evdi_ioctl_connect(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_connect *cmd = data;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[0]);

	if (!cmd->connected) {
		if (cmd->display_id >= LINDROID_MAX_CONNECTORS)
			return -EINVAL;
		atomic_set(&evdi->events.stopping, 1);
		wake_up_interruptible(&evdi->events.wait_queue);
		mutex_lock(&evdi->config_mutex);
		evdi->displays[cmd->display_id].connected = false;
		mutex_unlock(&evdi->config_mutex);
		{
			int i, any = 0;
			for (i = 0; i < LINDROID_MAX_CONNECTORS; i++)
				any |= evdi->displays[i].connected;
			if (!any)
				WRITE_ONCE(evdi->drm_client, NULL);
		}
		evdi_smp_wmb();

		evdi_info("Device %d disconnected", evdi->dev_index);
#ifdef EVDI_HAVE_KMS_HELPER
		drm_kms_helper_hotplug_event(dev);
#else
		drm_helper_hpd_irq_event(dev);
#endif
		return 0;
	}

	if (evdi->drm_client && evdi->drm_client != file) {
		evdi_warn("Device %d forcefully disconnecting previous client",
			  evdi->dev_index);
		atomic_set(&evdi->events.stopping, 1);
		evdi_smp_wmb();
		wake_up_interruptible(&evdi->events.wait_queue);
	}

	if (cmd->display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	mutex_lock(&evdi->config_mutex);
	evdi->displays[cmd->display_id].connected = true;
	evdi->displays[cmd->display_id].width = cmd->width;
	evdi->displays[cmd->display_id].height = cmd->height;
	evdi->displays[cmd->display_id].refresh_rate = cmd->refresh_rate;
	mutex_unlock(&evdi->config_mutex);

	evdi_smp_wmb();
	WRITE_ONCE(evdi->drm_client, file);

	evdi_info("Device %d connected: %ux%u@%uHz id:%u", evdi->dev_index,
		  cmd->width, cmd->height, cmd->refresh_rate, cmd->display_id);

	atomic_set(&evdi->events.stopping, 0);

#ifdef EVDI_HAVE_KMS_HELPER
	drm_kms_helper_hotplug_event(dev);
#else
	drm_helper_hpd_irq_event(dev);
#endif
	return 0;
}

int evdi_ioctl_poll(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_poll *cmd = data;
	struct evdi_event *event;
	size_t payload_size;
	int ret;

	u8 payload_buf[EVDI_EVENT_PAYLOAD_MAX];

	event = evdi_event_dequeue(evdi);
	if (!event) {
		ret = evdi_event_wait(evdi, file);
		if (ret)
			return ret;

		event = evdi_event_dequeue(evdi);
		if (!event)
			return -EAGAIN;
	}

	cmd->event = event->type;
	cmd->poll_id = event->poll_id;

	payload_size = evdi_event_serialize_payload(event, payload_buf,
						    sizeof(payload_buf));

	if (cmd->data && payload_size) {
		if (copy_to_user(cmd->data, payload_buf, payload_size)) {
			evdi_event_free(event);
			return -EFAULT;
		}
	}

	evdi_event_free(event);
	return 0;
}

static inline void evdi_install_gralloc_fds(struct evdi_gralloc_data *gralloc,
					    int *fds, int max_fds)
{
	int i, nfd;

	if (!gralloc)
		return;

	nfd = clamp(gralloc->numFds, 0, max_fds);
	for (i = 0; i < nfd; i++) {
		if (gralloc->data_files[i])
			fd_install(fds[i], gralloc->data_files[i]);
	}
}

static inline void evdi_cleanup_gralloc_fds(struct evdi_gralloc_data *gralloc,
					    int *fds, int max_fds, int error)
{
	int i, nfd;

	if (!gralloc)
		return;

	nfd = clamp(gralloc->numFds, 0, max_fds);
	for (i = 0; i < nfd; i++) {
		if (error && gralloc->data_files[i])
			fput(gralloc->data_files[i]);
		gralloc->data_files[i] = NULL;

		if (error)
			put_unused_fd(fds[i]);
	}
}

int evdi_ioctl_gbm_get_buff(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_get_buff *cmd = data;
	struct evdi_inflight_req *req;
	struct drm_evdi_gbm_get_buff evt_params = { 0 };
	struct evdi_gralloc_buf_stack stack_buf;
	struct evdi_gralloc_buf_user *gralloc_buf = &stack_buf.buf;
	struct evdi_gralloc_data *gralloc;
	int poll_id;
	long ret;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[7]);

	memset(stack_buf.installed_fds, -1, sizeof(stack_buf.installed_fds));

	req = evdi_inflight_alloc(evdi, file, get_buf, &poll_id);
	if (!req)
		return -ENOMEM;

	evt_params.id = cmd->id;

	if (evdi_queue_event(evdi, get_buf, &evt_params, sizeof(evt_params),
			     file, poll_id)) {
		evdi_inflight_cancel(evdi, req, poll_id);
		ret = -ENOMEM;
		goto out;
	}

	ret = evdi_wait_req(req);
	if (ret)
		goto out;

	ret = evdi_process_gralloc_buffer(req, stack_buf.installed_fds,
					  gralloc_buf);
	if (ret)
		goto out;

	gralloc = req->reply.get_buf.gralloc_buf.gralloc;

	if (copy_to_user(cmd->native_handle, gralloc_buf,
			 sizeof(int) * (3 + gralloc_buf->numFds +
					gralloc_buf->numInts))) {
		ret = -EFAULT;
		goto cleanup;
	}

	evdi_install_gralloc_fds(gralloc, stack_buf.installed_fds,
				 EVDI_MAX_FDS);
	ret = 0;

cleanup:
	evdi_cleanup_gralloc_fds(gralloc, stack_buf.installed_fds, EVDI_MAX_FDS,
				 ret != 0);

out:
	evdi_inflight_req_put(req);
	return ret;
}

static inline bool evdi_file_valid(struct drm_file *file,
				   struct evdi_file_priv **priv_out)
{
	if (!file || !file->driver_priv)
		return false;
	*priv_out = file->driver_priv;
	return true;
}

static inline void evdi_file_buffer_op(struct drm_file *file, int id,
				       enum evdi_buffer_op op)
{
	struct evdi_file_priv *priv;

	if (unlikely(id <= 0))
		return;

	if (unlikely(!file || !file->driver_priv))
		return;

	priv = file->driver_priv;

	mutex_lock(&priv->lock);
	if (op == EVDI_BUF_TRACK)
		idr_alloc(&priv->buffers, (void *)1, id, id + 1, GFP_KERNEL);
	else
		idr_remove(&priv->buffers, id);
	mutex_unlock(&priv->lock);
}

static inline int evdi_copy_user_val(void __user *u_ptr, void *val, size_t size)
{
	return u_ptr ? copy_to_user(u_ptr, val, size) ? -EFAULT : 0 : 0;
}

int evdi_ioctl_gbm_create_buff(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_create_buff *cmd = data;
	struct evdi_inflight_req *req;
	struct drm_evdi_gbm_create_buff evt_params = { 0 };
	int poll_id, ret;

	if ((cmd->id && !evdi_access_ok_write(cmd->id, sizeof(*cmd->id))) ||
	    (cmd->stride &&
	     !evdi_access_ok_write(cmd->stride, sizeof(*cmd->stride))))
		return -EFAULT;

	req = evdi_inflight_alloc(evdi, file, create_buf, &poll_id);
	if (!req)
		return -ENOMEM;

	evt_params.format = cmd->format;
	evt_params.width = cmd->width;
	evt_params.height = cmd->height;

	if (evdi_queue_event(evdi, create_buf, &evt_params, sizeof(evt_params),
			     file, poll_id)) {
		evdi_inflight_cancel(evdi, req, poll_id);
		ret = -ENOMEM;
		goto out;
	}

	ret = evdi_wait_req(req);
	if (ret)
		goto out;

	ret = evdi_copy_user_val(cmd->id, &req->reply.create.id,
				 sizeof(*cmd->id));
	if (ret)
		goto out;

	evdi_file_buffer_op(file, req->reply.create.id, EVDI_BUF_TRACK);

	ret = evdi_copy_user_val(cmd->stride, &req->reply.create.stride,
				 sizeof(*cmd->stride));

out:
	evdi_inflight_req_put(req);
	return ret;
}

int evdi_ioctl_get_buff_callback(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_get_buff_callabck *cb = data;
	struct evdi_inflight_req *req;
	struct evdi_gralloc_data *gralloc = NULL;
	int i, j, nfd, nint, fds_local[EVDI_MAX_FDS];

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[3]);

	req = evdi_inflight_take(evdi, cb->poll_id);
	if (!req)
		goto out_wake;

	if (cb->numFds < 0 || cb->numInts < 0 || cb->numFds > EVDI_MAX_FDS ||
	    cb->numInts > EVDI_MAX_INTS)
		goto complete_req;

	nfd = cb->numFds;
	nint = cb->numInts;

	gralloc =
		mempool_alloc(global_event_pool.gralloc_data_pool, GFP_KERNEL);
	if (!gralloc)
		goto complete_req;

	memset(gralloc, 0, sizeof(*gralloc));
	gralloc->version = cb->version;

	if (nint && copy_from_user(gralloc->data_ints, cb->data_ints,
				   sizeof(int) * nint))
		goto fail_gralloc;
	gralloc->numInts = nint;

	if (nfd) {
		if (copy_from_user(fds_local, cb->fd_ints, sizeof(int) * nfd))
			goto fail_gralloc;

		for (i = 0; i < nfd; i++) {
			gralloc->data_files[i] = fget(fds_local[i]);
			if (!gralloc->data_files[i]) {
				for (j = 0; j < i; j++)
					fput(gralloc->data_files[j]);
				goto fail_gralloc;
			}
		}
		gralloc->numFds = nfd;
	}

	req->reply.get_buf.gralloc_buf.gralloc = gralloc;

complete_req:
	complete_all(&req->done);
	evdi_inflight_req_put(req);

out_wake:
	evdi_wakeup_pollers(evdi);
	return 0;

fail_gralloc:
	mempool_free(gralloc, global_event_pool.gralloc_data_pool);
	goto complete_req;
}

int evdi_ioctl_destroy_buff_callback(struct drm_device *dev, void *data,
				     struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[4]);

	evdi_wakeup_pollers(evdi);

	return 0;
}

int evdi_ioctl_create_buff_callback(struct drm_device *dev, void *data,
				    struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_create_buff_callabck *cb = data;
	struct evdi_inflight_req *req;

	EVDI_PERF_INC64(&evdi_perf.ioctl_calls[6]);

	req = evdi_inflight_take(evdi, cb->poll_id);
	if (!req) {
		evdi_warn("create_buff_callback: poll_id %d not found",
			  cb->poll_id);
		return 0;
	}

	req->reply.create.id = (cb->id < 0) ? 0 : cb->id;
	req->reply.create.stride = (cb->stride < 0) ? 0 : cb->stride;

	complete_all(&req->done);
	evdi_inflight_req_put(req);

	return 0;
}

int evdi_ioctl_gbm_del_buff(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_gbm_del_buff *cmd = data;
	long ret;

	ret = evdi_queue_event_autoid(evdi, destroy_buf, &cmd->id,
				      sizeof(cmd->id), file);
	if (!ret)
		evdi_file_buffer_op(file, cmd->id, EVDI_BUF_UNTRACK);

	return ret;
}

int evdi_queue_swap_event(struct evdi_device *evdi, int id, int display_id,
			  struct drm_file *owner)
{
	struct drm_file *client;
	struct evdi_swap sw;

	if (display_id < 0 || display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	if (atomic_read(&evdi->events.stopping))
		return -ENODEV;

	if ((client = READ_ONCE(evdi->drm_client)))
		owner = client;

	sw.id = id;
	sw.display_id = display_id;

	if (evdi_queue_event_autoid(evdi, swap_to, &sw, sizeof(sw), owner))
		return -ENOMEM;

	EVDI_PERF_INC64(&evdi_perf.swap_updates);
	return 0;
}

int evdi_ioctl_flipped(struct drm_device *dev, void *data,
		       struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_flipped *vs = data;
	struct evdi_pipe *ep;

	if (vs->display_id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	ep = &evdi->pipe[vs->display_id];

	smp_store_release(&ep->flipped, true);

	return 0;
}

int evdi_ioctl_cursor_set(struct drm_device *dev, void *data,
			  struct drm_file *file)
{
	return 0;
}

int evdi_ioctl_cursor_move(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	return 0;
}
