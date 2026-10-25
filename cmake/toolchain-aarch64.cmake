# Freestanding aarch64 Mach-O toolchain. Selected via:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64.cmake
#
# Only the compiler side lives here. The final link runs ld64.lld directly
# (see LD64 in the top-level CMakeLists.txt) rather than through the
# compiler driver, since this is a non-hosted Mach-O with no libSystem/CRT.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang-18)
endif()
if(NOT CMAKE_ASM_COMPILER)
    set(CMAKE_ASM_COMPILER "${CMAKE_C_COMPILER}")
endif()

set(CMAKE_C_COMPILER_TARGET   aarch64-apple-darwin)
set(CMAKE_ASM_COMPILER_TARGET aarch64-apple-darwin)

# Required for a correct freestanding aarch64 image -- ABI facts, not
# optimization choices, so they live here rather than being left to the
# caller the way CMAKE_BUILD_TYPE is.
set(CMAKE_C_FLAGS_INIT
    "-mcpu=generic -march=armv8-a -mgeneral-regs-only -std=c11 -ffreestanding -fno-stack-protector -Wall -Wextra")
