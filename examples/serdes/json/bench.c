/* json.h harness: throughput + zero-copy rate.
 * Prefer ./bench.sh; manual: gcc -O2 -I ../../../cc/include bench.c \
 *   ../../../cc/runtime/arena_state.c -o bench && ./bench twitter.json 400 5 */
#include "json.h"
#include <time.h>
static double now_s(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int c,char**a){const char*fn=c>1?a[1]:"twitter.json";int K=c>2?atoi(a[2]):200,T=c>3?atoi(a[3]):3;
  FILE*f=fopen(fn,"rb");if(!f){perror(fn);return 1;}fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
  char*b=malloc(n+1);size_t rd=fread(b,1,n,f);b[rd]=0;fclose(f);
  CCArena ar=cc_arena_create((size_t)n*24+(48<<20));   /* request arena, reset per parse */
  double best=1e18;long bor=0,mat=0;int ok=0;
  for(int t=0;t<T;t++){double t0=now_s();for(int i=0;i<K;i++){
      cc_arena_reset(&ar);
      JsonParser p=json_parser(b,n,&ar);JsonNode v;   /* p.parse(&v) == JsonParser_parse(&p,&v) */
      ok=JsonParser_parse(&p,&v)==JSON_OK;
      if(ok){bor=p.borrowed_strs;mat=p.materialized_strs;}
    }double dt=now_s()-t0;if(dt<best)best=dt;}
  cc_arena_free(&ar);
  if(!ok){fprintf(stderr,"%s: parse failed\n",fn);return 1;}
  double zc = bor+mat ? 100.0*bor/(bor+mat) : 100.0;
  printf("%-14s %6.0f MB/s  %7.1f us/parse   %.1f%% zero-copy (borrow=%ld mat=%ld)  node=%zuB\n",
    fn, n*(double)K/best/1e6, best/K*1e6, zc, bor, mat, sizeof(JsonNode));
  return 0;}
