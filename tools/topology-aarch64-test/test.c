/* Host test for topology_aarch64.c's ACPI branch, run against the real
 * tables AAVMF publishes on QEMU virt (-smp 4), dumped from guest memory
 * 0x5F0B0000 (one page: RSDP at +0x18, XSDT, MADT, SPCR, ...). The dump's
 * physical pointers are rebased onto wherever the page is mapped here and
 * the two affected checksums recomputed; the tables themselves are
 * untouched. Expect 4 enabled cores (MPIDR 0..3), GICD 0x8000000, PL011
 * 0x9000000, backends wired, no DTB recorded. */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include "coreinit/topology.h"
void kprintf(const char *f, ...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);}
uint64_t gs, gi; void *gc;
void serial_aarch64_set_base(uint64_t b){gs=b;} void irq_aarch64_set_base(uint64_t b){gi=b;} void cross_core_signal_aarch64_set_base(void*b){gc=b;}
int fdt_valid(const void*b,uint64_t s){return 0;}
int main(int argc,char**argv){
  uint8_t *p=mmap(0,0x1000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
  FILE*f=fopen(argv[1],"rb"); if(!f||fread(p,1,0x1000,f)!=0x1000){puts("read");return 2;}
  uint64_t B=0x5F0B0000, N=(uint64_t)p; uint8_t *r=p+0x18;
  uint64_t x; memcpy(&x,r+24,8); x=x-B+N; memcpy(r+24,&x,8);
  uint8_t *xs=(uint8_t*)x; uint32_t l; memcpy(&l,xs+4,4);
  for(uint32_t i=0;i<(l-36)/8;i++){uint64_t e; memcpy(&e,xs+36+8*i,8); e=e-B+N; memcpy(xs+36+8*i,&e,8);}
  #define FIX(q,n,c) do{(q)[c]=0;uint8_t s_=0;for(size_t k=0;k<(n);k++)s_+=(q)[k];(q)[c]=(uint8_t)-s_;}while(0)
  FIX(xs,l,9); FIX(r,36,32);
  struct init_boot_info bi={0}; bi.early_rsdp_phys=(uint64_t)r;
  static topology_t t; int ok=topology_discover(&t, topology_boot_hint(&bi));
  int good = ok && t.count==4 && t.cores[3].target==3 && t.cores[0].enabled && gs==0x9000000 && gi==0x8000000 && gc==(void*)0x8000000 && t.arch_opaque[3]==0;
  printf("ok=%d count=%zu %s\n",ok,t.count,good?"HOSTTEST PASS":"HOSTTEST FAIL"); return !good;}
