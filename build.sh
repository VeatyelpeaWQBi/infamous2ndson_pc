#!/usr/bin/env bash
# Windows build orchestration inside an existing MSYS2 CLANG64 environment.
set -euo pipefail
cd -- "$(dirname -- "$0")"
case $(uname -s) in
    MINGW*|MSYS*) ;;
    *) echo 'This fork builds only Windows x86-64. Use build.bat and MSYS2 CLANG64.' >&2; exit 1 ;;
esac
if [[ ${MSYSTEM:-} != CLANG64 ]]; then
    echo 'Use the MSYS2 CLANG64 environment (build.bat), not MSYS/MINGW64/WSL.' >&2; exit 1
fi
build_tests=0
run_tests=0
case ${1:-} in
    '') ;;
    --build-tests) build_tests=1 ;;
    --test) build_tests=1; run_tests=1 ;;
    *) echo 'Usage: bash build.sh [--build-tests|--test]' >&2; exit 2 ;;
esac
if [[ $# -gt 1 ]]; then echo 'Unexpected build arguments.' >&2; exit 2; fi
CC=${CC:-clang}
CXX=${CXX:-clang++}
for tool in "$CC" "$CXX" pkg-config cmake ninja ar git; do
    if ! command -v "$tool" >/dev/null; then
        echo "Missing existing tool: $tool. See docs/WINDOWS_INSTALL.zh-CN.md." >&2; exit 1
    fi
done
compiler_version=$("$CC" --version)
if [[ ${compiler_version,,} != *clang* ]]; then
    echo 'Windows builds require Clang/libc++/LLD from MSYS2 CLANG64.' >&2; exit 1
fi
compiler_target=$("$CC" -dumpmachine)
if [[ $compiler_target != x86_64*w64* ]]; then
    echo "Unexpected compiler target: $compiler_target. Use MSYS2 CLANG64 x86-64." >&2; exit 1
fi
if ! pkg-config --exists vulkan sdl3; then
    echo 'Missing Vulkan/SDL3 development packages. No packages were installed.' >&2; exit 1
fi
if [[ ${BB_PGO:-off} != off ]]; then
    echo 'Windows PGO is not validated; use BB_PGO=off. GCC/Linux profiles are not reused.' >&2; exit 1
fi
# Ordinary builds are offline. Only an explicit opt-in permits missing source downloads.
allow_downloads=OFF
if [[ ${BB_ALLOW_DOWNLOADS:-0} == 1 ]]; then allow_downloads=ON; fi
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt ||
      ! -f gpu/third_party/imgui/imgui.h || ! -d third_party/LibAtrac9/C/src ]]; then
    if [[ $allow_downloads != ON ]]; then
        echo 'Submodule sources are missing. No downloads were started.' >&2
        echo 'After approving disk/network use, run build.bat --allow-downloads.' >&2
        exit 1
    fi
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
mkdir -p out
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" -DBB_PGO=off \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_ALLOW_DOWNLOADS="$allow_downloads" \
    -DBB_BUILD_TESTS="$([[ $build_tests == 1 ]] && echo ON || echo OFF)"
echo "Windows GPU library: PGO off, LTO ${BB_LTO:-ON}"
if ! cmake --build out/gpu --target bbgpu > out/gpu-build.log 2>&1; then
    tail -40 out/gpu-build.log >&2
    echo 'GPU build failed (full log: out/gpu-build.log).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
libraries=("${libraries[@]/-mwindows/-mconsole}")
cflags=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00 -O2 -g -Wall -Wextra -Werror)
mapfile -t gpu < out/gpu/bbgpu_link.txt
runtime=(src/runtime*.c src/win32_*.c)
link=(-Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi)
if [[ ${BB_LTO:-ON} != OFF ]]; then link+=(-flto=thin); fi
# Recompile every decoder source after a source change; no recursive deletion is needed.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    mkdir -p out/atrac9
    objects=()
    for source in "${atrac9[@]}"; do
        object="out/atrac9/$(basename "${source%.c}").o"
        "$CC" -std=c99 -O2 -g -w -c "$source" -o "$object"
        objects+=("$object")
    done
    # Build a fresh archive so objects for removed sources cannot survive an update.
    rm -f out/libatrac9.next.a
    ar rcs out/libatrac9.next.a "${objects[@]}"
    mv -f out/libatrac9.next.a out/libatrac9.a
fi
if [[ -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe &&
      -z $(find src gpu/bbgpu.h out/gpu/libbbgpu.a out/gpu/bbgpu_link.txt out/libatrac9.a build.sh \
               tools/gpu_capabilities.c -newer out/bb-probe.exe -print -quit) ]]; then
    echo "Up to date: $PWD/out/bb-probe.exe"
else
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" \
        src/vulkan_smoke.c out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/bb-probe.exe
    "$CC" "${cflags[@]}" tools/gpu_capabilities.c "${includes[@]}" "${libraries[@]}" -o out/bb-gpu-capabilities.exe
    echo "Built $PWD/out/bb-probe.exe"
fi
if [[ $build_tests == 1 ]]; then
    # All runtime tests use the same Win32 source set and flags as the executable.
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_pad.c src/runtime_host.c \
        "${libraries[@]}" -o out/pad-test.exe
    for name in runtime sema infamous; do
        "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc "tests/test_$name.c" "${runtime[@]}" \
            out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o "out/$name-test.exe"
    done
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_file_mods.c src/win32_compat.c \
        -lws2_32 -o out/file-mods-test.exe
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test.exe
    cmake --build out/gpu --target motion-history-test ui-composition-test upscaler-support-test \
        motion-shader-test scene-resolution-test taa-shader-test camera-motion-test videodec-test image-compat-test
fi
if [[ $run_tests == 1 ]]; then
    for name in pad runtime file-mods sema content; do "out/$name-test.exe"; done
    ctest --test-dir out/gpu -L unit --output-on-failure --no-tests=error
fi
