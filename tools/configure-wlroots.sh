#!/usr/bin/env bash
set -euo pipefail

if (($# != 2)); then
	echo "usage: $0 <wlroots-source-dir> <wlroots-build-dir>" >&2
	exit 2
fi

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
source_dir=$1
build_dir=$2
options_file=$repo_dir/wlroots-meson-options.txt

mapfile -t meson_options < <(sed -e '/^[[:space:]]*#/d' -e '/^[[:space:]]*$/d' "$options_file")
if [[ -f $build_dir/build.ninja ]]; then
	meson setup --reconfigure "$build_dir" "$source_dir" "${meson_options[@]}"
else
	meson setup "$build_dir" "$source_dir" "${meson_options[@]}"
fi

meson introspect "$build_dir" --buildoptions | python3 -c '
import json, sys
options = {item["name"]: item["value"] for item in json.load(sys.stdin)}
actual = options.get("renderers")
if actual != ["vulkan"]:
    raise SystemExit(f"renderer configuration mismatch: expected [vulkan], got {actual!r}")
print("wlroots.configure: ok renderers=[vulkan]")
'
