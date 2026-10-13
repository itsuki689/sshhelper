#include "app.hpp"
#include "resource.h"
#include <algorithm>

namespace {
constexpr wchar_t TrayClass[] = L"AirSFTPTrayWindow";
constexpr UINT TrayMessage = WM_APP + 10;
constexpr UINT_PTR RetryTrayTimer = 1;
constexpr UINT ShowConnected = 100, HideAll = 101, AddServer = 102, ExitApp = 103, ServerBase = 1000;
const UINT TaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
std::wstring wide(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), count);
    return result;
}
}

bool App::createTray() {
    WNDCLASSEXW cls{sizeof(cls)};
    cls.lpfnWndProc = trayWindowProc;
    cls.hInstance = instance_;
    cls.lpszClassName = TrayClass;
    if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // A hidden top-level window receives Explorer's TaskbarCreated broadcast.
    trayWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, TrayClass, L"AirSFTP",
        WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
    if (!trayWindow_) return false;
    return addTrayIcon();
}

bool App::addTrayIcon() {
    if (!trayWindow_ || exiting_) return false;
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = trayWindow_;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = TrayMessage;
    icon.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_AIRSFTP));
    lstrcpynW(icon.szTip, L"AirSFTP · 双击显示已连接服务器", ARRAYSIZE(icon.szTip));
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    if (trayAdded_) KillTimer(trayWindow_, RetryTrayTimer);
    else SetTimer(trayWindow_, RetryTrayTimer, 5000, nullptr);
    return trayAdded_;
}

void App::removeTray() {
    if (!trayWindow_) return;
    KillTimer(trayWindow_, RetryTrayTimer);
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = trayWindow_;
    icon.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    trayAdded_ = false;
    DestroyWindow(trayWindow_);
    trayWindow_ = nullptr;
}

LRESULT CALLBACK App::trayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wParam, lParam);
    if (message == TaskbarCreated) {
        app->trayAdded_ = false;
        app->addTrayIcon();
        return 0;
    }
    if (message == WM_TIMER && wParam == RetryTrayTimer) { app->addTrayIcon(); return 0; }
    if (message == TrayMessage) {
        if (lParam == WM_LBUTTONDBLCLK) app->showConnectedServers();
        else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) app->showTrayMenu();
        return 0;
    }
    if (message == WM_CLOSE || (message == WM_ENDSESSION && wParam)) {
        app->quitApplication();
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void App::hideToTray() {
    if (!root_->trayWindow_) { DestroyWindow(window_); return; }
    showWhenConnected_ = false;
    if (animating_) finishWindowAnimation();
    hideSettings(false);
    ShowWindow(window_, SW_HIDE);
}

void App::showAtBottomRight() {
    if (!window_) return;
    if (animating_) finishWindowAnimation();
    collapseMode_ = CollapseMode::Expanded;
    hasExpandedRect_ = false;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = MulDiv(380, dpi, 96), height = MulDiv(300, dpi, 96);
    const int margin = MulDiv(24, dpi, 96);
    const int rows = std::max(1, (int(work.bottom - work.top) - margin) / (height + margin));
    const int columns = std::max(1, (int(work.right - work.left) - margin) / (width + margin));
    RECT target{};
    const auto all = servers();
    for (int slot = 0; slot < rows * columns; ++slot) {
        const int x = std::max(int(work.left), int(work.right) - (slot / rows + 1) * (width + margin));
        const int y = std::max(int(work.top), int(work.bottom) - (slot % rows + 1) * (height + margin));
        target = {x, y, x + width, y + height};
        const bool occupied = std::any_of(all.begin(), all.end(), [&](App* other) {
            if (other == this || !other->window_ || !IsWindowVisible(other->window_)) return false;
            RECT rect{}, overlap{};
            GetWindowRect(other->window_, &rect);
            return IntersectRect(&overlap, &rect, &target) != FALSE;
        });
        if (!occupied) break;
    }
    SetWindowPos(window_, HWND_TOPMOST, target.left, target.top, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::showConnectedServers() {
    for (auto* server : servers()) {
        if (!server->window_ || !server->snapshot_.connected) continue;
        server->showWhenConnected_ = false;
        if (!IsWindowVisible(server->window_)) server->showAtBottomRight();
        else {
            if (server->animating_) server->finishWindowAnimation();
            if (server->isCollapsed()) server->restoreExpanded();
        }
    }
}

void App::showTrayMenu() {
    HMENU menu = CreatePopupMenu(), serverMenu = CreatePopupMenu();
    if (!menu || !serverMenu) {
        if (menu) DestroyMenu(menu);
        if (serverMenu) DestroyMenu(serverMenu);
        return;
    }
    AppendMenuW(menu, MF_STRING, ShowConnected, L"显示已连接服务器");
    AppendMenuW(menu, MF_STRING, HideAll, L"隐藏全部窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto all = servers();
    for (std::size_t i = 0; i < all.size(); ++i) {
        auto title = wide(all[i]->windowTitle());
        // Escape menu accelerators in server-provided hostnames.
        for (std::size_t pos = 0; (pos = title.find(L'&', pos)) != std::wstring::npos; pos += 2)
            title.insert(pos, 1, L'&');
        title += all[i]->snapshot_.connected ? L" · 已连接" : L" · 未连接";
        AppendMenuW(serverMenu, MF_STRING, ServerBase + UINT(i), title.c_str());
    }
    if (all.empty()) AppendMenuW(serverMenu, MF_STRING | MF_GRAYED, 0, L"暂无服务器");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(serverMenu), L"服务器设置");
    AppendMenuW(menu, MF_STRING, AddServer, L"新增服务器");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ExitApp, L"退出");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(trayWindow_);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        cursor.x, cursor.y, 0, trayWindow_, nullptr);
    PostMessageW(trayWindow_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (exiting_) return;
    if (command == ShowConnected) showConnectedServers();
    else if (command == HideAll) for (auto* server : servers()) server->hideToTray();
    else if (command == AddServer) addServer();
    else if (command == ExitApp) quitApplication();
    else if (command >= ServerBase && command - ServerBase < all.size()) {
        auto* server = all[command - ServerBase];
        if (server->deleted_) return; // A modal loop may have processed other events.
        if (!server->window_ && !server->createMainWindow(SW_HIDE)) return;
        server->showSettings();
    }
}

void App::quitApplication() {
    if (root_ != this) { root_->quitApplication(); return; }
    if (exiting_) return;
    exiting_ = true;
    // Request cancellation everywhere before any worker is joined by destructors.
    stopMonitor(); stopTransfer();
    for (auto& child : children_) { child->stopMonitor(); child->stopTransfer(); }
    for (auto& child : children_) if (child->window_) DestroyWindow(child->window_);
    if (window_) DestroyWindow(window_);
    removeTray();
    PostQuitMessage(0);
}
