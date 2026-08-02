#!/usr/bin/env bash
# 构建 tinywl：先装依赖，再用 CMake + Ninja 编译 wlroots 和 tinywl。
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR=build

# --- 依赖检查（只检查，不安装）--------------------------------------------
# wlroots 0.18 构建需要的开发包（Debian/Ubuntu）
APT_PACKAGES=(
	build-essential cmake meson ninja-build pkg-config
	libwayland-dev wayland-protocols
	libdrm-dev libgbm-dev libegl-dev libgles2-mesa-dev
	libxkbcommon-dev libpixman-1-dev
	libinput-dev libudev-dev libseat-dev libdisplay-info-dev
	hwdata
	# wlroots X11 后端（嵌套运行在 X11 桌面里需要）
	libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-render0-dev
	libxcb-render-util0-dev libxcb-shm0-dev libxcb-xfixes0-dev libxcb-xinput-dev
)

missing=()
for pkg in "${APT_PACKAGES[@]}"; do
	dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done

if ((${#missing[@]})); then
	echo "错误: 缺少以下依赖包:" >&2
	printf '  %s\n' "${missing[@]}" >&2
	echo >&2
	echo "请先安装:  sudo apt-get install ${missing[*]}" >&2
	echo "注意: 安装新依赖后需删掉 wlroots 的构建缓存再编译（meson 会缓存依赖探测结果）:" >&2
	echo "          rm -rf $BUILD_DIR/wlroots" >&2
	exit 1
fi

# --- 构建 -----------------------------------------------------------------
if [[ ! -f $BUILD_DIR/build.ninja ]]; then
	echo ">> 配置 CMake (Ninja)"
	cmake -S . -B "$BUILD_DIR" -G Ninja
fi

echo ">> 编译"
cmake --build "$BUILD_DIR"

echo
echo ">> 完成: $PWD/$BUILD_DIR/tinywl"
echo "   运行示例: ./$BUILD_DIR/tinywl -s foot   (在已有的 Wayland/X11 会话中)"
