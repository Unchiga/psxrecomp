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

    /* Optional UI copy overrides (NULL → generic defaults). */
    const char* prepare_note;
    const char* prepare_note_windows;
    const char* prepare_note_no_cmake;
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
 * On Windows the build half schedules a helper and asks the caller to exit --
 * the running .exe cannot be relinked while it holds its own file. That is
 * reported through out_exe being empty on an otherwise successful return, the
 * same signal the launcher acts on. */
int psxrecomp_codegen_host_generate_and_build(
    const char* disc_path, char* out_exe, size_t out_cap,
    char* err_msg, size_t err_cap,
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
