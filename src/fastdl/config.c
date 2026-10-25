#include <fastdl/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
void fastdl_config_defaults(fastdl_config_t *c) {
    memset(c, 0, sizeof *c);
    strcpy(c->bind, "127.0.0.1"); strcpy(c->root, "{game}");
    strcpy(c->allow_dirs, "maps models sound sprites gfx overviews");
    strcpy(c->allow_extensions, "bsp mdl wav mp3 spr tga bmp txt res wad");
    c->port=17015; c->max_clients=64; c->per_ip=4; c->requests_per_second=20;
    c->timeout_seconds=15; c->cpu=-1; c->bytes_per_second=8*1024*1024;
    c->access_log=1;
    c->max_file_bytes=128*1024*1024;
}
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) ++s;
    char *e=s+strlen(s); while(e>s && isspace((unsigned char)e[-1])) --e;
    *e=0; return s;
}
int fastdl_config_load(fastdl_config_t *c, const char *path, char *error, unsigned size) {
    FILE *f=fopen(path,"r"); unsigned line=0; char buf[8192];
    if(!f) { snprintf(error,size,"cannot open config '%s': %s",path,strerror(errno)); return 0; }
    while(fgets(buf,sizeof buf,f)) {
        ++line;
        if(!strchr(buf,'\n') && !feof(f)) goto bad;
        char *key=trim(buf); if(!*key || *key=='#' || *key==';') continue;
        char *value=strchr(key,'='); if(!value) goto bad;
        *value++=0; key=trim(key); value=trim(value);
        if(!strcmp(key,"source")) {
            if(!*value || c->source_count>=16 || strlen(value)>=sizeof c->sources[0]) goto bad;
            strcpy(c->sources[c->source_count++],value); continue;
        }
#define STR(k) if(!strcmp(key,#k)) { if(strlen(value)>=sizeof c->k) goto bad; strcpy(c->k,value); continue; }
        STR(bind) STR(root) STR(allow_dirs) STR(allow_extensions) STR(deny_prefixes)
#undef STR
        char *end; errno=0; long n=strtol(value,&end,10);
        if(errno || end==value || *end) goto bad;
#define NUM(k,lo,hi) if(!strcmp(key,#k)) { if(n<(lo)||n>(hi)) goto bad; c->k=n; continue; }
        NUM(port,1,65535) NUM(max_clients,1,256) NUM(per_ip,1,64)
        NUM(requests_per_second,1,1000) NUM(timeout_seconds,1,120) NUM(cpu,-1,1023)
        NUM(access_log,0,1)
        NUM(bytes_per_second,1024,1073741824) NUM(max_file_bytes,1,1073741824)
#undef NUM
        goto bad;
    }
    if(ferror(f)) goto bad;
    fclose(f); if(!*c->root) strcpy(c->root,"{game}"); if(!*c->allow_extensions) { snprintf(error,size,"config '%s': allow_extensions cannot be empty",path); return 0; }
    if(!c->source_count) { strcpy(c->sources[0],c->root); c->source_count=1; }
    return 1;
bad:
    snprintf(error,size,"config '%s': invalid setting at line %u",path,line); fclose(f); return 0;
}
/* Placeholders are config-only substitutions, never expanded from URLs. */
int fastdl_config_expand(const char *source,const char *module,const char *game,char *out,unsigned size) {
    unsigned used=0;
    while(*source) {
        const char *value=NULL; unsigned skip=0;
        if(!strncmp(source,"{game}",6)) { value=game; skip=6; }
        else if(!strncmp(source,"{module}",8)) { value=module; skip=8; }
        else if(!strncmp(source,"{data}",6)) { value=module; skip=6; }
        else if(*source=='{' || *source=='}') return 0;
        if(value) {
            size_t n=strlen(value);
            if(n>=size-used) return 0;
            memcpy(out+used,value,n); used+=(unsigned)n;
            if(skip==6 && !strncmp(source,"{data}",6)) {
                if(5>=size-used) return 0;
                memcpy(out+used,"/data",5); used+=5;
            }
            source+=skip;
        } else {
            if(used+1>=size) return 0;
            out[used++]=*source++;
        }
    }
    if(used>=size) return 0;
    out[used]=0; return 1;
}
