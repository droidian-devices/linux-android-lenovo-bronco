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
#include <linux/sched.h>
#include <linux/prefetch.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>

struct evdi_event_pool global_event_pool = { 0 };
DEFINE_STATIC_KEY_FALSE(evdi_perf_key);
bool evdi_perf_on;
struct evdi_perf_counters evdi_perf;

static inline void *evdi_mempool_kvzalloc(gfp_t gfp_mask, void *pool_data)
{
	return kvzalloc((size_t)pool_data, gfp_mask);
}

static inline void evdi_mempool_kvfree(void *element, void *pool_data)
{
	kvfree(element);
}

static inline void evdi_kmem_cache_destroy(struct kmem_cache **cache)
{
	if (*cache) {
		kmem_cache_destroy(*cache);
		*cache = NULL;
	}
}

static inline struct kmem_cache *evdi_event_cache(enum poll_event_type type)
{
	if (type >= 0 && type < EVDI_EVENT_TYPE_MAX)
		return global_event_pool.type_cache[type];

	return global_event_pool.cache;
}

int evdi_event_system_init(void)
{
	char name[32];
	int i;

	global_event_pool.cache =
		kmem_cache_create("evdi_events", sizeof(struct evdi_event), 0,
				  SLAB_HWCACHE_ALIGN, NULL);

	if (!global_event_pool.cache)
		return -ENOMEM;

	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++) {
		snprintf(name, sizeof(name), "evdi_events_%d", i);

		global_event_pool.type_cache[i] =
			kmem_cache_create(name, sizeof(struct evdi_event), 0,
					  SLAB_HWCACHE_ALIGN, NULL);

		if (!global_event_pool.type_cache[i])
			goto err_cache;
	}

	memset(&evdi_perf, 0, sizeof(evdi_perf));
	evdi_perf_on = false;
	evdi_smp_wmb();

	evdi_info("Event system initialized");
	return 0;

err_cache:
	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++)
		evdi_kmem_cache_destroy(&global_event_pool.type_cache[i]);

	evdi_kmem_cache_destroy(&global_event_pool.cache);
	return -ENOMEM;
}

void evdi_event_system_cleanup(void)
{
	int i;

	for (i = 0; i < EVDI_EVENT_TYPE_MAX; i++)
		evdi_kmem_cache_destroy(&global_event_pool.type_cache[i]);

	evdi_kmem_cache_destroy(&global_event_pool.cache);

	evdi_debug("Event system cleaned up");
}

int evdi_event_init(struct evdi_device *evdi)
{
	spin_lock_init(&evdi->events.lock);
	init_waitqueue_head(&evdi->events.wait_queue);

	INIT_LIST_HEAD(&evdi->events.high_prio);
	INIT_LIST_HEAD(&evdi->events.normal);

	atomic_set(&evdi->events.cleanup_in_progress, 0);
	atomic_set(&evdi->events.stopping, 0);
	atomic_set(&evdi->events.next_poll_id, 1);
	atomic_set(&evdi->events.wake_pending, 0);

	atomic64_set(&evdi->events.events_queued, 0);
	atomic64_set(&evdi->events.events_dequeued, 0);

	evdi_smp_wmb();

	evdi_debug("Event system initialized for device %d", evdi->dev_index);
	return 0;
}

struct evdi_event *evdi_event_alloc(struct evdi_device *evdi,
				    enum poll_event_type type, int poll_id,
				    void *data, size_t data_size,
				    struct drm_file *owner)
{
	struct evdi_event *event;
	struct kmem_cache *cache = evdi_event_cache(type);

	event = kmem_cache_alloc(cache, GFP_ATOMIC);
	if (!event)
		return NULL;

	event->from_pool = true;
	event->cache_idx = (cache != global_event_pool.cache) ? type : 0xff;

	event->type = type;
	event->poll_id = poll_id;

	event->payload_size =
		min_t(size_t, data_size, (size_t)EVDI_EVENT_PAYLOAD_MAX);

	if (data && data_size)
		memcpy(event->payload, data, event->payload_size);

	event->owner = owner;
	event->evdi = evdi;
	atomic_set(&event->freed, 0);

	EVDI_PERF_INC64(&evdi_perf.allocs);
	return event;
}

static void evdi_event_free_rcu_cb(struct rcu_head *head)
{
	struct evdi_event *event = container_of(head, struct evdi_event, rcu);

	if (likely(event->from_pool)) {
		struct kmem_cache *cache =
			(event->cache_idx < EVDI_EVENT_TYPE_MAX) ?
				global_event_pool.type_cache[event->cache_idx] :
				global_event_pool.cache;

		kmem_cache_free(cache, event);
	} else {
		kfree(event);
	}
}

void evdi_event_free(struct evdi_event *event)
{
	if (!event || atomic_xchg(&event->freed, 1))
		return;

	call_rcu(&event->rcu, evdi_event_free_rcu_cb);
}

static inline bool evdi_has_events(struct evdi_device *evdi)
{
	return !list_empty(&evdi->events.high_prio) ||
	       !list_empty(&evdi->events.normal);
}

static inline bool evdi_event_enqueue(struct evdi_device *evdi,
				      struct evdi_event *event)
{
	if (unlikely(atomic_read(&evdi->events.cleanup_in_progress) ||
		     atomic_read(&evdi->events.stopping))) {
		evdi_event_free(event);
		return false;
	}

	spin_lock(&evdi->events.lock);

	if (event->type == swap_to)
		list_add_tail(&event->node, &evdi->events.high_prio);
	else
		list_add_tail(&event->node, &evdi->events.normal);

	spin_unlock(&evdi->events.lock);

	atomic64_inc(&evdi->events.events_queued);
	EVDI_PERF_INC64(&evdi_perf.event_queue_ops);

	wake_up_interruptible(&evdi->events.wait_queue);

	return true;
}

void evdi_event_queue(struct evdi_device *evdi, struct evdi_event *event)
{
	if (evdi && event)
		evdi_event_enqueue(evdi, event);
}

struct evdi_event *evdi_event_dequeue(struct evdi_device *evdi)
{
	struct evdi_event *event = NULL;

	spin_lock(&evdi->events.lock);

	if (!list_empty(&evdi->events.high_prio)) {
		event = list_first_entry(&evdi->events.high_prio,
					 struct evdi_event, node);
		list_del(&event->node);

	} else if (!list_empty(&evdi->events.normal)) {
		event = list_first_entry(&evdi->events.normal,
					 struct evdi_event, node);
		list_del(&event->node);
	}

	spin_unlock(&evdi->events.lock);

	if (event) {
		atomic64_inc(&evdi->events.events_dequeued);
		EVDI_PERF_INC64(&evdi_perf.event_dequeue_ops);
	}

	return event;
}

int evdi_event_wait(struct evdi_device *evdi)
{
	int ret;

	ret = wait_event_interruptible(evdi->events.wait_queue,
				       atomic_read(&evdi->events.stopping) ||
					       evdi_has_events(evdi));

	if (ret)
		return ret;

	if (atomic_read(&evdi->events.stopping))
		return -ENODEV;

	return 0;
}

void evdi_event_cleanup_file(struct evdi_device *evdi, struct drm_file *file)
{
	struct evdi_event *event, *tmp;

	if (!evdi || !file)
		return;

	atomic_set(&evdi->events.cleanup_in_progress, 1);

	spin_lock(&evdi->events.lock);

	list_for_each_entry_safe (event, tmp, &evdi->events.high_prio, node) {
		if (event->owner == file) {
			list_del(&event->node);
			call_rcu(&event->rcu, evdi_event_free_rcu_cb);
		}
	}

	list_for_each_entry_safe (event, tmp, &evdi->events.normal, node) {
		if (event->owner == file) {
			list_del(&event->node);
			call_rcu(&event->rcu, evdi_event_free_rcu_cb);
		}
	}

	spin_unlock(&evdi->events.lock);

	atomic_set(&evdi->events.cleanup_in_progress, 0);
	evdi_smp_wmb();

	wake_up_interruptible(&evdi->events.wait_queue);
}

void evdi_event_queue_reset(struct evdi_device *evdi)
{
	struct evdi_event *e, *tmp;

	spin_lock(&evdi->events.lock);

	list_for_each_entry_safe (e, tmp, &evdi->events.normal, node) {
		list_del(&e->node);
		kfree(e);
	}

	list_for_each_entry_safe (e, tmp, &evdi->events.high_prio, node) {
		list_del(&e->node);
		kfree(e);
	}

	spin_unlock(&evdi->events.lock);
}