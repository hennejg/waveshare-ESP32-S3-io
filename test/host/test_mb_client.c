/* The firmware's own request builder and response checker, against a Modbus
 * TCP server written independently of them (modbus_tcp_server.py), over a real
 * socket.
 *
 * The unit tests next door check the framing against what this file's author
 * believed the specification says. This one checks it against somebody else's
 * reading of the same document, which is the only way a shared misreading
 * shows up. It is also exactly the code path mb_tcp_master.c uses on the
 * board, minus the sockets ESP-IDF provides.
 *
 *   make -C test/host client
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mb_pdu.h"

#define UNIT 11

static int checks, failures;

#define CHECK(cond, ...)                                      \
    do {                                                      \
        checks++;                                             \
        if (!(cond)) {                                        \
            failures++;                                       \
            printf("  FEHLER %d: ", __LINE__);                \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
        }                                                     \
    } while (0)

static int s_fd = -1;
static uint16_t s_tid;

static bool io_all(uint8_t *buf, size_t len, bool sending)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = sending ? send(s_fd, buf + done, len - done, 0)
                            : recv(s_fd, buf + done, len - done, 0);
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

/* One read, framed by the firmware's own code. Returns the exception code, or
   0 with *value set, or -1 if nothing usable came back. */
static int read_one(uint8_t fc, uint16_t reg, uint8_t type, bool swap, double *value)
{
    uint16_t regs = mb_value_regs(type);
    if (!regs) return -1;

    uint8_t req[MB_MBAP_LEN + 5];
    uint16_t tid = ++s_tid;
    uint16_t n = mb_build_read_request(req, tid, UNIT, fc, reg, regs);
    if (!io_all(req, n, true)) return -1;

    uint8_t rsp[MB_MBAP_LEN + 2 + 8];
    if (!io_all(rsp, 6, false)) return -1;
    uint16_t rest = mb_be16(&rsp[4]);
    if (rest < 2 || rest > sizeof(rsp) - 6) return -1;
    if (!io_all(&rsp[6], rest, false)) return -1;

    const uint8_t *data = NULL;
    uint8_t exc = 0;
    mb_rsp_t r = mb_parse_read_response(rsp, (uint16_t)(6 + rest), tid, UNIT, fc,
                                        regs, &data, &exc);
    if (r == MB_RSP_EXCEPTION) return exc;
    if (r != MB_RSP_OK) return -1;
    return mb_value_decode(data, type, swap, value) ? 0 : -1;
}

static bool close_to(double a, double b) { double d = a - b; return (d < 0 ? -d : d) < 1e-3; }

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 15020;

    s_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s_fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("kein Server auf 127.0.0.1:%d (errno %d)\n", port, errno);
        return 2;
    }
    int one = 1;
    setsockopt(s_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("Firmware-Rahmen gegen einen fremden Modbus-TCP-Server\n");

    double v;
    /* Holding 0..1 hold 0x1234 0x5678. */
    CHECK(read_one(3, 0, MB_VAL_U32, false, &v) == 0 && v == 305419896.0,
          "u32 ABCD ergab %.0f", v);
    CHECK(read_one(3, 0, MB_VAL_U32, true, &v) == 0 && v == 1450709556.0,
          "u32 CDAB ergab %.0f", v);
    CHECK(read_one(3, 2, MB_VAL_U16, false, &v) == 0 && v == 4660.0,
          "u16 ergab %.0f", v);
    CHECK(read_one(3, 3, MB_VAL_S16, false, &v) == 0 && v == -1.0,
          "s16 -1 ergab %.0f", v);
    CHECK(read_one(3, 6, MB_VAL_S16, false, &v) == 0 && v == -32768.0,
          "s16 min ergab %.0f", v);
    CHECK(read_one(3, 4, MB_VAL_F32, false, &v) == 0 && close_to(v, 231.845),
          "float32 ergab %.4f", v);
    CHECK(read_one(3, 7, MB_VAL_U32, false, &v) == 0 && v == 4294967295.0,
          "u32 max ergab %.0f", v);
    CHECK(read_one(3, 7, MB_VAL_S32, false, &v) == 0 && v == -1.0,
          "s32 -1 ergab %.0f", v);

    /* Input registers are a different table on the same device. */
    CHECK(read_one(4, 0, MB_VAL_F32, false, &v) == 0 && close_to(v, 100.0),
          "input float 100 ergab %.4f", v);
    CHECK(read_one(4, 2, MB_VAL_F32, false, &v) == 0 && close_to(v, -100.0),
          "input float -100 ergab %.4f", v);

    /* A register the device does not have. */
    CHECK(read_one(3, 99, MB_VAL_U16, false, &v) == 0x02,
          "fehlendes Register gab nicht 0x02");
    /* The second register of a 32-bit read is missing: still 0x02, and the
       connection must stay usable afterwards. */
    CHECK(read_one(4, 3, MB_VAL_U32, false, &v) == 0x02,
          "halb fehlendes Registerpaar gab nicht 0x02");
    CHECK(read_one(3, 2, MB_VAL_U16, false, &v) == 0 && v == 4660.0,
          "Verbindung nach einer Ausnahme unbrauchbar");

    /* A hundred reads in a row: the transaction id wraps nothing and no
       answer is ever mistaken for the previous one. */
    int bad = 0;
    for (int i = 0; i < 100; i++)
        if (read_one(3, 2, MB_VAL_U16, false, &v) != 0 || v != 4660.0) bad++;
    CHECK(bad == 0, "%d von 100 Wiederholungen falsch", bad);

    close(s_fd);
    printf("%d Pruefungen, %d Fehler\n", checks, failures);
    return failures ? 1 : 0;
}
