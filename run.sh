#!/usr/bin/env bash
# 运行 tinywl。
#   ./run.sh            自动检测环境运行，并启动桌面 shell
#   ./run.sh -s foot    参数原样传给 tinywl
set -euo pipefail

cd "$(dirname "$0")"

TINYWL=build/tinywl

if [[ ${WLR_RENDERER:-vulkan} != vulkan ]]; then
	echo "错误: renderer policy violation: expected vulkan" >&2
	exit 1
fi
export WLR_RENDERER=vulkan
if [[ -z ${WLR_BACKENDS:-} ]]; then
	if [[ -n ${WAYLAND_DISPLAY:-} ]]; then
		export WLR_BACKENDS=wayland
	elif [[ -n ${DISPLAY:-} ]]; then
		export WLR_BACKENDS=x11
	else
		echo "错误: 当前构建仅支持 Wayland/X11 嵌套后端。" >&2
		exit 1
	fi
fi
export VK_LOADER_DEBUG=${VK_LOADER_DEBUG:-error,warn}
if [[ ${TINYWL_VALIDATION:-0} == 1 ]]; then
	export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
fi

if [[ ! -x $TINYWL ]]; then
	echo ">> tinywl 还没编译，先构建"
	./build.sh
fi

# --- 参数处理：没传参数时默认 -s <程序> -----------------------------------
if (($# == 0)); then
	if [[ -x build/desktop-shell ]]; then
		# desktop-shell 提供壁纸、Dock，并由 Dock 启动终端。
		export TINYWL_TERMINAL="$PWD/build/gles-term"
		set -- -s "$PWD/build/desktop-shell"
	else
		for term in foot weston-terminal alacritty kitty wezterm gnome-terminal deepin-terminal xterm x-terminal-emulator; do
			if command -v "$term" >/dev/null 2>&1; then
				set -- -s "$term"
				break
			fi
		done
	fi
fi

# --- 运行 -----------------------------------------------------------------
if [[ -n ${WAYLAND_DISPLAY:-} || -n ${DISPLAY:-} ]]; then
	# 已有图形会话：嵌套窗口模式运行（Wayland 优先，否则走 X11 后端）
	echo ">> 嵌套模式运行: $TINYWL $*"
	exec "$TINYWL" "$@"
else
	# 纯 TTY：直接用 DRM/KMS 后端
	if [[ -z ${XDG_RUNTIME_DIR:-} ]]; then
		export XDG_RUNTIME_DIR=/run/user/$(id -u)
	fi
	if [[ ! -d $XDG_RUNTIME_DIR ]]; then
		echo "错误: XDG_RUNTIME_DIR ($XDG_RUNTIME_DIR) 不存在。" >&2
		echo "请用 systemd-logind 会话登录 TTY，或启动 seatd。" >&2
		exit 1
	fi
	echo ">> DRM 模式运行: $TINYWL $*"
	exec "$TINYWL" "$@"
fi
