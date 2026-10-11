#include "mb_status_json.h"

#include "mb_tcp_server.h"
#include "mb_tcp_master.h"
#include "sdkconfig.h"

/* What the two Modbus sides are doing, as opposed to how they are set up.
   Without it a value that never arrives looks the same as one that arrives
   wrong. */
void mb_status_json(cJSON *root)
{
    mb_tcp_stats_t st;
    mb_tcp_server_get_stats(&st);
    cJSON *srv = cJSON_AddObjectToObject(root, "tcp_server");
    cJSON_AddNumberToObject(srv, "accepted",   st.accepted);
    cJSON_AddNumberToObject(srv, "refused",    st.refused);
    cJSON_AddNumberToObject(srv, "evicted",    st.evicted);
    cJSON_AddNumberToObject(srv, "requests",   st.requests);
    cJSON_AddNumberToObject(srv, "exceptions", st.exceptions);
    cJSON_AddNumberToObject(srv, "forwarded",  st.forwarded);
    cJSON_AddNumberToObject(srv, "overloaded", st.overloaded);
    cJSON_AddNumberToObject(srv, "malformed",  st.malformed);

    /* "tcp_master" for what it was called before it read RS-485 too. */
    cJSON *arr = cJSON_AddArrayToObject(root, "tcp_master");
#if CONFIG_MB_RS485_MASTER || CONFIG_MB_TCP_MASTER
    mbm_status_t ms[APP_CFG_MBM_COUNT];
    uint8_t n = mb_tcp_master_get_status(ms, APP_CFG_MBM_COUNT);
    for (uint8_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name",    ms[i].name);
        cJSON_AddBoolToObject  (o, "enabled", ms[i].enabled);
        if (ms[i].valid) {
            cJSON_AddNumberToObject(o, "value",  ms[i].value);
            cJSON_AddNumberToObject(o, "age_ms", (double)ms[i].age_ms);
        } else {
            cJSON_AddNullToObject(o, "value");
        }
        cJSON_AddNumberToObject(o, "reads",  ms[i].reads);
        cJSON_AddNumberToObject(o, "errors", ms[i].errors);
        if (ms[i].last_error[0]) cJSON_AddStringToObject(o, "last_error", ms[i].last_error);
        cJSON_AddItemToArray(arr, o);
    }
#endif
}
