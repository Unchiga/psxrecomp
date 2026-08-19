/* psxrecomp_launcher_compat.h — the three recomp-ui symbols the codegen host
 * actually needs, for builds that have no recomp-ui.
 *
 * The setup host — disc, generate, rebuild, relaunch — was written against
 * recomp-ui and so was compiled only under PSX_RECOMP_UI, which made a
 * launcher a hard requirement for shipping a "bring your own disc" build.
 * That requirement turned out to be one #include deep. Of
 * psxrecomp_codegen_host.c's ~3,950 lines, exactly three things come from the
 * launcher:
 *
 *   1. RecompLauncherCPrepareProgressFn, a progress callback typedef;
 *   2. recomp_launcher_relaunch_exe(), one call, asking "which binary do I
 *      restart?";
 *   3. RecompLauncherCGameInfo, used ONLY by psxrecomp_codegen_host_apply(),
 *      which is the launcher-facing function and is compiled out without it.
 *
 * Everything else — toolchain bootstrap, codegen, cmake, PGO, forwarding — is
 * plain C with no UI in it at all. This header supplies (1) and (2) so a
 * runtime can drive the same engine from its own first-run path.
 *
 * With recomp-ui present nothing here is used: psxrecomp_codegen_host.h
 * includes the real recomp_launcher.h instead, and these declarations must
 * stay compatible with it.
 */
#ifndef PSXRECOMP_LAUNCHER_COMPAT_H
#define PSXRECOMP_LAUNCHER_COMPAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Progress during a long step. `frac` is 0..1, or negative for "indeterminate,
 * status only". `status` may be NULL, meaning "keep the previous message". */
typedef void (*RecompLauncherCPrepareProgressFn)(void* ctx, float frac,
                                                 const char* status);

/* Path of the binary to restart after a rebuild. The launcher answers this
 * with the shell it was started from; without one the honest answer is this
 * process's own image, which the host then overrides with the freshly built
 * product exe when it has one. Returns 0 and leaves `out` empty on failure. */
int recomp_launcher_relaunch_exe(char* out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_LAUNCHER_COMPAT_H */
