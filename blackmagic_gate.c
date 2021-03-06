/* -LICENSE-START-
** Copyright (c) 2013 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
**
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include <linux/kernel.h>
#include <linux/hash.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include "blackmagic_iml.h"
#include "blackmagic_core.h"

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
	#define raw_spinlock_t spinlock_t
	#define raw_spin_lock_irqsave spin_lock_irqsave
	#define raw_spin_unlock_irqrestore spin_unlock_irqrestore
	#define raw_spin_lock_irq spin_lock_irq
	#define raw_spin_unlock_irq spin_unlock_irq
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
	#define hlist_entry_safe(ptr, type, member) \
		({ typeof(ptr) ____ptr = (ptr); \
		   ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
		})

	#undef hlist_for_each_entry
	#define hlist_for_each_entry(pos, head, member) \
		for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); \
		     pos; \
		     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 25)
	#define TASK_NORMAL (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
	#define TASK_KILLABLE TASK_INTERRUPTIBLE
#endif

#define EVENT_TABLE_BITS 6
#define EVENT_TABLE_SIZE (1 << EVENT_TABLE_BITS)

struct blackmagic_gate_waiter
{
	struct task_struct	*task;
	struct list_head	list;
};

struct blackmagic_gate_event
{
	struct hlist_node	list;
	wait_queue_head_t	wqh;
	int					ref;
	void*				event;
};

struct blackmagic_gate_event_waiter
{
	wait_queue_t		wait;
	bool				triggered;
};

struct blackmagic_gate
{
	raw_spinlock_t					lock;
	unsigned int					count;
	struct list_head				wait_list;
	struct blackmagic_gate_waiter	*next;
	struct blackmagic_device		*dev;
	bool							run_bh_on_unlock;
	struct hlist_head				events[EVENT_TABLE_SIZE];
};

struct blackmagic_gate *dl_alloc_gate(void)
{
	int i;
	struct blackmagic_gate *gate = kmalloc(sizeof(struct blackmagic_gate), GFP_KERNEL);
	*gate = (struct blackmagic_gate) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
		.lock				= SPIN_LOCK_UNLOCKED,
#else
		.lock				= __RAW_SPIN_LOCK_UNLOCKED(gate->lock),
#endif
		.count				= 1,
		.wait_list			= LIST_HEAD_INIT(gate->wait_list),
		.next				= NULL,
		.dev				= NULL,
		.run_bh_on_unlock	= false,
	};

	for (i = 0; i < EVENT_TABLE_SIZE; ++i)
		INIT_HLIST_HEAD(&gate->events[i]);

	return gate;
}

void dl_free_gate(struct blackmagic_gate *gate)
{
	kfree(gate);
}

void dl_gate_set_device(struct blackmagic_gate *gate, void *dev)
{
	gate->dev = dev;
}

static void __sched __dl_gate_lock(struct blackmagic_gate *gate)
{
	long timeout = MAX_SCHEDULE_TIMEOUT;

	if (likely(gate->count > 0))
	{
		--gate->count;
	}
	else
	{
		struct task_struct *task = current;
		struct blackmagic_gate_waiter waiter;

		list_add_tail(&waiter.list, &gate->wait_list);
		waiter.task = task;

		for (;;)
		{
			if (timeout <= 0)
			{
				list_del(&waiter.list);
				break;
			}
			__set_task_state(task, TASK_UNINTERRUPTIBLE);
			raw_spin_unlock_irq(&gate->lock);
			timeout = schedule_timeout(timeout);
			raw_spin_lock_irq(&gate->lock);
			if (gate->next == &waiter)
			{
				gate->next = NULL;
				break;
			}
		}
	}
}

void __sched dl_gate_lock(struct blackmagic_gate *gate)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&gate->lock, flags);
		__dl_gate_lock(gate);
	raw_spin_unlock_irqrestore(&gate->lock, flags);
}

bool dl_gate_lock_interrupt(struct blackmagic_gate *gate)
{
	unsigned long flags;
	int count;
	bool locked = false;

	raw_spin_lock_irqsave(&gate->lock, flags);
	count = gate->count - 1;
	if (likely(count >= 0))
	{
		gate->count = count;
		locked = true;
	}
	else if (gate->next != NULL)
	{
		list_add(&gate->next->list, &gate->wait_list);
		gate->next = NULL;
		locked = true;
	}
	if (!locked)
		gate->run_bh_on_unlock = true;
	raw_spin_unlock_irqrestore(&gate->lock, flags);

	return locked;
}

static void __dl_gate_unlock(struct blackmagic_gate *gate)
{
	unsigned int status;

	if (gate->run_bh_on_unlock)
	{
		gate->run_bh_on_unlock = false;
		raw_spin_unlock_irq(&gate->lock);
		if (gate->dev)
		{
			status = dl_tasklet_handler_gated(gate->dev->driver);
			if (status & DL_INTERRUPT_SCHED_WORK)
				schedule_work(&gate->dev->work);
		}
		raw_spin_lock_irq(&gate->lock);
	}

	if (likely(list_empty(&gate->wait_list)))
	{
		++gate->count;
	}
	else
	{
		struct blackmagic_gate_waiter *waiter = list_first_entry(&gate->wait_list,
			struct blackmagic_gate_waiter, list);
		list_del(&waiter->list);
		gate->next = waiter;
		wake_up_process(waiter->task);
	}
}

void dl_gate_unlock(struct blackmagic_gate *gate)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&gate->lock, flags);
		__dl_gate_unlock(gate);
	raw_spin_unlock_irqrestore(&gate->lock, flags);
}

static struct blackmagic_gate_event* get_event(struct blackmagic_gate* gate, void* event, bool create)
{
	struct blackmagic_gate_event* ev;
	unsigned idx = hash_ptr(event, EVENT_TABLE_BITS);

	hlist_for_each_entry(ev, &gate->events[idx], list)
	{
		if (ev->event == event)
		{
			++ev->ref;
			goto done;
		}
	}

	if (create)
	{
		ev = kmalloc(sizeof(struct blackmagic_gate_event), GFP_ATOMIC);
		if (!ev)
			return NULL;

		*ev = (struct blackmagic_gate_event) {
			.list = { NULL, NULL },
			.wqh = __WAIT_QUEUE_HEAD_INITIALIZER(ev->wqh),
			.ref = 1,
			.event = event
		};
		hlist_add_head(&ev->list, &gate->events[idx]);
	}
	else
	{
		ev = NULL;
	}

done:
	return ev;
}

static void put_event(struct blackmagic_gate_event* event)
{
	if (--event->ref == 0)
	{
		hlist_del(&event->list);
		kfree(event);
	}
}

int dl_gate_sleep(struct blackmagic_gate *gate, void* key)
{
	int result = 0;
	struct blackmagic_gate_event* event = NULL;
	struct blackmagic_gate_event_waiter waiter;

	init_wait(&waiter.wait);
	waiter.triggered = false;

	raw_spin_lock_irq(&gate->lock);

	// Ensure we have an event in the list
	event = get_event(gate, key, true);
	if (!event)
	{
		result = -ENOMEM;
		goto bail;
	}

	// Release the gate
	__dl_gate_unlock(gate);

	spin_lock(&event->wqh.lock);

	// Wait for the event
	for (;;)
	{
		waiter.wait.flags &= ~WQ_FLAG_EXCLUSIVE;
		if (list_empty(&waiter.wait.task_list))
			__add_wait_queue_tail(&event->wqh, &waiter.wait);
		set_current_state(TASK_INTERRUPTIBLE);

		if (waiter.triggered)
			break;

		if (!signal_pending(current))
		{
			spin_unlock(&event->wqh.lock);
			raw_spin_unlock_irq(&gate->lock);
			schedule();
			raw_spin_lock_irq(&gate->lock);
			spin_lock(&event->wqh.lock);
			continue;
		}

		result = -ERESTARTSYS;

		break;
	}
	__remove_wait_queue(&event->wqh, &waiter.wait);
	__set_current_state(TASK_RUNNING);

	spin_unlock(&event->wqh.lock);

	// Clean up the event
	put_event(event);

	// Acquire the gate
	__dl_gate_lock(gate);

bail:
	raw_spin_unlock_irq(&gate->lock);

	return result != 0 ? THREAD_INTERRUPTED : THREAD_AWAKENED;
}

void dl_gate_wakeup(struct blackmagic_gate *gate, void* key)
{
	struct blackmagic_gate_event* event = NULL;
	struct list_head *tmp, *next;
	unsigned long flags;

	raw_spin_lock_irqsave(&gate->lock, flags);
	event = get_event(gate, key, false);
	raw_spin_unlock_irqrestore(&gate->lock, flags);

	if (!event)
		return;

	spin_lock_irqsave(&event->wqh.lock, flags);
	list_for_each_safe(tmp, next, &event->wqh.task_list)
	{
		wait_queue_t* curr = list_entry(tmp, wait_queue_t, task_list);
		struct blackmagic_gate_event_waiter* waiter = container_of(curr, struct blackmagic_gate_event_waiter, wait);

		waiter->triggered = true;

		if (curr->func(curr, TASK_NORMAL, 0, NULL))
			break;
	}
	spin_unlock_irqrestore(&event->wqh.lock, flags);

	raw_spin_lock_irqsave(&gate->lock, flags);
	put_event(event);
	raw_spin_unlock_irqrestore(&gate->lock, flags);
}
