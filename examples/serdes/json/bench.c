/* json.h harness: throughput + zero-copy rate.
 * Prefer ./bench.sh; manual: gcc -O2 -I ../../../cc/include bench.c \
 *   ../../../cc/runtime/arena_state.c -o bench && ./bench twitter.json 400 5
 * Pass -c to also verify a correctness checksum. The verification parse+walk
 * runs OUTSIDE the timed region, so -c never skews the reported timing. */
#include "json.h"
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
static double now_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}

static void out_fd(int fd, const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n) (void)write(fd, s, n);
}
static void outf(int fd, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) (void)write(fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

/* Deterministic DOM digest (untimed; -c only). Members are (key,value) pairs. */
static long sum(const JsonNode*n){long len=(long)JsonNode_len(n);switch(JsonNode_tag(n)){
  case JSON_NUM:case JSON_STR:return len+1;
  case JSON_ARR:{long s=len;for(JsonNode*c=n->u.head;c;c=c->next)s+=sum(c);return s;}
  case JSON_OBJ:{long s=len;for(JsonNode*k=n->u.head;k;){JsonNode*v=k->next;s+=(long)JsonNode_len(k)+sum(v);k=v->next;}return s;}
  default:return JsonNode_tag(n)+1;}}

int main(int argc,char**argv){
  const char*fn="twitter.json"; int K=200,T=3,check=0,pos=0;
  for(int i=1;i<argc;i++){
    if(!strcmp(argv[i],"-c")){check=1;continue;}
    if(pos==0)fn=argv[i]; else if(pos==1)K=atoi(argv[i]); else if(pos==2)T=atoi(argv[i]);
    pos++;
  }
  FILE*f=fopen(fn,"rb");if(!f){out_fd(STDERR_FILENO,fn);out_fd(STDERR_FILENO," open failed\n");return 1;}fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
  char*b=malloc(n+1);size_t rd=fread(b,1,n,f);b[rd]=0;fclose(f);
  CCArena ar=cc_arena_create((size_t)n*24+(48<<20));   /* request arena, reset per parse */
  double best=1e18;long bor=0,mat=0;int ok=0;
  for(int t=0;t<T;t++){double t0=now_s();for(int i=0;i<K;i++){
      cc_arena_reset(&ar);
      JsonParser p=json_parser(b,n,&ar);JsonNode v;   /* p.parse(&v) == JsonParser_parse(&p,&v) */
      ok=JsonParser_parse(&p,&v)==JSON_OK;
      if(ok){bor=p.borrowed_strs;mat=p.materialized_strs;}
    }double dt=now_s()-t0;if(dt<best)best=dt;}
  if(!ok){outf(STDERR_FILENO,"%s: parse failed\n",fn);return 1;}
  double zc = bor+mat ? 100.0*bor/(bor+mat) : 100.0;
  outf(STDOUT_FILENO,"%-14s %6.0f MB/s  %7.1f us/parse   %.1f%% zero-copy (borrow=%ld mat=%ld)  node=%zuB",
    fn, n*(double)K/best/1e6, best/K*1e6, zc, bor, mat, sizeof(JsonNode));
  if(check){                       /* one untimed parse + walk, after the clock stops */
    cc_arena_reset(&ar);
    JsonParser p=json_parser(b,n,&ar);JsonNode v;
    if(JsonParser_parse(&p,&v)!=JSON_OK){out_fd(STDOUT_FILENO,"\n");outf(STDERR_FILENO,"%s: verify parse failed\n",fn);return 1;}
    outf(STDOUT_FILENO,"  chk=%ld", sum(&v));
  }
  out_fd(STDOUT_FILENO,"\n");
  cc_arena_free(&ar);
  return 0;}
