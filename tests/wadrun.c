#include <stdio.h>
#include "wad_extract.h"
static void prog(void *ud,unsigned d,unsigned t,unsigned long long bd,unsigned long long bt,const char *n){
    (void)ud;(void)t;(void)bd;(void)bt; printf("  [%u] %s\n", d, n);
}
int main(int argc,char**argv){
    if(argc<4){fprintf(stderr,"usage: wadrun ARCHIVE DICT OUTDIR\n");return 2;}
    char err[512]; wad_opts o={prog,0};
    long long b = wad_payload_bytes(argv[1], err, sizeof err);
    printf("payload bytes: %lld\n", b);
    long n = wad_extract(argv[1],argv[2],argv[3],&o,err,sizeof err);
    if(n<0){fprintf(stderr,"ERROR: %s\n",err);return 1;}
    printf("wrote %ld files\n",n); return 0;
}
