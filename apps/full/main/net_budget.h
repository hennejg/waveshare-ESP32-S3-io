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
/* A client arriving with every slot taken is accepted and closed at once, so
   that it sees a clean close rather than a connection that never answers. For
   that instant it holds a socket of its own, over and above the slots. */
#define NET_SOCK_MB_REFUSE       1
#define NET_SOCK_MB_MASTER       3   /* kept open to the devices this board polls */

/* The WiFi provisioning portal (esp32-wifi-bootstrap, wifi_config.c): its own
   HTTP listener, the one client it serves at a time, and the captive DNS
   responder. It runs while the access point is up, and Ethernet -- with
   everything above -- can be up at the same time. */
#define NET_SOCK_PORTAL          3

/* Listen backlogs. A connection lwIP has completed but the application has
   not yet accepted holds no socket -- that is taken by accept() -- but it
   does hold a TCP PCB, and those are limited too (below). lwIP queues at most
   CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE of them per listener whatever the backlog
   says; the rest are aborted. */
#define NET_BACKLOG_MB          16   /* listen() in mb_tcp_server.c          */
#define NET_BACKLOG_HTTPD        5   /* HTTPD_DEFAULT_CONFIG's backlog_conn  */
#define NET_BACKLOG_PORTAL       4   /* listen() in wifi_config.c            */
#define NET_PENDING(b) ((b) < CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE ? (b) : CONFIG_LWIP_TCP_ACCEPTMBOX_SIZE)

/* TCP connections, as opposed to sockets: lwIP refuses a PCB past
   CONFIG_LWIP_MAX_ACTIVE_TCP, and before refusing it kills the oldest
   connection of lower priority -- an established Modbus or MQTT connection
   dropped to make room for a new one. Listening PCBs are counted apart, and
   one in TIME_WAIT is reclaimed first, so neither is in this sum. */
#define NET_TCP_ACTIVE (NET_SOCK_HTTPD_CLIENTS + NET_SOCK_MQTT +                  \
                        NET_SOCK_MB_CONN + NET_SOCK_MB_REFUSE + NET_SOCK_MB_MASTER + \
                        1 /* the portal's one client */ +                          \
                        NET_PENDING(NET_BACKLOG_MB) + NET_PENDING(NET_BACKLOG_HTTPD) + \
                        NET_PENDING(NET_BACKLOG_PORTAL))

_Static_assert(NET_TCP_ACTIVE <= CONFIG_LWIP_MAX_ACTIVE_TCP,
               "net_budget.h hands out more TCP connections than CONFIG_LWIP_MAX_ACTIVE_TCP has");

_Static_assert(NET_SOCK_HTTPD + NET_SOCK_MQTT + NET_SOCK_SNTP +
               NET_SOCK_MB_LISTEN + NET_SOCK_MB_CONN + NET_SOCK_MB_REFUSE +
               NET_SOCK_MB_MASTER + NET_SOCK_PORTAL
               <= CONFIG_LWIP_MAX_SOCKETS,
               "net_budget.h hands out more sockets than CONFIG_LWIP_MAX_SOCKETS has");
