#include "transfer_picker.hpp"
#include "ssh_client.hpp"
#include <commctrl.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>

namespace {
constexpr UINT ReadyMessage = WM_APP + 30;
constexpr int Items = 100, Info = 101, All = 102, None = 103;
std::wstring wide(const std::string& value) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
struct Picker {
    HostTrustConfirmation confirm;
    std::filesystem::path pins;
    ConnectionConfig config;
    TransferDirection direction;
    std::string folder;
    std::vector<TransferEntry> entries;
    std::vector<std::string> selected;
    std::string error;
    std::mutex mutex;
    HWND window = nullptr;
    HFONT font = nullptr;
    std::jthread scanner;
    bool ready = false;
    bool updatingChecks = false;
};
void layout(HWND window, Picker& state) {
    const UINT dpi = GetDpiForWindow(window);
    const auto p = [dpi](int n) { return MulDiv(n, dpi, 96); };
    RECT client{}; GetClientRect(window, &client);
    const int width = client.right, height = client.bottom;
    MoveWindow(GetDlgItem(window, Info), p(16), p(12), width - p(32), p(46), TRUE);
    MoveWindow(GetDlgItem(window, Items), p(16), p(62), width - p(32), std::max(p(60), height - p(118)), TRUE);
    MoveWindow(GetDlgItem(window, All), p(16), height - p(42), p(76), p(28), TRUE);
    MoveWindow(GetDlgItem(window, None), p(100), height - p(42), p(86), p(28), TRUE);
    MoveWindow(GetDlgItem(window, IDOK), width - p(210), height - p(42), p(108), p(28), TRUE);
    MoveWindow(GetDlgItem(window, IDCANCEL), width - p(94), height - p(42), p(78), p(28), TRUE);
    ListView_SetColumnWidth(GetDlgItem(window, Items), 0, std::max(p(140), width - p(180)));
    ListView_SetColumnWidth(GetDlgItem(window, Items), 1, p(118));
    HFONT old = state.font;
    state.font = CreateFontW(-p(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Microsoft YaHei UI");
    for (const int id : {Info, Items, All, None, IDOK, IDCANCEL})
        SendDlgItemMessageW(window, id, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    if (old) DeleteObject(old);
}
INT_PTR CALLBACK dialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<Picker*>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<Picker*>(lParam);
        SetWindowLongPtrW(window, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        state->window = window;
        SetWindowTextW(window, state->direction == TransferDirection::Upload ? L"选择上传内容" : L"选择下载内容");
        auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        };
        add(L"STATIC", L"正在读取源文件夹… 可随时取消", SS_LEFT, Info);
        HWND list = add(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS, Items);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.cx = 400;
        column.pszText = const_cast<wchar_t*>(L"相对路径（勾选目录会选中其内容）");
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<wchar_t*>(L"类型 / 大小");
        ListView_InsertColumn(list, 1, &column);
        add(L"BUTTON", L"全选", WS_TABSTOP | BS_PUSHBUTTON, All);
        add(L"BUTTON", L"全不选", WS_TABSTOP | BS_PUSHBUTTON, None);
        add(L"BUTTON", state->direction == TransferDirection::Upload ? L"开始上传" : L"开始下载",
            WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
        add(L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
        EnableWindow(GetDlgItem(window, IDOK), FALSE);
        EnableWindow(GetDlgItem(window, All), FALSE);
        EnableWindow(GetDlgItem(window, None), FALSE);
        const UINT dpi = GetDpiForWindow(window);
        RECT parent{}; GetWindowRect(GetParent(window), &parent);
        const int width = MulDiv(640, dpi, 96), height = MulDiv(480, dpi, 96);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(GetParent(window), MONITOR_DEFAULTTONEAREST), &monitor);
        const int x = std::clamp(static_cast<int>(parent.left), static_cast<int>(monitor.rcWork.left),
            std::max(static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width));
        const int y = std::clamp(static_cast<int>(parent.top), static_cast<int>(monitor.rcWork.top),
            std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height));
        SetWindowPos(window, nullptr, x, y, width, height, SWP_NOZORDER);
        layout(window, *state);
        state->scanner = std::jthread([state](std::stop_token stop) {
            SshSession session(stop, 15000, state->pins, state->confirm);
            std::vector<TransferEntry> entries;
            std::string error;
            if (state->direction == TransferDirection::Upload || session.connect(state->config, error))
                session.listFolder(state->direction, state->folder, entries, error);
            std::lock_guard lock(state->mutex);
            state->entries = std::move(entries);
            state->error = std::move(error);
            if (state->window) PostMessageW(state->window, ReadyMessage, 0, 0);
        });
        return TRUE;
    }
    if (!state) return FALSE;
    switch (message) {
    case ReadyMessage: {
        std::lock_guard lock(state->mutex);
        state->ready = true;
        if (!state->error.empty()) {
            SetDlgItemTextW(window, Info, wide("Cannot list folder: " + state->error).c_str());
            return TRUE;
        }
        const auto note = wide(state->folder) + L"\n保留目录结构；同名文件覆盖；不跟随符号链接。";
        SetDlgItemTextW(window, Info, state->entries.empty() ? L"源文件夹为空，可使用“整个文件夹”模式传输空目录。" : note.c_str());
        HWND list = GetDlgItem(window, Items);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        state->updatingChecks = true;
        for (std::size_t i = 0; i < state->entries.size(); ++i) {
            const auto& entry = state->entries[i];
            auto name = wide(entry.relativePath);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = name.data();
            ListView_InsertItem(list, &item);
            auto size = entry.directory ? std::wstring(L"文件夹") : std::to_wstring(entry.size) + L" B";
            ListView_SetItemText(list, item.iItem, 1, size.data());
        }
        state->updatingChecks = false;
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list, nullptr, TRUE);
        for (const int id : {All, None, IDOK}) EnableWindow(GetDlgItem(window, id), !state->entries.empty());
        return TRUE;
    }
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom != Items || header->code != LVN_ITEMCHANGED || state->updatingChecks || !state->ready)
            break;
        const auto* change = reinterpret_cast<NMLISTVIEW*>(lParam);
        if (!(change->uChanged & LVIF_STATE) ||
            ((change->uOldState ^ change->uNewState) & LVIS_STATEIMAGEMASK) == 0 ||
            change->iItem < 0 || static_cast<std::size_t>(change->iItem) >= state->entries.size()) break;
        const auto& entry = state->entries[change->iItem];
        if (!entry.directory) break;
        state->updatingChecks = true;
        const bool checked = ListView_GetCheckState(header->hwndFrom, change->iItem);
        const auto prefix = entry.relativePath + "/";
        for (std::size_t i = 0; i < state->entries.size(); ++i)
            if (state->entries[i].relativePath.starts_with(prefix))
                ListView_SetCheckState(header->hwndFrom, static_cast<int>(i), checked);
        state->updatingChecks = false;
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == All || LOWORD(wParam) == None) {
            state->updatingChecks = true;
            HWND list = GetDlgItem(window, Items);
            for (int i = 0; i < ListView_GetItemCount(list); ++i)
                ListView_SetCheckState(list, i, LOWORD(wParam) == All);
            state->updatingChecks = false;
            return TRUE;
        }
        if (LOWORD(wParam) == IDOK && state->ready && state->error.empty()) {
            state->selected.clear();
            for (std::size_t i = 0; i < state->entries.size(); ++i)
                if (ListView_GetCheckState(GetDlgItem(window, Items), static_cast<int>(i)))
                    state->selected.push_back(state->entries[i].relativePath);
            if (state->selected.empty()) {
                MessageBoxW(window, L"请至少勾选一个文件或文件夹。", L"AirSFTP", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            EndDialog(window, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            state->scanner.request_stop();
            EndDialog(window, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        state->scanner.request_stop();
        EndDialog(window, IDCANCEL);
        return TRUE;
    case WM_SIZE:
        layout(window, *state);
        return TRUE;
    case WM_DPICHANGED: {
        const auto* rect = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
            rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(window, *state);
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window);
        limits->ptMinTrackSize = {MulDiv(480, dpi, 96), MulDiv(300, dpi, 96)};
        return TRUE;
    }
    case WM_DESTROY:
        {
            std::lock_guard lock(state->mutex);
            state->window = nullptr;
        }
        state->scanner.request_stop();
        if (state->font) { DeleteObject(state->font); state->font = nullptr; }
        return TRUE;
    }
    return FALSE;
}
}

bool chooseTransferEntries(HWND owner, const ConnectionConfig& config, TransferDirection direction,
                           const std::string& sourceFolder, std::vector<std::string>& selected,
                           HostTrustConfirmation confirm, std::filesystem::path pins) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    Picker state;
    state.confirm = std::move(confirm);
    state.pins = std::move(pins);
    state.config = config;
    state.direction = direction;
    state.folder = sourceFolder;
    DLGTEMPLATE layout{};
    layout.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | DS_MODALFRAME;
    layout.dwExtendedStyle = WS_EX_CONTROLPARENT;
    layout.cx = 360; layout.cy = 260;
    // Header followed by empty menu, default dialog class and empty title.
    alignas(DWORD) std::array<std::byte, sizeof(DLGTEMPLATE) + 3 * sizeof(WORD)> bytes{};
    std::memcpy(bytes.data(), &layout, sizeof(layout));
    const auto result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
        reinterpret_cast<DLGTEMPLATE*>(bytes.data()), owner, dialogProc, reinterpret_cast<LPARAM>(&state));
    state.scanner.request_stop();
    if (state.scanner.joinable()) state.scanner.join();
    if (result == -1) MessageBoxW(owner, L"无法打开文件选择窗口。", L"AirSFTP", MB_OK | MB_ICONERROR);
    if (result != IDOK) return false;
    selected = std::move(state.selected);
    return true;
}
