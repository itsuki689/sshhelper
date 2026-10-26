#include "host_trust_ui.hpp"
#include <algorithm>

namespace {
constexpr wchar_t DispatcherClass[] = L"sshhelperHostTrustDispatcher";
constexpr wchar_t DialogClass[] = L"sshhelperHostTrustDialog";
std::wstring wide(const std::string& text) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), size);
    return result;
}
}
HostTrustUi::HostTrustUi(HINSTANCE instance) : instance_(instance) {
    WNDCLASSW cls{};
    cls.hInstance = instance; cls.lpfnWndProc = dispatchProc; cls.lpszClassName = DispatcherClass;
    RegisterClassW(&cls);
    cls.lpfnWndProc = dialogProc; cls.lpszClassName = DialogClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    dispatcher_ = CreateWindowExW(0, DispatcherClass, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, instance_, this);
    if (!dispatcher_ || !SetTimer(dispatcher_, 1, 50, nullptr)) shutdown();
}
HostTrustUi::~HostTrustUi() { shutdown(); if (dispatcher_) DestroyWindow(dispatcher_); }
HostTrustConfirmation HostTrustUi::confirmation(const void* owner) {
    return [this, owner](const HostKey& key, const auto& attempt) {
        return coordinator_.confirm(key, attempt, owner);
    };
}
void HostTrustUi::cancelOwner(const void* owner) { coordinator_.cancelOwner(owner); refresh(); }
void HostTrustUi::shutdown() {
    coordinator_.shutdown();
    if (dispatcher_) KillTimer(dispatcher_, 1);
    for (const auto& [id, dialog] : dialogs_) if (dialog->window) DestroyWindow(dialog->window);
    dialogs_.clear();
}
bool HostTrustUi::translate(MSG& message) {
    for (const auto& [id, dialog] : dialogs_)
        if (dialog->window && IsDialogMessageW(dialog->window, &message)) return true;
    return false;
}
LRESULT CALLBACK HostTrustUi::dispatchProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* ui = reinterpret_cast<HostTrustUi*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (ui && message == WM_TIMER) { ui->refresh(); return 0; }
    return DefWindowProcW(window, message, w, l);
}
void HostTrustUi::refresh() {
    const auto pending = coordinator_.pending();
    std::erase_if(dialogs_, [&](const auto& entry) {
        const bool remove = std::none_of(pending.begin(), pending.end(), [&](const auto& p) { return p->id == entry.first; });
        if (remove && entry.second->window) DestroyWindow(entry.second->window);
        return remove;
    });
    for (const auto& request : pending) {
        if (dialogs_.contains(request->id)) continue;
        auto dialog = std::make_unique<Dialog>(Dialog{this, request});
        const auto dpi = GetDpiForSystem();
        const auto p = [dpi](int n) { return MulDiv(n, dpi, 96); };
        const int offset = int(dialogs_.size() % 6) * p(24);
        RECT bounds{0, 0, p(620), p(326)};
        AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST, dpi);
        dialog->window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_CONTROLPARENT, DialogClass,
            L"首次 SSH 主机信任确认", WS_CAPTION | WS_SYSMENU,
            p(100) + offset, p(100) + offset, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance_, dialog.get());
        if (!dialog->window) { coordinator_.resolve(request, HostTrustResult::Cancelled); continue; }
        ShowWindow(dialog->window, SW_SHOWNORMAL);
        SetFocus(GetDlgItem(dialog->window, IDCANCEL));
        dialogs_.emplace(request->id, std::move(dialog));
    }
}
LRESULT CALLBACK HostTrustUi::dialogProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!dialog) return DefWindowProcW(window, message, w, l);
    if (message == WM_CREATE) {
        const auto dpi = GetDpiForWindow(window);
        const auto p = [dpi](int n) { return MulDiv(n, dpi, 96); };
        const auto& key = dialog->request->key;
        const auto text = L"目标主机：" + wide(key.host) + L"\r\n端口：" + std::to_wstring(key.port) +
            L"\r\n\r\n" + wide(displayHostFingerprint(key.fingerprint)) +
            L"\r\n\r\n请通过管理员或其他可信渠道核对上面的 SHA256 指纹。\r\n"
            L"只有核对一致后才接受；保存成功后才会发送登录凭据。\r\n"
            L"每次连接最多等待 120 秒。拒绝、取消或超时后需手动连接。";
        auto add = [&](const wchar_t* cls, const wchar_t* label, DWORD style,
                       int x, int y, int width, int height, int id) {
            auto control = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style,
                p(x), p(y), p(width), p(height), window, reinterpret_cast<HMENU>(INT_PTR(id)),
                dialog->ui->instance_, nullptr);
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        };
        add(L"EDIT", text.c_str(), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP,
            18, 16, 584, 248, 100);
        add(L"BUTTON", L"核对一致，接受", WS_TABSTOP, 196, 282, 150, 30, IDYES);
        add(L"BUTTON", L"拒绝", WS_TABSTOP, 358, 282, 112, 30, IDNO);
        add(L"BUTTON", L"取消", WS_TABSTOP | BS_DEFPUSHBUTTON, 482, 282, 112, 30, IDCANCEL);
        return 0;
    }
    if (message == WM_COMMAND && (LOWORD(w) == IDYES || LOWORD(w) == IDNO || LOWORD(w) == IDCANCEL)) {
        dialog->ui->coordinator_.resolve(dialog->request, LOWORD(w) == IDYES ? HostTrustResult::Accepted :
            LOWORD(w) == IDNO ? HostTrustResult::Rejected : HostTrustResult::Cancelled);
        // The next dispatcher tick destroys the window outside this callback.
        ShowWindow(window, SW_HIDE);
        return 0;
    }
    if (message == WM_CLOSE) { SendMessageW(window, WM_COMMAND, IDCANCEL, 0); return 0; }
    return DefWindowProcW(window, message, w, l);
}
