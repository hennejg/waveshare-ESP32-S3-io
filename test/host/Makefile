# Builds and runs the framing tests on the development machine. No board and
# no ESP-IDF needed: mb_pdu.c depends on nothing but stdint.
#
#   make -C test/host

MAIN  := ../../apps/full/main
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g \
          -fsanitize=address,undefined -fno-omit-frame-pointer -I$(MAIN)

.PHONY: all clean
all: test_mb_pdu
	./test_mb_pdu

test_mb_pdu: test_mb_pdu.c $(MAIN)/mb_pdu.c $(MAIN)/mb_pdu.h $(MAIN)/mb_request.h
	$(CC) $(CFLAGS) test_mb_pdu.c $(MAIN)/mb_pdu.c -o $@

clean:
	rm -rf test_mb_pdu test_mb_pdu.dSYM
