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

int evdi_ioctl_get_evdi_get_fd(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct evdi_device *evdi = dev->dev_private;
	struct drm_evdi_get_fd *cmd = data;
	struct drm_framebuffer *fb;
	struct evdi_framebuffer *efb;
	struct evdi_gem_object *obj;
	int i, id;

	id = cmd->display_id;
	if (id < 0 || id >= LINDROID_MAX_CONNECTORS)
		return -EINVAL;

	spin_lock(&evdi->fb_lock);
	fb = evdi->active_fb[id];

	if (fb)
		drm_framebuffer_get(fb);
	spin_unlock(&evdi->fb_lock);

	if (!fb)
		return -ENOENT;

	efb = to_evdi_fb(fb);

	if (!efb) {
		drm_framebuffer_put(fb);
		return -EINVAL;
	}

	cmd->buffer_id = efb->gralloc_buf_id;
	cmd->num_fds = efb->gem_count;

	for (i = 0; i < cmd->num_fds; i++) {

		struct drm_gem_object *gem = efb->gem_objs[i];
		struct file *f;
		int fd;

		if (!gem) {
			cmd->fds[i] = -1;
			continue;
		}

		obj = to_evdi_gem(gem);
		f = obj->dmabuf_file;

		if (!f) {
			cmd->fds[i] = -1;
			continue;
		}

		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			cmd->fds[i] = -1;
			continue;
		}

		get_file(f);
		fd_install(fd, f);
		cmd->fds[i] = fd;
	}

	return 0;
}

static inline bool evdi_file_valid(struct drm_file *file,
				   struct evdi_file_priv **priv_out)
{
	if (!file || !file->driver_priv)
		return false;
	*priv_out = file->driver_priv;
	return true;
}

static inline int evdi_copy_user_val(void __user *u_ptr, void *val, size_t size)
{
	return u_ptr ? copy_to_user(u_ptr, val, size) ? -EFAULT : 0 : 0;
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
