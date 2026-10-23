/* Compile with -D__aarch64__ or -D__x86_64__ to check that the shared
 * kernel/init region layout stays inside each platform's RAM convention. */
#include "../include/smp_layout.h"

#if defined(__aarch64__)
_Static_assert(KERNEL_LOAD_BASE == 0x41000000ull, "ARM kernel link address");
_Static_assert(BSP_INIT_LOAD_BASE == 0x40200000ull, "ARM init Mach-O vmaddr");
_Static_assert(BSP_MAILBOX_REGION_BASE + CORE_MAILBOX_OFFSET == 0x41800000ull,
               "ARM BSP mailbox within virt RAM");
_Static_assert(CORE_REGION_BASE >= 0x42000000ull, "AP regions in virt RAM");
_Static_assert(CORE_REGION_BASE + 64 * CORE_REGION_SIZE <= ALLOC_MIN,
               "AP regions cannot overlap the allocation pool");
_Static_assert(ALLOC_MIN < ALLOC_MAX && ALLOC_MAX <= 0x60000000ull,
               "pool stays inside QEMU virt's 512 MiB RAM");
#else
_Static_assert(KERNEL_LOAD_BASE == 0x1000000ull, "x86 kernel unchanged");
_Static_assert(BSP_INIT_LOAD_BASE == 0x1800000ull, "x86 init unchanged");
_Static_assert(CORE_REGION_BASE == 0x4000000ull, "x86 AP regions unchanged");
_Static_assert(ALLOC_MIN == 0x1C00000ull && ALLOC_MAX == CORE_REGION_BASE,
               "x86 allocation window unchanged");
#endif
