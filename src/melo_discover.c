/*
 * melo_discover.c: A Melo device discoverer
 *
 * Copyright (C) 2016 Alexandre Dilly <dillya@sparod.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <string.h>
#include <sys/types.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/rtnetlink.h>

#include <libsoup/soup.h>

#include "melo_discover.h"

#define MELO_DISCOVER_BUFFER_SIZE 4096
#define MELO_DISCOVER_URL "http://www.sparod.com/melo/discover.php"

struct _MeloDiscoverPrivate {
  GMutex mutex;
  gboolean registered;
  SoupSession *session;
  guint netlink_id;
  int netlink_fd;
  gchar *serial;
  GList *ifaces;
};

typedef struct {
  gchar *name;
  gchar *hw_address;
  gchar *address;
} MeloDiscoverInterface;

static void melo_discover_interface_free (MeloDiscoverInterface *iface);
static gboolean melo_netlink_event (gint fd, GIOCondition condition,
                                    gpointer user_data);

G_DEFINE_TYPE_WITH_PRIVATE (MeloDiscover, melo_discover, G_TYPE_OBJECT)

static void
melo_discover_finalize (GObject *gobject)
{
  MeloDiscover *disco = MELO_DISCOVER (gobject);
  MeloDiscoverPrivate *priv = melo_discover_get_instance_private (disco);

  /* Remove netlink source event */
  if (priv->netlink_id)
    g_source_remove (priv->netlink_id);

  /* Close netlink socket */
  if (priv->netlink_fd > 0)
    close (priv->netlink_fd);

  /* Free Soup session */
  g_object_unref (priv->session);

  /* Free serial */
  g_free (priv->serial);

  /* Free interfaces list */
  g_list_free_full (priv->ifaces,
                    (GDestroyNotify) melo_discover_interface_free);

  /* Clear mutex */
  g_mutex_clear (&priv->mutex);

  /* Chain up to the parent class */
  G_OBJECT_CLASS (melo_discover_parent_class)->finalize (gobject);
}

static void
melo_discover_class_init (MeloDiscoverClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  /* Add custom finalize() function */
  object_class->finalize = melo_discover_finalize;
}

static void
melo_discover_init (MeloDiscover *self)
{
  MeloDiscoverPrivate *priv = melo_discover_get_instance_private (self);

  self->priv = priv;

  /* Initialize mutex */
  g_mutex_init (&priv->mutex);

  /* Create a new Soup session */
  priv->session = soup_session_new_with_options (
                                SOUP_SESSION_USER_AGENT, "Melo",
                                NULL);


  /* Open netlink socket */
  priv->netlink_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (priv->netlink_fd > 0) {
    struct sockaddr_nl sock_addr;

    /* Set netlink socket to monitor interfaces and their addresses */
    memset(&sock_addr, 0, sizeof(sock_addr));
    sock_addr.nl_family = AF_NETLINK;
    sock_addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
    bind(priv->netlink_fd, (struct sockaddr *) &sock_addr, sizeof(sock_addr));

    /* Add netlink socket source event */
    priv->netlink_id = g_unix_fd_add (priv->netlink_fd, G_IO_IN,
                                      melo_netlink_event, self);
  }
}

MeloDiscover *
melo_discover_new ()
{
  return g_object_new (MELO_TYPE_DISCOVER, NULL);
}

static gchar *
melo_discover_get_hw_address (unsigned char *addr)
{
  return g_strdup_printf ("%02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1],
                          addr[2], addr[3], addr[4], addr[5]);
}

static gchar *
melo_discover_get_address (struct in_addr *addr)
{
  gchar *address;

  /* Get address string */
  address = g_malloc (INET_ADDRSTRLEN);
  if (address)
    inet_ntop(AF_INET, addr, address, INET_ADDRSTRLEN);

  return address;
}

static gchar *
melo_discover_get_serial (struct ifaddrs *ifap)
{
  struct ifaddrs *i;

  /* Get first hardware address for serial */
  for (i = ifap; i != NULL; i = i->ifa_next) {
    if (i && i->ifa_addr->sa_family == AF_PACKET &&
        !(i->ifa_flags & IFF_LOOPBACK)) {
      struct sockaddr_ll *s = (struct sockaddr_ll*) i->ifa_addr;
      return melo_discover_get_hw_address (s->sll_addr);
    }
  }

  return NULL;
}

static MeloDiscoverInterface *
melo_discover_interface_get (MeloDiscover *disco, const gchar *name)
{
  MeloDiscoverPrivate *priv = disco->priv;
  MeloDiscoverInterface *iface;
  GList *l;

  /* Find interface in list */
  for (l = priv->ifaces; l != NULL; l = l->next) {
    iface = l->data;
    if (!g_strcmp0 (iface->name, name))
      return iface;
  }

  /* Create a new item */
  iface = g_slice_new0 (MeloDiscoverInterface);
  if (iface) {
    iface->name = g_strdup (name);
    priv->ifaces = g_list_prepend (priv->ifaces, iface);
  }

  return iface;
}

static void
melo_discover_interface_free (MeloDiscoverInterface *iface)
{
  g_free (iface->name);
  g_free (iface->hw_address);
  g_free (iface->address);
  g_slice_free (MeloDiscoverInterface, iface);
}

static gboolean
melo_discover_add_address (MeloDiscover *disco, MeloDiscoverInterface *iface)
{
  MeloDiscoverPrivate *priv = disco->priv;
  SoupMessage *msg;
  gchar *req;

  /* No serial */
  if (!priv->serial)
    return FALSE;

  /* Prepare request for address registration */
  req = g_strdup_printf (MELO_DISCOVER_URL "?action=add_address&"
                         "serial=%s&hw_address=%s&address=%s",
                         priv->serial, iface->hw_address, iface->address);

  /* Send request */
  msg = soup_message_new ("GET", req);
  soup_session_queue_message (priv->session, msg, NULL, NULL);
  g_free (req);

  return TRUE;
}

static gboolean
melo_discover_remove_address (MeloDiscover *disco, MeloDiscoverInterface *iface)
{
  MeloDiscoverPrivate *priv = disco->priv;
  SoupMessage *msg;
  gchar *req;

  /* No serial */
  if (!priv->serial)
    return FALSE;

  /* Prepare request for address removal */
  req = g_strdup_printf (MELO_DISCOVER_URL "?action=remove_address&"
                         "serial=%s&hw_address=%s",
                         priv->serial, iface->hw_address);

  /* Send request */
  msg = soup_message_new ("GET", req);
  soup_session_queue_message (priv->session, msg, NULL, NULL);
  g_free (req);

  return TRUE;
}

static gboolean
melo_netlink_event (gint fd, GIOCondition condition, gpointer user_data)
{
  MeloDiscover *disco = user_data;
  MeloDiscoverPrivate *priv = disco->priv;
  char buffer[MELO_DISCOVER_BUFFER_SIZE];
  struct nlmsghdr *nh;
  ssize_t len;

  /* Get next message from netlink socket */
  len = recv (fd, buffer, MELO_DISCOVER_BUFFER_SIZE, 0);
  if (len <= 0)
    return FALSE;

  /* Lock interface list access */
  g_mutex_lock (&priv->mutex);

  /* Parse messages */
  for (nh = (struct nlmsghdr *) buffer; NLMSG_OK (nh, len);
       nh = NLMSG_NEXT (nh, len)) {
    static MeloDiscoverInterface *iface;
    char name[IF_NAMESIZE];
    struct rtattr *ra;
    int rlen;

    /* Process message */
    switch (nh->nlmsg_type) {
      case RTM_NEWLINK: {
        struct ifinfomsg *msg = (struct ifinfomsg *) NLMSG_DATA (nh);

        /* Get interface */
        if_indextoname (msg->ifi_index, name);
        iface = melo_discover_interface_get (disco, name);

        /* Update interface */
        if (iface) {
          /* Extract hardware address */
          ra = IFLA_RTA (msg);
          rlen = IFLA_PAYLOAD (nh);
          for (; rlen && RTA_OK (ra, rlen); ra = RTA_NEXT (ra,rlen)) {
            if (ra->rta_type != IFLA_ADDRESS)
              continue;

            /* Set hardware address */
            g_free (iface->hw_address);
            iface->hw_address = melo_discover_get_hw_address (
                                               (unsigned char *) RTA_DATA (ra));
          }
        }
        break;
      }
      case RTM_DELLINK:
        break;
      case RTM_NEWADDR: {
        struct ifaddrmsg *msg = (struct ifaddrmsg *) NLMSG_DATA (nh);
        struct in_addr addr;

        /* Get interface */
        if_indextoname (msg->ifa_index, name);
        iface = melo_discover_interface_get (disco, name);

        /* Update IP address */
        if (iface) {
          /* Extract local address */
          ra = IFA_RTA (msg);
          rlen = IFA_PAYLOAD (nh);
          for (; rlen && RTA_OK (ra, rlen); ra = RTA_NEXT (ra,rlen)) {
            if (ra->rta_type != IFA_LOCAL)
              continue;

            /* Get address */
            addr.s_addr = *((uint32_t *) RTA_DATA (ra));

            /* Set address */
            g_free (iface->address);
            iface->address = melo_discover_get_address (&addr);
            if (priv->registered && iface->hw_address)
              melo_discover_add_address (disco, iface);
          }
        }
        break;
      }
      case RTM_DELADDR: {
        struct ifaddrmsg *msg = (struct ifaddrmsg *) NLMSG_DATA (nh);

        /* Get interface */
        if_indextoname (msg->ifa_index, name);
        iface = melo_discover_interface_get (disco, name);

        /* Remove IP address */
        if (iface) {
          g_free (iface->address);
          iface->address = NULL;
          if (priv->registered && iface->hw_address)
            melo_discover_remove_address (disco, iface);
        }
        break;
      }
      case NLMSG_DONE:
      case NLMSG_ERROR:
        goto end;
      default:
        break;
    }
  }

end:
  /* Unock interface list access */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}

static void
melo_device_register_callback (SoupSession *session, SoupMessage *msg,
                               gpointer user_data)
{
  MeloDiscoverPrivate *priv = user_data;

  /* Lock interface list access */
  g_mutex_lock (&priv->mutex);

  /* Device is now registered */
  priv->registered = TRUE;

  /* Unlock interface list access */
  g_mutex_unlock (&priv->mutex);
}

gboolean
melo_discover_register_device (MeloDiscover *disco, const gchar *name,
                               guint port)
{
  MeloDiscoverPrivate *priv = disco->priv;
  MeloDiscoverInterface *iface;
  struct ifaddrs *ifap, *i;
  const gchar *host;
  SoupMessage *msg;
  gchar *req;
  GList *l;

  /* Get network interfaces */
  if (getifaddrs (&ifap))
    return FALSE;

  /* Lock interface list access */
  g_mutex_lock (&priv->mutex);

  /* Get serial */
  if (!priv->serial)
    priv->serial = melo_discover_get_serial (ifap);

  /* Get hostname */
  host = g_get_host_name ();

  /* Prepare request for device registration */
  req = g_strdup_printf (MELO_DISCOVER_URL "?action=add_device&"
                         "serial=%s&name=%s&hostname=%s&port=%u",
                         priv->serial, name, host, port);

  /* Register device on Melo website */
  msg = soup_message_new ("GET", req);
  soup_session_send_message (priv->session, msg);
  soup_session_queue_message (priv->session, msg,
                              melo_device_register_callback, priv);
  g_free (req);

  /* List all interfaces */
  for (i = ifap; i != NULL; i = i->ifa_next) {
    /* Skip loopback interface */
    if (i->ifa_flags & IFF_LOOPBACK || !i->ifa_addr)
      continue;

    /* Get addresses */
    if (i->ifa_addr->sa_family == AF_PACKET) {
      struct sockaddr_ll *s = (struct sockaddr_ll *) i->ifa_addr;

      /* Find interface in list */
      iface = melo_discover_interface_get (disco, i->ifa_name);
      if (!iface)
        continue;

      /* Get hardware address */
      g_free (iface->hw_address);
      iface->hw_address = melo_discover_get_hw_address (s->sll_addr);
    } else if (i->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in *s = (struct sockaddr_in *) i->ifa_addr;

      /* Find interface in list */
      iface = melo_discover_interface_get (disco, i->ifa_name);
      if (!iface)
        continue;

      /* Get address */
      g_free (iface->address);
      iface->address = melo_discover_get_address (&s->sin_addr);
    }
  }

  /* Add device addresses on Sparod */
  for (l = priv->ifaces; l != NULL; l = l->next) {
    /* Get interface */
    iface = l->data;

    /* Add or remove device address on Melo website */
    if (iface->hw_address) {
      if (iface->address)
        melo_discover_add_address (disco, iface);
      else
        melo_discover_remove_address (disco, iface);
    }
  }

  /* Unock interface list access */
  g_mutex_unlock (&priv->mutex);

  /* Free intarfaces list */
  freeifaddrs (ifap);

  return TRUE;
}

gboolean
melo_discover_unregister_device (MeloDiscover *disco)
{
  MeloDiscoverPrivate *priv = disco->priv;
  SoupMessage *msg;
  gchar *req;

  /* Lock interface list access */
  g_mutex_lock (&priv->mutex);

  /* No serial found */
  if (!priv->serial) {
    g_mutex_unlock (&priv->mutex);
    return FALSE;
  }

  /* Device is not registered */
  priv->registered = FALSE;

  /* Prepare request for device removal */
  req = g_strdup_printf (MELO_DISCOVER_URL "?action=remove_device&serial=%s",
                         priv->serial);

  /* Unregister device from Melo website */
  msg = soup_message_new ("GET", req);
  soup_session_queue_message (priv->session, msg, NULL, NULL);
  g_free (req);

  /* Unock interface list access */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}
