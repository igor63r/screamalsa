/*
 * ScreamALSA Linux Kernel Driver
 *
 * Compatibility: Linux kernel 3.8 - 6.x
 *
 * This driver implements a virtual sound card that streams audio over network
 *
 * Author: I.Antonov, igor63r@gmail.com
 *
 * ScreamALSA Driver Binaries Repository:
 *
 * https://albumplayer.ru/screamalsa
 *
 * License: GPL-2
 */


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <net/sock.h>
#include <linux/inet.h>
#include <net/tcp.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/version.h>

#ifndef TCP_USER_TIMEOUT
#define TCP_USER_TIMEOUT 18
#endif
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/compiler.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/memalloc.h>
#include <linux/jiffies.h>
#include <linux/fcntl.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
#include <uapi/linux/sched/types.h>
#endif

/* Check minimum kernel version */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 8, 0)
#error "This driver requires Linux kernel 3.8 or later"
#endif

/* For KERNEL_SOCKPTR macro on newer kernels */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
    #include <linux/sockptr.h>
#endif

MODULE_AUTHOR("I.Antonov igor63r@gmail.com");
MODULE_VERSION("2.0.0");

MODULE_DESCRIPTION("ALSA driver to stream high-rate PCM/DSD over TCP/UDP (Scream protocol)");
MODULE_LICENSE("GPL v2");

/* ------------------------------
 *      Connection state
 * ------------------------------ */
enum {
    STATE_DISCONNECTED,
    STATE_CONNECTING,
    STATE_CONNECTED,
};

static char ip_addr_str[32] = "192.168.1.77";
module_param_string(ip_addr_str, ip_addr_str, sizeof(ip_addr_str), 0644);
MODULE_PARM_DESC(ip_addr_str, "Target IP address");

static char protocol_str[8] = "udp"; // "udp" or "cp"
module_param_string(protocol_str, protocol_str, sizeof(protocol_str), 0644);
MODULE_PARM_DESC(protocol_str, "Network protocol: 'udp' or 'tcp'");

static int port = 4011;
module_param(port, int, 0644);
MODULE_PARM_DESC(port, "Target port");

#define DRIVER_NAME "ScreamALSA"
static struct snd_card *scream_card_ptr = NULL;
static struct platform_device *scream_pdev = NULL;
static const u8 ch_mask[] = {0, 1, 3, 7, 15, 31, 63, 127, 255};
#define SCREAM_PAYLOAD_SIZE 1152
#define SCREAM_HEADER_SIZE 5
#define SCREAM_PACKET_SIZE (SCREAM_HEADER_SIZE + SCREAM_PAYLOAD_SIZE)


#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
# define SCREAM_DMA_TYPE SNDRV_DMA_TYPE_VMALLOC
# define SCREAM_DMA_DATA NULL
#else
# define SCREAM_DMA_TYPE SNDRV_DMA_TYPE_CONTINUOUS
# define SCREAM_DMA_DATA snd_dma_continuous_data(GFP_KERNEL)
#endif

#define SCREAM_INFO_FLAGS (SNDRV_PCM_INFO_INTERLEAVED | \
                           SNDRV_PCM_INFO_MMAP | \
                           SNDRV_PCM_INFO_MMAP_VALID)

/* HR timer logic removed, using kthread sleep instead */

struct snd_scream_device {
    struct snd_card *card;
    struct snd_pcm *pcm;
    struct snd_pcm_substream *substream;

    struct socket *sock;
    struct sockaddr_in remote_addr;
    bool is_tcp;

    spinlock_t lock;
    wait_queue_head_t playback_waitq;
    struct task_struct *playback_thread;
    ktime_t period_time_ns;
    size_t hw_ptr;          /* in bytes */
    bool is_running;
    u8 network_buffer[SCREAM_PACKET_SIZE];

    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_format_t format;
    bool is_dsd;

    struct delayed_work reconnect_work;
    atomic_t connection_state;
    atomic_t reconnect_attempts;
    atomic_t closing;        /* set to 1 during close to stop reconnect rescheduling */

    /* Flexible periods natively supported */
    size_t alsa_period_bytes;
    size_t bytes_in_period;
};

static struct snd_pcm_hardware snd_scream_hw = {
    .info = SCREAM_INFO_FLAGS,
    .formats =
        (SNDRV_PCM_FMTBIT_S32_LE
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
#ifdef SNDRV_PCM_FMTBIT_DSD_U32_BE
         | SNDRV_PCM_FMTBIT_DSD_U32_BE
#endif
#endif
        ),
    .rates = SNDRV_PCM_RATE_CONTINUOUS | SNDRV_PCM_RATE_KNOT,
    .rate_min = 44100,
    .rate_max = 1536000,
    .channels_min = 2,
    .channels_max = 8,
    .buffer_bytes_max = 1024 * 1024,
    .period_bytes_min = SCREAM_PAYLOAD_SIZE,
    .period_bytes_max = SCREAM_PAYLOAD_SIZE * 128,
    .periods_min = 2,
    .periods_max = 1024,
};

static void convert_data(char*src, int frames)
{
        int i = 0;
        char src1, src2;
        while(i++ < frames)
        {
            src1 = src[1];
            src[1] = src[4];
            src2 = src[2];
            src[2] = src1;
            src1 = src[3];
            src[3] = src[5];
            src[4] = src2;
            src[5] = src[6];
            src[6] = src1;
            src+=8;
        }
}

static inline void set_sock_timeouts(struct socket *sock, unsigned int msec)
{
    struct sock *sk = sock->sk;
    long to = msecs_to_jiffies(msec);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
    WRITE_ONCE(sk->sk_rcvtimeo, to);
    WRITE_ONCE(sk->sk_sndtimeo, to);
#else
    sk->sk_rcvtimeo = to;
    sk->sk_sndtimeo = to;
#endif
}

/* ------------------------------
 *       Networking helpers
 * ------------------------------ */
/* Compatibility macros for different kernel versions */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
#define SET_SOCKOPT(sock, level, optname, optval, optlen) \
    sock_setsockopt(sock, level, optname, KERNEL_SOCKPTR(optval), optlen)
#else
#define SET_SOCKOPT(sock, level, optname, optval, optlen) \
    kernel_setsockopt(sock, level, optname, (char *)(optval), optlen)
#endif

/* Socket creation compatibility */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,2,0)
    #define SCREAM_SOCK_CREATE(af, type, proto, sock) \
        sock_create_kern(&init_net, af, type, proto, sock)
#else
    #define SCREAM_SOCK_CREATE(af, type, proto, sock) \
        sock_create_kern(af, type, proto, sock)
#endif

/* hrtimer_forward function removed */

static void scream_cleanup_resources(struct snd_scream_device *dev)
{
    unsigned long flags;
    struct task_struct *thd = NULL;
    spin_lock_irqsave(&dev->lock, flags);
    if (dev->is_running) {
        dev->is_running = false;
    }
    thd = dev->playback_thread;
    dev->playback_thread = NULL;
    spin_unlock_irqrestore(&dev->lock, flags);
    
    if (thd) {
        kthread_stop(thd);
    }

    cancel_delayed_work_sync(&dev->reconnect_work);
    /* Close socket with proper TCP shutdown */
    if (dev->sock) {
        if (dev->is_tcp && atomic_read(&dev->connection_state) == STATE_CONNECTED) {
            dev->sock->ops->shutdown(dev->sock, SHUT_WR);
        }
        sock_release(dev->sock);
        dev->sock = NULL;
    }

    /* Reset all state atomically */
    atomic_set(&dev->connection_state, STATE_DISCONNECTED);
    atomic_set(&dev->reconnect_attempts, 0);
    spin_lock_irqsave(&dev->lock, flags);
    dev->is_running = false;
    dev->substream = NULL;
    spin_unlock_irqrestore(&dev->lock, flags);
}

static void scream_reconnect_work(struct work_struct *work)
{
    struct snd_scream_device *dev = container_of(to_delayed_work(work),
                                                 struct snd_scream_device,
                                                 reconnect_work);
    int ret;
    if (!dev->is_tcp)
        return;
    if (atomic_read(&dev->closing))
        return;

    pr_info("Called scream_reconnect_work.\n");
    switch (atomic_read(&dev->connection_state)) {
    case STATE_CONNECTED:
        /* Nothing to do */
        return;

    case STATE_CONNECTING:
        /* Poll non-blocking connect progress without recreating the socket */
        if (!dev->sock || !dev->sock->sk) {
            atomic_set(&dev->connection_state, STATE_DISCONNECTED);
            if (!atomic_read(&dev->closing))
                schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(200));
            return;
        }

        if (dev->sock->sk->sk_state == TCP_ESTABLISHED) {
            set_sock_timeouts(dev->sock, 5000);
            atomic_set(&dev->connection_state, STATE_CONNECTED);
            atomic_set(&dev->reconnect_attempts, 0);
            pr_info(DRIVER_NAME ": TCP reconnected successfully.\n");
            return;
        }
        if (dev->sock->sk->sk_state == TCP_SYN_SENT || dev->sock->sk->sk_state == TCP_SYN_RECV) {
            /* Still in progress; poll again soon */
            pr_debug(DRIVER_NAME ": TCP connect in progress (state=%d)\n", dev->sock->sk->sk_state);
            if (!atomic_read(&dev->closing))
                schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(200));
            return;
        }
        /* Connection failed (e.g., CLOSED/FIN_WAIT/ERROR). Restart from scratch. */
        pr_warn(DRIVER_NAME ": TCP connect failed (state=%d). Restarting.\n", dev->sock->sk->sk_state);
        kernel_sock_shutdown(dev->sock, SHUT_RDWR);
        sock_release(dev->sock);
        dev->sock = NULL;
        atomic_set(&dev->connection_state, STATE_DISCONNECTED);
        /* fallthrough to DISCONNECTED path below */
        /* no break */
    case STATE_DISCONNECTED:
    default: {
        int attempts = atomic_inc_return(&dev->reconnect_attempts);
        pr_info(DRIVER_NAME ": Reconnecting... (attempt %d)\n", attempts);

        if (attempts > 10) {
            pr_err(DRIVER_NAME ": Too many reconnect attempts, giving up\n");
            atomic_set(&dev->reconnect_attempts, 0);
            return;
        }
        /* Close any leftover socket */
        if (dev->sock) {
            pr_info(DRIVER_NAME ": Closing old TCP connection before reconnect\n");
            kernel_sock_shutdown(dev->sock, SHUT_RDWR);
            sock_release(dev->sock);
            dev->sock = NULL;
        }
        /* Create new socket */
        ret = SCREAM_SOCK_CREATE(AF_INET, SOCK_STREAM, IPPROTO_TCP, &dev->sock);
        if (ret < 0) {
            pr_err(DRIVER_NAME ": Failed to create socket for reconnect: %d\n", ret);
            goto retry_long;
        }
        /* Set TCP socket options */
        {
            int opt = 1;
            ret = SET_SOCKOPT(dev->sock, SOL_TCP, TCP_NODELAY, &opt, sizeof(opt));
            if (ret < 0)
                pr_warn(DRIVER_NAME ": Failed to set TCP_NODELAY: %d\n", ret);

            ret = SET_SOCKOPT(dev->sock, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
            if (ret < 0)
                pr_warn(DRIVER_NAME ": Failed to set SO_KEEPALIVE: %d\n", ret);

            opt = 3000;
            ret = SET_SOCKOPT(dev->sock, SOL_TCP, TCP_USER_TIMEOUT, &opt, sizeof(opt));
            if (ret < 0)
                pr_warn(DRIVER_NAME ": Failed to set TCP_USER_TIMEOUT: %d\n", ret);
        }
        /* Start non-blocking connect and keep socket for polling */
        ret = kernel_connect(dev->sock, (struct sockaddr *)&dev->remote_addr,
                             sizeof(dev->remote_addr), O_NONBLOCK);
        if (ret == 0) {
            /* Connected immediately */
            set_sock_timeouts(dev->sock, 5000);
            atomic_set(&dev->connection_state, STATE_CONNECTED);
            atomic_set(&dev->reconnect_attempts, 0);
            pr_info(DRIVER_NAME ": TCP reconnected successfully.\n");
            return;
        } else if (ret == -EINPROGRESS) {
            /* Connection in progress, keep state and poll soon */
            pr_debug(DRIVER_NAME ": TCP connect started (non-blocking).\n");
            atomic_set(&dev->connection_state, STATE_CONNECTING);
            if (!atomic_read(&dev->closing))
                schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(200));
            return;
        }
        pr_warn(DRIVER_NAME ": Reconnect attempt failed immediately: %d\n", ret);
        /* Failure: release socket and retry later */
        sock_release(dev->sock);
        dev->sock = NULL;
        goto retry_long;
      }
    }
retry_long:
    atomic_set(&dev->connection_state, STATE_DISCONNECTED);
    if (!atomic_read(&dev->closing))
        schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(2000));
}

static unsigned int scream_reconnect_delay_ms_for_err(int err)
{
    switch (err) {
    case -EPIPE:
    case -ECONNRESET:
    case -ESHUTDOWN:
    case -ENOTCONN:
        return 100;
    case -ETIMEDOUT:
        return 1000;
    case -ENETUNREACH:
    case -EHOSTUNREACH:
    case -EADDRNOTAVAIL:
        return 2000;
    default:
        return 500;
    }
}

static u8 lastbuf[SCREAM_HEADER_SIZE + SCREAM_PAYLOAD_SIZE] = {0};
static int scream_send_last_packet(struct snd_scream_device *dev)
{
    struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };
    struct kvec iov;
    int ret = 0;

    memcpy(lastbuf, dev->network_buffer, SCREAM_HEADER_SIZE);
    lastbuf[4] = 0x80;

    iov.iov_base = lastbuf;

    if (dev->is_tcp) {
        if (atomic_read(&dev->connection_state) != STATE_CONNECTED)
            return -ENOTCONN;
        iov.iov_len = SCREAM_PACKET_SIZE;
        ret = kernel_sendmsg(dev->sock, &msg, &iov, 1, SCREAM_PACKET_SIZE);
    } else {
        iov.iov_len = SCREAM_HEADER_SIZE;
        msg.msg_name = &dev->remote_addr;
        msg.msg_namelen = sizeof(dev->remote_addr);
        ret = kernel_sendmsg(dev->sock, &msg, &iov, 1, SCREAM_HEADER_SIZE);
    }
    return ret;
}

static void scream_build_payload_locked(struct snd_scream_device *dev,
                                        struct snd_pcm_runtime *runtime,
                                        size_t current_hw_ptr,
                                        void *data)
{
    size_t buffer_size =runtime->buffer_size*4*dev->channels;
    if (current_hw_ptr + SCREAM_PAYLOAD_SIZE > buffer_size) {
        size_t len1 = buffer_size - current_hw_ptr;
        size_t len2 = SCREAM_PAYLOAD_SIZE - len1;
        memcpy(data, runtime->dma_area + current_hw_ptr, len1);
        memcpy(data + len1, runtime->dma_area, len2);
    } else {
        memcpy(data, runtime->dma_area + current_hw_ptr, SCREAM_PAYLOAD_SIZE);
    }
    if(dev->is_dsd)
        convert_data(data, SCREAM_PAYLOAD_SIZE/8);
}

static int scream_playback_thread(void *data)
{
    struct snd_scream_device *dev = data;
    struct snd_pcm_substream *sub;
    struct snd_pcm_runtime *rt;
    ktime_t next_wake;
    bool is_first_packet;

    /* Set realtime priority SCHED_FIFO 50 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
    sched_set_fifo(current);
#else
    struct sched_param param = { .sched_priority = 50 };
    sched_setscheduler(current, SCHED_FIFO, &param);
#endif

    while (!kthread_should_stop()) {
        wait_event_interruptible(dev->playback_waitq, kthread_should_stop() || dev->is_running);
        
        if (kthread_should_stop())
            break;

        is_first_packet = true;
        next_wake = ktime_get();

        while (!kthread_should_stop()) {
            unsigned long flags;
            snd_pcm_sframes_t avail_fr;
            bool do_send = false;

            spin_lock_irqsave(&dev->lock, flags);
        if (!dev->is_running) {
            spin_unlock_irqrestore(&dev->lock, flags);
            break;
        }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
        sub = READ_ONCE(dev->substream);
#else
        sub = dev->substream;
#endif
        if (!sub) {
            spin_unlock_irqrestore(&dev->lock, flags);
            usleep_range(1000, 2000);
            continue;
        }

        rt = sub->runtime;
        avail_fr = snd_pcm_playback_hw_avail(rt);
        if (avail_fr < 0) avail_fr = 0;

        if (avail_fr * 4 * dev->channels >= SCREAM_PAYLOAD_SIZE) {
            size_t buf_bytes = rt->buffer_size * 4 * dev->channels;
            
            scream_build_payload_locked(dev, rt, dev->hw_ptr,
                                        dev->network_buffer + SCREAM_HEADER_SIZE);
            dev->hw_ptr = (dev->hw_ptr + SCREAM_PAYLOAD_SIZE) % buf_bytes;
            
            do_send = true;
        }
        spin_unlock_irqrestore(&dev->lock, flags);

        if (do_send) {
            if (!dev->is_tcp || atomic_read(&dev->connection_state) == STATE_CONNECTED) {
                struct msghdr msg = { .msg_flags = MSG_DONTWAIT | MSG_NOSIGNAL };
                struct kvec iov = { .iov_base = dev->network_buffer, .iov_len = SCREAM_PACKET_SIZE };
                int ret;
                if (!dev->is_tcp) {
                    msg.msg_name = &dev->remote_addr;
                    msg.msg_namelen = sizeof(dev->remote_addr);
                }
                ret = kernel_sendmsg(dev->sock, &msg, &iov, 1, SCREAM_PACKET_SIZE);
                if (ret < 0 && dev->is_tcp) {
                    if (ret != -EAGAIN && ret != -ENOBUFS) {
                        unsigned int delay = scream_reconnect_delay_ms_for_err(ret);
                        if (atomic_cmpxchg(&dev->connection_state, STATE_CONNECTED, STATE_DISCONNECTED) == STATE_CONNECTED) {
                            if (!atomic_read(&dev->closing))
                                schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(delay));
                        }
                    }
                } else if (dev->is_tcp && ret != SCREAM_PACKET_SIZE) {
                    /* Force reconnect on partial send to prevent receiver desync */
                    if (atomic_cmpxchg(&dev->connection_state, STATE_CONNECTED, STATE_DISCONNECTED) == STATE_CONNECTED) {
                        if (!atomic_read(&dev->closing))
                            schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(100));
                    }
                }
            }
            
            /* Handle ALSA period elapsed natively */
            dev->bytes_in_period += SCREAM_PAYLOAD_SIZE;
            if (dev->bytes_in_period >= dev->alsa_period_bytes) {
                dev->bytes_in_period -= dev->alsa_period_bytes;
                snd_pcm_period_elapsed(sub);
            }

            /* Wait precisely for the next packet interval */
            if (is_first_packet) {
                next_wake = ktime_get();
                is_first_packet = false;
            } else {
                next_wake = ktime_add(next_wake, dev->period_time_ns);
                
                /* Catch up if we are severely behind */
                if (ktime_compare(ktime_get(), next_wake) > 0) {
                     next_wake = ktime_get();
                } else {
                     set_current_state(TASK_INTERRUPTIBLE);
                     schedule_hrtimeout(&next_wake, HRTIMER_MODE_ABS);
                }
            }

        } else {
            /* No data available, sleep lightly to avoid CPU burn */
            usleep_range(200, 500);
            next_wake = ktime_get(); /* reset timeline after underrun */
        }
    }
    }
    return 0;
}

static int snd_scream_pcm_open(struct snd_pcm_substream *substream)
{
    struct snd_scream_device *dev = snd_pcm_substream_chip(substream);
    struct snd_pcm_runtime *runtime = substream->runtime;
    int ret;

    atomic_set(&dev->closing, 0);
    dev->substream = substream;
    runtime->hw = snd_scream_hw;
    ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    if (ret < 0)
        return ret;
    dev->is_tcp = sysfs_streq(protocol_str, "tcp");

    /* Reuse existing socket for seamless track switching */
    if (dev->sock) {
        if (dev->is_tcp &&
            atomic_read(&dev->connection_state) != STATE_DISCONNECTED)
            return 0;  /* TCP connected/connecting - reuse */
        if (!dev->is_tcp)
            return 0;  /* UDP - always reuse */

        /* TCP disconnected - clean up stale socket */
        atomic_set(&dev->closing, 1);
        cancel_delayed_work_sync(&dev->reconnect_work);
        sock_release(dev->sock);
        dev->sock = NULL;
        atomic_set(&dev->connection_state, STATE_DISCONNECTED);
        atomic_set(&dev->reconnect_attempts, 0);
        atomic_set(&dev->closing, 0);
    }

    ret = SCREAM_SOCK_CREATE(AF_INET,
                           dev->is_tcp ? SOCK_STREAM : SOCK_DGRAM,
                           dev->is_tcp ? IPPROTO_TCP : IPPROTO_UDP,
                           &dev->sock);
    if (ret < 0)
        return ret;

    memset(&dev->remote_addr, 0, sizeof(dev->remote_addr));
    dev->remote_addr.sin_family = AF_INET;
    dev->remote_addr.sin_port = htons(port);
    dev->remote_addr.sin_addr.s_addr = in_aton(ip_addr_str);

    if (dev->is_tcp) {
        atomic_set(&dev->connection_state, STATE_DISCONNECTED);
        {
            int opt = 1;
            SET_SOCKOPT(dev->sock, SOL_TCP, TCP_NODELAY, (char *)&opt, sizeof(opt));
            opt = 1;
            SET_SOCKOPT(dev->sock, SOL_SOCKET, SO_KEEPALIVE, (char *)&opt, sizeof(opt));
            opt = 3000;
            SET_SOCKOPT(dev->sock, SOL_TCP, TCP_USER_TIMEOUT, (char *)&opt, sizeof(opt));
        }
        schedule_delayed_work(&dev->reconnect_work, msecs_to_jiffies(100));
    } else {
        atomic_set(&dev->connection_state, STATE_CONNECTED);
    }

    if (!dev->playback_thread) {
        dev->playback_thread = kthread_run(scream_playback_thread, dev, "scream_tx");
        if (IS_ERR(dev->playback_thread)) {
            pr_err(DRIVER_NAME ": Failed to create playback thread\n");
            dev->playback_thread = NULL;
            return -ENOMEM;
        }
    }

    return 0;
}

static int snd_scream_pcm_close(struct snd_pcm_substream *substream)
{
    struct snd_scream_device *dev = snd_pcm_substream_chip(substream);
    unsigned long flags;
    struct task_struct *thd = NULL;

    /* Stop playback first if still running */
    spin_lock_irqsave(&dev->lock, flags);
    if (dev->is_running) {
        dev->is_running = false;
        thd = dev->playback_thread;
        dev->playback_thread = NULL;
    }
    spin_unlock_irqrestore(&dev->lock, flags);

    if (thd) {
        kthread_stop(thd);
    }

    /* Send end-of-track marker */
    if (dev->sock && atomic_read(&dev->connection_state) == STATE_CONNECTED) {
        scream_send_last_packet(dev);
    }

    /* Keep socket alive for seamless track switching */
    spin_lock_irqsave(&dev->lock, flags);
    dev->substream = NULL;
    spin_unlock_irqrestore(&dev->lock, flags);
    return 0;
}

static int snd_scream_pcm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
    struct snd_scream_device *dev = snd_pcm_substream_chip(substream);
    int ret;
    unsigned int srt;

    ret = snd_pcm_lib_malloc_pages(substream, params_buffer_bytes(params));
    if (ret < 0)
        return ret;

    dev->sample_rate = params_rate(params);
    dev->channels = params_channels(params);
    dev->format = params_format(params);


#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
#ifdef SNDRV_PCM_FORMAT_DSD_U32_BE
    dev->is_dsd = (dev->format == SNDRV_PCM_FORMAT_DSD_U32_BE);
#else
    dev->is_dsd = false;
#endif
#else
    dev->is_dsd = false;
#endif

    /* Scream 5-byte header */
    if (dev->is_dsd) {
        srt = dev->sample_rate / 2;      /* DSD marker uses sample_rate/2 */
        dev->network_buffer[1] = 1;      /* DSD marker */
    } else {
        srt = dev->sample_rate;
        dev->network_buffer[1] = 32;     /* 32-bit PCM */
    }

    dev->network_buffer[0] = (u8)((srt % 44100) ? (0 + (srt / 48000)) : (128 + (srt / 44100)));
    dev->network_buffer[2] = (u8)dev->channels;
    dev->network_buffer[3] = ch_mask[dev->channels];
    dev->network_buffer[4] = 0;
     {
         unsigned int frame_bytes = (snd_pcm_format_physical_width(dev->format) / 8) * dev->channels;
         u64 num = (u64)SCREAM_PAYLOAD_SIZE * 1000000000ULL; /* bytes * 1e9 */
         do_div(num, (u32)(dev->sample_rate * frame_bytes)); /* -> nanoseconds per 1152 bytes */
         dev->period_time_ns = ktime_set(0, (unsigned long)num);
     }
     
     dev->alsa_period_bytes = params_period_size(params) * dev->channels * 4;
     dev->bytes_in_period = 0;

    return 0;
}

static int snd_scream_pcm_prepare(struct snd_pcm_substream *substream)
{
    struct snd_scream_device *dev = snd_pcm_substream_chip(substream);
    dev->hw_ptr = 0;
    substream->runtime->start_threshold = substream->runtime->period_size;
    substream->runtime->stop_threshold = substream->runtime->buffer_size;
    return 0;
}

static int snd_scream_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct snd_scream_device *dev = snd_pcm_substream_chip(substream);
    unsigned long flags;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
        spin_lock_irqsave(&dev->lock, flags);
        if (!dev->is_running) {
            dev->is_running = true;
            wake_up_interruptible(&dev->playback_waitq);
        }
        spin_unlock_irqrestore(&dev->lock, flags);
        break;
    case SNDRV_PCM_TRIGGER_STOP:
        spin_lock_irqsave(&dev->lock, flags);
        if (dev->is_running) {
            dev->is_running = false;
        }
        spin_unlock_irqrestore(&dev->lock, flags);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static snd_pcm_uframes_t snd_scream_pcm_pointer(struct snd_pcm_substream *substream)
{
    size_t frames;
    unsigned long flags;
    struct snd_scream_device *dev = snd_pcm_substream_chip(substream);
    spin_lock_irqsave(&dev->lock, flags);
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
    frames = READ_ONCE(dev->hw_ptr) / 4 / dev->channels;
    #else
    frames = dev->hw_ptr / 4 / dev->channels;
    #endif
    spin_unlock_irqrestore(&dev->lock, flags);
    return frames;
}

/* ioctl: forward to helper to avoid crashes */
static int snd_scream_pcm_ioctl(struct snd_pcm_substream *substream, unsigned int cmd, void *arg)
{
    return snd_pcm_lib_ioctl(substream, cmd, arg);
}

/* .page: works for both vmalloc/kmalloc */
static struct page *snd_scream_pcm_page(struct snd_pcm_substream *substream, unsigned long offset)
{
    void *addr = substream->runtime->dma_area + offset;
    return is_vmalloc_addr(addr) ? vmalloc_to_page(addr) : virt_to_page(addr);
}
/*
static int scream_pcm_copy(struct snd_pcm_substream *sub, int channel,
snd_pcm_uframes_t pos, void __user *src,
snd_pcm_uframes_t frames)
{
    struct snd_pcm_runtime *rt = sub->runtime;
    size_t bytes = frames_to_bytes(rt, frames);
    size_t off = frames_to_bytes(rt, pos);
    void *dst = rt->dma_area + off;
    if (copy_from_user(dst, src, bytes))
    return -EFAULT;
    return 0;
}

static int scream_pcm_silence(struct snd_pcm_substream *sub, int channel,
snd_pcm_uframes_t pos, snd_pcm_uframes_t frames)
{
    struct snd_pcm_runtime *rt = sub->runtime;
    memset(rt->dma_area + frames_to_bytes(rt, pos), 0, frames_to_bytes(rt, frames));
    return 0;
}
*/
static struct snd_pcm_ops snd_scream_pcm_ops = {
    .open = snd_scream_pcm_open,
    .close = snd_scream_pcm_close,
    .ioctl = snd_scream_pcm_ioctl,
    .hw_params = snd_scream_pcm_hw_params,
    .hw_free = snd_pcm_lib_free_pages,
    .prepare = snd_scream_pcm_prepare,
    .trigger = snd_scream_pcm_trigger,
    .pointer = snd_scream_pcm_pointer,
    .page = snd_scream_pcm_page,
//    .copy = scream_pcm_copy,
//    .silence = scream_pcm_silence,
};


static int __init alsa_scream_driver_init(void)
{
    int ret;
    struct snd_card *card;
    struct snd_scream_device *dev;
    struct snd_pcm *pcm;

    /* Register a dummy platform device to provide a valid parent struct device */
    scream_pdev = platform_device_register_simple("screamalsa", -1, NULL, 0);
    if (IS_ERR(scream_pdev))
        return PTR_ERR(scream_pdev);

    ret = snd_card_new(&scream_pdev->dev, -1, DRIVER_NAME, THIS_MODULE, 0, &card);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": Failed to create sound card: %d\n", ret);
        platform_device_unregister(scream_pdev);
        scream_pdev = NULL;
        return ret;
    }
    /* Allocate private data separately */
    dev = kzalloc(sizeof(struct snd_scream_device), GFP_KERNEL);
    if (!dev) {
        pr_err(DRIVER_NAME ": Failed to allocate private data\n");
        snd_card_free(card);
        platform_device_unregister(scream_pdev);
        scream_pdev = NULL;
        return -ENOMEM;
    }

    card->private_data = dev;
    strcpy(card->driver, DRIVER_NAME);
    strcpy(card->shortname, "ScreamALSA (Network)");
    snprintf(card->longname, sizeof(card->longname), "%s, streaming to %s:%d",
             card->shortname, ip_addr_str, port);

    snd_card_set_dev(card, &scream_pdev->dev);

    dev->card = card;
    spin_lock_init(&dev->lock);
    init_waitqueue_head(&dev->playback_waitq);
    INIT_DELAYED_WORK(&dev->reconnect_work, scream_reconnect_work);
    atomic_set(&dev->connection_state, STATE_DISCONNECTED);
    atomic_set(&dev->reconnect_attempts, 0);
    atomic_set(&dev->closing, 0);
    dev->sock = NULL;
    dev->substream = NULL;
    dev->is_running = false;
    dev->hw_ptr = 0;
    dev->playback_thread = NULL;
    dev->bytes_in_period = 0;
    dev->alsa_period_bytes = 0;

    ret = snd_pcm_new(card, "Scream HQ PCM", 0, 1, 0, &pcm);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": Failed to create PCM device: %d\n", ret);
        goto cleanup_dev;
    }

    dev->pcm = pcm;
    pcm->private_data = dev;
    strcpy(pcm->name, "Scream HQ Virtual Audio");

    snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &snd_scream_pcm_ops);
    snd_pcm_lib_preallocate_pages_for_all(pcm, SCREAM_DMA_TYPE, SCREAM_DMA_DATA, 128 * 1024, 1024 * 1024);
    ret = snd_card_register(card);
    if (ret < 0) {
        pr_err(DRIVER_NAME ": Failed to register sound card: %d\n", ret);
        goto cleanup_dev;
    }

    scream_card_ptr = card;
    pr_info(DRIVER_NAME ": driver loaded successfully.\n");
    return 0;

cleanup_dev:
    kfree(dev);
    snd_card_free(card);
    platform_device_unregister(scream_pdev);
    scream_pdev = NULL;
    return ret;
}

static void __exit alsa_scream_driver_exit(void)
{
    if (scream_card_ptr) {
        struct snd_scream_device *dev = scream_card_ptr->private_data;

        if (dev) {
            /* Stop playback first if running */
            unsigned long flags;
            struct task_struct *thd = NULL;
            spin_lock_irqsave(&dev->lock, flags);
            if (dev->is_running) {
                dev->is_running = false;
                thd = dev->playback_thread;
                dev->playback_thread = NULL;
            }
            spin_unlock_irqrestore(&dev->lock, flags);
            
            if (thd) {
                kthread_stop(thd);
            }
            
            cancel_delayed_work_sync(&dev->reconnect_work);

            scream_cleanup_resources(dev);
            kfree(dev);
        }

        snd_card_free(scream_card_ptr);
        scream_card_ptr = NULL;
    }
    if (scream_pdev) {
        platform_device_unregister(scream_pdev);
        scream_pdev = NULL;
    }
    pr_info(DRIVER_NAME ": driver unloaded.\n");
}

module_init(alsa_scream_driver_init);
module_exit(alsa_scream_driver_exit);

