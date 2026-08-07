#!/usr/bin/env bash
# 构建 tinywl：先装依赖，再用 CMake + Ninja 编译 wlroots 和 tinywl。
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR=build

# --- 依赖检查（只检查，不安装）--------------------------------------------
# 嵌套合成器：wlroots 只编 x11 后端，禁掉 drm/libinput 后端，因此
# 不依赖 libdrm / libinput / libudev（避开和系统运行库的版本冲突）。
# libpng / freetype 走源码子项目本地编译；wayland 1.23 / libgbm 用
# wlroots 子项目或 vendor 的 .pc，全都不碰系统包。
APT_PACKAGES=(
	build-essential cmake meson ninja-build pkg-config
	python3 libvulkan-dev glslang-tools vulkan-tools
	libwayland-dev wayland-protocols
	libegl-dev libgles2-mesa-dev
	libxkbcommon-dev libpixman-1-dev
	zlib1g-dev
	# wlroots X11 后端（嵌套运行在 X11 桌面里需要）
	libxcb1-dev libxcb-present-dev libxcb-render0-dev
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
	exit 1
fi

if ! vulkaninfo --summary >/dev/null 2>&1; then
	echo "错误: vulkaninfo 无法枚举 Vulkan 设备。" >&2
	exit 1
fi
if ! vulkaninfo --summary 2>/dev/null | grep -q 'VK_LAYER_KHRONOS_validation'; then
	echo "提示: 未发现 VK_LAYER_KHRONOS_validation；M2 validation 验收需要 vulkan-validationlayers。" >&2
fi

# --- 构建 -----------------------------------------------------------------
# 先单独配置+编译 wlroots（及其 wayland 1.23 子项目）。原因：tinywl/gles-term
# 需要链接 wayland 1.23（wlroots 0.18 用了 wl_shm v2，系统只有 1.22），
# 先把 wlroots 编出来才能拿到 wayland 的 .pc 给后续 CMake 用。
WLROOTS_BUILD=$BUILD_DIR/wlroots
echo ">> 配置 wlroots (Meson)"
cmake -E env PKG_CONFIG_PATH=$PWD/third_party/gbm \
	tools/configure-wlroots.sh third_party/wlroots "$WLROOTS_BUILD"
echo ">> 编译 wlroots"
ninja -C "$WLROOTS_BUILD"

# 再用 CMake 编 tinywl / gles-term。
# PKG_CONFIG_PATH 包含：gbm.pc，以及 wlroots 子项目生成的 wayland 1.23
# 的 *-uninstalled.pc（指向 build 树里的头和 .so，带 wl_shm v2，系统 1.22 没有）。
if [[ ! -f $BUILD_DIR/build.ninja ]]; then
	echo ">> 配置 CMake (Ninja)"
	cmake -E env PKG_CONFIG_PATH="$PWD/third_party/gbm:$PWD/$WLROOTS_BUILD/meson-uninstalled" \
		cmake -S . -B "$BUILD_DIR" -G Ninja
fi

echo ">> 编译"
cmake --build "$BUILD_DIR"

echo
echo ">> 完成: $PWD/$BUILD_DIR/tinywl"
echo "   运行示例: ./$BUILD_DIR/tinywl -s foot   (在已有的 Wayland/X11 会话中)"
