#include <linux/file.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/ipv6.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/syscalls.h>
#include <linux/version.h>
#include <linux/unistd.h>
#include <net/ip.h>
#include "whitelist.h"
#include "netlog.h"
#include "log.h"
#include "probes.h"
#include "sparse_compat.h"
#include "retro-compat.h"
#include "internal.h"

/********************************/
/*          Variables           */
/********************************/

static u32 loaded_probes = 0;
static DEFINE_SPINLOCK(probe_lock);


/********************************/
/*            Tools             */
/********************************/

static char *path_from_mm(struct mm_struct *mm, char *buffer, int length)
{
	char *p = NULL;
	if (unlikely(mm == NULL))
		return NULL;

	down_read(&mm->mmap_sem);

	if (unlikely(mm->exe_file == NULL)) {
		p = NULL;
	} else {
		p = d_path(&mm->exe_file->f_path, buffer, length);
		if(IS_ERR(p))
			p = NULL;
	}

	up_read(&mm->mmap_sem);
	return p;
}

static void log_if_not_whitelisted(struct socket *sock, u8 protocol, u8 action)
{
	/* sock & sock->sk need to be non null */

	char buffer[MAX_EXEC_PATH + 1], *path;
	unsigned short family;
	const void *dst_ip;
	const void *src_ip;
	int dst_port;
	int src_port;

	path = path_from_mm(current->mm, buffer, MAX_EXEC_PATH);
	buffer[MAX_EXEC_PATH] = '\0';
	if(unlikely(path == NULL))
		return;

	/* Get everything */
	family = sock->sk->sk_family;
	dst_port = ntohs(inet_sk(sock->sk)->DPORT);
	src_port = ntohs(inet_sk(sock->sk)->SPORT);
	switch(family)
	{
		case AF_INET:
			dst_ip = &inet_sk(sock->sk)->DADDR;
			src_ip = &inet_sk(sock->sk)->SADDR;
			break;
		case AF_INET6:
			dst_ip = &inet6_sk(sock->sk)->daddr;
			src_ip = &inet6_sk(sock->sk)->saddr;
			break;
		default:
			dst_ip = NULL;
			src_ip = NULL;
			break;
	}

#if WHITELISTING
	/* Are we whitelisted ? */
	if(is_whitelisted(path, family, dst_ip, dst_port))
		return;
#endif

	store_netlog_record(path, action, protocol,
	                    family, src_ip, src_port, dst_ip, dst_port);
}


/**********************************/
/*           PROBES               */
/**********************************/

/* Some of the probes are grouped by 2: one probe before the syscall and one afterwards.
 * In those cases the socket file descriptor is only complete after the call and only available before the call.
 * A single process (thread) can be in a single system call at a time
 * because when a system call is called, the process is suspended until its end of execution.
 */

static struct socket *match_socket[PID_MAX_LIMIT] = {NULL};

static int pre_inet_stream_connect(struct socket *sock, struct sockaddr *addr, int addr_len, int flags)
{
	if (likely(current != NULL))
		match_socket[current->pid] = sock;

	jprobe_return();
	return 0;
}

static int post_inet_stream_connect(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *sock;

	sock = match_socket[current->pid];

	if (likely(current != NULL) &&
	    likely(sock != NULL) &&
	    likely(sock->sk != NULL) &&
	    likely(sock->sk->sk_family == AF_INET ||
	           sock->sk->sk_family == AF_INET6) &&
	    likely(sock->sk->sk_protocol == IPPROTO_TCP))
		log_if_not_whitelisted(sock, PROTO_TCP, ACTION_CONNECT);

	match_socket[current->pid] = NULL;
	return 0;
}

static int pre_inet_dgram_connect(struct socket *sock, struct sockaddr *addr, int addr_len, int flags)
{
	if (likely(current != NULL))
		match_socket[current->pid] = sock;

	jprobe_return();
	return 0;
}

static int post_inet_dgram_connect(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *sock;

	sock = match_socket[current->pid];

	if (likely(current != NULL) &&
	    likely(sock != NULL) &&
	    likely(sock->sk != NULL) &&
	    likely(sock->sk->sk_family == AF_INET ||
	           sock->sk->sk_family == AF_INET6) &&
	    likely(sock->sk->sk_protocol == IPPROTO_UDP))
		log_if_not_whitelisted(sock, PROTO_UDP, ACTION_CONNECT);

	match_socket[current->pid] = NULL;
	return 0;
}

/* post_sys_accept probe is called right after the accept system call returns.
 * In the return register is placed the socket file descriptor. So with the
 * user of regs_register_status we can get the socket file descriptor and log
 * the data that we want for the socket.
 */

static int post_sys_accept(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(regs_return_value(regs), &err);

	if (likely(sock != NULL)) {
		if (likely(sock->sk != NULL) &&
		    likely(sock->sk->sk_family == AF_INET ||
		           sock->sk->sk_family == AF_INET6) &&
		    likely(sock->sk->sk_protocol == IPPROTO_TCP))
			log_if_not_whitelisted(sock, PROTO_TCP, ACTION_ACCEPT);
		sockfd_put(sock);
	}
	return 0;
}

asmlinkage static long pre_sys_close(unsigned int fd)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(fd, &err);

	if (unlikely(current == NULL) ||
	    unlikely(sock == NULL) ||
	    unlikely(sock->sk == NULL) ||
	    likely(sock->sk->sk_family != AF_INET &&
	           sock->sk->sk_family != AF_INET6))
		goto out;

	if ((loaded_probes & (1 << PROBE_TCP_CLOSE)) &&
	    sock->sk->sk_protocol == IPPROTO_TCP &&
	    likely(inet_sk(sock->sk)->DPORT != 0))
		log_if_not_whitelisted(sock, PROTO_TCP, ACTION_CLOSE);
	else if ((loaded_probes & (1 << PROBE_UDP_CLOSE)) &&
	         sock->sk->sk_protocol == IPPROTO_UDP &&
	         inet_sk(sock->sk)->SPORT != 0)
		log_if_not_whitelisted(sock, PROTO_UDP, ACTION_CLOSE);

out:
	if(likely(sock != NULL))
		sockfd_put(sock);

	jprobe_return();
	return 0;
}

static int post_sys_bind(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct socket *sock;
	sock = match_socket[current->pid];

	if (likely(sock != NULL)) {
		if (likely(sock->sk != NULL) &&
		    likely(sock->sk->sk_family == AF_INET ||
		           sock->sk->sk_family == AF_INET6) &&
		    likely(sock->sk->sk_protocol == IPPROTO_UDP))
			log_if_not_whitelisted(sock, PROTO_UDP, ACTION_BIND);
		sockfd_put(sock);
	}

	match_socket[current->pid] = NULL;
	return 0;
}

/* UDP protocol is connectionless protocol, so we probe the bind system call */
asmlinkage static int pre_sys_bind(int sockfd, const struct sockaddr *addr, int addrlen)
{
	int err;
	struct socket *sock;

	if (unlikely(current == NULL))
		return 0;

	sock = sockfd_lookup(sockfd, &err);

	if (likely(sock != NULL))
		match_socket[current->pid] = sock;

	jprobe_return();
	return 0;
}

static int handler_fault(struct kprobe *p, struct pt_regs *regs, int trap_number)
{
	switch(trap_number)
	{
		case SIGABRT:
		case SIGSEGV:
		case SIGQUIT:
			//TODO Other signals that we need to handle?
			printk(KERN_ERR MODULE_NAME ": fault handler: Detected fault %d from inside probes.", trap_number);
			return 0;
		default:
			return 0;
	}
}


/*************************************/
/*         probe definitions        */
/*************************************/

static struct jprobe stream_connect_jprobe =
{
	.entry = (kprobe_opcode_t *) pre_inet_stream_connect,
	.kp =
	{
		.symbol_name = "inet_stream_connect",
		.fault_handler = handler_fault,
	},
};

static struct kretprobe stream_connect_kretprobe =
{
        .handler = post_inet_stream_connect,
        .maxactive = 16 * NR_CPUS,
        .kp =
        {
        	.symbol_name = "inet_stream_connect",
		.fault_handler = handler_fault,
        },
};

static struct jprobe dgram_connect_jprobe =
{
	.entry = (kprobe_opcode_t *) pre_inet_dgram_connect,
	.kp =
	{
		.symbol_name = "inet_dgram_connect",
		.fault_handler = handler_fault,
	},
};

static struct kretprobe dgram_connect_kretprobe =
{
        .handler = post_inet_dgram_connect,
        .maxactive = 16 * NR_CPUS,
        .kp =
        {
        	.symbol_name = "inet_dgram_connect",
		.fault_handler = handler_fault,
        },
};

static struct kretprobe accept_kretprobe =
{
	.handler = post_sys_accept,
	.maxactive = 16 * NR_CPUS,
        .kp =
        {
		#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 32)
        	.symbol_name = "sys_accept",
        	#else
        	.symbol_name = "sys_accept4",
		#endif
		.fault_handler = handler_fault,
        },
};

static struct jprobe close_jprobe =
{
	.entry = (kprobe_opcode_t *) pre_sys_close,
	.kp =
	{
		.symbol_name = "sys_close",
		.fault_handler = handler_fault,
	}
};

static struct kretprobe bind_kretprobe =
{
	.handler = post_sys_bind,
	.maxactive = 16 * NR_CPUS,
        .kp =
        {
		.symbol_name = "sys_bind",
		.fault_handler = handler_fault,
        },
};

static struct jprobe bind_jprobe =
{
	.entry = (kprobe_opcode_t *) pre_sys_bind,
	.kp =
	{
		.symbol_name = "sys_bind",
		.fault_handler = handler_fault,
	},
};


/****************************************/
/*     Planting/unplanting probes       */
/****************************************/

static void unplant_jprobe(struct jprobe * probe) __must_hold(probe_lock)
{
	printk(KERN_INFO MODULE_NAME ":\t[+] Unplanting jprobe on %s\n", probe->kp.symbol_name);
	unregister_jprobe(probe);
	printk(KERN_INFO MODULE_NAME ":\t[+] Unplanted jprobe on %s\n", probe->kp.symbol_name);
	probe->kp.addr = NULL;
}

static void unplant_kretprobe(struct kretprobe * probe) __must_hold(probe_lock)
{
	printk(KERN_INFO MODULE_NAME ":\t[+] Unplanting kretprobe on %s\n", probe->kp.symbol_name);
	unregister_kretprobe(probe);
	printk(KERN_INFO MODULE_NAME ":\t[+] Unplanted kretprobe on %s\n", probe->kp.symbol_name);
	probe->kp.addr = NULL;
}

static void unplant_tcp_connect(void) __must_hold(probe_lock)
{
	unplant_jprobe(&stream_connect_jprobe);
	unplant_kretprobe(&stream_connect_kretprobe);
}

static void unplant_udp_connect(void) __must_hold(probe_lock)
{
	unplant_jprobe(&dgram_connect_jprobe);
	unplant_kretprobe(&dgram_connect_kretprobe);
}

static void unplant_tcp_accept(void) __must_hold(probe_lock)
{
	unplant_kretprobe(&accept_kretprobe);
}

static void unplant_close(void) __must_hold(probe_lock)
{
	unplant_jprobe(&close_jprobe);
}

static void unplant_udp_bind(void) __must_hold(probe_lock)
{
	unplant_jprobe(&bind_jprobe);
	unplant_kretprobe(&bind_kretprobe);
}

static void unplant_probe_locked(u32 probe) __must_hold(probe_lock)
{
	u32 removed_probes;

	removed_probes = loaded_probes & probe;
	loaded_probes ^= removed_probes;

	if (removed_probes & (1 << PROBE_TCP_CONNECT))
		unplant_tcp_connect();

	if (removed_probes & (1 << PROBE_TCP_ACCEPT))
		unplant_tcp_accept();

	if (removed_probes & ((1 << PROBE_TCP_CLOSE) | (1 << PROBE_UDP_CLOSE))) {
		if (!(loaded_probes & ((1 << PROBE_TCP_CLOSE) | (1 << PROBE_UDP_CLOSE))))
			unplant_close();
 	}
	if (removed_probes & (1 << PROBE_UDP_CONNECT))
		unplant_udp_connect();

	if (removed_probes & (1 << PROBE_UDP_BIND))
		unplant_udp_bind();
}

void unplant_probe(u32 probe)
{
	unsigned long flags;

	spin_lock_irqsave(&probe_lock, flags);
	unplant_probe_locked(probe);
	spin_unlock_irqrestore(&probe_lock, flags);
}

void unplant_all(void)
{
	unplant_probe((1 << (PROBES_NUMBER + 1)) - 1);
}

static int plant_jprobe(struct jprobe * probe) __must_hold(probe_lock)
{
	int err;

	printk(KERN_INFO MODULE_NAME ":\t[+] Planting jprobe on %s\n", probe->kp.symbol_name);
	err = register_jprobe(probe);
	if (err < 0)
		printk(KERN_INFO MODULE_NAME ":\t[-] Failed to planted jprobe on %s: %i\n", probe->kp.symbol_name, err);
	else
		printk(KERN_INFO MODULE_NAME ":\t[+] Planted jprobe on %s\n", probe->kp.symbol_name);

	return err;
}

static int plant_kretprobe(struct kretprobe * probe) __must_hold(probe_lock)
{
	int err;

	printk(KERN_INFO MODULE_NAME ":\t[+] Planting kretprobe on %s\n", probe->kp.symbol_name);
	err = register_kretprobe(probe);
	if (err < 0)
		printk(KERN_INFO MODULE_NAME ":\t[-] Failed to planted kretprobe on %s: %i\n", probe->kp.symbol_name, err);
	else
		printk(KERN_INFO MODULE_NAME ":\t[+] Planted kretprobe on %s\n", probe->kp.symbol_name);

	return err;
}

static int plant_tcp_connect(void) __must_hold(probe_lock)
{
	int err;

	err = plant_jprobe(&stream_connect_jprobe);
	if (err < 0)
		return -CONNECT_PROBE_FAILED;

	err = plant_kretprobe(&stream_connect_kretprobe);
	if (err < 0) {
		unplant_jprobe(&stream_connect_jprobe);
		return -CONNECT_PROBE_FAILED;
	}

	return 0;
}

static int plant_udp_connect(void) __must_hold(probe_lock)
{
	int err;

	err = plant_jprobe(&dgram_connect_jprobe);
	if (err < 0)
		return -CONNECT_PROBE_FAILED;

	err = plant_kretprobe(&dgram_connect_kretprobe);
	if (err < 0) {
		unplant_jprobe(&dgram_connect_jprobe);
		return -CONNECT_PROBE_FAILED;
	}

	return 0;
}

static int plant_tcp_accept(void) __must_hold(probe_lock)
{
	int err;

	err = plant_kretprobe(&accept_kretprobe);
	if (err < 0)
		return -ACCEPT_PROBE_FAILED;

	return 0;
}

static int plant_close(void) __must_hold(probe_lock)
{
	int err;

	err = plant_jprobe(&close_jprobe);
	if (err < 0)
		return -CLOSE_PROBE_FAILED;

	return 0;
}

static int plant_udp_bind(void) __must_hold(probe_lock)
{
	int err;

	err = plant_jprobe(&bind_jprobe);
	if(err < 0)
		return -BIND_PROBE_FAILED;

	err = plant_kretprobe(&bind_kretprobe);
	if(err < 0)
	{
		unplant_jprobe(&bind_jprobe);
		return -BIND_PROBE_FAILED;
	}

	return 0;
}

int plant_probe(u32 probe)
{
	unsigned long flags;
	u32 new_probes;
	int err = 0;

	spin_lock_irqsave(&probe_lock, flags);
	new_probes = (probe ^ loaded_probes) & probe;

	if (new_probes & (1 << PROBE_TCP_CONNECT)) {
		err = plant_tcp_connect();
		if (err)
			goto unlock;
		loaded_probes |= 1 << PROBE_TCP_CONNECT;
 	}

	if (new_probes & (1 << PROBE_TCP_ACCEPT)) {
		err = plant_tcp_accept();
		if (err)
			goto unlock;
		loaded_probes |= 1 << PROBE_TCP_ACCEPT;
 	}

	if (new_probes & (1 << PROBE_TCP_CLOSE)) {
		if (!(loaded_probes & (1 << PROBE_UDP_CLOSE))) {
			err = plant_close();
			if (err)
				goto unlock;
		}
		loaded_probes |= 1 << PROBE_TCP_CLOSE;
 	}
	if (new_probes & (1 << PROBE_UDP_CONNECT)) {
		err = plant_udp_connect();
		if (err)
			goto unlock;
		loaded_probes |= 1 << PROBE_UDP_CONNECT;
 	}

	if (new_probes & (1 << PROBE_UDP_BIND)) {
		err = plant_udp_bind();
		if (err)
			goto unlock;
		loaded_probes |= 1 << PROBE_UDP_BIND;
 	}

	if (new_probes & (1 << PROBE_UDP_CLOSE)) {
		if (!(loaded_probes & (1 << PROBE_TCP_CLOSE))) {
			err = plant_close();
			if (err)
				goto unlock;
		}
		loaded_probes |= 1 << PROBE_UDP_CLOSE;
 	}

unlock:
	spin_unlock_irqrestore(&probe_lock, flags);
	return err;
}

int probe_status(u32 probe)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&probe_lock, flags);
	ret = !!(probe & loaded_probes);
	spin_unlock_irqrestore(&probe_lock, flags);
	return ret;
}
