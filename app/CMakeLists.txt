# vita-turbo-vpn LiveArea app, title VPNT00001.
#
# Build with ./build.sh (pinned VitaSDK Docker image), or with VitaSDK
# installed: cmake -S . -B build && make -C build
#
#   -DVPNTURBO_BACKEND=mock   scripted scenarios, Select cycles them (default)
#   -DVPNTURBO_BACKEND=vt     the real back end: the kernel module's control API
#   -DVPNTURBO_RELEASE=ON     a release: the real back end only; configuring or
#                         compiling with the mock fails (it ships with an
#                         empty profile list). ./build.sh release
#   -DVPNTURBO_BRAND=A|B|C    how far the brand red reaches (src/ui.h); A default

cmake_minimum_required(VERSION 3.12)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK or pass -DCMAKE_TOOLCHAIN_FILE")
  endif()
endif()

project(vpnturbo_app C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

option(VPNTURBO_RELEASE "Release build: real back end only" OFF)
if(VPNTURBO_RELEASE)
  set(VPNTURBO_BACKEND "vt" CACHE STRING "Back end: mock or vt")
else()
  set(VPNTURBO_BACKEND "mock" CACHE STRING "Back end: mock or vt")
endif()
if(VPNTURBO_RELEASE AND NOT VPNTURBO_BACKEND STREQUAL "vt")
  message(FATAL_ERROR "A release cannot include the mock back end (VPNTURBO_BACKEND=${VPNTURBO_BACKEND}); use VPNTURBO_BACKEND=vt")
endif()
set(VPNTURBO_BRAND "A" CACHE STRING "Brand colour reach: none, A, B or C")
if(VPNTURBO_BRAND STREQUAL "none")
  set(BRAND_LEVEL 0)
elseif(VPNTURBO_BRAND STREQUAL "A")
  set(BRAND_LEVEL 1)
elseif(VPNTURBO_BRAND STREQUAL "B")
  set(BRAND_LEVEL 2)
elseif(VPNTURBO_BRAND STREQUAL "C")
  set(BRAND_LEVEL 3)
else()
  message(FATAL_ERROR "VPNTURBO_BRAND must be none, A, B or C")
endif()
set(VITA_APP_NAME "Vita Turbo VPN") # interim name
set(VITA_TITLEID  "VPNT00001")
set(VITA_VERSION  "00.10")
set(FONTS "${CMAKE_SOURCE_DIR}/../third_party/fonts")

set(CMAKE_C_STANDARD 99)
set(QUIRC ${CMAKE_SOURCE_DIR}/../third_party/quirc/lib)
set(MINIZ ${CMAKE_SOURCE_DIR}/third_party/miniz)
set(CONF ${CMAKE_SOURCE_DIR}/../conf)

# quirc (ISC), vendored unmodified, plus the relaxed grouping build of
# identify.c (../qr/quirc_relaxed.h) generated from it here, as the host build does.
file(READ ${QUIRC}/identify.c IDENTIFY)
string(FIND "${IDENTIFY}" "squareness < (quirc_float_t)0.2)" AT)
if(AT EQUAL -1)
  message(FATAL_ERROR "quirc identify.c changed: update the relaxed patch")
endif()
string(REPLACE "squareness < (quirc_float_t)0.2)" "squareness < (quirc_float_t)0.6)"
  IDENTIFY "${IDENTIFY}")
file(WRITE ${CMAKE_BINARY_DIR}/identify_relaxed.c "${IDENTIFY}")
set(QUIRC_SRC ${QUIRC}/decode.c ${QUIRC}/identify.c ${QUIRC}/quirc.c ${QUIRC}/version_db.c)
set(RELAXED_SRC ${CMAKE_BINARY_DIR}/identify_relaxed.c)
# Third-party code as it comes: only its known benign warnings are off.
set_source_files_properties(${QUIRC_SRC} ${RELAXED_SRC} PROPERTIES
  COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter;-Wno-type-limits")
set_source_files_properties(${RELAXED_SRC} PROPERTIES COMPILE_DEFINITIONS
  "quirc_begin=quirc_begin_relaxed;quirc_end=quirc_end_relaxed;quirc_extract=quirc_extract_relaxed")
# miniz (MIT): reading .zip bundles in memory only
set_source_files_properties(${MINIZ}/miniz.c PROPERTIES COMPILE_OPTIONS "-w")
add_compile_definitions(MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_DEFLATE_APIS
  MINIZ_NO_ZLIB_COMPATIBLE_NAMES BRAND_LEVEL=${BRAND_LEVEL})
if(VPNTURBO_RELEASE)
  add_compile_definitions(VPNTURBO_RELEASE)
endif()

add_compile_options(-Wall -Wextra -O2)
include_directories(src third_party/stb ${MINIZ} ${CONF} ${QUIRC} ${CMAKE_SOURCE_DIR}/../qr)

add_executable(vpnturbo_app
  src/main_vita.c
  src/app.c
  src/screens.c
  src/ui.c
  src/draw.c
  src/draw_vita.c
  src/strings.c
  src/stb_impl.c
  src/profiles.c
  src/qr.c
  src/camera_vita.c
  src/backend_${VPNTURBO_BACKEND}.c
  ${CONF}/vtconf.c
  ${CONF}/vtconf_util.c
  ${QUIRC_SRC}
  ${RELAXED_SRC}
  ${MINIZ}/miniz.c
)
target_link_libraries(vpnturbo_app
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceAppUtil_stub
  SceAppMgr_stub
  SceCommonDialog_stub
  ScePvf_stub
  SceCamera_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
  m
)

# The real back end calls the kernel module's syscall library
# (SceVpnTurbo). Its link stub is made here from the module's own yml:
# a stand-in ELF with one empty function per exported name (NIDs come from
# the names), as the SceShell plugin's kstub does, so the app builds without
# building the kernel module first. The import is weak: without the module
# the app still starts, and backend_vt.c finds the module by name before it
# makes any call.
if(VPNTURBO_BACKEND STREQUAL "vt")
  set(VT_KERNEL_YML ${CMAKE_SOURCE_DIR}/../vpnturbo_kernel.yml)
  file(STRINGS ${VT_KERNEL_YML} VT_EXPORTS REGEX "^ +- +[A-Za-z_][A-Za-z0-9_]*")
  set(VT_KSTUB_C "/* generated from vpnturbo_kernel.yml; never installed */\n")
  string(APPEND VT_KSTUB_C "int module_start(void) { return 0; }\n")
  string(APPEND VT_KSTUB_C "int module_stop(void) { return 0; }\n")
  string(APPEND VT_KSTUB_C "int _start(void) __attribute__((weak, alias(\"module_start\")));\n")
  foreach(line ${VT_EXPORTS})
    string(REGEX REPLACE "^ +- +([A-Za-z_][A-Za-z0-9_]*).*" "\\1" fn "${line}")
    string(APPEND VT_KSTUB_C "int ${fn}(void) { return -1; }\n")
  endforeach()
  file(WRITE ${CMAKE_BINARY_DIR}/vt_kstub.c "${VT_KSTUB_C}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${VT_KERNEL_YML})
  add_executable(vt_kstub ${CMAKE_BINARY_DIR}/vt_kstub.c)
  target_link_options(vt_kstub PRIVATE -nostartfiles -nostdlib)
  vita_create_stubs(vt-kstubs vt_kstub ${VT_KERNEL_YML} KERNEL)
  add_dependencies(vpnturbo_app vt-kstubs)
  # The exit-IP check: mbedTLS from the VitaSDK, and
  # Mozilla's root bundle built in with .incbin (src/ca_bundle.c)
  set(CA_PEM ${CMAKE_SOURCE_DIR}/../third_party/ca-bundle/cacert.pem)
  target_sources(vpnturbo_app PRIVATE src/https_get.c src/ca_bundle.c)
  set_source_files_properties(src/ca_bundle.c PROPERTIES
    COMPILE_DEFINITIONS "CA_PEM=\"${CA_PEM}\""
    OBJECT_DEPENDS ${CA_PEM})
  target_link_libraries(vpnturbo_app
    mbedtls mbedx509 mbedcrypto pthread
    ${CMAKE_BINARY_DIR}/libvpnturbo_kernel_stub_weak.a
    SceVshBridge_stub         # finds the kernel module by name
    SceKernelThreadMgr_stub   # the connect and check workers
    SceIofilemgr_stub
  )
endif()

vita_create_self(eboot.bin vpnturbo_app UNSAFE)
vita_create_vpk(vpnturbo.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME "${VITA_APP_NAME}"
  FILE sce_sys/icon0.png sce_sys/icon0.png
  FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ${FONTS}/chakra-petch/ChakraPetch-SemiBold.ttf fonts/ChakraPetch-SemiBold.ttf
  FILE ${FONTS}/chakra-petch/ChakraPetch-Bold.ttf fonts/ChakraPetch-Bold.ttf
  FILE ${FONTS}/barlow-semi-condensed/BarlowSemiCondensed-Regular.ttf fonts/BarlowSemiCondensed-Regular.ttf
  FILE ${FONTS}/martian-mono/MartianMono-StdRg.ttf fonts/MartianMono-StdRg.ttf
  FILE ${FONTS}/martian-mono/MartianMono-StdMd.ttf fonts/MartianMono-StdMd.ttf
  FILE ${FONTS}/chakra-petch/OFL.txt fonts/OFL-ChakraPetch.txt
  FILE ${FONTS}/barlow-semi-condensed/OFL.txt fonts/OFL-BarlowSemiCondensed.txt
  FILE ${FONTS}/martian-mono/OFL.txt fonts/OFL-MartianMono.txt
  FILE ${QUIRC}/../LICENSE licenses/quirc-LICENSE.txt
  FILE ${MINIZ}/LICENSE licenses/miniz-LICENSE.txt
  FILE third_party/stb/LICENSE licenses/stb-LICENSE.txt
  FILE third_party/mbedtls/LICENSE licenses/mbedtls-LICENSE.txt
)
