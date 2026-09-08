/*
 * webview_host.cpp - WebView2 COM integration layer.
 * Implements decoupled C-callable interfaces for WebView2 host window.
 */

#include "webview_host.h"

#include <windows.h>
#include <wrl.h>
#include <string>
#include <vector>

#include "WebView2.h"

using namespace Microsoft::WRL;

#include <map>
#include <memory>

static std::wstring Utf8ToWide(const std::string &str) {
    if (str.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    std::wstring wstr(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], size);
    return wstr;
}

static std::string WideToUtf8(const std::wstring &wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string str(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], size, NULL, NULL);
    return str;
}

static std::string Base64Encode(const unsigned char *data, size_t len) {
    static const char s_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int val = ((unsigned int)data[i]) << 16;
        if (i + 1 < len) val |= ((unsigned int)data[i + 1]) << 8;
        if (i + 2 < len) val |= ((unsigned int)data[i + 2]);
        out.push_back(s_b64[(val >> 18) & 0x3F]);
        out.push_back(s_b64[(val >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? s_b64[(val >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? s_b64[val & 0x3F] : '=');
    }
    return out;
}

struct WebViewWindow {
    HWND hwnd = NULL;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool ready = false;
    std::vector<std::wstring> pending_messages;
    WebViewMessageCallback on_message = NULL;
    void *userdata = NULL;
};

static std::map<HWND, std::shared_ptr<WebViewWindow>> s_windows;
static ComPtr<ICoreWebView2Environment> s_environment;

static void FlushPendingMessages(WebViewWindow *win)
{
    if (!win) return;
    win->ready = true;
    if (win->webview) {
        for (const auto &wmsg : win->pending_messages) {
            win->webview->PostWebMessageAsString(wmsg.c_str());
        }
        win->pending_messages.clear();
    }
}

void webview_host_resize(HWND hwnd)
{
    if (!hwnd) return;
    auto it = s_windows.find(hwnd);
    if (it != s_windows.end() && it->second->controller) {
        RECT bounds;
        GetClientRect(hwnd, &bounds);
        it->second->controller->put_Bounds(bounds);
    }
}

void webview_host_focus(HWND hwnd)
{
    if (!hwnd) return;
    auto it = s_windows.find(hwnd);
    if (it != s_windows.end() && it->second->controller) {
        it->second->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
}

void webview_host_close(HWND hwnd)
{
    if (!hwnd) return;
    auto it = s_windows.find(hwnd);
    if (it != s_windows.end()) {
        if (it->second->controller) {
            it->second->controller->Close();
            it->second->controller = nullptr;
        }
        it->second->webview = nullptr;
        s_windows.erase(it);
    }
}

void webview_host_send_to_window(HWND hwnd, const char *msg)
{
    if (!msg || !hwnd) return;
    auto it = s_windows.find(hwnd);
    if (it == s_windows.end()) return;
    auto win = it->second;

    std::wstring wmsg = Utf8ToWide(msg);
    if (win->ready && win->webview) {
        win->webview->PostWebMessageAsString(wmsg.c_str());
    } else {
        win->pending_messages.push_back(wmsg);
    }
}

void webview_host_send_session_binary_to_window(HWND hwnd, char type, int session_id,
                                                const void *data, size_t len)
{
    if (!data || len == 0 || !hwnd) return;
    std::string b64 = Base64Encode((const unsigned char *)data, len);
    std::string full = std::string(1, type) + std::to_string(session_id) + ":" + b64;
    webview_host_send_to_window(hwnd, full.c_str());
}

void webview_host_send_session_text_to_window(HWND hwnd, char type, int session_id,
                                              const char *text)
{
    if (!hwnd) return;
    std::string full = std::string(1, type) + std::to_string(session_id) + ":" + (text ? text : "");
    webview_host_send_to_window(hwnd, full.c_str());
}

static void SetupWindowController(std::shared_ptr<WebViewWindow> win,
                                  ICoreWebView2Environment *env,
                                  const std::wstring &initial_url)
{
    HWND hwnd = win->hwnd;
    env->CreateCoreWebView2Controller(
        hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [win, initial_url](HRESULT result, ICoreWebView2Controller *controller) -> HRESULT {
                if (FAILED(result) || !controller) {
                    MessageBoxA(win->hwnd, "Failed to create WebView2 Controller.", "PuTTY-WebView", MB_OK | MB_ICONERROR);
                    return result;
                }

                win->controller = controller;
                win->controller->get_CoreWebView2(&win->webview);

                ComPtr<ICoreWebView2Settings> settings;
                if (SUCCEEDED(win->webview->get_Settings(&settings)) && settings) {
                    settings->put_AreDefaultContextMenusEnabled(FALSE);
                    settings->put_IsStatusBarEnabled(FALSE);
                }

                webview_host_resize(win->hwnd);

                win->webview->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [win](ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                            FlushPendingMessages(win.get());
                            LPWSTR raw = nullptr;
                            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                std::string utf8 = WideToUtf8(raw);
                                CoTaskMemFree(raw);
                                if (win->on_message) {
                                    win->on_message(win->hwnd, utf8.c_str(), win->userdata);
                                }
                            }
                            return S_OK;
                        }).Get(),
                    nullptr
                );

                win->webview->add_NavigationCompleted(
                    Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [win](ICoreWebView2 *sender, ICoreWebView2NavigationCompletedEventArgs *args) -> HRESULT {
                            FlushPendingMessages(win.get());
                            return S_OK;
                        }).Get(),
                    nullptr
                );

                if (!initial_url.empty()) {
                    std::wstring url = initial_url;
                    if (url.find(L"://") == std::wstring::npos) {
                        for (auto &ch : url) {
                            if (ch == L'\\') ch = L'/';
                        }
                        url = L"file:///" + url;
                    }
                    win->webview->Navigate(url.c_str());
                }

                return S_OK;
            }).Get()
    );
}

bool webview_host_init(HWND hwnd, const wchar_t *html_path,
                       WebViewMessageCallback on_message, void *userdata)
{
    if (!hwnd) return false;

    auto win = std::make_shared<WebViewWindow>();
    win->hwnd = hwnd;
    win->on_message = on_message;
    win->userdata = userdata;
    s_windows[hwnd] = win;

    std::wstring initial_url = html_path ? html_path : L"";

    if (s_environment) {
        SetupWindowController(win, s_environment.Get(), initial_url);
        return true;
    }

    wchar_t temp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_dir);
    std::wstring user_data_folder = std::wstring(temp_dir) + L"putty_webview_data";

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data_folder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [win, initial_url](HRESULT result, ICoreWebView2Environment *env) -> HRESULT {
                if (FAILED(result) || !env) {
                    MessageBoxA(win->hwnd,
                        "Failed to initialize WebView2 Runtime.\n"
                        "Please install the Microsoft Edge WebView2 Runtime from:\n"
                        "https://developer.microsoft.com/en-us/microsoft-edge/webview2/",
                        "PuTTY-WebView", MB_OK | MB_ICONERROR);
                    return result;
                }
                s_environment = env;
                SetupWindowController(win, s_environment.Get(), initial_url);
                return S_OK;
            }).Get()
    );

    return SUCCEEDED(hr);
}

