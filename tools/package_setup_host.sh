#!/usr/bin/env bash
# Universal setup-host zip packager for PSXRecomp game repos.
#
# Stages the host exe, title sources, filtered psxrecomp/ + recomp-ui/, then
# finishes with stage_setup_sdk.sh (emitters, OpenBIOS, MinGW DLLs).
# Portable cmake/clang is NOT embedded by default — RetComM / the setup wizard
# download cmake-clang-v1 from retcomm-toolchains (or accept an offline zip).
#
# Usage (from game repo root):
#   psxrecomp/tools/package_setup_host.sh \
#     --build-dir build-ci \
#     --artifact linux-x64 \
#     --zip-prefix bpe \
#     --exe-name Bomberman_Party_Edition_Recompiled \
#     --display-name "Bomberman Party Edition Recompiled" \
#     --recompiler-build build-recompiler \
#     [--project-file REL]... [--project-dir REL]... \
#     [--runtime-dir NAME]... [--runtime-dir-optional NAME]... \
#     [--disc-hint "your legally owned disc"] \
#     [--version-env BPE_RELEASE_VERSION] \
#     [--embed-toolchain]   # optional: copy PSXRECOMP_TOOLCHAIN_DIR into zip
#
# --runtime-dir stages a directory the runtime loads exe-relative (mods,
# bezels, ...) from the built exe's directory into the stage root, so the
# shipped layout matches what the build tree staged next to the binary.
# Per-user state (mods/state.toml) is always excluded. The -optional variant
# warns and continues when the directory is absent from the build.
#
# mods/ IS STAGED BY DEFAULT and is REQUIRED. Every non-oracle game runtime
# gets <exe>/mods from runtime.cmake, so a release missing it means the build
# wiring is broken and the title would ship with an empty Mods page -- which
# is exactly what happened: this was opt-in per title, and 21 of 23 ports
# silently shipped no mods at all because their generated wrapper never passed
# --runtime-dir mods. Enforce it here rather than in each title's wrapper.
# A title that genuinely ships no catalog passes --no-mods.
#
# DEVELOPER CHANNEL. A package manifest may declare channel = "developer".
# Those are work-in-progress catalogs that should stay on the maintainer's
# machine: they ship with ordinary local builds and local exports, but
# --exclude-dev-mods (or EXCLUDE_DEV_MODS=1, which CI sets) drops them from a
# public release. Pruning is by PACKAGE directory, never by rewriting a
# manifest -- editing a package during packaging would change content that is
# meant to be verifiable.
#
# Env:
#   RELEASE_VERSION / <version-env> / VERSION file  (must match binary stamp)
#   PSXRECOMP_TOOLCHAIN_DIR | TOOLCHAIN_DIR | BPE_TOOLCHAIN_DIR  (only with --embed-toolchain)
#   PSXRECOMP_EMBED_TOOLCHAIN=1  same as --embed-toolchain
#   PSXRECOMP_RUNTIME_BIN_DIR | BPE_RUNTIME_BIN_DIR  (Windows MinGW DLL search)
#
# Lobby pin: the host exe must have been built with current runtime.cmake so
# $<TARGET_FILE_DIR>/psx_game_version.txt exists. This script refuses to ship a
# VERSION file that disagrees with that stamp (netplay list filter bug).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(pwd)"

BUILD_DIR=""
ARTIFACT=""
ZIP_PREFIX=""
EXE_NAME=""
DISPLAY_NAME=""
RECOMPILER_BUILD="build-recompiler"
VERSION_ENV="RELEASE_VERSION"
DISC_HINT="your legally owned game disc"
# A title that ships OpenBIOS uses it and nothing else -- setup never adopts a
# retail dump for one, so telling the player a dump is "optional" invites them
# to supply a file that is then silently ignored. Opt-in, because titles that
# genuinely need a retail image still exist.
OPENBIOS_ONLY=0
PROJECT_FILES=()
PROJECT_DIRS=()
RUNTIME_DIRS=()
RUNTIME_DIRS_OPTIONAL=()
# mods/ ships by default; --no-mods opts a catalog-less title out.
STAGE_MODS=1
# Drop channel = "developer" packages from the shipped catalog.
#
# Defaults to ON under CI and OFF locally, which is the whole point: a
# maintainer's local build and local export keep the mods they are working on,
# while anything a release workflow publishes does not. Keying off $CI (set by
# GitHub Actions and every other provider) means this holds for every title
# without editing 23 separate release workflows -- a per-workflow opt-in is
# exactly the shape that left 21 of 23 ports shipping no mods at all.
# --include-dev-mods / --exclude-dev-mods override either way.
if [[ -z "${EXCLUDE_DEV_MODS:-}" ]]; then
  if [[ -n "${CI:-}" ]]; then EXCLUDE_DEV_MODS=1; else EXCLUDE_DEV_MODS=0; fi
fi
PROJECT_EXCLUDES=()
RUNTIME_BIN_DIR="${PSXRECOMP_RUNTIME_BIN_DIR:-${BPE_RUNTIME_BIN_DIR:-/usr/x86_64-w64-mingw32/bin}}"
EMBED_TOOLCHAIN=0
if [[ "${PSXRECOMP_EMBED_TOOLCHAIN:-0}" == "1" ]]; then
  EMBED_TOOLCHAIN=1
fi

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-2}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage 0 ;;
    --build-dir) BUILD_DIR="${2:?}"; shift 2 ;;
    --artifact) ARTIFACT="${2:?}"; shift 2 ;;
    --zip-prefix) ZIP_PREFIX="${2:?}"; shift 2 ;;
    --exe-name) EXE_NAME="${2:?}"; shift 2 ;;
    --display-name) DISPLAY_NAME="${2:?}"; shift 2 ;;
    --recompiler-build) RECOMPILER_BUILD="${2:?}"; shift 2 ;;
    --version-env) VERSION_ENV="${2:?}"; shift 2 ;;
    --disc-hint) DISC_HINT="${2:?}"; shift 2 ;;
    --project-file) PROJECT_FILES+=("${2:?}"); shift 2 ;;
    --project-dir) PROJECT_DIRS+=("${2:?}"); shift 2 ;;
    --no-mods) STAGE_MODS=0; shift ;;
    --exclude-dev-mods) EXCLUDE_DEV_MODS=1; shift ;;
    --include-dev-mods) EXCLUDE_DEV_MODS=0; shift ;;
    --runtime-dir) RUNTIME_DIRS+=("${2:?}"); shift 2 ;;
    --runtime-dir-optional) RUNTIME_DIRS_OPTIONAL+=("${2:?}"); shift 2 ;;
    --project-exclude) PROJECT_EXCLUDES+=("${2:?}"); shift 2 ;;
    --runtime-bin) RUNTIME_BIN_DIR="${2:?}"; shift 2 ;;
    --root) ROOT="${2:?}"; shift 2 ;;
    --embed-toolchain) EMBED_TOOLCHAIN=1; shift ;;
    --no-embed-toolchain) EMBED_TOOLCHAIN=0; shift ;;
    --openbios-only) OPENBIOS_ONLY=1; shift ;;
    *)
      echo "error: unknown arg: $1" >&2
      usage 2
      ;;
  esac
done

# Default the mods catalog into the required set unless the caller already
# named it (in either list) or opted out. Appending rather than overriding
# keeps an explicit --runtime-dir-optional mods meaningful for a title that
# knowingly builds without one.
if [[ "${STAGE_MODS}" -eq 1 ]]; then
  _mods_named=0
  for _d in "${RUNTIME_DIRS[@]:-}" "${RUNTIME_DIRS_OPTIONAL[@]:-}"; do
    [[ "${_d}" == "mods" ]] && _mods_named=1
  done
  if [[ "${_mods_named}" -eq 0 ]]; then
    RUNTIME_DIRS+=("mods")
  fi
  # Also ship the SOURCE catalog (mods/preloaded/...) when the title has one.
  # The setup-host zip rebuilds on the player's machine, and that rebuild
  # re-runs the title's POST_BUILD mod staging -- which reads the source tree,
  # not the staged copy. Without this the rebuilt exe silently loses every
  # game-owned mod and keeps only the framework builtins. copy_proj skips a
  # path that does not exist, so this is inert for a title with no catalog.
  _mods_proj=0
  for _d in "${PROJECT_DIRS[@]:-}"; do
    [[ "${_d}" == "mods" ]] && _mods_proj=1
  done
  if [[ "${_mods_proj}" -eq 0 ]]; then
    PROJECT_DIRS+=("mods")
  fi
fi

if [[ -z "${BUILD_DIR}" || -z "${ARTIFACT}" || -z "${ZIP_PREFIX}" || -z "${EXE_NAME}" ]]; then
  echo "error: --build-dir, --artifact, --zip-prefix, and --exe-name are required" >&2
  usage 2
fi
if [[ -z "${DISPLAY_NAME}" ]]; then
  DISPLAY_NAME="${EXE_NAME}"
fi

ROOT="$(cd "${ROOT}" && pwd)"
if [[ -d "${ROOT}/${BUILD_DIR}" ]]; then
  BUILD_DIR="$(cd "${ROOT}/${BUILD_DIR}" && pwd)"
elif [[ -d "${BUILD_DIR}" ]]; then
  BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"
else
  echo "error: build dir not found: ${BUILD_DIR}" >&2
  exit 1
fi

# Defaults for a typical title if caller passed none.
# Codegen host sources live in psxrecomp/host/ (copied with the framework tree).
if [[ ${#PROJECT_FILES[@]} -eq 0 && ${#PROJECT_DIRS[@]} -eq 0 ]]; then
  PROJECT_FILES=(CMakeLists.txt game.toml VERSION)
  for d in seeds launcher_assets; do
    if [[ -e "${ROOT}/${d}" ]]; then
      PROJECT_DIRS+=("${d}")
    fi
  done
  for f in codegen_setup.c codegen_setup.h DISC.md README.md; do
    if [[ -e "${ROOT}/${f}" ]]; then
      PROJECT_FILES+=("${f}")
    fi
  done
fi

# Resolve *requested* version from env / VERSION file (may be empty until stamp).
REQUESTED=""
if [[ -n "${VERSION_ENV}" ]]; then
  REQUESTED="${!VERSION_ENV:-}"
fi
if [[ -z "${REQUESTED}" && -n "${RELEASE_VERSION:-}" ]]; then
  REQUESTED="${RELEASE_VERSION}"
fi
if [[ -z "${REQUESTED}" && -n "${BPE_RELEASE_VERSION:-}" ]]; then
  REQUESTED="${BPE_RELEASE_VERSION}"
fi
if [[ -z "${REQUESTED}" && -f "${ROOT}/VERSION" ]]; then
  REQUESTED="$(tr -d '[:space:]' <"${ROOT}/VERSION")"
fi
REQUESTED="$(printf '%s' "${REQUESTED}" | tr -d '[:space:]')"
REQUESTED="${REQUESTED#v}"

DIST="${ROOT}/dist"
STAGE="${DIST}/stage-setup-${ARTIFACT}"

rm -rf "${STAGE}"
mkdir -p "${STAGE}" "${DIST}"

EXE=""
for cand in \
  "${BUILD_DIR}/${EXE_NAME}" \
  "${BUILD_DIR}/${EXE_NAME}.exe" \
  "${BUILD_DIR}/Release/${EXE_NAME}.exe"
do
  if [[ -f "${cand}" ]]; then
    EXE="${cand}"
    break
  fi
done
if [[ -z "${EXE}" ]]; then
  echo "error: setup host executable '${EXE_NAME}' not found under ${BUILD_DIR}" >&2
  ls -la "${BUILD_DIR}" >&2 || true
  exit 1
fi

# Authoritative lobby pin = stamp written at compile time next to the exe
# (see runtime.cmake file(GENERATE) psx_game_version.txt). Never invent a
# newer VERSION for the zip than what was baked into PSX_GAME_VERSION.
normalize_ver() {
  local v
  v="$(printf '%s' "${1:-}" | tr -d '[:space:]')"
  v="${v#v}"
  printf '%s' "${v}"
}
BUILT=""
for stamp in \
  "$(dirname "${EXE}")/psx_game_version.txt" \
  "${BUILD_DIR}/psx_game_version.txt" \
  "${BUILD_DIR}/Release/psx_game_version.txt"
do
  if [[ -f "${stamp}" ]]; then
    BUILT="$(normalize_ver "$(cat "${stamp}")")"
    echo "lobby pin stamp: ${stamp} -> ${BUILT}"
    break
  fi
done
if [[ -z "${BUILT}" ]]; then
  echo "error: missing psx_game_version.txt next to ${EXE}" >&2
  echo "  Rebuild with current psxrecomp runtime.cmake so the lobby pin is stamped." >&2
  echo "  Refusing to package (VERSION file alone can drift from PSX_GAME_VERSION)." >&2
  exit 1
fi
if [[ -n "${REQUESTED}" && "${REQUESTED}" != "${BUILT}" ]]; then
  echo "error: RELEASE_VERSION/VERSION=${REQUESTED} but binary stamp=${BUILT}" >&2
  echo "  Rebuild with -DPSX_GAME_VERSION=${REQUESTED} (or pin VERSION then reconfigure)," >&2
  echo "  then re-run this packager. Shipping mismatched pins breaks netplay lobby lists." >&2
  exit 1
fi
VERSION="${BUILT}"
printf '%s\n' "${VERSION}" >"${ROOT}/VERSION"
ZIP_NAME="${ZIP_PREFIX}-${VERSION}-${ARTIFACT}.zip"
rm -f "${DIST}/${ZIP_NAME}"

cp -a "${EXE}" "${STAGE}/"
# Ship the stamp beside the exe so installers / RetComM can prefer it over VERSION.
if [[ -f "$(dirname "${EXE}")/psx_game_version.txt" ]]; then
  cp -a "$(dirname "${EXE}")/psx_game_version.txt" "${STAGE}/psx_game_version.txt"
else
  printf '%s\n' "${VERSION}" >"${STAGE}/psx_game_version.txt"
fi
EXE_BASENAME="$(basename "${EXE}")"
EXE_DIR="$(dirname "${EXE}")"

# Windows: CMake may already have placed imported runtime DLLs (zlib1.dll)
# next to the host. Copy siblings into the stage before MinGW bundling —
# this packager used to copy only the .exe, so a missed PE-import walk
# shipped hosts that die on clean machines with "zlib1.dll was not found".
if [[ "${EXE_BASENAME}" == *.exe ]]; then
  shopt -s nullglob
  for dll in "${EXE_DIR}"/*.dll "${EXE_DIR}"/*.DLL; do
    [[ -f "${dll}" ]] || continue
    cp -a "${dll}" "${STAGE}/"
    echo "staged sibling DLL $(basename "${dll}")"
  done
  shopt -u nullglob
fi

# assets/{fonts,img} belong to the recomp-ui launcher. A project built with
# PSX_RECOMP_UI=OFF has no launcher and therefore none of them — that is a
# supported product shape, not a broken build: the runtime asks for the BIOS
# and the disc itself on first run. Only a project that HAS recomp-ui can be
# missing them by mistake, so that is the only case that still fails.
if [[ -d "${EXE_DIR}/assets/fonts" && -d "${EXE_DIR}/assets/img" ]]; then
  mkdir -p "${STAGE}/assets"
  cp -a "${EXE_DIR}/assets/fonts" "${STAGE}/assets/"
  cp -a "${EXE_DIR}/assets/img" "${STAGE}/assets/"
  if [[ ! -f "${STAGE}/assets/img/boxart.tga" && -f "${ROOT}/launcher_assets/img/boxart.tga" ]]; then
    cp -a "${ROOT}/launcher_assets/img/boxart.tga" "${STAGE}/assets/img/boxart.tga"
  fi
elif [[ -d "${ROOT}/recomp-ui" ]]; then
  echo "error: ${EXE_DIR}/assets/{fonts,img} missing — rebuild psx-runtime" >&2
  exit 1
else
  echo "no recomp-ui in this project — staging a launcher-less host"
  HAS_LAUNCHER=0
fi

HAS_LAUNCHER="${HAS_LAUNCHER:-1}"

# Exe-relative runtime data (mods catalog, bezels, ...): the runtime resolves
# these from the exe's own directory, so ship them at the stage root exactly
# as the build tree staged them. Two things under mods/ belong to the local
# machine and must never leak into a release: state.toml is this machine's mod
# enable/disable state (preloaded catalogs ship default-disabled), and
# installed/ holds .psxmod archives the developer installed as a player would.
# Only mods/bundled/ is build output, and only build output ships.
copy_runtime_dir() {
  local name="$1" optional="$2"
  if [[ ! -d "${EXE_DIR}/${name}" ]]; then
    if [[ "${optional}" -eq 1 ]]; then
      echo "warn: ${EXE_DIR}/${name} missing -- skipped (--runtime-dir-optional)" >&2
      return 0
    fi
    echo "error: ${EXE_DIR}/${name} missing -- rebuild the runtime target" >&2
    echo "  (build wiring stages it next to the exe; see the title CMakeLists)" >&2
    exit 1
  fi
  mkdir -p "${STAGE}/${name}"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --exclude 'state.toml' --exclude 'state.toml.tmp' \
      --exclude 'installed' --exclude 'installed/***' \
      "${EXE_DIR}/${name}/" "${STAGE}/${name}/"
  else
    cp -a "${EXE_DIR}/${name}/." "${STAGE}/${name}/"
    rm -f "${STAGE}/${name}/state.toml" "${STAGE}/${name}/state.toml.tmp"
    rm -rf "${STAGE}/${name}/installed"
  fi
  echo "staged runtime dir ${name}/"
}

# Drop developer-channel work from a staged catalog tree.
#
# Channels are per FEATURE, so this cannot be a grep: a line-anchored match on
# `channel = "developer"` cannot tell a package-level key from one inside a
# [[feature]] block, and would drop a whole catalog because one instrument in
# it is unfinished. mod_channel_filter.py parses the manifest, removes the
# version directory when nothing in it ships, and otherwise emits a manifest
# without the developer features and the entries that only served them.
#
# Fails closed: if the filter cannot run, packaging stops rather than
# publishing unfinished work.
prune_dev_mods() {  # prune_dev_mods <dir of <id>/<version>/manifest.toml>...
  local python=""
  for candidate in python3 python; do
    if command -v "${candidate}" >/dev/null 2>&1; then python="${candidate}"; break; fi
  done
  if [[ -z "${python}" ]]; then
    echo "error: no python3 on PATH; cannot filter developer-channel mods" >&2
    exit 1
  fi
  if ! "${python}" "${SCRIPT_DIR}/mod_channel_filter.py" "$@"; then
    echo "error: developer-channel filtering failed" >&2
    exit 1
  fi
}

if ((${#RUNTIME_DIRS[@]})); then
  for d in "${RUNTIME_DIRS[@]}"; do
    copy_runtime_dir "${d}" 0
  done
fi
if ((${#RUNTIME_DIRS_OPTIONAL[@]})); then
  for d in "${RUNTIME_DIRS_OPTIONAL[@]}"; do
    copy_runtime_dir "${d}" 1
  done
fi

copy_proj() {
  local rel="$1"
  if [[ -e "${ROOT}/${rel}" ]]; then
    mkdir -p "$(dirname "${STAGE}/${rel}")"
    if [[ -d "${ROOT}/${rel}" ]]; then
      # Merge, never nest: a --runtime-dir of the same name may already have
      # staged both exe-relative data and project-local source data.
      mkdir -p "${STAGE}/${rel}"
      cp -a "${ROOT}/${rel}/." "${STAGE}/${rel}/"
    else
      cp -a "${ROOT}/${rel}" "${STAGE}/${rel}"
    fi
  else
    echo "error: missing ${rel}" >&2
    exit 1
  fi
}

for f in "${PROJECT_FILES[@]}"; do
  copy_proj "${f}"
done
for d in "${PROJECT_DIRS[@]}"; do
  copy_proj "${d}"
done

# Developer-channel pruning runs AFTER every catalog is staged, and covers BOTH
# copies: the runtime catalog under mods/bundled that the launcher lists, and
# the source catalog under mods/preloaded that the setup-host rebuild re-stages
# from. Pruning only the first would publish the package in source form and let
# the player's own rebuild put it straight back. mods/packages is the pre-split
# layout, kept here so an older staged tree is still pruned rather than shipped.
if [[ "${EXCLUDE_DEV_MODS}" -eq 1 ]]; then
  echo "excluding developer-channel mods from this package"
  prune_dev_mods "${STAGE}/mods/bundled" \
                 "${STAGE}/mods/preloaded/packages" \
                 "${STAGE}/mods/packages"
  _dev_left=$( { grep -rlE '^[[:space:]]*channel[[:space:]]*=[[:space:]]*"developer"[[:space:]]*$' \
      "${STAGE}" --include=manifest.toml 2>/dev/null || true; } | wc -l)
  if [[ "${_dev_left}" -ne 0 ]]; then
    echo "error: ${_dev_left} developer manifest(s) survived pruning:" >&2
    grep -rlE '^[[:space:]]*channel[[:space:]]*=[[:space:]]*"developer"[[:space:]]*$' \
        "${STAGE}" --include=manifest.toml >&2 || true
    exit 1
  fi
else
  _dev_in=$( { grep -rlE '^[[:space:]]*channel[[:space:]]*=[[:space:]]*"developer"[[:space:]]*$' \
      "${STAGE}" --include=manifest.toml 2>/dev/null || true; } | wc -l)
  if [[ "${_dev_in}" -ne 0 ]]; then
    echo "note: package includes ${_dev_in} developer-channel manifest(s);" \
         "pass --exclude-dev-mods (or EXCLUDE_DEV_MODS=1) for a public release"
  fi
fi
# A retail PlayStation BIOS dump is Sony's, and a release must never carry one
# -- not from a developer's working tree, not from a submodule's bios/. This is
# not hypothetical: an SCPH1001.BIN dropped into psxrecomp/bios/ during a
# debugging session sat one packaging run away from shipping, and nothing in
# the copy path would have caught it.
#
# Identified by CONTENT, not by name or size. OpenBIOS is the image a release
# exists to ship, it lives at bios/openbios.bin, and it deliberately carries
# "Licensed by Sony Computer Entertainment Inc." for compatibility -- so a name
# match, a size match, or a string match on "Sony" would delete the wrong file.
# Only a retail dump carries a Sony COPYRIGHT line (verified: 0 hits in
# OpenBIOS, 6 in SCPH1001).
scrub_retail_bios() {
  local dir="$1" f
  [[ -d "${dir}" ]] || return 0
  while IFS= read -r -d '' f; do
    if grep -qa 'Copyright.*Sony Computer Entertainment' "${f}" 2>/dev/null; then
      echo "scrub: removed retail BIOS dump from release: ${f#${dir}/}" >&2
      rm -f "${f}"
    fi
  done < <(find "${dir}" -type f -size -1025k -size +255k                 \( -iname '*.bin' -o -iname '*.rom' -o -iname '*.img' \)                 -print0 2>/dev/null)
}

# Project paths a release must NOT carry, removed after the copy because
# copy_proj is a plain cp -a of the working tree. The motivating case: art a
# title bakes from the player's own disc or captures from a running game
# (gitignored, but the working tree has it) sitting inside a --project-dir —
# shipping it is exactly what the bake-from-disc rule exists to prevent, and
# nothing fails when it leaks. Same defensive posture as the .mcd scrub.
for x in "${PROJECT_EXCLUDES[@]}"; do
  rm -rf "${STAGE:?}/${x}"
done
find "${STAGE}" -type d -name '__pycache__' -prune -exec rm -rf {} + \
  2>/dev/null || true
scrub_retail_bios "${STAGE}"

copy_tree_filtered() {
  local src="$1" dest="$2"
  shift 2
  mkdir -p "${dest}"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a "$@" "${src}/" "${dest}/"
  else
    cp -a "${src}/." "${dest}/"
    rm -rf "${dest}/.git" "${dest}/recompiler/build" "${dest}/generated" 2>/dev/null || true
    find "${dest}" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
    find "${dest}" -type d \( -name 'build' -o -name 'build-*' \) -prune -exec rm -rf {} + 2>/dev/null || true
  fi
  # Memory-card images are somebody's saves. Working trees accumulate them
  # (test fixtures, a developer's own card), and a release must carry none.
  # Done here, after BOTH branches, on purpose: the no-rsync fallback above
  # ignores every --exclude it is handed, so a filter argument alone would
  # quietly ship these on any machine without rsync.
  find "${dest}" -type f \( -name '*.mcd' -o -name '*.mcr' \) -delete 2>/dev/null || true
  # Same posture, same reason for being here rather than in a --exclude.
  scrub_retail_bios "${dest}"
}

if [[ ! -d "${ROOT}/psxrecomp" ]]; then
  echo "error: ${ROOT}/psxrecomp missing (expected framework submodule)" >&2
  exit 1
fi
copy_tree_filtered "${ROOT}/psxrecomp" "${STAGE}/psxrecomp" \
  --exclude '.git' \
  --exclude 'recompiler/build' \
  --exclude 'generated' \
  --exclude '__pycache__' \
  --exclude 'build' \
  --exclude 'build-*'

if [[ -d "${ROOT}/recomp-ui" ]]; then
  copy_tree_filtered "${ROOT}/recomp-ui" "${STAGE}/recomp-ui" \
    --exclude '.git' \
    --exclude 'build' \
    --exclude '__pycache__'
fi

# Never ship game generated C or common disc working trees.
rm -rf "${STAGE}/generated" "${STAGE}/bpe" "${STAGE}/motk" "${STAGE}/disc"

STAGE_SDK="${SCRIPT_DIR}/stage_setup_sdk.sh"
if [[ ! -f "${STAGE_SDK}" ]]; then
  echo "error: missing ${STAGE_SDK}" >&2
  exit 1
fi
chmod +x "${STAGE_SDK}" 2>/dev/null || true

stage_args=(
  --stage "${STAGE}"
  --framework "${ROOT}/psxrecomp"
  --search-dir "${EXE_DIR}"
  --search-dir "${BUILD_DIR}"
  --runtime-bin "${RUNTIME_BIN_DIR}"
  --host-exe "${STAGE}/${EXE_BASENAME}"
  --recompiler-build "${RECOMPILER_BUILD}"
)
if [[ "${EMBED_TOOLCHAIN}" -eq 1 ]]; then
  if [[ -z "${PSXRECOMP_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR:-}}}" ]]; then
    echo "error: --embed-toolchain requires PSXRECOMP_TOOLCHAIN_DIR (or TOOLCHAIN_DIR)" >&2
    exit 1
  fi
  stage_args+=(--toolchain-dir "${PSXRECOMP_TOOLCHAIN_DIR:-${TOOLCHAIN_DIR:-${BPE_TOOLCHAIN_DIR}}}")
else
  stage_args+=(--allow-no-toolchain)
fi

bash "${STAGE_SDK}" "${stage_args[@]}"

if [[ "${HAS_LAUNCHER}" == "1" ]]; then
  STEP4="Follow the Generate & rebuild wizard."
else
  STEP4="Nothing else. It does the rest by itself."
fi

if [[ "${OPENBIOS_ONLY}" -eq 1 ]]; then
  BIOS_HINT=" This build uses the OpenBIOS image it ships with; a retail BIOS dump is neither needed nor accepted."
else
  BIOS_HINT=" A retail SCPH-1001 BIOS dump is optional; otherwise OpenBIOS is regenerated locally."
fi

cat >"${STAGE}/README-SETUP.txt" <<EOF
${DISPLAY_NAME} ${VERSION} — setup package
Platform: ${ARTIFACT}

One zip for first install and updates. Does NOT include disc images, retail
BIOS dumps, pre-generated game C, or a portable cmake/clang pack. Emitters
(psxrecomp-game / psxrecomp-bios) and the CLI are inside psxrecomp/.

Standalone:
1. Run ${EXE_BASENAME}. Nothing needs installing first -- the setup brings
   its own compiler and its own Python, and uses yours if you have them.
2. Provide ${DISC_HINT}.${BIOS_HINT}
3. ${STEP4}

THE FIRST RUN TAKES A WHILE - LET IT FINISH.

It downloads a compiler if you have none, translates the game to C from
your disc, and compiles it. Expect minutes, not seconds, and a console
window that sits there working. Every run after that starts immediately.

That wait is the point. This download contains no game code and no game
assets - not the executable, not the artwork, not the font. All of it is
built on your machine, from the disc you already own, and never leaves it.
Shipping a ready-made build would mean shipping Konami's work; this way
nobody does.

YOUR SAVES ARE NOT IN THIS FOLDER

Memory cards, save states and your settings live in

    Documents\\My Games\\${DISPLAY_NAME}

so updating cannot touch them. Extract a new version wherever you like --
over the top of the old one or into a brand new folder -- and your saves
are safe either way. You do not have to copy anything.

UPGRADING FROM AN OLDER VERSION -- READ THIS ONCE

Older versions kept saves inside the game folder. To bring them across,
EXTRACT THIS UPDATE OVER YOUR EXISTING INSTALL. The first launch then finds
your old saves, copies them to Documents, and leaves the originals alone --
you do nothing, and nothing is lost if you change your mind.

If you instead extracted into a brand new folder, the game cannot see your
old saves, because they are still sitting in the old folder. Nothing is
gone. Copy these from the OLD game folder into

    Documents\\My Games\\${DISPLAY_NAME}

    card1.mcd, card2.mcd   (memory cards)
    saves\\                 (or the openbios\\ folder inside it: save states)

After that first launch, this no longer applies: your saves are outside the
game folder for good, and any later update can be extracted anywhere.

Want everything in this folder instead (USB stick, shared machine)? Put an
empty file named portable.txt next to ${EXE_BASENAME}, or set PSX_PORTABLE=1.

RetComM uses this same zip: it harvests emitters into a shared SDK cache,
downloads the toolchain pack (or uses RETCOMM_TOOLCHAIN_DIR), and preserves
saves/user config across updates.
EOF

# --- Gate: the staged tree must be able to configure itself ----------------
# The project file/dir list is an allowlist, so any path a project adds to
# CMakeLists.txt has to be added here too. When that is missed the zip still
# builds and only fails on the user's machine, at cmake configure, with
# "Cannot find source file". Catch it at package time instead.
#
# Only unguarded references are required: a path used solely inside
# if(EXISTS "...") is optional by construction, and a glob or a path built from
# a cmake variable cannot be resolved here.
if [[ -f "${STAGE}/CMakeLists.txt" ]]; then
  cml="${STAGE}/CMakeLists.txt"
  guarded="$(grep -oE 'if\(EXISTS[[:space:]]+"\$\{CMAKE_CURRENT_SOURCE_DIR\}/[^"]+"' "${cml}" \
               | sed -E 's|.*\$\{CMAKE_CURRENT_SOURCE_DIR\}/||; s|"$||' | sort -u)"
  missing_refs=()
  while IFS= read -r rel; do
    [[ -z "${rel}" ]] && continue
    case "${rel}" in *"*"*|*"?"*|*'$'*) continue ;; esac
    if [[ -n "${guarded}" ]] && grep -qxF -- "${rel}" <<<"${guarded}"; then
      continue
    fi
    [[ -e "${STAGE}/${rel}" ]] || missing_refs+=("${rel}")
  done < <(grep -oE '\$\{CMAKE_CURRENT_SOURCE_DIR\}/[^"]+' "${cml}" \
             | sed 's|^\${CMAKE_CURRENT_SOURCE_DIR}/||' | sort -u)
  if (( ${#missing_refs[@]} )); then
    echo "error: CMakeLists.txt references paths that are not staged in the zip:" >&2
    for r in "${missing_refs[@]}"; do echo "  - ${r}" >&2; done
    echo "  add them via --project-file / --project-dir in scripts/package_setup_release.sh" >&2
    exit 1
  fi
fi

# Second gate: the project's own C must be able to include what it includes.
# The CMakeLists check above cannot see this — src/<game>_mods.c pulls in
# "../psx_symbols.h", a compile input nothing in CMakeLists.txt names, and a zip
# missing it fails at the first object file rather than at configure.
#
# Only project-owned staged paths are scanned. Framework trees (psxrecomp/,
# recomp-ui/) resolve their headers through -I and are not ours to police.
missing_incs=()
for rel in "${PROJECT_FILES[@]}" "${PROJECT_DIRS[@]}"; do
  [[ -e "${STAGE}/${rel}" ]] || continue
  while IFS= read -r src; do
    case "${src}" in *.c|*.cc|*.cpp|*.h|*.hpp) ;; *) continue ;; esac
    src_dir="$(dirname "${src}")"
    while IFS= read -r inc; do
      [[ -z "${inc}" ]] && continue
      # Only relative includes can name a file the zip is expected to carry.
      case "${inc}" in /*) continue ;; esac
      target="${src_dir}/${inc}"
      [[ -e "${target}" ]] && continue
      # Resolve against the stage root too (some projects include "x.h" flatly).
      [[ -e "${STAGE}/${inc}" ]] && continue
      # Finally, anything reachable through an -I path: the framework trees are
      # staged whole, so a bare "mod_plugins.h" resolves out of
      # psxrecomp/runtime/include. Match on basename — deliberately loose, since
      # a false pass here only forfeits a warning while a false failure would
      # block a good release.
      inc_base="${inc##*/}"
      if find "${STAGE}" -name "${inc_base}" -type f -print -quit 2>/dev/null | grep -q .; then
        continue
      fi
      rel_src="${src#"${STAGE}"/}"
      missing_incs+=("${inc}  (included by ${rel_src})")
    done < <(grep -oE '^[[:space:]]*#[[:space:]]*include[[:space:]]+"[^"]+"' "${src}" 2>/dev/null \
               | sed -E 's|.*"([^"]+)".*|\1|')
  done < <(find "${STAGE}/${rel}" -type f 2>/dev/null)
done
if (( ${#missing_incs[@]} )); then
  echo "error: staged project sources include files that are not in the zip:" >&2
  printf '  - %s\n' "${missing_incs[@]}" >&2
  echo "  add them via --project-file / --project-dir in scripts/package_setup_release.sh" >&2
  exit 1
fi

# --- Windows: Authenticode-sign what the player will run -------------------
# The host exe, its DLLs, and the emitters. A no-op without a certificate in
# the environment (see tools/ci/sign_windows.sh); fatal if one is configured
# and signing fails. Note the limit of what signing can do for a setup-host
# title: the game exe the player's own machine builds after Generate is not
# this file and carries no signature -- Smart App Control judges that build
# on its own, and only the host/wizard, emitters and DLLs shipped here are
# covered.
if [[ "${EXE_BASENAME}" == *.exe ]]; then
  SIGN_SH="${SCRIPT_DIR}/ci/sign_windows.sh"
  if [[ -f "${SIGN_SH}" ]]; then
    bash "${SIGN_SH}" "${STAGE}"
  else
    echo "note: ${SIGN_SH} missing; Windows binaries ship unsigned" >&2
  fi
fi

find "${STAGE}" -exec touch -c {} + 2>/dev/null || find "${STAGE}" -exec touch {} +

(
  cd "${STAGE}"
  if command -v zip >/dev/null 2>&1; then
    zip -r -q "${DIST}/${ZIP_NAME}" .
  elif command -v 7z >/dev/null 2>&1; then
    7z a -tzip -bso0 -bsp0 "${DIST}/${ZIP_NAME}" . >/dev/null
  elif command -v powershell.exe >/dev/null 2>&1; then
    # The release target is Windows, where `zip` is not standard — MSYS ships
    # it only if selected, so requiring it made the last step of a release fail
    # on an otherwise complete stage. Compress-Archive is always present.
    # -Force overwrites a stale archive from a previous run of the same tag.
    rm -f "${DIST}/${ZIP_NAME}"
    powershell.exe -NoProfile -NonInteractive -Command \
      "Compress-Archive -Path '.\\*' -DestinationPath '$(cygpath -w "${DIST}/${ZIP_NAME}" 2>/dev/null || echo "${DIST}/${ZIP_NAME}")' -Force" \
      || { echo "error: Compress-Archive failed" >&2; exit 1; }
  else
    echo "error: no zip, 7z, or powershell.exe available to build the archive" >&2
    exit 1
  fi
)

echo "Wrote ${DIST}/${ZIP_NAME}"
du -h "${DIST}/${ZIP_NAME}"
