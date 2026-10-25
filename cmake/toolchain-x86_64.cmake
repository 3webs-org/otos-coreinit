# Freestanding x86_64 Mach-O toolchain. Selected via:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-x86_64.cmake
#
# Only the compiler side lives here. The final link still goes through the
# clang driver (see the top-level CMakeLists.txt's target_link_options),
# with --ld-path selecting ld64.lld (LD64) over the system default, since
# this is a non-hosted Mach-O with no libSystem/CRT.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-18)
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
endif()

set(CMAKE_C_COMPILER_TARGET   x86_64-apple-darwin)
set(CMAKE_ASM_COMPILER_TARGET x86_64-apple-darwin)

# Required for a correct freestanding x86_64 image -- ABI facts, not
# optimization choices, so they live here rather than being left to the
# caller the way CMAKE_BUILD_TYPE is.
#
# -fpie: ld64.lld already marks the output Mach-O as PIE regardless of this
# flag, but that header bit means nothing on its own -- without -fpie the
# generated code still uses absolute addressing, so a byte-for-byte AP copy
# at a different address would silently read/call through BSP-relative
# addresses. This is what makes that copy actually safe.
set(CMAKE_C_FLAGS_INIT
    "-std=c11 -ffreestanding -fno-stack-protector -fpie -mno-red-zone -mno-sse -mno-mmx -Wall -Wextra")
