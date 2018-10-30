/****************************************************************************
 * drivers/net/rpmsgdrv.c
 *
 *   Copyright (C) 2018 Pinecone Inc. All rights reserved.
 *   Author: Jianli Dong <dongjianli@pinecone.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>

#include <openamp/open_amp.h>

#include <nuttx/kmalloc.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include <nuttx/net/arp.h>
#include <nuttx/net/dns.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/pkt.h>
#include <nuttx/net/rpmsg.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The address family that we used to create the socket really does not
 * matter. It should, however, be valid in the current configuration.
 */

#if defined(CONFIG_NET_IPv4)
#  define NET_RPMSG_DRV_FAMILY     AF_INET
#elif defined(CONFIG_NET_IPv6)
#  define NET_RPMSG_DRV_FAMILY     AF_INET6
#elif defined(CONFIG_NET_IEEE802154)
#  define NET_RPMSG_DRV_FAMILY     AF_IEEE802154
#elif defined(CONFIG_WIRELESS_PKTRADIO)
#  define NET_RPMSG_DRV_FAMILY     AF_PKTRADIO
#elif defined(CONFIG_NET_USRSOCK)
#  define NET_RPMSG_DRV_FAMILY     AF_INET
#elif defined(CONFIG_NET_PKT)
#  define NET_RPMSG_DRV_FAMILY     AF_PACKET
#elif defined(CONFIG_NET_LOCAL)
#  define NET_RPMSG_DRV_FAMILY     AF_LOCAL
#else
#  define NET_RPMSG_DRV_FAMILY     AF_UNSPEC
#endif

/* SOCK_DGRAM is the preferred socket type to use when we just want a
 * socket for performing driver ioctls.  However, we can't use SOCK_DRAM
 * if UDP is disabled.
 *
 * Pick a socket type (and perhaps protocol) compatible with the currently
 * selected address family.
 */

#if NET_RPMSG_DRV_FAMILY == AF_INET
#  if defined(CONFIG_NET_UDP)
#    define NET_RPMSG_DRV_TYPE     SOCK_DGRAM
#  elif defined(CONFIG_NET_TCP)
#   define NET_RPMSG_DRV_TYPE      SOCK_STREAM
#  elif defined(CONFIG_NET_ICMP_SOCKET)
#   define NET_RPMSG_DRV_TYPE      SOCK_DGRAM
#   define NET_RPMSG_DRV_PROTOCOL  IPPROTO_ICMP
#  endif
#elif NET_RPMSG_DRV_FAMILY == AF_INET6
#  if defined(CONFIG_NET_UDP)
#    define NET_RPMSG_DRV_TYPE     SOCK_DGRAM
#  elif defined(CONFIG_NET_TCP)
#   define NET_RPMSG_DRV_TYPE      SOCK_STREAM
#  elif defined(CONFIG_NET_ICMPv6_SOCKET)
#   define NET_RPMSG_DRV_TYPE      SOCK_DGRAM
#   define NET_RPMSG_DRV_PROTOCOL  IPPROTO_ICMP6
#  endif
#elif NET_RPMSG_DRV_FAMILY == AF_IEEE802154
#  define NET_RPMSG_DRV_TYPE       SOCK_DGRAM
#elif NET_RPMSG_DRV_FAMILY == AF_PKTRADIO
#  define NET_RPMSG_DRV_TYPE       SOCK_DGRAM
#elif NET_RPMSG_DRV_FAMILY == AF_PACKET
#  define NET_RPMSG_DRV_TYPE       SOCK_RAW
#elif NET_RPMSG_DRV_FAMILY == AF_LOCAL
#  if defined(CONFIG_NET_LOCAL_DGRAM)
#    define NET_RPMSG_DRV_TYPE     SOCK_DGRAM
#  elif defined(CONFIG_NET_LOCAL_STREAM)
#     define NET_RPMSG_DRV_TYPE    SOCK_STREAM
#  endif
#endif

/* Socket protocol of zero normally works */

#ifndef NET_RPMSG_DRV_PROTOCOL
#  define NET_RPMSG_DRV_PROTOCOL   0
#endif

/* Work queue support is required. */

#if !defined(CONFIG_SCHED_WORKQUEUE)
#  error Work queue support is required in this configuration (CONFIG_SCHED_WORKQUEUE)
#else

/* Use the selected work queue */

#  if defined(CONFIG_NET_RPMSG_DRV_HPWORK)
#    define NET_RPMSG_DRV_WORK     HPWORK
#  elif defined(CONFIG_NET_RPMSG_DRV_LPWORK)
#    define NET_RPMSG_DRV_WORK     LPWORK
#  else
#    error Neither CONFIG_NET_RPMSG_DRV_HPWORK nor CONFIG_NET_RPMSG_DRV_LPWORK defined
#  endif
#endif

#ifdef CONFIG_NET_DUMPPACKET
#  define net_rpmsg_drv_dumppacket lib_dumpbuffer
#else
#  define net_rpmsg_drv_dumppacket(m, b, l)
#endif

/* TX poll delay = 1 seconds. CLK_TCK is the number of clock ticks per second */

#define NET_RPMSG_DRV_WDDELAY      (1*CLK_TCK)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct net_rpmsg_drv_cookie_s
{
  struct net_rpmsg_header_s *header;
  sem_t                      sem;
};

/* net_rpmsg_drv_s encapsulates all state information for a single hardware
 * interface
 */

struct net_rpmsg_drv_s
{
  FAR const char       *cpuname;
  char                 channelname[RPMSG_NAME_SIZE];
  sem_t                channelready;
  struct rpmsg_channel *channel;
  WDOG_ID              txpoll;   /* TX poll timer */
  struct work_s        pollwork; /* For deferring poll work to the work queue */

  /* This holds the information visible to the NuttX network */

  struct net_driver_s  dev;      /* Interface understood by the network */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Common TX logic */

static int  net_rpmsg_drv_transmit(FAR struct net_driver_s *dev, bool nocopy);
static int  net_rpmsg_drv_txpoll(FAR struct net_driver_s *dev);
static void net_rpmsg_drv_reply(FAR struct net_driver_s *dev);

/* RPMSG related functions */

static void net_rpmsg_drv_default_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src);
static void net_rpmsg_drv_sockioctl_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src);
static void net_rpmsg_drv_transfer_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src);

static void net_rpmsg_drv_device_created(struct remote_device *rdev, void *priv_);
static void net_rpmsg_drv_channel_created(struct rpmsg_channel *channel);
static void net_rpmsg_drv_channel_destroyed(struct rpmsg_channel *channel);
static void net_rpmsg_drv_channel_received(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src);

static int  net_rpmsg_drv_send_recv(struct net_driver_s *dev,
                    void *header_, uint32_t command, int len);

/* Watchdog timer expirations */

static void net_rpmsg_drv_poll_work(FAR void *arg);
static void net_rpmsg_drv_poll_expiry(int argc, wdparm_t arg, ...);

/* NuttX callback functions */

static int  net_rpmsg_drv_ifup(FAR struct net_driver_s *dev);
static int  net_rpmsg_drv_ifdown(FAR struct net_driver_s *dev);

static void net_rpmsg_drv_txavail_work(FAR void *arg);
static int  net_rpmsg_drv_txavail(FAR struct net_driver_s *dev);

#if defined(CONFIG_NET_IGMP) || defined(CONFIG_NET_ICMPv6)
static int  net_rpmsg_drv_addmac(FAR struct net_driver_s *dev,
              FAR const uint8_t *mac);
#ifdef CONFIG_NET_IGMP
static int  net_rpmsg_drv_rmmac(FAR struct net_driver_s *dev,
              FAR const uint8_t *mac);
#endif
#ifdef CONFIG_NET_ICMPv6
static void net_rpmsg_drv_ipv6multicast(FAR struct net_driver_s *dev);
#endif
#endif
#ifdef CONFIG_NETDEV_IOCTL
static int  net_rpmsg_drv_ioctl(FAR struct net_driver_s *dev, int cmd,
              unsigned long arg);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const rpmsg_rx_cb_t g_net_rpmsg_drv_handler[] =
{
  [NET_RPMSG_IFUP]      = net_rpmsg_drv_default_handler,
  [NET_RPMSG_IFDOWN]    = net_rpmsg_drv_default_handler,
  [NET_RPMSG_ADDMCAST]  = net_rpmsg_drv_default_handler,
  [NET_RPMSG_RMMCAST]   = net_rpmsg_drv_default_handler,
  [NET_RPMSG_DEVIOCTL]  = net_rpmsg_drv_default_handler,
  [NET_RPMSG_SOCKIOCTL] = net_rpmsg_drv_sockioctl_handler,
  [NET_RPMSG_TRANSFER]  = net_rpmsg_drv_transfer_handler,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void net_rpmsg_drv_wait(sem_t *sem)
{
  int ret;

  do
    {
      /* Take the semaphore (perhaps waiting) */

      ret = net_lockedwait(sem);

      /* The only case that an error should occur here is if the wait was
       * awakened by a signal.
       */

      DEBUGASSERT(ret == OK || ret == -EINTR);
    }
  while (ret == -EINTR);
}

/****************************************************************************
 * Name: net_rpmsg_drv_transmit
 *
 * Description:
 *   Start hardware transmission. Called from watchdog based polling.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int net_rpmsg_drv_transmit(FAR struct net_driver_s *dev, bool nocopy)
{
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;
  FAR struct net_rpmsg_transfer_s *msg;
  int ret;

  /* Verify that the hardware is ready to send another packet. If we get
   * here, then we are committed to sending a packet; Higher level logic
   * must have assured that there is no transmission in progress.
   */

  /* Increment statistics */

  net_rpmsg_drv_dumppacket("transmit", dev->d_buf, dev->d_len);
  NETDEV_TXPACKETS(dev);

  /* Send the packet: address=dev->d_buf, length=dev->d_len */

  msg = (FAR struct net_rpmsg_transfer_s *)dev->d_buf - 1;

  msg->header.command = NET_RPMSG_TRANSFER;
  msg->header.result  = 0;
  msg->header.cookie  = 0;
  msg->length         = dev->d_len;

  if (nocopy)
    {
      ret = rpmsg_send_nocopy(priv->channel, msg, sizeof(*msg) + msg->length);
    }
  else
    {
      ret = rpmsg_send(priv->channel, msg, sizeof(*msg) + msg->length);
    }

  if (ret < 0)
    {
      NETDEV_TXERRORS(dev);
      return ret;
    }
  else
    {
      NETDEV_TXDONE(dev);
      return OK;
    }
}

/****************************************************************************
 * Name: net_rpmsg_drv_txpoll
 *
 * Description:
 *   The transmitter is available, check if the network has any outgoing
 *   packets ready to send.  This is a callback from devif_poll().
 *   devif_poll() may be called:
 *
 *   1. When the preceding TX packet send is complete,
 *   2. When the preceding TX packet send fail
 *   3. During normal TX polling
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   OK on success; a negated errno on failure
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int net_rpmsg_drv_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;
  uint32_t size;

  /* If the polling resulted in data that should be sent out on the network,
   * the field d_len is set to a value > 0.
   */

  if (dev->d_len > 0)
    {
      /* Look up the destination MAC address and add it to the Ethernet
       * header.
       */

#ifdef CONFIG_NET_IPv4
      if (IFF_IS_IPv4(dev->d_flags))
        {
          arp_out(dev);
        }
#endif /* CONFIG_NET_IPv4 */
#ifdef CONFIG_NET_IPv6
      if (IFF_IS_IPv6(dev->d_flags))
        {
          neighbor_out(dev);
        }
#endif /* CONFIG_NET_IPv6 */

      if (!devif_loopback(dev))
        {
          /* Send the packet */

          net_rpmsg_drv_transmit(dev, true);

          /* Check if there is room in the device to hold another packet. If not,
           * return a non-zero value to terminate the poll.
           */

          dev->d_buf = rpmsg_get_tx_payload_buffer(priv->channel, &size, false);
          if (dev->d_buf)
            {
              dev->d_buf += sizeof(struct net_rpmsg_transfer_s);
            }

          return dev->d_buf == NULL;
        }
    }

  /* If zero is returned, the polling will continue until all connections have
   * been examined.
   */

  return 0;
}

/****************************************************************************
 * Name: net_rpmsg_drv_reply
 *
 * Description:
 *   After a packet has been received and dispatched to the network, it
 *   may return with an outgoing packet. This function checks for
 *   that case and performs the transmission if necessary.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static void net_rpmsg_drv_reply(FAR struct net_driver_s *dev)
{
  /* If the packet dispatch resulted in data that should be sent out on the
   * network, the field d_len will set to a value > 0.
   */

  if (dev->d_len > 0)
    {
      /* Update the Ethernet header with the correct MAC address */

#ifdef CONFIG_NET_IPv4
      if (IFF_IS_IPv4(dev->d_flags))
        {
          arp_out(dev);
        }
#endif
#ifdef CONFIG_NET_IPv6
      if (IFF_IS_IPv6(dev->d_flags))
        {
          neighbor_out(dev);
        }
#endif

      /* And send the packet */

      net_rpmsg_drv_transmit(dev, false);
    }
}

/* RPMSG related functions */

static void net_rpmsg_drv_default_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src)
{
  struct net_rpmsg_header_s *header = data;
  struct net_rpmsg_drv_cookie_s *cookie =
    (struct net_rpmsg_drv_cookie_s *)(uintptr_t)header->cookie;

  memcpy(cookie->header, header, len);
  nxsem_post(&cookie->sem);
}

static int net_rpmsg_drv_sockioctl_task(int argc, FAR char *argv[])
{
  struct net_rpmsg_ioctl_s *msg;
  struct rpmsg_channel *channel;
  struct socket sock;

  /* Restore pointers from argv */

  channel = (struct rpmsg_channel *)strtoul(argv[1], NULL, 0);
  msg = (struct net_rpmsg_ioctl_s *)strtoul(argv[2], NULL, 0);

  /* We need a temporary sock for ioctl here */

  sock.s_crefs = 1; /* Initialize reference count manually */
  msg->header.result = psock_socket(NET_RPMSG_DRV_FAMILY,
          NET_RPMSG_DRV_TYPE, NET_RPMSG_DRV_PROTOCOL, &sock);
  if (msg->header.result >= 0)
    {
      msg->header.result = psock_ioctl(&sock, msg->code, (unsigned long)msg->arg);
      psock_close(&sock); /* Close the temporary sock */
    }

  rpmsg_send(channel, msg, sizeof(*msg) + msg->length);
  rpmsg_release_rx_buffer(channel, msg);

  return 0;
}

static void net_rpmsg_drv_sockioctl_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src)
{
  char *argv[3];
  char arg1[16];
  char arg2[16];
  int pid;

  /* Save pointers into argv */

  sprintf(arg1, "%#p", channel);
  sprintf(arg2, "%#p", data);

  argv[0] = arg1;
  argv[1] = arg2;
  argv[2] = NULL;

  /* Move the action into a temp thread to avoid the deadlock */

  rpmsg_hold_rx_buffer(channel, data);

  pid = kthread_create("rpmsg-net", CONFIG_RPTUN_PRIORITY,
          CONFIG_RPTUN_STACKSIZE, net_rpmsg_drv_sockioctl_task, argv);
  if (pid < 0)
    {
      rpmsg_send(channel, data, len);
      rpmsg_release_rx_buffer(channel, data);
    }
}

/****************************************************************************
 * Name: net_rpmsg_drv_transfer_handler
 *
 * Description:
 *   An message was received indicating the availability of a new RX packet
 *
 * Parameters:
 *   channel - Reference to the channel which receive the message
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_IPv4
static bool net_rpmsg_drv_is_ipv4(struct net_driver_s *dev)
{
  struct ipv4_hdr_s *ip = (struct ipv4_hdr_s *)(dev->d_buf + dev->d_llhdrlen);
  struct eth_hdr_s *eth = (struct eth_hdr_s *)dev->d_buf;

  if (dev->d_lltype == NET_LL_ETHERNET || dev->d_lltype == NET_LL_IEEE80211)
    {
      return eth->type == HTONS(ETHTYPE_IP);
    }
  else
    {
      return (ip->vhl & IP_VERSION_MASK) == IPv4_VERSION;
    }
}
#endif

#ifdef CONFIG_NET_IPv6
static bool net_rpmsg_drv_is_ipv6(struct net_driver_s *dev)
{
  struct ipv6_hdr_s *ip = (struct ipv6_hdr_s *)(dev->d_buf + dev->d_llhdrlen);
  struct eth_hdr_s *eth = (struct eth_hdr_s *)dev->d_buf;

  if (dev->d_lltype == NET_LL_ETHERNET || dev->d_lltype == NET_LL_IEEE80211)
    {
      return eth->type == HTONS(ETHTYPE_IP6);
    }
  else
    {
      return (ip->vtc & IP_VERSION_MASK) == IPv6_VERSION;
    }
}
#endif

#ifdef CONFIG_NET_ARP
static bool net_rpmsg_drv_is_arp(struct net_driver_s *dev)
{
  struct eth_hdr_s *eth = (struct eth_hdr_s *)dev->d_buf;

  if (dev->d_lltype == NET_LL_ETHERNET || dev->d_lltype == NET_LL_IEEE80211)
    {
      return eth->type == HTONS(ETHTYPE_ARP);
    }
  else
    {
      return false;
    }
}
#endif

static void net_rpmsg_drv_transfer_handler(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src)
{
  struct net_driver_s *dev = rpmsg_get_privdata(channel);
  struct net_rpmsg_transfer_s *msg = data;
  void *oldbuf;

  if (dev == NULL)
    {
      return;
    }

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Check for errors and update statistics */

  net_rpmsg_drv_dumppacket("receive", msg->data, msg->length);

  /* Check if the packet is a valid size for the network buffer
   * configuration.
   */

  if (msg->length < dev->d_llhdrlen || msg->length > dev->d_pktsize)
    {
      NETDEV_RXERRORS(dev);
      net_unlock();
      return;
    }
  else
    {
      NETDEV_RXPACKETS(dev);
    }

  /* Copy the data from the hardware to dev->d_buf. Set
   * amount of data in dev->d_len
   */

  oldbuf = dev->d_buf;

  dev->d_buf = msg->data;
  dev->d_len = msg->length;

#ifdef CONFIG_NET_PKT
  /* When packet sockets are enabled, feed the frame into the packet tap */

  pkt_input(dev);
#endif

  /* We only accept IP packets of the configured type and ARP packets */

#ifdef CONFIG_NET_IPv4
  if (net_rpmsg_drv_is_ipv4(dev))
    {
      ninfo("IPv4 frame\n");
      NETDEV_RXIPV4(dev);

      /* Handle ARP on input, then dispatch IPv4 packet to the network
       * layer.
       */

      arp_ipin(dev);
      ipv4_input(dev);

      /* Check for a reply to the IPv4 packet */

      net_rpmsg_drv_reply(dev);
    }
  else
#endif
#ifdef CONFIG_NET_IPv6
  if (net_rpmsg_drv_is_ipv6(dev))
    {
      ninfo("Iv6 frame\n");
      NETDEV_RXIPV6(dev);

      /* Dispatch IPv6 packet to the network layer */

      ipv6_input(dev);

      /* Check for a reply to the IPv6 packet */

      net_rpmsg_drv_reply(dev);
    }
  else
#endif
#ifdef CONFIG_NET_ARP
  if (net_rpmsg_drv_is_arp(dev))
    {
      ninfo("ARP frame\n");
      NETDEV_RXARP(dev);

      /* Dispatch ARP packet to the network layer */

      arp_arpin(dev);

      /* Check for a reply to the ARP packet */

      net_rpmsg_drv_reply(dev);
    }
  else
#endif
    {
      NETDEV_RXDROPPED(dev);
    }

  dev->d_buf = oldbuf;
  net_unlock();
}

static void net_rpmsg_drv_device_created(struct remote_device *rdev, void *priv_)
{
  struct net_driver_s *dev = priv_;
  struct net_rpmsg_drv_s *priv = dev->d_private;
  struct rpmsg_channel *channel;

  if (strcmp(priv->cpuname, rdev->proc->cpu_name) == 0)
    {
      channel = rpmsg_create_channel(rdev, priv->channelname);
      if (channel != NULL)
        {
          rpmsg_set_privdata(channel, dev);
        }
    }
}

static void net_rpmsg_drv_channel_created(struct rpmsg_channel *channel)
{
  struct net_driver_s *dev = rpmsg_get_privdata(channel);
  struct net_rpmsg_drv_s *priv;
  uint32_t size;

  if (dev != NULL)
    {
      size  = rpmsg_get_buffer_size(channel);
      size -= sizeof(struct net_rpmsg_transfer_s);
      if (dev->d_pktsize > size)
        {
          dev->d_pktsize = size;
        }

      priv = dev->d_private;
      priv->channel = channel;
      nxsem_post(&priv->channelready);
    }
}

static void net_rpmsg_drv_channel_destroyed(struct rpmsg_channel *channel)
{
  struct net_driver_s *dev = rpmsg_get_privdata(channel);
  struct net_rpmsg_drv_s *priv;

  if (dev != NULL)
    {
      priv = dev->d_private;
      net_rpmsg_drv_wait(&priv->channelready);
      dev->d_buf    = NULL;
      priv->channel = NULL;
    }
}

static void net_rpmsg_drv_channel_received(struct rpmsg_channel *channel,
                    void *data, int len, void *priv, unsigned long src)
{
  struct net_rpmsg_header_s *header = data;
  uint32_t command = header->command;

  if (command < sizeof(g_net_rpmsg_drv_handler) / sizeof(g_net_rpmsg_drv_handler[0]))
    {
      g_net_rpmsg_drv_handler[command](channel, data, len, priv, src);
    }
}

static int net_rpmsg_drv_send_recv(struct net_driver_s *dev,
                    void *header_, uint32_t command, int len)
{
  struct net_rpmsg_drv_s *priv = dev->d_private;
  struct net_rpmsg_header_s *header = header_;
  struct net_rpmsg_drv_cookie_s cookie;
  int ret;

  nxsem_init(&cookie.sem, 0, 0);
  nxsem_setprotocol(&cookie.sem, SEM_PRIO_NONE);

  cookie.header   = header;
  header->command = command;
  header->result  = -ENXIO;
  header->cookie  = (uintptr_t)&cookie;

  /* Is the channel ready? */

  if (priv->channel == NULL)
    {
      /* No, wait until the channel ready */

      net_rpmsg_drv_wait(&priv->channelready);

      /* Repost to keep the semaphore still ready */

      nxsem_post(&priv->channelready);
    }

  ret = rpmsg_send(priv->channel, header, len);
  if (ret < 0)
    {
      goto out;
    }

  net_rpmsg_drv_wait(&cookie.sem);
  ret = cookie.header->result;

out:
  nxsem_destroy(&cookie.sem);
  return ret;
}

/****************************************************************************
 * Name: net_rpmsg_drv_poll_work
 *
 * Description:
 *   Perform periodic polling from the worker thread
 *
 * Parameters:
 *   arg - The argument passed when work_queue() as called.
 *
 * Returned Value:
 *   OK on success
 *
 * Assumptions:
 *   Run on a work queue thread.
 *
 ****************************************************************************/

static void net_rpmsg_drv_poll_work(FAR void *arg)
{
  FAR struct net_driver_s *dev = arg;
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;
  uint32_t size;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Perform the poll */

  /* Check if there is room in the send another TX packet.  We cannot perform
   * the TX poll if he are unable to accept another packet for transmission.
   */

  if (dev->d_buf == NULL)
    {
      /* Try to get the payload buffer if not yet */

      dev->d_buf = rpmsg_get_tx_payload_buffer(priv->channel, &size, false);
      if (dev->d_buf)
        {
          dev->d_buf += sizeof(struct net_rpmsg_transfer_s);
        }
    }

  if (dev->d_buf)
    {
      /* If so, update TCP timing states and poll the network for new XMIT data.
       * Hmmm.. might be bug here.  Does this mean if there is a transmit in
       * progress, we will missing TCP time state updates?
       */

      devif_timer(dev, net_rpmsg_drv_txpoll);
    }

  /* Setup the watchdog poll timer again */

  wd_start(priv->txpoll, NET_RPMSG_DRV_WDDELAY, net_rpmsg_drv_poll_expiry, 1,
           (wdparm_t)dev);
  net_unlock();
}

/****************************************************************************
 * Name: net_rpmsg_drv_poll_expiry
 *
 * Description:
 *   Periodic timer handler.  Called from the timer interrupt handler.
 *
 * Parameters:
 *   argc - The number of available arguments
 *   arg  - The first argument
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs in the context of a the timer interrupt handler.  Local
 *   interrupts are disabled by the interrupt logic.
 *
 ****************************************************************************/

static void net_rpmsg_drv_poll_expiry(int argc, wdparm_t arg, ...)
{
  FAR struct net_driver_s *dev = (FAR struct net_driver_s *)arg;
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;

  /* Schedule to perform the interrupt processing on the worker thread. */

  work_queue(NET_RPMSG_DRV_WORK, &priv->pollwork, net_rpmsg_drv_poll_work, dev, 0);
}

/****************************************************************************
 * Name: net_rpmsg_drv_ifup
 *
 * Description:
 *   NuttX Callback: Bring up the link interface when an IP address is
 *   provided
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int net_rpmsg_drv_ifup(FAR struct net_driver_s *dev)
{
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;
  struct net_rpmsg_ifup_s msg = {};
  int ret;

#ifdef CONFIG_NET_IPv4
  ninfo("Bringing up: %d.%d.%d.%d\n",
        dev->d_ipaddr & 0xff, (dev->d_ipaddr >> 8) & 0xff,
        (dev->d_ipaddr >> 16) & 0xff, dev->d_ipaddr >> 24);
#endif
#ifdef CONFIG_NET_IPv6
  ninfo("Bringing up: %04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x\n",
        dev->d_ipv6addr[0], dev->d_ipv6addr[1], dev->d_ipv6addr[2],
        dev->d_ipv6addr[3], dev->d_ipv6addr[4], dev->d_ipv6addr[5],
        dev->d_ipv6addr[6], dev->d_ipv6addr[7]);
#endif

  net_lock();

  /* Prepare the message */

  msg.lnkaddr.length = netdev_lladdrsize(dev);
  memcpy(msg.lnkaddr.addr, &dev->d_mac, msg.lnkaddr.length);

#ifdef CONFIG_NET_IPv4
  net_ipv4addr_copy(msg.ipaddr, dev->d_ipaddr);
  net_ipv4addr_copy(msg.draddr, dev->d_draddr);
  net_ipv4addr_copy(msg.netmask, dev->d_netmask);
#endif

#ifdef CONFIG_NET_IPv6
  net_ipv6addr_copy(msg.ipv6addr, dev->d_ipv6addr);
  net_ipv6addr_copy(msg.ipv6draddr, dev->d_ipv6draddr);
  net_ipv6addr_copy(msg.ipv6netmask, dev->d_ipv6netmask);
#endif

  /* Send the message */

  ret = net_rpmsg_drv_send_recv(dev, &msg, NET_RPMSG_IFUP, sizeof(msg));
  if (ret < 0)
    {
      goto out;
    }

  /* Update net_driver_t field */

  memcpy(&dev->d_mac, msg.lnkaddr.addr, msg.lnkaddr.length);

#ifdef CONFIG_NET_IPv4
  net_ipv4addr_copy(dev->d_ipaddr, msg.ipaddr);
  net_ipv4addr_copy(dev->d_draddr, msg.draddr);
  net_ipv4addr_copy(dev->d_netmask, msg.netmask);
#endif

#ifdef CONFIG_NET_IPv6
  net_ipv6addr_copy(dev->d_ipv6addr, msg.ipv6addr);
  net_ipv6addr_copy(dev->d_ipv6draddr, msg.ipv6draddr);
  net_ipv6addr_copy(dev->d_ipv6netmask, msg.ipv6netmask);
#endif

#ifdef CONFIG_NET_ICMPv6
  /* Set up IPv6 multicast address filtering */

  net_rpmsg_drv_ipv6multicast(dev);
#endif

#ifdef CONFIG_NETDB_DNSCLIENT
#  ifdef CONFIG_NET_IPv4
  if (net_ipv4addr_cmp(msg.dnsaddr, INADDR_ANY))
    {
      struct sockaddr_in dnsaddr = {};

      dnsaddr.sin_family = AF_INET;
      dnsaddr.sin_port   = htons(DNS_DEFAULT_PORT);
      memcpy(&dnsaddr.sin_addr, &msg.dnsaddr, sizeof(msg.dnsaddr));

      dns_add_nameserver((FAR const struct sockaddr *)&dnsaddr, sizeof(dnsaddr));
    }
#  endif
#  ifdef CONFIG_NET_IPv6
  if (net_ipv6addr_cmp(msg.ipv6dnsaddr, &in6addr_any))
    {
      struct sockaddr_in6 dnsaddr = {};

      dnsaddr.sin6_family = AF_INET6;
      dnsaddr.sin6_port   = htons(DNS_DEFAULT_PORT);
      memcpy(&dnsaddr.sin6_addr, msg.ipv6dnsaddr, sizeof(msg.ipv6dnsaddr));

      dns_add_nameserver((FAR const struct sockaddr *)&dnsaddr, sizeof(dnsaddr));
    }
#  endif
#endif

  /* Set and activate a timer process */

  wd_start(priv->txpoll, NET_RPMSG_DRV_WDDELAY, net_rpmsg_drv_poll_expiry, 1,
           (wdparm_t)dev);
out:
  net_unlock();
  return ret;
}

/****************************************************************************
 * Name: net_rpmsg_drv_ifdown
 *
 * Description:
 *   NuttX Callback: Stop the interface.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int net_rpmsg_drv_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;
  struct net_rpmsg_ifdown_s msg;
  irqstate_t flags;

  /* Disable the interrupt */

  flags = enter_critical_section();

  /* Cancel the TX poll timer and work */

  wd_cancel(priv->txpoll);
  work_cancel(NET_RPMSG_DRV_WORK, &priv->pollwork);

  leave_critical_section(flags);

  /* Put the EMAC in its reset, non-operational state.  This should be
   * a known configuration that will guarantee the net_rpmsg_drv_ifup() always
   * successfully brings the interface back up.
   */

  return net_rpmsg_drv_send_recv(dev, &msg, NET_RPMSG_IFDOWN, sizeof(msg));
}

/****************************************************************************
 * Name: net_rpmsg_drv_txavail_work
 *
 * Description:
 *   Perform an out-of-cycle poll on the worker thread.
 *
 * Parameters:
 *   arg - Reference to the NuttX driver state structure (cast to void*)
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Runs on a work queue thread.
 *
 ****************************************************************************/

static void net_rpmsg_drv_txavail_work(FAR void *arg)
{
  FAR struct net_driver_s *dev = arg;
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;
  uint32_t size;

  /* Lock the network and serialize driver operations if necessary.
   * NOTE: Serialization is only required in the case where the driver work
   * is performed on an LP worker thread and where more than one LP worker
   * thread has been configured.
   */

  net_lock();

  /* Ignore the notification if the interface is not yet up */

  if (IFF_IS_UP(dev->d_flags))
    {
      /* Try to get the payload buffer if not yet */

      if (dev->d_buf == NULL)
        {

          dev->d_buf = rpmsg_get_tx_payload_buffer(priv->channel, &size, false);
          if (dev->d_buf)
            {
              dev->d_buf += sizeof(struct net_rpmsg_transfer_s);
            }
        }

      /* Check if there is room in the hardware to hold another outgoing packet. */

      if (dev->d_buf)
        {
          /* If so, then poll the network for new XMIT data */

          devif_poll(dev, net_rpmsg_drv_txpoll);
        }
    }

  net_unlock();
}

/****************************************************************************
 * Name: net_rpmsg_drv_txavail
 *
 * Description:
 *   Driver callback invoked when new TX data is available.  This is a
 *   stimulus perform an out-of-cycle poll and, thereby, reduce the TX
 *   latency.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

static int net_rpmsg_drv_txavail(FAR struct net_driver_s *dev)
{
  FAR struct net_rpmsg_drv_s *priv = dev->d_private;

  /* Is our single work structure available?  It may not be if there are
   * pending interrupt actions and we will have to ignore the Tx
   * availability action.
   */

  if (work_available(&priv->pollwork))
    {
      /* Schedule to serialize the poll on the worker thread. */

      work_queue(NET_RPMSG_DRV_WORK, &priv->pollwork, net_rpmsg_drv_txavail_work, dev, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: net_rpmsg_drv_addmac
 *
 * Description:
 *   NuttX Callback: Add the specified MAC address to the hardware multicast
 *   address filtering
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be added
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#if defined(CONFIG_NET_IGMP) || defined(CONFIG_NET_ICMPv6)
static int net_rpmsg_drv_addmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  struct net_rpmsg_mcast_s msg;

  /* Add the MAC address to the hardware multicast routing table */

  msg.lnkaddr.length = netdev_lladdrsize(dev);
  memcpy(msg.lnkaddr.addr, mac, msg.lnkaddr.length);
  return net_rpmsg_drv_send_recv(dev, &msg, NET_RPMSG_ADDMCAST, sizeof(msg));
}
#endif

/****************************************************************************
 * Name: net_rpmsg_drv_rmmac
 *
 * Description:
 *   NuttX Callback: Remove the specified MAC address from the hardware multicast
 *   address filtering
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *   mac  - The MAC address to be removed
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_IGMP
static int net_rpmsg_drv_rmmac(FAR struct net_driver_s *dev, FAR const uint8_t *mac)
{
  struct net_rpmsg_mcast_s msg;

  /* Remove the MAC address from the hardware multicast routing table */

  msg.lnkaddr.length = netdev_lladdrsize(dev);
  memcpy(msg.lnkaddr.addr, mac, msg.lnkaddr.length);
  return net_rpmsg_drv_send_recv(dev, &msg, NET_RPMSG_RMMCAST, sizeof(msg));
}
#endif

/****************************************************************************
 * Name: net_rpmsg_drv_ipv6multicast
 *
 * Description:
 *   Configure the IPv6 multicast MAC address.
 *
 * Parameters:
 *   dev  - Reference to the NuttX driver state structure
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_NET_ICMPv6
static void net_rpmsg_drv_ipv6multicast(FAR struct net_driver_s *dev)
{
  if (dev->d_lltype == NET_LL_ETHERNET || dev->d_lltype == NET_LL_IEEE80211)
    {
      uint16_t tmp16;
      uint8_t mac[6];

      /* For ICMPv6, we need to add the IPv6 multicast address
       *
       * For IPv6 multicast addresses, the Ethernet MAC is derived by
       * the four low-order octets OR'ed with the MAC 33:33:00:00:00:00,
       * so for example the IPv6 address FF02:DEAD:BEEF::1:3 would map
       * to the Ethernet MAC address 33:33:00:01:00:03.
       *
       * NOTES:  This appears correct for the ICMPv6 Router Solicitation
       * Message, but the ICMPv6 Neighbor Solicitation message seems to
       * use 33:33:ff:01:00:03.
       */

      mac[0] = 0x33;
      mac[1] = 0x33;

      tmp16  = dev->d_ipv6addr[6];
      mac[2] = 0xff;
      mac[3] = tmp16 >> 8;

      tmp16  = dev->d_ipv6addr[7];
      mac[4] = tmp16 & 0xff;
      mac[5] = tmp16 >> 8;

      ninfo("IPv6 Multicast: %02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

      net_rpmsg_drv_addmac(dev, mac);

#if defined(CONFIG_NET_ETHERNET) && defined(CONFIG_NET_ICMPv6_AUTOCONF)
      /* Add the IPv6 all link-local nodes Ethernet address.  This is the
       * address that we expect to receive ICMPv6 Router Advertisement
       * packets.
       */

      net_rpmsg_drv_addmac(dev, g_ipv6_ethallnodes.ether_addr_octet);
#endif /* CONFIG_NET_ETHERNET && CONFIG_NET_ICMPv6_AUTOCONF */

#if defined(CONFIG_NET_ETHERNET) && defined(CONFIG_NET_ICMPv6_ROUTER)
      /* Add the IPv6 all link-local routers Ethernet address.  This is the
       * address that we expect to receive ICMPv6 Router Solicitation
       * packets.
       */

      net_rpmsg_drv_addmac(dev, g_ipv6_ethallrouters.ether_addr_octet);
#endif /* CONFIG_NET_ETHERNET && CONFIG_NET_ICMPv6_ROUTER */
    }
}
#endif /* CONFIG_NET_ICMPv6 */

/****************************************************************************
 * Name: net_rpmsg_drv_ioctl
 *
 * Description:
 *   Handle network IOCTL commands directed to this device.
 *
 * Parameters:
 *   dev - Reference to the NuttX driver state structure
 *   cmd - The IOCTL command
 *   arg - The argument for the IOCTL command
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *   The network is locked.
 *
 ****************************************************************************/

#ifdef CONFIG_NETDEV_IOCTL
static int net_rpmsg_drv_ioctl(FAR struct net_driver_s *dev, int cmd,
                               unsigned long arg)
{
  ssize_t len;
  int ret;

  len = net_ioctl_arglen(cmd);
  if (len >= 0)
    {
      FAR struct net_rpmsg_ioctl_s *msg;
      char buf[sizeof(*msg) + len];

      msg = (FAR struct net_rpmsg_ioctl_s *)buf;

      msg->code   = cmd;
      msg->length = len;
      memcpy(msg->arg, (FAR void *)arg, len);

      ret = net_rpmsg_drv_send_recv(dev, msg,
              NET_RPMSG_DEVIOCTL, sizeof(*msg) + len);
      if (ret >= 0)
        {
          memcpy((FAR void *)arg, msg->arg, len);
        }
    }
  else
    {
      ret = len;
    }

  return ret;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: net_rpmsg_drv_init
 *
 * Description:
 *   Initialize the net rpmsg driver
 *
 * Parameters:
 *   name - Specify the netdev name
     lltype - Identify the link type
 *
 * Returned Value:
 *   OK on success; Negated errno on failure.
 *
 * Assumptions:
 *   Called early in initialization before multi-tasking is initiated.
 *
 ****************************************************************************/

int net_rpmsg_drv_init(FAR const char *cpuname,
                       FAR const char *devname,
                       enum net_lltype_e lltype)
{
  FAR struct net_rpmsg_drv_s *priv;
  FAR struct net_driver_s *dev;

  /* Allocate the interface structure */

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }
  dev = &priv->dev;

  priv->cpuname = cpuname;
  sprintf(priv->channelname, NET_RPMSG_CHANNEL_NAME, devname);

  nxsem_init(&priv->channelready, 0, 0);
  nxsem_setprotocol(&priv->channelready, SEM_PRIO_NONE);

  /* Initialize the driver structure */

  strcpy(dev->d_ifname, devname);
  dev->d_ifup    = net_rpmsg_drv_ifup;    /* I/F up (new IP address) callback */
  dev->d_ifdown  = net_rpmsg_drv_ifdown;  /* I/F down callback */
  dev->d_txavail = net_rpmsg_drv_txavail; /* New TX data callback */
#ifdef CONFIG_NET_IGMP
  dev->d_addmac  = net_rpmsg_drv_addmac;  /* Add multicast MAC address */
  dev->d_rmmac   = net_rpmsg_drv_rmmac;   /* Remove multicast MAC address */
#endif
#ifdef CONFIG_NETDEV_IOCTL
  dev->d_ioctl   = net_rpmsg_drv_ioctl;   /* Handle network IOCTL commands */
#endif
  dev->d_private = priv;                  /* Used to recover private state from dev */

  /* Create a watchdog for timing polling for transmissions */

  priv->txpoll   = wd_create();           /* Create periodic poll timer */
  DEBUGASSERT(priv->txpoll != NULL);

  /* Register the device with the openamp */

  rpmsg_register_callback(priv->channelname, dev,
    net_rpmsg_drv_device_created, NULL, net_rpmsg_drv_channel_created,
    net_rpmsg_drv_channel_destroyed, net_rpmsg_drv_channel_received);

  /* Register the device with the OS so that socket IOCTLs can be performed */

  netdev_register(dev, lltype);
  return OK;
}
