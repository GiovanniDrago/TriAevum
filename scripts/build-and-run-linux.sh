#!/usr/bin/env bash
# Build TriAevum from this checkout, prepare the official translated title, and
# run the game with a personal decrypted ROM.
#
# The public source tree contains the runtime/renderer/host services, but not
# the translated OoT3D title logic.  TriAevum distributes that title (and its
# corresponding source) in the official Linux release.  This helper downloads
# the matching release once, builds the runtime here, optionally rebuilds the
# title from the release source, then imports the ROM and launches the game.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build="${TRIAEVUM_BUILD_DIR:-$root/../triaevum-linux-build}"
dev_root="${TRIAEVUM_DEV_ROOT:-${XDG_DATA_HOME:-$HOME/.local/share}/TriAevum-dev}"
release_tag="${TRIAEVUM_RELEASE_TAG:-}"
release_arg=""
rom=""
do_build=1
do_launch=1
rebuild_title=0
refresh_title_manifest=0
skip_source=0
title_source_dir=""
hd_font_path=""
do_hd_font=1
jobs="${TRIAEVUM_BUILD_JOBS:-$(nproc)}"

default_release_tag="v0.8.0-beta.1.1"

usage() {
    cat <<EOF
usage: $0 [options] /path/to/decrypted.cci

Options:
  --release PATH            Use an official Linux AppImage or an already
                            extracted release directory instead of downloading.
  --rebuild-title           Build the title from the release's translated source
                            instead of using the release's precompiled title.
  --title-source-dir PATH   Build the title from this (possibly edited) source
                            tree. Implies --rebuild-title.
  --refresh-title-manifest  Update TITLE_SOURCE_MANIFEST.json hashes after local
                            edits before building the title.
  --no-build                Reuse the existing runtime build.
  --no-launch               Prepare everything, then print the launch command.
  --skip-source             Do not extract/index the translated title source.
  --hd-font PATH            Build the built-in HD dialogue font from this TTF/OTF
                            (forces regeneration of font_coverage.zip).
  --no-hd-font              Do not generate the built-in HD dialogue font.
  -j, --jobs N              Build parallelism (default: nproc).
  -h, --help                Show this help.

Environment:
  TRIAEVUM_BUILD_DIR        Runtime build directory (default: ../triaevum-linux-build).
  TRIAEVUM_DEV_ROOT         Development package/data root
                            (default: \$XDG_DATA_HOME/TriAevum-dev).
  TRIAEVUM_RELEASE_TAG      Official release tag to download (default: matching
                            git tag, or $default_release_tag).
  TRIAEVUM_BUILD_JOBS       Default build parallelism.

Data created by this helper stays outside the repository.  Saves, settings and
caches live under \$TRIAEVUM_DEV_ROOT/data.
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

need() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

while (($#)); do
    case "$1" in
        --release)
            (($# >= 2)) || die "--release needs a path"
            release_arg="$2"; shift 2 ;;
        --rebuild-title)
            rebuild_title=1; shift ;;
        --title-source-dir)
            (($# >= 2)) || die "--title-source-dir needs a path"
            title_source_dir="$2"; rebuild_title=1; shift 2 ;;
        --refresh-title-manifest)
            refresh_title_manifest=1; shift ;;
        --no-build)
            do_build=0; shift ;;
        --no-launch)
            do_launch=0; shift ;;
        --skip-source)
            skip_source=1; shift ;;
        --hd-font)
            (($# >= 2)) || die "--hd-font needs a path"
            hd_font_path="$2"; shift 2 ;;
        --no-hd-font)
            do_hd_font=0; shift ;;
        -j|--jobs)
            (($# >= 2)) || die "$1 needs a number"
            jobs="$2"; shift 2 ;;
        -h|--help)
            usage; exit 0 ;;
        -*)
            die "unknown option: $1 (try --help)" ;;
        *)
            [[ -z "$rom" ]] || die "only one ROM path is accepted"
            rom="$1"; shift ;;
    esac
done

[[ -n "$rom" ]] || { usage >&2; exit 2; }
[[ -f "$rom" ]] || die "ROM not found: $rom"
rom="$(realpath -- "$rom")"
[[ "$jobs" =~ ^[0-9]+$ ]] && ((jobs > 0)) || die "invalid job count: $jobs"

for command in cmake ninja clang clang++ llvm-ar ld.lld python3 pkg-config git curl sha256sum realpath; do
    need "$command"
done

mkdir -p "$dev_root"
build="$(realpath -m -- "$build")"
dev_root="$(realpath -m -- "$dev_root")"

spdlog_found() {
    pkg-config --exists spdlog >/dev/null 2>&1 && return 0
    local candidate
    for candidate in \
        /usr/lib/cmake/spdlog/spdlogConfig.cmake \
        /usr/lib64/cmake/spdlog/spdlogConfig.cmake \
        /usr/local/lib/cmake/spdlog/spdlogConfig.cmake \
        /usr/share/cmake/spdlog/spdlogConfig.cmake \
        "$dev_root/deps/spdlog/lib/cmake/spdlog/spdlogConfig.cmake"; do
        [[ -f "$candidate" ]] && return 0
    done
    return 1
}

ensure_spdlog() {
    spdlog_found && return 0
    printf 'spdlog was not found; building a private copy under %s\n' "$dev_root/deps/spdlog"
    local source="$dev_root/deps/spdlog-src"
    if [[ ! -d "$source/.git" ]]; then
        rm -rf "$source"
        git clone --depth 1 --branch v1.17.0 https://github.com/gabime/spdlog.git "$source"
    fi
    cmake -S "$source" -B "$dev_root/deps/spdlog-build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$dev_root/deps/spdlog" \
        -DSPDLOG_BUILD_SHARED=OFF -DSPDLOG_BUILD_EXAMPLE=OFF \
        -DSPDLOG_BUILD_TESTS=OFF -DSPDLOG_FMT_EXTERNAL=ON
    cmake --build "$dev_root/deps/spdlog-build" --parallel "$jobs"
    cmake --install "$dev_root/deps/spdlog-build" >/dev/null
}

deps_prefix=""
ensure_spdlog
if ! spdlog_found; then
    deps_prefix="$dev_root/deps/spdlog"
elif [[ -f "$dev_root/deps/spdlog/lib/cmake/spdlog/spdlogConfig.cmake" ]] && \
     ! pkg-config --exists spdlog >/dev/null 2>&1; then
    deps_prefix="$dev_root/deps/spdlog"
fi

if ((do_build)); then
    printf 'Building the TriAevum runtime (first build takes several minutes)...\n'
    env TRIAEVUM_BUILD_DIR="$build" \
        CMAKE_BUILD_PARALLEL_LEVEL="$jobs" \
        TRIAEVUM_DEPS_PREFIX="$deps_prefix" \
        bash "$root/scripts/build-linux-runtime.sh"
else
    [[ -f "$build/TriAevum" ]] || \
        die "runtime build not found in $build; run without --no-build first"
fi

tag="${release_tag:-}"
if [[ -z "$tag" ]]; then
    tag="$(git -C "$root" describe --tags --abbrev=0 2>/dev/null || true)"
fi
if [[ -z "$tag" ]]; then
    tag="$default_release_tag"
fi
if ! git -C "$root" describe --tags --exact-match HEAD >/dev/null 2>&1; then
    printf 'warning: HEAD is not the exact %s tag; using its official release title/source\n' "$tag" >&2
    printf '         pass --release with a matching package, or use --rebuild-title for local changes\n' >&2
fi

extract_release() {
    local image="$1" destination="$2" temporary
    printf 'Extracting the official Linux release...\n'
    mkdir -p "$dev_root/release" "$(dirname "$destination")"
    temporary="$(mktemp -d "$dev_root/release/.extract.XXXXXX")"
    trap 'rm -rf "$temporary"' EXIT
    chmod +x "$image"
    (cd "$temporary" && "$image" --appimage-extract >/dev/null)
    [[ -d "$temporary/squashfs-root/usr/lib/triaevum" ]] || \
        die "unexpected AppImage layout: $image"
    rm -rf "${destination}.tmp"
    mv "$temporary/squashfs-root/usr/lib/triaevum" "${destination}.tmp"
    rm -rf "$destination"
    mv "${destination}.tmp" "$destination"
    rm -rf "$temporary"
    trap - EXIT
}

download_release() {
    local directory="$dev_root/release/$tag"
    local image="$directory/TriAevum-$tag-Linux-x86_64.AppImage"
    local sums="$directory/SHA256SUMS.txt"
    local base="https://github.com/coccofresco/TriAevum/releases/download/$tag"
    mkdir -p "$directory"
    if [[ ! -f "$image" ]]; then
        printf 'Downloading official Linux release %s (one time, ~218 MB)...\n' "$tag" >&2
        curl --fail --location --retry 3 --output "$image.part" \
            "$base/TriAevum-$tag-Linux-x86_64.AppImage" >&2
        curl --fail --location --retry 3 --output "$sums" "$base/SHA256SUMS.txt" >&2
        local expected actual
        expected="$(awk -v name="$(basename "$image")" '{ n=$2; sub(/^\*/, "", n); if (n == name) print $1 }' "$sums")"
        [[ -n "$expected" ]] || die "release checksum is missing for $(basename "$image")"
        actual="$(sha256sum "$image.part" | awk '{ print $1 }')"
        [[ "$actual" == "$expected" ]] || die "release checksum mismatch (expected $expected, got $actual)"
        mv "$image.part" "$image"
    fi
    printf '%s\n' "$image"
}

release_root=""
if [[ -n "$release_arg" ]]; then
    release_arg="$(realpath -- "$release_arg")"
    if [[ -d "$release_arg" ]]; then
        release_root="$release_arg"
    elif [[ -f "$release_arg" ]]; then
        release_root="$dev_root/release/local/$(basename "$release_arg" .AppImage)"
        if [[ ! -f "$release_root/TriAevumForge" ]]; then
            extract_release "$release_arg" "$release_root"
        fi
    else
        die "release path not found: $release_arg"
    fi
else
    image="$(download_release)"
    release_root="$dev_root/release/$tag/extracted"
    if [[ ! -f "$release_root/TriAevumForge" ]]; then
        extract_release "$image" "$release_root"
    fi
fi
[[ -f "$release_root/TriAevumForge" ]] || die "not an extracted TriAevum release: $release_root"

source_archive="$release_root/source/titles/oot3d-linux-translated.zip"
[[ -f "$source_archive" ]] || die "release has no translated source archive: $source_archive"
source_hash="$(sha256sum "$source_archive" | awk '{ print $1 }')"
pristine_source="$dev_root/title-source/$source_hash"

if ((!skip_source)); then
    python3 "$root/scripts/linux/title_source_tools.py" extract \
        --release "$release_root" --output "$pristine_source"
    python3 "$root/scripts/linux/title_source_tools.py" index --source "$pristine_source"
fi

title_plugin=""
if ((rebuild_title)); then
    if [[ -z "$title_source_dir" ]]; then
        [[ -d "$pristine_source" ]] || \
            die "translated source was skipped; remove --skip-source or pass --title-source-dir"
        title_source_dir="$pristine_source"
    fi
    title_source_dir="$(realpath -- "$title_source_dir")"
    [[ -f "$title_source_dir/TITLE_SOURCE_MANIFEST.json" ]] || \
        die "not a translated title source tree: $title_source_dir"
    if ((refresh_title_manifest)); then
        python3 "$root/scripts/linux/title_source_tools.py" refresh-manifest \
            --source "$title_source_dir"
    fi
    title_build="$dev_root/title-build"
    printf 'Building the translated title from %s...\n' "$title_source_dir"
    cmake -S "$root/tools/triaevum_release/linux_title" -B "$title_build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DTRIAEVUM_TRANSLATED_TITLE_DIR="$title_source_dir"
    cmake --build "$title_build" --parallel "$jobs"
    title_plugin="$title_build/triaevum_title_aot.so"
    [[ -f "$title_plugin" ]] || die "title build produced no plugin: $title_plugin"
fi

package="$dev_root/package"
printf 'Preparing the development package...\n'
package_args=(
    --release "$release_root"
    --build-dir "$build"
    --package "$package"
    --source-commit "$(git -C "$root" rev-parse HEAD)"
)
if [[ -n "$title_plugin" ]]; then
    package_args+=(--title-plugin "$title_plugin")
fi
python3 "$root/scripts/linux/prepare_dev_package.py" "${package_args[@]}"

# The frozen release Forge is the supported player import path.  Do not let an
# inherited AppImage/Flatpak environment redirect its package-relative paths.
unset APPDIR FLATPAK_ID
install_log="$dev_root/forge-install.log"
printf 'Importing and verifying the ROM (first run prepares GPU pipelines)...\n'
set -o pipefail
if ! "$package/TriAevumForge" --install-worker --rom "$rom" \
        --data-root "$dev_root/data" 2>&1 | tee "$install_log" | \
        python3 "$root/scripts/linux/forge_progress.py"; then
    die "installation failed; see $install_log"
fi

if ((do_hd_font)); then
    hd_args=(
        --data-root "$dev_root/data"
        --launch-profile "$package/TriAevum.launch.json"
    )
    if [[ -n "$hd_font_path" ]]; then
        hd_args+=(--font "$hd_font_path")
    fi
    if ! python3 "$root/scripts/linux/prepare_hd_font.py" "${hd_args[@]}"; then
        printf 'warning: the built-in HD font pack was not generated; the native font remains active\n' >&2
    fi
else
    python3 "$root/scripts/linux/prepare_hd_font.py" --remove \
        --data-root "$dev_root/data" \
        --launch-profile "$package/TriAevum.launch.json" || \
        printf 'warning: could not remove the built-in HD font pack\n' >&2
fi

# F1 stores the requested output resolution in the config.  The release
# installer writes a fixed profile, so mirror the persisted value before launch.
python3 "$root/scripts/linux/sync_launch_resolution.py" \
    --config "$dev_root/data/config/TriAevum.json" \
    --launch-profile "$package/TriAevum.launch.json" || \
    printf 'warning: could not sync the launch resolution\n' >&2

launch_command=("$package/TriAevum" --launch-profile "$package/TriAevum.launch.json")
if ((do_launch)); then
    printf 'Launching TriAevum...\n'
    cd "$package"
    exec "${launch_command[@]}"
else
    printf 'Ready. Launch with:\n  %q ' "${launch_command[0]}"
    printf '%q ' "${launch_command[@]:1}"
    printf '\n'
fi
