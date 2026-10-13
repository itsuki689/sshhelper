#include "app.hpp"
#include <commctrl.h>
#include <iostream>
#include <cmath>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n"; return __LINE__; } } while (false)

struct AppWindowTest {
    static int run() {
        App first(GetModuleHandleW(nullptr));
        first.config_.host = "offline-first.invalid";
        first.serverName_ = "First";
        first.background_ = 0x123456;
        first.foreground_ = 0xEEDDCC;
        CHECK(first.registerClasses());
        CHECK(first.createMainWindow(SW_HIDE));
        auto child = std::make_unique<App>(GetModuleHandleW(nullptr));
        child->root_ = &first;
        child->windowSlot_ = 1;
        child->config_.host = "offline-second.invalid";
        child->serverName_ = "Second";
        auto* second = child.get();
        first.children_.push_back(std::move(child));
        CHECK(second->createMainWindow(SW_HIDE));
        CHECK(first.servers().size() == 2);
        CHECK(first.window_ != second->window_);
        CHECK(first.windowTitle() == "First");
        auto* named = new TelemetrySnapshot;
        named->connected = true;
        named->hostName = "compute-node-01";
        SendMessageW(first.window_, WM_APP + 1, first.monitorGeneration_, reinterpret_cast<LPARAM>(named));
        CHECK(first.windowTitle() == "compute-node-01");
        wchar_t caption[128]{};
        GetWindowTextW(first.window_, caption, 128);
        CHECK(std::wstring(caption) == L"compute-node-01");
        CHECK(second->windowTitle() == "Second");
        auto* offline = new TelemetrySnapshot;
        SendMessageW(first.window_, WM_APP + 1, first.monitorGeneration_, reinterpret_cast<LPARAM>(offline));
        CHECK(first.windowTitle() == "compute-node-01");
        RECT a{}, b{};
        GetWindowRect(first.window_, &a);
        GetWindowRect(second->window_, &b);
        CHECK(a.left != b.left || a.top != b.top);
        CHECK(GetWindowLongPtrW(first.window_, GWL_EXSTYLE) & WS_EX_ACCEPTFILES);
        CHECK(GetWindowLongPtrW(second->window_, GWL_EXSTYLE) & WS_EX_ACCEPTFILES);
        CHECK(first.ensureGraphics());
        auto surface = first.surfaceBrush_->GetColor();
        auto text = first.textBrush_->GetColor();
        CHECK(std::abs(surface.r - 0x12 / 255.0f) < 0.001f);
        CHECK(std::abs(text.r - 0xEE / 255.0f) < 0.001f);
        first.collapseMode_ = App::CollapseMode::Side;
        first.discardGraphics();
        CHECK(first.ensureGraphics());
        surface = first.surfaceBrush_->GetColor();
        text = first.textBrush_->GetColor();
        CHECK(std::abs(surface.g - 0x34 / 255.0f) < 0.001f);
        CHECK(std::abs(text.b - 0xCC / 255.0f) < 0.001f);
        CHECK(second->collapseMode_ == App::CollapseMode::Expanded);
        first.createSettingsWindow();
        second->createSettingsWindow();
        CHECK(first.settingsWindow_ && second->settingsWindow_);
        CHECK(first.settingsWindow_ != second->settingsWindow_);
        first.collapseMode_ = App::CollapseMode::Expanded;
        ShowWindow(first.window_, SW_SHOWNOACTIVATE);
        first.showSettings();
        CHECK(IsWindowVisible(first.settingsWindow_));
        RECT panel{}, workArea{};
        GetWindowRect(first.settingsWindow_, &panel);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(first.window_, MONITOR_DEFAULTTONEAREST), &monitor);
        workArea = monitor.rcWork;
        CHECK(panel.left >= workArea.left && panel.right <= workArea.right);
        CHECK(panel.top >= workArea.top && panel.bottom <= workArea.bottom);
        RECT ownerBounds{}, settingsOverlap{};
        GetWindowRect(first.window_, &ownerBounds);
        if (workArea.right - workArea.left >= MulDiv(920, GetDpiForWindow(first.window_), 96))
            CHECK(!IntersectRect(&settingsOverlap, &ownerBounds, &panel));
        SetDlgItemTextW(first.settingsWindow_, App::HostEdit, L"draft.example");
        first.selectSettingsTab(1);
        CHECK(IsWindowVisible(GetDlgItem(first.settingsWindow_, App::UploadButton)));
        CHECK(!IsWindowVisible(GetDlgItem(first.settingsWindow_, App::HostEdit)));
        SendMessageW(first.settingsWindow_, WM_COMMAND, App::SelectedFiles, 0);
        CHECK(first.partialTransfer_);
        first.selectSettingsTab(2);
        CHECK(IsWindowVisible(GetDlgItem(first.settingsWindow_, App::NextHotkey)));
        CHECK(!IsWindowVisible(GetDlgItem(first.settingsWindow_, App::UploadButton)));
        first.showSettings(); // Repeated gear clicks must not discard an in-progress edit.
        wchar_t draft[64]{};
        GetDlgItemTextW(first.settingsWindow_, App::HostEdit, draft, 64);
        CHECK(std::wstring(draft) == L"draft.example");
        HDC colorDc = CreateCompatibleDC(nullptr);
        SendMessageW(first.settingsWindow_, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(colorDc), 0);
        CHECK(GetTextColor(colorDc) == RGB(0xEE, 0xDD, 0xCC));
        DeleteDC(colorDc);
        first.settingsAnimating_ = true;
        first.settingsAnimationStarted_ = GetTickCount64() - 90;
        first.tickSettingsAnimation();
        HRGN clip = CreateRectRgn(0, 0, 0, 0);
        CHECK(GetWindowRgn(first.settingsWindow_, clip) != ERROR);
        SendMessageW(first.settingsWindow_, WM_CLOSE, 0, 0);
        CHECK(first.settingsClosing_ || !IsWindowVisible(first.settingsWindow_));
        first.finishSettingsAnimation();
        CHECK(!first.settingsAnimating_ && !IsWindowVisible(first.settingsWindow_));
        CHECK(GetWindowRgn(first.settingsWindow_, clip) == ERROR);
        SendMessageW(first.settingsWindow_, WM_TIMER, 2, 0);
        CHECK(!IsWindowVisible(first.settingsWindow_));
        DeleteObject(clip);
        first.showSettings();
        GetDlgItemTextW(first.settingsWindow_, App::HostEdit, draft, 64);
        CHECK(std::wstring(draft) == L"draft.example");
        first.finishSettingsAnimation();
        first.selectSettingsTab(0);
        CHECK(IsWindowVisible(GetDlgItem(first.settingsWindow_, App::HostEdit)));
        HWND ownedDialog = CreateWindowExW(0, L"STATIC", L"picker", WS_POPUP,
            0, 0, 10, 10, first.settingsWindow_, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(ownedDialog);
        SendMessageW(first.settingsWindow_, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(ownedDialog));
        CHECK(IsWindowVisible(first.settingsWindow_) && !first.settingsClosing_);
        DestroyWindow(ownedDialog);
        // The owning monitor activates before its gear click; activation must not race the toggle.
        SendMessageW(first.settingsWindow_, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(first.window_));
        CHECK(!first.settingsClosing_);
        SendMessageW(first.window_, WM_LBUTTONUP, 0,
            MAKELPARAM(MulDiv(314, GetDpiForWindow(first.window_), 96), MulDiv(27, GetDpiForWindow(first.window_), 96)));
        CHECK(first.settingsClosing_ || !IsWindowVisible(first.settingsWindow_));
        first.finishSettingsAnimation();
        CHECK(!IsWindowVisible(first.settingsWindow_));
        first.toggleSettings();
        first.finishSettingsAnimation();
        CHECK(IsWindowVisible(first.settingsWindow_));
        SendMessageW(first.settingsWindow_, WM_ACTIVATE, WA_INACTIVE, reinterpret_cast<LPARAM>(second->window_));
        CHECK(first.settingsClosing_ || !IsWindowVisible(first.settingsWindow_));
        first.finishSettingsAnimation();
        CHECK(!IsWindowVisible(first.settingsWindow_));
        first.showSettings();
        first.finishSettingsAnimation();
        GetDlgItemTextW(first.settingsWindow_, App::HostEdit, draft, 64);
        CHECK(std::wstring(draft) == L"draft.example");
        // Reverse a close in progress without clearing the form or leaving it clipped.
        first.hideSettings();
        first.showSettings();
        first.finishSettingsAnimation();
        CHECK(IsWindowVisible(first.settingsWindow_));
        first.hideSettings(false);
        CHECK(GetDlgItem(first.settingsWindow_, App::OpenLocalFolder));
        CHECK(GetDlgItem(first.settingsWindow_, App::CancelTransfer));
        auto decision = std::make_shared<App::TransferDecision>();
        auto* staleDecision = new std::shared_ptr<App::TransferDecision>(decision);
        SendMessageW(first.window_, WM_APP + 3, first.transferGeneration_ + 1, reinterpret_cast<LPARAM>(staleDecision));
        CHECK(decision->result == 0);
        first.transfer_.active = true;
        SendMessageW(first.settingsWindow_, WM_COMMAND, App::CancelTransfer, 0);
        CHECK(!first.transfer_.active && first.transfer_.failed);
        constexpr unsigned next = MAKEWORD(VK_F20, HOTKEYF_CONTROL | HOTKEYF_ALT);
        constexpr unsigned previous = MAKEWORD(VK_F21, HOTKEYF_CONTROL | HOTKEYF_ALT);
        CHECK(first.setHotkeys(next, previous, false));
        CHECK(first.hotkeysRegistered_);
        CHECK(SendDlgItemMessageW(second->settingsWindow_, App::NextHotkey, HKM_GETHOTKEY, 0, 0) == next);
        CHECK(!first.setHotkeys(next, next, false));
        CHECK(!first.setHotkeys('N', previous, false));
        CHECK(first.nextHotkey_ == next);
        CHECK(RegisterHotKey(nullptr, 99, MOD_CONTROL | MOD_ALT, VK_F22));
        CHECK(!first.setHotkeys(MAKEWORD(VK_F22, HOTKEYF_CONTROL | HOTKEYF_ALT), previous, false));
        CHECK(first.hotkeysRegistered_ && first.nextHotkey_ == next);
        UnregisterHotKey(nullptr, 99);
        UnregisterHotKey(nullptr, 1);
        UnregisterHotKey(nullptr, 2);
        // Closing the original window must neither quit nor destroy another server.
        first.transfer_.active = true;
        auto pendingDecision = std::make_shared<App::TransferDecision>();
        PostMessageW(first.window_, WM_APP + 3, first.transferGeneration_,
            reinterpret_cast<LPARAM>(new std::shared_ptr<App::TransferDecision>(pendingDecision)));
        DestroyWindow(first.window_);
        CHECK(pendingDecision->result == 0);
        MSG message{};
        CHECK(!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
        CHECK(second->window_ && IsWindow(second->window_));
        CHECK(first.servers().size() == 2); // Saved configuration survives window close.
        CHECK(first.createMainWindow(SW_HIDE));
        CHECK(first.messageWindow_ == first.window_);
        CHECK(!first.transfer_.active);
        CHECK(first.collapseMode_ == App::CollapseMode::Expanded);
        // Late worker messages from a previous generation cannot replace current state.
        auto* stale = new TelemetrySnapshot;
        stale->status = "stale";
        SendMessageW(first.window_, WM_APP + 1, first.monitorGeneration_ - 1, reinterpret_cast<LPARAM>(stale));
        CHECK(first.snapshot_.status != "stale");
        DestroyWindow(second->window_);
        CHECK(!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
        DestroyWindow(first.window_);
        // Visible settings enqueue paints/activation messages; process them before
        // expecting the low-priority quit notification.
        bool quitReceived = false;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { quitReceived = true; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        CHECK(quitReceived);

        // Startup keeps offline profiles hidden; only first successful snapshots reveal them.
        App tray(GetModuleHandleW(nullptr));
        tray.config_.host = "tray-first.invalid";
        CHECK(tray.createMainWindow(SW_HIDE));
        tray.createTray();
        CHECK(tray.trayWindow_ && IsWindow(tray.trayWindow_));
        tray.showWhenConnected_ = true;
        auto hidden = std::make_unique<App>(GetModuleHandleW(nullptr));
        hidden->root_ = &tray;
        hidden->config_.host = "tray-second.invalid";
        hidden->windowSlot_ = 1;
        CHECK(hidden->createMainWindow(SW_HIDE));
        hidden->showWhenConnected_ = true;
        auto* nextServer = hidden.get();
        tray.children_.push_back(std::move(hidden));
        auto post = [](App& app, bool online, std::uintptr_t generation) {
            auto* sample = new TelemetrySnapshot;
            sample->connected = online;
            SendMessageW(app.window_, WM_APP + 1, generation, reinterpret_cast<LPARAM>(sample));
        };
        post(tray, false, tray.monitorGeneration_);
        CHECK(!IsWindowVisible(tray.window_) && !IsWindowVisible(nextServer->window_));
        post(tray, true, tray.monitorGeneration_ + 1);
        CHECK(!IsWindowVisible(tray.window_));
        post(*nextServer, true, nextServer->monitorGeneration_);
        CHECK(IsWindowVisible(nextServer->window_) && !IsWindowVisible(tray.window_));
        RECT firstOnline{}, secondOnline{}, work{};
        GetWindowRect(nextServer->window_, &firstOnline);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        CHECK(firstOnline.right <= work.right && firstOnline.bottom <= work.bottom);
        CHECK(work.right - firstOnline.right < MulDiv(30, GetDpiForWindow(nextServer->window_), 96));
        CHECK(work.bottom - firstOnline.bottom < MulDiv(30, GetDpiForWindow(nextServer->window_), 96));
        post(tray, true, tray.monitorGeneration_);
        CHECK(IsWindowVisible(tray.window_));
        GetWindowRect(tray.window_, &secondOnline);
        RECT overlap{};
        if (work.bottom - work.top >= 650 || work.right - work.left >= 900)
            CHECK(!IntersectRect(&overlap, &firstOnline, &secondOnline));
        tray.transfer_.active = true;
        const auto generation = tray.monitorGeneration_;
        SendMessageW(tray.window_, WM_CLOSE, 0, 0);
        CHECK(tray.window_ && !IsWindowVisible(tray.window_));
        CHECK(tray.transfer_.active && tray.monitorGeneration_ == generation);
        post(tray, true, tray.monitorGeneration_);
        CHECK(!IsWindowVisible(tray.window_)); // Periodic samples do not undo a manual hide.
        nextServer->hideToTray();
        CHECK(!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
        post(*nextServer, false, nextServer->monitorGeneration_);
        tray.showConnectedServers();
        CHECK(IsWindowVisible(tray.window_) && !IsWindowVisible(nextServer->window_));
        // Destroying a profile window does not remove the independent tray.
        DestroyWindow(tray.window_);
        CHECK(IsWindow(tray.trayWindow_));
        CHECK(!PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
        tray.quitApplication();
        CHECK(!tray.trayWindow_ && !nextServer->window_);
        CHECK(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE));
        return 0;
    }
};

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int result = AppWindowTest::run();
    CoUninitialize();
    return result;
}
