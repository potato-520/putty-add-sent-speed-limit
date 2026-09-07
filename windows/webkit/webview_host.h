#ifndef WEBVIEW_HOST_H
#define WEBVIEW_HOST_H

#include <windows.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*WebViewMessageCallback)(HWND hwnd, const char *message, void *userdata);

bool webview_host_init(HWND hwnd, const wchar_t *html_path,
                       WebViewMessageCallback on_message, void *userdata);
void webview_host_resize(HWND hwnd);
void webview_host_close(HWND hwnd);
void webview_host_send_to_window(HWND hwnd, const char *msg);
void webview_host_send_session_binary_to_window(HWND hwnd, char type, int session_id,
                                                const void *data, size_t len);
void webview_host_send_session_text_to_window(HWND hwnd, char type, int session_id,
                                              const char *text);

#ifdef __cplusplus
}
#endif

#endif /* WEBVIEW_HOST_H */

