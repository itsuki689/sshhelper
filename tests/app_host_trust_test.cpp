#include "mock_ssh.hpp"
#include "app.hpp"
#include <commctrl.h>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << "Line " << __LINE__ << ": " #c "\n"; return __LINE__; } } while(false)
using namespace std::chrono_literals;

struct HostTrustUiTest {
    static HWND window(HostTrustUi& ui) {
        ui.refresh();
        return ui.dialogs_.empty() ? nullptr : ui.dialogs_.begin()->second->window;
    }
    static auto pending(HostTrustUi& ui) { return ui.coordinator_.pending(); }
    static bool accept(HostTrustUi& ui, const HostTrustCoordinator::RequestPtr& request) {
        return ui.coordinator_.resolve(request, HostTrustResult::Accepted);
    }
};
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
}
struct AppWindowTest {
    static int run(const std::filesystem::path& root) {
        LoopbackSsh server, other;
        App app(GetModuleHandleW(nullptr), root / "AirSFTP.credentials");
        app.nextHotkey_ = MAKEWORD(VK_F23, HOTKEYF_CONTROL | HOTKEYF_ALT);
        app.previousHotkey_ = MAKEWORD(VK_F24, HOTKEYF_CONTROL | HOTKEYF_ALT);
        app.config_ = server.config;
        CHECK(app.registerClasses() && app.createMainWindow(SW_HIDE) && app.createTray());
        const auto pins = root / "AirSFTP.known_hosts";
        CHECK(!std::filesystem::exists(pins));
        expectedPinStore = pins;
        auto waitPrompt = [&]() {
            return eventually([&] { pump(); return app.hostTrustUi_ && HostTrustUiTest::window(*app.hostTrustUi_); });
        };
        app.startMonitor(app.config_);
        CHECK(waitPrompt());
        const auto prompt = HostTrustUiTest::window(*app.hostTrustUi_);
        wchar_t text[2048]{};
        GetDlgItemTextW(prompt, 100, text, 2048);
        CHECK(std::wstring(text).find(L"127.0.0.1") != std::wstring::npos);
        CHECK(std::wstring(text).find(std::to_wstring(server.config.port)) != std::wstring::npos);
        CHECK(std::wstring(text).find(L"SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA") != std::wstring::npos);
        CHECK(sshAuthentications == 0 && !std::filesystem::exists(pins));
        SendMessageW(prompt, WM_COMMAND, IDNO, 0);
        CHECK(eventually([&] { pump(); return app.snapshot_.status.find("rejected") != std::string::npos; }));
        // Wait across the production three-second reconnect interval.
        const auto end = std::chrono::steady_clock::now() + 3300ms;
        while (std::chrono::steady_clock::now() < end) { pump(); std::this_thread::sleep_for(5ms); }
        CHECK(sshHandshakes == 1 && sshAuthentications == 0 && HostTrustUiTest::pending(*app.hostTrustUi_).empty());
        app.monitorThread_.join();
        std::cout << "Rejection suppressed automatic reconnect\n" << std::flush;

        app.startMonitor(app.config_); CHECK(waitPrompt());
        const auto old = HostTrustUiTest::pending(*app.hostTrustUi_)[0];
        app.startMonitor(app.config_);
        CHECK(!HostTrustUiTest::accept(*app.hostTrustUi_, old));
        CHECK(eventually([&] {
            pump(); const auto pending = HostTrustUiTest::pending(*app.hostTrustUi_);
            return pending.size() == 1 && pending[0]->id != old->id;
        }));
        CHECK(waitPrompt());
        app.transfer_.active = true;
        const auto monitorGeneration = app.monitorGeneration_, transferGeneration = app.transferGeneration_;
        SendMessageW(app.window_, WM_CLOSE, 0, 0);
        CHECK(!IsWindowVisible(app.window_) && app.transfer_.active);
        CHECK(app.monitorGeneration_ == monitorGeneration && app.transferGeneration_ == transferGeneration);
        CHECK(eventually([&] { pump(); return app.snapshot_.status.find("cancelled") != std::string::npos; }));
        CHECK(!HostTrustUiTest::window(*app.hostTrustUi_) && !std::filesystem::exists(pins));
        app.monitorThread_.join(); app.transfer_.active = false;
        std::cout << "Reconnect and hide cancelled stale requests\n" << std::flush;

        app.startMonitor(app.config_); CHECK(waitPrompt());
        const auto configRequest = HostTrustUiTest::pending(*app.hostTrustUi_)[0];
        app.showSettings(); app.finishSettingsAnimation();
        CHECK(app.rememberConnection(other.config, "", ""));
        std::cout << "Configuration saved to isolated fixture\n" << std::flush;
        CHECK(!HostTrustUiTest::accept(*app.hostTrustUi_, configRequest));
        app.monitorThread_.join();
        app.startMonitor(app.config_); CHECK(waitPrompt());
        CHECK(HostTrustUiTest::pending(*app.hostTrustUi_)[0]->key.port == other.config.port);

        auto child = std::make_unique<App>(GetModuleHandleW(nullptr), root / "AirSFTP.credentials");
        child->root_ = &app; child->config_ = server.config;
        CHECK(child->createMainWindow(SW_HIDE));
        auto* peer = child.get(); app.children_.push_back(std::move(child));
        peer->startMonitor(peer->config_);
        CHECK(eventually([&] { pump(); return HostTrustUiTest::pending(*app.hostTrustUi_).size() == 2; }));
        const auto deleteRequest = HostTrustUiTest::pending(*app.hostTrustUi_)[0];
        app.deleted_ = true;
        DestroyWindow(app.window_);
        CHECK(!HostTrustUiTest::accept(*app.hostTrustUi_, deleteRequest));
        CHECK(eventually([&] { pump(); return HostTrustUiTest::pending(*app.hostTrustUi_).size() == 1; }));
        CHECK(HostTrustUiTest::pending(*app.hostTrustUi_)[0]->key.port == server.config.port);
        const auto exitRequest = HostTrustUiTest::pending(*app.hostTrustUi_)[0];
        app.quitApplication();
        CHECK(!HostTrustUiTest::accept(*app.hostTrustUi_, exitRequest));
        if (app.monitorThread_.joinable()) app.monitorThread_.join();
        if (peer->monitorThread_.joinable()) peer->monitorThread_.join();
        CHECK(HostTrustUiTest::pending(*app.hostTrustUi_).empty());
        CHECK(!std::filesystem::exists(pins) && sshAuthentications == 0);
        pump();
        // Exercise the modeless accept button through a real SshSession worker.
        HostTrustUi acceptUi(GetModuleHandleW(nullptr));
        auto confirm = acceptUi.confirmation(&server);
        auto accepted = std::async(std::launch::async, [&] {
            SshSession session({}, 15000, pins, confirm);
            std::string error;
            return session.connect(server.config, error);
        });
        CHECK(eventually([&] { pump(); return HostTrustUiTest::window(acceptUi); }));
        SendMessageW(HostTrustUiTest::window(acceptUi), WM_COMMAND, IDYES, 0);
        CHECK(accepted.get() && std::filesystem::exists(pins) && sshAuthentications == 1);
        CHECK(!authenticationWithoutPin);
        acceptUi.shutdown();
        std::cout << "Modeless trust UI, rejection retry policy, hide, reconnect, configuration, deletion and exit passed\n";
        return 0;
    }
};
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) || libssh2_init(0)) return 1;
    const auto root = std::filesystem::temp_directory_path() / ("airsftp-trust-ui-" + std::to_string(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(root)) return 1;
    const auto result = AppWindowTest::run(root);
    std::filesystem::remove_all(root); // Only this process's isolated fixture.
    libssh2_exit(); WSACleanup(); CoUninitialize();
    return result;
}
