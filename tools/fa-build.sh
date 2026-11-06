#!/usr/bin/env bash
# One build script for every desktop test: checks out a clean commit, regenerates ppc/ whenever the
# recompiler inputs changed, and builds fullauto. Use this instead of copying trees or rebuilding by hand,
# so no run ever uses stale recompiled code.
#
#   tools/fa-build.sh                 # latest origin/main
#   tools/fa-build.sh <branch|commit> # e.g. claude/project-thread-xyz or a sha
#   tools/fa-build.sh --here          # build the current checkout as is (local edits allowed, marked -dirty)
#   tools/fa-build.sh --regen ...     # force a ppc/ regeneration
#   FA_STORE_TRACE_BUILD=ON tools/fa-build.sh   # diag build that can trace GPU command buffer writers
#
# Needs private/default.xex (from: build/faiso game.iso extract default.xex private/default.xex).
# Inputs that decide whether ppc/ is current: private/default.xex, config/fullauto.toml,
# config/extra_switch_tables.toml, patches/*.patch, the XenonRecomp base commit and this script's
# recompile steps. Their hash is stored in ppc/.fa-inputs; when it differs, ppc/ is regenerated
# (the previous one is kept as ppc.prev). fullauto prints the commit and this hash at startup:
#   [build] commit <sha> ppc <hash>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

REF=main HERE=0 REGEN=0
for a in "$@"; do
    case "$a" in
        --here) HERE=1 ;;
        --regen) REGEN=1 ;;
        -h|--help) sed -n 2,16p "$0"; exit 0 ;;
        *) REF="$a" ;;
    esac
done

say() { printf '[fa-build] %s\n' "$*"; }
die() { printf '[fa-build] error: %s\n' "$*" >&2; exit 1; }

# The order matters: each patch builds on the ones above it (see README, "Patching XenonRecomp").
PATCHES=(
    xenonrecomp-vmx-halfword.patch
    xenonrecomp-jumptables-and-more-vmx.patch
    xenonrecomp-vmx-round3.patch
    xenonrecomp-diag.patch
    xenonrecomp-overlap.patch
    xenonrecomp-mulhd-fix.patch
    xenonrecomp-keep-entries.patch
    xenonrecomp-switch-u32.patch
    xenonrecomp-switch-trap.patch
)
# Bump when the recompile steps below change in a way that changes the output.
RECIPE=1

# ---- 1. source ----------------------------------------------------------------------------------
if [ "$HERE" = 0 ]; then
    [ -z "$(git status --porcelain --untracked-files=no)" ] ||
        die "the checkout has local changes; commit or stash them, or use --here"
    say "fetching origin"
    git fetch -q origin
    if git rev-parse -q --verify "origin/$REF^{commit}" >/dev/null; then target="origin/$REF"
    else target="$REF"; fi
    git checkout -q --detach "$target" || die "cannot check out $REF"
fi
COMMIT="$(git rev-parse --short=12 HEAD)"
[ -z "$(git status --porcelain --untracked-files=no)" ] || COMMIT="$COMMIT-dirty"
say "source $COMMIT ($(git log -1 --format=%s HEAD | cut -c1-80))"

# Every patch in patches/ must be in the list above, so a new one is never silently skipped.
for p in patches/*.patch; do
    n="$(basename "$p")"
    printf '%s\n' "${PATCHES[@]}" | grep -qxF "$n" || die "patches/$n is not in the PATCHES list of tools/fa-build.sh"
done

[ -f private/default.xex ] || die "private/default.xex is missing (extract it with faiso first)"

# ---- 2. XenonRecomp with our patches ---------------------------------------------------------------
XR=third_party/XenonRecomp
if [ ! -d "$XR/.git" ] && [ ! -f "$XR/.git" ]; then
    say "cloning XenonRecomp"
    git clone -q --recursive https://github.com/hedge-dev/XenonRecomp "$XR"
fi
XR_BASE="$(git -C "$XR" rev-parse HEAD)"
PATCH_HASH="$( { echo "$XR_BASE"; for p in "${PATCHES[@]}"; do echo "$p"; cat "patches/$p"; done; } | sha256sum | cut -c1-16)"
if [ "$(cat "$XR/.fa-patches" 2>/dev/null)" != "$PATCH_HASH" ]; then
    say "re-applying the XenonRecomp patches"
    git -C "$XR" reset -q --hard "$XR_BASE"
    git -C "$XR" clean -qfd -e out
    git -C "$XR" submodule -q update --init --recursive
    for p in "${PATCHES[@]}"; do git -C "$XR" apply "$ROOT/patches/$p" || die "patch $p does not apply"; done
    echo "$PATCH_HASH" > "$XR/.fa-patches"
fi
if [ ! -f "$XR/out/build.ninja" ]; then
    cmake -S "$XR" -B "$XR/out" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
fi
say "building XenonAnalyse and XenonRecomp"
cmake --build "$XR/out" --target XenonAnalyse XenonRecomp >/dev/null

# ---- 3. ppc/ ------------------------------------------------------------------------------------
INPUT_HASH="$( { echo "recipe $RECIPE"; echo "$PATCH_HASH"; sha256sum private/default.xex config/fullauto.toml config/extra_switch_tables.toml; } | sha256sum | cut -c1-16)"
if [ "$REGEN" = 1 ] || [ ! -f ppc/ppc_config.h ] || [ "$(cat ppc/.fa-inputs 2>/dev/null)" != "$INPUT_HASH" ]; then
    say "regenerating ppc/ (inputs $INPUT_HASH, was $(cat ppc/.fa-inputs 2>/dev/null || echo none))"
    if [ -d ppc ]; then rm -rf ppc.prev; mv ppc ppc.prev; fi
    mkdir -p ppc
    "$XR/out/XenonAnalyse/XenonAnalyse" private/default.xex private/switch_tables.toml >/dev/null
    cat config/extra_switch_tables.toml >> private/switch_tables.toml
    "$XR/out/XenonRecomp/XenonRecomp" config/fullauto.toml "$XR/XenonUtils/ppc_context.h" > private/xenonrecomp.log 2>&1 ||
        { tail -20 private/xenonrecomp.log; die "XenonRecomp failed (full output in private/xenonrecomp.log)"; }
    n="$(cat ppc/ppc_recomp.*.cpp | grep -c 'default: __builtin_unreachable' || true)"
    [ "$n" = 0 ] || say "warning: $n switch defaults are still __builtin_unreachable (switch-trap patch missing?)"
    echo "$INPUT_HASH" > ppc/.fa-inputs
else
    say "ppc/ is current (inputs $INPUT_HASH)"
fi

# ---- 4. fullauto ----------------------------------------------------------------------------------
cmake -S runtime -B runtime/out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DFA_BUILD_COMMIT="$COMMIT" -DFA_BUILD_PPC="$INPUT_HASH" -DFA_STORE_TRACE="${FA_STORE_TRACE_BUILD:-OFF}" >/dev/null
say "building fullauto"
cmake --build runtime/out --target fullauto
say "done: runtime/out/fullauto, commit $COMMIT, ppc $INPUT_HASH"
