/* yyjson comparison (default + insitu). Build via ./bench.sh -y, or:
 *   gcc -O2 yy.c yyjson.c -o yy */
#include "yyjson.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
int main(int argc,char**argv){
  const char*fn=argc>1?argv[1]:"twitter.json"; int K=argc>2?atoi(argv[2]):2000;
  FILE*f=fopen(fn,"rb");if(!f){out_fd(STDERR_FILENO,fn);out_fd(STDERR_FILENO," open failed\n");return 1;}fseek(f,0,SEEK_END);long n=ftell(f);fseek(f,0,SEEK_SET);
  char*b=malloc(n+64);size_t rd=fread(b,1,n,f);b[rd]=0;fclose(f);size_t len=n;double best;
  best=0;for(int t=0;t<3;t++){double t0=now_s();for(int i=0;i<K;i++){yyjson_doc*d=yyjson_read(b,len,0);yyjson_doc_free(d);}double dt=now_s()-t0;double mb=len*(double)K/dt/1e6;if(mb>best)best=mb;}
  outf(STDOUT_FILENO,"  yyjson default %6.0f MB/s  (copy strings + eager num)\n",best);
  char*ib=malloc(len+64);
  best=0;for(int t=0;t<3;t++){double t0=now_s();for(int i=0;i<K;i++){memcpy(ib,b,len);yyjson_doc*d=yyjson_read_opts(ib,len,YYJSON_READ_INSITU,NULL,NULL);yyjson_doc_free(d);}double dt=now_s()-t0;double mb=len*(double)K/dt/1e6;if(mb>best)best=mb;}
  outf(STDOUT_FILENO,"  yyjson insitu  %6.0f MB/s  (in-place, no str alloc; incl input memcpy)\n",best);
  return 0;}
