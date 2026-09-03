/* Portable setup host: disc → generate → cmake/PGO rebuild → relaunch.
 *
 * Two ways in:
 *
 *   With recomp-ui — games fill PsxrecompCodegenHostConfig and call
 *   psxrecomp_codegen_host_apply() when building RecompLauncherCGameInfo, and
 *   the launcher's first-run wizard drives everything.
 *
 *   Without it — games call psxrecomp_codegen_host_init() once, then
 *   psxrecomp_codegen_host_generate_and_build() from their own first-run path.
 *   Same engine, no launcher; see psxrecomp_launcher_compat.h for why that is
 *   only three symbols wide.
 */
#ifndef PSXRECOMP_CODEGEN_HOST_H
#define PSXRECOMP_CODEGEN_HOST_H

#include <stddef.h>

#if defined(PSX_HAS_RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#else
#include "psxrecomp_launcher_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PsxrecompCodegenHostConfig {
    const char* display_name;

    /* Env vars (optional). Defaults: PSXRECOMP_PROJECT_ROOT / PSXRECOMP_BUILD_DIR /
     * PSXRECOMP_FORCE_SETUP when NULL. */
    const char* project_root_env;
    const char* build_dir_env;
    const char* force_setup_env;

    /* Paths relative to project root. NULL → defaults below. */
    const char* psxrecomp_cli_relpath; /* default: psxrecomp/psxrecomp_cli.py */
    const char* seed_cfg_relpath;      /* default: game.toml (root probe) */
    const char* game_toml_relpath;     /* default: game.toml */
    const char* gen_marker_relpath;    /* default: generated/SLUS_011.89_dispatch.c */
    const char* build_dir_name;        /* default: build */

    /* CMake / binary identity (required for auto-rebuild). */
    const char* cmake_target;  /* e.g. psx-runtime */
    const char* exe_basename;  /* no .exe */

    /* Updates (optional; NULL update_repo disables the whole feature).
     *
     * update_repo is "owner/name" on GitHub. The newest release there is
     * compared against the local VERSION file, and its asset whose name ends
     * with update_asset_suffix is what gets installed.
     *
     * This works because a setup-host zip carries SOURCE ONLY: no generated/,
     * no disc, no saves, no settings, no build tree. Unpacking one over an
     * existing install therefore replaces exactly the files that should be
     * replaced and cannot touch anything the player produced. A title whose
     * zip does not have that property must not set these. */
    const char* update_repo;   /* e.g. "owner/MyGameRecomp" */
    /* printf format for the release asset, given the version with no leading
     * "v" -- e.g. "mygame-%s-win-x64.zip". A format rather than a suffix so the
     * download URL can be built from the tag alone, with no second request to
     * list a release's assets. Rename an asset and update this together. */
    const char* update_asset_format;

    /* Optional UI copy overrides (NULL → generic defaults). */
    const char* prepare_note;
    const char* prepare_note_windows;
    const char* prepare_note_no_cmake;

    /* Non-zero: this title runs the bundled OpenBIOS and nothing else. The
     * first-run wizard shows no BIOS step, setup never adopts a retail dump
     * found beside the install, and generate is never handed --bios. Pair it
     * with PSXRECOMP_BIOS_STEMS=OpenBIOS in the title's CMakeLists so the
     * product build links only that backend; the runtime then hides its BIOS
     * row and ignores any bios.cfg by itself (see resolve_bios_for_runtime).
     * Appended last so older config initialisers keep their layout. */
    int openbios_only;
} PsxrecompCodegenHostConfig;

/* Bring the host up: find the project root, the psxrecomp CLI and game.toml,
 * and put the portable toolchain on PATH. Call once before generating.
 *
 *   1  ready — generate and rebuild will run
 *   0  no project root
 *  -1  project root found, but no psxrecomp CLI in it
 *  -2  no game.toml
 *
 * Anything but 1 means setup cannot proceed; the value says which input is
 * missing, so a caller can tell the player what to fix rather than reporting a
 * bare failure. */
int psxrecomp_codegen_host_init(const PsxrecompCodegenHostConfig* cfg);

/* The whole first run in one call: generate C from `disc_path`, then build it.
 * On success fills out_exe with the product binary. On failure fills err_msg
 * and returns 0. `on_progress` may be NULL.
 *
 * On Windows the build half does not build: the running .exe holds its own file
 * and cannot be relinked, so it writes a helper .cmd and asks the caller to
 * exit. It reports that by returning THE HELPER'S PATH in out_exe, not the
 * product binary. Hand that to psxrecomp_codegen_host_relaunch_or_exit(), which
 * starts the helper in its own console; the helper waits for this process to
 * die, builds, and launches the product itself. */
int psxrecomp_codegen_host_generate_and_build(
    const char* disc_path, char* out_exe, size_t out_cap,
    char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx);

/* Is there a newer release than this install?
 *
 *   1  yes -- local and remote are filled with the two versions
 *   0  no, or unknown: no update_repo, no VERSION file, offline, or the
 *      answer was cached as "up to date" less than 24h ago
 *
 * Answers from a cache written beside the executable, refreshed at most once
 * a day, so this costs nothing on a normal launch -- startup latency is not
 * something to spend on a check that changes at most weekly. `force` skips
 * the cache. Set <FORCE_SETUP_ENV-prefix>_SKIP_UPDATE=1 to disable entirely.
 *
 * Requires psxrecomp_codegen_host_init() to have succeeded. */
int psxrecomp_codegen_host_update_check(char* local_ver, size_t local_cap,
                                        char* remote_ver, size_t remote_cap,
                                        int force);

/* Download the newest release and install it over this tree.
 *
 * Returns 1 having SCHEDULED the work, not having finished it: the update
 * replaces the running executable, which Windows will not permit in place, so
 * a helper does the unpack, the regenerate, the rebuild and the relaunch after
 * this process exits. Hand out_helper to
 * psxrecomp_codegen_host_relaunch_or_exit() and quit. On failure fills err_msg
 * and returns 0, having changed nothing.
 *
 * The regenerate is not optional: an update can change the recompiler, and
 * generate is cheap when its output already matches. It needs the disc, which
 * the player already chose. */
int psxrecomp_codegen_host_update_apply(
    char* out_helper, size_t helper_cap, char* err_msg, size_t err_cap,
    RecompLauncherCPrepareProgressFn on_progress, void* progress_ctx);

#if defined(PSX_HAS_RECOMP_LAUNCHER)
void psxrecomp_codegen_host_apply(RecompLauncherCGameInfo* gi,
                                  const PsxrecompCodegenHostConfig* cfg);
#endif

int psxrecomp_codegen_host_sources_missing(
    const PsxrecompCodegenHostConfig* cfg);

void psxrecomp_codegen_host_relaunch_or_exit(const char* disc_path);

/* Setup-host only (no PSX_HAS_GAME_DISPATCH): if generated/ is present and
 * build-<dir>/<exe> exists, exec that binary (product tree with bios/, mods/,
 * assets/, settings) and do not return. Full builds are a no-op.
 * Skip with PSXRECOMP_NO_FORWARD=1 or the title force-setup env (=1). */
void psxrecomp_codegen_host_forward_if_built(
    const PsxrecompCodegenHostConfig* cfg, int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CODEGEN_HOST_H */
