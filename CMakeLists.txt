# CMake build for Vita Turbo VPN (firmware 3.60-3.74, taiHEN/HENkaku).
#
# Three artifacts (the LiveArea app is a separate project in app/):
#   vpnturbo_kernel.skprx  kernel module (kernel/, core/): the tunnel
#                                interface, data path, routes, DNS, HUD, and
#                                the user-callable syscall library
#   vpnturbo.suprx               SceShell's module, listed under *main (user/)
#   vpnturbo_settings.suprx      the Settings page, listed under *NPXS10015
#                                (settings_app/): its own module, as in Wi-Fi
#                                Turbo and Turbo Storage, so SceShell never
#                                holds the page or its strings
#   vpnturbo_qm.suprx      quick-menu rows (quickmenu/), loaded by
#                                QuickMenuReborn from ur0:QuickMenuReborn/
#
# Build with `make` (VitaSDK if $VITASDK is set, otherwise the pinned Docker
# image). The protocol core (core/) is built first by the Makefile.
#   cmake -S . -B build [-DVT_CLIENT_STUB=ON] && cmake --build build

cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
  endif()
endif()

project(vitaturbovpn C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

option(VT_CLIENT_STUB "Scripted states instead of the kernel module" OFF)
# Quick-menu rows refresh while the menu stays open (quickmenu.c). Off until
# checked on the console: ./build.sh with VTW_QM_LIVE=ON in the environment.
option(VTW_QM_LIVE "Refresh the quick-menu rows while the menu is open" OFF)
# The version lives in include/vpnturbo.h (VT_VERSION_STR), once.
file(STRINGS ${CMAKE_SOURCE_DIR}/include/vpnturbo.h VTW_VERSION_LINE
     REGEX "^#define VT_VERSION_STR ")
string(REGEX REPLACE "^#define VT_VERSION_STR \"([^\"]+)\".*$" "\\1" VTW_VERSION "${VTW_VERSION_LINE}")
if(NOT VTW_VERSION)
  message(FATAL_ERROR "VT_VERSION_STR not found in include/vpnturbo.h")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/include/vpnturbo.h)

set(CMAKE_C_STANDARD 99)
add_compile_options(-Wall -Wextra -O2)
include_directories(include user settings_app quickmenu)

# The kernel module (kernel/CMakeLists.txt).
add_subdirectory(kernel)

# --------------------------------------------------------------------------
# Link stub for the kernel module's syscall library (SceVpnTurbo): generated
# by kernel/CMakeLists.txt (vpnturbo_kernel-stubs) from the module itself and
# its .yml, so the plugins import the NIDs the module really exports.
# --------------------------------------------------------------------------
# vita_create_stubs copies the libraries into the build directory of the
# CMakeLists that calls it (kernel/), so the path follows that call.
set(VT_KSTUB_DIR ${CMAKE_BINARY_DIR}/kernel)

# --------------------------------------------------------------------------
# QuickMenuReborn link stub, from the NIDs in quickmenu/qmr_imports.yml. Only the
# quick-menu module links it, strongly, as wifiturbo_qm does.
# --------------------------------------------------------------------------
set(QMR_STUB_DIR ${CMAKE_BINARY_DIR}/qmr_stubs)
add_custom_command(
  OUTPUT ${QMR_STUB_DIR}/libQuickMenuReborn_stub.a
  COMMAND ${CMAKE_COMMAND} -E make_directory ${QMR_STUB_DIR}
  COMMAND vita-libs-gen-2 -yml=${CMAKE_SOURCE_DIR}/quickmenu/qmr_imports.yml -output=${QMR_STUB_DIR}
  COMMAND $(MAKE) -s -C ${QMR_STUB_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/quickmenu/qmr_imports.yml
  COMMENT "Generating the QuickMenuReborn link stub")
add_custom_target(qmr-stubs DEPENDS ${QMR_STUB_DIR}/libQuickMenuReborn_stub.a)

# Two PAF main-thread calls from SceShell's libpaf (quickmenu/paf_task.h), for the
# live refresh only.
set(PAF_STUB_DIR ${CMAKE_BINARY_DIR}/paf_stubs)
add_custom_command(
  OUTPUT ${PAF_STUB_DIR}/libScePaf_stub.a
  COMMAND ${CMAKE_COMMAND} -E make_directory ${PAF_STUB_DIR}
  COMMAND vita-libs-gen-2 -yml=${CMAKE_SOURCE_DIR}/quickmenu/paf_imports.yml -output=${PAF_STUB_DIR}
  COMMAND $(MAKE) -s -C ${PAF_STUB_DIR}
  DEPENDS ${CMAKE_SOURCE_DIR}/quickmenu/paf_imports.yml
  COMMENT "Generating the ScePafCommon link stub")
add_custom_target(paf-stubs DEPENDS ${PAF_STUB_DIR}/libScePaf_stub.a)

# --------------------------------------------------------------------------
# The main plugin: vpnturbo.suprx (*main)
# --------------------------------------------------------------------------
#
# LINK RECIPE (from Wi-Fi Turbo): a STRONG import of a user
# library that is not loaded in the host process makes taiHEN drop the whole
# module silently. So every system library is a *_stub_weak import. Unlike
# Wi-Fi Turbo, the kernel module's library is weak too, so without the kernel
# module the Settings page stays and says so; the code checks the module is
# there before calling it. Only taihen is strong. Nothing here imports
# QuickMenuReborn.
set(VTW_SOURCES
  user/main.c
  user/shell.c
  user/preload.c                # parse + load the last profile (no connect)
  user/ifaddr.c                 # tunnel address and IFF_UP, set from SceShell
  user/resolve.c                # hostname endpoints, resolved once the network is up
  user/notify.c
  user/i18n_fmt.c               # language and formatting (no string table)
  user/log.c
  conf/vtconf.c                 # shared profile parser
  conf/vtconf_util.c
)
if(VT_CLIENT_STUB)
  set(VTW_CLIENT user/vt_client_stub.c)
else()
  set(VTW_CLIENT user/vt_client.c)
endif()
list(APPEND VTW_SOURCES ${VTW_CLIENT})

add_executable(vpnturbo ${VTW_SOURCES})

target_link_libraries(vpnturbo
  taihen_stub
  SceLibKernel_stub_weak        # sceKernelLoadStartModule, sceIo* helpers
  SceKernelThreadMgr_stub_weak  # threads, semaphores, system time
  SceKernelModulemgr_stub_weak
  SceSysmem_stub_weak           # lent-thread buffers
  SceIofilemgr_stub_weak        # log (and the stub's state file)
  SceProcessmgr_stub_weak       # sceKernelGetProcessTimeWide
  SceRegistryMgr_stub_weak      # system language
  SceAppUtil_stub_weak          # system language fallback
  SceSysmodule_stub_weak        # loads the notification library (SceShell)
  SceNotificationUtil_stub_weak # drop notification
  SceVshBridge_stub_weak        # finds the kernel module by name
  SceNetPs_stub_weak            # sceNetSyscallIoctl: the tunnel address (ifaddr.c)
  SceNet_stub_weak              # the system resolver (resolve.c)
  SceNetCtl_stub_weak           # network readiness after a wake (shell.c)
)
if(NOT VT_CLIENT_STUB)
  add_dependencies(vpnturbo vpnturbo_kernel-stubs)
  target_link_libraries(vpnturbo ${VT_KSTUB_DIR}/libvpnturbo_kernel_stub_weak.a)
endif()

# -Tdata: vita-elf-create appends module info to the end of the text segment;
# a fixed data address keeps it from overlapping data (Wi-Fi Turbo's fix).
target_link_options(vpnturbo PRIVATE -nostartfiles -Wl,-Tdata=0x81100000)

vita_create_self(vpnturbo.suprx vpnturbo
  CONFIG ${CMAKE_SOURCE_DIR}/vpnturbo.yml
  UNSAFE)

# --------------------------------------------------------------------------
# The Settings page: vpnturbo_settings.suprx (*NPXS10015 only)
# --------------------------------------------------------------------------
# Kept out of vpnturbo.suprx so SceShell never loads the page's code or its
# strings (Wi-Fi Turbo 0.7.0, Turbo Storage 0.4.0). Weak imports throughout,
# as above: without the kernel module the page stays and says so.
add_executable(vpnturbo_settings
  settings_app/settings_main.c
  settings_app/settings.c
  user/status.c
  settings_app/i18n.c
  user/i18n_fmt.c
  user/log.c
  ${VTW_CLIENT}
)
target_link_libraries(vpnturbo_settings
  taihen_stub
  SceLibKernel_stub_weak
  SceKernelThreadMgr_stub_weak
  SceKernelModulemgr_stub_weak
  SceIofilemgr_stub_weak        # log (and the test build's state file)
  SceProcessmgr_stub_weak
  SceRegistryMgr_stub_weak      # system language
  SceAppUtil_stub_weak
  SceVshBridge_stub_weak        # finds the kernel module by name
)
if(NOT VT_CLIENT_STUB)
  add_dependencies(vpnturbo_settings vpnturbo_kernel-stubs)
  target_link_libraries(vpnturbo_settings ${VT_KSTUB_DIR}/libvpnturbo_kernel_stub_weak.a)
endif()
target_link_options(vpnturbo_settings PRIVATE -nostartfiles -Wl,-Tdata=0x81100000)
target_compile_options(vpnturbo_settings PRIVATE -Os)

vita_create_self(vpnturbo_settings.suprx vpnturbo_settings
  CONFIG ${CMAKE_SOURCE_DIR}/vpnturbo_settings.yml
  UNSAFE)

# --------------------------------------------------------------------------
# Quick-menu module: vpnturbo_qm.suprx (a QuickMenuReborn client)
# --------------------------------------------------------------------------
#
# Dropped into ux0:QuickMenuReborn/ or ur0:QuickMenuReborn/, not config.txt:
# QuickMenuReborn loads it into SceShell after its own boot delay. Built as
# Wi-Fi Turbo's wifiturbo_qm.suprx: QuickMenuReborn and the kernel module's
# library are STRONG imports (without either, the module does not load and
# the quick menu shows nothing extra); system libraries are weak.
add_executable(vpnturbo_qm
  quickmenu/quickmenu.c
  user/status.c
  settings_app/i18n.c
  user/i18n_fmt.c
  user/log.c
  ${VTW_CLIENT}
)
add_dependencies(vpnturbo_qm qmr-stubs)
target_link_libraries(vpnturbo_qm
  ${QMR_STUB_DIR}/libQuickMenuReborn_stub.a
  SceLibKernel_stub_weak
  SceKernelThreadMgr_stub_weak  # system time (test build)
  SceIofilemgr_stub_weak        # log (and the test build's state file)
  SceProcessmgr_stub_weak
  SceRegistryMgr_stub_weak      # system language
  SceAppUtil_stub_weak
  SceVshBridge_stub_weak        # finds the kernel module by name
)
if(VTW_QM_LIVE)
  # Strong: libpaf is SceShell's own UI framework, always loaded there, and
  # QuickMenuReborn (required anyway) imports the same library.
  add_dependencies(vpnturbo_qm paf-stubs)
  target_compile_definitions(vpnturbo_qm PRIVATE VTW_QM_LIVE=1)
  target_link_libraries(vpnturbo_qm
    ${PAF_STUB_DIR}/libScePaf_stub.a
    SceAppMgr_stub_weak         # _sceAppMgrGetAppState: is the overlay shown
  )
endif()
if(NOT VT_CLIENT_STUB)
  add_dependencies(vpnturbo_qm vpnturbo_kernel-stubs)
  target_link_libraries(vpnturbo_qm ${VT_KSTUB_DIR}/libvpnturbo_kernel_stub.a)
endif()
target_link_options(vpnturbo_qm PRIVATE -nostartfiles -Wl,-Tdata=0x81100000)
target_compile_options(vpnturbo_qm PRIVATE -Os)

# Only the quick menu's own strings from the translation table.
target_compile_definitions(vpnturbo_qm PRIVATE VTW_I18N_QM_ONLY)

vita_create_self(vpnturbo_qm.suprx vpnturbo_qm
  CONFIG ${CMAKE_SOURCE_DIR}/vpnturbo_qm.yml
  UNSAFE)
