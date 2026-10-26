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
        // Name clicks fold; releasing outside/capture loss cancels without affecting transfers.
        const auto dpi = GetDpiForWindow(first.window_);
        const LPARAM namePoint = MAKELPARAM(MulDiv(60, dpi, 96), MulDiv(27, dpi, 96));
        POINT screen{MulDiv(60, dpi, 96), MulDiv(27, dpi, 96)};
        ClientToScreen(first.window_, &screen);
        CHECK(SendMessageW(first.window_, WM_NCHITTEST, 0, MAKELPARAM(screen.x, screen.y)) == HTCLIENT);
        const auto monitorGeneration = first.monitorGeneration_;
        first.transfer_.active = true;
        MONITORINFO clickMonitor{sizeof(clickMonitor)};
        CHECK(GetMonitorInfoW(MonitorFromWindow(first.window_, MONITOR_DEFAULTTONEAREST), &clickMonitor));
        RECT expanded{}; GetWindowRect(first.window_, &expanded);
        // Even near the top edge, a name click chooses the nearest left/right side.
        for (const bool left : {true, false}) {
            const int inset = MulDiv(40, dpi, 96);
            const int x = left ? clickMonitor.rcWork.left + inset :
                clickMonitor.rcWork.right - (expanded.right - expanded.left) - inset;
            SetWindowPos(first.window_, nullptr, x, clickMonitor.rcWork.top + MulDiv(8, dpi, 96),
                0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(first.window_, WM_LBUTTONDOWN, MK_LBUTTON, namePoint);
            CHECK(first.compactPointerDown_ && first.titlePointerDown_);
            SendMessageW(first.window_, WM_LBUTTONUP, 0, namePoint);
            first.finishWindowAnimation();
            CHECK(first.collapseMode_ == App::CollapseMode::Side);
            RECT compact{}; GetWindowRect(first.window_, &compact);
            CHECK(left ? compact.left == clickMonitor.rcWork.left : compact.right == clickMonitor.rcWork.right);
            CHECK(compact.right - compact.left == MulDiv(36, dpi, 96));
            CHECK(compact.bottom - compact.top == MulDiv(78, dpi, 96));
            CHECK(compact.top >= clickMonitor.rcWork.top && compact.bottom <= clickMonitor.rcWork.bottom);
            CHECK(first.transfer_.active && first.monitorGeneration_ == monitorGeneration);
            CHECK(second->collapseMode_ == App::CollapseMode::Expanded);
            SendMessageW(first.window_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(5, 5));
            SendMessageW(first.window_, WM_LBUTTONUP, 0, MAKELPARAM(5, 5));
            first.finishWindowAnimation(); CHECK(!first.isCollapsed());
        }
        SendMessageW(first.window_, WM_LBUTTONDOWN, MK_LBUTTON, namePoint);
        SendMessageW(first.window_, WM_LBUTTONUP, 0, MAKELPARAM(MulDiv(200, dpi, 96), MulDiv(90, dpi, 96)));
        CHECK(!first.isCollapsed() && !first.animating_);
        SendMessageW(first.window_, WM_LBUTTONDOWN, MK_LBUTTON, namePoint);
        ReleaseCapture(); CHECK(!first.compactPointerDown_ && !first.titlePointerDown_);
        first.transfer_.active = false;
        first.showSettings();
        std::uint32_t interval = 0;
        CHECK(first.readRefreshInterval(interval) && interval == 2);
        for (const auto* invalid : {L"", L"0", L"3601", L"-2", L"1.5", L"2abc"}) {
            SetDlgItemTextW(first.settingsWindow_, App::RefreshIntervalEdit, invalid);
            CHECK(!first.readRefreshInterval(interval));
        }
        SetDlgItemTextW(first.settingsWindow_, App::RefreshIntervalEdit, L"3600");
        CHECK(first.readRefreshInterval(interval) && interval == 3600);
        SetDlgItemTextW(first.settingsWindow_, App::RefreshIntervalEdit, L"7");
        CHECK(first.readRefreshInterval(interval) && interval == 7);
        first.refreshIntervalSeconds_ = 3600;
        const auto waitStarted = std::chrono::steady_clock::now();
        std::jthread waiter([&](std::stop_token token) { first.waitRefreshInterval(token); });
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        first.refreshIntervalSeconds_ = 1; // A shorter setting updates an existing wait.
        waiter.join();
        const auto elapsed = std::chrono::steady_clock::now() - waitStarted;
        CHECK(elapsed >= std::chrono::milliseconds(900) && elapsed < std::chrono::seconds(3));
        first.refreshIntervalSeconds_ = 3600;
        const auto cancelStarted = std::chrono::steady_clock::now();
        std::jthread cancelledWait([&](std::stop_token token) { first.waitRefreshInterval(token); });
        cancelledWait.request_stop(); cancelledWait.join();
        CHECK(std::chrono::steady_clock::now() - cancelStarted < std::chrono::seconds(1));
        first.refreshIntervalSeconds_ = 2;

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
        CHECK(!FindWindowExW(first.settingsWindow_, nullptr, HOTKEY_CLASSW, nullptr));
        CHECK(!IsWindowVisible(GetDlgItem(first.settingsWindow_, App::UploadButton)));
        first.showSettings(); // Repeated gear clicks must not discard an in-progress edit.
        wchar_t draft[64]{};
        GetDlgItemTextW(first.settingsWindow_, App::HostEdit, draft, 64);
        CHECK(std::wstring(draft) == L"draft.example");
        CHECK(first.readRefreshInterval(interval) && interval == 7);
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
        // No global switching shortcuts or HOTKEY controls remain.
        CHECK(!FindWindowExW(first.settingsWindow_, nullptr, HOTKEY_CLASSW, nullptr));
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
        // The built-in host is a singleton peer with the same theme, settings and tray behavior.
        auto local = std::make_unique<App>(GetModuleHandleW(nullptr));
        local->root_ = &tray;
        ServerProfile localProfile; localProfile.local = true; localProfile.background = 0x123456;
        localProfile.refreshIntervalSeconds = 7;
        local->applyProfile(localProfile);
        CHECK(local->local_ && local->config_.host.empty() && local->profile().local);
        CHECK(local->refreshIntervalSeconds_ == 7);
        CHECK(local->createMainWindow(SW_HIDE));
        CHECK(!(GetWindowLongPtrW(local->window_, GWL_EXSTYLE) & WS_EX_ACCEPTFILES));
        local->startMonitor({});
        const auto sampleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!local->snapshot_.connected && std::chrono::steady_clock::now() < sampleDeadline) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(local->snapshot_.connected && local->snapshot_.status == "Local");
        CHECK(!local->detectedHostName_.empty());
        local->stopMonitor(); local->monitorThread_.join();
        local->showSettings(); local->finishSettingsAnimation();
        CHECK(!IsWindowVisible(GetDlgItem(local->settingsWindow_, App::HostEdit)));
        CHECK(!IsWindowEnabled(GetDlgItem(local->settingsWindow_, App::TransferTab)));
        CHECK(!IsWindowVisible(GetDlgItem(local->settingsWindow_, App::DeleteServer)));
        ConnectionConfig noSsh; std::string localFolder, remoteFolder;
        CHECK(local->readSettings(noSsh, localFolder, remoteFolder));
        CHECK(noSsh.host.empty() && noSsh.username.empty());
        local->selectSettingsTab(2);
        CHECK(IsWindowVisible(GetDlgItem(local->settingsWindow_, App::RefreshIntervalEdit)));
        local->selectSettingsTab(1); CHECK(local->settingsTab_ == 0);
        local->startTransfer(TransferDirection::Upload);
        local->uploadDropped({"C:/unused.txt"}); CHECK(!local->transfer_.active);
        local->detectedHostName_ = "desktop";
        CHECK(local->windowTitle().find("desktop") != std::string::npos);
        auto* localPeer = local.get();
        tray.children_.push_back(std::move(local));
        const auto peers = tray.servers().size();
        tray.showLocalMonitor(); tray.showLocalMonitor();
        CHECK(tray.servers().size() == peers && IsWindowVisible(localPeer->window_));
        localPeer->hideToTray(); CHECK(!IsWindowVisible(localPeer->window_));
        post(*localPeer, true, localPeer->monitorGeneration_);
        tray.showConnectedServers(); CHECK(IsWindowVisible(localPeer->window_));
        tray.quitApplication();
        CHECK(!tray.trayWindow_ && !nextServer->window_);
        bool localQuit = false;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { localQuit = true; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        CHECK(localQuit);
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
