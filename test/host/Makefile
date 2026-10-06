# Builds and runs the framing tests on the development machine. No board and
# no ESP-IDF needed: mb_pdu.c depends on nothing but stdint.
#
#   make -C test/host

MAIN  := ../../apps/full/main
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g \
          -fsanitize=address,undefined -fno-omit-frame-pointer -I$(MAIN)

PORT ?= 15020

.PHONY: all clean client poller master
all: test_mb_pdu client poller master
	./test_mb_pdu

# The firmware's framing against a Modbus TCP server written independently of
# it. The server is started and stopped around the run, so the target is
# self-contained.
client: test_mb_client modbus_tcp_server.py
	@python3 modbus_tcp_server.py $(PORT) >/dev/null 2>&1 & \
	 SRV=$$!; \
	 sleep 1; \
	 ./test_mb_client $(PORT); RC=$$?; \
	 kill $$SRV 2>/dev/null; \
	 exit $$RC

# The Modbus master's configuration handover, cut out of the real source.
master: test_mb_master
	./test_mb_master

test_mb_master: gen_master_test.py $(MAIN)/mb_tcp_master.c ../../components/board/app_config.h
	python3 gen_master_test.py
	$(CC) $(CFLAGS) -I../../components/board -Istubs test_mb_master.c -o $@

# The TCP server's receive path, cut out of the real source by the generator
# so the test cannot drift away from the code.
poller: test_mb_poller
	./test_mb_poller

test_mb_poller: gen_poller_test.py $(MAIN)/mb_tcp_server.c $(MAIN)/mb_pdu.c $(MAIN)/mb_pdu.h
	python3 gen_poller_test.py
	$(CC) $(CFLAGS) test_mb_poller.c $(MAIN)/mb_pdu.c -o $@

test_mb_client: test_mb_client.c $(MAIN)/mb_pdu.c $(MAIN)/mb_pdu.h $(MAIN)/mb_request.h
	$(CC) $(CFLAGS) test_mb_client.c $(MAIN)/mb_pdu.c -o $@

test_mb_pdu: test_mb_pdu.c $(MAIN)/mb_pdu.c $(MAIN)/mb_pdu.h $(MAIN)/mb_request.h
	$(CC) $(CFLAGS) test_mb_pdu.c $(MAIN)/mb_pdu.c -o $@

clean:
	rm -rf test_mb_pdu test_mb_client test_mb_poller test_mb_poller.c \
	       test_mb_master test_mb_master.c *.dSYM
