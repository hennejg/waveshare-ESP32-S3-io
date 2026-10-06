/* Host tests for the Modbus PDU framing.
 *
 * mb_pdu.c depends on nothing but stdint, so it compiles and runs here. The
 * cases below are the boundaries that are tedious to reach through a real
 * client and easy to get wrong in the code: every count limit, every length
 * that does not match its function code, and the byte counts of the two
 * multiple-write functions.
 *
 * The random part matters more than the fixed cases. Built with the address
 * and undefined-behaviour sanitizers, it turns "the parser must never read
 * past the frame" from a claim into something the build checks.
 *
 *   cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -I../../apps/full/main \
 *      test_mb_pdu.c ../../apps/full/main/mb_pdu.c -o test_mb_pdu && ./test_mb_pdu
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "mb_pdu.h"

static int checks, failures;

#define CHECK(cond, ...)                                               \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            printf("  FEHLER %s:%d  ", __func__, __LINE__);            \
            printf(__VA_ARGS__);                                       \
            printf("\n");                                              \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ helpers */

static uint16_t put_read(uint8_t *p, uint8_t fc, uint16_t addr, uint16_t cnt)
{
    p[0] = fc;
    p[1] = (uint8_t)(addr >> 8); p[2] = (uint8_t)addr;
    p[3] = (uint8_t)(cnt >> 8);  p[4] = (uint8_t)cnt;
    return 5;
}

static uint16_t put_multi(uint8_t *p, uint8_t fc, uint16_t addr, uint16_t cnt,
                          uint8_t byte_cnt)
{
    p[0] = fc;
    p[1] = (uint8_t)(addr >> 8); p[2] = (uint8_t)addr;
    p[3] = (uint8_t)(cnt >> 8);  p[4] = (uint8_t)cnt;
    p[5] = byte_cnt;
    memset(&p[6], 0xA5, byte_cnt);
    return (uint16_t)(6 + byte_cnt);
}

/* ------------------------------------------------------------ the cases */

static void test_reads_accepted(void)
{
    const struct { uint8_t fc; uint16_t max; } tbl[] = {
        { MB_FC_READ_COILS,    2000 }, { MB_FC_READ_DISCRETE, 2000 },
        { MB_FC_READ_HOLDING,   125 }, { MB_FC_READ_INPUT,     125 },
    };
    for (unsigned i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        uint8_t pdu[8];
        mb_request_t r;

        for (uint16_t cnt = 1; ; cnt = tbl[i].max) {
            uint16_t len = put_read(pdu, tbl[i].fc, 0x1234, cnt);
            uint8_t exc = mb_parse_pdu(pdu, len, &r);
            CHECK(exc == 0, "fc %u count %u abgelehnt mit 0x%02X", tbl[i].fc, cnt, exc);
            CHECK(r.addr == 0x1234, "fc %u Adresse falsch", tbl[i].fc);
            CHECK(r.count == cnt, "fc %u Anzahl falsch", tbl[i].fc);
            if (cnt == tbl[i].max) break;
        }

        /* Zero and one past the limit are both client errors. */
        uint16_t len = put_read(pdu, tbl[i].fc, 0, 0);
        CHECK(mb_parse_pdu(pdu, len, &r) == MB_EXC_ILLEGAL_VALUE,
              "fc %u count 0 angenommen", tbl[i].fc);
        len = put_read(pdu, tbl[i].fc, 0, (uint16_t)(tbl[i].max + 1));
        CHECK(mb_parse_pdu(pdu, len, &r) == MB_EXC_ILLEGAL_VALUE,
              "fc %u count %u angenommen", tbl[i].fc, tbl[i].max + 1);

        /* A read request is exactly five bytes. */
        for (uint16_t bad = 0; bad < 9; bad++) {
            if (bad == 5) continue;
            put_read(pdu, tbl[i].fc, 0, 1);
            uint8_t exc = mb_parse_pdu(pdu, bad, &r);
            CHECK(exc != 0, "fc %u Laenge %u angenommen", tbl[i].fc, bad);
        }
    }
}

static void test_single_writes(void)
{
    uint8_t pdu[8];
    mb_request_t r;

    /* Only 0xFF00 and 0x0000 mean anything for a single coil. */
    CHECK(mb_parse_pdu(pdu, put_read(pdu, MB_FC_WRITE_COIL, 7, 0xFF00), &r) == 0,
          "FC05 ein abgelehnt");
    CHECK(r.addr == 7 && r.count == 1 && r.data_len == 2, "FC05 Felder falsch");
    CHECK(r.data[0] == 0xFF && r.data[1] == 0x00, "FC05 Wert nicht durchgereicht");
    CHECK(mb_parse_pdu(pdu, put_read(pdu, MB_FC_WRITE_COIL, 7, 0x0000), &r) == 0,
          "FC05 aus abgelehnt");
    for (uint32_t v = 0; v <= 0xFFFF; v += 97) {
        if (v == 0xFF00 || v == 0x0000) continue;
        uint16_t len = put_read(pdu, MB_FC_WRITE_COIL, 0, (uint16_t)v);
        CHECK(mb_parse_pdu(pdu, len, &r) == MB_EXC_ILLEGAL_VALUE,
              "FC05 Wert 0x%04X angenommen", v);
    }

    /* A holding register takes any value at all. */
    CHECK(mb_parse_pdu(pdu, put_read(pdu, MB_FC_WRITE_REGISTER, 1, 0xBEEF), &r) == 0,
          "FC06 abgelehnt");
    CHECK(r.count == 1 && r.data[0] == 0xBE && r.data[1] == 0xEF, "FC06 Wert falsch");
}

static void test_multi_writes(void)
{
    uint8_t pdu[300];
    mb_request_t r;

    const struct { uint8_t fc; uint16_t max; bool bits; } tbl[] = {
        { MB_FC_WRITE_COILS,     1968, true  },
        { MB_FC_WRITE_REGISTERS,  123, false },
    };
    for (unsigned i = 0; i < 2; i++) {
        uint16_t max = tbl[i].max;
        uint16_t want_max = tbl[i].bits ? (uint16_t)((max + 7) / 8) : (uint16_t)(max * 2);

        for (uint16_t cnt = 1; ; cnt = max) {
            uint16_t want = tbl[i].bits ? (uint16_t)((cnt + 7) / 8) : (uint16_t)(cnt * 2);
            uint16_t len  = put_multi(pdu, tbl[i].fc, 3, cnt, (uint8_t)want);
            uint8_t exc = mb_parse_pdu(pdu, len, &r);
            CHECK(exc == 0, "fc %u count %u abgelehnt mit 0x%02X", tbl[i].fc, cnt, exc);
            CHECK(r.data_len == want, "fc %u Nutzlaenge falsch", tbl[i].fc);
            CHECK(r.data == &pdu[6], "fc %u Datenzeiger falsch", tbl[i].fc);
            if (cnt == max) break;
        }
        CHECK(want_max <= MB_DATA_MAX, "fc %u Nutzlast %u passt nicht in %u",
              tbl[i].fc, want_max, MB_DATA_MAX);

        /* Count out of range. */
        uint16_t len = put_multi(pdu, tbl[i].fc, 0, 0, 0);
        CHECK(mb_parse_pdu(pdu, len, &r) == MB_EXC_ILLEGAL_VALUE,
              "fc %u count 0 angenommen", tbl[i].fc);

        /* Byte count that does not match the count. */
        uint16_t cnt = 4;
        uint16_t want = tbl[i].bits ? 1 : 8;
        for (int delta = -2; delta <= 2; delta++) {
            if (delta == 0) continue;
            int bc = (int)want + delta;
            if (bc < 0 || bc > 255) continue;
            len = put_multi(pdu, tbl[i].fc, 0, cnt, (uint8_t)bc);
            /* put_multi sized the frame from bc, so the length agrees with the
               byte count and only the count/byte-count pair is wrong. */
            CHECK(mb_parse_pdu(pdu, len, &r) == MB_EXC_ILLEGAL_VALUE,
                  "fc %u Bytezahl %d statt %u angenommen", tbl[i].fc, bc, want);
        }

        /* Byte count right, frame length wrong. */
        len = put_multi(pdu, tbl[i].fc, 0, cnt, (uint8_t)want);
        CHECK(mb_parse_pdu(pdu, (uint16_t)(len - 1), &r) == MB_EXC_ILLEGAL_VALUE,
              "fc %u zu kurzer Rahmen angenommen", tbl[i].fc);
        CHECK(mb_parse_pdu(pdu, (uint16_t)(len + 1), &r) == MB_EXC_ILLEGAL_VALUE,
              "fc %u zu langer Rahmen angenommen", tbl[i].fc);

        /* Below the minimum length the header itself is incomplete. */
        for (uint16_t bad = 0; bad < 7; bad++)
            CHECK(mb_parse_pdu(pdu, bad, &r) != 0,
                  "fc %u Laenge %u angenommen", tbl[i].fc, bad);
    }
}

static void test_unknown_function_codes(void)
{
    uint8_t pdu[8];
    mb_request_t r;
    const uint8_t known[] = { 1, 2, 3, 4, 5, 6, 15, 16, 43 };

    for (unsigned fc = 0; fc < 256; fc++) {
        bool is_known = false;
        for (unsigned k = 0; k < sizeof(known); k++) if (known[k] == fc) is_known = true;
        if (is_known) continue;
        memset(pdu, 0, sizeof(pdu));
        pdu[0] = (uint8_t)fc;
        CHECK(mb_parse_pdu(pdu, 5, &r) == MB_EXC_ILLEGAL_FUNC,
              "Funktionscode %u nicht als unbekannt gemeldet", fc);
    }
}

static void test_responses(void)
{
    uint8_t pdu[300], out[2 + MB_DATA_MAX], data[MB_DATA_MAX];
    mb_request_t r;
    memset(data, 0x5A, sizeof(data));

    /* A read answers with the function code, a byte count and the data. */
    mb_parse_pdu(pdu, put_read(pdu, MB_FC_READ_HOLDING, 0, 125), &r);
    uint16_t n = mb_build_response(&r, pdu, data, 250, MB_EXC_NONE, out);
    CHECK(n == 252, "Leseantwort %u statt 252 Byte", n);
    CHECK(out[0] == MB_FC_READ_HOLDING && out[1] == 250, "Leseantwort Kopf falsch");
    CHECK(out[2] == 0x5A && out[251] == 0x5A, "Leseantwort Daten falsch");

    /* Every write answers with the first five bytes of the request. */
    const uint8_t writes[] = { MB_FC_WRITE_COIL, MB_FC_WRITE_REGISTER,
                               MB_FC_WRITE_COILS, MB_FC_WRITE_REGISTERS };
    for (unsigned i = 0; i < 4; i++) {
        uint16_t len = (writes[i] == MB_FC_WRITE_COIL)
                     ? put_read(pdu, writes[i], 0x0102, 0xFF00)
                     : (writes[i] == MB_FC_WRITE_REGISTER)
                       ? put_read(pdu, writes[i], 0x0102, 0x0304)
                       : put_multi(pdu, writes[i], 0x0102, 4,
                                   writes[i] == MB_FC_WRITE_COILS ? 1 : 8);
        uint8_t exc = mb_parse_pdu(pdu, len, &r);
        CHECK(exc == 0, "fc %u abgelehnt", writes[i]);
        n = mb_build_response(&r, pdu, NULL, 0, MB_EXC_NONE, out);
        CHECK(n == 5, "fc %u Antwort %u statt 5 Byte", writes[i], n);
        CHECK(memcmp(out, pdu, 5) == 0, "fc %u Echo weicht ab", writes[i]);
    }

    /* An exception is the function code with the top bit set, and the code. */
    mb_parse_pdu(pdu, put_read(pdu, MB_FC_READ_COILS, 0, 8), &r);
    n = mb_build_response(&r, pdu, data, 1, MB_EXC_ILLEGAL_ADDR, out);
    CHECK(n == 2, "Ausnahmeantwort %u statt 2 Byte", n);
    CHECK(out[0] == 0x81 && out[1] == 0x02, "Ausnahmeantwort falsch");
}

/* -------------------------------------------------- device identification */

static void test_device_id_parse(void)
{
    uint8_t pdu[8];
    mb_request_t r;

    /* The one well-formed shape: four bytes, MEI 14, read code 1..4. */
    for (uint8_t code = 1; code <= 4; code++) {
        pdu[0] = 0x2B; pdu[1] = 0x0E; pdu[2] = code; pdu[3] = 0x81;
        uint8_t exc = mb_parse_pdu(pdu, 4, &r);
        CHECK(exc == 0, "FC43 Lesecode %u abgelehnt mit 0x%02X", code, exc);
        CHECK(r.fc == MB_FC_DEVICE_ID && r.count == code && r.addr == 0x81,
              "FC43 Felder falsch (fc %u code %u id %u)", r.fc, r.count, r.addr);
        CHECK(r.data == &pdu[1] && r.data_len == 3, "FC43 Datenzeiger falsch");
    }

    /* Read code 0 and 5 are bad values; MEI 13 is a function we lack. */
    pdu[0] = 0x2B; pdu[1] = 0x0E; pdu[2] = 0; pdu[3] = 0;
    CHECK(mb_parse_pdu(pdu, 4, &r) == MB_EXC_ILLEGAL_VALUE, "FC43 Lesecode 0 angenommen");
    pdu[2] = 5;
    CHECK(mb_parse_pdu(pdu, 4, &r) == MB_EXC_ILLEGAL_VALUE, "FC43 Lesecode 5 angenommen");
    pdu[2] = 1; pdu[1] = 0x0D;
    CHECK(mb_parse_pdu(pdu, 4, &r) == MB_EXC_ILLEGAL_FUNC, "FC43 MEI 13 nicht als unbekannt gemeldet");

    /* Any other length is wrong, including the five bytes a register read has. */
    pdu[1] = 0x0E;
    for (uint16_t bad = 0; bad < 8; bad++) {
        if (bad == 4) continue;
        CHECK(mb_parse_pdu(pdu, bad, &r) != 0, "FC43 Laenge %u angenommen", bad);
    }
}

/* Reads the objects out of an encoded response, checking the framing on the
   way: the list must end exactly where the length says it does. */
static unsigned devid_objects(const uint8_t *d, uint16_t len, uint8_t *ids, unsigned max)
{
    unsigned n = 0;
    uint16_t pos = 6;
    while (pos < len && n < max) {
        if (pos + 2 > len) return 9999;
        uint8_t id = d[pos], l = d[pos + 1];
        if (pos + 2 + l > len) return 9999;
        ids[n++] = id;
        pos = (uint16_t)(pos + 2 + l);
    }
    return (pos == len && n == d[5]) ? n : 9999;
}

static void test_device_id_encode(void)
{
    const mb_devid_obj_t tbl[] = {
        { 0x00, "Vendor" }, { 0x01, "Product" }, { 0x02, "1.2.3" },
        { 0x03, "https://example.invalid" }, { 0x04, "Name" }, { 0x05, "Model" }, { 0x06, "App" },
        { 0x80, "SERIAL" }, { 0x81, "00:11:22:33:44:55" },
    };
    const uint16_t N = sizeof(tbl) / sizeof(tbl[0]);
    uint8_t out[MB_DATA_MAX], ids[16];
    uint16_t len;

    /* Basic stream from the start: the three mandatory objects, no more. */
    CHECK(mb_devid_encode(tbl, N, MB_DEVID_BASIC, 0, out, &len) == 0, "Basisstrom abgelehnt");
    CHECK(out[0] == 0x0E && out[1] == 1 && out[2] == 0x83 && out[3] == 0 && out[4] == 0,
          "Basisstrom Kopf falsch (%02X %02X %02X %02X %02X)", out[0], out[1], out[2], out[3], out[4]);
    CHECK(devid_objects(out, len, ids, 16) == 3 && ids[0] == 0 && ids[2] == 2, "Basisstrom Objekte falsch");

    /* A regular stream asked for from object 0 restarts at the category's first. */
    CHECK(mb_devid_encode(tbl, N, MB_DEVID_REGULAR, 0, out, &len) == 0, "Regulaerstrom abgelehnt");
    CHECK(devid_objects(out, len, ids, 16) == 4 && ids[0] == 3 && ids[3] == 6, "Regulaerstrom Objekte falsch");
    /* ...and from the middle, only what follows. */
    CHECK(mb_devid_encode(tbl, N, MB_DEVID_REGULAR, 5, out, &len) == 0, "Regulaerstrom ab 5 abgelehnt");
    CHECK(devid_objects(out, len, ids, 16) == 2 && ids[0] == 5 && ids[1] == 6, "Regulaerstrom ab 5 falsch");

    /* Extended: the private objects. */
    CHECK(mb_devid_encode(tbl, N, MB_DEVID_EXTENDED, 0, out, &len) == 0, "Erweiterter Strom abgelehnt");
    CHECK(devid_objects(out, len, ids, 16) == 2 && ids[0] == 0x80 && ids[1] == 0x81, "Erweiterter Strom falsch");

    /* Individual: one object of any category, and an unknown one is refused. */
    CHECK(mb_devid_encode(tbl, N, MB_DEVID_INDIVIDUAL, 0x81, out, &len) == 0, "Einzelobjekt abgelehnt");
    CHECK(devid_objects(out, len, ids, 16) == 1 && ids[0] == 0x81, "Einzelobjekt falsch");
    CHECK(memcmp(&out[8], "00:11:22:33:44:55", 17) == 0 && out[7] == 17, "Einzelobjekt Wert falsch");
    CHECK(mb_devid_encode(tbl, N, MB_DEVID_INDIVIDUAL, 0x42, out, &len) == MB_EXC_ILLEGAL_ADDR,
          "unbekanntes Einzelobjekt angenommen");
    CHECK(len == 0, "Ausnahme hinterlaesst Laenge %u", len);
    CHECK(mb_devid_encode(tbl, N, 0, 0, out, &len) == MB_EXC_ILLEGAL_VALUE, "Lesecode 0 kodiert");

    /* Conformity follows the table: without private objects it is 0x82,
       with only the mandatory three 0x81. */
    CHECK(mb_devid_encode(tbl, 7, MB_DEVID_BASIC, 0, out, &len) == 0 && out[2] == 0x82, "Konformitaet ohne Privatobjekte");
    CHECK(mb_devid_encode(tbl, 3, MB_DEVID_BASIC, 0, out, &len) == 0 && out[2] == 0x81, "Konformitaet nur Basis");
    /* A category the table does not have at all. */
    CHECK(mb_devid_encode(tbl, 3, MB_DEVID_EXTENDED, 0, out, &len) == MB_EXC_ILLEGAL_ADDR, "leere Kategorie geliefert");

    /* Segmentation: private objects too long for one PDU. The client walks
       the "next object id" chain and must see every object exactly once. */
    char big[8][120];
    mb_devid_obj_t many[3 + 8];
    memcpy(many, tbl, 3 * sizeof(tbl[0]));
    for (unsigned i = 0; i < 8; i++) {
        memset(big[i], 'A' + (int)i, 119); big[i][119] = 0;
        many[3 + i].id = (uint8_t)(0x80 + i); many[3 + i].value = big[i];
    }
    uint8_t next = 0; unsigned seen = 0, rounds = 0;
    do {
        CHECK(mb_devid_encode(many, 11, MB_DEVID_EXTENDED, next, out, &len) == 0, "Segment abgelehnt");
        CHECK(len <= MB_DATA_MAX, "Segment %u Byte, Puffer %u", len, MB_DATA_MAX);
        unsigned n = devid_objects(out, len, ids, 16);
        CHECK(n != 9999 && n >= 1, "Segment unlesbar");
        for (unsigned i = 0; i < n && n != 9999; i++)
            CHECK(ids[i] == 0x80 + seen + i, "Objekt %u ausser der Reihe", ids[i]);
        seen += (n == 9999) ? 0 : n;
        next = out[4];
        rounds++;
    } while (out[3] == 0xFF && rounds < 10);
    CHECK(seen == 8, "Segmentierung lieferte %u von 8 Objekten in %u Runden", seen, rounds);
    CHECK(rounds == 4, "Segmentierung brauchte %u Runden statt 4", rounds);

    /* A single value that cannot ever be delivered is refused outright. */
    char huge[260]; memset(huge, 'x', 259); huge[259] = 0;
    mb_devid_obj_t bad[] = { { 0x00, huge } };
    CHECK(mb_devid_encode(bad, 1, MB_DEVID_BASIC, 0, out, &len) == MB_EXC_DEVICE_FAILURE, "unlieferbares Objekt angenommen");

    /* The response framing puts the data straight behind the function code. */
    uint8_t pdu[4] = { 0x2B, 0x0E, 0x01, 0x00 }, resp[2 + MB_DATA_MAX];
    mb_request_t r;
    mb_parse_pdu(pdu, 4, &r);
    mb_devid_encode(tbl, N, r.count, (uint8_t)r.addr, out, &len);
    uint16_t n = mb_build_response(&r, pdu, out, len, MB_EXC_NONE, resp);
    CHECK(n == 1 + len && resp[0] == 0x2B && resp[1] == 0x0E, "FC43 Antwortrahmen falsch");
    n = mb_build_response(&r, pdu, NULL, 0, MB_EXC_ILLEGAL_ADDR, resp);
    CHECK(n == 2 && resp[0] == 0xAB && resp[1] == 0x02, "FC43 Ausnahmerahmen falsch");
}

static void test_ascii_regs(void)
{
    uint8_t out[8];
    memset(out, 0xEE, sizeof(out));
    CHECK(mb_ascii_to_regs("abc", 3, out) == 6, "Laenge falsch");
    CHECK(out[0] == 'a' && out[1] == 'b' && out[2] == 'c' && out[3] == 0 && out[4] == 0 && out[5] == 0,
          "Zeichen oder Auffuellung falsch");
    CHECK(out[6] == 0xEE, "ueber die Register hinaus geschrieben");
    memset(out, 0xEE, sizeof(out));
    CHECK(mb_ascii_to_regs("abcdefgh", 2, out) == 4 && out[3] == 'd' && out[4] == 0xEE, "Abschneiden falsch");
    CHECK(mb_ascii_to_regs(NULL, 1, out) == 2 && out[0] == 0 && out[1] == 0, "NULL nicht als leer behandelt");
}

/* ------------------------------------------------------------ random input */

/* The parser must survive anything and, when it accepts, must leave a request
   whose data really lies inside the frame and whose response fits the buffer
   the caller provides. The sanitizers catch the reading; these checks catch
   the arithmetic. */
static void test_random(unsigned rounds)
{
    uint8_t  out[2 + MB_DATA_MAX];
    uint8_t  data[MB_DATA_MAX];
    unsigned accepted = 0;
    memset(data, 0x33, sizeof(data));
    srand(12345);

    for (unsigned i = 0; i < rounds; i++) {
        uint16_t len = (uint16_t)(rand() % (MB_PDU_MAX + 2));   /* 0 is allowed too */
        uint8_t *pdu = malloc(len ? len : 1);                   /* exact size: ASAN sees overruns */
        for (uint16_t j = 0; j < len; j++) pdu[j] = (uint8_t)(rand() & 0xFF);

        /* Pure noise almost never parses, and the arithmetic worth checking is
           on the accepting path. Two thirds of the rounds therefore start from
           a well-formed request with random field values and then corrupt one
           byte of it, which lands either side of every limit. */
        if (i % 3) {
            free(pdu);
            const uint8_t known[] = { 1, 2, 3, 4, 5, 6, 15, 16, 43 };
            uint8_t  fc   = known[rand() % sizeof(known)];
            uint16_t addr = (uint16_t)(rand() & 0xFFFF);
            uint16_t cnt;
            uint8_t  scratch[MB_PDU_MAX + 4];

            switch (fc) {
            case 15: cnt = (uint16_t)(1 + rand() % 1970); break;   /* just over the limit */
            case 16: cnt = (uint16_t)(1 + rand() % 125);  break;
            case 5:  cnt = (rand() & 1) ? 0xFF00 : 0x0000; break;
            case 6:  cnt = (uint16_t)(rand() & 0xFFFF);   break;
            default: cnt = (uint16_t)(1 + rand() % 2002); break;
            }

            if (fc == 43) {
                scratch[0] = 43; scratch[1] = (uint8_t)(rand() % 3 ? 0x0E : rand());
                scratch[2] = (uint8_t)(1 + rand() % 5);     /* 5 is just past the limit */
                scratch[3] = (uint8_t)(rand() & 0xFF);
                len = 4;
            } else if (fc == 15 || fc == 16) {
                uint16_t want = (fc == 15) ? (uint16_t)((cnt + 7) / 8) : (uint16_t)(cnt * 2);
                if (want > 250) want = 250;            /* keep the frame legal in size */
                len = (uint16_t)(6 + want);
                if (len > MB_PDU_MAX) len = MB_PDU_MAX;
                put_multi(scratch, fc, addr, cnt, (uint8_t)(len - 6));
            } else {
                len = put_read(scratch, fc, addr, cnt);
            }

            /* One byte wrong, anywhere -- including the length itself. */
            if ((i % 3) == 2) {
                scratch[rand() % len] ^= (uint8_t)(1 << (rand() % 8));
                if (rand() % 4 == 0) len = (uint16_t)(rand() % (MB_PDU_MAX + 1));
            }
            pdu = malloc(len ? len : 1);
            memcpy(pdu, scratch, len);
        }

        mb_request_t r;
        uint8_t exc = mb_parse_pdu(pdu, len, &r);
        if (exc == MB_EXC_NONE) {
            accepted++;
            if (r.data_len) {
                CHECK(r.data >= pdu && r.data + r.data_len <= pdu + len,
                      "Nutzdaten liegen ausserhalb des Rahmens");
            }
            /* What a read handler would produce has to fit the caller's buffer. */
            uint16_t produced = 0;
            switch (r.fc) {
            case MB_FC_READ_COILS: case MB_FC_READ_DISCRETE:
                produced = (uint16_t)((r.count + 7) / 8); break;
            case MB_FC_READ_HOLDING: case MB_FC_READ_INPUT:
                produced = (uint16_t)(r.count * 2); break;
            default: break;
            }
            CHECK(produced <= MB_DATA_MAX,
                  "fc %u count %u ergaebe %u Byte, Puffer hat %u",
                  r.fc, r.count, produced, MB_DATA_MAX);

            uint16_t n = mb_build_response(&r, pdu, data, produced, MB_EXC_NONE, out);
            CHECK(n <= 2 + MB_DATA_MAX, "Antwort %u Byte, Puffer %u", n, 2 + MB_DATA_MAX);
        } else {
            uint16_t n = mb_build_response(&r, pdu, NULL, 0, exc, out);
            CHECK(n == 2, "Ausnahmeantwort %u statt 2 Byte", n);
        }
        free(pdu);
    }
    printf("  Zufallstest: %u Rahmen, davon %u angenommen\n", rounds, accepted);
}

/* --------------------------------------------------- reading a value out */

/* The register bytes are written out by hand rather than built with the same
   arithmetic the code under test uses -- a shared helper would agree with a
   wrong implementation. */
static void test_value_decode(void)
{
    double v;

    CHECK(mb_value_regs(MB_VAL_U16) == 1 && mb_value_regs(MB_VAL_S16) == 1,
          "16-Bit-Typen brauchen ein Register");
    CHECK(mb_value_regs(MB_VAL_U32) == 2 && mb_value_regs(MB_VAL_S32) == 2 &&
          mb_value_regs(MB_VAL_F32) == 2, "32-Bit-Typen brauchen zwei Register");
    for (unsigned t = MB_VAL_COUNT; t < 256; t++)
        CHECK(mb_value_regs((uint8_t)t) == 0, "Typ %u hat eine Registerzahl", t);

    /* Unsigned and signed 16 bit. */
    CHECK(mb_value_decode((const uint8_t[]){0x00, 0x00}, MB_VAL_U16, false, &v) && v == 0, "U16 0");
    CHECK(mb_value_decode((const uint8_t[]){0x12, 0x34}, MB_VAL_U16, false, &v) && v == 4660, "U16 0x1234");
    CHECK(mb_value_decode((const uint8_t[]){0xFF, 0xFF}, MB_VAL_U16, false, &v) && v == 65535, "U16 max");
    CHECK(mb_value_decode((const uint8_t[]){0xFF, 0xFF}, MB_VAL_S16, false, &v) && v == -1, "S16 -1");
    CHECK(mb_value_decode((const uint8_t[]){0x80, 0x00}, MB_VAL_S16, false, &v) && v == -32768, "S16 min");
    CHECK(mb_value_decode((const uint8_t[]){0x7F, 0xFF}, MB_VAL_S16, false, &v) && v == 32767, "S16 max");
    /* The word-swap flag must not touch a one-register value. */
    CHECK(mb_value_decode((const uint8_t[]){0x12, 0x34}, MB_VAL_U16, true, &v) && v == 4660,
          "U16 vom Wortsupplement veraendert");

    /* 32 bit, both word orders. 0x12345678 = 305419896. */
    CHECK(mb_value_decode((const uint8_t[]){0x12,0x34,0x56,0x78}, MB_VAL_U32, false, &v)
          && v == 305419896.0, "U32 ABCD");
    CHECK(mb_value_decode((const uint8_t[]){0x56,0x78,0x12,0x34}, MB_VAL_U32, true, &v)
          && v == 305419896.0, "U32 CDAB");
    CHECK(mb_value_decode((const uint8_t[]){0xFF,0xFF,0xFF,0xFF}, MB_VAL_U32, false, &v)
          && v == 4294967295.0, "U32 max");
    CHECK(mb_value_decode((const uint8_t[]){0xFF,0xFF,0xFF,0xFF}, MB_VAL_S32, false, &v)
          && v == -1.0, "S32 -1");
    CHECK(mb_value_decode((const uint8_t[]){0x80,0x00,0x00,0x00}, MB_VAL_S32, false, &v)
          && v == -2147483648.0, "S32 min");
    CHECK(mb_value_decode((const uint8_t[]){0x00,0x00,0x80,0x00}, MB_VAL_S32, true, &v)
          && v == -2147483648.0, "S32 min CDAB");

    /* IEEE 754: 0x42C80000 = 100.0, 0x43670000 = 231.0, 0xC2C80000 = -100.0 */
    CHECK(mb_value_decode((const uint8_t[]){0x42,0xC8,0x00,0x00}, MB_VAL_F32, false, &v)
          && v == 100.0, "F32 100 ABCD");
    CHECK(mb_value_decode((const uint8_t[]){0x00,0x00,0x42,0xC8}, MB_VAL_F32, true, &v)
          && v == 100.0, "F32 100 CDAB");
    CHECK(mb_value_decode((const uint8_t[]){0xC2,0xC8,0x00,0x00}, MB_VAL_F32, false, &v)
          && v == -100.0, "F32 -100");
    CHECK(mb_value_decode((const uint8_t[]){0x00,0x00,0x00,0x00}, MB_VAL_F32, false, &v)
          && v == 0.0, "F32 0");

    /* Values a meter really sends, taken from the bench twins. */
    CHECK(mb_value_decode((const uint8_t[]){0x43,0x67,0xD8,0x35}, MB_VAL_F32, false, &v)
          && v > 231.84 && v < 231.85, "F32 Spannung L1 (231,845)");
    CHECK(mb_value_decode((const uint8_t[]){0x42,0x70,0x00,0x00}, MB_VAL_F32, false, &v)
          && v == 60.0, "F32 Pulsbreite 60");

    /* Nonsense in, false out -- and the output untouched. */
    v = 12345.0;
    CHECK(!mb_value_decode((const uint8_t[]){0,0,0,0}, MB_VAL_COUNT, false, &v) && v == 12345.0,
          "unbekannter Typ liefert einen Wert");
    CHECK(!mb_value_decode(NULL, MB_VAL_U16, false, &v), "NULL-Register angenommen");
    CHECK(!mb_value_decode((const uint8_t[]){0,0}, MB_VAL_U16, false, NULL),
          "NULL-Ausgabe angenommen");
}

/* Every byte pattern, both word orders, every type: the decoder must either
   refuse or produce a number, never read outside the registers it was given. */
static void test_value_random(unsigned rounds)
{
    for (unsigned i = 0; i < rounds; i++) {
        uint8_t type = (uint8_t)(rand() % (MB_VAL_COUNT + 2));
        uint16_t n = mb_value_regs(type);
        size_t bytes = n ? (size_t)n * 2 : 2;
        uint8_t *regs = malloc(bytes);          /* exact size: ASAN sees overruns */
        for (size_t j = 0; j < bytes; j++) regs[j] = (uint8_t)(rand() & 0xFF);

        double v = 0.0;
        bool ok = mb_value_decode(regs, type, (rand() & 1) != 0, &v);
        CHECK(ok == (n != 0), "Typ %u: Annahme %d, erwartet %d", type, ok, n != 0);
        if (ok) CHECK(v == v || type == MB_VAL_F32,
                      "Typ %u ergab keinen Wert", type);   /* NaN nur bei float */
        free(regs);
    }
    printf("  Zufallstest Werte: %u Muster\n", rounds);
}

/* ------------------------------------------- asking somebody else for data */

static void test_client_framing(void)
{
    uint8_t req[16];
    uint16_t n = mb_build_read_request(req, 0xBEEF, 11, MB_FC_READ_HOLDING, 0x0102, 2);
    CHECK(n == 12, "Anfrage %u statt 12 Byte", n);
    const uint8_t want[] = { 0xBE,0xEF, 0x00,0x00, 0x00,0x06, 11, 0x03, 0x01,0x02, 0x00,0x02 };
    CHECK(memcmp(req, want, 12) == 0, "Anfrage weicht ab");

    /* A good answer: two registers of data. */
    uint8_t rsp[32];
    const uint8_t good[] = { 0xBE,0xEF, 0x00,0x00, 0x00,0x07, 11, 0x03, 0x04,
                             0x42,0xC8,0x00,0x00 };
    const uint8_t *data; uint8_t exc;
    memcpy(rsp, good, sizeof(good));
    CHECK(mb_parse_read_response(rsp, sizeof(good), 0xBEEF, 11, 0x03, 2, &data, &exc)
          == MB_RSP_OK, "gute Antwort abgelehnt");
    CHECK(data == rsp + 9, "Datenzeiger falsch");
    double v;
    CHECK(mb_value_decode(data, MB_VAL_F32, false, &v) && v == 100.0, "Wert falsch");

    /* An exception. */
    const uint8_t ex[] = { 0xBE,0xEF, 0x00,0x00, 0x00,0x03, 11, 0x83, 0x02 };
    memcpy(rsp, ex, sizeof(ex));
    CHECK(mb_parse_read_response(rsp, sizeof(ex), 0xBEEF, 11, 0x03, 2, &data, &exc)
          == MB_RSP_EXCEPTION && exc == 0x02, "Ausnahme nicht erkannt");

    /* Everything that must be refused. Each case changes one thing about an
       otherwise good answer -- the stale transaction id above all, which is
       the failure that would otherwise produce a plausible wrong number. */
    struct { const char *what; int off; uint8_t val; uint16_t len; } bad[] = {
        { "alte Transaktionsnummer", 1, 0xEE, 13 },
        { "fremde Unit-ID",          6, 12,   13 },
        { "falscher Funktionscode",  7, 0x04, 13 },
        { "Protokoll-ID ungleich 0", 3, 0x01, 13 },
        { "Bytezahl passt nicht",    8, 0x02, 13 },
        { "Laenge passt nicht",     -1, 0,    12 },
        { "Laengenfeld luegt",       5, 0x09, 13 },
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(rsp, good, sizeof(good));
        if (bad[i].off >= 0) rsp[bad[i].off] = bad[i].val;
        mb_rsp_t r = mb_parse_read_response(rsp, bad[i].len, 0xBEEF, 11, 0x03, 2, &data, &exc);
        CHECK(r == MB_RSP_MALFORMED, "%s wurde angenommen", bad[i].what);
    }

    /* A byte count of count*2 truncated into a uint8: at 128 registers that
       is 0, and a frame carrying no data at all would have been accepted as
       the answer to a 256-byte read. The count is now checked first. */
    const uint8_t empty[] = { 0xBE,0xEF, 0x00,0x00, 0x00,0x03, 11, 0x03, 0x00 };
    memcpy(rsp, empty, sizeof(empty));
    CHECK(mb_parse_read_response(rsp, sizeof(empty), 0xBEEF, 11, 0x03, 128, &data, &exc)
          == MB_RSP_MALFORMED, "count 128 mit Bytezahl 0 angenommen");
    CHECK(mb_parse_read_response(rsp, sizeof(empty), 0xBEEF, 11, 0x03, 0, &data, &exc)
          == MB_RSP_MALFORMED, "count 0 angenommen");
    CHECK(mb_parse_read_response(rsp, sizeof(empty), 0xBEEF, 11, 0x03, 126, &data, &exc)
          == MB_RSP_MALFORMED, "count ueber 125 angenommen");

    /* Too short, at every length. */
    for (uint16_t l = 0; l < sizeof(good); l++) {
        memcpy(rsp, good, sizeof(good));
        if (l == sizeof(good)) continue;
        mb_rsp_t r = mb_parse_read_response(rsp, l, 0xBEEF, 11, 0x03, 2, &data, &exc);
        CHECK(r == MB_RSP_MALFORMED, "Laenge %u angenommen", l);
    }
}

/* Random frames into the response parser: it must never read past what it was
   given, and must never report OK with a data pointer outside the frame. */
static void test_client_random(unsigned rounds)
{
    unsigned ok = 0;
    for (unsigned i = 0; i < rounds; i++) {
        uint16_t len = (uint16_t)(rand() % 40);
        uint8_t *f = malloc(len ? len : 1);
        for (uint16_t j = 0; j < len; j++) f[j] = (uint8_t)(rand() & 0xFF);
        if (len >= 8 && (i & 1)) {            /* bias towards plausible answers */
            f[0] = 0xBE; f[1] = 0xEF; f[2] = 0; f[3] = 0;
            f[4] = 0; f[5] = (uint8_t)(len - 6);
            f[6] = 11; f[7] = (rand() & 1) ? 0x03 : 0x83;
            if (len >= 9) f[8] = (uint8_t)(rand() % 6);
        }
        const uint8_t *data; uint8_t exc;
        mb_rsp_t r = mb_parse_read_response(f, len, 0xBEEF, 11, 0x03, 2, &data, &exc);
        if (r == MB_RSP_OK) {
            ok++;
            CHECK(data >= f && data + 4 <= f + len, "Daten liegen ausserhalb des Rahmens");
        } else {
            CHECK(data == NULL || r == MB_RSP_EXCEPTION, "Datenzeiger trotz Fehler gesetzt");
        }
        free(f);
    }
    printf("  Zufallstest Antworten: %u Rahmen, davon %u gueltig\n", rounds, ok);
}

int main(void)
{
    printf("Modbus-Rahmenpruefung\n");
    test_reads_accepted();
    test_single_writes();
    test_multi_writes();
    test_unknown_function_codes();
    test_device_id_parse();
    test_device_id_encode();
    test_ascii_regs();
    test_responses();
    test_random(200000);
    test_value_decode();
    test_value_random(100000);
    test_client_framing();
    test_client_random(200000);
    printf("%d Pruefungen, %d Fehler\n", checks, failures);
    return failures ? 1 : 0;
}
