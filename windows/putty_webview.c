/*
 * putty_webview.c - Modern Web-based frontend host for PuTTY / Plink core.
 * Phase 3: Full-duplex SSH & Serial backend integration with ttyd protocol.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdbool.h>
#include <limits.h>

#include "putty.h"
#include "storage.h"
#include "ssh.h"
#include "putty-rc.h"
#include "win-gui-seat.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <aclapi.h>
#include <sddl.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#include "webview/webview_host.h"

/* appname is generated in be_list.c by be_list() macro */

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_PORT_ARG |
    TOOLTYPE_NO_VERBOSE_OPTION;

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = true;

HINSTANCE hinst;

typedef struct WebViewWindow {
    HWND hwnd;
    struct WebViewWindow *next;
} WebViewWindow;

static WebViewWindow *windows_head = NULL;
static wchar_t global_html_path[MAX_PATH] = {0};
static wchar_t global_editor_html_path[MAX_PATH] = {0};
static wchar_t target_web_dir[MAX_PATH] = {0};

#define MEM_LOG_MAX_LINES 1000
static char *g_mem_log_ring[MEM_LOG_MAX_LINES] = {0};
static size_t g_mem_log_head = 0;
static size_t g_mem_log_count = 0;
static CRITICAL_SECTION g_mem_log_cs;
static volatile LONG g_mem_log_cs_state = 0;

static void mem_log_ensure_init(void)
{
    if (InterlockedCompareExchange(&g_mem_log_cs_state, 1, 0) == 0) {
        InitializeCriticalSection(&g_mem_log_cs);
        InterlockedExchange(&g_mem_log_cs_state, 2);

        // Clean up any stale debug log file left on disk
        char temp[MAX_PATH];
        if (GetTempPathA(MAX_PATH, temp)) {
            char old_log[MAX_PATH];
            snprintf(old_log, sizeof(old_log), "%sputty_webview_debug.log", temp);
            DeleteFileA(old_log);
        }
    } else {
        while (g_mem_log_cs_state != 2) {
            Sleep(1);
        }
    }
}

static void dbg_log(const char *fmt, ...)
{
    mem_log_ensure_init();

    char buf[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int prefix_len = snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ",
                              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    int needed = vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, ap);
    va_end(ap);

    char *allocated = NULL;
    char *final_str = buf;
    if (needed >= (int)(sizeof(buf) - prefix_len)) {
        size_t total_sz = prefix_len + needed + 1;
        allocated = (char*)malloc(total_sz);
        if (allocated) {
            memcpy(allocated, buf, prefix_len);
            va_start(ap, fmt);
            vsnprintf(allocated + prefix_len, needed + 1, fmt, ap);
            va_end(ap);
            final_str = allocated;
        }
    }

    OutputDebugStringA(final_str);
    OutputDebugStringA("\n");

    EnterCriticalSection(&g_mem_log_cs);
    if (g_mem_log_ring[g_mem_log_head]) {
        free(g_mem_log_ring[g_mem_log_head]);
        g_mem_log_ring[g_mem_log_head] = NULL;
    }
    g_mem_log_ring[g_mem_log_head] = allocated ? allocated : _strdup(final_str);
    g_mem_log_head = (g_mem_log_head + 1) % MEM_LOG_MAX_LINES;
    if (g_mem_log_count < MEM_LOG_MAX_LINES) {
        g_mem_log_count++;
    }
    LeaveCriticalSection(&g_mem_log_cs);
}

static void mem_log_dump_to_strbuf(strbuf *sb)
{
    mem_log_ensure_init();
    EnterCriticalSection(&g_mem_log_cs);
    if (g_mem_log_count == 0) {
        const char *empty_msg = "(暂无宿主内存日志)\n";
        put_data(sb, empty_msg, strlen(empty_msg));
    } else {
        size_t start = (g_mem_log_count < MEM_LOG_MAX_LINES) ? 0 : g_mem_log_head;
        for (size_t i = 0; i < g_mem_log_count; i++) {
            size_t idx = (start + i) % MEM_LOG_MAX_LINES;
            if (g_mem_log_ring[idx]) {
                put_data(sb, g_mem_log_ring[idx], strlen(g_mem_log_ring[idx]));
                put_byte(sb, '\n');
            }
        }
    }
    LeaveCriticalSection(&g_mem_log_cs);
}

#define IDR_WEB_INDEX_HTML          2001
#define IDR_WEB_XTERM_JS            2002
#define IDR_WEB_XTERM_CSS           2003
#define IDR_WEB_FIT_ADDON_JS        2004
#define IDR_WEB_WEBLINKS_ADDON_JS   2005
#define IDR_WEB_EDITOR_HTML         2006

struct EmbeddedAsset {
    const wchar_t *filename;
    int res_id;
};

static const struct EmbeddedAsset embedded_assets[] = {
    { L"index.html", IDR_WEB_INDEX_HTML },
    { L"xterm.js", IDR_WEB_XTERM_JS },
    { L"xterm.css", IDR_WEB_XTERM_CSS },
    { L"xterm-addon-fit.js", IDR_WEB_FIT_ADDON_JS },
    { L"xterm-addon-web-links.js", IDR_WEB_WEBLINKS_ADDON_JS },
    { L"editor.html", IDR_WEB_EDITOR_HTML },
};
#define NUM_EMBEDDED_ASSETS (sizeof(embedded_assets) / sizeof(embedded_assets[0]))

static bool extract_resource_to_file(int res_id, const wchar_t *filepath)
{
    HRSRC hrsrc = FindResourceW(hinst, MAKEINTRESOURCEW(res_id), MAKEINTRESOURCEW(10));
    if (!hrsrc) return false;
    HGLOBAL hg = LoadResource(hinst, hrsrc);
    if (!hg) return false;
    const void *data = LockResource(hg);
    DWORD size = SizeofResource(hinst, hrsrc);
    if (!data || size == 0) return false;

    HANDLE hf = CreateFileW(filepath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hf, data, size, &written, NULL);
    CloseHandle(hf);
    return ok && (written == size);
}

static bool create_directory_recursive(const wchar_t *dir)
{
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, dir, MAX_PATH - 1);
    tmp[MAX_PATH - 1] = L'\0';

    for (wchar_t *p = tmp + 1; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            *p = L'\0';
            CreateDirectoryW(tmp, NULL);
            *p = L'\\';
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void ensure_webview_assets(wchar_t *out_html_path, wchar_t *out_editor_path, size_t max_len)
{
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wchar_t *last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash) *last_slash = L'\0';

    /* 1. Check development tree fallback (source checkout) FIRST so dev edits are instant */
    wchar_t dev_html[MAX_PATH];
    _snwprintf(dev_html, MAX_PATH, L"%s\\..\\..\\windows\\webview\\web\\index.html", exe_path);
    if (GetFileAttributesW(dev_html) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy(out_html_path, dev_html, max_len - 1);
        out_html_path[max_len - 1] = L'\0';
        _snwprintf(out_editor_path, max_len, L"%s\\..\\..\\windows\\webview\\web\\editor.html", exe_path);
        return;
    }

    /* 2. Deployed standalone mode: ensure target directory exists */
    _snwprintf(target_web_dir, MAX_PATH, L"%s\\webview\\web", exe_path);
    _snwprintf(out_html_path, max_len, L"%s\\index.html", target_web_dir);
    _snwprintf(out_editor_path, max_len, L"%s\\editor.html", target_web_dir);

    bool dir_ok = create_directory_recursive(target_web_dir);
    if (!dir_ok) {
        /* If exe directory is read-only (e.g. Program Files), fall back to %LOCALAPPDATA% */
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0) {
            _snwprintf(target_web_dir, MAX_PATH, L"%s\\PuTTY-WebView\\webview\\web", appdata);
            create_directory_recursive(target_web_dir);
            _snwprintf(out_html_path, max_len, L"%s\\index.html", target_web_dir);
            _snwprintf(out_editor_path, max_len, L"%s\\editor.html", target_web_dir);
        }
    }

    /* 3. Fast synchronous verification & sync of embedded assets (< 1ms total)
     * Automatically updates disk files if exe is upgraded, without requiring manual deletion of webview/ */
    for (size_t i = 0; i < NUM_EMBEDDED_ASSETS; i++) {
        HRSRC hrsrc = FindResourceW(hinst, MAKEINTRESOURCEW(embedded_assets[i].res_id), MAKEINTRESOURCEW(10));
        if (!hrsrc) continue;
        DWORD res_size = SizeofResource(hinst, hrsrc);
        if (res_size == 0) continue;

        wchar_t filepath[MAX_PATH];
        _snwprintf(filepath, MAX_PATH, L"%s\\%s", target_web_dir, embedded_assets[i].filename);

        bool needs_update = false;
        HANDLE hf = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            needs_update = true;
        } else {
            LARGE_INTEGER fsize;
            if (!GetFileSizeEx(hf, &fsize) || (DWORD)fsize.QuadPart != res_size) {
                needs_update = true;
            }
            CloseHandle(hf);
        }

        if (needs_update) {
            dbg_log("Asset sync: extracting %ls (%u bytes)", embedded_assets[i].filename, res_size);
            extract_resource_to_file(embedded_assets[i].res_id, filepath);
        }
    }
}

#define REPLAY_BUF_SIZE (64 * 1024)

typedef struct WebViewSession {
    int id;
    char name[128];
    Conf *cfg;
    Backend *backend;
    LogContext *logctx;
    WinGuiSeat wgs;
    HWND hwnd; // Current window hosting this session
    char replay_buf[REPLAY_BUF_SIZE];
    size_t replay_len;
    size_t replay_head;
    bufchain send_queue;
    int send_rate_limit;
    unsigned long send_next_tick;
    bool send_timer_active;
    FILE *log_fp;
    bool is_logging;
    bool log_enabled;
    char log_filename[MAX_PATH];
    bool is_connected;
    bool in_backend_free;
    bool in_connecting;
    bool auto_reconnect;
    bool reconnect_timer_active;
    int reconnect_countdown;
    unsigned long reconnect_next_tick;
    unsigned long reconnect_target_tick;

    /* SSH Interactive prompt state & password vault context */
    prompts_t *cur_prompts;
    size_t cur_prompt_idx;
    strbuf *prompt_input_buf;
    char temp_prompt_user[128];
    char temp_prompt_pass[256];
    char active_auth_pass[256];
    bool tried_saved_pw;
    bool modal_remember;
    bool modal_autologin;
    bool login_failed;
    int prompt_attempts;
    bool username_displayed;

    struct WebViewSession *next;
} WebViewSession;

static WebViewSession *sessions_head = NULL;
static int next_session_id = 1;

static bool session_is_connected(WebViewSession *sess)
{
    if (!sess) return false;
    if (!sess->is_connected) return false;
    if (!sess->backend) return false;
    return backend_connected(sess->backend);
}

static WebViewSession *session_from_seat(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return container_of(wgs, WebViewSession, wgs);
}

static WebViewSession *session_find(int id)
{
    for (WebViewSession *s = sessions_head; s; s = s->next) {
        if (s->id == id)
            return s;
    }
    return NULL;
}

static unsigned session_send_interval_ticks(int rate_limit)
{
    if (rate_limit <= 0) return 0;
    return 1 + (TICKSPERSEC - 1) / (unsigned)rate_limit;
}

static bool session_send_tick_due(unsigned long now, unsigned long due)
{
    return now - due < INT_MAX;
}

static void session_send_queue_try(WebViewSession *sess);

static void session_send_queue_timer(void *ctx, unsigned long now)
{
    WebViewSession *sess = (WebViewSession *)ctx;
    sess->send_timer_active = false;
    session_send_queue_try(sess);
}

static void session_send_queue_arm(WebViewSession *sess, unsigned long now)
{
    if (sess->send_timer_active)
        return;

    unsigned long wait = session_send_tick_due(now, sess->send_next_tick) ?
        1 : sess->send_next_tick - now;
    sess->send_timer_active = true;
    schedule_timer(wait, session_send_queue_timer, sess);
}

static void session_send_queue_try(WebViewSession *sess)
{
    if (!sess || !sess->backend || !backend_connected(sess->backend) || !backend_sendok(sess->backend))
        return;

    while (bufchain_size(&sess->send_queue) > 0 && backend_sendok(sess->backend)) {
        ptrlen pl = bufchain_prefix(&sess->send_queue);
        size_t len;
        assert(pl.len > 0);

        if (sess->send_rate_limit > 0) {
            unsigned long now = GETTICKCOUNT();
            if (!session_send_tick_due(now, sess->send_next_tick)) {
                session_send_queue_arm(sess, now);
                return;
            }
            len = 1;
            if (sess->send_rate_limit > (int)TICKSPERSEC) {
                len = (size_t)(sess->send_rate_limit / TICKSPERSEC);
            }
        } else {
            len = pl.len;
        }

        if (len > pl.len)
            len = pl.len;

        backend_send(sess->backend, pl.ptr, len);
        bufchain_consume(&sess->send_queue, len);

        if (sess->send_rate_limit > 0) {
            sess->send_next_tick = GETTICKCOUNT() + session_send_interval_ticks(sess->send_rate_limit);
            if (bufchain_size(&sess->send_queue) > 0) {
                session_send_queue_arm(sess, GETTICKCOUNT());
                return;
            }
        }
    }
}

static void session_send(WebViewSession *sess, const void *data, size_t len)
{
    if (!sess || !sess->backend || !data || len == 0)
        return;

    if (sess->send_rate_limit <= 0 && bufchain_size(&sess->send_queue) == 0) {
        if (backend_connected(sess->backend) && backend_sendok(sess->backend)) {
            backend_send(sess->backend, data, len);
            return;
        }
    }

    bufchain_add(&sess->send_queue, data, len);
    session_send_queue_try(sess);
}

static void session_record_output(WebViewSession *sess, const void *data, size_t len)
{
    const char *src = (const char *)data;
    for (size_t i = 0; i < len; i++) {
        sess->replay_buf[sess->replay_head] = src[i];
        sess->replay_head = (sess->replay_head + 1) % REPLAY_BUF_SIZE;
        if (sess->replay_len < REPLAY_BUF_SIZE)
            sess->replay_len++;
    }
}

static void session_replay_output(HWND target_hwnd, WebViewSession *sess)
{
    if (sess->replay_len == 0 || !target_hwnd) return;
    char *linear = (char *)smalloc(sess->replay_len);
    size_t start = (sess->replay_head + REPLAY_BUF_SIZE - sess->replay_len) % REPLAY_BUF_SIZE;
    for (size_t i = 0; i < sess->replay_len; i++) {
        linear[i] = sess->replay_buf[(start + i) % REPLAY_BUF_SIZE];
    }
    webview_host_send_session_binary_to_window(target_hwnd, '0', sess->id, linear, sess->replay_len);
    sfree(linear);
}

/* Forward declarations */
static LRESULT CALLBACK WebViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void on_web_message(HWND hwnd, const char *msg, void *userdata);
static HWND create_webview_window(int x, int y, int width, int height);
static size_t webview_seat_output(Seat *seat, SeatOutputType type,
                                 const void *data, size_t len);
static bool webview_seat_eof(Seat *seat);
static size_t webview_seat_banner(Seat *seat, const void *data, size_t len);
static SeatPromptResult webview_seat_get_userpass_input(Seat *seat, prompts_t *p);
static void webview_seat_notify_session_started(Seat *seat);
static void webview_seat_notify_remote_exit(Seat *seat);
static void webview_seat_notify_remote_disconnect(Seat *seat);
static void webview_seat_connection_fatal(Seat *seat, const char *msg);

static const SeatVtable webview_seat_vt = {
    .output = webview_seat_output,
    .eof = webview_seat_eof,
    .sent = nullseat_sent,
    .banner = webview_seat_banner,
    .get_userpass_input = webview_seat_get_userpass_input,
    .notify_session_started = webview_seat_notify_session_started,
    .notify_remote_exit = webview_seat_notify_remote_exit,
    .notify_remote_disconnect = webview_seat_notify_remote_disconnect,
    .connection_fatal = webview_seat_connection_fatal,
    .nonfatal = nullseat_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = nullseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = win_seat_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = win_seat_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = nullseat_confirm_weak_cached_hostkey,
    .prompt_descriptions = win_seat_prompt_descriptions,
    .is_utf8 = nullseat_is_always_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = nullseat_stripctrl_new,
    .set_trust_status = nullseat_set_trust_status,
    .can_set_trust_status = nullseat_can_set_trust_status_yes,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_yes,
    .verbose = cmdline_seat_verbose,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = nullseat_get_cursor_position,
};

static void session_stop_log(WebViewSession *sess)
{
    if (!sess) return;
    if (sess->log_fp) {
        fclose(sess->log_fp);
        sess->log_fp = NULL;
    }
    sess->log_enabled = false;
    sess->is_logging = false;
    sess->log_filename[0] = '\0';
    dbg_log("Stopped logging session %d", sess->id);
    if (sess->hwnd) {
        char notify[64];
        snprintf(notify, sizeof(notify), "L%d:0", sess->id);
        webview_host_send_to_window(sess->hwnd, notify);
    }
}

static bool session_start_log(WebViewSession *sess)
{
    if (!sess) return false;
    if (sess->log_fp) {
        fclose(sess->log_fp);
        sess->log_fp = NULL;
    }

    sess->log_enabled = true;

    wchar_t log_dir[MAX_PATH] = {0};
    const char *auto_dir = conf_get_str(sess->cfg, CONF_auto_log_dir);
    if (auto_dir && auto_dir[0] != '\0') {
        MultiByteToWideChar(CP_UTF8, 0, auto_dir, -1, log_dir, MAX_PATH);
    } else {
        wchar_t exe_path[MAX_PATH];
        GetModuleFileNameW(NULL, exe_path, MAX_PATH);
        wchar_t *last_slash = wcsrchr(exe_path, L'\\');
        if (last_slash) *last_slash = L'\0';
        _snwprintf(log_dir, MAX_PATH, L"%s\\logs", exe_path);
    }

    bool dir_ok = create_directory_recursive(log_dir);
    if (!dir_ok) {
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0) {
            _snwprintf(log_dir, MAX_PATH, L"%s\\PuTTY-WebView\\logs", appdata);
            create_directory_recursive(log_dir);
        }
    }

    int proto = conf_get_int(sess->cfg, CONF_protocol);
    const char *type_name = "Session";
    if (proto == PROT_SSH) type_name = "SSH";
    else if (proto == PROT_SERIAL) type_name = "Serial";
    else if (proto == PROT_CONPTY) type_name = "WSL";
    else if (proto == PROT_TELNET) type_name = "Telnet";
    else if (proto == PROT_RAW) type_name = "Raw";

    SYSTEMTIME st;
    GetLocalTime(&st);

    char filename_utf8[MAX_PATH];
    snprintf(filename_utf8, sizeof(filename_utf8), "%s_%04d%02d%02d_%02d%02d%02d.log",
             type_name,
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond);

    wchar_t w_filename[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, filename_utf8, -1, w_filename, MAX_PATH);

    wchar_t fullpath[MAX_PATH];
    _snwprintf(fullpath, MAX_PATH, L"%s\\%s", log_dir, w_filename);

    FILE *fp = _wfopen(fullpath, L"ab");
    if (!fp) {
        dbg_log("Failed to open log file %ls", fullpath);
        return false;
    }

    sess->log_fp = fp;
    sess->is_logging = true;
    strncpy(sess->log_filename, filename_utf8, sizeof(sess->log_filename) - 1);

    fprintf(fp, "=~=~=~=~=~=~=~=~=~=~=~= PuTTY-WebView log %04d.%02d.%02d %02d:%02d:%02d =~=~=~=~=~=~=~=~=~=~=\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fflush(fp);

    dbg_log("Started logging session %d to %s", sess->id, filename_utf8);

    if (sess->hwnd) {
        char notify[MAX_PATH + 32];
        snprintf(notify, sizeof(notify), "L%d:1:%s", sess->id, filename_utf8);
        webview_host_send_to_window(sess->hwnd, notify);
    }
    return true;
}

static void session_toggle_log(WebViewSession *sess)
{
    if (!sess) return;
    if (sess->log_enabled) {
        session_stop_log(sess);
    } else {
        session_start_log(sess);
    }
}

static void session_write_terminal(WebViewSession *sess, const char *text)
{
    if (!sess || !text) return;
    size_t len = strlen(text);
    session_record_output(sess, text, len);
    if (sess->log_fp) {
        fwrite(text, 1, len, sess->log_fp);
        fflush(sess->log_fp);
    }
    if (sess->hwnd) {
        webview_host_send_session_binary_to_window(sess->hwnd, '0', sess->id, text, len);
    }
}

static void strip_ansi_sequences(const char *src, size_t src_len, char **out_buf, size_t *out_len)
{
    char *dst = snewn(src_len + 1, char);
    size_t di = 0;
    size_t si = 0;

    while (si < src_len) {
        if ((unsigned char)src[si] == 0x1B) { /* ESC */
            si++;
            if (si >= src_len) break;
            char c = src[si++];
            if (c == '[') {
                /* CSI: ESC [ [parameter/intermediate bytes] final_byte */
                while (si < src_len && (unsigned char)src[si] >= 0x20 && (unsigned char)src[si] <= 0x3F) {
                    si++;
                }
                if (si < src_len && (unsigned char)src[si] >= 0x40 && (unsigned char)src[si] <= 0x7E) {
                    si++;
                }
            } else if (c == ']' || c == 'P' || c == '_' || c == '^') {
                /* OSC, DCS, APC, PM: ESC ] ... (BEL or ST: ESC \) */
                while (si < src_len) {
                    if ((unsigned char)src[si] == 0x07) { /* BEL */
                        si++;
                        break;
                    }
                    if ((unsigned char)src[si] == 0x1B) { /* ST part 1 */
                        si++;
                        if (si < src_len && src[si] == '\\') { /* ST part 2 */
                            si++;
                        }
                        break;
                    }
                    si++;
                }
            } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                /* Charset designator: ESC ( 0, etc. */
                if (si < src_len) si++;
            } else {
                /* 2-character escape sequence (e.g. ESC =, ESC >, ESC M, etc.) */
            }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    *out_buf = dst;
    *out_len = di;
}

static void strip_log_ansi_via_dialog(HWND hwnd)
{
    wchar_t szFile[MAX_PATH] = {0};
    wchar_t initial_dir[MAX_PATH] = {0};

    /* Check default logs directory */
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wchar_t *p = wcsrchr(exe_path, L'\\');
    if (p) *p = L'\0';
    _snwprintf(initial_dir, MAX_PATH, L"%s\\logs", exe_path);
    DWORD attr = GetFileAttributesW(initial_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        initial_dir[0] = L'\0';
    }

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
    ofn.lpstrFilter = L"Log Files (*.log;*.txt)\0*.log;*.txt\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = initial_dir[0] ? initial_dir : NULL;
    ofn.lpstrTitle = L"选择需要去除颜色编码的 SSH 日志文件 (Select SSH Log File)";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) {
        return; /* User cancelled */
    }

    wchar_t outPath[MAX_PATH] = {0};
    const wchar_t *last_sep = wcsrchr(szFile, L'\\');
    const wchar_t *last_dot = wcsrchr(szFile, L'.');

    if (last_dot && (!last_sep || last_dot > last_sep)) {
        int prefix_len = (int)(last_dot - szFile);
        _snwprintf(outPath, MAX_PATH, L"%.*s_uncolored%s", prefix_len, szFile, last_dot);
    } else {
        _snwprintf(outPath, MAX_PATH, L"%s_uncolored", szFile);
    }

    FILE *fp_in = _wfopen(szFile, L"rb");
    if (!fp_in) {
        MessageBoxW(hwnd, L"无法打开源文件！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    fseek(fp_in, 0, SEEK_END);
    long fsize = ftell(fp_in);
    fseek(fp_in, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(fp_in);
        MessageBoxW(hwnd, L"读取文件大小失败！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    char *src_buf = snewn(fsize + 1, char);
    size_t read_bytes = fread(src_buf, 1, fsize, fp_in);
    fclose(fp_in);

    char *out_buf = NULL;
    size_t out_len = 0;
    strip_ansi_sequences(src_buf, read_bytes, &out_buf, &out_len);
    sfree(src_buf);

    FILE *fp_out = _wfopen(outPath, L"wb");
    if (!fp_out) {
        sfree(out_buf);
        MessageBoxW(hwnd, L"无法创建输出文件！", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    fwrite(out_buf, 1, out_len, fp_out);
    fclose(fp_out);
    sfree(out_buf);

    wchar_t msg[MAX_PATH + 160];
    _snwprintf(msg, sizeof(msg) / sizeof(msg[0]),
               L"已成功去除颜色控制编码！\n\n新文件已保存至：\n%s\n\n是否打开所在文件夹？", outPath);
    if (MessageBoxW(hwnd, msg, L"SSH 日志颜色去除完成", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        wchar_t param[MAX_PATH + 16];
        _snwprintf(param, sizeof(param) / sizeof(param[0]), L"/select,\"%s\"", outPath);
        ShellExecuteW(hwnd, L"open", L"explorer.exe", param, NULL, SW_SHOWNORMAL);
    }
}

static void fix_ssh_key_perm_via_dialog(HWND hwnd)
{
    wchar_t szFile[MAX_PATH] = {0};
    wchar_t initial_dir[MAX_PATH] = {0};

    /* Check default user .ssh directory */
    wchar_t user_profile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", user_profile, MAX_PATH) > 0) {
        _snwprintf(initial_dir, MAX_PATH, L"%s\\.ssh", user_profile);
        DWORD attr = GetFileAttributesW(initial_dir);
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            wcsncpy(initial_dir, user_profile, MAX_PATH - 1);
            initial_dir[MAX_PATH - 1] = L'\0';
        }
    }

    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
    ofn.lpstrFilter =
        L"SSH 私钥文件 (*; *.pem; *.key; id_*)\0*;*.pem;*.key;id_*\0"
        L"所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = initial_dir[0] ? initial_dir : NULL;
    ofn.lpstrTitle = L"选择需要修复权限的 SSH 私钥文件 (Select SSH Private Key File)";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) {
        return; /* User cancelled */
    }

    /* 1. Retrieve current user SID and SYSTEM SID */
    HANDLE hToken = NULL;
    PTOKEN_USER pTokenUser = NULL;
    PSID pSystemSid = NULL;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        DWORD dwSize = 0;
        GetTokenInformation(hToken, TokenUser, NULL, 0, &dwSize);
        if (dwSize > 0) {
            pTokenUser = (PTOKEN_USER)smalloc(dwSize);
            if (!GetTokenInformation(hToken, TokenUser, pTokenUser, dwSize, &dwSize)) {
                sfree(pTokenUser);
                pTokenUser = NULL;
            }
        }
        CloseHandle(hToken);
    }

    AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0, 0, 0, &pSystemSid);

    PACL pNewDacl = NULL;
    DWORD res = ERROR_SUCCESS;

    if (pTokenUser && pSystemSid) {
        EXPLICIT_ACCESS_W ea[2];
        ZeroMemory(ea, sizeof(ea));

        /* Entry 0: Current User - Full Control */
        ea[0].grfAccessPermissions = GENERIC_ALL;
        ea[0].grfAccessMode = SET_ACCESS;
        ea[0].grfInheritance = NO_INHERITANCE;
        ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea[0].Trustee.ptstrName = (LPWSTR)pTokenUser->User.Sid;

        /* Entry 1: SYSTEM - Full Control */
        ea[1].grfAccessPermissions = GENERIC_ALL;
        ea[1].grfAccessMode = SET_ACCESS;
        ea[1].grfInheritance = NO_INHERITANCE;
        ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        ea[1].Trustee.ptstrName = (LPWSTR)pSystemSid;

        res = SetEntriesInAclW(2, ea, NULL, &pNewDacl);
        if (res == ERROR_SUCCESS && pNewDacl) {
            /* Try setting Owner and DACL with inheritance protection */
            res = SetNamedSecurityInfoW(
                szFile,
                SE_FILE_OBJECT,
                OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                pTokenUser->User.Sid,
                NULL,
                pNewDacl,
                NULL
            );

            /* If setting owner fails, try setting only DACL with inheritance protection */
            if (res != ERROR_SUCCESS) {
                res = SetNamedSecurityInfoW(
                    szFile,
                    SE_FILE_OBJECT,
                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                    NULL,
                    NULL,
                    pNewDacl,
                    NULL
                );
            }
        }
    } else {
        res = ERROR_ACCESS_DENIED;
    }

    if (pNewDacl) LocalFree(pNewDacl);
    if (pSystemSid) FreeSid(pSystemSid);
    if (pTokenUser) sfree(pTokenUser);

    /* 2. If access denied, offer UAC elevation fallback */
    if (res == ERROR_ACCESS_DENIED) {
        int ask_uac = MessageBoxW(
            hwnd,
            L"当前普通用户权限无法直接修改该文件的安全属性（可能所有者为管理员或其他用户）。\n\n"
            L"是否以系统管理员身份 (UAC) 进行一键修复？",
            L"需要管理员权限 (Administrator Required)",
            MB_YESNO | MB_ICONQUESTION
        );

        if (ask_uac == IDYES) {
            wchar_t username[256];
            DWORD ulen = sizeof(username) / sizeof(username[0]);
            if (!GetUserNameW(username, &ulen)) {
                wcsncpy(username, L"%USERNAME%", 255);
            }

            wchar_t cmd_args[1024];
            _snwprintf(cmd_args, sizeof(cmd_args) / sizeof(cmd_args[0]),
                L"/c \"takeown /F \"%s\" && icacls \"%s\" /reset && icacls \"%s\" /inheritance:r && icacls \"%s\" /grant:r \"%s\":(R,W) && icacls \"%s\" /grant:r \"SYSTEM\":(R,W)\"",
                szFile, szFile, szFile, szFile, username, szFile);

            SHELLEXECUTEINFOW sei;
            ZeroMemory(&sei, sizeof(sei));
            sei.cbSize = sizeof(sei);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.hwnd = hwnd;
            sei.lpVerb = L"runas"; /* Request UAC elevation */
            sei.lpFile = L"cmd.exe";
            sei.lpParameters = cmd_args;
            sei.nShow = SW_HIDE;

            if (ShellExecuteExW(&sei)) {
                if (sei.hProcess) {
                    WaitForSingleObject(sei.hProcess, 10000);
                    DWORD exit_code = 0;
                    GetExitCodeProcess(sei.hProcess, &exit_code);
                    CloseHandle(sei.hProcess);
                    if (exit_code == 0) {
                        res = ERROR_SUCCESS;
                    } else {
                        res = exit_code;
                    }
                }
            } else {
                /* User cancelled UAC prompt */
                return;
            }
        }
    }

    /* 3. Feedback Dialog */
    if (res == ERROR_SUCCESS) {
        wchar_t msg[1024];
        _snwprintf(msg, sizeof(msg) / sizeof(msg[0]),
            L"✅ SSH 私钥文件权限已成功修复！\n\n"
            L"目标文件：\n%s\n\n"
            L"已配置的安全策略（等同于 Linux chmod 600）：\n"
            L"• 禁用并移除所有继承权限 (Inheritance Removed)\n"
            L"• 移除 Users / Everyone 等外部未授权访问组\n"
            L"• 仅保留当前登录用户与 SYSTEM 专属读写权限\n\n"
            L"现在该私钥已可直接在 VS Code Remote-SSH、Git 以及 Windows 原生 OpenSSH 中安全使用，不会再有权限过大拦截报错。\n\n"
            L"是否在文件资源管理器中定位并高亮显示此文件？",
            szFile);

        if (MessageBoxW(hwnd, msg, L"SSH 私钥权限修复完成", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            wchar_t param[MAX_PATH + 16];
            _snwprintf(param, sizeof(param) / sizeof(param[0]), L"/select,\"%s\"", szFile);
            ShellExecuteW(hwnd, L"open", L"explorer.exe", param, NULL, SW_SHOWNORMAL);
        }
    } else {
        wchar_t err_msg[512];
        _snwprintf(err_msg, sizeof(err_msg) / sizeof(err_msg[0]),
            L"❌ 修复私钥权限失败！\n\n"
            L"目标文件：%s\n"
            L"系统错误代码：%lu",
            szFile, res);
        MessageBoxW(hwnd, err_msg, L"修复失败", MB_OK | MB_ICONERROR);
    }
}

static void session_reconnect(WebViewSession *sess);
static void session_schedule_reconnect(WebViewSession *sess);

/* ============================================================================
 * Local Encrypted Credential Vault (pw/ directory)
 * Uses Windows DPAPI (CryptProtectData / CryptUnprotectData) with app-specific entropy
 * ============================================================================ */

static const char PW_APP_ENTROPY[] = "PuTTY_WebView_Vault_v1_Secret!#@9841&%_Key";
static const char PW_APP_ENTROPY_LEGACY[] = "PuTTY_WebKit_Vault_v1_Secret!#@9841&%_Key";

static void pw_get_dir(wchar_t *out_dir, size_t max_len)
{
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wchar_t *last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash) *last_slash = L'\0';
    _snwprintf(out_dir, max_len, L"%s\\pw", exe_path);

    bool ok = create_directory_recursive(out_dir);
    if (!ok) {
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0) {
            _snwprintf(out_dir, max_len, L"%s\\PuTTY-WebView\\pw", appdata);
            create_directory_recursive(out_dir);
        }
    }
}

static void pw_sanitize_filename_part(const char *src, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0) return;
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 1 < dst_sz; i++) {
        char c = src[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 0x20) {
            dst[j++] = '_';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
    if (dst[0] == '\0') {
        strncpy(dst, "default", dst_sz - 1);
        dst[dst_sz - 1] = '\0';
    }
}

static void pw_get_user_filepath(wchar_t *out_path, size_t max_len,
                                 const char *host, int port, const char *user)
{
    wchar_t pw_dir[MAX_PATH];
    pw_get_dir(pw_dir, MAX_PATH);

    char safe_host[128], safe_user[128];
    pw_sanitize_filename_part(host, safe_host, sizeof(safe_host));
    pw_sanitize_filename_part(user, safe_user, sizeof(safe_user));

    wchar_t w_host[128], w_user[128];
    MultiByteToWideChar(CP_UTF8, 0, safe_host, -1, w_host, 128);
    MultiByteToWideChar(CP_UTF8, 0, safe_user, -1, w_user, 128);

    _snwprintf(out_path, max_len, L"%s\\%s_%d_%s.enc", pw_dir, w_host, port > 0 ? port : 22, w_user);
}

static void pw_get_last_user_filepath(wchar_t *out_path, size_t max_len,
                                      const char *host, int port)
{
    wchar_t pw_dir[MAX_PATH];
    pw_get_dir(pw_dir, MAX_PATH);

    char safe_host[128];
    pw_sanitize_filename_part(host, safe_host, sizeof(safe_host));

    wchar_t w_host[128];
    MultiByteToWideChar(CP_UTF8, 0, safe_host, -1, w_host, 128);

    _snwprintf(out_path, max_len, L"%s\\%s_%d_last.txt", pw_dir, w_host, port > 0 ? port : 22);
}

static void pw_get_legacy_filepath(wchar_t *out_path, size_t max_len,
                                   const char *host, int port)
{
    wchar_t pw_dir[MAX_PATH];
    pw_get_dir(pw_dir, MAX_PATH);

    char safe_host[128];
    pw_sanitize_filename_part(host, safe_host, sizeof(safe_host));

    wchar_t w_host[128];
    MultiByteToWideChar(CP_UTF8, 0, safe_host, -1, w_host, 128);

    _snwprintf(out_path, max_len, L"%s\\%s_%d.enc", pw_dir, w_host, port > 0 ? port : 22);
}

static void pw_delete_credential(const char *host, int port, const char *user)
{
    if (!host || !*host)
        return;
    if (user && *user) {
        wchar_t filepath[MAX_PATH];
        pw_get_user_filepath(filepath, MAX_PATH, host, port, user);
        _wremove(filepath);
    }
    wchar_t legacy[MAX_PATH];
    pw_get_legacy_filepath(legacy, MAX_PATH, host, port);
    _wremove(legacy);
}

static bool pw_save_credential(const char *host, int port, const char *user, const char *pass,
                               bool remember, bool autologin)
{
    if (!host || !*host || !user || !*user)
        return false;

    if (!remember) {
        pw_delete_credential(host, port, user);
        return true;
    }

    if (!pass || !*pass)
        return false;

    wchar_t filepath[MAX_PATH];
    pw_get_user_filepath(filepath, MAX_PATH, host, port, user);

    char plaintext[512];
    int pt_len = snprintf(plaintext, sizeof(plaintext), "USER:%s\nPASS:%s\nREMEMBER:%d\nAUTOLOGIN:%d\n",
                          user, pass, remember ? 1 : 0, autologin ? 1 : 0);
    if (pt_len <= 0 || (size_t)pt_len >= sizeof(plaintext))
        return false;

    DATA_BLOB data_in;
    data_in.pbData = (BYTE *)plaintext;
    data_in.cbData = (DWORD)pt_len;

    DATA_BLOB entropy;
    entropy.pbData = (BYTE *)PW_APP_ENTROPY;
    entropy.cbData = (DWORD)strlen(PW_APP_ENTROPY);

    DATA_BLOB data_out;
    ZeroMemory(&data_out, sizeof(data_out));

    BOOL res = CryptProtectData(&data_in, L"PuTTY-WebView SSH Credential", &entropy,
                                NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &data_out);
    smemclr(plaintext, sizeof(plaintext));

    if (!res || !data_out.pbData)
        return false;

    FILE *fp = _wfopen(filepath, L"wb");
    if (!fp) {
        LocalFree(data_out.pbData);
        return false;
    }

    size_t written = fwrite(data_out.pbData, 1, data_out.cbData, fp);
    fclose(fp);
    LocalFree(data_out.pbData);

    /* Record last used user */
    wchar_t last_file[MAX_PATH];
    pw_get_last_user_filepath(last_file, MAX_PATH, host, port);
    FILE *lfp = _wfopen(last_file, L"w");
    if (lfp) {
        fprintf(lfp, "%s\n", user);
        fclose(lfp);
    }

    /* Remove legacy file if it exists */
    wchar_t legacy[MAX_PATH];
    pw_get_legacy_filepath(legacy, MAX_PATH, host, port);
    _wremove(legacy);

    dbg_log("pw_save_credential: Saved %s@%s:%d to %ls (%zu bytes, rem=%d, auto=%d)",
            user, host, port, filepath, written, remember ? 1 : 0, autologin ? 1 : 0);
    return written > 0;
}

static bool pw_load_single_file(const wchar_t *filepath,
                                char *out_user, size_t user_size,
                                char *out_pass, size_t pass_size,
                                bool *out_remember, bool *out_autologin)
{
    if (out_user && user_size > 0) out_user[0] = '\0';
    if (out_pass && pass_size > 0) out_pass[0] = '\0';
    if (out_remember) *out_remember = false;
    if (out_autologin) *out_autologin = false;

    FILE *fp = _wfopen(filepath, L"rb");
    if (!fp)
        return false;

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 64 * 1024) {
        fclose(fp);
        return false;
    }

    BYTE *cipher_buf = (BYTE *)smalloc(fsize);
    size_t read_bytes = fread(cipher_buf, 1, fsize, fp);
    fclose(fp);

    if (read_bytes != (size_t)fsize) {
        sfree(cipher_buf);
        return false;
    }

    DATA_BLOB data_in;
    data_in.pbData = cipher_buf;
    data_in.cbData = (DWORD)fsize;

    DATA_BLOB entropy;
    entropy.pbData = (BYTE *)PW_APP_ENTROPY;
    entropy.cbData = (DWORD)strlen(PW_APP_ENTROPY);

    DATA_BLOB data_out;
    ZeroMemory(&data_out, sizeof(data_out));

    BOOL res = CryptUnprotectData(&data_in, NULL, &entropy,
                                  NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &data_out);
    if (!res || !data_out.pbData) {
        entropy.pbData = (BYTE *)PW_APP_ENTROPY_LEGACY;
        entropy.cbData = (DWORD)strlen(PW_APP_ENTROPY_LEGACY);
        res = CryptUnprotectData(&data_in, NULL, &entropy,
                                 NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &data_out);
    }
    sfree(cipher_buf);

    if (!res || !data_out.pbData)
        return false;

    char *pt = (char *)data_out.pbData;
    size_t pt_len = (size_t)data_out.cbData;

    bool found_user = false;
    bool found_pass = false;
    bool found_remember_tag = false;
    bool found_autologin_tag = false;

    char *line = pt;
    char *end = pt + pt_len;

    while (line < end) {
        char *eol = line;
        while (eol < end && *eol != '\n' && *eol != '\r')
            eol++;

        size_t line_len = eol - line;
        if (line_len >= 5 && !memcmp(line, "USER:", 5)) {
            if (out_user && user_size > 0) {
                size_t cpy_len = line_len - 5;
                if (cpy_len >= user_size) cpy_len = user_size - 1;
                memcpy(out_user, line + 5, cpy_len);
                out_user[cpy_len] = '\0';
                found_user = true;
            }
        } else if (line_len >= 5 && !memcmp(line, "PASS:", 5)) {
            if (out_pass && pass_size > 0) {
                size_t cpy_len = line_len - 5;
                if (cpy_len >= pass_size) cpy_len = pass_size - 1;
                memcpy(out_pass, line + 5, cpy_len);
                out_pass[cpy_len] = '\0';
                found_pass = true;
            }
        } else if (line_len >= 9 && !memcmp(line, "REMEMBER:", 9)) {
            if (out_remember) {
                *out_remember = (line[9] == '1' || line[9] == 't');
                found_remember_tag = true;
            }
        } else if (line_len >= 10 && !memcmp(line, "AUTOLOGIN:", 10)) {
            if (out_autologin) {
                *out_autologin = (line[10] == '1' || line[10] == 't');
                found_autologin_tag = true;
            }
        }

        while (eol < end && (*eol == '\n' || *eol == '\r'))
            eol++;
        line = eol;
    }

    smemclr(data_out.pbData, data_out.cbData);
    LocalFree(data_out.pbData);

    if (found_pass) {
        if (!found_remember_tag && out_remember) *out_remember = true;
        if (!found_autologin_tag && out_autologin) *out_autologin = true;
    }

    return (found_user && found_pass);
}

static void json_escape_string(const char *src, char *dst, size_t dst_sz);

static int pw_load_all_for_host(const char *host, int port,
                                char *out_last_user, size_t last_user_sz,
                                strbuf *out_json_users)
{
    if (out_last_user && last_user_sz > 0) out_last_user[0] = '\0';
    if (!host || !*host) return 0;

    wchar_t pw_dir[MAX_PATH];
    pw_get_dir(pw_dir, MAX_PATH);

    char safe_host[128];
    pw_sanitize_filename_part(host, safe_host, sizeof(safe_host));
    wchar_t w_host[128];
    MultiByteToWideChar(CP_UTF8, 0, safe_host, -1, w_host, 128);

    /* 1. Try reading last used username */
    wchar_t last_file[MAX_PATH];
    pw_get_last_user_filepath(last_file, MAX_PATH, host, port);
    FILE *lfp = _wfopen(last_file, L"r");
    if (lfp) {
        char line[128];
        if (fgets(line, sizeof(line), lfp)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = '\0';
            if (out_last_user && line[0] != '\0') {
                strncpy(out_last_user, line, last_user_sz - 1);
                out_last_user[last_user_sz - 1] = '\0';
            }
        }
        fclose(lfp);
    }

    int count = 0;
    char first_user[128] = {0};

    /* 2. Scan for multi-user files: pw\%s_%d_*.enc */
    wchar_t search_pattern[MAX_PATH];
    _snwprintf(search_pattern, MAX_PATH, L"%s\\%s_%d_*.enc", pw_dir, w_host, port > 0 ? port : 22);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;

            wchar_t filepath[MAX_PATH];
            _snwprintf(filepath, MAX_PATH, L"%s\\%s", pw_dir, fd.cFileName);

            char u[128] = {0}, p[256] = {0};
            bool rem = false, autolog = false;
            if (pw_load_single_file(filepath, u, sizeof(u), p, sizeof(p), &rem, &autolog)) {
                if (count == 0) {
                    strncpy(first_user, u, sizeof(first_user) - 1);
                }
                char esc_u[256], esc_p[512];
                json_escape_string(u, esc_u, sizeof(esc_u));
                json_escape_string(p, esc_p, sizeof(esc_p));

                if (count > 0) put_byte(out_json_users, ',');
                put_fmt(out_json_users, "\"%s\":{\"pass\":\"%s\",\"remember\":%s,\"autoLogin\":%s}",
                            esc_u, esc_p, rem ? "true" : "false", autolog ? "true" : "false");
                smemclr(p, sizeof(p));
                smemclr(esc_p, sizeof(esc_p));
                count++;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    /* 3. Check legacy file: pw\%s_%d.enc */
    wchar_t legacy_file[MAX_PATH];
    pw_get_legacy_filepath(legacy_file, MAX_PATH, host, port);
    if (GetFileAttributesW(legacy_file) != INVALID_FILE_ATTRIBUTES) {
        char u[128] = {0}, p[256] = {0};
        bool rem = false, autolog = false;
        if (pw_load_single_file(legacy_file, u, sizeof(u), p, sizeof(p), &rem, &autolog)) {
            if (count == 0) {
                strncpy(first_user, u, sizeof(first_user) - 1);
            }
            char esc_u[256], esc_p[512];
            json_escape_string(u, esc_u, sizeof(esc_u));
            json_escape_string(p, esc_p, sizeof(esc_p));

            if (count > 0) put_byte(out_json_users, ',');
            put_fmt(out_json_users, "\"%s\":{\"pass\":\"%s\",\"remember\":%s,\"autoLogin\":%s}",
                        esc_u, esc_p, rem ? "true" : "false", autolog ? "true" : "false");
            smemclr(p, sizeof(p));
            smemclr(esc_p, sizeof(esc_p));
            count++;
        }
    }

    if (out_last_user && out_last_user[0] == '\0' && first_user[0] != '\0') {
        strncpy(out_last_user, first_user, last_user_sz - 1);
        out_last_user[last_user_sz - 1] = '\0';
    }

    return count;
}

static void json_escape_string(const char *src, char *dst, size_t dst_sz)
{
    if (!dst || dst_sz == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t j = 0;
    for (size_t i = 0; src[i] && j + 2 < dst_sz; i++) {
        char c = src[i];
        if (c == '\\' || c == '"') {
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c == '\n') {
            dst[j++] = '\\';
            dst[j++] = 'n';
        } else if (c == '\r') {
            dst[j++] = '\\';
            dst[j++] = 'r';
        } else if (c == '\t') {
            dst[j++] = '\\';
            dst[j++] = 't';
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

static void json_escape_to_strbuf(strbuf *dst, const char *src)
{
    put_byte(dst, '"');
    if (src) {
        for (; *src; src++) {
            unsigned char c = (unsigned char)*src;
            if (c == '"') { put_data(dst, "\\\"", 2); }
            else if (c == '\\') { put_data(dst, "\\\\", 2); }
            else if (c == '\b') { put_data(dst, "\\b", 2); }
            else if (c == '\f') { put_data(dst, "\\f", 2); }
            else if (c == '\n') { put_data(dst, "\\n", 2); }
            else if (c == '\r') { put_data(dst, "\\r", 2); }
            else if (c == '\t') { put_data(dst, "\\t", 2); }
            else if (c < 32) { put_fmt(dst, "\\u%04x", c); }
            else { put_byte(dst, c); }
        }
    }
    put_byte(dst, '"');
}

static void parse_auth_json(const char *json,
                            char *out_user, size_t user_sz,
                            char *out_pass, size_t pass_sz,
                            bool *out_remember, bool *out_autologin)
{
    if (out_user && user_sz > 0) out_user[0] = '\0';
    if (out_pass && pass_sz > 0) out_pass[0] = '\0';
    if (out_remember) *out_remember = false;
    if (out_autologin) *out_autologin = false;

    if (!json || !*json) return;

    /* Extract user */
    const char *p = strstr(json, "\"user\":");
    if (p) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            p++;
            size_t idx = 0;
            while (*p && *p != '"' && idx + 1 < user_sz) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                }
                out_user[idx++] = *p++;
            }
            out_user[idx] = '\0';
        }
    }

    /* Extract pass */
    p = strstr(json, "\"pass\":");
    if (p) {
        p += 7;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            p++;
            size_t idx = 0;
            while (*p && *p != '"' && idx + 1 < pass_sz) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                }
                out_pass[idx++] = *p++;
            }
            out_pass[idx] = '\0';
        }
    }

    /* Extract remember */
    p = strstr(json, "\"remember\":");
    if (p && out_remember) {
        p += 11;
        while (*p == ' ' || *p == '\t') p++;
        *out_remember = (!strncmp(p, "true", 4) || *p == '1');
    }

    /* Extract autoLogin */
    p = strstr(json, "\"autoLogin\":");
    if (p && out_autologin) {
        p += 12;
        while (*p == ' ' || *p == '\t') p++;
        *out_autologin = (!strncmp(p, "true", 4) || *p == '1');
    }
}

static void session_cleanup_backend(WebViewSession *sess)
{
    if (!sess)
        return;

    sess->cur_prompts = NULL;
    if (sess->prompt_input_buf) {
        strbuf_free(sess->prompt_input_buf);
        sess->prompt_input_buf = NULL;
    }
    smemclr(sess->temp_prompt_pass, sizeof(sess->temp_prompt_pass));
    sess->temp_prompt_pass[0] = '\0';
    smemclr(sess->active_auth_pass, sizeof(sess->active_auth_pass));
    sess->active_auth_pass[0] = '\0';
    sess->temp_prompt_user[0] = '\0';
    sess->login_failed = false;
    sess->prompt_attempts = 0;
    sess->modal_remember = false;
    sess->modal_autologin = false;
    sess->username_displayed = false;

    if (!sess->backend || sess->in_backend_free)
        return;

    sess->in_backend_free = true;
    Backend *be = sess->backend;
    sess->backend = NULL;
    sess->wgs.backend = NULL;
    backend_free(be);
    sess->in_backend_free = false;
}

static void session_close_backend_cb(void *vctx)
{
    WebViewSession *sess = (WebViewSession *)vctx;
    session_cleanup_backend(sess);
}

static void session_handle_disconnect(WebViewSession *sess, const char *reason)
{
    if (!sess || sess->in_backend_free || sess->in_connecting)
        return;

    bool was_connected = sess->is_connected;
    sess->is_connected = false;

    if (sess->log_fp) {
        fclose(sess->log_fp);
        sess->log_fp = NULL;
        sess->is_logging = false;
    }

    expire_timer_context(sess);
    bufchain_clear(&sess->send_queue);
    sess->send_timer_active = false;

    if (sess->hwnd) {
        webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "disconnected");
    }

    if (was_connected || !sess->reconnect_timer_active) {
        if (reason && reason[0]) {
            char banner[512];
            snprintf(banner, sizeof(banner), "\r\n\x1b[1;31m[%s]\x1b[0m\r\n", reason);
            session_write_terminal(sess, banner);
        }
    }

    queue_toplevel_callback(session_close_backend_cb, sess);

    if (sess->auto_reconnect) {
        session_schedule_reconnect(sess);
    }
}

static void session_reconnect_timer(void *ctx, unsigned long now)
{
    WebViewSession *sess = container_of((bool *)ctx, WebViewSession, reconnect_timer_active);
    if (!sess->reconnect_timer_active)
        return;
    sess->reconnect_timer_active = false;
    if (!sess->auto_reconnect)
        return;
    session_reconnect(sess);
}

static void session_schedule_reconnect(WebViewSession *sess)
{
    if (!sess || !sess->auto_reconnect)
        return;
    if (sess->reconnect_timer_active)
        return;

    sess->reconnect_timer_active = true;
    sess->reconnect_countdown = 5;
    unsigned long now = GETTICKCOUNT();
    sess->reconnect_next_tick = now + 1 * TICKSPERSEC;
    sess->reconnect_target_tick = now + 5 * TICKSPERSEC;

    session_write_terminal(sess, "\x1b[1;33m[自动重连] 5 秒后尝试重新连接...\x1b[0m");
    schedule_timer(5 * TICKSPERSEC, session_reconnect_timer, &sess->reconnect_timer_active);
}

static void session_reconnect(WebViewSession *sess)
{
    if (!sess || !sess->auto_reconnect)
        return;
    if (session_is_connected(sess))
        return;

    session_write_terminal(sess, "\r\x1b[2K\x1b[1;36m[自动重连] 正在尝试连接...\x1b[0m\r\n");

    session_cleanup_backend(sess);
    sess->tried_saved_pw = false;

    bufchain_clear(&sess->send_queue);
    sess->send_timer_active = false;

    sess->wgs.cmdline_get_passwd_state = cmdline_get_passwd_input_state_new;
    seat_set_trust_status(&sess->wgs.seat, true);

    int proto = conf_get_int(sess->cfg, CONF_protocol);
    const struct BackendVtable *vt = backend_vt_from_proto(proto);
    if (!vt) {
        session_write_terminal(sess, "\x1b[1;31m[自动重连失败: 不支持的协议]\x1b[0m\r\n");
        return;
    }

    sess->in_connecting = true;
    char *realhost = NULL;
    char *err = backend_init(vt, &sess->wgs.seat, &sess->backend, sess->logctx, sess->cfg,
                             conf_get_str(sess->cfg, CONF_host),
                             conf_get_int(sess->cfg, CONF_port),
                             &realhost,
                             conf_get_bool(sess->cfg, CONF_tcp_nodelay),
                             conf_get_bool(sess->cfg, CONF_tcp_keepalives));
    sfree(realhost);
    sess->in_connecting = false;

    if (err) {
        char banner[512];
        snprintf(banner, sizeof(banner), "\x1b[1;31m[连接失败: %s]\x1b[0m\r\n", err);
        session_write_terminal(sess, banner);
        sfree(err);
        session_cleanup_backend(sess);
        sess->is_connected = false;
        if (sess->hwnd) {
            webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "disconnected");
        }
        if (sess->auto_reconnect) {
            session_schedule_reconnect(sess);
        }
    } else {
        sess->is_connected = true;
        sess->wgs.backend = sess->backend;
        if (proto == PROT_SERIAL || proto == PROT_CONPTY || proto == PROT_RAW) {
            if (sess->auto_reconnect) {
                session_write_terminal(sess, "\x1b[1;32m[自动重连成功]\x1b[0m\r\n");
            }
            if (sess->log_enabled) {
                session_start_log(sess);
                char log_hint[MAX_PATH + 64];
                snprintf(log_hint, sizeof(log_hint), "\x1b[1;36m[已自动创建新日志文件: %s]\x1b[0m\r\n", sess->log_filename);
                session_write_terminal(sess, log_hint);
            }
            if (sess->hwnd) {
                webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "connected");
            }
        }
    }
}

static void session_toggle_auto_reconnect(WebViewSession *sess)
{
    if (!sess) return;
    sess->auto_reconnect = !sess->auto_reconnect;

    if (sess->hwnd) {
        char notify[32];
        snprintf(notify, sizeof(notify), "A%d:%d", sess->id, sess->auto_reconnect ? 1 : 0);
        webview_host_send_to_window(sess->hwnd, notify);
    }

    if (sess->auto_reconnect) {
        session_write_terminal(sess, "\r\n\x1b[1;32m[自动重连已开启]\x1b[0m\r\n");
        if (!session_is_connected(sess) && !sess->reconnect_timer_active) {
            session_schedule_reconnect(sess);
        }
    } else {
        if (sess->reconnect_timer_active) {
            expire_timer_context(&sess->reconnect_timer_active);
            sess->reconnect_timer_active = false;
            session_write_terminal(sess, "\r\x1b[2K\x1b[33m[自动重连已关闭]\x1b[0m\r\n");
        } else {
            session_write_terminal(sess, "\r\n\x1b[33m[自动重连已关闭]\x1b[0m\r\n");
        }
    }
}

/* SeatVtable implementations */
static size_t webview_seat_output(Seat *seat, SeatOutputType type,
                                 const void *data, size_t len)
{
    if (len > 0) {
        WebViewSession *sess = session_from_seat(seat);
        if (sess && data) {
            if (strstr((const char *)data, "Access denied") ||
                strstr((const char *)data, "Authentication failed") ||
                strstr((const char *)data, "Login incorrect")) {
                sess->login_failed = true;
            }
        }
        session_record_output(sess, data, len);
        if (sess->log_fp) {
            fwrite(data, 1, len, sess->log_fp);
            fflush(sess->log_fp);
        }
        if (sess->hwnd) {
            webview_host_send_session_binary_to_window(sess->hwnd, '0', sess->id, data, len);
        }
    }
    return 0;
}

static bool webview_seat_eof(Seat *seat)
{
    return false;
}

static size_t webview_seat_banner(Seat *seat, const void *data, size_t len)
{
    return webview_seat_output(seat, SEAT_OUTPUT_STDOUT, data, len);
}

static void session_handle_prompt_input(WebViewSession *sess, const char *data, size_t len)
{
    if (!sess || !sess->cur_prompts || !data || len == 0)
        return;

    prompts_t *p = sess->cur_prompts;
    if (sess->cur_prompt_idx >= p->n_prompts)
        return;

    prompt_t *pr = p->prompts[sess->cur_prompt_idx];

    for (size_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < len && data[i + 1] == '\n') {
                i++;
            }

            const char *val = (sess->prompt_input_buf && sess->prompt_input_buf->s) ?
                              sess->prompt_input_buf->s : "";
            prompt_set_result(pr, val);

            if (pr->echo) {
                strncpy(sess->temp_prompt_user, val, sizeof(sess->temp_prompt_user) - 1);
                sess->temp_prompt_user[sizeof(sess->temp_prompt_user) - 1] = '\0';
                if (val[0] != '\0' && sess->cfg) {
                    conf_set_str(sess->cfg, CONF_username, sess->temp_prompt_user);
                }
            } else {
                strncpy(sess->temp_prompt_pass, val, sizeof(sess->temp_prompt_pass) - 1);
                sess->temp_prompt_pass[sizeof(sess->temp_prompt_pass) - 1] = '\0';
            }

            session_write_terminal(sess, "\r\n");
            if (sess->prompt_input_buf) {
                strbuf_clear(sess->prompt_input_buf);
            }

            sess->cur_prompt_idx++;
            if (sess->cur_prompt_idx < p->n_prompts) {
                prompt_t *next_pr = p->prompts[sess->cur_prompt_idx];
                session_write_terminal(sess, next_pr->prompt);
                pr = next_pr;
            } else {
                sess->cur_prompts = NULL;
                p->spr = SPR_OK;
                if (p->callback) {
                    queue_toplevel_callback(p->callback, p->callback_ctx);
                }
                return;
            }
        } else if (c == '\x08' || c == '\x7f') {
            if (sess->prompt_input_buf && sess->prompt_input_buf->len > 0) {
                strbuf_shrink_by(sess->prompt_input_buf, 1);
                if (pr->echo) {
                    session_write_terminal(sess, "\b \b");
                }
            }
        } else if (c == '\x03') {
            session_write_terminal(sess, "^C\r\n");
            if (sess->prompt_input_buf) {
                strbuf_clear(sess->prompt_input_buf);
            }
            sess->cur_prompts = NULL;
            p->spr = SPR_USER_ABORT;
            if (p->callback) {
                queue_toplevel_callback(p->callback, p->callback_ctx);
            }
            return;
        } else if ((unsigned char)c >= 0x20) {
            if (!sess->prompt_input_buf) {
                sess->prompt_input_buf = strbuf_new_nm();
            }
            put_byte(sess->prompt_input_buf, c);
            if (pr->echo) {
                char echo_str[2] = { c, '\0' };
                session_write_terminal(sess, echo_str);
            }
        }
    }
}

static SeatPromptResult webview_seat_get_userpass_input(Seat *seat, prompts_t *p)
{
    WebViewSession *sess = session_from_seat(seat);
    if (!sess)
        return SPR_SW_ABORT("Session not found");

    if (p->spr.kind != SPRK_INCOMPLETE)
        return p->spr;

    const char *host = conf_get_str(sess->cfg, CONF_host);
    int port = conf_get_int(sess->cfg, CONF_port);

    /* 1. Try command line password if supplied */
    SeatPromptResult spr = cmdline_get_passwd_input(p, &sess->wgs.cmdline_get_passwd_state, false);
    if (spr.kind != SPRK_INCOMPLETE)
        return spr;

    /* Detect if previous attempt failed (prompt called again without session_started having run) */
    if (sess->prompt_attempts > 0 && !sess->is_connected) {
        sess->login_failed = true;
        smemclr(sess->temp_prompt_pass, sizeof(sess->temp_prompt_pass));
        sess->temp_prompt_pass[0] = '\0';
    }

    /* If user already submitted credentials via the modal during this sequence (e.g. username prompted first, then password) */
    if (sess->temp_prompt_pass[0] != '\0') {
        bool all_filled = true;
        for (size_t i = 0; i < p->n_prompts; i++) {
            prompt_t *pr = p->prompts[i];
            if (!pr->echo) {
                prompt_set_result(pr, sess->temp_prompt_pass);
            } else {
                const char *u = (sess->temp_prompt_user[0] != '\0') ? sess->temp_prompt_user : conf_get_str_ambi(sess->cfg, CONF_username, NULL);
                if (u && *u) {
                    prompt_set_result(pr, u);
                    if (!sess->username_displayed) {
                        session_write_terminal(sess, "login as: ");
                        session_write_terminal(sess, u);
                        session_write_terminal(sess, "\r\n");
                        sess->username_displayed = true;
                    }
                } else {
                    all_filled = false;
                }
            }
        }
        if (all_filled) {
            sess->prompt_attempts++;
            p->spr = SPR_OK;
            return SPR_OK;
        }
    }


    /* 4. Pop up WebView SSH Auth modal dialog */
    if (sess->hwnd) {
        sess->cur_prompts = p;

        char last_user[128] = {0};
        strbuf *json_users = strbuf_new_nm();
        pw_load_all_for_host(host, port, last_user, sizeof(last_user), json_users);

        const char *cfg_user = conf_get_str_ambi(sess->cfg, CONF_username, NULL);
        const char *cur_u = (last_user[0] != '\0') ? last_user :
                            ((cfg_user && *cfg_user) ? cfg_user : sess->temp_prompt_user);

        const char *err_msg = sess->login_failed ? "用户名或密码错误，请重新输入" : "";

        char esc_host[256], esc_user[256], esc_err[256];
        json_escape_string(host ? host : "", esc_host, sizeof(esc_host));
        json_escape_string(cur_u ? cur_u : "", esc_user, sizeof(esc_user));
        json_escape_string(err_msg, esc_err, sizeof(esc_err));

        strbuf *msg = strbuf_new_nm();
        put_fmt(msg, "P%d:{\"host\":\"%s\",\"port\":%d,\"currentUser\":\"%s\",\"users\":{%s},\"error\":\"%s\"}",
                sess->id, esc_host, port > 0 ? port : 22, esc_user,
                json_users->s ? json_users->s : "", esc_err);

        strbuf_free(json_users);

        webview_host_send_to_window(sess->hwnd, msg->s);
        strbuf_free(msg);
        return SPR_INCOMPLETE;
    }

    /* Fallback to terminal prompt if hwnd not available */
    sess->cur_prompts = p;
    sess->cur_prompt_idx = 0;
    if (!sess->prompt_input_buf) {
        sess->prompt_input_buf = strbuf_new_nm();
    } else {
        strbuf_clear(sess->prompt_input_buf);
    }

    /* Display instruction or name if required */
    if (p->name_reqd && p->name && *p->name) {
        session_write_terminal(sess, "\r\n\x1b[1;37m");
        session_write_terminal(sess, p->name);
        session_write_terminal(sess, "\x1b[0m\r\n");
    }
    if (p->instruction && *p->instruction) {
        session_write_terminal(sess, p->instruction);
        session_write_terminal(sess, "\r\n");
    }

    /* Output first prompt string */
    if (sess->cur_prompt_idx < p->n_prompts) {
        prompt_t *pr = p->prompts[sess->cur_prompt_idx];
        session_write_terminal(sess, pr->prompt);
    } else {
        sess->cur_prompts = NULL;
        p->spr = SPR_OK;
        return SPR_OK;
    }

    return SPR_INCOMPLETE;
}

static void webview_seat_notify_session_started(Seat *seat)
{
    WebViewSession *sess = session_from_seat(seat);
    if (!sess) return;
    sess->is_connected = true;
    sess->login_failed = false;
    sess->prompt_attempts = 0;

    /* If user entered password via modal and modal_remember is set, save to pw/ */
    if (sess->modal_remember && sess->temp_prompt_pass[0] != '\0') {
        const char *host = conf_get_str(sess->cfg, CONF_host);
        int port = conf_get_int(sess->cfg, CONF_port);
        const char *user = (sess->temp_prompt_user[0] != '\0') ? sess->temp_prompt_user : conf_get_str_ambi(sess->cfg, CONF_username, NULL);

        if (pw_save_credential(host, port, user, sess->temp_prompt_pass, true, sess->modal_autologin)) {
            if (sess->modal_autologin) {
                session_write_terminal(sess, "\x1b[1;32m[已加密保存凭据至 pw/ 目录，下次将自动免密登录]\x1b[0m\r\n");
            } else {
                session_write_terminal(sess, "\x1b[1;32m[已加密保存凭据至 pw/ 目录]\x1b[0m\r\n");
            }
        }
    } else if (!sess->modal_remember && sess->temp_prompt_pass[0] != '\0') {
        const char *host = conf_get_str(sess->cfg, CONF_host);
        int port = conf_get_int(sess->cfg, CONF_port);
        const char *user = (sess->temp_prompt_user[0] != '\0') ? sess->temp_prompt_user : conf_get_str_ambi(sess->cfg, CONF_username, NULL);
        pw_delete_credential(host, port, user);
    }

    if (sess->temp_prompt_pass[0] != '\0') {
        strncpy(sess->active_auth_pass, sess->temp_prompt_pass, sizeof(sess->active_auth_pass) - 1);
        sess->active_auth_pass[sizeof(sess->active_auth_pass) - 1] = '\0';
    }

    smemclr(sess->temp_prompt_pass, sizeof(sess->temp_prompt_pass));
    sess->temp_prompt_pass[0] = '\0';

    if (sess->auto_reconnect) {
        session_write_terminal(sess, "\x1b[1;32m[自动重连成功]\x1b[0m\r\n");
    }
    if (sess->log_enabled) {
        session_start_log(sess);
        char log_hint[MAX_PATH + 64];
        snprintf(log_hint, sizeof(log_hint), "\x1b[1;36m[已自动创建新日志文件: %s]\x1b[0m\r\n", sess->log_filename);
        session_write_terminal(sess, log_hint);
    }
    if (sess->hwnd) {
        webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "connected");
    }
}

static void webview_seat_notify_remote_exit(Seat *seat)
{
    WebViewSession *sess = session_from_seat(seat);
    if (!sess) return;
    session_handle_disconnect(sess, "Connection closed by remote host");
}

static void webview_seat_notify_remote_disconnect(Seat *seat)
{
    WebViewSession *sess = session_from_seat(seat);
    if (!sess) return;
    session_handle_disconnect(sess, "Connection disconnected");
}

static void webview_seat_connection_fatal(Seat *seat, const char *msg)
{
    WebViewSession *sess = session_from_seat(seat);
    if (!sess) return;
    char reason[512];
    snprintf(reason, sizeof(reason), "Fatal Error: %s", msg ? msg : "Connection failed");
    session_handle_disconnect(sess, reason);
}

/* System callbacks */
const wchar_t *get_app_user_model_id(void)
{
    return L"SimonTatham.PuTTYWebView";
}

char *handle_restrict_acl_cmdline_prefix(char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '&' && p[1] == 'R' &&
        (!p[2] || p[2] == '@' || p[2] == '&')) {
        restrict_process_acl();
        p += 2;
    }
    return p;
}

bool handle_special_sessionname_cmdline(char *p, Conf *conf)
{
    if (*p != '@')
        return false;

    ptrlen sessionname = ptrlen_from_asciz(p + 1);
    while (sessionname.len > 0 &&
           isspace(((unsigned char *)sessionname.ptr)[sessionname.len - 1]))
        sessionname.len--;

    char *dup = mkstr(sessionname);
    bool loaded = do_defaults(dup, conf);
    sfree(dup);

    return loaded;
}

static bool safe_open_clipboard(HWND hwnd)
{
    for (int retry = 0; retry < 5; retry++) {
        if (OpenClipboard(hwnd))
            return true;
        Sleep(10);
    }
    return false;
}

static void copy_to_clipboard_utf8(HWND hwnd, const char *str, int len)
{
    if (!str || len <= 0)
        return;

    /* Expand lone \n to \r\n for standard Windows clipboard */
    int expanded_len = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] == '\n' && (i == 0 || str[i - 1] != '\r')) {
            expanded_len += 2;
        } else {
            expanded_len += 1;
        }
    }

    char *expanded = snewn(expanded_len + 1, char);
    int exp_idx = 0;
    for (int i = 0; i < len; i++) {
        if (str[i] == '\n' && (i == 0 || str[i - 1] != '\r')) {
            expanded[exp_idx++] = '\r';
            expanded[exp_idx++] = '\n';
        } else {
            expanded[exp_idx++] = str[i];
        }
    }
    expanded[exp_idx] = '\0';

    int wlen = MultiByteToWideChar(CP_UTF8, 0, expanded, exp_idx, NULL, 0);
    if (wlen > 0) {
        HGLOBAL hglb = GlobalAlloc(GMEM_MOVEABLE, (wlen + 1) * sizeof(wchar_t));
        if (hglb) {
            wchar_t *wstr = (wchar_t *)GlobalLock(hglb);
            if (wstr) {
                MultiByteToWideChar(CP_UTF8, 0, expanded, exp_idx, wstr, wlen);
                wstr[wlen] = L'\0';
                GlobalUnlock(hglb);

                if (safe_open_clipboard(hwnd)) {
                    EmptyClipboard();
                    SetClipboardData(CF_UNICODETEXT, hglb);
                    CloseClipboard();
                } else {
                    GlobalFree(hglb);
                }
            } else {
                GlobalFree(hglb);
            }
        }
    }
    sfree(expanded);
}

static void paste_to_session(HWND hwnd, int session_id)
{
    WebViewSession *sess = session_find(session_id);
    if (!sess || !sess->backend || !backend_connected(sess->backend))
        return;
    if (!safe_open_clipboard(hwnd))
        return;

    HGLOBAL hglb = GetClipboardData(CF_UNICODETEXT);
    if (hglb) {
        const wchar_t *wstr = (const wchar_t *)GlobalLock(hglb);
        if (wstr) {
            int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
            if (utf8_len > 1) {
                char *utf8 = snewn(utf8_len, char);
                WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8, utf8_len, NULL, NULL);

                /* Normalise CRLF (\r\n) or lone \n to \r for terminal input */
                char *norm = snewn(utf8_len, char);
                int norm_len = 0;
                for (int i = 0; i < utf8_len - 1; i++) {
                    if (utf8[i] == '\r') {
                        if (i + 1 < utf8_len - 1 && utf8[i + 1] == '\n') {
                            i++; /* Skip \n */
                        }
                        norm[norm_len++] = '\r';
                    } else if (utf8[i] == '\n') {
                        norm[norm_len++] = '\r';
                    } else {
                        norm[norm_len++] = utf8[i];
                    }
                }
                session_send(sess, norm, norm_len);
                sfree(norm);
                sfree(utf8);
            }
            GlobalUnlock(hglb);
        }
    } else {
        hglb = GetClipboardData(CF_TEXT);
        if (hglb) {
            const char *str = (const char *)GlobalLock(hglb);
            if (str) {
                int slen = strlen(str);
                char *norm = snewn(slen + 1, char);
                int norm_len = 0;
                for (int i = 0; i < slen; i++) {
                    if (str[i] == '\r') {
                        if (i + 1 < slen && str[i + 1] == '\n') {
                            i++;
                        }
                        norm[norm_len++] = '\r';
                    } else if (str[i] == '\n') {
                        norm[norm_len++] = '\r';
                    } else {
                        norm[norm_len++] = str[i];
                    }
                }
                session_send(sess, norm, norm_len);
                sfree(norm);
                GlobalUnlock(hglb);
            }
        }
    }
    CloseClipboard();
}

void write_aclip(HWND hwnd, int clipboard, char *data, int len)
{
    copy_to_clipboard_utf8(hwnd, data, len);
}

static bool query_reg_string(HKEY hRoot, const wchar_t *subKey, const wchar_t *valueName, wchar_t *out, size_t max_len)
{
    HKEY hKey = NULL;
    if (RegOpenKeyExW(hRoot, subKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = (DWORD)(max_len * sizeof(wchar_t));
    LONG res = RegQueryValueExW(hKey, valueName, NULL, &type, (LPBYTE)out, &bytes);
    RegCloseKey(hKey);

    if (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out[max_len - 1] = L'\0';
        /* Strip surrounding quotes if present */
        if (out[0] == L'"') {
            size_t len = wcslen(out);
            if (len > 1 && out[len - 1] == L'"') {
                out[len - 1] = L'\0';
                memmove(out, out + 1, len * sizeof(wchar_t));
            }
        }
        /* If there are command line arguments e.g. "...\Code.exe" "%1", strip after .exe */
        wchar_t *exe_ext = wcsstr(out, L".exe");
        if (!exe_ext) exe_ext = wcsstr(out, L".EXE");
        if (exe_ext) {
            *(exe_ext + 4) = L'\0';
        }
        return (out[0] != L'\0');
    }
    return false;
}

static bool find_vscode_path(wchar_t *out_path, size_t max_len)
{
    /* 1. Check SearchPathW for Code.exe */
    if (SearchPathW(NULL, L"Code.exe", NULL, (DWORD)max_len, out_path, NULL) > 0) {
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    /* 2. Check Windows Registry: HKCU App Paths */
    if (query_reg_string(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Code.exe", NULL, out_path, max_len)) {
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    /* 3. Check Windows Registry: HKLM App Paths */
    if (query_reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Code.exe", NULL, out_path, max_len)) {
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    /* 4. Check Windows Registry: HKCR Applications */
    if (query_reg_string(HKEY_CLASSES_ROOT, L"Applications\\Code.exe\\shell\\open\\command", NULL, out_path, max_len)) {
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    /* 5. Check %LOCALAPPDATA%\Programs\Microsoft VS Code\Code.exe */
    wchar_t localappdata[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localappdata, MAX_PATH) > 0) {
        _snwprintf(out_path, max_len, L"%s\\Programs\\Microsoft VS Code\\Code.exe", localappdata);
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    /* 6. Check %ProgramFiles%\Microsoft VS Code\Code.exe */
    wchar_t progfiles[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles", progfiles, MAX_PATH) > 0) {
        _snwprintf(out_path, max_len, L"%s\\Microsoft VS Code\\Code.exe", progfiles);
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    /* 7. Check %ProgramFiles(x86)%\Microsoft VS Code\Code.exe */
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", progfiles, MAX_PATH) > 0) {
        _snwprintf(out_path, max_len, L"%s\\Microsoft VS Code\\Code.exe", progfiles);
        if (GetFileAttributesW(out_path) != INVALID_FILE_ATTRIBUTES)
            return true;
    }

    return false;
}

static bool get_default_wsl_distro(char *out_distro, size_t max_len)
{
    HKEY hLxss = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",
                      0, KEY_READ, &hLxss) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t default_guid[128] = {0};
    DWORD guid_size = sizeof(default_guid);
    DWORD type = 0;
    LONG res = RegQueryValueExW(hLxss, L"DefaultDistribution", NULL, &type,
                                (LPBYTE)default_guid, &guid_size);
    bool found = false;
    if (res == ERROR_SUCCESS && default_guid[0]) {
        HKEY hDistro = NULL;
        if (RegOpenKeyExW(hLxss, default_guid, 0, KEY_READ, &hDistro) == ERROR_SUCCESS) {
            wchar_t wdistro[128] = {0};
            DWORD distro_size = sizeof(wdistro);
            if (RegQueryValueExW(hDistro, L"DistributionName", NULL, &type,
                                 (LPBYTE)wdistro, &distro_size) == ERROR_SUCCESS && wdistro[0]) {
                WideCharToMultiByte(CP_UTF8, 0, wdistro, -1, out_distro, (int)max_len, NULL, NULL);
                found = true;
            }
            RegCloseKey(hDistro);
        }
    }

    if (!found) {
        DWORD idx = 0;
        wchar_t subkey_name[256];
        DWORD subkey_len = sizeof(subkey_name) / sizeof(wchar_t);
        while (RegEnumKeyExW(hLxss, idx++, subkey_name, &subkey_len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
            subkey_len = sizeof(subkey_name) / sizeof(wchar_t);
            HKEY hDistro = NULL;
            if (RegOpenKeyExW(hLxss, subkey_name, 0, KEY_READ, &hDistro) == ERROR_SUCCESS) {
                wchar_t wdistro[128] = {0};
                DWORD distro_size = sizeof(wdistro);
                if (RegQueryValueExW(hDistro, L"DistributionName", NULL, &type,
                                     (LPBYTE)wdistro, &distro_size) == ERROR_SUCCESS && wdistro[0]) {
                    WideCharToMultiByte(CP_UTF8, 0, wdistro, -1, out_distro, (int)max_len, NULL, NULL);
                    found = true;
                    RegCloseKey(hDistro);
                    break;
                }
                RegCloseKey(hDistro);
            }
        }
    }

    RegCloseKey(hLxss);
    return found;
}

static void get_session_wsl_distro(WebViewSession *sess, char *out_distro, size_t max_len)
{
    out_distro[0] = '\0';
    if (sess && sess->cfg) {
        /* Check CONF_remote_cmd e.g. "wsl.exe -d Ubuntu ~" */
        const char *rcmd = conf_get_str_ambi(sess->cfg, CONF_remote_cmd, NULL);
        if (rcmd && *rcmd) {
            const char *d = strstr(rcmd, "-d ");
            if (d) {
                d += 3;
                while (*d == ' ') d++;
                const char *space = strchr(d, ' ');
                size_t len = space ? (size_t)(space - d) : strlen(d);
                if (len > 0 && len < max_len) {
                    strncpy(out_distro, d, len);
                    out_distro[len] = '\0';
                    return;
                }
            }
        }

        /* Check CONF_host e.g. "wsl:Ubuntu" */
        const char *host = conf_get_str(sess->cfg, CONF_host);
        if (host && !strnicmp(host, "wsl:", 4)) {
            const char *d = host + 4;
            if (*d && strlen(d) < max_len) {
                strncpy(out_distro, d, max_len - 1);
                out_distro[max_len - 1] = '\0';
                return;
            }
        }
    }

    /* Fallback to Windows Registry default WSL distro */
    if (get_default_wsl_distro(out_distro, max_len)) {
        return;
    }

    /* Final fallback */
    strncpy(out_distro, "Ubuntu", max_len - 1);
    out_distro[max_len - 1] = '\0';
}

static bool launch_wsl_code(const char *distro, const char *linux_path, int line)
{
    wchar_t wdistro[128] = {0};
    if (distro && *distro) {
        MultiByteToWideChar(CP_UTF8, 0, distro, -1, wdistro, sizeof(wdistro) / sizeof(wchar_t));
    }

    wchar_t wpath[MAX_PATH * 2] = {0};
    MultiByteToWideChar(CP_UTF8, 0, linux_path, -1, wpath, sizeof(wpath) / sizeof(wchar_t));

    wchar_t wtarget[MAX_PATH * 2 + 32] = {0};
    if (line > 0) {
        _snwprintf(wtarget, sizeof(wtarget) / sizeof(wchar_t), L"%s:%d", wpath, line);
    } else {
        _snwprintf(wtarget, sizeof(wtarget) / sizeof(wchar_t), L"%s", wpath);
    }

    /*
     * Build command line for wsl.exe using bash -lc so environment & PATH are properly sourced.
     * Searches in order:
     *   1) code in PATH
     *   2) Windows VS Code bin/code via /mnt/c
     *   3) ~/.vscode-server/bin/wildcard/bin/remote-cli/code
     */
    const wchar_t *script =
        L"code -g \"$1\" 2>/dev/null || "
        L"(for p in /mnt/c/Users/*/AppData/Local/Programs/\"Microsoft VS Code\"/bin/code "
        L"\"/mnt/c/Program Files/Microsoft VS Code/bin/code\" ~/.vscode-server/bin/*/bin/remote-cli/code; "
        L"do [ -x \"$p\" ] && \"$p\" -g \"$1\" && exit 0; done; exit 1)";

    wchar_t cmd[MAX_PATH * 8] = {0};
    if (wdistro[0]) {
        _snwprintf(cmd, sizeof(cmd) / sizeof(wchar_t),
                   L"wsl.exe -d \"%s\" -- bash -lc \"%s\" _ \"%s\"",
                   wdistro, script, wtarget);
    } else {
        _snwprintf(cmd, sizeof(cmd) / sizeof(wchar_t),
                   L"wsl.exe -- bash -lc \"%s\" _ \"%s\"",
                   script, wtarget);
    }

    dbg_log("launch_wsl_code: running cmdline: %ls", cmd);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    memset(&pi, 0, sizeof(pi));

    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        dbg_log("launch_wsl_code: CreateProcessW failed with err=%lu", GetLastError());
        return false;
    }

    /* Wait up to 3000ms for wsl.exe process to complete */
    DWORD wait_res = WaitForSingleObject(pi.hProcess, 3000);
    DWORD exit_code = 0;
    if (wait_res == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exit_code);
        dbg_log("launch_wsl_code: wsl.exe process finished with exit code %lu", exit_code);
    } else {
        dbg_log("launch_wsl_code: wsl.exe process still active after 3s (backgrounded)");
        exit_code = 0;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0) {
        dbg_log("launch_wsl_code: wsl.exe failed with exit_code=%lu", exit_code);
        if (wdistro[0]) {
            /* Try again without -d flag in case distro name differed */
            dbg_log("launch_wsl_code: retrying without -d flag for default WSL instance");
            return launch_wsl_code(NULL, linux_path, line);
        }
        return false;
    }
    return true;
}

static void url_decode(char *dst, const char *src)
{
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            char *end = NULL;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                *dst++ = (char)val;
                src += 3;
                continue;
            }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

typedef struct WebViewEditorWindow {
    HWND hwnd;
    int session_id;
    char session_name[128];
    char remote_host[128];
    bool is_ready;
    char pending_file[MAX_PATH * 2];
    int pending_line;

    /* SFTP worker process handles */
    HANDLE h_proc;
    HANDLE h_stdin_write;
    HANDLE h_stdout_read;
    HANDLE h_stderr_read;
    HANDLE h_reader_thread;
    bool worker_running;
    char temp_pwfile[MAX_PATH];

    struct WebViewEditorWindow *next;
} WebViewEditorWindow;

static WebViewEditorWindow *editor_windows_head = NULL;

static WebViewEditorWindow *editor_window_find_by_session(int session_id)
{
    for (WebViewEditorWindow *ed = editor_windows_head; ed; ed = ed->next) {
        if (ed->session_id == session_id)
            return ed;
    }
    return NULL;
}

static WebViewEditorWindow *editor_window_find_by_hwnd(HWND hwnd)
{
    for (WebViewEditorWindow *ed = editor_windows_head; ed; ed = ed->next) {
        if (ed->hwnd == hwnd)
            return ed;
    }
    return NULL;
}

static DWORD WINAPI sftp_reader_thread_proc(LPVOID param)
{
    WebViewEditorWindow *ed = (WebViewEditorWindow *)param;
    if (!ed || !ed->h_stdout_read) return 0;

    char buf[65536];
    strbuf *line_sb = strbuf_new_nm();
    DWORD bytes_read = 0;
    bool got_ready = false;

    while (ReadFile(ed->h_stdout_read, buf, sizeof(buf) - 1, &bytes_read, NULL) && bytes_read > 0) {
        if (ed->temp_pwfile[0] != '\0') {
            DeleteFileA(ed->temp_pwfile);
            ed->temp_pwfile[0] = '\0';
        }

        for (DWORD i = 0; i < bytes_read; i++) {
            char ch = buf[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (line_sb->len > 0) {
                    if (line_sb->s[0] == '{') {
                        if (strstr(line_sb->s, "\"sftp_ready\"")) {
                            got_ready = true;
                            dbg_log("sftp worker: received sftp_ready: %s", line_sb->s);
                        } else {
                            dbg_log("sftp worker: stdout json (len=%zu): %.120s...", line_sb->len, line_sb->s);
                        }
                        if (ed->hwnd && IsWindow(ed->hwnd)) {
                            webview_host_send_to_window(ed->hwnd, line_sb->s);
                        }
                    } else {
                        dbg_log("sftp worker non-json stdout: %s", line_sb->s);
                    }
                }
                strbuf_clear(line_sb);
            } else {
                put_byte(line_sb, ch);
            }
        }
    }

    if (line_sb->len > 0 && line_sb->s[0] == '{' && ed->hwnd && IsWindow(ed->hwnd)) {
        if (strstr(line_sb->s, "\"sftp_ready\"")) {
            got_ready = true;
        }
        webview_host_send_to_window(ed->hwnd, line_sb->s);
    }
    strbuf_free(line_sb);

    if (ed->temp_pwfile[0] != '\0') {
        DeleteFileA(ed->temp_pwfile);
        ed->temp_pwfile[0] = '\0';
    }

    /* If worker exited without sftp_ready, read stderr to inform the UI */
    if (!got_ready && ed->hwnd && IsWindow(ed->hwnd)) {
        char err_text[1024] = {0};
        DWORD err_read = 0;
        if (ed->h_stderr_read) {
            ReadFile(ed->h_stderr_read, err_text, sizeof(err_text) - 1, &err_read, NULL);
            if (err_read > 0) {
                err_text[err_read] = '\0';
                while (err_read > 0 && (err_text[err_read - 1] == '\r' || err_text[err_read - 1] == '\n')) {
                    err_text[--err_read] = '\0';
                }
            }
        }
        dbg_log("sftp worker exited without sftp_ready. Stderr: '%s'", err_text);

        char esc_err[1024];
        const char *raw_err = (err_text[0] != '\0') ? err_text : "SFTP 身份验证失败或进程异常退出";
        json_escape_string(raw_err, esc_err, sizeof(esc_err));
        char err_msg[1280];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"cmd\":\"sftp_error\",\"error\":\"%s\"}", esc_err);
        webview_host_send_to_window(ed->hwnd, err_msg);
    }

    ed->worker_running = false;
    dbg_log("sftp_reader_thread_proc: exited for hwnd=%p", (void*)ed->hwnd);
    return 0;
}

static void sftp_worker_stop(WebViewEditorWindow *ed)
{
    if (!ed || !ed->worker_running) return;
    dbg_log("sftp_worker_stop: stopping worker for hwnd=%p", (void*)ed->hwnd);

    if (ed->temp_pwfile[0] != '\0') {
        DeleteFileA(ed->temp_pwfile);
        ed->temp_pwfile[0] = '\0';
    }

    if (ed->h_stdin_write) {
        DWORD written = 0;
        WriteFile(ed->h_stdin_write, "{\"cmd\":\"quit\"}\n", 15, &written, NULL);
        FlushFileBuffers(ed->h_stdin_write);
        CloseHandle(ed->h_stdin_write);
        ed->h_stdin_write = NULL;
    }

    if (ed->h_proc) {
        if (WaitForSingleObject(ed->h_proc, 500) == WAIT_TIMEOUT) {
            dbg_log("sftp_worker_stop: terminating worker process");
            TerminateProcess(ed->h_proc, 0);
        }
        CloseHandle(ed->h_proc);
        ed->h_proc = NULL;
    }

    if (ed->h_reader_thread) {
        WaitForSingleObject(ed->h_reader_thread, 1000);
        CloseHandle(ed->h_reader_thread);
        ed->h_reader_thread = NULL;
    }

    if (ed->h_stdout_read) {
        CloseHandle(ed->h_stdout_read);
        ed->h_stdout_read = NULL;
    }

    if (ed->h_stderr_read) {
        CloseHandle(ed->h_stderr_read);
        ed->h_stderr_read = NULL;
    }

    ed->worker_running = false;
    dbg_log("sftp_worker_stop: worker stopped cleanly");
}

static bool sftp_worker_start(WebViewEditorWindow *ed)
{
    if (!ed || ed->worker_running) return true;

    WebViewSession *sess = session_find(ed->session_id);
    if (!sess) {
        dbg_log("sftp_worker_start: session %d not found", ed->session_id);
        return false;
    }

    int proto = conf_get_int(sess->cfg, CONF_protocol);
    if (proto != PROT_SSH) {
        dbg_log("sftp_worker_start: session %d is not SSH (proto=%d)", ed->session_id, proto);
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"cmd\":\"sftp_error\",\"error\":\"SFTP 仅支持 SSH 会话 (当前协议: %d)\"}", proto);
        webview_host_send_to_window(ed->hwnd, err_msg);
        return false;
    }

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    char *slash = strrchr(exe_dir, '\\');
    if (slash) *(slash + 1) = '\0';
    else exe_dir[0] = '\0';

    char psftp_path[MAX_PATH];
    snprintf(psftp_path, sizeof(psftp_path), "%spsftp.exe", exe_dir);

    if (GetFileAttributesA(psftp_path) == INVALID_FILE_ATTRIBUTES) {
        if (SearchPathA(NULL, "psftp.exe", NULL, sizeof(psftp_path), psftp_path, NULL) == 0) {
            dbg_log("sftp_worker_start: psftp.exe not found at '%s'", psftp_path);
            char esc_path[MAX_PATH * 2];
            json_escape_string(psftp_path, esc_path, sizeof(esc_path));
            char err_msg[1024];
            snprintf(err_msg, sizeof(err_msg),
                     "{\"cmd\":\"sftp_error\",\"error\":\"未找到 psftp.exe: %s\"}", esc_path);
            webview_host_send_to_window(ed->hwnd, err_msg);
            return false;
        }
    }

    const char *host = conf_get_str(sess->cfg, CONF_host);
    int port = conf_get_int(sess->cfg, CONF_port);
    const char *cfg_user = conf_get_str_ambi(sess->cfg, CONF_username, NULL);
    char effective_user[128] = {0};
    if (sess->temp_prompt_user[0] != '\0') {
        strncpy(effective_user, sess->temp_prompt_user, sizeof(effective_user) - 1);
    } else if (cfg_user && *cfg_user) {
        strncpy(effective_user, cfg_user, sizeof(effective_user) - 1);
    }

    char pass[256] = {0};
    if (sess->active_auth_pass[0] != '\0') {
        strncpy(pass, sess->active_auth_pass, sizeof(pass) - 1);
    } else if (sess->temp_prompt_pass[0] != '\0') {
        strncpy(pass, sess->temp_prompt_pass, sizeof(pass) - 1);
    } else {
        /* Fallback: load saved credentials from pw/ directory */
        bool loaded = false;
        if (effective_user[0] != '\0') {
            wchar_t fp[MAX_PATH];
            pw_get_user_filepath(fp, MAX_PATH, host, port, effective_user);
            bool rem = false, autolog = false;
            char u[128] = {0};
            if (pw_load_single_file(fp, u, sizeof(u), pass, sizeof(pass), &rem, &autolog)) {
                loaded = true;
                dbg_log("sftp_worker_start: loaded saved password for %s@%s:%d from %ls", effective_user, host, port, fp);
            }
        }
        if (!loaded) {
            /* Try last user file */
            wchar_t last_file[MAX_PATH];
            pw_get_last_user_filepath(last_file, MAX_PATH, host, port);
            FILE *lfp = _wfopen(last_file, L"r");
            if (lfp) {
                char last_u[128] = {0};
                if (fgets(last_u, sizeof(last_u), lfp)) {
                    char *nl = strpbrk(last_u, "\r\n");
                    if (nl) *nl = '\0';
                    if (last_u[0] != '\0') {
                        wchar_t fp[MAX_PATH];
                        pw_get_user_filepath(fp, MAX_PATH, host, port, last_u);
                        bool rem = false, autolog = false;
                        char u[128] = {0};
                        if (pw_load_single_file(fp, u, sizeof(u), pass, sizeof(pass), &rem, &autolog)) {
                            loaded = true;
                            if (effective_user[0] == '\0') {
                                strncpy(effective_user, last_u, sizeof(effective_user) - 1);
                            }
                            dbg_log("sftp_worker_start: loaded saved password for last user %s@%s:%d", last_u, host, port);
                        }
                    }
                }
                fclose(lfp);
            }
        }
        if (!loaded) {
            /* Try legacy file */
            wchar_t legacy_file[MAX_PATH];
            pw_get_legacy_filepath(legacy_file, MAX_PATH, host, port);
            bool rem = false, autolog = false;
            char u[128] = {0};
            if (pw_load_single_file(legacy_file, u, sizeof(u), pass, sizeof(pass), &rem, &autolog)) {
                loaded = true;
                if (effective_user[0] == '\0' && u[0] != '\0') {
                    strncpy(effective_user, u, sizeof(effective_user) - 1);
                }
                dbg_log("sftp_worker_start: loaded saved password from legacy file %ls", legacy_file);
            }
        }
    }

    Filename *keyfn = conf_get_filename(sess->cfg, CONF_keyfile);
    const char *keyfile = keyfn ? filename_to_str(keyfn) : "";

    char cmdline[2048];
    int len = snprintf(cmdline, sizeof(cmdline), "\"%s\" -rpc -batch -share", psftp_path);

    if (port > 0 && port != 22) {
        len += snprintf(cmdline + len, sizeof(cmdline) - len, " -P %d", port);
    }
    if (keyfile && *keyfile) {
        len += snprintf(cmdline + len, sizeof(cmdline) - len, " -i \"%s\"", keyfile);
    }
    if (pass[0] != '\0') {
        char temp_dir[MAX_PATH];
        GetTempPathA(sizeof(temp_dir), temp_dir);
        char temp_file[MAX_PATH];
        if (GetTempFileNameA(temp_dir, "ptp", 0, temp_file)) {
            FILE *pfp = fopen(temp_file, "wb");
            if (pfp) {
                fprintf(pfp, "%s\n", pass);
                fclose(pfp);
                strncpy(ed->temp_pwfile, temp_file, sizeof(ed->temp_pwfile) - 1);
                len += snprintf(cmdline + len, sizeof(cmdline) - len, " -pwfile \"%s\"", temp_file);
            } else {
                len += snprintf(cmdline + len, sizeof(cmdline) - len, " -pw \"%s\"", pass);
            }
        } else {
            len += snprintf(cmdline + len, sizeof(cmdline) - len, " -pw \"%s\"", pass);
        }
        smemclr(pass, sizeof(pass));
    }
    if (effective_user[0] != '\0') {
        len += snprintf(cmdline + len, sizeof(cmdline) - len, " %s@%s", effective_user, (host && *host) ? host : "localhost");
    } else {
        len += snprintf(cmdline + len, sizeof(cmdline) - len, " %s", (host && *host) ? host : "localhost");
    }

    dbg_log("sftp_worker_start: spawning psftp for session %d (port=%d, host=%s, user=%s, has_key=%d, has_pwfile=%d)",
            ed->session_id, port, host, effective_user, (keyfile && *keyfile) ? 1 : 0, (ed->temp_pwfile[0] != '\0') ? 1 : 0);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE h_stdin_read = NULL;
    HANDLE h_stdout_write = NULL;
    HANDLE h_stderr_write = NULL;

    if (!CreatePipe(&h_stdin_read, &ed->h_stdin_write, &sa, 0)) {
        dbg_log("sftp_worker_start: CreatePipe stdin failed %lu", GetLastError());
        if (ed->temp_pwfile[0] != '\0') {
            DeleteFileA(ed->temp_pwfile);
            ed->temp_pwfile[0] = '\0';
        }
        return false;
    }
    SetHandleInformation(ed->h_stdin_write, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&ed->h_stdout_read, &h_stdout_write, &sa, 0)) {
        dbg_log("sftp_worker_start: CreatePipe stdout failed %lu", GetLastError());
        if (ed->temp_pwfile[0] != '\0') {
            DeleteFileA(ed->temp_pwfile);
            ed->temp_pwfile[0] = '\0';
        }
        CloseHandle(h_stdin_read);
        CloseHandle(ed->h_stdin_write);
        ed->h_stdin_write = NULL;
        return false;
    }
    SetHandleInformation(ed->h_stdout_read, HANDLE_FLAG_INHERIT, 0);

    if (!CreatePipe(&ed->h_stderr_read, &h_stderr_write, &sa, 0)) {
        ed->h_stderr_read = NULL;
    } else {
        SetHandleInformation(ed->h_stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = h_stdin_read;
    si.hStdOutput = h_stdout_write;
    si.hStdError = h_stderr_write ? h_stderr_write : h_stdout_write;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(h_stdin_read);
    CloseHandle(h_stdout_write);
    if (h_stderr_write) CloseHandle(h_stderr_write);

    if (!ok) {
        dbg_log("sftp_worker_start: CreateProcessA failed %lu", GetLastError());
        if (ed->temp_pwfile[0] != '\0') {
            DeleteFileA(ed->temp_pwfile);
            ed->temp_pwfile[0] = '\0';
        }
        CloseHandle(ed->h_stdin_write);
        CloseHandle(ed->h_stdout_read);
        if (ed->h_stderr_read) {
            CloseHandle(ed->h_stderr_read);
            ed->h_stderr_read = NULL;
        }
        ed->h_stdin_write = NULL;
        ed->h_stdout_read = NULL;
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg),
                 "{\"cmd\":\"sftp_error\",\"error\":\"启动 psftp.exe 失败 (Error %lu)\"}", GetLastError());
        webview_host_send_to_window(ed->hwnd, err_msg);
        return false;
    }

    ed->h_proc = pi.hProcess;
    CloseHandle(pi.hThread);
    ed->worker_running = true;

    ed->h_reader_thread = CreateThread(NULL, 0, sftp_reader_thread_proc, ed, 0, NULL);
    dbg_log("sftp_worker_start: psftp worker spawned pid=%lu, reader thread=%p", pi.dwProcessId, (void*)ed->h_reader_thread);
    return true;
}

static void editor_window_destroy(HWND hwnd)
{
    dbg_log("editor_window_destroy: closing editor window %p", (void*)hwnd);
    WebViewEditorWindow **pp = &editor_windows_head;
    while (*pp) {
        if ((*pp)->hwnd == hwnd) {
            WebViewEditorWindow *to_free = *pp;
            *pp = (*pp)->next;
            sftp_worker_stop(to_free);
            webview_host_close(hwnd);
            sfree(to_free);
            dbg_log("editor_window_destroy: editor window %p destroyed and unlinked", (void*)hwnd);
            return;
        }
        pp = &(*pp)->next;
    }
}

static void editor_window_close_for_session(int session_id)
{
    WebViewEditorWindow *ed = editor_window_find_by_session(session_id);
    if (ed && ed->hwnd && IsWindow(ed->hwnd)) {
        dbg_log("editor_window_close_for_session: destroying editor window for session %d", session_id);
        DestroyWindow(ed->hwnd);
    }
}

static void on_editor_web_message(HWND hwnd, const char *message, void *userdata)
{
    if (!message || !*message) return;
    dbg_log("on_editor_web_message: hwnd=%p, msg='%s'", (void*)hwnd, message);

    WebViewEditorWindow *ed = editor_window_find_by_hwnd(hwnd);
    if (!ed) return;

    if (strstr(message, "\"cmd\":\"editor_init\"") || strstr(message, "\"cmd\": \"editor_init\"")) {
        ed->is_ready = true;
        char resp[512];
        snprintf(resp, sizeof(resp),
                 "{\"cmd\":\"editor_init_ack\",\"sessionId\":%d,\"sessionName\":\"%s\",\"host\":\"%s\"}",
                 ed->session_id, ed->session_name, ed->remote_host);
        webview_host_send_to_window(hwnd, resp);

        sftp_worker_start(ed);

        if (ed->pending_file[0]) {
            char esc_file[MAX_PATH * 2];
            json_escape_string(ed->pending_file, esc_file, sizeof(esc_file));
            char open_cmd[MAX_PATH * 2 + 64];
            snprintf(open_cmd, sizeof(open_cmd),
                     "{\"cmd\":\"open_file\",\"path\":\"%s\",\"line\":%d}",
                     esc_file, ed->pending_line);
            webview_host_send_to_window(hwnd, open_cmd);
            ed->pending_file[0] = '\0';
            ed->pending_line = 0;
        }
        return;
    }

    if (strncmp(message, "{\"cmd\":\"sftp_", 13) == 0) {
        if (ed->worker_running && ed->h_stdin_write) {
            DWORD written = 0;
            WriteFile(ed->h_stdin_write, message, (DWORD)strlen(message), &written, NULL);
            WriteFile(ed->h_stdin_write, "\n", 1, &written, NULL);
            dbg_log("on_editor_web_message: forwarded SFTP cmd (len=%zu): %.100s...", strlen(message), message);
        } else {
            dbg_log("on_editor_web_message: WARNING: received SFTP cmd but worker is not ready! (worker_running=%d, h_stdin_write=%p)",
                    ed->worker_running, (void*)ed->h_stdin_write);
            char err_msg[256];
            snprintf(err_msg, sizeof(err_msg),
                     "{\"cmd\":\"sftp_error\",\"error\":\"SFTP 工作进程未就绪或已退出\"}");
            webview_host_send_to_window(hwnd, err_msg);
        }
        return;
    }

    if (strstr(message, "\"cmd\":\"get_host_log\"") || strstr(message, "\"cmd\": \"get_host_log\"")) {
        strbuf *log_sb = strbuf_new_nm();
        mem_log_dump_to_strbuf(log_sb);

        strbuf *resp = strbuf_new_nm();
        put_fmt(resp, "{\"cmd\":\"host_log_resp\",\"content\":");
        json_escape_to_strbuf(resp, log_sb->s);
        put_data(resp, "}\n", 2);
        webview_host_send_to_window(hwnd, resp->s);
        strbuf_free(log_sb);
        strbuf_free(resp);
        return;
    }

    if (strstr(message, "\"cmd\":\"ping\"") || strstr(message, "\"cmd\": \"ping\"")) {
        webview_host_send_to_window(hwnd, "{\"cmd\":\"pong\"}");
        return;
    }

    if (strstr(message, "\"cmd\":\"close\"") || strstr(message, "\"cmd\": \"close\"")) {
        PostMessage(hwnd, WM_CLOSE, 0, 0);
        return;
    }
}

static LRESULT CALLBACK WebViewEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_WEBVIEW_POST_MSG: {
        char *str = (char *)lParam;
        if (str) {
            webview_host_send_to_window(hwnd, str);
            free(str);
        }
        return 0;
    }
    case WM_SIZE:
        webview_host_resize(hwnd);
        return 0;
    case WM_SETFOCUS:
        webview_host_focus(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            webview_host_focus(hwnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        editor_window_destroy(hwnd);
        return 0;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

static void get_default_window_rect(int *out_x, int *out_y, int *out_w, int *out_h)
{
    RECT rcWork;
    /* sqrt(0.60) ≈ 0.77459667: preserving screen aspect ratio yields exactly 60% screen area */
    const double ratio = 0.77459667;
    int work_w, work_h, work_left = 0, work_top = 0;

    if (SystemParametersInfo(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        work_left = rcWork.left;
        work_top = rcWork.top;
        work_w = rcWork.right - rcWork.left;
        work_h = rcWork.bottom - rcWork.top;
    } else {
        work_w = GetSystemMetrics(SM_CXSCREEN);
        work_h = GetSystemMetrics(SM_CYSCREEN);
    }

    int w = (int)(work_w * ratio);
    int h = (int)(work_h * ratio);
    int x = work_left + (work_w - w) / 2;
    int y = work_top + (work_h - h) / 2;

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

static HWND editor_window_open(int session_id, const char *initial_file, int initial_line)
{
    WebViewSession *sess = session_find(session_id);
    if (!sess) {
        dbg_log("editor_window_open: session %d not found", session_id);
        return NULL;
    }

    WebViewEditorWindow *ed = editor_window_find_by_session(session_id);
    if (ed && ed->hwnd && IsWindow(ed->hwnd)) {
        dbg_log("editor_window_open: activating existing editor window for session %d", session_id);
        ShowWindow(ed->hwnd, SW_RESTORE);
        SetForegroundWindow(ed->hwnd);
        webview_host_focus(ed->hwnd);

        if (initial_file && *initial_file) {
            if (ed->is_ready) {
                char esc_file[MAX_PATH * 2];
                json_escape_string(initial_file, esc_file, sizeof(esc_file));
                char open_cmd[MAX_PATH * 2 + 64];
                snprintf(open_cmd, sizeof(open_cmd),
                         "{\"cmd\":\"open_file\",\"path\":\"%s\",\"line\":%d}",
                         esc_file, initial_line);
                webview_host_send_to_window(ed->hwnd, open_cmd);
            } else {
                strncpy(ed->pending_file, initial_file, sizeof(ed->pending_file) - 1);
                ed->pending_line = initial_line;
            }
        }
        return ed->hwnd;
    }

    char title[256];
    const char *h = conf_get_str(sess->cfg, CONF_host);
    snprintf(title, sizeof(title), "PuTTY Remote Editor - %s [%s]",
             sess->name, (h && *h) ? h : "Remote");

    int def_x, def_y, def_w, def_h;
    get_default_window_rect(&def_x, &def_y, &def_w, &def_h);

    HWND hwnd = CreateWindowEx(
        WS_EX_APPWINDOW,
        "PuTTYWebViewEditorClass",
        title,
        WS_OVERLAPPEDWINDOW,
        def_x, def_y, def_w, def_h,
        NULL, NULL, hinst, NULL
    );
    if (!hwnd) {
        dbg_log("editor_window_open: CreateWindowEx failed");
        return NULL;
    }

    ed = snew(WebViewEditorWindow);
    memset(ed, 0, sizeof(*ed));
    ed->hwnd = hwnd;
    ed->session_id = session_id;
    strncpy(ed->session_name, sess->name, sizeof(ed->session_name) - 1);
    if (h && *h) strncpy(ed->remote_host, h, sizeof(ed->remote_host) - 1);
    if (initial_file && *initial_file) {
        strncpy(ed->pending_file, initial_file, sizeof(ed->pending_file) - 1);
        ed->pending_line = initial_line;
    }

    ed->next = editor_windows_head;
    editor_windows_head = ed;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    dbg_log("editor_window_open: initializing WebView2 with '%ls'", global_editor_html_path);
    webview_host_init(hwnd, global_editor_html_path, on_editor_web_message, NULL);
    return hwnd;
}

static void open_url_or_file(HWND hwnd, int sess_id, const char *input)
{
    if (!input || !*input)
        return;

    dbg_log("open_url_or_file: sess_id=%d, raw input='%s'", sess_id, input);

    /* Web links or email */
    if (!strncmp(input, "http://", 7) || !strncmp(input, "https://", 8) || !strncmp(input, "mailto:", 7)) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, input, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t *wurl = (wchar_t *)smalloc(wlen * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, input, -1, wurl, wlen);
            ShellExecuteW(hwnd, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
            sfree(wurl);
        }
        return;
    }

    /* Custom protocol (vscode://, etc.) */
    if (!strncmp(input, "vscode://", 9)) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, input, -1, NULL, 0);
        if (wlen > 0) {
            wchar_t *wurl = (wchar_t *)smalloc(wlen * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, input, -1, wurl, wlen);
            ShellExecuteW(hwnd, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
            sfree(wurl);
        }
        return;
    }

    /* Process file:// or local paths */
    char raw[2048];
    strncpy(raw, input, sizeof(raw) - 1);
    raw[sizeof(raw) - 1] = '\0';

    int line = 0;
    /* Check for #L1779-L1815 or #L1779 or #1779 */
    char *hash = strrchr(raw, '#');
    if (hash) {
        char *p = hash + 1;
        if (*p == 'L' || *p == 'l') p++;
        if (*p >= '0' && *p <= '9') {
            line = atoi(p);
        }
        *hash = '\0';
    }

    /* If no hash, check for trailing :line or :line:col */
    if (line == 0) {
        char *colon = strrchr(raw, ':');
        if (colon && colon > raw + 2) {
            char *p = colon + 1;
            bool all_digits = (*p != '\0');
            for (char *q = p; *q; q++) {
                if (*q < '0' || *q > '9') { all_digits = false; break; }
            }
            if (all_digits) {
                *colon = '\0';
                char *colon1 = strrchr(raw, ':');
                if (colon1 && colon1 > raw + 2) {
                    char *p1 = colon1 + 1;
                    bool c1_digits = (*p1 != '\0');
                    for (char *q = p1; *q; q++) {
                        if (*q < '0' || *q > '9') { c1_digits = false; break; }
                    }
                    if (c1_digits) {
                        line = atoi(p1);
                        *colon1 = '\0';
                    } else {
                        line = atoi(p);
                    }
                } else {
                    line = atoi(p);
                }
            }
        }
    }

    char decoded[2048];
    url_decode(decoded, raw);

    const char *p = decoded;
    if (!strncmp(p, "file://", 7)) {
        p += 7;
        if (p[0] == '/' && p[1] != '/') {
            if (isalpha((unsigned char)p[1]) && p[2] == ':') {
                p++; /* file:///C:/path -> C:/path */
            }
        } else if (!strncmp(p, "localhost/", 10)) {
            p += 9;
            if (isalpha((unsigned char)p[1]) && p[2] == ':') {
                p++;
            }
        }
    }

    WebViewSession *sess = session_find(sess_id);

    /* Check if this is an SSH session - route to independent SFTP Editor window! */
    if (sess && sess->cfg && conf_get_int(sess->cfg, CONF_protocol) == PROT_SSH) {
        dbg_log("open_url_or_file: SSH remote session %d, routing to SFTP Editor window: '%s', line=%d",
                sess_id, p, line);
        editor_window_open(sess_id, p, line);
        return;
    }

    char distro[128] = {0};
    get_session_wsl_distro(sess, distro, sizeof(distro));

    /* Detect whether this is a WSL session (ConPTY protocol + distro resolved) */
    bool is_wsl_session = false;
    if (sess && sess->cfg) {
        int proto = conf_get_int(sess->cfg, CONF_protocol);
        if (proto == PROT_CONPTY) {
            /* It's a ConPTY session.  Consider it WSL if either:
               - a distro name was resolved (from cmd line / host / registry), or
               - the remote_cmd contains "wsl"                                */
            const char *rcmd = conf_get_str_ambi(sess->cfg, CONF_remote_cmd, NULL);
            const char *host  = conf_get_str(sess->cfg, CONF_host);
            if (distro[0] ||
                (rcmd && strstr(rcmd, "wsl")) ||
                (host && strnicmp(host, "wsl:", 4) == 0)) {
                is_wsl_session = true;
            }
        }
    }

    dbg_log("open_url_or_file: is_wsl_session=%d, distro='%s', path='%s', line=%d",
            (int)is_wsl_session, distro, p, line);

    /* ── WSL session: always hand the Linux path to WSL's code command ───── */
    if (is_wsl_session && p[0] == '/') {
        dbg_log("open_url_or_file: WSL session - executing 'code' inside WSL for '%s'", p);
        if (launch_wsl_code(distro, p, line)) {
            dbg_log("open_url_or_file: WSL code launch OK");
            return;
        }
        /* launch failed – fall through to Windows path as last resort */
        dbg_log("open_url_or_file: WSL code launch failed, falling back to Windows path");
    }

    /* ── Non-WSL: native Linux path (rare, e.g. SSH-tunnelled local) ──────── */
    bool is_mnt = (p[0] == '/' && (p[1] == 'm' || p[1] == 'M') &&
                   (p[2] == 'n' || p[2] == 'N') &&
                   (p[3] == 't' || p[3] == 'T') && p[4] == '/' &&
                   isalpha((unsigned char)p[5]) &&
                   (p[6] == '/' || p[6] == '\0'));

    if (!is_wsl_session && p[0] == '/' && !is_mnt) {
        dbg_log("open_url_or_file: non-WSL native Linux path '%s', trying wsl.exe code", p);
        if (launch_wsl_code(distro, p, line)) {
            dbg_log("open_url_or_file: launched via wsl.exe code successfully");
            return;
        }
        dbg_log("open_url_or_file: launch_wsl_code failed, falling back to UNC path");
    }

    /* ── Windows path fallback (non-WSL, /mnt/c/... or drive paths) ───────── */
    char win_path[MAX_PATH * 2] = {0};
    if (is_mnt) {
        char drive = (char)toupper((unsigned char)p[5]);
        const char *rest = (p[6] == '/') ? (p + 7) : "";
        snprintf(win_path, sizeof(win_path), "%c:\\%s", drive, rest);
    } else if (isalpha((unsigned char)p[0]) && p[1] == ':') {
        snprintf(win_path, sizeof(win_path), "%s", p);
    } else if (p[0] == '/' && p[1] != '/') {
        /* Last-resort UNC path: \\wsl.localhost\<distro>\<path> */
        snprintf(win_path, sizeof(win_path), "\\\\wsl.localhost\\%s%s", distro, p);
    } else {
        snprintf(win_path, sizeof(win_path), "%s", p);
    }

    for (char *c = win_path; *c; c++) {
        if (*c == '/') *c = '\\';
    }

    dbg_log("open_url_or_file: resolved win_path='%s', line=%d", win_path, line);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, win_path, -1, NULL, 0);
    if (wlen <= 0) return;
    wchar_t *wwin_path = (wchar_t *)smalloc(wlen * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, win_path, -1, wwin_path, wlen);

    wchar_t vscode_exe[MAX_PATH];
    if (find_vscode_path(vscode_exe, MAX_PATH)) {
        wchar_t params[MAX_PATH * 2 + 32];
        if (line > 0) {
            _snwprintf(params, sizeof(params) / sizeof(wchar_t), L"-g \"%s:%d\"", wwin_path, line);
        } else {
            _snwprintf(params, sizeof(params) / sizeof(wchar_t), L"-g \"%s\"", wwin_path);
        }
        dbg_log("open_url_or_file: launching Windows VS Code with params");
        ShellExecuteW(hwnd, L"open", vscode_exe, params, NULL, SW_SHOWNORMAL);
    } else {
        dbg_log("open_url_or_file: launching via default ShellExecute");
        ShellExecuteW(hwnd, L"open", wwin_path, NULL, NULL, SW_SHOWNORMAL);
    }
    sfree(wwin_path);
}


static WebViewSession *session_create(HWND target_hwnd, Conf *conf_to_use, const char *suggested_title)
{
    int proto = conf_get_int(conf_to_use, CONF_protocol);
    const struct BackendVtable *vt = backend_vt_from_proto(proto);
    if (!vt) {
        dbg_log("Fatal: unsupported protocol %d", proto);
        if (target_hwnd) {
            char banner[512];
            snprintf(banner, sizeof(banner), "\r\n\x1b[1;31m[Unsupported protocol: %d]\x1b[0m\r\n", proto);
            webview_host_send_session_binary_to_window(target_hwnd, '0', next_session_id, banner, strlen(banner));
        }
        return NULL;
    }

    WebViewSession *sess = snew(WebViewSession);
    memset(sess, 0, sizeof(*sess));
    sess->id = next_session_id++;
    sess->cfg = conf_copy(conf_to_use);
    if (proto == PROT_SSH) {
        conf_set_bool(sess->cfg, CONF_ssh_connection_sharing, true);
        conf_set_bool(sess->cfg, CONF_ssh_connection_sharing_upstream, true);
        conf_set_bool(sess->cfg, CONF_ssh_connection_sharing_downstream, false);
    }
    sess->hwnd = target_hwnd;

    memset(&sess->wgs, 0, sizeof(sess->wgs));
    sess->wgs.seat.vt = &webview_seat_vt;
    sess->wgs.logpolicy.vt = &win_gui_logpolicy_vt;
    sess->wgs.term_hwnd = target_hwnd;
    sess->wgs.conf = sess->cfg;

    sess->logctx = log_init(&sess->wgs.logpolicy, sess->cfg);
    sess->wgs.logctx = sess->logctx;
    seat_set_trust_status(&sess->wgs.seat, true);

    bufchain_init(&sess->send_queue);
    sess->send_rate_limit = conf_get_int(sess->cfg, CONF_send_rate_limit);
    if (sess->send_rate_limit < 0)
        sess->send_rate_limit = 0;
    sess->send_next_tick = 0;
    sess->send_timer_active = false;

    if (suggested_title && *suggested_title) {
        strncpy(sess->name, suggested_title, sizeof(sess->name) - 1);
    } else {
        char *host = conf_get_str(sess->cfg, CONF_host);
        if (proto == PROT_CONPTY) {
            if (host && *host) {
                snprintf(sess->name, sizeof(sess->name), "WSL (%s)", host);
            } else {
                strncpy(sess->name, "WSL", sizeof(sess->name) - 1);
            }
        } else if (proto == PROT_SERIAL) {
            const char *serline = conf_get_str(sess->cfg, CONF_serline);
            int speed = conf_get_int(sess->cfg, CONF_serspeed);
            if (serline && *serline) {
                snprintf(sess->name, sizeof(sess->name), "%s (%d)", serline, speed);
            } else {
                strncpy(sess->name, "Serial", sizeof(sess->name) - 1);
            }
        } else if (host && *host) {
            strncpy(sess->name, host, sizeof(sess->name) - 1);
        } else {
            snprintf(sess->name, sizeof(sess->name), "Session %d", sess->id);
        }
    }

    sess->in_connecting = true;
    char *realhost = NULL;
    char *err = backend_init(vt, &sess->wgs.seat, &sess->backend, sess->logctx, sess->cfg,
                             conf_get_str(sess->cfg, CONF_host),
                             conf_get_int(sess->cfg, CONF_port),
                             &realhost,
                             conf_get_bool(sess->cfg, CONF_tcp_nodelay),
                             conf_get_bool(sess->cfg, CONF_tcp_keepalives));
    sfree(realhost);
    sess->in_connecting = false;

    /* Append to sessions list */
    sess->next = NULL;
    if (!sessions_head) {
        sessions_head = sess;
    } else {
        WebViewSession *cur = sessions_head;
        while (cur->next) cur = cur->next;
        cur->next = sess;
    }

    /* Notify target window to create new Tab */
    if (target_hwnd) {
        char tab_msg[256];
        snprintf(tab_msg, sizeof(tab_msg), "T%d:%s", sess->id, sess->name);
        webview_host_send_to_window(target_hwnd, tab_msg);
    }

    if (err) {
        dbg_log("Connection failed for session %d: %s", sess->id, err);
        char banner[512];
        snprintf(banner, sizeof(banner), "\r\n\x1b[1;31m[Connection error: %s]\x1b[0m\r\n", err);
        session_record_output(sess, banner, strlen(banner));
        if (target_hwnd) {
            webview_host_send_session_binary_to_window(target_hwnd, '0', sess->id, banner, strlen(banner));
            webview_host_send_session_text_to_window(target_hwnd, '2', sess->id, "disconnected");
        }
        sfree(err);
        session_cleanup_backend(sess);
        sess->is_connected = false;
        if (sess->auto_reconnect) {
            session_schedule_reconnect(sess);
        }
    } else {
        sess->is_connected = true;
        sess->wgs.backend = sess->backend;
    }

    return sess;
}

static void session_close(int id)
{
    dbg_log("session_close: requested to close session %d", id);
    WebViewSession **pp = &sessions_head;
    WebViewSession *target = NULL;
    while (*pp) {
        WebViewSession *s = *pp;
        if (s->id == id) {
            *pp = s->next;
            target = s;
            break;
        }
        pp = &(*pp)->next;
    }

    if (target) {
        dbg_log("session_close: closing session %d (hwnd=%p, auto_reconnect=%d)", id, (void*)target->hwnd, target->auto_reconnect);
        target->auto_reconnect = false;
        target->reconnect_timer_active = false;
        expire_timer_context(&target->reconnect_timer_active);
        expire_timer_context(target);
        delete_callbacks_for_context(target);

        if (target->log_fp) {
            fclose(target->log_fp);
            target->log_fp = NULL;
        }
        target->is_logging = false;
        target->log_enabled = false;
        expire_timer_context(target);
        expire_timer_context(&target->reconnect_timer_active);
        target->reconnect_timer_active = false;
        bufchain_clear(&target->send_queue);
        target->send_timer_active = false;

        HWND win_hwnd = target->hwnd;
        session_cleanup_backend(target);
        delete_callbacks_for_context(target);
        expire_timer_context(&target->reconnect_timer_active);
        expire_timer_context(target);

        if (target->logctx) {
            log_free(target->logctx);
            target->logctx = NULL;
        }
        if (target->cfg) {
            conf_free(target->cfg);
            target->cfg = NULL;
        }
        editor_window_close_for_session(id);
        sfree(target);

        if (win_hwnd) {
            char close_msg[32];
            snprintf(close_msg, sizeof(close_msg), "X%d", id);
            webview_host_send_to_window(win_hwnd, close_msg);

            /* Check if any remaining sessions in this window */
            bool window_has_session = false;
            for (WebViewSession *s = sessions_head; s; s = s->next) {
                if (s->hwnd == win_hwnd) {
                    window_has_session = true;
                    break;
                }
            }
            if (!window_has_session) {
                dbg_log("session_close: no more sessions in window %p, destroying window", (void*)win_hwnd);
                DestroyWindow(win_hwnd);
            }
        }
    } else {
        dbg_log("session_close: session %d not found in sessions list", id);
    }

    if (!sessions_head) {
        dbg_log("session_close: all sessions closed, posting quit message");
        PostQuitMessage(0);
    }
}

static HWND create_webview_window(int x, int y, int width, int height)
{
    HWND hwnd = CreateWindowEx(
        0,
        "PuTTYWebViewHostClass",
        appname,
        WS_OVERLAPPEDWINDOW,
        x, y, width, height,
        NULL, NULL, hinst, NULL
    );
    if (!hwnd) return NULL;

    WebViewWindow *win = snew(WebViewWindow);
    win->hwnd = hwnd;
    win->next = windows_head;
    windows_head = win;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    webview_host_init(hwnd, global_html_path, on_web_message, NULL);
    return hwnd;
}

static void session_detach_to_new_window(int sess_id, int screen_x, int screen_y)
{
    WebViewSession *sess = session_find(sess_id);
    if (!sess) return;
    HWND old_hwnd = sess->hwnd;

    int remaining_in_old = 0;
    for (WebViewSession *s = sessions_head; s; s = s->next) {
        if (s->hwnd == old_hwnd && s->id != sess_id)
            remaining_in_old++;
    }

    int def_x, def_y, def_w, def_h;
    get_default_window_rect(&def_x, &def_y, &def_w, &def_h);

    int x = (screen_x > 0) ? (screen_x - 100) : def_x;
    int y = (screen_y > 0) ? (screen_y - 20) : def_y;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND new_hwnd = create_webview_window(x, y, def_w, def_h);
    if (!new_hwnd) return;

    /* Tell old window to remove tab */
    if (old_hwnd) {
        char close_msg[32];
        snprintf(close_msg, sizeof(close_msg), "X%d", sess_id);
        webview_host_send_to_window(old_hwnd, close_msg);
    }

    /* Transfer session ownership to new window */
    sess->hwnd = new_hwnd;
    sess->wgs.term_hwnd = new_hwnd;

    /* If old window has no tabs left, close old window */
    if (remaining_in_old == 0 && old_hwnd) {
        DestroyWindow(old_hwnd);
    }
}

static void session_attach_to_window(HWND target_hwnd, int sess_id)
{
    WebViewSession *sess = session_find(sess_id);
    if (!sess || !target_hwnd) return;
    HWND old_hwnd = sess->hwnd;
    if (old_hwnd == target_hwnd) return;

    int remaining_in_old = 0;
    for (WebViewSession *s = sessions_head; s; s = s->next) {
        if (s->hwnd == old_hwnd && s->id != sess_id)
            remaining_in_old++;
    }

    /* Remove tab from old window */
    if (old_hwnd) {
        char close_msg[32];
        snprintf(close_msg, sizeof(close_msg), "X%d", sess_id);
        webview_host_send_to_window(old_hwnd, close_msg);
    }

    /* Transfer session ownership to target_hwnd */
    sess->hwnd = target_hwnd;
    sess->wgs.term_hwnd = target_hwnd;

    /* Add tab to target window */
    char tab_msg[256];
    snprintf(tab_msg, sizeof(tab_msg), "T%d:%s", sess->id, sess->name);
    webview_host_send_to_window(target_hwnd, tab_msg);
    webview_host_send_session_text_to_window(target_hwnd, '2', sess->id,
        session_is_connected(sess) ? "connected" : "disconnected");

    char log_msg[MAX_PATH + 32];
    if (sess->log_enabled) {
        snprintf(log_msg, sizeof(log_msg), "L%d:1:%s", sess->id, sess->log_filename);
    } else {
        snprintf(log_msg, sizeof(log_msg), "L%d:0", sess->id);
    }
    webview_host_send_to_window(target_hwnd, log_msg);

    char auto_msg[32];
    snprintf(auto_msg, sizeof(auto_msg), "A%d:%d", sess->id, sess->auto_reconnect ? 1 : 0);
    webview_host_send_to_window(target_hwnd, auto_msg);

    /* Replay output history into the target window */
    session_replay_output(target_hwnd, sess);

    /* If old window has no tabs left, close old window */
    if (remaining_in_old == 0 && old_hwnd) {
        DestroyWindow(old_hwnd);
    }
}

static void session_merge_all_to_window(HWND target_hwnd)
{
    if (!target_hwnd) return;
    WebViewSession *s = sessions_head;
    while (s) {
        WebViewSession *next = s->next;
        if (s->hwnd && s->hwnd != target_hwnd) {
            session_attach_to_window(target_hwnd, s->id);
        }
        s = next;
    }
}

static void session_clone(HWND target_hwnd, int src_id)
{
    WebViewSession *src = session_find(src_id);
    if (!src || !src->cfg)
        return;

    char clone_title[160];
    snprintf(clone_title, sizeof(clone_title), "%s (copy)", src->name);
    session_create(target_hwnd, src->cfg, clone_title);
}

static void session_new_via_dialog(HWND target_hwnd)
{
    Conf *new_cfg = conf_new();
    do_defaults(NULL, new_cfg);
    if (do_config(new_cfg)) {
        session_create(target_hwnd, new_cfg, NULL);
    }
    conf_free(new_cfg);
}

void cmdline_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = dupvprintf(fmt, ap);
    va_end(ap);
    MessageBoxA(NULL, msg, "PuTTY-WebView Command Line Error", MB_OK | MB_ICONERROR);
    sfree(msg);
    exit(1);
}

void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = dupvprintf(fmt, ap);
    va_end(ap);

    dbg_log("modalfatalbox: %s", msg);

    if (sessions_head) {
        char banner[512];
        snprintf(banner, sizeof(banner), "\r\n\x1b[1;31m[Fatal Error: %s]\x1b[0m\r\n", msg);
        for (WebViewSession *s = sessions_head; s; s = s->next) {
            session_record_output(s, banner, strlen(banner));
            if (s->hwnd) {
                webview_host_send_session_binary_to_window(s->hwnd, '0', s->id, banner, strlen(banner));
                webview_host_send_session_text_to_window(s->hwnd, '2', s->id, "disconnected");
            }
        }
    } else {
        MessageBoxA(NULL, msg, "PuTTY-WebView Fatal Error", MB_ICONERROR | MB_OK);
        cleanup_exit(1);
    }
    sfree(msg);
}

void nonfatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = dupvprintf(fmt, ap);
    va_end(ap);

    dbg_log("nonfatal: %s", msg);

    if (sessions_head) {
        char banner[512];
        snprintf(banner, sizeof(banner), "\r\n\x1b[1;33m[Warning: %s]\x1b[0m\r\n", msg);
        for (WebViewSession *s = sessions_head; s; s = s->next) {
            session_record_output(s, banner, strlen(banner));
            if (s->hwnd) {
                webview_host_send_session_binary_to_window(s->hwnd, '0', s->id, banner, strlen(banner));
            }
        }
    }
    sfree(msg);
}

void cleanup_exit(int code)
{
    dbg_log("cleanup_exit called with code: %d", code);
    while (sessions_head) {
        session_close(sessions_head->id);
    }
    sk_cleanup();
    random_save_seed();
    shutdown_help();
    CoUninitialize();
    exit(code);
}

/* Callback receiving messages from Web frontend */
static void on_web_message(HWND hwnd, const char *msg, void *userdata)
{
    if (!msg || !*msg) return;

    char type = msg[0];
    const char *payload = msg + 1;

    if (type == 'R') {
        /* Web frontend ready in window `hwnd`: flush sessions hosted in `hwnd` */
        for (WebViewSession *s = sessions_head; s; s = s->next) {
            if (s->hwnd == hwnd) {
                char tab_msg[256];
                snprintf(tab_msg, sizeof(tab_msg), "T%d:%s", s->id, s->name);
                webview_host_send_to_window(hwnd, tab_msg);
                webview_host_send_session_text_to_window(hwnd, '2', s->id,
                    session_is_connected(s) ? "connected" : "disconnected");
                char log_msg[MAX_PATH + 32];
                if (s->log_enabled) {
                    snprintf(log_msg, sizeof(log_msg), "L%d:1:%s", s->id, s->log_filename);
                } else {
                    snprintf(log_msg, sizeof(log_msg), "L%d:0", s->id);
                }
                webview_host_send_to_window(hwnd, log_msg);
                char auto_msg[32];
                snprintf(auto_msg, sizeof(auto_msg), "A%d:%d", s->id, s->auto_reconnect ? 1 : 0);
                webview_host_send_to_window(hwnd, auto_msg);
                session_replay_output(hwnd, s);
            }
        }
        return;
    }

    if (type == '0') {
        /* User keystroke input: 0{session_id}:{data} */
        const char *colon = strchr(payload, ':');
        if (colon) {
            int sess_id = atoi(payload);
            const char *data = colon + 1;
            WebViewSession *sess = session_find(sess_id);
            if (sess) {
                if (sess->cur_prompts) {
                    session_handle_prompt_input(sess, data, strlen(data));
                } else {
                    session_send(sess, data, strlen(data));
                }
            }
        }
    } else if (type == '1') {
        /* Terminal resize: 1{session_id}:{"cols":X,"rows":Y} */
        const char *colon = strchr(payload, ':');
        if (colon) {
            int sess_id = atoi(payload);
            int cols = 80, rows = 24;
            if (sscanf(colon + 1, "{\"cols\":%d,\"rows\":%d}", &cols, &rows) == 2 ||
                sscanf(colon + 1, "{\"columns\":%d,\"rows\":%d}", &cols, &rows) == 2) {
                WebViewSession *sess = session_find(sess_id);
                if (sess && sess->backend && backend_connected(sess->backend)) {
                    backend_size(sess->backend, cols, rows);
                }
            }
        }
    } else if (type == '3') {
        dbg_log("handle_webview_message type 3: hwnd=%p payload='%s'", (void*)hwnd, payload);
        if (!strcmp(payload, "new_tab")) {
            session_new_via_dialog(hwnd);
        } else if (!strcmp(payload, "strip_log_ansi")) {
            strip_log_ansi_via_dialog(hwnd);
        } else if (!strcmp(payload, "fix_ssh_key_perm")) {
            fix_ssh_key_perm_via_dialog(hwnd);
        } else if (strstr(payload, ":toggle_log")) {
            int sess_id = atoi(payload);
            WebViewSession *sess = session_find(sess_id);
            if (sess) {
                session_toggle_log(sess);
            }
        } else if (strstr(payload, ":toggle_auto_reconnect")) {
            int sess_id = atoi(payload);
            WebViewSession *sess = session_find(sess_id);
            if (sess) {
                session_toggle_auto_reconnect(sess);
            }
        } else if (strstr(payload, ":clone")) {
            int sess_id = atoi(payload);
            session_clone(hwnd, sess_id);
        } else if (strstr(payload, ":open_editor")) {
            int sess_id = atoi(payload);
            editor_window_open(sess_id, NULL, 0);
        } else if (strstr(payload, ":detach")) {
            int sess_id = 0, screen_x = 0, screen_y = 0;
            if (sscanf(payload, "%d:detach:%d:%d", &sess_id, &screen_x, &screen_y) >= 1) {
                session_detach_to_new_window(sess_id, screen_x, screen_y);
            }
        } else if (strstr(payload, ":attach")) {
            int sess_id = atoi(payload);
            session_attach_to_window(hwnd, sess_id);
        } else if (!strcmp(payload, "merge_all")) {
            session_merge_all_to_window(hwnd);
        } else if (strstr(payload, ":close")) {
            int sess_id = atoi(payload);
            session_close(sess_id);
        } else if (strstr(payload, ":paste")) {
            int sess_id = atoi(payload);
            paste_to_session(hwnd, sess_id);
        } else if (strstr(payload, ":reconfig")) {
            int sess_id = atoi(payload);
            WebViewSession *sess = session_find(sess_id);
            if (sess && do_reconfig(hwnd, sess->cfg, sess->backend ? backend_cfg_info(sess->backend) : 0)) {
                int old_limit = sess->send_rate_limit;
                int new_limit = conf_get_int(sess->cfg, CONF_send_rate_limit);
                if (new_limit < 0) new_limit = 0;
                sess->send_rate_limit = new_limit;
                if (old_limit != new_limit) {
                    expire_timer_context(sess);
                    sess->send_timer_active = false;
                    if (new_limit > 0)
                        sess->send_next_tick = GETTICKCOUNT() + session_send_interval_ticks(new_limit);
                    else
                        sess->send_next_tick = 0;
                    session_send_queue_try(sess);
                }
                if (sess->backend) {
                    backend_reconfig(sess->backend, sess->cfg);
                }
            }
        }
    } else if (type == '4') {
        /* Auto-copy selected text to native clipboard */
        copy_to_clipboard_utf8(hwnd, payload, strlen(payload));
    } else if (type == '5') {
        /* Open URL or local file / jump to line */
        int sess_id = 0;
        const char *link_payload = payload;
        const char *colon = strchr(payload, ':');
        if (colon && colon > payload) {
            bool all_digits = true;
            for (const char *q = payload; q < colon; q++) {
                if (*q < '0' || *q > '9') { all_digits = false; break; }
            }
            if (all_digits) {
                sess_id = atoi(payload);
                link_payload = colon + 1;
            }
        }
        open_url_or_file(hwnd, sess_id, link_payload);
    } else if (type == 'P') {
        /* WebView SSH Auth response: P{sess_id}:{json} or P{sess_id}:cancel */
        const char *colon = strchr(payload, ':');
        if (colon) {
            int sess_id = atoi(payload);
            const char *action = colon + 1;
            WebViewSession *sess = session_find(sess_id);
            if (sess && sess->cur_prompts) {
                prompts_t *p = sess->cur_prompts;
                if (!strcmp(action, "cancel")) {
                    sess->cur_prompts = NULL;
                    p->spr = SPR_USER_ABORT;
                    if (p->callback) {
                        queue_toplevel_callback(p->callback, p->callback_ctx);
                    }
                    session_write_terminal(sess, "\r\n\x1b[1;31m[用户取消了身份验证]\x1b[0m\r\n");
                } else if (action[0] == '{') {
                    char user[128] = {0};
                    char pass[256] = {0};
                    bool remember = false;
                    bool autologin = false;
                    parse_auth_json(action, user, sizeof(user), pass, sizeof(pass), &remember, &autologin);

                    strncpy(sess->temp_prompt_user, user, sizeof(sess->temp_prompt_user) - 1);
                    sess->temp_prompt_user[sizeof(sess->temp_prompt_user) - 1] = '\0';
                    if (user[0] != '\0' && sess->cfg) {
                        conf_set_str(sess->cfg, CONF_username, sess->temp_prompt_user);
                    }
                    strncpy(sess->temp_prompt_pass, pass, sizeof(sess->temp_prompt_pass) - 1);
                    sess->temp_prompt_pass[sizeof(sess->temp_prompt_pass) - 1] = '\0';
                    sess->modal_remember = remember;
                    sess->modal_autologin = autologin;
                    sess->prompt_attempts++;

                    for (size_t i = 0; i < p->n_prompts; i++) {
                        prompt_t *pr = p->prompts[i];
                        if (pr->echo) {
                            const char *u = (user[0] != '\0') ? user : conf_get_str_ambi(sess->cfg, CONF_username, NULL);
                            prompt_set_result(pr, u ? u : "");
                            if (!sess->username_displayed && u && *u) {
                                session_write_terminal(sess, "login as: ");
                                session_write_terminal(sess, u);
                                session_write_terminal(sess, "\r\n");
                                sess->username_displayed = true;
                            }
                        } else {
                            prompt_set_result(pr, pass);
                        }
                    }

                    sess->cur_prompts = NULL;
                    p->spr = SPR_OK;
                    if (p->callback) {
                        queue_toplevel_callback(p->callback, p->callback_ctx);
                    }
                }
            }
        }
    }
}

/* Window procedure */
static LRESULT CALLBACK WebViewWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_WEBVIEW_POST_MSG: {
        char *str = (char *)lParam;
        if (str) {
            webview_host_send_to_window(hwnd, str);
            free(str);
        }
        return 0;
    }
    case WM_NETEVENT:
    case WM_DONE_WITH_SOCKET:
        winselgui_response(msg, wParam, lParam);
        return 0;
    case WM_SIZE:
        webview_host_resize(hwnd);
        return 0;
    case WM_SETFOCUS:
        webview_host_focus(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE) {
            webview_host_focus(hwnd);
        }
        return 0;
    case WM_DESTROY: {
        bool is_ui_window = false;
        WebViewWindow **wp = &windows_head;
        while (*wp) {
            if ((*wp)->hwnd == hwnd) {
                WebViewWindow *to_free = *wp;
                *wp = (*wp)->next;
                sfree(to_free);
                is_ui_window = true;
                break;
            }
            wp = &(*wp)->next;
        }

        if (is_ui_window) {
            webview_host_close(hwnd);

            WebViewSession *s = sessions_head;
            while (s) {
                WebViewSession *next = s->next;
                if (s->hwnd == hwnd) {
                    session_close(s->id);
                }
                s = next;
            }

            if (!windows_head) {
                PostQuitMessage(0);
            }
        }
        return 0;
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    MSG msg;
    HRESULT hr;

    /* Do NOT call dll_hijacking_protection() as WebView2 Runtime
     * needs to load components from Edge/WebView2 installation directory */

    hinst = inst;

    sk_init();
    init_common_controls();
    init_winver();
    init_help();
    setup_gui_timing();

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        MessageBoxA(NULL, "Failed to initialize COM subsystem.",
                    appname, MB_OK | MB_ICONERROR);
        return 1;
    }

    dbg_log("WinMain started. cmdline: '%s'", cmdline ? cmdline : "");

    Conf *cfg = conf_new();
    do_defaults(NULL, cfg);

    bool is_conpty = false;
    char conpty_cmd[512] = {0};

    /* Process command line if provided */
    char *p = handle_restrict_acl_cmdline_prefix(cmdline);
    if (handle_special_sessionname_cmdline(p, cfg)) {
        dbg_log("Loaded special sessionname from prefix '@'");
    } else if (*p) {
        CmdlineArgList *arglist = cmdline_arg_list_from_GetCommandLineW();
        size_t arglistpos = 0;
        while (arglist->args[arglistpos]) {
            CmdlineArg *arg = arglist->args[arglistpos++];
            const char *argstr = cmdline_arg_to_str(arg);
            if (!strcmp(argstr, "-wsl")) {
                is_conpty = true;
                CmdlineArg *nextarg = arglist->args[arglistpos];
                if (nextarg && !strcmp(cmdline_arg_to_str(nextarg), "-d")) {
                    arglistpos++; // consume -d
                    CmdlineArg *distroarg = arglist->args[arglistpos];
                    if (distroarg) {
                        arglistpos++; // consume distro name
                        snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe -d %s ~", cmdline_arg_to_str(distroarg));
                    } else {
                        snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe ~");
                    }
                } else {
                    snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe ~");
                }
                conf_set_str(cfg, CONF_host, "WSL");
                conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
                continue;
            } else if (!strcmp(argstr, "-local")) {
                is_conpty = true;
                CmdlineArg *nextarg = arglist->args[arglistpos];
                if (nextarg) {
                    arglistpos++;
                    snprintf(conpty_cmd, sizeof(conpty_cmd), "%s", cmdline_arg_to_str(nextarg));
                } else {
                    snprintf(conpty_cmd, sizeof(conpty_cmd), "cmd.exe");
                }
                conf_set_str(cfg, CONF_host, "Local");
                conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
                continue;
            }

            CmdlineArg *nextarg = arglist->args[arglistpos];
            int ret = cmdline_process_param(arg, nextarg, 1, cfg);
            if (ret == -2) {
                cmdline_error("option \"%s\" requires an argument", argstr);
            } else if (ret == 2) {
                arglistpos++;
            }
        }
    }

    cmdline_run_saved(cfg);

    /* Check if host is wsl or local */
    const char *cur_h = conf_get_str(cfg, CONF_host);
    if (!is_conpty && cur_h && *cur_h) {
        if (!stricmp(cur_h, "wsl")) {
            is_conpty = true;
            snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe ~");
            conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
        } else if (!strnicmp(cur_h, "wsl:", 4)) {
            is_conpty = true;
            snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe -d %s ~", cur_h + 4);
            conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
        } else if (!strnicmp(cur_h, "local:", 6)) {
            is_conpty = true;
            snprintf(conpty_cmd, sizeof(conpty_cmd), "%s", cur_h + 6);
            conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
        }
    }

    dbg_log("cmdline_run_saved done. conf_launchable: %d, host: '%s', is_conpty: %d",
            conf_launchable(cfg), conf_get_str(cfg, CONF_host), is_conpty);

    /* If not launchable directly via cmdline and not conpty, pop up native config box */
    if (!is_conpty && !conf_launchable(cfg)) {
        dbg_log("Session not launchable, showing do_config()...");
        if (!do_config(cfg)) {
            dbg_log("User cancelled do_config()");
            cleanup_exit(0);
        }
        dbg_log("do_config() accepted. host: '%s'", conf_get_str(cfg, CONF_host));

        cur_h = conf_get_str(cfg, CONF_host);
        if (cur_h && *cur_h) {
            if (!stricmp(cur_h, "wsl")) {
                is_conpty = true;
                snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe ~");
                conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
            } else if (!strnicmp(cur_h, "wsl:", 4)) {
                is_conpty = true;
                snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe -d %s ~", cur_h + 4);
                conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
            } else if (!strnicmp(cur_h, "local:", 6)) {
                is_conpty = true;
                snprintf(conpty_cmd, sizeof(conpty_cmd), "%s", cur_h + 6);
                conf_set_str(cfg, CONF_remote_cmd, conpty_cmd);
            }
        }
    }

    if (is_conpty) {
        conf_set_int(cfg, CONF_protocol, PROT_CONPTY);
    } else {
        prepare_session(cfg);
        dbg_log("prepare_session done. host: '%s', port: %d",
                conf_get_str(cfg, CONF_host), conf_get_int(cfg, CONF_port));
    }

    /* Register Host Window Class */
    WNDCLASSEX wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WebViewWndProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "PuTTYWebViewHostClass";
    RegisterClassEx(&wc);

    /* Register Editor Host Window Class */
    WNDCLASSEX wc_ed;
    memset(&wc_ed, 0, sizeof(wc_ed));
    wc_ed.cbSize = sizeof(wc_ed);
    wc_ed.style = CS_HREDRAW | CS_VREDRAW;
    wc_ed.lpfnWndProc = WebViewEditorWndProc;
    wc_ed.hInstance = hinst;
    wc_ed.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON));
    wc_ed.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc_ed.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc_ed.lpszClassName = "PuTTYWebViewEditorClass";
    RegisterClassEx(&wc_ed);

    char title[128];
    const char *h = conf_get_str(cfg, CONF_host);
    int proto = conf_get_int(cfg, CONF_protocol);
    if (proto == PROT_CONPTY) {
        is_conpty = true;
        if (!conpty_cmd[0]) {
            if (h && *h && stricmp(h, "wsl") && stricmp(h, "localhost") && stricmp(h, "127.0.0.1")) {
                snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe -d %s ~", h);
            } else {
                snprintf(conpty_cmd, sizeof(conpty_cmd), "wsl.exe ~");
            }
        }
    }

    if (is_conpty) {
        snprintf(title, sizeof(title), "%s - %s", appname, conpty_cmd);
    } else if (proto == PROT_SERIAL) {
        snprintf(title, sizeof(title), "%s - %s (%d baud)",
                 appname, conf_get_str(cfg, CONF_serline),
                 conf_get_int(cfg, CONF_serspeed));
    } else {
        snprintf(title, sizeof(title), "%s - %s:%d",
                 appname, (h && *h) ? h : "Session",
                 conf_get_int(cfg, CONF_port));
    }

    /* Ensure web assets: load existing immediately or extract if missing */
    ensure_webview_assets(global_html_path, global_editor_html_path, MAX_PATH);

    /* Create hidden message-only window for network socket events */
    HWND sock_hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        "PuTTYWebViewNetSink",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, hinst, NULL
    );
    winselgui_set_hwnd(sock_hwnd);

    /* Create initial UI window (centered, 60% screen area, screen aspect ratio) */
    int def_x, def_y, def_w, def_h;
    get_default_window_rect(&def_x, &def_y, &def_w, &def_h);
    HWND first_hwnd = create_webview_window(def_x, def_y, def_w, def_h);
    if (!first_hwnd) {
        MessageBoxA(NULL, "Failed to create host window.", appname, MB_OK | MB_ICONERROR);
        cleanup_exit(1);
    }
    SetWindowTextA(first_hwnd, title);

    /* Create initial session */
    session_create(first_hwnd, cfg, title);
    conf_free(cfg);

    dbg_log("Entering message pump loop...");

    /* High-performance non-blocking message & network event loop */
    while (1) {
        int n;
        DWORD timeout;

        if (toplevel_callback_pending() ||
            PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE)) {
            timeout = 0;
        } else {
            timeout = 20; /* 20ms tick guarantee to prevent any event stalling */
            for (WebViewSession *s = sessions_head; s; s = s->next) {
                if (bufchain_size(&s->send_queue) > 0) {
                    timeout = 1;
                    break;
                }
            }
        }

        HandleWaitList *hwl = get_handle_wait_list();
        n = MsgWaitForMultipleObjects(hwl->nhandles, hwl->handles, FALSE,
                                      timeout, QS_ALLINPUT);

        if ((unsigned)(n - WAIT_OBJECT_0) < (unsigned)hwl->nhandles) {
            handle_wait_activate(hwl, n - WAIT_OBJECT_0);
        }
        handle_wait_list_free(hwl);

        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto finished;

            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (msg.message != WM_NETEVENT)
                break;
        }

        for (WebViewSession *s = sessions_head; s; s = s->next) {
            if (bufchain_size(&s->send_queue) > 0) {
                session_send_queue_try(s);
            }
        }

        run_toplevel_callbacks();

        /* Process PuTTY timer queue */
        unsigned long next_timer;
        run_timers(GETTICKCOUNT(), &next_timer);

        /* Guarantee auto-reconnect trigger and dynamic countdown */
        for (WebViewSession *s = sessions_head; s; s = s->next) {
            if (s->auto_reconnect && s->reconnect_timer_active) {
                unsigned long now_tick = GETTICKCOUNT();
                if ((long)(now_tick - s->reconnect_target_tick) >= 0) {
                    s->reconnect_timer_active = false;
                    expire_timer_context(&s->reconnect_timer_active);
                    session_reconnect(s);
                } else if (s->reconnect_countdown > 1 && (long)(now_tick - s->reconnect_next_tick) >= 0) {
                    s->reconnect_countdown--;
                    s->reconnect_next_tick += 1 * TICKSPERSEC;
                    char cd_buf[64];
                    snprintf(cd_buf, sizeof(cd_buf), "\r\x1b[2K\x1b[1;33m[自动重连] %d 秒后尝试重新连接...\x1b[0m", s->reconnect_countdown);
                    session_write_terminal(s, cd_buf);
                }
            }
        }
    }

finished:
    cleanup_exit((int)msg.wParam);
    return 0;
}
