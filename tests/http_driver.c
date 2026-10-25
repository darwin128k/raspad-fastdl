#define _GNU_SOURCE
/* Test-only process wrapper around the exact same worker used by fastdl.so. */
#include <fastdl/http.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static volatile sig_atomic_t stopped;
static void on_signal(int n) { (void)n; stopped=1; }
static void access_line(const char *line,void *user) { (void)user; fprintf(stderr,"%s\n",line); }
int main(int argc,char **argv) {
    const char *config=NULL,*base=".",*game_dir=NULL; int control=-1;
    for(int i=1;i<argc;i++) {
        if(i+1>=argc) return 1;
        if(!strcmp(argv[i],"--config")) config=argv[++i];
        else if(!strcmp(argv[i],"--base")) base=argv[++i];
        else if(!strcmp(argv[i],"--game-dir")) game_dir=argv[++i];
        else if(!strcmp(argv[i],"--control-fd")) control=atoi(argv[++i]);
        else return 1;
    }
    if(!config) return 1;
    char module[4096],game[4096],fallback[4096],error[512];
    if(!realpath(base,module)) return 1;
    if(!game_dir) {
        if(snprintf(fallback,sizeof fallback,"%s/../../../..",module)>=(int)sizeof fallback) return 1;
        game_dir=fallback;
    }
    if(!realpath(game_dir,game)) return 1;
    fastdl_config_t cfg; fastdl_config_defaults(&cfg);
    if(!fastdl_config_load(&cfg,config,error,sizeof error)) { fprintf(stderr,"%s\n",error); return 1; }
    signal(SIGTERM,on_signal); signal(SIGINT,on_signal);
    fastdl_http_t *http=fastdl_http_start(&cfg,module,game,error,sizeof error);
    if(!http) { fprintf(stderr,"%s\n",error); return 1; }
    if(control>=0 && send(control,"READY",5,MSG_NOSIGNAL)!=5) { fastdl_http_stop(http); return 1; }
    while(!stopped) {
        if(!getenv("FASTDL_TEST_HOLD_LOGS")) fastdl_http_drain_logs(http,access_line,NULL,16);
        struct pollfd p={.fd=control,.events=POLLIN};
        if(poll(&p,1,50)>0) break;
    }
    fastdl_http_shutdown(http);
    fastdl_http_drain_logs(http,access_line,NULL,257);
    fastdl_http_stop(http);
    if(control>=0) close(control);
    return 0;
}
