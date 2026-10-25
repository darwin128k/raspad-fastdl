#define _GNU_SOURCE
#include <fastdl/http.h>
#include <pthread.h>
#include "picohttpparser.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stdatomic.h>

/* A deliberately bounded HTTP/1.x GET/HEAD server. Parsing belongs to pico;
 * resource policy, connection accounting and lifecycle belong here. */
typedef struct client {
    int fd, file, ip_slot, state;
    char input[8192], output[512];
    size_t used, sent, output_size;
    off_t offset, remaining;
    int64_t born, progress;
    unsigned id, source;
    int status;
    uint64_t body_bytes;
    char ip[INET_ADDRSTRLEN], method[16], path[320];
    unsigned port;
    const char *close_reason;
} client_t;
typedef struct peer { uint32_t ip; int used, active, count; int64_t window; } peer_t;
struct fastdl_http {
    fastdl_config_t config;
    int roots[16], listener, wake[2];
    pthread_t thread;
    client_t clients[256];
    peer_t peers[1024];
    unsigned next_id;
    int joined;
    atomic_uint log_read, log_write, log_dropped;
    char logs[256][768];
};
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static void log_client(fastdl_http_t *s,const client_t *c,const char *event) {
    if(!s->config.access_log) return;
    unsigned w=atomic_load_explicit(&s->log_write,memory_order_relaxed);
    unsigned r=atomic_load_explicit(&s->log_read,memory_order_acquire);
    if(w-r>=256) { atomic_fetch_add_explicit(&s->log_dropped,1,memory_order_relaxed); return; }
    snprintf(s->logs[w%256],sizeof s->logs[0],
        "http event=%s id=%u ip=%s:%u method=%s path=\"%s\" status=%d bytes=%llu source=%u duration_ms=%lld",
        event,c->id,c->ip,c->port,*c->method?c->method:"-",*c->path?c->path:"-",c->status,
        (unsigned long long)c->body_bytes,c->source,(long long)(now_ms()-c->born));
    atomic_store_explicit(&s->log_write,w+1,memory_order_release);
}
static void log_token(char *dst,size_t capacity,const char *src,size_t length) {
    size_t n=length<capacity-1?length:capacity-1;
    for(size_t i=0;i<n;i++) {
        unsigned char x=(unsigned char)src[i];
        dst[i]=(x>=33 && x<=126 && x!='"' && x!='\\')?(char)x:'_';
    }
    if(length>=capacity && n>=3) memcpy(dst+n-3,"...",3);
    dst[n]=0;
}
static void close_client(fastdl_http_t *s,client_t *c) {
    if(c->fd>=0) {
        const char *event=c->close_reason?c->close_reason:
            (c->state && c->sent==c->output_size && !c->remaining?"complete":"disconnect");
        log_client(s,c,event);
        close(c->fd); --s->peers[c->ip_slot].active;
    }
    if(c->file>=0) close(c->file);
    memset(c,0,sizeof *c); c->fd=c->file=-1;
}
static int in_list(const char *list,const char *value,size_t n,int prefix) {
    while(*list) {
        while(*list==' ') ++list;
        const char *e=strchr(list,' '); size_t z=e?(size_t)(e-list):strlen(list);
        if(z && (prefix ? n>=z && !memcmp(list,value,z) && (n==z || value[z]=='/') : z==n && !memcmp(list,value,n))) return 1;
        list+=z; if(!*list) break;
    }
    return 0;
}
/* Refuse URL escapes entirely: published resource names use a conservative
 * ASCII alphabet. Every component is opened relative to its parent fd and
 * O_NOFOLLOW, avoiding both traversal and symlink TOCTOU escapes. */
static int open_resource(int root,const fastdl_config_t *cfg,const char *url,size_t len,off_t *length) {
    char path[2048];
    if(len<2 || len>=sizeof path || url[0]!='/') return -1;
    for(size_t i=1;i<len;i++) {
        unsigned char x=url[i];
        if(!((x>='a'&&x<='z')||(x>='A'&&x<='Z')||(x>='0'&&x<='9')||x=='_'||x=='-'||x=='.'||x=='/'||x=='$')) return -1;
    }
    memcpy(path,url+1,len-1); path[len-1]=0;
    if(in_list(cfg->deny_prefixes,path,len-1,1)) return -1;
    char *slash=strchr(path,'/');
    if(slash && !in_list(cfg->allow_dirs,path,(size_t)(slash-path),0)) return -1;
    /* Root-level WADs are useful; no other root-level files are published. */
    const char *ext=strrchr(path,'.');
    if(!ext || !in_list(cfg->allow_extensions,ext+1,strlen(ext+1),0) || (!slash && strcmp(ext,".wad"))) return -1;
    int dir=dup(root); if(dir<0) return -1;
    char *part=path;
    for(;;) {
        char *end=strchr(part,'/'); if(end) *end=0;
        if(!*part || part[0]=='.') { close(dir); return -1; }
        int next=openat(dir,part,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK|(end?O_DIRECTORY:0));
        close(dir); if(next<0) return -1;
        if(!end) {
            struct stat st;
            if(fstat(next,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uint64_t)st.st_size>cfg->max_file_bytes) { close(next); return -1; }
            *length=st.st_size; return next;
        }
        dir=next; part=end+1;
    }
}
static void respond(client_t *c,int code,const char *reason,off_t size,int head) {
    c->status=code;
    c->output_size=(size_t)snprintf(c->output,sizeof c->output,
        "HTTP/1.0 %d %s\r\nContent-Length: %lld\r\nContent-Type: application/octet-stream\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",code,reason,(long long)size);
    c->sent=0; c->state=1; c->remaining=head?0:size;
}
static void parse_request(client_t *c,const int *roots,const fastdl_config_t *cfg) {
    const char *method,*path; size_t ml,pl,nh=64; int minor;
    struct phr_header headers[64];
    int r=phr_parse_request(c->input,c->used,&method,&ml,&path,&pl,&minor,headers,&nh,0);
    if(r==-2 && c->used<sizeof c->input) return;
    if(r<0 || (size_t)r!=c->used || minor>1) { respond(c,400,"Bad Request",0,1); return; }
    log_token(c->method,sizeof c->method,method,ml);
    log_token(c->path,sizeof c->path,path,pl);
    int head=ml==4 && !memcmp(method,"HEAD",4);
    if(!head && !(ml==3 && !memcmp(method,"GET",3))) { respond(c,405,"Method Not Allowed",0,1); return; }
    for(size_t i=0;i<nh;i++) {
        if(!headers[i].name || (headers[i].name_len==17 && !strncasecmp(headers[i].name,"Transfer-Encoding",17)) ||
           (headers[i].name_len==14 && !strncasecmp(headers[i].name,"Content-Length",14))) {
            respond(c,400,"Bad Request",0,1); return;
        }
    }
    off_t length=0;
    for(unsigned i=0;i<cfg->source_count;i++) {
        c->file=open_resource(roots[i],cfg,path,pl,&length);
        if(c->file>=0) { c->source=i+1; break; }
    }
    if(c->file<0) respond(c,404,"Not Found",0,1);
    else respond(c,200,"OK",length,head);
}
static int peer_slot(fastdl_http_t *s,uint32_t ip,int64_t now,const fastdl_config_t *cfg) {
    int free_slot=-1;
    for(int i=0;i<1024;i++) {
        peer_t *p=&s->peers[i];
        if(p->used && p->ip==ip) {
            if(now-p->window>=1000) { p->window=now; p->count=0; }
            if(p->active>=cfg->per_ip || p->count>=cfg->requests_per_second) return -1;
            ++p->count; ++p->active; return i;
        }
        if(!p->used || (!p->active && now-p->window>=1000)) free_slot=i;
    }
    if(free_slot<0) return -1;
    s->peers[free_slot]=(peer_t){.ip=ip,.used=1,.active=1,.count=1,.window=now}; return free_slot;
}
static void *http_run(void *arg) {
    fastdl_http_t *s=arg;
    const fastdl_config_t *cfg=&s->config;
    /* sendfile has no MSG_NOSIGNAL. Block SIGPIPE only in this worker;
     * never change the game's signal dispositions or signal mask. */
    sigset_t blocked; sigemptyset(&blocked); sigaddset(&blocked,SIGPIPE);
    pthread_sigmask(SIG_BLOCK,&blocked,NULL);
    int64_t last=now_ms(); double tokens=cfg->bytes_per_second/10.0;
    unsigned cursor=0;
    for(;;) {
        struct pollfd fds[258];
        fds[0]=(struct pollfd){.fd=s->listener,.events=POLLIN};
        fds[1]=(struct pollfd){.fd=s->wake[0],.events=POLLIN};
        int64_t now=now_ms();
        tokens+=(now-last)*(double)cfg->bytes_per_second/1000.0; last=now;
        if(tokens>cfg->bytes_per_second/10.0) tokens=cfg->bytes_per_second/10.0;
        for(int i=0;i<cfg->max_clients;i++) {
            client_t *c=&s->clients[i];
            if(c->fd>=0 && (now-c->progress>cfg->timeout_seconds*1000LL || now-c->born>300000 || (!c->state && now-c->born>cfg->timeout_seconds*1000LL))) { c->close_reason="timeout"; close_client(s,c); }
            fds[i+2]=(struct pollfd){.fd=c->fd,.events=c->state?(tokens>=1?POLLOUT:0):POLLIN};
        }
        int n=poll(fds,(nfds_t)cfg->max_clients+2,20);
        if(n<0) { if(errno==EINTR) continue; break; }
        if(fds[1].revents) break; /* Owning thread requested shutdown. */
        /* Rotate the starting connection so a large transfer cannot starve peers. */
        for(int k=0;k<cfg->max_clients;k++) {
            int i=(int)((cursor+(unsigned)k)%(unsigned)cfg->max_clients);
            client_t *c=&s->clients[i]; short ev=fds[i+2].revents;
            if(c->fd<0) continue;
            if(ev&(POLLERR|POLLNVAL)) { close_client(s,c); continue; }
            if(!c->state && (ev&POLLIN)) {
                ssize_t got=recv(c->fd,c->input+c->used,sizeof c->input-c->used,0);
                if(got>0) { c->used+=(size_t)got; c->progress=now; parse_request(c,s->roots,cfg); if(c->state) log_client(s,c,"request"); }
                else if(!got || (errno!=EAGAIN&&errno!=EINTR)) close_client(s,c);
            } else if(c->state && (ev&POLLOUT) && tokens>=1) {
                ssize_t sent=0; size_t budget=(size_t)tokens; if(budget>65536) budget=65536;
                if(c->sent<c->output_size) {
                    size_t z=c->output_size-c->sent; if(z>budget) z=budget;
                    sent=send(c->fd,c->output+c->sent,z,MSG_NOSIGNAL);
                    if(sent>0) c->sent+=(size_t)sent;
                } else if(c->remaining>0) {
                    if((off_t)budget>c->remaining) budget=(size_t)c->remaining;
                    sent=sendfile(c->fd,c->file,&c->offset,budget);
                    if(sent>0) { c->remaining-=sent; c->body_bytes+=(uint64_t)sent; }
                    else if(!sent) { close_client(s,c); continue; }
                }
                if(sent>0) { tokens-=sent; c->progress=now; }
                if((sent<0 && errno!=EAGAIN&&errno!=EINTR) || (c->sent==c->output_size && !c->remaining)) close_client(s,c);
            }
            if(c->fd>=0 && (ev&POLLHUP)) close_client(s,c);
        }
        cursor=(cursor+1)%(unsigned)cfg->max_clients;
        if(fds[0].revents&POLLIN) for(int a=0;a<32;a++) {
            struct sockaddr_in remote; socklen_t z=sizeof remote;
            int fd=accept4(s->listener,(struct sockaddr *)&remote,&z,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(fd<0) break;
            int slot=-1; for(int i=0;i<cfg->max_clients;i++) if(s->clients[i].fd<0) { slot=i; break; }
            int ip=slot<0?-1:peer_slot(s,remote.sin_addr.s_addr,now,cfg);
            if(ip<0) {
                client_t rejected={.id=++s->next_id,.born=now,.port=ntohs(remote.sin_port)};
                inet_ntop(AF_INET,&remote.sin_addr,rejected.ip,sizeof rejected.ip);
                log_client(s,&rejected,"reject_limit"); close(fd); continue;
            }
            s->clients[slot]=(client_t){.fd=fd,.file=-1,.ip_slot=ip,.born=now,.progress=now};
            client_t *accepted=&s->clients[slot]; accepted->id=++s->next_id; accepted->port=ntohs(remote.sin_port);
            inet_ntop(AF_INET,&remote.sin_addr,accepted->ip,sizeof accepted->ip);
            log_client(s,accepted,"connect");
        }
    }
    for(int i=0;i<cfg->max_clients;i++) { s->clients[i].close_reason="shutdown"; close_client(s,&s->clients[i]); }
    return NULL;
}

static void http_release(fastdl_http_t *s) {
    for(unsigned i=0;i<16;i++) if(s->roots[i]>=0) close(s->roots[i]);
    if(s->listener>=0) close(s->listener);
    for(unsigned i=0;i<2;i++) if(s->wake[i]>=0) close(s->wake[i]);
    free(s);
}
fastdl_http_t *fastdl_http_start(const fastdl_config_t *config,
                               const char *module_dir,const char *game_dir,
                               char *error,unsigned error_size) {
    fastdl_http_t *s=calloc(1,sizeof *s);
    if(!s) { snprintf(error,error_size,"HTTP state allocation failed"); return NULL; }
    s->config=*config; s->listener=s->wake[0]=s->wake[1]=-1;
    atomic_init(&s->log_read,0); atomic_init(&s->log_write,0); atomic_init(&s->log_dropped,0);
    for(unsigned i=0;i<16;i++) s->roots[i]=-1;
    for(unsigned i=0;i<256;i++) s->clients[i].fd=s->clients[i].file=-1;
    if(!config->source_count || config->source_count>16 || config->max_clients<1 || config->max_clients>256 || config->cpu < -1 || config->cpu>=CPU_SETSIZE) {
        snprintf(error,error_size,"invalid HTTP configuration bounds"); goto failed;
    }
    int module=open(module_dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(module<0) { snprintf(error,error_size,"cannot open module directory '%.150s': %s",module_dir,strerror(errno)); goto failed; }
    for(unsigned i=0;i<config->source_count;i++) {
        char expanded[4096];
        if(!fastdl_config_expand(config->sources[i],module_dir,game_dir,expanded,sizeof expanded)) {
            snprintf(error,error_size,"source[%u] '%.140s': unknown placeholder or path too long",i+1,config->sources[i]); close(module); goto failed;
        }
        s->roots[i]=openat(module,expanded,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(s->roots[i]<0) { snprintf(error,error_size,"cannot open source[%u] '%.140s': %s",i+1,expanded,strerror(errno)); close(module); goto failed; }
    }
    close(module);
    if(pipe2(s->wake,O_NONBLOCK|O_CLOEXEC)) { snprintf(error,error_size,"HTTP wake pipe: %s",strerror(errno)); goto failed; }
    s->listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(s->listener<0) { snprintf(error,error_size,"HTTP socket: %s",strerror(errno)); goto failed; }
    int one=1; setsockopt(s->listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)config->port)};
    if(inet_pton(AF_INET,config->bind,&addr.sin_addr)!=1) { snprintf(error,error_size,"bind '%s' must be an IPv4 address",config->bind); goto failed; }
    if(bind(s->listener,(struct sockaddr *)&addr,sizeof addr)||listen(s->listener,64)) {
        snprintf(error,error_size,"cannot listen on %s:%d: %s",config->bind,config->port,strerror(errno)); goto failed;
    }
    pthread_attr_t attr; int rc=pthread_attr_init(&attr);
    if(rc) { snprintf(error,error_size,"HTTP thread attributes: %s",strerror(rc)); goto failed; }
    if(config->cpu>=0) {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(config->cpu,&cpus);
        rc=pthread_attr_setaffinity_np(&attr,sizeof cpus,&cpus);
    }
    if(!rc) rc=pthread_create(&s->thread,&attr,http_run,s);
    pthread_attr_destroy(&attr);
    if(rc) { snprintf(error,error_size,"cannot create HTTP thread (cpu=%d): %s",config->cpu,strerror(rc)); goto failed; }
    return s;
failed:
    http_release(s); return NULL;
}
void fastdl_http_shutdown(fastdl_http_t *s) {
    if(!s || s->joined) return;
    /* Read end stays open until after join, even if the worker exited early. */
    ssize_t n;
    do { n=write(s->wake[1],"x",1); } while(n<0 && errno==EINTR);
    pthread_join(s->thread,NULL);
    s->joined=1;
}
unsigned fastdl_http_drain_logs(fastdl_http_t *s,fastdl_http_log_fn sink,void *user,unsigned limit) {
    if(!s || !sink || !limit) return 0;
    unsigned count=0;
    unsigned dropped=atomic_exchange_explicit(&s->log_dropped,0,memory_order_relaxed);
    if(dropped) {
        char line[128]; snprintf(line,sizeof line,"http event=log_overflow dropped=%u (access log queue full)",dropped);
        sink(line,user); ++count;
    }
    while(count<limit) {
        unsigned r=atomic_load_explicit(&s->log_read,memory_order_relaxed);
        unsigned w=atomic_load_explicit(&s->log_write,memory_order_acquire);
        if(r==w) break;
        char line[768]; memcpy(line,s->logs[r%256],sizeof line);
        atomic_store_explicit(&s->log_read,r+1,memory_order_release);
        sink(line,user); ++count;
    }
    return count;
}
void fastdl_http_stop(fastdl_http_t *s) {
    if(!s) return;
    fastdl_http_shutdown(s);
    http_release(s);
}
