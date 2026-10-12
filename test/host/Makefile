# Builds and runs the framing tests on the development machine. No board and
# no ESP-IDF needed: mb_pdu.c depends on nothing but stdint.
#
#   make -C test/host

MAIN  := ../../apps/full/main
MB    := ../../components/modbus
CFLAGS := -std=c11 -Wall -Wextra -Werror -O1 -g \
          -fsanitize=address,undefined -fno-omit-frame-pointer -I$(MAIN) -I$(MB) \
          -include stubs/host_compat.h

PORT ?= 15020

# Every target below; GEN are the ones a gen_<name>_test.py writes as
# test_<name>.c and builds into test_<name>.
TESTS := client poller master dns tz resync qjscron qjsint names order di trial drain nvsread auth cfgnum resume auxwrite auxlock wifimon jobs authguard mqttlog timerid evict gwerr n2khb pwhash chan reseterr pwmfail wifiportal sntpapply capdns uiauth timeset mqtttopic cfgpost portalconn cantx mqttstate ledstatus i2clock sntprtc jsoncast rtuunwind caninit rtuerr mqttdrop portalurl payload mqttfrag padlevel mbmmatter canmatter pwmlevel canfmt rulesubs nulbytes mqttowner ledinit ledapi scriptinit
GEN   := tz resync names order di trial drain nvsread auth cfgnum resume auxwrite auxlock wifimon jobs authguard mqttlog timerid evict gwerr n2khb pwhash chan reseterr pwmfail wifiportal sntpapply capdns timeset mqtttopic cfgpost portalconn cantx mqttstate ledstatus i2clock sntprtc jsoncast rtuunwind caninit rtuerr mqttdrop portalurl payload mqttfrag padlevel mbmmatter canmatter pwmlevel canfmt rulesubs nulbytes mqttowner ledinit ledapi scriptinit

.PHONY: all clean $(TESTS)
all: test_mb_pdu $(TESTS)
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

# The web password as stored and checked, cut out of auth.c, against
# ESP-IDF's own mbedTLS (MD, SHA-256, PKCS5) built for the host.
MBT     := ../../esp-idf/components/mbedtls/mbedtls
MBT_SRC := md.c sha256.c pkcs5.c platform_util.c constant_time.c
MBT_OBJ := $(MBT_SRC:%.c=mbt-obj/%.o)
MBT_INC := -I$(MBT)/include -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"' -Istubs
mbt-obj/%.o: $(MBT)/library/%.c stubs/mbedtls_host_config.h
	@mkdir -p mbt-obj
	$(CC) -O1 -w $(MBT_INC) -I$(MBT)/library -c $< -o $@

pwhash: test_pwhash
	./test_pwhash

test_pwhash: gen_pwhash_test.py ../../components/board/auth.c $(MBT_OBJ)
	python3 gen_pwhash_test.py
	$(CC) $(CFLAGS) $(MBT_INC) -Wno-unused-function -Wno-unused-parameter test_pwhash.c $(MBT_OBJ) -o $@

# The NMEA 2000 heartbeat as it goes on the bus, cut out of can_server.c.
# What the CAN server sends on its own, cut out of can_server.c.
# How the rule engine learns the broker state, cut out of scripting.c.
mqttstate: test_mqttstate
	./test_mqttstate

test_mqttstate: gen_mqttstate_test.py ../../components/scripting/scripting.c
	python3 gen_mqttstate_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_mqttstate.c -o $@

cantx: test_cantx
	./test_cantx

test_cantx: gen_cantx_test.py ../../components/can/can_server.c
	python3 gen_cantx_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_cantx.c -o $@

n2khb: test_n2khb
	./test_n2khb

test_n2khb: gen_n2khb_test.py ../../components/can/can_server.c
	python3 gen_n2khb_test.py
	$(CC) $(CFLAGS) -Wno-unused-function test_n2khb.c -o $@

# What the RS-485 gateway reports for a failed transaction, cut out of
# mb_gateway.c.
gwerr: test_gwerr
	./test_gwerr

test_gwerr: gen_gwerr_test.py $(MB)/mb_gateway.c $(MB)/mb_gateway.h
	python3 gen_gwerr_test.py
	$(CC) $(CFLAGS) -Istubs -Wno-unused-function -Wno-unused-parameter test_gwerr.c $(MB)/mb_pdu.c -o $@

# The Modbus TCP server at its connection limit: accept_one() and the
# eviction of the slot idle longest, cut out of mb_tcp_server.c.
evict: test_evict
	./test_evict

test_evict: gen_evict_test.py $(MB)/mb_tcp_server.c $(MAIN)/net_budget.h ../../components/modbus/Kconfig
	python3 gen_evict_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_evict.c -o $@

# The Modbus master's configuration handover, cut out of the real source.
master: test_mb_master
	./test_mb_master

test_mb_master: gen_master_test.py $(MB)/mb_tcp_master.c $(MAIN)/main.c $(MB)/Kconfig ../../components/board/app_config.h
	python3 gen_master_test.py
	$(CC) $(CFLAGS) -I../../components/board -Istubs test_mb_master.c -o $@

# The Modbus master's name cache and connection pick, cut out of the real source.
dns: test_mb_dns
	./test_mb_dns

test_mb_dns: gen_dns_test.py $(MB)/mb_tcp_master.c $(MB)/Kconfig ../../components/board/app_config.h
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

test_cfgnum: gen_cfgnum_test.py qjs-obj/cJSON.o $(MAIN)/web_server.c ../../components/board/app_config.c ../../components/board/app_config.h ../../components/board/web_common.c
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

test_auxwrite: gen_auxwrite_test.py $(MB)/mb_server.c ../../components/board/aux_io.c ../../components/board/aux_io.h
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

test_authguard: gen_authguard_test.py $(MAIN)/web_server.c ../../apps/matter/main/web_server.c ../../components/board/web_common.c
	python3 gen_authguard_test.py
	$(CC) $(CFLAGS) -Wno-unused-function test_authguard.c -o $@

# The WiFi bootstrap's link monitor and portal routing, cut out of wifi_config.c
# and compiled with the definitions its CMakeLists.txt sets.
# The WiFi setup portal's settings page and POST, cut out of wifi_config.c,
# with form_urlencoded.c and the page templates as they are.
WIFI := ../../components/esp32-wifi-bootstrap
wifiportal: test_wifiportal
	./test_wifiportal

test_wifiportal: gen_wifiportal_test.py $(WIFI)/src/wifi_config.c $(WIFI)/src/form_urlencoded.c $(WIFI)/content/index.html.h
	python3 gen_wifiportal_test.py
	$(CC) $(CFLAGS) -I$(WIFI)/src -I$(WIFI)/content -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_wifiportal.c -o $@

# When a configuration save restarts SNTP, cut out of sntp_sync.c.
sntpapply: test_sntpapply
	./test_sntpapply

test_sntpapply: gen_sntpapply_test.py ../../components/board/sntp_sync.c
	python3 gen_sntpapply_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_sntpapply.c -o $@

# The setup AP's DNS reply builder, cut out of wifi_config.c.
capdns: test_capdns
	./test_capdns

test_capdns: gen_capdns_test.py ../../components/esp32-wifi-bootstrap/src/wifi_config.c
	python3 gen_capdns_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_capdns.c -o $@

# The Basic auth header both web UIs build, run in node.
uiauth: gen_uiauth_test.py ../../www/index.html ../../apps/matter/www/index.html
	python3 gen_uiauth_test.py
	node test_uiauth.js

# How long one client can hold the setup portal, cut out of wifi_config.c.
portalconn: test_portalconn
	./test_portalconn

test_portalconn: gen_portalconn_test.py ../../components/esp32-wifi-bootstrap/src/wifi_config.c
	python3 gen_portalconn_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_portalconn.c -o $@

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

# A PWM update the LEDC timer refuses, pwm_set() cut out of aux_io.c.
pwmfail: test_pwmfail
	./test_pwmfail

test_pwmfail: gen_pwmfail_test.py ../../components/board/aux_io.c
	python3 gen_pwmfail_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_pwmfail.c -o $@

# The handlers that reboot after an NVS write, cut out of web_server.c.
# Setting the clock from the web UI, cut out of web_server.c.
timeset: test_timeset
	./test_timeset

test_timeset: gen_timeset_test.py qjs-obj/cJSON.o $(MAIN)/web_server.c ../../components/board/web_common.c ../../components/board/json_exact.c
	python3 gen_timeset_test.py
	$(CC) $(CFLAGS) -I$(CJSON) -Wno-unused-function test_timeset.c qjs-obj/cJSON.o -o $@

# Saving the configuration, the whole handler cut out of web_server.c.
cfgpost: test_cfgpost
	./test_cfgpost

test_cfgpost: gen_cfgpost_test.py qjs-obj/cJSON.o $(MAIN)/web_server.c ../../components/board/app_config.h ../../components/board/web_common.c ../../components/board/json_exact.c
	python3 gen_cfgpost_test.py
	$(CC) $(CFLAGS) -I$(CJSON) -I../../components/board -Istubs -Wno-unused-function test_cfgpost.c qjs-obj/cJSON.o -lm -o $@

reseterr: test_reseterr
	./test_reseterr

test_reseterr: gen_reseterr_test.py qjs-obj/cJSON.o $(MAIN)/web_server.c ../../components/board/json_exact.c
	python3 gen_reseterr_test.py
	$(CC) $(CFLAGS) -I$(CJSON) -Wno-unused-function -Wno-unused-parameter test_reseterr.c qjs-obj/cJSON.o -o $@

# Channel numbers handed to the I/O bindings, cut out of bindings.c, real QuickJS.
# Topics refused before they reach the broker, cut out of app_mqtt.c and
# bindings.c. Real QuickJS.
mqtttopic: test_mqtttopic
	./test_mqtttopic

test_mqtttopic: gen_mqtttopic_test.py $(MAIN)/app_mqtt.c ../../components/scripting/bindings.c $(QJS_OBJ)
	python3 gen_mqtttopic_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I$(QJS) test_mqtttopic.c $(QJS_OBJ) -lm -o $@

chan: test_chan
	./test_chan

test_chan: gen_chan_test.py ../../components/scripting/bindings.c $(QJS_OBJ)
	python3 gen_chan_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I$(QJS) test_chan.c $(QJS_OBJ) -lm -o $@

# Rule timer ids past 2^31: the timer table cut out of scripting.c, real
# QuickJS and dsl.js.
timerid: test_timerid
	./test_timerid

test_timerid: gen_timerid_test.py ../../components/scripting/scripting.c ../../components/scripting/dsl.js $(QJS_OBJ)
	python3 gen_timerid_test.py
	$(CC) -O1 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I$(QJS) test_timerid.c $(QJS_OBJ) -lm -o $@

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

test_mb_poller: gen_poller_test.py $(MB)/mb_tcp_server.c $(MB)/mb_pdu.c $(MB)/mb_pdu.h ../../components/modbus/Kconfig
	python3 gen_poller_test.py
	$(CC) $(CFLAGS) test_mb_poller.c $(MB)/mb_pdu.c -o $@

test_mb_client: test_mb_client.c $(MB)/mb_pdu.c $(MB)/mb_pdu.h $(MB)/mb_request.h
	$(CC) $(CFLAGS) test_mb_client.c $(MB)/mb_pdu.c -o $@

test_mb_pdu: test_mb_pdu.c $(MB)/mb_pdu.c $(MB)/mb_pdu.h $(MB)/mb_request.h
	$(CC) $(CFLAGS) test_mb_pdu.c $(MB)/mb_pdu.c -o $@

# The status LED after network loss and after a forced colour, cut out of
# led.c and auth.c.
ledstatus: test_ledstatus
	./test_ledstatus

test_ledstatus: gen_ledstatus_test.py ../../components/board/led.c ../../components/board/auth.c
	python3 gen_ledstatus_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_ledstatus.c -o $@

# The shared I2C bus lock around transfers and the bus reset, cut out of
# dout.c and rtc.c.
i2clock: test_i2clock
	./test_i2clock

test_i2clock: gen_i2clock_test.py ../../components/board/dout.c ../../components/board/rtc.c
	python3 gen_i2clock_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_i2clock.c -o $@

# The RTC written off lwIP's task after an SNTP sync, cut out of sntp_sync.c.
sntprtc: test_sntprtc
	./test_sntprtc

test_sntprtc: gen_sntprtc_test.py ../../components/board/sntp_sync.c
	python3 gen_sntprtc_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_sntprtc.c -o $@

# Numbers from MQTT LED and buzzer commands, cut out of led.c and buzzer.c;
# ESP-IDF's cJSON, UBSan as a hard error.
jsoncast: test_jsoncast
	./test_jsoncast

test_jsoncast: gen_jsoncast_test.py qjs-obj/cJSON.o ../../components/board/led.c ../../components/board/buzzer.c
	python3 gen_jsoncast_test.py
	$(CC) $(CFLAGS) -fno-sanitize-recover=all -I$(CJSON) -Wno-unused-function -Wno-unused-parameter test_jsoncast.c qjs-obj/cJSON.o -lm -o $@

# A failed RTU slave start, cut out of mb_server.c.
rtuunwind: test_rtuunwind
	./test_rtuunwind

test_rtuunwind: gen_rtuunwind_test.py $(MB)/mb_server.c
	python3 gen_rtuunwind_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_rtuunwind.c -o $@

# A failed CAN start, cut out of can_server.c.
caninit: test_caninit
	./test_caninit

test_caninit: gen_caninit_test.py ../../components/can/can_server.c
	python3 gen_caninit_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_caninit.c -o $@

# The poll table's error text for an RS-485 read, cut out of mb_tcp_master.c.
rtuerr: test_rtuerr
	./test_rtuerr

test_rtuerr: gen_rtuerr_test.py $(MB)/mb_tcp_master.c $(MB)/mb_request.h $(MB)/mb_gateway.h
	python3 gen_rtuerr_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_rtuerr.c -o $@

# MQTT messages too long for the rule engine, cut out of scripting.c.
mqttdrop: test_mqttdrop
	./test_mqttdrop

test_mqttdrop: gen_mqttdrop_test.py ../../components/scripting/scripting.c
	python3 gen_mqttdrop_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_mqttdrop.c -o $@

# Request routing in the setup portal, cut out of wifi_config.c.
portalurl: test_portalurl
	./test_portalurl

test_portalurl: gen_portalurl_test.py ../../components/esp32-wifi-bootstrap/src/wifi_config.c
	python3 gen_portalurl_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_portalurl.c -o $@

clean:
	rm -rf $(GEN:%=test_%) $(GEN:%=test_%.c) test_resume_old_dsl.js test_uiauth.js test_qjs_interrupt test_mb_pdu test_mb_client test_mb_poller test_mb_poller.c test_mb_master test_mb_master.c test_mb_dns test_mb_dns.c test_qjs_cron qjs-obj mbt-obj *.dSYM

# MQTT LED and buzzer payloads parsed in place, cut out of led.c and buzzer.c.
payload: test_payload
	./test_payload

test_payload: gen_payload_test.py qjs-obj/cJSON.o ../../components/board/led.c ../../components/board/buzzer.c ../../components/board/json_exact.c
	python3 gen_payload_test.py
	$(CC) $(CFLAGS) -I$(CJSON) -Wno-unused-function -Wno-unused-parameter test_payload.c qjs-obj/cJSON.o -lm -o $@

# MQTT messages larger than the client's buffer, cut out of app_mqtt.c.
mqttfrag: test_mqttfrag
	./test_mqttfrag

test_mqttfrag: gen_mqttfrag_test.py $(MAIN)/app_mqtt.c
	python3 gen_mqttfrag_test.py
	$(CC) $(CFLAGS) -Wno-unused-function test_mqttfrag.c -o $@

# A PWM header pad as a Matter Dimmable Plug-in Unit, cut out of matter.cpp.
padlevel: test_padlevel
	./test_padlevel

test_padlevel: gen_padlevel_test.py ../../apps/matter/main/matter.cpp ../../components/board/aux_io.h
	python3 gen_padlevel_test.py
	$(CC) $(CFLAGS) -Wno-unused-function test_padlevel.c -o $@

# The poll table's values as Matter sensors: unit conversion, the table's
# check, when a save needs a reboot, and no gateway without CONFIG_MB_GATEWAY.
mbmmatter: test_mbmmatter
	./test_mbmmatter

test_mbmmatter: gen_mbmmatter_test.py qjs-obj/cJSON.o ../../apps/matter/main/mbm_value.c ../../apps/matter/main/matter_mbm.cpp ../../components/board/web_common.c ../../components/board/web_common.h ../../components/board/app_config.c ../../components/board/app_config.h ../../components/modbus/mb_gateway.c
	python3 gen_mbmmatter_test.py
	$(CC) $(CFLAGS) -I$(CJSON) -I../../components/board -Istubs -Wno-unused-function test_mbmmatter.c qjs-obj/cJSON.o -lm -o $@

# CAN's hooks into the firmware and outputs reported to Matter, cut out of
# can_server.c and dout.c.
canmatter: test_canmatter
	./test_canmatter

test_canmatter: gen_canmatter_test.py ../../components/can/can_server.c ../../components/board/dout.c
	python3 gen_canmatter_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_canmatter.c -o $@

# A pad's PWM level as Matter remembers it, cut out of matter.cpp (C++).
pwmlevel: test_pwmlevel
	./test_pwmlevel

test_pwmlevel: gen_pwmlevel_test.py ../../apps/matter/main/matter.cpp
	python3 gen_pwmlevel_test.py
	c++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-unused-function test_pwmlevel.cpp -o $@

# The documented CAN command formats and the first reports after a start,
# cut out of can_server.c.
canfmt: test_canfmt
	./test_canfmt

test_canfmt: gen_canfmt_test.py ../../components/can/can_server.c
	python3 gen_canfmt_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_canfmt.c -o $@

# The rule engine's broker subscriptions, cut out of scripting.c, with
# QuickJS and the real dsl.js.
rulesubs: test_rulesubs
	./test_rulesubs

test_rulesubs: gen_rulesubs_test.py ../../components/scripting/scripting.c ../../components/scripting/dsl.js $(QJS_OBJ)
	python3 gen_rulesubs_test.py
	$(CC) -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined -I$(QJS) test_rulesubs.c $(QJS_OBJ) -lm -o $@

# NUL bytes and trailing data in commands, cut out of dout.c, json_exact.c
# and scripting.c.
nulbytes: test_nulbytes
	./test_nulbytes

test_nulbytes: gen_nulbytes_test.py ../../components/board/dout.c ../../components/board/json_exact.c ../../components/scripting/scripting.c qjs-obj/cJSON.o $(QJS_OBJ)
	python3 gen_nulbytes_test.py
	$(CC) -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -fsanitize=address,undefined -I$(CJSON) -I$(QJS) test_nulbytes.c qjs-obj/cJSON.o $(QJS_OBJ) -lm -o $@

# MQTT subscriptions shared by several owners, cut out of app_mqtt.c.
mqttowner: test_mqttowner
	./test_mqttowner

test_mqttowner: gen_mqttowner_test.py $(MAIN)/app_mqtt.c $(MAIN)/app_mqtt.h
	python3 gen_mqttowner_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter test_mqttowner.c -o $@

# The RGB LED driver: start-up failures, instant colours against sequences,
# the colour format -- led.c taken whole.
ledinit: test_ledinit
	./test_ledinit

test_ledinit: gen_ledinit_test.py ../../components/board/led.c ../../components/board/json_exact.c qjs-obj/cJSON.o
	python3 gen_ledinit_test.py
	$(CC) $(CFLAGS) -I$(CJSON) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-missing-field-initializers test_ledinit.c qjs-obj/cJSON.o -lm -o $@

# Both firmware LED HTTP handlers: refuse invalid commands before any write.
ledapi: test_ledapi
	./test_ledapi

test_ledapi: gen_ledapi_test.py $(MAIN)/web_server.c ../../apps/matter/main/web_server.c ../../components/board/web_common.c ../../components/board/json_exact.c qjs-obj/cJSON.o
	python3 gen_ledapi_test.py
	$(CC) $(CFLAGS) -I$(CJSON) test_ledapi.c qjs-obj/cJSON.o -lm -o $@

# Startup allocation failures and resource ownership in the rule engine.
scriptinit: test_scriptinit
	./test_scriptinit

test_scriptinit: gen_scriptinit_test.py ../../components/scripting/scripting.c
	python3 gen_scriptinit_test.py
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable test_scriptinit.c -o $@
