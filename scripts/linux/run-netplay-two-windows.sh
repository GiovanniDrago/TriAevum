#!/usr/bin/env bash
# Launch two TriAevum windows with separate saves, configs and controllers.
# All paths are derived from $HOME or an environment override; no machine path
# is stored in the repository.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${TRIAEVUM_BUILD_DIR:-$repo_root/../triaevum-netplay-build}"
package="${TRIAEVUM_DEV_PACKAGE:-$HOME/.local/share/TriAevum-dev/package}"
data="${TRIAEVUM_DEV_DATA:-$HOME/.local/share/TriAevum-dev/data}"
game_config="${TRIAEVUM_GAME_CONFIG:-$data/config/TriAevum.json}"
controls="${TRIAEVUM_CONTROLS_CONFIG:-$data/config/oot3d_controls.json}"
save_seed="${TRIAEVUM_SAVE_SEED:-$data/savedata}"
root="${TRIAEVUM_NETPLAY_ROOT:-${TMPDIR:-/tmp}/triaevum-netplay-two-windows}"
warmup="${TRIAEVUM_NETPLAY_WARMUP:-30}"
launcher="$build_dir/triaevum_launcher"

for path in "$package" "$package/TriAevum.launch.json" "$data" \
            "$game_config" "$controls" "$save_seed" "$build_dir"; do
  if [ ! -e "$path" ]; then
    echo "Missing required path: $path" >&2
    exit 1
  fi
done

if [ "${TRIAEVUM_SKIP_BUILD:-0}" != "1" ]; then
  cmake --build "$build_dir" --target oot3d_native_game triaevum_launcher \
    --parallel "${TRIAEVUM_BUILD_JOBS:-4}"
fi

if [ ! -x "$build_dir/TriAevum" ] || [ ! -x "$launcher" ]; then
  echo "Build did not produce $build_dir/TriAevum and $launcher" >&2
  exit 1
fi

# The title plugin is resolved relative to the executable, so run the current
# binary from the package that owns the plugin and assets.
install -m 755 "$build_dir/TriAevum" "$package/TriAevum"

args=(
  "$launcher"
  --package "$package"
  --runtime "$package/TriAevum"
  --launch-profile "$package/TriAevum.launch.json"
  --data "$data"
  --game-config "$game_config"
  --controls-config "$controls"
  --save-seed "$save_seed"
  --mode multiplayer
  --root "$root"
  --warmup "$warmup"
)

if [ -n "${TRIAEVUM_CACHE_SOURCE:-}" ]; then
  args+=(--cache-source "$TRIAEVUM_CACHE_SOURCE")
elif [ -d "$data/cache/renderer" ]; then
  args+=(--cache-source "$data/cache/renderer")
fi

exec "${args[@]}" "$@"
