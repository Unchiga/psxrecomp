#ifndef PSX_UPDATE_CHECK_H
#define PSX_UPDATE_CHECK_H

/* Release-update check against the project's GitHub releases.
 *
 * Runs on its OWN thread and never blocks the boot path. A synchronous network
 * call at startup is a launch-time stall waiting to happen -- this runtime has
 * already shipped one 42-second startup hang from a blocking enumeration, and
 * a player on a bad connection would get the same symptom from a DNS timeout.
 * Nothing here is on the critical path: if the request is slow, fails, or is
 * never answered, the game has already started and the player sees nothing.
 *
 * The module reports; it does not act. It never downloads, never writes to the
 * install, and never opens anything. The host polls _take() and decides what to
 * show, which keeps the "should we interrupt the player" judgement in one place
 * with the other dialogs.
 *
 * Disabled at compile time when PSX_UPDATE_REPO is undefined, so a title that
 * has no releases to check makes no network access at all.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Start the check. `current_version` is what this build believes it is
 * (PSX_GAME_VERSION); a NULL/empty/"dev" version disables the check, because
 * "dev" cannot be meaningfully compared to a release tag and would prompt on
 * every launch of an unreleased build. Safe to call once per process. */
void psx_update_check_start(const char *current_version);

/* 1 exactly once, when a NEWER release than `current_version` was found.
 * `out_tag` receives the release tag (e.g. "v0.2.3") and `out_url` the page to
 * send the player to. Both buffers are filled only on a 1 return. Returns 0
 * while the check is still running, when it failed, and when the installed
 * version is already current -- the caller cannot tell those apart on purpose:
 * every one of them means "say nothing". */
int psx_update_check_take(char *out_tag, int tag_cap,
                          char *out_url, int url_cap);

/* Compare two dotted versions ("0.2.10" > "0.2.9"). Numeric per component, so
 * it does not repeat the classic string-compare bug where 0.2.10 sorts before
 * 0.2.9. A leading 'v' is ignored on either side. Returns <0, 0, >0. Exposed
 * for tests. */
int psx_update_version_cmp(const char *a, const char *b);

/* The version string this build was stamped with, for messages. Never NULL;
 * "dev" for an unreleased build. */
const char *psx_update_check_current_version(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_UPDATE_CHECK_H */
