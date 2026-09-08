/*
 * putty_webkit.c - Modern Web-based frontend host for PuTTY / Plink core.
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
#include "webkit/webview_host.h"

/* appname is generated in be_list.c by be_list() macro */

const unsigned cmdline_tooltype =
    TOOLTYPE_HOST_ARG |
    TOOLTYPE_PORT_ARG |
    TOOLTYPE_NO_VERBOSE_OPTION;

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = true;

HINSTANCE hinst;

typedef struct WebKitWindow {
    HWND hwnd;
    struct WebKitWindow *next;
} WebKitWindow;

static WebKitWindow *windows_head = NULL;
static wchar_t global_html_path[MAX_PATH] = {0};
static wchar_t target_web_dir[MAX_PATH] = {0};

static void dbg_log(const char *fmt, ...)
{
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    char logpath[MAX_PATH];
    snprintf(logpath, sizeof(logpath), "%sputty_webkit_debug.log", temp);

    FILE *f = fopen(logpath, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

#define IDR_WEB_INDEX_HTML          2001
#define IDR_WEB_XTERM_JS            2002
#define IDR_WEB_XTERM_CSS           2003
#define IDR_WEB_FIT_ADDON_JS        2004
#define IDR_WEB_WEBLINKS_ADDON_JS   2005

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

static void ensure_webkit_assets(wchar_t *out_html_path, size_t max_len)
{
    wchar_t exe_path[MAX_PATH];
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    wchar_t *last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash) *last_slash = L'\0';

    /* 1. Check if webkit/web/index.html already exists next to exe */
    _snwprintf(target_web_dir, MAX_PATH, L"%s\\webkit\\web", exe_path);
    _snwprintf(out_html_path, max_len, L"%s\\index.html", target_web_dir);

    if (GetFileAttributesW(out_html_path) != INVALID_FILE_ATTRIBUTES) {
        /* Already exists on disk! Immediate 0ms load */
        return;
    }

    /* 2. Check development tree fallback (source checkout) */
    wchar_t dev_html[MAX_PATH];
    _snwprintf(dev_html, MAX_PATH, L"%s\\..\\..\\windows\\webkit\\web\\index.html", exe_path);
    if (GetFileAttributesW(dev_html) != INVALID_FILE_ATTRIBUTES) {
        wcsncpy(out_html_path, dev_html, max_len - 1);
        out_html_path[max_len - 1] = L'\0';
        return;
    }

    /* 3. Not found on disk! Create webkit/web directory next to exe */
    bool dir_ok = create_directory_recursive(target_web_dir);
    if (!dir_ok) {
        /* If exe directory is read-only (e.g. Program Files), fall back to %LOCALAPPDATA% */
        wchar_t appdata[MAX_PATH];
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", appdata, MAX_PATH) > 0) {
            _snwprintf(target_web_dir, MAX_PATH, L"%s\\PuTTY-WebKit\\webkit\\web", appdata);
            create_directory_recursive(target_web_dir);
            _snwprintf(out_html_path, max_len, L"%s\\index.html", target_web_dir);
        }
    }

    /* Extract embedded assets synchronously (340KB total, takes ~2ms) */
    for (size_t i = 0; i < NUM_EMBEDDED_ASSETS; i++) {
        wchar_t filepath[MAX_PATH];
        _snwprintf(filepath, MAX_PATH, L"%s\\%s", target_web_dir, embedded_assets[i].filename);
        extract_resource_to_file(embedded_assets[i].res_id, filepath);
    }
}

static DWORD WINAPI background_asset_sync_thread(LPVOID param)
{
    const wchar_t *web_dir = (const wchar_t *)param;
    if (!web_dir || !*web_dir) return 0;

    /* Run with low priority so UI rendering and user interaction stay completely unaffected */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    /* Polite initial delay to give WebView2 full priority during startup */
    Sleep(300);

    for (size_t i = 0; i < NUM_EMBEDDED_ASSETS; i++) {
        HRSRC hrsrc = FindResourceW(hinst, MAKEINTRESOURCEW(embedded_assets[i].res_id), MAKEINTRESOURCEW(10));
        if (!hrsrc) continue;
        HGLOBAL hg = LoadResource(hinst, hrsrc);
        if (!hg) continue;
        const void *res_data = LockResource(hg);
        DWORD res_size = SizeofResource(hinst, hrsrc);
        if (!res_data || res_size == 0) continue;

        wchar_t filepath[MAX_PATH];
        _snwprintf(filepath, MAX_PATH, L"%s\\%s", web_dir, embedded_assets[i].filename);

        bool needs_update = false;

        HANDLE hf = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            needs_update = true;
        } else {
            LARGE_INTEGER fsize;
            if (!GetFileSizeEx(hf, &fsize) || (DWORD)fsize.QuadPart != res_size) {
                needs_update = true;
            } else {
                char *buf = (char *)smalloc(res_size);
                DWORD read_bytes = 0;
                if (ReadFile(hf, buf, res_size, &read_bytes, NULL) && read_bytes == res_size) {
                    if (memcmp(buf, res_data, res_size) != 0) {
                        needs_update = true;
                    }
                } else {
                    needs_update = true;
                }
                sfree(buf);
            }
            CloseHandle(hf);
        }

        if (needs_update) {
            dbg_log("Background asset sync: updating %ls (%u bytes)", embedded_assets[i].filename, res_size);
            extract_resource_to_file(embedded_assets[i].res_id, filepath);
        }

        Sleep(20); /* Yield CPU politely between files */
    }

    return 0;
}

#define REPLAY_BUF_SIZE (64 * 1024)

typedef struct WebKitSession {
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
    bool auto_reconnect;
    bool reconnect_timer_active;
    int reconnect_countdown;
    unsigned long reconnect_next_tick;
    unsigned long reconnect_target_tick;
    struct WebKitSession *next;
} WebKitSession;

static WebKitSession *sessions_head = NULL;
static int next_session_id = 1;

static bool session_is_connected(WebKitSession *sess)
{
    if (!sess) return false;
    if (!sess->is_connected) return false;
    if (!sess->backend) return false;
    return backend_connected(sess->backend);
}

static WebKitSession *session_from_seat(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return container_of(wgs, WebKitSession, wgs);
}

static WebKitSession *session_find(int id)
{
    for (WebKitSession *s = sessions_head; s; s = s->next) {
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

static void session_send_queue_try(WebKitSession *sess);

static void session_send_queue_timer(void *ctx, unsigned long now)
{
    WebKitSession *sess = (WebKitSession *)ctx;
    sess->send_timer_active = false;
    session_send_queue_try(sess);
}

static void session_send_queue_arm(WebKitSession *sess, unsigned long now)
{
    if (sess->send_timer_active)
        return;

    unsigned long wait = session_send_tick_due(now, sess->send_next_tick) ?
        1 : sess->send_next_tick - now;
    sess->send_timer_active = true;
    schedule_timer(wait, session_send_queue_timer, sess);
}

static void session_send_queue_try(WebKitSession *sess)
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

static void session_send(WebKitSession *sess, const void *data, size_t len)
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

static void session_record_output(WebKitSession *sess, const void *data, size_t len)
{
    const char *src = (const char *)data;
    for (size_t i = 0; i < len; i++) {
        sess->replay_buf[sess->replay_head] = src[i];
        sess->replay_head = (sess->replay_head + 1) % REPLAY_BUF_SIZE;
        if (sess->replay_len < REPLAY_BUF_SIZE)
            sess->replay_len++;
    }
}

static void session_replay_output(HWND target_hwnd, WebKitSession *sess)
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
static LRESULT CALLBACK WebKitWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static void on_web_message(HWND hwnd, const char *msg, void *userdata);
static HWND create_webkit_window(int x, int y, int width, int height);
static size_t webkit_seat_output(Seat *seat, SeatOutputType type,
                                 const void *data, size_t len);
static bool webkit_seat_eof(Seat *seat);
static size_t webkit_seat_banner(Seat *seat, const void *data, size_t len);
static SeatPromptResult webkit_seat_get_userpass_input(Seat *seat, prompts_t *p);
static void webkit_seat_notify_session_started(Seat *seat);
static void webkit_seat_notify_remote_exit(Seat *seat);
static void webkit_seat_notify_remote_disconnect(Seat *seat);
static void webkit_seat_connection_fatal(Seat *seat, const char *msg);

static const SeatVtable webkit_seat_vt = {
    .output = webkit_seat_output,
    .eof = webkit_seat_eof,
    .sent = nullseat_sent,
    .banner = webkit_seat_banner,
    .get_userpass_input = webkit_seat_get_userpass_input,
    .notify_session_started = webkit_seat_notify_session_started,
    .notify_remote_exit = webkit_seat_notify_remote_exit,
    .notify_remote_disconnect = webkit_seat_notify_remote_disconnect,
    .connection_fatal = webkit_seat_connection_fatal,
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

static void session_stop_log(WebKitSession *sess)
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

static bool session_start_log(WebKitSession *sess)
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
            _snwprintf(log_dir, MAX_PATH, L"%s\\PuTTY-WebKit\\logs", appdata);
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

    fprintf(fp, "=~=~=~=~=~=~=~=~=~=~=~= PuTTY-WebKit log %04d.%02d.%02d %02d:%02d:%02d =~=~=~=~=~=~=~=~=~=~=\r\n",
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

static void session_toggle_log(WebKitSession *sess)
{
    if (!sess) return;
    if (sess->log_enabled) {
        session_stop_log(sess);
    } else {
        session_start_log(sess);
    }
}

static void session_write_terminal(WebKitSession *sess, const char *text)
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

static void session_reconnect(WebKitSession *sess);
static void session_schedule_reconnect(WebKitSession *sess);

static void session_reconnect_timer(void *ctx, unsigned long now)
{
    WebKitSession *sess = container_of((bool *)ctx, WebKitSession, reconnect_timer_active);
    if (!sess->reconnect_timer_active)
        return;
    sess->reconnect_timer_active = false;
    if (!sess->auto_reconnect)
        return;
    session_reconnect(sess);
}

static void session_schedule_reconnect(WebKitSession *sess)
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

    session_write_terminal(sess, "\r\n\x1b[1;33m[自动重连] 5 秒后尝试重新连接...\x1b[0m");
    schedule_timer(5 * TICKSPERSEC, session_reconnect_timer, &sess->reconnect_timer_active);
}

static void session_reconnect(WebKitSession *sess)
{
    if (!sess) return;
    if (sess->backend && backend_connected(sess->backend))
    if (!sess->auto_reconnect)
    if (!sess || !sess->auto_reconnect)
        return;
    if (session_is_connected(sess))
        return;

    session_write_terminal(sess, "\r\x1b[2K\x1b[1;36m[自动重连] 正在尝试连接...\x1b[0m\r\n");

    if (sess->backend) {
        backend_free(sess->backend);
        sess->backend = NULL;
        sess->wgs.backend = NULL;
    }

    bufchain_clear(&sess->send_queue);
    sess->send_timer_active = false;

    sess->wgs.cmdline_get_passwd_state = cmdline_get_passwd_input_state_new;
    seat_set_trust_status(&sess->wgs.seat, true);

    int proto = conf_get_int(sess->cfg, CONF_protocol);
    const struct BackendVtable *vt = backend_vt_from_proto(proto);
    if (!vt) {
        session_write_terminal(sess, "\r\n\x1b[1;31m[自动重连失败: 不支持的协议]\x1b[0m\r\n");
        return;
    }

    session_write_terminal(sess, "\x1b[1;36m[自动重连] 正在尝试连接...\x1b[0m\r\n");

    char *realhost = NULL;
    char *err = backend_init(vt, &sess->wgs.seat, &sess->backend, sess->logctx, sess->cfg,
                             conf_get_str(sess->cfg, CONF_host),
                             conf_get_int(sess->cfg, CONF_port),
                             &realhost,
                             conf_get_bool(sess->cfg, CONF_tcp_nodelay),
                             conf_get_bool(sess->cfg, CONF_tcp_keepalives));
    sfree(realhost);

    if (err) {
        char banner[512];
        snprintf(banner, sizeof(banner), "\x1b[1;31m[连接失败: %s]\x1b[0m\r\n", err);
        session_write_terminal(sess, banner);
        sfree(err);
        if (sess->backend) {
            backend_free(sess->backend);
            sess->backend = NULL;
            sess->wgs.backend = NULL;
        }
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
        }
        if (sess->hwnd) {
            webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "connected");
        }
    }
}

static void session_toggle_auto_reconnect(WebKitSession *sess)
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
static size_t webkit_seat_output(Seat *seat, SeatOutputType type,
                                 const void *data, size_t len)
{
    if (len > 0) {
        WebKitSession *sess = session_from_seat(seat);
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

static bool webkit_seat_eof(Seat *seat)
{
    return false;
}

static size_t webkit_seat_banner(Seat *seat, const void *data, size_t len)
{
    return webkit_seat_output(seat, SEAT_OUTPUT_STDOUT, data, len);
}

static SeatPromptResult webkit_seat_get_userpass_input(Seat *seat, prompts_t *p)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return cmdline_get_passwd_input(p, &wgs->cmdline_get_passwd_state, false);
}

static void webkit_seat_notify_session_started(Seat *seat)
{
    WebKitSession *sess = session_from_seat(seat);
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

static void webkit_seat_notify_remote_exit(Seat *seat)
{
    WebKitSession *sess = session_from_seat(seat);
    if (sess->log_fp) {
        fclose(sess->log_fp);
        sess->log_fp = NULL;
        sess->is_logging = false;
    }
    expire_timer_context(sess);
    bufchain_clear(&sess->send_queue);
    sess->send_timer_active = false;
    if (sess->backend) {
        backend_free(sess->backend);
        sess->backend = NULL;
        sess->wgs.backend = NULL;
    }
    sess->is_connected = false;
    if (sess->hwnd) {
        webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "disconnected");
        webview_host_send_session_binary_to_window(sess->hwnd, '0', sess->id, "\r\n\x1b[31m[Connection closed by remote host]\x1b[0m\r\n", 48);
    }
    if (sess->auto_reconnect) {
        session_schedule_reconnect(sess);
    }
}

static void webkit_seat_notify_remote_disconnect(Seat *seat)
{
    WebKitSession *sess = session_from_seat(seat);
    if (sess->log_fp) {
        fclose(sess->log_fp);
        sess->log_fp = NULL;
        sess->is_logging = false;
    }
    expire_timer_context(sess);
    bufchain_clear(&sess->send_queue);
    sess->send_timer_active = false;
    if (sess->backend) {
        backend_free(sess->backend);
        sess->backend = NULL;
        sess->wgs.backend = NULL;
    }
    sess->is_connected = false;
    if (sess->hwnd) {
        webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "disconnected");
        webview_host_send_session_binary_to_window(sess->hwnd, '0', sess->id, "\r\n\x1b[1;31m[Connection disconnected]\x1b[0m\r\n", 39);
    }
    if (sess->auto_reconnect) {
        session_schedule_reconnect(sess);
    }
}

static void webkit_seat_connection_fatal(Seat *seat, const char *msg)
{
    WebKitSession *sess = session_from_seat(seat);
    if (sess->log_fp) {
        fclose(sess->log_fp);
        sess->log_fp = NULL;
        sess->is_logging = false;
    }
    expire_timer_context(sess);
    bufchain_clear(&sess->send_queue);
    sess->send_timer_active = false;
    if (sess->backend) {
        backend_free(sess->backend);
        sess->backend = NULL;
        sess->wgs.backend = NULL;
    }
    sess->is_connected = false;
    if (sess->hwnd) {
        webview_host_send_session_text_to_window(sess->hwnd, '2', sess->id, "disconnected");
        char banner[512];
        snprintf(banner, sizeof(banner), "\r\n\x1b[1;31m[Fatal Error: %s]\x1b[0m\r\n", msg ? msg : "Connection failed");
        webview_host_send_session_binary_to_window(sess->hwnd, '0', sess->id, banner, strlen(banner));
    }
    if (sess->auto_reconnect) {
        session_schedule_reconnect(sess);
    }
}

/* System callbacks */
const wchar_t *get_app_user_model_id(void)
{
    return L"SimonTatham.PuTTYWebKit";
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
    WebKitSession *sess = session_find(session_id);
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

static WebKitSession *session_create(HWND target_hwnd, Conf *conf_to_use, const char *suggested_title)
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

    WebKitSession *sess = snew(WebKitSession);
    memset(sess, 0, sizeof(*sess));
    sess->id = next_session_id++;
    sess->cfg = conf_copy(conf_to_use);
    sess->hwnd = target_hwnd;

    memset(&sess->wgs, 0, sizeof(sess->wgs));
    sess->wgs.seat.vt = &webkit_seat_vt;
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

    char *realhost = NULL;
    char *err = backend_init(vt, &sess->wgs.seat, &sess->backend, sess->logctx, sess->cfg,
                             conf_get_str(sess->cfg, CONF_host),
                             conf_get_int(sess->cfg, CONF_port),
                             &realhost,
                             conf_get_bool(sess->cfg, CONF_tcp_nodelay),
                             conf_get_bool(sess->cfg, CONF_tcp_keepalives));
    sfree(realhost);

    /* Append to sessions list */
    sess->next = NULL;
    if (!sessions_head) {
        sessions_head = sess;
    } else {
        WebKitSession *cur = sessions_head;
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
        if (sess->backend) {
            backend_free(sess->backend);
            sess->backend = NULL;
            sess->wgs.backend = NULL;
        }
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
    WebKitSession **pp = &sessions_head;
    WebKitSession *target = NULL;
    while (*pp) {
        WebKitSession *s = *pp;
        if (s->id == id) {
            *pp = s->next;
            target = s;
            break;
        }
        pp = &(*pp)->next;
    }

    if (target) {
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
        HWND win_hwnd = target->hwnd;
        if (target->backend) {
            backend_free(target->backend);
            target->backend = NULL;
        }
        if (target->logctx) {
            log_free(target->logctx);
            target->logctx = NULL;
        }
        if (target->cfg) {
            conf_free(target->cfg);
            target->cfg = NULL;
        }
        sfree(target);

        if (win_hwnd) {
            char close_msg[32];
            snprintf(close_msg, sizeof(close_msg), "X%d", id);
            webview_host_send_to_window(win_hwnd, close_msg);

            /* Check if any remaining sessions in this window */
            bool window_has_session = false;
            for (WebKitSession *s = sessions_head; s; s = s->next) {
                if (s->hwnd == win_hwnd) {
                    window_has_session = true;
                    break;
                }
            }
            if (!window_has_session) {
                DestroyWindow(win_hwnd);
            }
        }
    }

    if (!sessions_head) {
        PostQuitMessage(0);
    }
}

static HWND create_webkit_window(int x, int y, int width, int height)
{
    HWND hwnd = CreateWindowEx(
        0,
        "PuTTYWebKitHostClass",
        appname,
        WS_OVERLAPPEDWINDOW,
        x, y, width, height,
        NULL, NULL, hinst, NULL
    );
    if (!hwnd) return NULL;

    WebKitWindow *win = snew(WebKitWindow);
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
    WebKitSession *sess = session_find(sess_id);
    if (!sess) return;
    HWND old_hwnd = sess->hwnd;

    int remaining_in_old = 0;
    for (WebKitSession *s = sessions_head; s; s = s->next) {
        if (s->hwnd == old_hwnd && s->id != sess_id)
            remaining_in_old++;
    }

    int x = (screen_x > 0) ? (screen_x - 100) : CW_USEDEFAULT;
    int y = (screen_y > 0) ? (screen_y - 20) : CW_USEDEFAULT;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    HWND new_hwnd = create_webkit_window(x, y, 960, 600);
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
    WebKitSession *sess = session_find(sess_id);
    if (!sess || !target_hwnd) return;
    HWND old_hwnd = sess->hwnd;
    if (old_hwnd == target_hwnd) return;

    int remaining_in_old = 0;
    for (WebKitSession *s = sessions_head; s; s = s->next) {
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
    WebKitSession *s = sessions_head;
    while (s) {
        WebKitSession *next = s->next;
        if (s->hwnd && s->hwnd != target_hwnd) {
            session_attach_to_window(target_hwnd, s->id);
        }
        s = next;
    }
}

static void session_clone(HWND target_hwnd, int src_id)
{
    WebKitSession *src = session_find(src_id);
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
    MessageBoxA(NULL, msg, "PuTTY-WebKit Command Line Error", MB_OK | MB_ICONERROR);
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
        for (WebKitSession *s = sessions_head; s; s = s->next) {
            session_record_output(s, banner, strlen(banner));
            if (s->hwnd) {
                webview_host_send_session_binary_to_window(s->hwnd, '0', s->id, banner, strlen(banner));
                webview_host_send_session_text_to_window(s->hwnd, '2', s->id, "disconnected");
            }
        }
    } else {
        MessageBoxA(NULL, msg, "PuTTY-WebKit Fatal Error", MB_ICONERROR | MB_OK);
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
        for (WebKitSession *s = sessions_head; s; s = s->next) {
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
        for (WebKitSession *s = sessions_head; s; s = s->next) {
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
            WebKitSession *sess = session_find(sess_id);
            if (sess) {
                session_send(sess, data, strlen(data));
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
                WebKitSession *sess = session_find(sess_id);
                if (sess && sess->backend && backend_connected(sess->backend)) {
                    backend_size(sess->backend, cols, rows);
                }
            }
        }
    } else if (type == '3') {
        if (!strcmp(payload, "new_tab")) {
            session_new_via_dialog(hwnd);
        } else if (!strcmp(payload, "strip_log_ansi")) {
            strip_log_ansi_via_dialog(hwnd);
        } else if (!strcmp(payload, "fix_ssh_key_perm")) {
            fix_ssh_key_perm_via_dialog(hwnd);
        } else if (strstr(payload, ":toggle_log")) {
            int sess_id = atoi(payload);
            WebKitSession *sess = session_find(sess_id);
            if (sess) {
                session_toggle_log(sess);
            }
        } else if (strstr(payload, ":toggle_auto_reconnect")) {
            int sess_id = atoi(payload);
            WebKitSession *sess = session_find(sess_id);
            if (sess) {
                session_toggle_auto_reconnect(sess);
            }
        } else if (strstr(payload, ":clone")) {
            int sess_id = atoi(payload);
            session_clone(hwnd, sess_id);
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
            WebKitSession *sess = session_find(sess_id);
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
    }
}

/* Window procedure */
static LRESULT CALLBACK WebKitWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
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
        WebKitWindow **wp = &windows_head;
        while (*wp) {
            if ((*wp)->hwnd == hwnd) {
                WebKitWindow *to_free = *wp;
                *wp = (*wp)->next;
                sfree(to_free);
                is_ui_window = true;
                break;
            }
            wp = &(*wp)->next;
        }

        if (is_ui_window) {
            webview_host_close(hwnd);

            WebKitSession *s = sessions_head;
            while (s) {
                WebKitSession *next = s->next;
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
    wc.lpfnWndProc = WebKitWndProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "PuTTYWebKitHostClass";
    RegisterClassEx(&wc);

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
    ensure_webkit_assets(global_html_path, MAX_PATH);

    /* Create hidden message-only window for network socket events */
    HWND sock_hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        "PuTTYWebKitNetSink",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, hinst, NULL
    );
    winselgui_set_hwnd(sock_hwnd);

    /* Create initial UI window */
    HWND first_hwnd = create_webkit_window(CW_USEDEFAULT, CW_USEDEFAULT, 960, 600);
    if (!first_hwnd) {
        MessageBoxA(NULL, "Failed to create host window.", appname, MB_OK | MB_ICONERROR);
        cleanup_exit(1);
    }
    SetWindowTextA(first_hwnd, title);

    /* Start background asset sync thread to verify and update disk assets if exe was upgraded */
    CreateThread(NULL, 0, background_asset_sync_thread, (LPVOID)target_web_dir, 0, NULL);

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
            for (WebKitSession *s = sessions_head; s; s = s->next) {
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

        for (WebKitSession *s = sessions_head; s; s = s->next) {
            if (bufchain_size(&s->send_queue) > 0) {
                session_send_queue_try(s);
            }
        }

        run_toplevel_callbacks();

        /* Process PuTTY timer queue */
        unsigned long next_timer;
        run_timers(GETTICKCOUNT(), &next_timer);

        /* Guarantee auto-reconnect trigger and dynamic countdown */
        for (WebKitSession *s = sessions_head; s; s = s->next) {
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
