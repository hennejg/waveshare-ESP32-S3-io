# Builds and runs the framing tests on the development machine. No board and
# no ESP-IDF needed: mb_pdu.c depends on nothing but stdint.
#
#   make -C test/host

MAIN  := ../../apps/full/main
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g \
          -fsanitize=address,undefined -fno-omit-frame-pointer -I$(MAIN)

PORT ?= 15020

.PHONY: all clean client poller master dns tz resync qjscron qjsint names order di trial drain nvsread auth cfgnum resume auxwrite auxlock wifimon jobs authguard mqttlog
all: test_mb_pdu client poller master dns tz resync qjscron qjsint names order di trial drain nvsread auth cfgnum resume auxwrite auxlock wifimon jobs authguard mqttlog
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

# The Modbus master's name cache and connection pick, cut out of the real source.
dns: test_mb_dns
	./test_mb_dns

test_mb_dns: gen_dns_test.py $(MAIN)/mb_tcp_master.c $(MAIN)/net_budget.h ../../components/board/app_config.h
	python3 gen_dns_test.py
	$(CC) $(CFLAGS) -I../../components/board -Istubs test_mb_dns.c -o $@

# The rule engine's resync of dropped input changes, helpers cut out of scripting.c.
order: test_order
	./test_order

test_order: gen_order_test.py ../../components/scripting/scripting.c
	python3 gen_order_test.py
	$(CC) $(CFLAGS) test_order.c -o $@

# The order in which input levels reach the rule engine and MQTT, cut out of di.c.
di: test_di
	./test_di

test_di: gen_di_test.py ../../components/board/di.c
	python3 gen_di_test.py
	$(CC) $(CFLAGS) test_di.c -o $@

# Reading the stored rule script while a save changes it, cut out of web_server.c.
nvsread: test_nvsread
	./test_nvsread

test_nvsread: gen_nvsread_test.py $(MAIN)/web_server.c
	python3 gen_nvsread_test.py
	$(CC) $(CFLAGS) test_nvsread.c -o $@

# The button token flow's timeout against a new begin, cut out of auth.c.
auth: test_auth
	./test_auth

test_auth: gen_auth_test.py ../../components/board/auth.c ../../components/board/auth.h
	python3 gen_auth_test.py
	$(CC) $(CFLAGS) -I../../components/board -Wno-unused-parameter -Istubs test_auth.c -lpthread -o $@

# POST /api/config's number checks and the CAN check on load, cut out of
# web_server.c and app_config.c; ESP-IDF's cJSON.
CJSON := ../../esp-idf/components/json/cJSON
cfgnum: test_cfgnum
	./test_cfgnum

test_cfgnum: gen_cfgnum_test.py qjs-obj/cJSON.o $(MAIN)/web_server.c ../../components/board/app_config.c ../../components/board/app_config.h
	python3 gen_cfgnum_test.py
	$(CC) $(CFLAGS) -I$(CJSON) test_cfgnum.c qjs-obj/cJSON.o -o $@

# Upstream's cJSON, as QuickJS: without the warnings, built once.
qjs-obj/cJSON.o: $(CJSON)/cJSON.c
	@mkdir -p qjs-obj
	$(CC) -O1 -w -c $< -o $@

# Modbus writes to several header pads against a configuration save, cut
# out of mb_server.c and aux_io.c.
auxwrite: test_auxwrite
	./test_auxwrite

test_auxwrite: gen_auxwrite_test.py $(MAIN)/mb_server.c ../../components/board/aux_io.c ../../components/board/aux_io.h
	python3 gen_auxwrite_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable -I../../components/board -Istubs test_auxwrite.c -o $@

# The header pads' status getters against a mode change, cut out of
# aux_io.c; pthreads. Thread sanitizer instead of address sanitizer.
auxlock: test_auxlock
	./test_auxlock

test_auxlock: gen_auxlock_test.py ../../components/board/aux_io.c ../../components/board/aux_io.h
	python3 gen_auxlock_test.py
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -O1 -g -fsanitize=thread -I../../components/board -Istubs test_auxlock.c -lpthread -o $@

# The MQTT broker URL as it is logged, url_for_log() cut out of app_mqtt.c.
mqttlog: test_mqttlog
	./test_mqttlog

test_mqttlog: gen_mqttlog_test.py $(MAIN)/app_mqtt.c
	python3 gen_mqttlog_test.py
	$(CC) $(CFLAGS) -Wno-format-truncation test_mqttlog.c -o $@

# The web servers' access guard: every handler of both apps opens with
# check_auth(), and cross_site() cut out of each.
authguard: test_authguard
	./test_authguard

test_authguard: gen_authguard_test.py $(MAIN)/web_server.c ../../apps/matter/main/web_server.c
	python3 gen_authguard_test.py
	$(CC) $(CFLAGS) -Wno-unused-function test_authguard.c -o $@

# The WiFi bootstrap's link monitor and portal routing, cut out of wifi_config.c
# and compiled with the definitions its CMakeLists.txt sets.
wifimon: test_wifimon
	./test_wifimon

test_wifimon: gen_wifimon_test.py ../../components/esp32-wifi-bootstrap/src/wifi_config.c ../../components/esp32-wifi-bootstrap/CMakeLists.txt
	python3 gen_wifimon_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_wifimon.c -o $@

names: test_names
	./test_names

test_names: gen_names_test.py ../../components/board/app_config.c
	python3 gen_names_test.py
	$(CC) $(CFLAGS) test_names.c -o $@

resync: test_resync
	./test_resync

test_resync: gen_resync_test.py ../../components/scripting/scripting.c
	python3 gen_resync_test.py
	$(CC) $(CFLAGS) test_resync.c -o $@

# QuickJS itself, with the firmware's patches, running the real dsl.js: local
# time around the DST switches and a minute cron across them. The engine is
# built once into qjs-obj/ without sanitizers -- it is large, and upstream's.
QJS     := ../../components/quickjs/extern/quickjs
QJS_SRC := quickjs.c libregexp.c libunicode.c cutils.c dtoa.c
QJS_OBJ := $(QJS_SRC:%.c=qjs-obj/%.o)
qjs-obj/%.o: $(QJS)/%.c
	@mkdir -p qjs-obj
	$(CC) -O1 -w -D_GNU_SOURCE -DCONFIG_VERSION='"host"' -I$(QJS) -c $< -o $@

qjscron: test_qjs_cron
	./test_qjs_cron

test_qjs_cron: test_qjs_cron.c $(QJS_OBJ) ../../components/scripting/dsl.js
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -I$(QJS) test_qjs_cron.c $(QJS_OBJ) -lm -o $@

qjsint: test_qjs_interrupt
	./test_qjs_interrupt

test_qjs_interrupt: test_qjs_interrupt.c $(QJS_OBJ) ../../components/scripting/dsl.js
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -I$(QJS) test_qjs_interrupt.c $(QJS_OBJ) -lm -o $@

# The reload's trial load against a boot under the same heap limit: the dry
# timers and outputs cut out of scripting.c, real QuickJS and dsl.js.
trial: test_trial
	./test_trial

test_trial: gen_trial_test.py ../../components/scripting/scripting.c ../../components/scripting/bindings.c ../../components/scripting/scripting_priv.h $(QJS_OBJ) ../../components/scripting/dsl.js
	python3 gen_trial_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -I$(QJS) -Istubs -I../../components/scripting/include test_trial.c $(QJS_OBJ) -lm -o $@

# Deferred rule timers after a budget interrupt: drain_overflow() and the
# recovery cut out of scripting.c, real QuickJS and dsl.js.
drain: test_drain
	./test_drain

test_drain: gen_drain_test.py ../../components/scripting/scripting.c $(QJS_OBJ) ../../components/scripting/dsl.js
	python3 gen_drain_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -I$(QJS) test_drain.c $(QJS_OBJ) -lm -o $@

# Promise jobs under the time budget: drain_jobs() cut out of scripting.c,
# real QuickJS.
jobs: test_jobs
	./test_jobs

test_jobs: gen_jobs_test.py ../../components/scripting/scripting.c $(QJS_OBJ)
	python3 gen_jobs_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I$(QJS) test_jobs.c $(QJS_OBJ) -lm -o $@

# A time-budget interrupt and the other rules on the same event, heldFor()
# checks cut short: recover_interrupt() cut out of scripting.c, real dsl.js.
resume: test_resume
	./test_resume

test_resume: gen_resume_test.py ../../components/scripting/scripting.c $(QJS_OBJ) ../../components/scripting/dsl.js
	python3 gen_resume_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -I$(QJS) test_resume.c $(QJS_OBJ) -lm -o $@

# QuickJS's timezone offset, newlib branch, cut out of the patched quickjs.c.
tz: test_tz
	./test_tz

test_tz: gen_tz_test.py ../../components/quickjs/extern/quickjs/quickjs.c
	python3 gen_tz_test.py
	$(CC) $(CFLAGS) test_tz.c -o $@

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
	rm -rf test_mqttlog test_mqttlog.c test_authguard test_authguard.c test_jobs test_jobs.c test_wifimon test_wifimon.c test_auxlock test_auxlock.c test_auxwrite test_auxwrite.c test_resume test_resume.c test_resume_old_dsl.js test_cfgnum test_cfgnum.c test_auth test_auth.c test_nvsread test_nvsread.c test_drain test_drain.c test_di test_di.c test_trial test_trial.c test_order test_order.c test_names test_names.c test_qjs_interrupt test_mb_pdu test_mb_client test_mb_poller test_mb_poller.c \
	       test_mb_master test_mb_master.c \
	       test_mb_dns test_mb_dns.c test_tz test_tz.c test_resync test_resync.c test_qjs_cron qjs-obj *.dSYM
