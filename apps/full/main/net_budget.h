#pragma once

/* How the board's lwIP sockets are divided up.
 *
 * CONFIG_LWIP_MAX_SOCKETS is one number for the whole firmware, and running
 * past it fails as accept() returning -1 and the web server quietly going
 * dark -- the worst shape to find on a customer's site. The shares used to
 * live in three comments in three files that could not see each other, so
 * nothing noticed when one of them grew. Here they add up in one place and
 * the compiler checks the sum.
 *
 * Each figure is how many sockets that subsystem may hold at the same time.
 * Raising one means lowering another or raising CONFIG_LWIP_MAX_SOCKETS. */

#include "sdkconfig.h"

/* esp_http_server reserves three sockets for its own working on top of the
   clients it will serve -- see max_open_sockets in esp_http_server.h. The
   client figure is set explicitly rather than left to HTTPD_DEFAULT_CONFIG,
   so that this budget is what actually applies. */
#define NET_SOCK_HTTPD_CLIENTS   7
#define NET_SOCK_HTTPD          (NET_SOCK_HTTPD_CLIENTS + 3)

#define NET_SOCK_MQTT            1
#define NET_SOCK_SNTP            1

#define NET_SOCK_MB_LISTEN       1   /* the Modbus TCP listening socket      */
#define NET_SOCK_MB_CONN         8   /* Modbus TCP clients served at once    */
#define NET_SOCK_MB_MASTER       3   /* kept open to the devices this board polls */

_Static_assert(NET_SOCK_HTTPD + NET_SOCK_MQTT + NET_SOCK_SNTP +
               NET_SOCK_MB_LISTEN + NET_SOCK_MB_CONN + NET_SOCK_MB_MASTER
               <= CONFIG_LWIP_MAX_SOCKETS,
               "net_budget.h hands out more sockets than CONFIG_LWIP_MAX_SOCKETS has");
