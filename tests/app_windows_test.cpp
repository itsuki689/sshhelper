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
        DestroyWindow(first.window_);
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
