/* -LICENSE-START-
** Copyright (c) 2009-2013 Blackmagic Design
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
#include <linux/sched.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include "blackmagic_core.h"

extern struct blackmagic_device *blackmagic_find_device_by_id(int);
extern struct blackmagic_device *blackmagic_find_device_by_ptr(void *);

static struct tty_driver *blackmagic_tty_driver = NULL;

static inline void *get_driver_from_serial(struct blackmagic_serial *sdev)
{
	return container_of(sdev, struct blackmagic_device, sdev)->driver;
}

static inline struct tty_struct *get_tty_from_serial(struct blackmagic_serial *sdev)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
	return tty_port_tty_get(&sdev->port);
#else
	return sdev->tty;
#endif
}

static inline struct blackmagic_serial *find_serial_by_tty(struct tty_struct *tty)
{
	struct blackmagic_device *ddev = blackmagic_find_device_by_id(tty->index);
	if (!ddev)
		return ERR_PTR(-ENODEV);

	if (!(ddev->flags & BLACKMAGIC_DEV_HAS_SERIAL))
		return ERR_PTR(-ENODEV);

	return &ddev->sdev;
}

static inline struct blackmagic_serial *find_serial_by_ptr(void *ptr)
{
	struct blackmagic_device *ddev = blackmagic_find_device_by_ptr(ptr);
	if (!ddev)
		return ERR_PTR(-ENODEV);

	if (!(ddev->flags & BLACKMAGIC_DEV_HAS_SERIAL))
		return ERR_PTR(-ENODEV);

	return &ddev->sdev;
}

static inline int test_and_change_open_state(struct blackmagic_serial *sdev, enum blackmagic_serial_open_states required_state, enum blackmagic_serial_open_states new_state)
{
	if (sdev->open_state == required_state)
	{
		sdev->open_state = new_state;
		return 0;
	}
	return -EBUSY;
}

static void blackmagic_serial_reset_buffers(struct blackmagic_serial *sdev)
{
	struct blackmagic_serial_buffer *buffer = &sdev->write_buffer;
	
	/* init write buffer */
	memset(buffer->data, 0x0, sizeof(BLACKMAGIC_SERIAL_BUFFER_SIZE));
	buffer->available_bytes = 0;
	buffer->end = buffer->data + BLACKMAGIC_SERIAL_BUFFER_SIZE;
	buffer->next = buffer->last = buffer->data;

	/* init read buffer */
	buffer = &sdev->read_buffer;
	
	memset(buffer->data, 0x0, sizeof(BLACKMAGIC_SERIAL_BUFFER_SIZE));
	buffer->available_bytes = 0;
	buffer->end = buffer->data + BLACKMAGIC_SERIAL_BUFFER_SIZE;
	buffer->next = buffer->last = buffer->data;
}

static int blackmagic_serial_open_common(struct blackmagic_serial *sdev, struct tty_struct *tty)
{
	unsigned long iflags;
	int ret = 0;

	spin_lock_irqsave(&sdev->lock, iflags);
		ret = test_and_change_open_state(sdev, PORT_CLOSED, tty ? PORT_OPEN_TTY : PORT_OPEN_IOCTL);
		if (ret < 0)
			goto abort;
		atomic_set(&sdev->tx_interrupt_pending, 0);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		sdev->tty = tty;
#endif
abort:
	spin_unlock_irqrestore(&sdev->lock, iflags);

	return ret;
}

int blackmagic_serial_open_ioctl(void *dev)
{
	struct blackmagic_serial *sdev = find_serial_by_ptr(dev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);

	return blackmagic_serial_open_common(sdev, NULL);
}

static int blackmagic_serial_open_tty(struct tty_struct *tty, struct file *file)
{
	struct blackmagic_serial *sdev;
	int ret;

	sdev = find_serial_by_tty(tty);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);

	ret = blackmagic_serial_open_common(sdev, tty);
	if (ret < 0)
		return ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
	return ret;
#else
	return tty_port_open(&sdev->port, tty, file);
#endif
}

int blackmagic_serial_port_is_in_use(void *dev)
{
	bool in_use = false;
	unsigned long iflags;
	struct blackmagic_serial *sdev;

	sdev = find_serial_by_ptr(dev);
	if (IS_ERR(sdev))
		return false;

	spin_lock_irqsave(&sdev->lock, iflags);
		in_use = (sdev->open_state != PORT_CLOSED);
	spin_unlock_irqrestore(&sdev->lock, iflags);

	return in_use;
}

static int blackmagic_serial_close_common(struct blackmagic_serial *sdev, enum blackmagic_serial_open_states state)
{
	unsigned long iflags;
	int ret = 0;

	spin_lock_irqsave(&sdev->lock, iflags);
		ret = test_and_change_open_state(sdev, state, PORT_CLOSED);
		if (ret < 0)
			goto abort;
		blackmagic_serial_reset_buffers(sdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		sdev->tty = NULL;
#endif
abort:
	spin_unlock_irqrestore(&sdev->lock, iflags);

	return ret;
}

int blackmagic_serial_close_ioctl(void *dev)
{
	struct blackmagic_serial *sdev = find_serial_by_ptr(dev);
	if (IS_ERR(sdev))
		return PTR_ERR(sdev);

	return blackmagic_serial_close_common(sdev, PORT_OPEN_IOCTL);
}

static void blackmagic_serial_close_tty(struct tty_struct *tty, struct file *file)
{
	struct blackmagic_serial *sdev = find_serial_by_tty(tty);
	if (IS_ERR(sdev))
		return;

	if (blackmagic_serial_close_common(sdev, PORT_OPEN_TTY) < 0)
		return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
	tty_port_close(&sdev->port, tty, file);
#endif
}

/* Dequeue data from the read buffer - Must be called only when port open in PORT_OPEN_IOCTL mode
 * Called from DaisyCutterDriver
 */
int blackmagic_serial_dequeue_data(void *dev, unsigned char *data, int count)
{
	struct blackmagic_serial *sdev;
	struct blackmagic_serial_buffer *buffer;
	unsigned long iflags;
	int bytes_read = -EBUSY;

	sdev = find_serial_by_ptr(dev);
	if (IS_ERR(sdev))
		return 0;

	buffer = &sdev->read_buffer;

	if (!spin_trylock_irqsave(&sdev->lock, iflags))
		return 0;

	if (sdev->open_state != PORT_OPEN_IOCTL)
		goto abort;

	bytes_read = 0;

	while (bytes_read < count)
	{
		if (buffer->last == buffer->next)
			break;

		/* copy byte */
		*(data++) = *(buffer->last++);

		bytes_read++;

		if (buffer->last >= buffer->end)
			buffer->last = buffer->data;
	}

abort:
	spin_unlock_irqrestore(&sdev->lock, iflags);
	return bytes_read;
}

/*
 * Called by on RX interrupt (in hard IRQ context)
 */
void blackmagic_serial_rx_interrupt(void *driver)
{
	unsigned long iflags;
	struct blackmagic_serial *sdev;
	struct blackmagic_serial_buffer *buffer;
	int rx_len;
	int i;
	unsigned char c;

	sdev = find_serial_by_ptr(driver);
	if (IS_ERR(sdev))
		return;

	spin_lock_irqsave(&sdev->lock, iflags);

	/* The device is not open, clear the interrupt without doing anything */
	if (sdev->open_state == PORT_CLOSED)
	{
		blackmagic_serial_clear_rx_buffer(driver);
		goto out;
	}

	rx_len = blackmagic_serial_read_len_priv(driver);
	if (!rx_len)
		goto out;

	buffer = &sdev->read_buffer;

	for (i = 0; i < rx_len; i++)
	{
		c = blackmagic_serial_read_byte_priv(driver);

		// check if we are open in IOCTL or TTY mode
		if (sdev->open_state == PORT_OPEN_TTY)
		{
			// opened in TTY mode, pass bytes to the TTY layer
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
			if (tty_insert_flip_char(sdev->tty, c, TTY_NORMAL) == 0)
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
			if (tty_insert_flip_char(sdev->port.tty, c, TTY_NORMAL) == 0)
#else
			if (tty_insert_flip_char(&sdev->port, c, TTY_NORMAL) == 0)
#endif
				break;
		}
		else
		{
			// opened in IOCTL mode, place bytes in read buffer
			// dont check if buffer is full, just overwrite previous data
			*(buffer->next++) = c;

			if (buffer->next >= buffer->end)
				buffer->next = buffer->data;

		}
	}

	if (sdev->open_state == PORT_OPEN_TTY)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
		tty_flip_buffer_push(sdev->tty);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
		tty_flip_buffer_push(sdev->port.tty);
#else
		tty_flip_buffer_push(&sdev->port);
#endif

out:
	spin_unlock_irqrestore(&sdev->lock, iflags);
	return;
}

/*
 * Setup a new serial transfer. Caller must hold serial device lock.
 */
static void blackmagic_serial_setup_tx(struct blackmagic_serial *sdev)
{
	struct blackmagic_serial_buffer *buffer = &sdev->write_buffer;
	void *driver = get_driver_from_serial(sdev);
	int tx_bytes = 0;
	
	/* We need to wait until there are no pending interrupts so we don't overwrite
	   untransmitted data */
	if (atomic_read(&sdev->tx_interrupt_pending) != 0)
		return;
	
	while (tx_bytes < BLACKMAGIC_HW_TX_SIZE)
	{
		if (buffer->last == buffer->next)
			break;

		/* Write a byte to HW registers */
		blackmagic_serial_write_byte_priv(driver, *(buffer->last++));

		if (buffer->last >= buffer->end)
			buffer->last = buffer->data;

		tx_bytes++;
	}

	if (! tx_bytes)
		return;
	
	/* Set transfer size */
	blackmagic_serial_write_byte_size_priv(driver, tx_bytes - 1);
	
	buffer->available_bytes -= tx_bytes;
	atomic_set(&sdev->tx_interrupt_pending, 1);
}

/*
 * Handle serial TX interrupts. Not called from actual interrupt
 * context.
 */
void blackmagic_serial_tx_interrupt(void *driver, int continue_tx)
{
	unsigned long iflags;
	struct blackmagic_serial *sdev = NULL;
	struct tty_struct *tty;

	sdev = find_serial_by_ptr(driver);
	if (IS_ERR(sdev))
		return;

	spin_lock_irqsave(&sdev->lock, iflags);

		if (sdev->open_state == PORT_CLOSED)
			goto abort;

		atomic_set(&sdev->tx_interrupt_pending, 0);

		if (continue_tx)
			blackmagic_serial_setup_tx(sdev);

		/*
		 * Signal the writer to indicate more room in input buffer, as we have now emptied out
		 * BLACKMAGIC_HW_TX_SIZE bytes.
		 */
		if (sdev->open_state == PORT_OPEN_TTY)
		{
			tty = get_tty_from_serial(sdev);
			wake_up(&tty->write_wait);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
			tty_kref_put(tty);
#endif
		}

abort:
	spin_unlock_irqrestore(&sdev->lock, iflags);
}

/* Queue data in the write buffer and send it over the serial port if possible
 * Called from this driver (because of a userspace write() ) and from DaisyCutterDriver
 */
int blackmagic_serial_enqueue_data(void *dev, const unsigned char *data, int count)
{
	struct blackmagic_serial *sdev;
	struct blackmagic_serial_buffer *buffer;
	unsigned long iflags;
	unsigned char *ptr;
	int write_bytes = 0;

	sdev = find_serial_by_ptr(dev);
	if (IS_ERR(sdev))
		return 0;
	
	buffer = &sdev->write_buffer;
	
	spin_lock_irqsave(&sdev->lock, iflags);
	while (write_bytes < count)
	{
		ptr = buffer->next + 1;
		if (ptr >= buffer->end)
			ptr = buffer->data;
		
		/* Buffer is full */
		if (ptr == buffer->last)
			break;
		/* copy data */
		*buffer->next = *(data++);
		buffer->next = ptr;

		write_bytes++;
	}
	buffer->available_bytes += write_bytes;

	if (!write_bytes)
		goto abort;

	/*
	* If the HW is not already transmitting something, start
	* pushing out data.
	*/
	blackmagic_serial_setup_tx(sdev);

abort:
	spin_unlock_irqrestore(&sdev->lock, iflags);
	return write_bytes;
}

static int blackmagic_serial_write(struct tty_struct *tty,
		      const unsigned char *data, int count)
{
	struct blackmagic_serial *sdev = find_serial_by_tty(tty);

	if (IS_ERR(sdev) || sdev->open_state == PORT_CLOSED)
		return -ENODEV;

	if (sdev->open_state == PORT_OPEN_IOCTL)
		return -EBUSY;

	return blackmagic_serial_enqueue_data(get_driver_from_serial(sdev), data, count);
}

static int blackmagic_serial_write_room(struct tty_struct *tty)
{
	unsigned long iflags;
	struct blackmagic_serial *sdev = find_serial_by_tty(tty);
	struct blackmagic_serial_buffer *buffer;
	int room = 0;

	if (IS_ERR(sdev) || sdev->open_state == PORT_CLOSED)
		return -ENODEV;

	if (sdev->open_state == PORT_OPEN_IOCTL)
		return -EBUSY;

	buffer = &sdev->write_buffer;

	spin_lock_irqsave(&sdev->lock, iflags);
	room = BLACKMAGIC_SERIAL_BUFFER_SIZE - 1 - buffer->available_bytes;
	spin_unlock_irqrestore(&sdev->lock, iflags);

	return room;
}

static int blackmagic_serial_chars_in_buffer(struct tty_struct *tty)
{
	unsigned long iflags;
	struct blackmagic_serial *sdev = find_serial_by_tty(tty);
	int chars = 0;

	if (IS_ERR(sdev) || sdev->open_state == PORT_CLOSED)
		return -ENODEV;

	if (sdev->open_state == PORT_OPEN_IOCTL)
		return -EBUSY;

	spin_lock_irqsave(&sdev->lock, iflags);
	chars = sdev->write_buffer.available_bytes;
	spin_unlock_irqrestore(&sdev->lock, iflags);

	return chars;
}

static struct tty_operations blackmagic_tty_ops =
{
	.close = blackmagic_serial_close_tty,
	.open = blackmagic_serial_open_tty,
	.write = blackmagic_serial_write,
	.write_room = blackmagic_serial_write_room,
	.chars_in_buffer = blackmagic_serial_chars_in_buffer,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
static struct tty_port_operations blackmagic_tty_port_ops =
{
	/* No ops needed at this time */
};
#endif

int blackmagic_serial_port_path(void* dev, char *buffer, int len)
{
	struct blackmagic_device *ddev = blackmagic_find_device_by_ptr(dev);
	size_t path_len;
	size_t i;

	if (!ddev || !(ddev->flags & BLACKMAGIC_DEV_HAS_SERIAL))
	{
		buffer[0] = '\0';
		return -ENODEV;
	}

	path_len = snprintf(buffer, len - 1, "/dev/%s%u", blackmagic_tty_driver->name, ddev->id);

	buffer[len - 1] = '\0';

	for (i = 5; i < path_len; ++i)
		if (buffer[i] == '!')
			buffer[i] = '/';

	return 0;
}

int blackmagic_serial_probe(struct blackmagic_device *ddev, struct device *dev)
{
	struct blackmagic_serial *sdev = &ddev->sdev;
	void *tty_dev;

	if (ddev->id > BLACKMAGIC_SERIAL_MINORS)
		return -ERANGE;

	spin_lock_init(&sdev->lock);
	sdev->open_state = PORT_CLOSED;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
	sdev->tty = NULL;
#endif

	/* init our ring buffers */
	blackmagic_serial_reset_buffers(sdev);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
	tty_port_init(&sdev->port);
	blackmagic_tty_driver->ports[ddev->id] = &sdev->port;
	sdev->port.ops = &blackmagic_tty_port_ops;
#endif

	tty_dev = tty_register_device(blackmagic_tty_driver, ddev->id, dev);
	if (IS_ERR(tty_dev))
	{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
		tty_port_destroy(&sdev->port);
#endif
		return PTR_ERR(tty_dev);
	}

	return 0;
}

void blackmagic_serial_remove(struct blackmagic_device *ddev)
{
	tty_unregister_device(blackmagic_tty_driver, ddev->id);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 8, 0)
	tty_port_destroy(&ddev->sdev.port);
#endif
}

int __init blackmagic_serial_init(void)
{
	int ret;
	struct tty_driver *driver;

	driver = alloc_tty_driver(BLACKMAGIC_SERIAL_MINORS);
	if (!driver)
		return -ENOMEM;

	driver->owner = THIS_MODULE;
	driver->driver_name = "blackmagic_serial";
	driver->name = "blackmagic!ttydv";
	driver->major = 0,
	driver->type = TTY_DRIVER_TYPE_SERIAL,
	driver->subtype = SERIAL_TYPE_NORMAL,
	driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV,
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	driver->init_termios.c_lflag = 0;
	tty_set_operations(driver, &blackmagic_tty_ops);

	ret = tty_register_driver(driver);
	if (ret)
		goto abort;

	blackmagic_tty_driver = driver;

	return 0;

abort:
	printk(KERN_ERR "failed to register blackmagic serial driver");
	put_tty_driver(driver);
	return ret;
}

void __exit blackmagic_serial_exit(void)
{
	if (blackmagic_tty_driver)
	{
		tty_unregister_driver(blackmagic_tty_driver);
		put_tty_driver(blackmagic_tty_driver);
	}
}
