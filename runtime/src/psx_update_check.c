/* psx_update_check.c — "is there a newer release?", off the boot path.
 *
 * See psx_update_check.h for the contract. Windows-only transport (WinHTTP,
 * part of the OS — no new dependency, no bundled TLS stack to keep patched);
 * elsewhere the check compiles to nothing rather than pretending to work.
 */

#include "psx_update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

/* ---- version comparison -------------------------------------------------- */

static const char *ver_skip_v(const char *s) {
    if (!s) return "";
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 'v' || *s == 'V') s++;
    return s;
}

int psx_update_version_cmp(const char *a, const char *b) {
    const char *pa = ver_skip_v(a), *pb = ver_skip_v(b);
    for (;;) {
        long va = 0, vb = 0;
        int da = 0, db = 0;
        while (*pa >= '0' && *pa <= '9') { va = va * 10 + (*pa++ - '0'); da = 1; }
        while (*pb >= '0' && *pb <= '9') { vb = vb * 10 + (*pb++ - '0'); db = 1; }
        /* Numeric per component. A plain strcmp sorts 0.2.10 BEFORE 0.2.9 and
         * would tell everyone on .10 that .9 is an upgrade, forever. */
        if (va != vb) return (va < vb) ? -1 : 1;
        if (!da && !db) return 0;          /* no numeric components left */
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
        if (!*pa && !*pb) return 0;
        /* "1.2" vs "1.2.1": the side that ran out reads as 0 next pass, so the
         * shorter version is older, which is what a missing patch level means. */
    }
}

#if defined(_WIN32) && defined(PSX_UPDATE_REPO)

#include <windows.h>
#include <winhttp.h>

/* Result, published by the worker thread and consumed once by the host. */
static SDL_AtomicInt s_ready;      /* 0 = nothing to report, 1 = newer found */
static SDL_AtomicInt s_taken;
static char          s_tag[64];
static char          s_url[512];
static char          s_current[64];

/* Minimal field pluck. The response is a single GitHub release object and we
 * need two string fields from it; a JSON parser would be a dependency and a
 * parsing surface for a remote document, for no gain. Anything unexpected
 * simply fails to match and the check goes quiet. */
static int json_str_field(const char *body, const char *key,
                          char *out, int cap) {
    char pat[64];
    const char *p, *q;
    int n;
    if (!body || !key || !out || cap <= 1) return 0;
    n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pat)) return 0;
    p = strstr(body, pat);
    if (!p) return 0;
    p += n;
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    q = p;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) q++;   /* skip escapes; we want plain values */
        q++;
    }
    if (*q != '"') return 0;
    n = (int)(q - p);
    if (n <= 0 || n >= cap) return 0;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
    return 1;
}

static int http_get(const wchar_t *host, const wchar_t *path,
                    char *out, int cap) {
    HINTERNET ses = NULL, con = NULL, req = NULL;
    int ok = 0, len = 0;
    DWORD status = 0, slen = sizeof(status);

    ses = WinHttpOpen(L"psxrecomp-update-check/1.0",
                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) goto done;
    /* Bounded on every phase. An unreachable or black-holed host must end the
     * thread, not leave it parked for the life of the process. */
    WinHttpSetTimeouts(ses, 5000, 5000, 8000, 8000);

    con = WinHttpConnect(ses, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!con) goto done;
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto done;
    /* GitHub rejects requests without a User-Agent. */
    if (!WinHttpAddRequestHeaders(req,
            L"Accept: application/vnd.github+json\r\n", (DWORD)-1,
            WINHTTP_ADDREQ_FLAG_ADD)) goto done;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
    if (!WinHttpReceiveResponse(req, NULL)) goto done;
    if (!WinHttpQueryHeaders(req,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX))
        goto done;
    if (status != 200) goto done;

    for (;;) {
        DWORD avail = 0, got = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        if (len + (int)avail >= cap) avail = (DWORD)(cap - len - 1);
        if (avail == 0) break;             /* response larger than the buffer */
        if (!WinHttpReadData(req, out + len, avail, &got) || got == 0) break;
        len += (int)got;
    }
    out[len] = '\0';
    ok = (len > 0);

done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

static int update_thread(void *unused) {
    static char body[65536];
    char tag[64], url[512];
    (void)unused;

    /* PSX_UPDATE_REPO is a narrow literal from the build system, so the path
     * is built and widened at runtime -- it cannot be pasted into a wide
     * literal. */
    {
        char  npath[256];
        wchar_t wpath[256];
        snprintf(npath, sizeof(npath), "/repos/%s/releases/latest",
                 PSX_UPDATE_REPO);
        if (MultiByteToWideChar(CP_UTF8, 0, npath, -1, wpath,
                                (int)(sizeof(wpath) / sizeof(wpath[0]))) == 0)
            return 0;
        if (!http_get(L"api.github.com", wpath, body, (int)sizeof(body)))
            return 0;
    }

    if (!json_str_field(body, "tag_name", tag, (int)sizeof(tag)))
        return 0;
    /* A draft or prerelease is not something to push at players. */
    if (strstr(body, "\"prerelease\": true") || strstr(body, "\"prerelease\":true"))
        return 0;
    if (strstr(body, "\"draft\": true") || strstr(body, "\"draft\":true"))
        return 0;
    if (psx_update_version_cmp(tag, s_current) <= 0)
        return 0;                          /* current, or somehow older */

    if (!json_str_field(body, "html_url", url, (int)sizeof(url)))
        snprintf(url, sizeof(url), "https://github.com/%s/releases/latest",
                 PSX_UPDATE_REPO);

    snprintf(s_tag, sizeof(s_tag), "%s", tag);
    snprintf(s_url, sizeof(s_url), "%s", url);
    SDL_SetAtomicInt(&s_ready, 1);
    return 0;
}

void psx_update_check_start(const char *current_version) {
    SDL_Thread *t;
    /* NULL means "whatever this build was stamped with". PSX_GAME_VERSION is
     * defined for THIS translation unit only - the build keeps it off the
     * target-wide compile line so a version bump does not rebuild every
     * shard - so main.cpp cannot pass it and does not have to know it. */
    if (!current_version || !current_version[0]) current_version = PSX_GAME_VERSION;
    if (!current_version || !current_version[0]) return;
    /* An unreleased build has no comparable version; checking would prompt on
     * every launch of every dev build. */
    if (!strcmp(current_version, "dev")) return;
    snprintf(s_current, sizeof(s_current), "%s", current_version);
    t = SDL_CreateThread(update_thread, "psx-update-check", NULL);
    /* Detached: nothing joins it, and it must not hold up shutdown. */
    if (t) SDL_DetachThread(t);
}

const char *psx_update_check_current_version(void) { return PSX_GAME_VERSION; }

int psx_update_check_take(char *out_tag, int tag_cap,
                          char *out_url, int url_cap) {
    if (!SDL_GetAtomicInt(&s_ready)) return 0;
    if (SDL_SetAtomicInt(&s_taken, 1)) return 0;   /* already reported once */
    if (out_tag && tag_cap > 0) snprintf(out_tag, (size_t)tag_cap, "%s", s_tag);
    if (out_url && url_cap > 0) snprintf(out_url, (size_t)url_cap, "%s", s_url);
    return 1;
}

#else  /* no repo configured, or not Windows */

void psx_update_check_start(const char *current_version) { (void)current_version; }

const char *psx_update_check_current_version(void) {
#ifdef PSX_GAME_VERSION
    return PSX_GAME_VERSION;
#else
    return "dev";
#endif
}

int psx_update_check_take(char *out_tag, int tag_cap,
                          char *out_url, int url_cap) {
    (void)out_tag; (void)tag_cap; (void)out_url; (void)url_cap;
    return 0;
}

#endif
