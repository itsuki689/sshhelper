#include "app.hpp"
#include "credential_store.hpp"
#include "ssh_client.hpp"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <filesystem>

namespace {
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    std::wstring text(MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), text.data(), int(text.size()));
    return text;
}
std::string encoded(const std::wstring& value) {
    const auto text = std::filesystem::path(value).u8string();
    return {text.begin(), text.end()};
}
UINT modifiers(unsigned shortcut) {
    const auto flags = HIBYTE(shortcut);
    return MOD_NOREPEAT | ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
        ((flags & HOTKEYF_ALT) ? MOD_ALT : 0) | ((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0);
}
COLORREF colorRef(std::uint32_t rgb) { return RGB(rgb >> 16, (rgb >> 8) & 255, rgb & 255); }
}

std::vector<App*> App::servers() {
    std::vector<App*> result;
    if (!root_->deleted_) result.push_back(root_);
    for (auto& child : root_->children_)
        if (!child->deleted_) result.push_back(child.get());
    return result;
}

void App::applyProfile(const ServerProfile& value) {
    local_ = value.local;
    config_ = local_ ? ConnectionConfig{} : value.connection;
    serverName_ = value.name.empty() ? value.connection.host : value.name;
    localPath_ = value.localFolder;
    remotePath_ = value.remoteFolder;
    background_ = value.background;
    foreground_ = value.foreground;
    refreshIntervalSeconds_ = std::clamp(value.refreshIntervalSeconds, 1u, 3600u);
}

std::string App::windowTitle() const {
    if (local_) return (detectedHostName_.empty() ? std::string("本机") : detectedHostName_ + " · 本机");
    if (!detectedHostName_.empty()) return detectedHostName_;
    if (!serverName_.empty()) return serverName_;
    return config_.host.empty() ? "AirSFTP" : config_.host;
}

ServerProfile App::profile() const {
    ServerProfile value;
    value.local = local_;
    value.connection = config_;
    value.name = serverName_;
    value.localFolder = localPath_;
    value.remoteFolder = remotePath_;
    value.background = background_;
    value.foreground = foreground_;
    value.refreshIntervalSeconds = refreshIntervalSeconds_.load();
    return value;
}

bool App::saveServers() {
    ServerSettings saved;
    saved.nextHotkey = root_->nextHotkey_;
    saved.previousHotkey = root_->previousHotkey_;
    for (auto* server : servers())
        if (server->local_ || !server->config_.host.empty()) saved.servers.push_back(server->profile());
    std::string error;
    if (saveServerSettings(defaultCredentialPath(), saved, error)) return true;
    MessageBoxW(settingsWindow_ ? settingsWindow_ : window_, wide(error).c_str(),
        L"AirSFTP 配置保存失败", MB_OK | MB_ICONWARNING);
    return false;
}

void App::addServer() {
    const auto all = servers();
    if (std::count_if(all.begin(), all.end(), [](App* app) { return !app->local_; }) >= 64) {
        MessageBoxW(settingsWindow_, L"最多支持 64 台服务器。", L"AirSFTP", MB_OK);
        return;
    }
    auto server = std::make_unique<App>(instance_);
    server->root_ = root_;
    server->windowSlot_ = root_->children_.size() + 1;
    auto* added = server.get();
    root_->children_.push_back(std::move(server));
    if (!added->createMainWindow(SW_SHOWNORMAL)) {
        added->deleted_ = true;
        MessageBoxW(settingsWindow_, L"无法创建服务器窗口。", L"AirSFTP", MB_OK | MB_ICONERROR);
        return;
    }
    hideSettings();
    added->showSettings();
}

void App::showLocalMonitor() {
    for (auto* server : servers()) {
        if (!server->local_) continue;
        if (!server->window_ && server->createMainWindow(SW_HIDE)) server->startMonitor({});
        server->showWhenConnected_ = false;
        server->showAtBottomRight();
        return;
    }
}

void App::reopenServers() {
    for (auto* server : servers()) {
        if (server->window_) { server->showWhenConnected_ = false; server->showAtBottomRight(); continue; }
        server->collapseMode_ = CollapseMode::Expanded;
        server->animating_ = false;
        server->hasExpandedRect_ = false;
        if (server->createMainWindow(SW_SHOWNOACTIVATE) && (server->local_ || !server->config_.host.empty()))
            server->startMonitor(server->config_);
    }
}

void App::switchServer(bool previous) {
    auto open = servers();
    std::erase_if(open, [](App* app) { return !app->window_ || !IsWindowVisible(app->window_); });
    if (open.empty()) return;
    const HWND foreground = GetForegroundWindow();
    auto current = std::find_if(open.begin(), open.end(), [foreground](App* app) {
        return app->window_ == foreground || app->settingsWindow_ == foreground;
    });
    std::size_t index = current == open.end() ? (previous ? 0 : open.size() - 1) :
        std::size_t(current - open.begin());
    index = (index + (previous ? open.size() - 1 : 1)) % open.size();
    auto* target = open[index];
    if (target->animating_) target->finishWindowAnimation();
    if (target->isCollapsed()) target->restoreExpanded();
    SetWindowPos(target->window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(target->window_);
}

bool App::setHotkeys(unsigned next, unsigned previous, bool reportError) {
    auto* owner = root_;
    if (owner->hotkeysRegistered_ && next == owner->nextHotkey_ && previous == owner->previousHotkey_)
        return true;
    auto valid = [](unsigned key) {
        return key <= 0x0FFF && LOBYTE(key) != 0 && (HIBYTE(key) & (HOTKEYF_CONTROL | HOTKEYF_ALT));
    };
    auto warn = [&](const wchar_t* message) {
        if (reportError) MessageBoxW(settingsWindow_ ? settingsWindow_ : window_, message,
            L"AirSFTP 快捷键", MB_OK | MB_ICONWARNING);
    };
    if (!valid(next) || !valid(previous) ||
        (LOBYTE(next) == LOBYTE(previous) && modifiers(next) == modifiers(previous))) {
        warn(L"请设置两个不同的快捷键，至少包含 Ctrl 或 Alt。");
        return false;
    }
    const bool hadOld = owner->hotkeysRegistered_;
    UnregisterHotKey(nullptr, 1);
    UnregisterHotKey(nullptr, 2);
    const bool first = RegisterHotKey(nullptr, 1, modifiers(next), LOBYTE(next)) != FALSE;
    const bool second = first && RegisterHotKey(nullptr, 2, modifiers(previous), LOBYTE(previous)) != FALSE;
    if (!second) {
        UnregisterHotKey(nullptr, 1);
        UnregisterHotKey(nullptr, 2);
        bool restored = false;
        if (hadOld) {
            const bool a = RegisterHotKey(nullptr, 1, modifiers(owner->nextHotkey_), LOBYTE(owner->nextHotkey_)) != FALSE;
            const bool b = RegisterHotKey(nullptr, 2, modifiers(owner->previousHotkey_), LOBYTE(owner->previousHotkey_)) != FALSE;
            restored = a && b;
            if (!restored) { UnregisterHotKey(nullptr, 1); UnregisterHotKey(nullptr, 2); }
        }
        owner->hotkeysRegistered_ = restored;
        warn(L"快捷键被其他程序占用或不受系统支持，请更换组合。");
        return false;
    }
    owner->hotkeysRegistered_ = true;
    owner->nextHotkey_ = next;
    owner->previousHotkey_ = previous;
    for (auto* server : servers()) {
        if (!server->settingsWindow_) continue;
        SendDlgItemMessageW(server->settingsWindow_, NextHotkey, HKM_SETHOTKEY, next, 0);
        SendDlgItemMessageW(server->settingsWindow_, PreviousHotkey, HKM_SETHOTKEY, previous, 0);
    }
    return true;
}

void App::chooseTheme(bool background) {
    static COLORREF custom[16]{};
    auto& color = background ? background_ : foreground_;
    CHOOSECOLORW dialog{sizeof(dialog)};
    dialog.hwndOwner = settingsWindow_;
    dialog.rgbResult = colorRef(color);
    dialog.lpCustColors = custom;
    dialog.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&dialog)) return;
    const auto old = color;
    color = (std::uint32_t(GetRValue(dialog.rgbResult)) << 16) |
        (std::uint32_t(GetGValue(dialog.rgbResult)) << 8) | GetBValue(dialog.rgbResult);
    if ((local_ || !config_.host.empty()) && !saveServers()) color = old;
    discardGraphics();
    InvalidateRect(window_, nullptr, FALSE);
    if (settingsWindow_) RedrawWindow(settingsWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void App::dropFiles(HDROP drop) {
    std::vector<std::string> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count <= 100000) {
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length + 1, L'\0');
            if (!DragQueryFileW(drop, i, path.data(), length + 1)) continue;
            path.resize(length);
            paths.push_back(encoded(path));
        }
    }
    DragFinish(drop);
    if (!paths.empty()) uploadDropped(std::move(paths));
}

void App::uploadDropped(std::vector<std::string> paths) {
    if (local_) return;
    if (transfer_.active) {
        MessageBoxW(window_, L"当前窗口正在传输，请完成后再拖入文件。", L"AirSFTP", MB_OK);
        return;
    }
    if (config_.host.empty() || config_.username.empty() || remotePath_.empty() || remotePath_.front() != '/') {
        showSettings();
        MessageBoxW(settingsWindow_, L"请先保存服务器连接信息和远程文件夹，然后重新拖入文件。", L"AirSFTP", MB_OK);
        return;
    }
    transferLocalFolder_ = encoded(std::filesystem::u8path(paths.front()).parent_path().wstring());
    stopTransfer();
    const auto generation = transferGeneration_;
    transfer_ = {};
    transfer_.active = true;
    transfer_.message = "Connecting";
    transfer_.fileName = "Drop upload";
    InvalidateRect(window_, nullptr, FALSE);
    transferThread_ = std::jthread([this, config = config_, remote = remotePath_,
        paths = std::move(paths), generation, previous = std::move(transferThread_)](std::stop_token token) mutable {
        if (previous.joinable()) previous.join();
        if (token.stop_requested()) return;
        SshSession session(token);
        std::string error;
        if (!session.connect(config, error)) {
            TransferProgress failure;
            failure.failed = true;
            failure.message = error;
            postTransfer(failure, generation);
            return;
        }
        session.uploadPaths(paths, remote, [this, generation](const TransferProgress& value) {
            postTransfer(value, generation);
        }, error, [this, generation, token, remote, &paths](std::size_t count, std::uint64_t bytes) {
            return confirmTransfer(count, bytes, TransferDirection::Upload,
                paths.size() == 1 ? paths.front() : std::to_string(paths.size()) + " dropped items",
                remote, generation, token);
        });
    });
}

bool App::confirmTransfer(std::size_t count, std::uint64_t bytes, TransferDirection direction,
    const std::string& source, const std::string& destination, std::uintptr_t generation, std::stop_token token) {
    auto request = std::make_shared<TransferDecision>();
    request->count = count; request->bytes = bytes; request->direction = direction;
    request->source = source; request->destination = destination;
    auto payload = std::make_unique<std::shared_ptr<TransferDecision>>(request);
    {
        std::lock_guard lock(messageMutex_);
        if (!messageWindow_ || !PostMessageW(messageWindow_, WM_APP + 3, generation,
            reinterpret_cast<LPARAM>(payload.get()))) return false;
        payload.release();
    }
    while (request->result.load() == -1 && !token.stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return !token.stop_requested() && request->result.load() == 1;
}

void App::openLocalFolder() {
    if (local_) return;
    std::string path = transferLocalFolder_.empty() ? localPath_ : transferLocalFolder_;
    if (settingsWindow_ && IsWindowVisible(settingsWindow_)) {
        const auto edit = GetDlgItem(settingsWindow_, LocalEdit);
        std::wstring value(GetWindowTextLengthW(edit) + 1, L'\0');
        const int count = GetWindowTextW(edit, value.data(), int(value.size()));
        value.resize(count);
        if (!value.empty()) path = encoded(value);
    }
    std::error_code error;
    const auto folder = std::filesystem::u8path(path);
    if (path.empty() || !folder.is_absolute() || !std::filesystem::is_directory(folder, error)) {
        MessageBoxW(settingsWindow_ ? settingsWindow_ : window_, L"本地文件夹不存在，请先选择或创建文件夹。",
            L"AirSFTP", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        MessageBoxW(window_, L"无法打开本地文件夹。", L"AirSFTP", MB_OK | MB_ICONWARNING);
}
