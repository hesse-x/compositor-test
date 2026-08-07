cmake_minimum_required(VERSION 3.20)
project(compositor C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# wlroots (third_party/wlroots) — built with its native Meson build system,
# driven from CMake via custom commands.
# ---------------------------------------------------------------------------
set(WLROOTS_SRC   ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wlroots)
set(WLROOTS_BUILD ${CMAKE_CURRENT_BINARY_DIR}/wlroots)
set(WLROOTS_LIB   ${WLROOTS_BUILD}/libwlroots-0.18.so)

find_program(MESON_EXECUTABLE  meson  REQUIRED)
find_program(NINJA_EXECUTABLE  ninja  REQUIRED)
find_program(PYTHON3_EXECUTABLE python3 REQUIRED)
find_package(Vulkan 1.2.182 REQUIRED)

# Options passed to `meson setup` for wlroots.
# examples/tests are disabled because we compile tinywl.c ourselves.
# 嵌套合成器：只编 x11 后端，禁掉 drm/libinput 后端，因此
# 不依赖 libdrm / libinput / libudev（避免和系统包版本打架）。
# wayland 后端默认无条件编入（backend/meson.build 里 subdir('wayland')），
# 不受 backends 选项控制，所以嵌套到 Wayland 会话也支持。
# gbm allocator 保留（嵌套后端靠它分配 buffer）；libgbm 走 vendor 的 gbm.pc。
# Configure wlroots before every build so changed options cannot be hidden by
# Meson's existing cache. The helper also verifies the effective renderer list.
# 系统 wayland 1.22 < wlroots 0.18 要求的 1.23：把 wayland 1.23 源码放进
# wlroots 源码树的 subprojects/wayland（git submodule），meson 的 fallback
# 机制会自动发现并把它作为子项目源码编译，不碰系统包。
#
# libgbm 同理装不上（版本坑），但它在 mesa 里没法独立源码编译。这里用
# vendor 的 gbm.h + 一个 gbm.pc 指向系统 libgbm.so.1，让 wlroots 的
# dependency('gbm') 满足。通过 PKG_CONFIG_PATH 暴露给 meson。
set(GBM_PC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/gbm)
set(WLROOTS_CONFIGURE_HELPER ${CMAKE_CURRENT_SOURCE_DIR}/tools/configure-wlroots.sh)
add_custom_target(wlroots_configure
	COMMAND ${CMAKE_COMMAND} -E env
	        PKG_CONFIG_PATH=${GBM_PC_DIR}
	        ${WLROOTS_CONFIGURE_HELPER} ${WLROOTS_SRC} ${WLROOTS_BUILD}
	BYPRODUCTS ${WLROOTS_BUILD}/build.ninja
	DEPENDS ${WLROOTS_CONFIGURE_HELPER}
	        ${CMAKE_CURRENT_SOURCE_DIR}/wlroots-meson-options.txt
	COMMENT "Configuring wlroots with Meson"
	VERBATIM
)

# Build wlroots after the mandatory reconfiguration.
add_custom_target(wlroots_build ALL
	COMMAND ${NINJA_EXECUTABLE} -C ${WLROOTS_BUILD}
	BYPRODUCTS ${WLROOTS_LIB}
	COMMENT "Building wlroots"
	VERBATIM
)
add_dependencies(wlroots_build wlroots_configure)

# Expose the built library as an imported target.
# Meson generates version.h/config.h and the *-protocol.h headers into the
# build tree; create the directories now so CMake accepts them as include
# paths before wlroots builds.
file(MAKE_DIRECTORY ${WLROOTS_BUILD}/include ${WLROOTS_BUILD}/protocol)
add_library(wlroots SHARED IMPORTED GLOBAL)
set_target_properties(wlroots PROPERTIES
	IMPORTED_LOCATION ${WLROOTS_LIB}
	# Public headers live in the source tree; version.h/config.h and the
	# generated protocol headers live in the build tree.
	INTERFACE_INCLUDE_DIRECTORIES
		"${WLROOTS_SRC}/include;${WLROOTS_BUILD}/include;${WLROOTS_BUILD}/protocol"
	# wlroots 0.x headers require this opt-in define.
	INTERFACE_COMPILE_DEFINITIONS WLR_USE_UNSTABLE
)
add_dependencies(wlroots wlroots_build)

# ---------------------------------------------------------------------------
# Dependencies of tinywl itself
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON      REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN         REQUIRED IMPORTED_TARGET pixman-1)

# tinywl 只用到 drm_fourcc.h 里的 DRM_FORMAT_* 宏（纯头文件，无需链接 libdrm）。
# 系统 libdrm-dev 因运行库版本冲突装不上，但 libdrm2 运行库自带
# /usr/include/drm/drm_fourcc.h，直接用它即可。
if(NOT EXISTS /usr/include/drm/drm_fourcc.h)
	message(FATAL_ERROR "找不到 /usr/include/drm/drm_fourcc.h（需 libdrm2 运行库）")
endif()

# libpng：系统 libpng-dev 因运行库版本冲突装不上，改为源码子项目本地编译。
# 完全不碰系统包；产物在 build/libpng/，头文件和库都来自这里。
set(PNG_SHARED ON CACHE BOOL "" FORCE)
set(PNG_STATIC OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libpng ${CMAKE_CURRENT_BINARY_DIR}/libpng)

# ---------------------------------------------------------------------------
# tinywl（仓库内的定制版：在官方 tinywl 基础上加了桌面壁纸支持）
# ---------------------------------------------------------------------------
add_executable(tinywl ${CMAKE_CURRENT_SOURCE_DIR}/tinywl.c)
target_include_directories(tinywl PRIVATE
	/usr/include/drm
	${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpng
	${CMAKE_CURRENT_BINARY_DIR}/libpng)
target_link_libraries(tinywl PRIVATE
	wlroots
	PkgConfig::WAYLAND_SERVER
	PkgConfig::XKBCOMMON
	PkgConfig::PIXMAN
	Vulkan::Vulkan
	png_shared
)
# 让 tinywl 在构建目录里直接运行时能找到 libwlroots-0.18.so 和 libpng16.so
set_target_properties(tinywl PROPERTIES
	BUILD_RPATH "${WLROOTS_BUILD};${CMAKE_CURRENT_BINARY_DIR}/libpng")

# ---------------------------------------------------------------------------
# freetype：源码子项目本地编译，给 gles-term 做字形光栅化。
# 禁掉 harfbuzz/brotli/bzip2 避免引入新的系统包依赖；zlib 用系统的，
# PNG 复用上面本地编的 png target。
# ---------------------------------------------------------------------------
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI  ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2   ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HVF     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG     ON CACHE BOOL "" FORCE)
set(FT_REQUIRE_ZLIB    ON  CACHE BOOL "" FORCE)
add_subdirectory(third_party/freetype ${CMAKE_CURRENT_BINARY_DIR}/freetype)

# ---------------------------------------------------------------------------
# gles-term：GLES2 渲染的最小终端窗口（mac 风标题栏 + 半透明背景）。
# 渲染全部走 GLES（freetype 光栅化字形图集），不依赖 cairo。
# ---------------------------------------------------------------------------
find_program(WAYLAND_SCANNER_EXECUTABLE wayland-scanner REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_EGL    REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(EGL            REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLESV2         REQUIRED IMPORTED_TARGET glesv2)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

# 用 wayland-scanner 从 wayland-protocols 生成 xdg-shell 客户端协议代码
set(XDG_SHELL_XML ${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml)
set(XDG_SHELL_H   ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol.h)
set(XDG_SHELL_C   ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c)
set(XDG_DECORATION_XML ${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
set(XDG_DECORATION_H   ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-client-protocol.h)
set(XDG_DECORATION_C   ${CMAKE_CURRENT_BINARY_DIR}/xdg-decoration-protocol.c)
set(LAYER_SHELL_XML ${WLROOTS_SRC}/protocol/wlr-layer-shell-unstable-v1.xml)
set(LAYER_SHELL_H   ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-client-protocol.h)
set(LAYER_SHELL_C   ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-protocol.c)

add_custom_command(
	OUTPUT ${XDG_SHELL_H}
	COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${XDG_SHELL_XML} ${XDG_SHELL_H}
	DEPENDS ${XDG_SHELL_XML}
	VERBATIM
)
add_custom_command(
	OUTPUT ${XDG_SHELL_C}
	COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${XDG_SHELL_XML} ${XDG_SHELL_C}
	DEPENDS ${XDG_SHELL_XML}
	VERBATIM
)

add_custom_command(
	OUTPUT ${XDG_DECORATION_H}
	COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${XDG_DECORATION_XML} ${XDG_DECORATION_H}
	DEPENDS ${XDG_DECORATION_XML}
	VERBATIM
)
add_custom_command(
	OUTPUT ${XDG_DECORATION_C}
	COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${XDG_DECORATION_XML} ${XDG_DECORATION_C}
	DEPENDS ${XDG_DECORATION_XML}
	VERBATIM
)

add_custom_command(
	OUTPUT ${LAYER_SHELL_H}
	COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header ${LAYER_SHELL_XML} ${LAYER_SHELL_H}
	DEPENDS ${LAYER_SHELL_XML}
	VERBATIM
)
add_custom_command(
	OUTPUT ${LAYER_SHELL_C}
	COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code ${LAYER_SHELL_XML} ${LAYER_SHELL_C}
	DEPENDS ${LAYER_SHELL_XML}
	VERBATIM
)

add_executable(gles-term
	terminal/gles-term.c
	${XDG_SHELL_H}
	${XDG_SHELL_C}
	${XDG_DECORATION_H}
	${XDG_DECORATION_C}
)
target_include_directories(gles-term PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gles-term PRIVATE
	PkgConfig::WAYLAND_CLIENT
	PkgConfig::WAYLAND_EGL
	PkgConfig::EGL
	PkgConfig::GLESV2
	PkgConfig::XKBCOMMON
	freetype
	util  # forkpty
	m
)
# 运行时找得到本地编的 libfreetype.so
set_target_properties(gles-term PROPERTIES
	BUILD_RPATH ${CMAKE_CURRENT_BINARY_DIR}/freetype)

# desktop-shell owns wallpaper and Dock as independent layer-shell surfaces.
add_executable(desktop-shell
	desktop/desktop-shell.c
	${LAYER_SHELL_H}
	${LAYER_SHELL_C}
	${XDG_SHELL_C}
)
target_include_directories(desktop-shell PRIVATE
	${CMAKE_CURRENT_BINARY_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpng
	${CMAKE_CURRENT_BINARY_DIR}/libpng)
target_link_libraries(desktop-shell PRIVATE
	PkgConfig::WAYLAND_CLIENT
	png_shared)
set_target_properties(desktop-shell PROPERTIES
	BUILD_RPATH ${CMAKE_CURRENT_BINARY_DIR}/libpng)
