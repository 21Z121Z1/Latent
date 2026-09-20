#!/usr/bin/env bash
# Archive source inputs, never a checkout's credential-bearing .git/config.
set -euo pipefail
build=${1:?build directory required}
out=${2:?output directory required}
mkdir -p "$out"
git bundle create "$out/latent.bundle" HEAD
git archive --format=tar.gz -o "$out/latent-source.tar.gz" HEAD
{
    printf 'source_commit='
    git rev-parse HEAD
    cmake --version | head -n 1
    c++ --version | head -n 1
    for dep in vulkan_headers volk glslang; do
        dir="$build/_deps/$dep-src"
        if test -d "$dir"; then
            printf '%s_commit=' "$dep"
            git -C "$dir" rev-parse HEAD
        fi
    done
} > "$out/inputs.txt"
if test -d "$build/_deps/glslang-src"; then
    tar --exclude=.git -czf "$out/vulkan-build-inputs.tar.gz" -C "$build/_deps" \
        vulkan_headers-src volk-src glslang-src
fi
(cd "$out" && sha256sum *.gz *.bundle > SHA256SUMS)
