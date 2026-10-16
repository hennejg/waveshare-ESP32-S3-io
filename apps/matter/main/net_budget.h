#pragma once

/* How the Matter firmware's lwIP sockets are divided up -- the same idea as
 * apps/full/main/net_budget.h: CONFIG_LWIP_MAX_SOCKETS is one number for the
 * whole firmware, and running past it fails as accept() returning -1. Matter
 * itself talks over lwIP's raw API and holds no sockets. */

#include "sdkconfig.h"

/* esp_http_server reserves three sockets on top of the clients it serves. */
#define NET_SOCK_HTTPD_CLIENTS   7
#define NET_SOCK_HTTPD          (NET_SOCK_HTTPD_CLIENTS + 3)

/* The setup portal (esp32-wifi-bootstrap): listener, one client, DNS. */
#define NET_SOCK_PORTAL          3

/* Modbus TCP (components/modbus): listener, the clients, one refused. */
#define NET_SOCK_MB              (1 + CONFIG_MB_TCP_MAX_CONN + 1)

#define NET_PENDING(b) ((b) < CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE ? (b) : CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE)
#define NET_TCP_ACTIVE (NET_SOCK_HTTPD_CLIENTS + CONFIG_MB_TCP_MAX_CONN + 1 + 1 /* portal client */ + \
                        NET_PENDING(CONFIG_MB_TCP_BACKLOG) + NET_PENDING(5 /* httpd backlog */) +  \
                        NET_PENDING(4 /* portal backlog */))

_Static_assert(NET_SOCK_HTTPD + NET_SOCK_PORTAL + NET_SOCK_MB <= CONFIG_LWIP_MAX_SOCKETS,
               "net_budget.h hands out more sockets than CONFIG_LWIP_MAX_SOCKETS has");
_Static_assert(NET_TCP_ACTIVE <= CONFIG_LWIP_MAX_ACTIVE_TCP,
               "net_budget.h hands out more TCP connections than CONFIG_LWIP_MAX_ACTIVE_TCP has");
