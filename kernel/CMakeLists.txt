# vpnturbo_kernel.skprx - the vita-turbo-vpn kernel module.
#
#   ./build.sh        builds the protocol core, then this module, in the
#                     pinned VitaSDK Docker image
# or with VitaSDK installed and build/vita/libtncore.a already made:
#   cmake -S . -B build && make -C build

cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
  endif()
endif()

project(vpnturbo_kernel C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_STANDARD 99)
# -Wframe-larger-than: the lent steps and the control calls run in syscalls,
# on the caller's small kernel stack (the Lockdown connect crash):
# a frame over 1 KB is a warning, and
# the build must have none.
add_compile_options(-Wall -Wextra -Wno-unused-parameter -O2 -ffunction-sections -fdata-sections
  -Wframe-larger-than=1024)

set(ROOT  ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE  ${ROOT}/core)
set(OBSD  ${ROOT}/third_party/openbsd/sys)
set(CORELIB ${CORE}/build/vita/libtncore.a)   # built first by build.sh

add_executable(vpnturbo_kernel
  module.c
  klog.c
  iface.c
  tcp_rst.c
  strand.c                        # reset connections stranded when the tunnel goes away
  strand_core.c
  strand_fw.c
  strand_hook.c
  transport.c
  routes.c
  restore.c
  control.c
  suspend.c
  latency.c
  proto.c
  dns.c                           # DNS override
  overlay.c                       # framebuffer hook: padlock + HUD
  hud.c                           # HUD text, copied from Wi-Fi Turbo
  hud_i18n.c
)

# proto.c is the only file that talks to the OpenBSD core, so it alone takes
# the core's build flags (shim force-included, BSD-kernel name remapping). The
# other files keep the Vita's own net headers unmolested.
option(TN_CRYPTO_BENCH "log a crypto self-test and benchmark at module start" OFF)
set(PROTO_FLAGS "-include ${CORE}/include/tn_shim.h -I${CORE}/include -I${CORE}/shim/include -I${OBSD} -DTN_SHIM_VITA")
if(TN_CRYPTO_BENCH)
  set(PROTO_FLAGS "${PROTO_FLAGS} -DTN_CRYPTO_BENCH")
endif()
set_source_files_properties(proto.c PROPERTIES COMPILE_FLAGS "${PROTO_FLAGS}")

# The hook runs inside the network stack's own calls, in whatever thread makes
# them: its object and the frame test it calls use general registers only and
# never call a library routine (the copies are loops the compiler may not turn
# into memcpy). tests/check_strand.sh checks both object files after the link.
set_source_files_properties(strand_core.c strand_hook.c PROPERTIES
  COMPILE_FLAGS "-mgeneral-regs-only -fno-tree-loop-distribute-patterns")

target_link_libraries(vpnturbo_kernel
  ${CORELIB}                      # noise/cookie/crypto + the Vita shim back end
  taihenForKernel_stub
  taihenModuleUtils_stub          # module_get_export_func
  SceSysmemForDriver_stub         # lent/route buffers, memblocks
  SceSysclibForDriver_stub        # memcpy/memset/memcmp, 64-bit division
  SceThreadmgrForDriver_stub      # threads, event flags, mutexes, system time
  SceModulemgrForDriver_stub      # ksceKernelSearchModuleByName (attach resolve)
  SceNetPsForDriver_stub          # kernel sockets (transport, routes)
  SceIofilemgrForDriver_stub      # kernel.log and the state file
  SceKernelSuspendForDriver_stub  # sysevent handler
  SceProcessmgrForDriver_stub     # caller pid
  SceSysrootForKernel_stub        # caller title id
  SceSblSsMgrForDriver_stub       # core RNG
  SceRtcForDriver_stub            # core wall-clock (TAI64N)
  SceDebugForDriver_stub          # core panic path
  ScePowerForDriver_stub          # restart after a faster-sending change
)
# -Tdata: vita-elf-create appends ~1.5 KB of module info to the end of the text
# segment. With data on the very next page, that fails ("segment 1 overlaps",
# then a segfault) whenever the code happens to end in the last ~1.5 KB of a
# page, as it did here at 0xfc68. A fixed data address leaves room regardless;
# segments are relocated independently at load (same fix as Wi-Fi Turbo's
# plugin/wifi/CMakeLists.txt).
target_link_options(vpnturbo_kernel PRIVATE -nostartfiles -Wl,-Tdata=0x81100000)

add_custom_command(TARGET vpnturbo_kernel POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E env OBJDUMP=${CMAKE_OBJDUMP}
          sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_strand.sh
          ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/vpnturbo_kernel.dir/strand_core.c.obj
          ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/vpnturbo_kernel.dir/strand_hook.c.obj
  COMMENT "checking the stranded-connection hook object")

vita_create_self(vpnturbo_kernel.skprx vpnturbo_kernel
  CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/../vpnturbo_kernel.yml UNSAFE)

# The user-side link stub for the SceVpnTurbo syscall library, so the
# app and the SceShell plugin link against the real module's NIDs. The
# libraries are copied next to this file (build/kernel/), where the root
# CMakeLists.txt links them from.
vita_create_stubs(vpnturbo_kernel-stubs vpnturbo_kernel
  ${CMAKE_CURRENT_SOURCE_DIR}/../vpnturbo_kernel.yml KERNEL)
