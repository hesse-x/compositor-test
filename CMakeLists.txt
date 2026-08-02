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

# Options passed to `meson setup` for wlroots.
# examples/tests are disabled because we compile tinywl.c ourselves.
set(WLROOTS_MESON_OPTIONS
	-Dexamples=false
	-Dxwayland=disabled
	CACHE STRING "Extra options for wlroots meson setup")

# Configure wlroots (runs once; delete the wlroots build dir to reconfigure).
add_custom_command(
	OUTPUT ${WLROOTS_BUILD}/build.ninja
	COMMAND ${MESON_EXECUTABLE} setup ${WLROOTS_BUILD} ${WLROOTS_SRC}
	        ${WLROOTS_MESON_OPTIONS}
	COMMENT "Configuring wlroots with Meson"
	VERBATIM
)

# Build wlroots.
add_custom_command(
	OUTPUT ${WLROOTS_LIB}
	COMMAND ${NINJA_EXECUTABLE} -C ${WLROOTS_BUILD}
	DEPENDS ${WLROOTS_BUILD}/build.ninja
	COMMENT "Building wlroots"
	VERBATIM
)

add_custom_target(wlroots_build ALL DEPENDS ${WLROOTS_LIB})

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
pkg_check_modules(CAIRO          REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(LIBDRM         REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(LIBPNG         REQUIRED IMPORTED_TARGET libpng)

# ---------------------------------------------------------------------------
# tinywl（仓库内的定制版：在官方 tinywl 基础上加了桌面壁纸支持）
# ---------------------------------------------------------------------------
add_executable(tinywl ${CMAKE_CURRENT_SOURCE_DIR}/tinywl.c)
target_link_libraries(tinywl PRIVATE
	wlroots
	PkgConfig::WAYLAND_SERVER
	PkgConfig::XKBCOMMON
	PkgConfig::PIXMAN
	PkgConfig::LIBDRM
	PkgConfig::LIBPNG
)
# 让 tinywl 在构建目录里直接运行时能找到 libwlroots-0.18.so
set_target_properties(tinywl PROPERTIES BUILD_RPATH ${WLROOTS_BUILD})

# ---------------------------------------------------------------------------
# gles-term：GLES 渲染的最小终端窗口（mac 风标题栏 + 半透明背景）
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

add_executable(gles-term
	terminal/gles-term.c
	${XDG_SHELL_H}
	${XDG_SHELL_C}
)
target_include_directories(gles-term PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(gles-term PRIVATE
	PkgConfig::WAYLAND_CLIENT
	PkgConfig::WAYLAND_EGL
	PkgConfig::EGL
	PkgConfig::GLESV2
	PkgConfig::XKBCOMMON
	PkgConfig::CAIRO
	util  # forkpty
	m
)
