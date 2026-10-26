/* Host test for topology_aarch64.c's ACPI branch against AAVMF tables
 * dumped from QEMU virt (-smp 4, GICv2 and GICv3). The one-page fixtures
 * cover RSDP at +0x18, XSDT, MADT, FADT and SPCR. Rebase the XSDT pointers
 * and recompute their checksums, then vary FADT PSCI flags and recompute
 * its checksum. Expect four cores, GICD, GICC or GICR, and PL011. */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/mman.h>
#include <string.h>
#include "coreinit/topology.h"
void kprintf(const char *f, ...){va_list a;va_start(a,f);vprintf(f,a);va_end(a);}
uint64_t gs, gi, gicc, gicr, gicr_size; void *gc;
void serial_aarch64_set_base(uint64_t b){gs=b;} void irq_aarch64_set_base(uint64_t b){gi=b;} void irq_aarch64_set_cpu_base(uint64_t b){gicc=b;} void irq_aarch64_set_redistributor_region(uint64_t b,uint64_t n){gicr=b;gicr_size=n;} void cross_core_signal_aarch64_set_base(void*b){gc=b;} void cross_core_signal_aarch64_set_gic_v2(void){}
void serial_aarch64_set_cadence_base(uint64_t b){gs=b;}
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
  int v3=argc>2 && argv[2][0]=='3';
  int good = ok && t.count==4 && t.cores[3].target==3 && t.cores[0].enabled && gs==0x9000000 && gi==0x8000000 && gc==(void*)0x8000000 && t.arch_opaque[3]==0;
  good &= v3 ? (gicr==0x80a0000 && gicr_size>=0x80000 && !gicc) : (gicc==0x8010000 && !gicr);
  int use_hvc = -1;
  good &= topology_aarch64_psci_method((uint64_t)r, &use_hvc) && use_hvc == 1;
  int fadt_found=0;
  for (uint32_t i=0;i<(l-36)/8;i++) {
    uint64_t addr;
    memcpy(&addr,xs+36+8*i,8);
    uint8_t *fadt=(uint8_t*)(uintptr_t)addr;
    if (memcmp(fadt,"FACP",4)) continue;
    fadt_found=1;
    uint32_t len=*(uint32_t*)(fadt+4);
    uint8_t original=fadt[129];
    fadt[129]=0; FIX(fadt,len,9);
    good &= !topology_aarch64_psci_method((uint64_t)r,&use_hvc);
    fadt[129]=1; FIX(fadt,len,9);
    good &= topology_aarch64_psci_method((uint64_t)r,&use_hvc) && use_hvc==0;
    fadt[129]=original; FIX(fadt,len,9);
    break;
  }
  good &= fadt_found;
  printf("ok=%d count=%zu %s\n",ok,t.count,good?"HOSTTEST PASS":"HOSTTEST FAIL"); return !good;}
