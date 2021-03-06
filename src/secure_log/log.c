#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/in.h>
#include <linux/ipv6.h>
#include <linux/module.h>
#include <linux/version.h>
#include "log.h"
#include "sparse_compat.h"
#include "current_details.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vincent Brillault <vincent.brillault@cern.ch>");
MODULE_DESCRIPTION("Create a new logging device, /dev/"MODULE_NAME);
MODULE_VERSION("0.3");

static int simple_format;
module_param(simple_format, int, 0664);
MODULE_PARM_DESC(simple_format, "Use a simpler out format than syslog RFC, only valid for new open call on the device");

static int send_eof;
module_param(send_eof, int, 0664);
MODULE_PARM_DESC(send_eof, "Return a EOF at the current end of the buffer, only valid for new open call on the device");


/*
 * This kernel module is heavily inspired from linux/kernel/printk.c
 * Here is the original copyright notice on that file:
 ******
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 * Modified to make sys_syslog() more flexible: added commands to
 * return the last 4k of kernel messages, regardless of whether
 * they've been read or not.  Added option to suppress kernel printk's
 * to the console.  Added hook for sending the console messages
 * elsewhere, in preparation for a serial line console (someday).
 * Ted Ts'o, 2/11/93.
 * Modified for sysctl support, 1/8/97, Chris Horn.
 * Fixed SMP synchronization, 08/08/99, Manfred Spraul
 *     manfred@colorfullife.com
 * Rewrote bits to get rid of console_lock
 *      01Mar01 Andrew Morton
 ******
 */

/* Log structures of records stored the buffer */
struct sec_log {
	size_t len /** Total size of the record, including the strings at the end */;
	struct current_details process /* Details of the process */;
	enum secure_log_type type /** Type of this record (for cast)*/;
};

struct netlog_log {
	struct sec_log header    /** Mandatory header */;
	size_t path_len          /** Length of the path of the executable responsible for the activity, including the tailing '\0'. The string is accessible via get_netlog_path */;
	enum netlog_protocol protocol /** Network protocol used (currently supported: UDP & TCP */;
	enum netlog_action action /** Type of call used (currently supported: bind, connect, accept, close */;
	unsigned short family    /** Familly of the socket used (currently supported: AF_INET, AF_INET6 */;
	int src_port             /** Source port (local) */;
	int dst_port             /** Destination port (distant) */;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
		u8 raw[16];
	} src                    /** Source address (local) */;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
		u8 raw[16];
	} dst                    /** Destination address (distant) */;
};

struct execlog_log {
	struct sec_log header /** Mandatory header */;
	size_t path_len       /** Length of the path of the executable, including the tailing '\0'. The string is accessible via get_netlog_path */;
	size_t argv_len       /** Length of the arguments given to the executable including the tailing '\0'. The string is accessible via get_netlog_argv. MUST be set after the 'path_len' */;
};

/* The bigger structure is definitely the netlog_log one */
#define LOG_ALIGN __alignof__(struct netlog_log)

/* Buffer */
static char log_buf[LOG_BUF_LEN];

/* index and sequence number of the first record stored in the buffer
 * Note that there is no code to handle overflow of the sequence number
 * as it's 64bits and even at 16K logs per second, it would need 30
 * million years of constant running to overflow
 */
static u64 log_first_seq = 0;
static u32 log_first_idx = 0;

/* index and sequence number of the next record to store in the buffer
 * Note that there is no code to handle overflow of the sequence number
 * as it's 64bits and even at 16K logs per second, it would need 30
 * million years of constant running to overflow
 */
static u64 log_next_seq = 0;
static u32 log_next_idx = 0;

/* Buffer protection */
static DEFINE_SPINLOCK(log_lock);

/* Poll queue */
static DECLARE_WAIT_QUEUE_HEAD(log_wait);

static char first_read = 1;

/* Device identifiers */
static struct device *dev;
static dev_t secure_dev;
static struct cdev secure_c_dev;
static struct class *secure_class;

/* sprintf is always > 0, let's cast them */
#define SPRINTF (unsigned long)sprintf

/* Get the path of a log */
static char *
get_netlog_path(struct netlog_log *log)
__must_hold(log_lock)
{
	return ((char *)log) + sizeof(struct netlog_log);
}

static char *
get_execlog_path(struct execlog_log *log)
__must_hold(log_lock)
{
	return ((char *)log) + sizeof(struct execlog_log);
}

static char *
get_execlog_argv(struct execlog_log *log)
__must_hold(log_lock)
{
	return ((char *)log) + sizeof(struct execlog_log) + log->path_len;
}

static u32
next_record(u32 idx)
__must_hold(log_lock)
{
	size_t *len;

	len = &((struct sec_log *)(log_buf + idx))->len;
	if (*len == 0) {
		/* We need to wrap around */
		return 0;
	}
	/* Length of items inside the cache can't get out of the cache */
	return (u32)(idx + *len);
}

/* Small tool */
static void
copy_ip(void *dst, const void *src, unsigned short family)
{
	switch (family) {
	case AF_INET:
		memcpy(dst, src, sizeof(struct in_addr));
		break;
	case AF_INET6:
		memcpy(dst, src, sizeof(struct in6_addr));
		break;
	default:
		break;
	}
}

static inline void
find_new_record_place(size_t size)
__must_hold(log_lock)
{
	// align size to next block
	size += (-size) & (LOG_ALIGN - 1);

	while (log_first_seq < log_next_seq) {
		size_t free;

		if (log_next_idx > log_first_idx)
			free = max(LOG_BUF_LEN - log_next_idx, log_first_idx);
		else
			free = log_first_idx - log_next_idx;

		if (free > size + sizeof(struct sec_log))
			break;

		/* Drop old messages until we have enough contiuous space */
		log_first_idx = next_record(log_first_idx);
		log_first_seq++;
	}

	if (unlikely(log_next_idx + size + sizeof(struct sec_log) >= LOG_BUF_LEN)) {
		/*
		 * As free > size + sizeof(struct sec_log), this mean that we had
		 * free = max(log_buf_len - log_next_idx, log_first_idx)
		 * But as we are too close to the end, it means that the max
		 * is log_first_idx, thus we must wrap around.
		 * Add an empty size_t to indicate the wrap around
		 */
		*((size_t *)(log_buf + log_next_idx)) = 0;
		log_next_idx = 0;
	}
}


void
store_netlog_record(const char *path, enum netlog_action action,
		    enum netlog_protocol protocol, unsigned short family,
		    const void *src_ip, int src_port,
		    const void *dst_ip, int dst_port)
{
	struct netlog_log *record;
	size_t path_len, record_size;
	unsigned long flags;

	path_len = strlen(path) + 1;
	if (unlikely(path_len > (LOG_BUF_LEN >> 4) ||
		     path_len > INT_MAX)) {
		dev_warn(dev, "troncating path (size %zu > %i)\n",
			 path_len, min((LOG_BUF_LEN >> 4), (unsigned int)INT_MAX));
		path_len = min((LOG_BUF_LEN >> 4), (unsigned int)INT_MAX);
	}
	record_size = sizeof(struct netlog_log) + path_len;

	spin_lock_irqsave(&log_lock, flags);

	find_new_record_place(record_size);
	record = (struct netlog_log *)(log_buf + log_next_idx);
	/* Store basic information */
	fill_current_details(&(record->header.process));
	record->header.type = LOG_NETWORK_INTERACTION;
	record->header.len = record_size;
	record->path_len = path_len;

	/* Store advanced information */
	record->action = action;
	record->protocol = protocol;
	record->family = family;
	if (src_ip == NULL)
		memset(record->src.raw, 0, 16);
	else
		copy_ip(record->src.raw, src_ip, family);
	if (dst_ip == NULL)
		memset(record->dst.raw, 0, 16);
	else
		copy_ip(record->dst.raw, dst_ip, family);
	record->src_port = src_port;
	record->dst_port = dst_port;
	memcpy(get_netlog_path(record), path, path_len);

	/* Update the next position */
	log_next_idx += record_size;
	log_next_seq++;

	spin_unlock_irqrestore(&log_lock, flags);

	/* Wake-up reading threads */
	wake_up_interruptible(&log_wait);
}
EXPORT_SYMBOL(store_netlog_record);


void
store_execlog_record(const char *path,
		     const char *argv, size_t argv_size)
{
	struct execlog_log *record;
	size_t path_len, record_size;
	unsigned long flags;

	path_len = strlen(path) + 1;
	if (unlikely(path_len > (LOG_BUF_LEN >> 5) ||
		     path_len > INT_MAX)) {
		dev_warn(dev, "Troncating path (size %zu > %i)\n",
			 path_len, min((LOG_BUF_LEN >> 5), (unsigned int)INT_MAX));
		path_len = min((LOG_BUF_LEN >> 5), (unsigned int)INT_MAX);
	}
	if (unlikely(argv_size > (LOG_BUF_LEN >> 5) ||
		     argv_size > INT_MAX)) {
		dev_warn(dev, "Troncating argv (size %zu > %i)\n",
			 argv_size, min((LOG_BUF_LEN >> 5), (unsigned int)INT_MAX));
		argv_size = min((LOG_BUF_LEN >> 5), (unsigned int)INT_MAX);
	}
	record_size = sizeof(struct execlog_log) + path_len + argv_size;

	spin_lock_irqsave(&log_lock, flags);

	find_new_record_place(record_size);
	record = (struct execlog_log *)(log_buf + log_next_idx);
	/* Store basic information */
	fill_current_details(&(record->header.process));
	record->header.type = LOG_EXECUTION;
	record->header.len = record_size;

	/* Store advanced information */
	record->path_len = path_len;
	memcpy(get_execlog_path(record), path, path_len);
	record->argv_len = argv_size;
	memcpy(get_execlog_argv(record), argv, argv_size);

	/* Update the next position */
	log_next_idx += record_size;
	log_next_seq++;

	spin_unlock_irqrestore(&log_lock, flags);

	/* Wake-up reading threads */
	wake_up_interruptible(&log_wait);
}
EXPORT_SYMBOL(store_execlog_record);


struct user_data {
	u64 log_curr_seq;
	u32 log_curr_idx;
	u8  simple_format;
	u8  send_eof;
	struct mutex lock /** Lock when reading (only one read a at time) */;
	char buf[USER_BUFFER_SIZE];
};


static loff_t
secure_log_llseek(struct file *file, loff_t offset, int whence)
{
	struct user_data *data = file->private_data;
	unsigned long flags;

	if (unlikely(data == NULL))
		return -EBADF;

	/* Support rsyslog file reader: accept but ignore custom seeks */
	if (unlikely(offset != 0))
		return 0;

	/* Set the 'offset' to the desired value */
	spin_lock_irqsave(&log_lock, flags);
	switch (whence) {
	case SEEK_SET:
		data->log_curr_seq = log_first_seq;
		data->log_curr_idx = log_first_idx;
		break;
	case SEEK_CUR:
		break;
	case SEEK_END:
		data->log_curr_seq = log_next_seq;
		data->log_curr_idx = log_next_idx;
		break;
	default:
		spin_unlock_irqrestore(&log_lock, flags);
		return -EINVAL;
	}
	spin_unlock_irqrestore(&log_lock, flags);

	return 0;
}


#define UPDATE_POINTERS(change, remaining, len) \
do {						\
	if (change >= remaining) {		\
		/* Output truncated */		\
		return 0;			\
	}					\
	/* 'change' is always > 0 */		\
	len += (unsigned long) change;		\
	remaining -= (unsigned long) change;	\
} while (0)


static size_t
netlog_print(struct netlog_log *record, char *data, size_t len)
__must_hold(log_lock)
{
	size_t remaining = USER_BUFFER_SIZE - len;
	long change;

	if (WARN_ON(record->header.len < sizeof(struct netlog_log))) {
		change = snprintf(data + len, remaining, "BROKEN RECCORD");
		UPDATE_POINTERS(change, remaining, len);
		return len;
	}

	change = snprintf(data + len, remaining, "%.*s ",
			  (int)record->path_len, get_netlog_path(record));
	UPDATE_POINTERS(change, remaining, len);

	change = print_netlog(data + len, remaining,
			      record->protocol, record->family, record->action,
			      &record->src, record->src_port,
			      &record->dst, record->dst_port);
	if (change < 0)
		return 0;
	UPDATE_POINTERS(change, remaining, len);
	return len;
}


static size_t
execlog_print(struct execlog_log *record, char *data, size_t len)
__must_hold(log_lock)
{
	size_t remaining = USER_BUFFER_SIZE - len;
	long change;

	if (WARN_ON(record->header.len < sizeof(struct execlog_log))) {
		change = snprintf(data + len, remaining, "BROKEN RECCORD");
		UPDATE_POINTERS(change, remaining, len);
		return len;
	}

	change = snprintf(data + len, remaining, "%.*s %.*s",
			  (int) record->path_len, get_execlog_path(record),
			  (int) record->argv_len, get_execlog_argv(record));
	UPDATE_POINTERS(change, remaining, len);
	return len;
}

static inline char *
get_module_name(enum secure_log_type type)
{
	switch (type) {
	case LOG_NETWORK_INTERACTION:
		return "netlog";
	case LOG_EXECUTION:
		return "execlog";
	default:
		return "unknown";
	}
}

static inline size_t
secure_log_read_fill_record(char *buf, size_t len, struct sec_log *record)
__must_hold(log_lock)
{
	/* Fill the common header 'len' here is only set to the headers, it
	 * can't overflow here*/
	len += SPRINTF(buf + len, CURRENT_DETAILS_FORMAT " ",
		       CURRENT_DETAILS_ARGS(record->process));

	/* Print the content */
	switch (record->type) {
	case LOG_NETWORK_INTERACTION:
		len = netlog_print((struct netlog_log *)record, buf, len);
		break;
	case LOG_EXECUTION:
		len = execlog_print((struct execlog_log *)record, buf, len);
		break;
	default:
		/* We can't overflow here as only static headers have been
		 * written up to here */
		len += SPRINTF(buf + len, "Unknown entry");
	}
	if (len == 0) {
		sprintf(buf + (USER_BUFFER_SIZE - 7), "TRUNC");
		len = USER_BUFFER_SIZE - 2;
	}
	len += SPRINTF(buf + len, "\n");

	return len;
}

static ssize_t
secure_log_read(struct file *file, char __user *buf, size_t count,
		loff_t *offset)
{
	struct user_data *data = file->private_data;
	struct sec_log *record;
	u64 ts;
	unsigned long rem_nsec;
	unsigned long flags;
	size_t len;
	ssize_t err, ret;

	if (unlikely(data == NULL))
		return -EBADF;

	/* Is the user already reading ? */
	err = mutex_lock_interruptible(&data->lock);
	if (err)
		return err;

	spin_lock_irqsave(&log_lock, flags);
	/* Wait until we have something to read */
	while (data->log_curr_seq == log_next_seq) {
		/* Too bad, this call cannot be non-blocking */
		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			spin_unlock_irqrestore(&log_lock, flags);
			goto out;
		}

		/* The caller asked for a EOF */
		if (data->send_eof) {
			ret = 0;
			spin_unlock_irqrestore(&log_lock, flags);
			goto out;
		}

		/* We need to wait, unlock */
		spin_unlock_irqrestore(&log_lock, flags);
		ret = wait_event_interruptible(log_wait,
				data->log_curr_seq != log_next_seq);
		if (ret)
			goto out;
		spin_lock_irqsave(&log_lock, flags);
	}

	/* Perhaps we waited for too long and some data is lost */
	if (unlikely(data->log_curr_seq < log_first_seq)) {
		/* Rest the position and alert the user */
		data->log_curr_seq = log_first_seq;
		data->log_curr_idx = log_first_idx;
		spin_unlock_irqrestore(&log_lock, flags);
		ret = -EPIPE;
		goto out;
	}

	/* Get the current record */
	record = (struct sec_log *)(log_buf + data->log_curr_idx);
	if (record->len == 0) {
		/* We have cycled back to the start */
		record = (struct sec_log *)(log_buf);
	}

	ts = record->process.nsec;
	rem_nsec = do_div(ts, 1000000000);
	if (data->simple_format == 0) {
		/* Fill the syslog header */
		len = SPRINTF(data->buf, "<%u>1 - - %s - - - [%5lu.%06lu]: ",
			      (LOG_FACILITY << 3) | LOG_LEVEL,
			      get_module_name(record->type),
			      (unsigned long)ts, rem_nsec / 1000);
	} else {
		/* Use a simpler header */
		len = SPRINTF(data->buf, "%s [%5lu.%06lu]: ",
			      get_module_name(record->type),
			      (unsigned long)ts, rem_nsec / 1000);
	}

	len = secure_log_read_fill_record(data->buf, len, record);

	/* Prepare for next iteration */
	data->log_curr_idx = next_record(data->log_curr_idx);
	++data->log_curr_seq;

	/* Unlock */
	spin_unlock_irqrestore(&log_lock, flags);

	/* The user buffer is too small, abort */
	if (unlikely(len > count)) {
		ret = -EINVAL;
		goto out;
	}

	/* Copy the data into userspace */
	if (unlikely(copy_to_user(buf, data->buf, len))) {
		/* Copy failed */
		ret = -EFAULT;
		goto out;
	}
	/* len < USER_BUFFER_SIZE, can't overflow */
	ret = (ssize_t)len;
out:
	mutex_unlock(&data->lock);
	return ret;
}

static unsigned int
secure_log_poll(struct file *file, poll_table *wait)
{
	struct user_data *data = file->private_data;
	unsigned long flags;
	unsigned int ret = 0;

	if (unlikely(data == NULL))
		return POLLERR|POLLNVAL;

	/* Update the poll state */
	poll_wait(file, &log_wait, wait);

	/* Check if there is anything to read */
	spin_lock_irqsave(&log_lock, flags);
	if (data->log_curr_seq < log_next_seq) {
		/* Return error when data has vanished underneath us */
		if (data->log_curr_seq < log_first_seq)
			ret = POLLIN|POLLRDNORM|POLLERR|POLLPRI;
		else
			ret = POLLIN|POLLRDNORM;
	}
	spin_unlock_irqrestore(&log_lock, flags);

	return ret;
}

static int
secure_log_open(struct inode *inode, struct file *file)
{
	struct user_data *data;
	unsigned long flags;

	/* Allocate private data */
	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (unlikely(data == NULL))
		return -ENOMEM;

	/* Initialize read mutex */
	mutex_init(&data->lock);

	/* Set the format */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	kernel_param_lock(THIS_MODULE);
	data->simple_format = !!simple_format;
	data->send_eof = !!send_eof;
	kernel_param_unlock(THIS_MODULE);
#else /* LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0) */
	kparam_block_sysfs_write(simple_format);
	data->simple_format = !!simple_format;
	kparam_unblock_sysfs_write(simple_format);
	kparam_block_sysfs_write(send_eof);
	data->send_eof = !!send_eof;
	kparam_unblock_sysfs_write(send_eof);
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(4, 2, 0) */

	/* Get current state */
	spin_lock_irqsave(&log_lock, flags);
	if (first_read) {
		data->log_curr_seq = log_first_seq;
		data->log_curr_idx = log_first_idx;
		first_read = 0;
	} else {
		data->log_curr_seq = log_next_seq;
		data->log_curr_idx = log_next_idx;
	}
	spin_unlock_irqrestore(&log_lock, flags);


	/* Store private data */
	file->private_data = data;

	return 0;
}

static int
secure_log_release(struct inode *inode, struct file *file)
{
	struct user_data *data = file->private_data;

	if (data == NULL)
		return 0;

	mutex_destroy(&data->lock);
	kfree(data);

	return 0;
}


static const struct file_operations secure_log_fops = {
	.owner = THIS_MODULE,
	.open = secure_log_open,
	.read = secure_log_read,
	.llseek = secure_log_llseek,
	.poll = secure_log_poll,
	.release = secure_log_release,
};


static int __init
init_secure_dev(void)
{
	int err;

	secure_class = class_create(THIS_MODULE, MODULE_NAME);
	if (IS_ERR(secure_class))
		return PTR_ERR(secure_class);

	err =  alloc_chrdev_region(&secure_dev, 0, 1, MODULE_NAME);
	if (err < 0)
		goto clean_class;

	cdev_init(&secure_c_dev, &secure_log_fops);
	err = cdev_add(&secure_c_dev, secure_dev, 1);
	if (err < 0)
		goto clean_chrdev_region;

	dev = device_create(secure_class, NULL, secure_dev, NULL, MODULE_NAME);
	if (IS_ERR(dev)) {
		err = PTR_ERR(dev);
		goto clean_cdev;
	}

	dev_info(dev, "[+] Created /dev/"MODULE_NAME" for logs\n");
	return 0;

clean_cdev:
	cdev_del(&secure_c_dev);
clean_chrdev_region:
	unregister_chrdev_region(secure_dev, 1);
clean_class:
	class_destroy(secure_class);
	return err;
}

module_init(init_secure_dev)

static void __exit
destroy_secure_dev(void)
{
	dev_info(dev, "[+] Removing /dev/"MODULE_NAME"\n");
	device_destroy(secure_class, secure_dev);
	cdev_del(&secure_c_dev);
	unregister_chrdev_region(secure_dev, 1);
	class_destroy(secure_class);
	return;
}

module_exit(destroy_secure_dev)
