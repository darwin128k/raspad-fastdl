#define _GNU_SOURCE
#include <fastdl/bridge.h>
#include <fastdl/http.h>
#include <authum/logger.h>
#include <authum/path.h>
#include <authum/event/server.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static authum_host_t g_host;
static authum_logger_t g_log;
static fastdl_http_t *g_http;
static int g_log_subscribed;
static unsigned g_log_budget;
static time_t g_log_second;
static void access_line(const char *line,void *user) {
    (void)user;
    authum_logger_info(&g_log,"%s",line);
}
static authum_event_result_t access_frame(const authum_char_t *name,authum_ptr payload,authum_ptr user) {
    (void)name; (void)payload; (void)user;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    if(now.tv_sec!=g_log_second) { g_log_second=now.tv_sec; g_log_budget=100; }
    unsigned limit=g_log_budget<8?g_log_budget:8;
    g_log_budget-=fastdl_http_drain_logs(g_http,access_line,NULL,limit);
    return authum_event_result_continue;
}
void fastdl_authum_stop(void) {
    if(g_log_subscribed) {
        authum_events_unsubscribe(authum_host_events(&g_host),AUTHUM_EVENT_SERVER_FRAME,access_frame,NULL);
        g_log_subscribed=0;
    }
    fastdl_http_shutdown(g_http);
    fastdl_http_drain_logs(g_http,access_line,NULL,257);
    fastdl_http_stop(g_http); g_http=NULL;
    authum_logger_close(&g_log); authum_host_clear(&g_host);
}
authum_bool_t fastdl_authum_start(authum_host_t *host) {
    if(g_http) return authum_bool_false;
    authum_host_copy(&g_host,host); authum_logger_init(&g_log,&g_host);
    const authum_path_t *module=authum_host_env_module(&g_host);
    const char *dir=module?authum_path_as_cstr(module):NULL;
    char base[4096], config[4096], error[512];
    if(!dir || !realpath(dir,base)) {
        authum_logger_error(&g_log,"cannot resolve module directory '%s': %s",dir?dir:"(missing)",strerror(errno)); goto failed;
    }
    if(snprintf(config,sizeof config,"%s/configs/fastdl.cfg",base)>=(int)sizeof config) {
        authum_logger_error(&g_log,"module path too long: '%s'",base); goto failed;
    }
    char game[4096], fallback[4096];
    const authum_path_t *game_path=authum_host_env_game(&g_host);
    const char *game_dir=game_path?authum_path_as_cstr(game_path):NULL;
    if(!game_dir) {
        if(snprintf(fallback,sizeof fallback,"%s/../../../..",base)>=(int)sizeof fallback) goto failed;
        game_dir=fallback;
    }
    if(!realpath(game_dir,game)) { authum_logger_error(&g_log,"cannot resolve game directory '%s': %s",game_dir,strerror(errno)); goto failed; }
    fastdl_config_t cfg; fastdl_config_defaults(&cfg);
    if(!fastdl_config_load(&cfg,config,error,sizeof error)) { authum_logger_error(&g_log,"%s",error); goto failed; }
    authum_logger_info(&g_log,"config='%s', game='%s', sources=%u, listen=%s:%d",config,game,cfg.source_count,cfg.bind,cfg.port);
    for(unsigned i=0;i<cfg.source_count;i++) {
        char expanded[4096];
        if(!fastdl_config_expand(cfg.sources[i],base,game,expanded,sizeof expanded)) {
            authum_logger_error(&g_log,"source[%u]='%s': invalid placeholder or path too long",i+1,cfg.sources[i]); goto failed;
        }
        authum_logger_info(&g_log,"source[%u]='%s' (relative paths use '%s')",i+1,expanded,base);
    }
    g_http=fastdl_http_start(&cfg,base,game,error,sizeof error);
    if(!g_http) { authum_logger_error(&g_log,"HTTP startup: %s",error); goto failed; }
    if(cfg.access_log) {
        if(!authum_host_subscribe(&g_host,AUTHUM_EVENT_SERVER_FRAME,access_frame,NULL)) {
            authum_logger_error(&g_log,"access_log requires Authum server-frame events"); goto failed;
        }
        g_log_subscribed=1; g_log_second=0; g_log_budget=100;
    }
    authum_logger_info(&g_log,"ready %s:%d, HTTP worker thread (cpu=%d)",cfg.bind,cfg.port,cfg.cpu);
    return authum_bool_true;
failed:
    authum_logger_error(&g_log,"FastDL startup failed"); fastdl_authum_stop(); return authum_bool_false;
}
