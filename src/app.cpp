#include "app.hpp"

#include "ssh_client.hpp"
#include "telemetry.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include <sstream>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t MainClass[] = L"AirSFTPFloatingWindow";
constexpr wchar_t SettingsClass[] = L"AirSFTPSettingsWindow";
constexpr UINT SnapshotMessage = WM_APP + 1;
constexpr UINT TransferMessage = WM_APP + 2;
constexpr int MainWidth = 380;
constexpr int MainHeight = 300;
constexpr int CompactWidth = 208;
constexpr int CompactHeight = 42;
constexpr int SideWidth = 44;
constexpr int SideHeight = 104;
constexpr int EdgeThreshold = 12;

int pixels(int dips, UINT dpi) {
    return MulDiv(dips, static_cast<int>(dpi), 96);
}

int dips(int value, UINT dpi) {
    return MulDiv(value, 96, static_cast<int>(dpi));
}

enum ControlId {
    HostEdit = 2001, PortEdit, UserEdit, PasswordEdit, KeyEdit,
    LocalEdit, RemoteEdit, BrowseLocal, SaveConnect, UploadButton, DownloadButton
};

template <typename T> void release(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring controlText(HWND parent, int id) {
    HWND control = GetDlgItem(parent, id);
    const int length = GetWindowTextLengthW(control);
    std::wstring value(length + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(length);
    return value;
}

std::wstring humanBytes(std::uint64_t value, bool perSecond = false) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double number = static_cast<double>(value);
    int unit = 0;
    while (number >= 1024.0 && unit < 4) { number /= 1024.0; ++unit; }
    wchar_t buffer[64]{};
    swprintf(buffer, 64, number >= 100.0 ? L"%.0f %ls%ls" : L"%.1f %ls%ls", number, units[unit], perSecond ? L"/s" : L"");
    return buffer;
}

std::wstring numberText(double value, const wchar_t* suffix = L"") {
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%.0f%ls", value, suffix);
    return buffer;
}

float ratio(std::uint64_t used, std::uint64_t total) {
    return total ? std::clamp(static_cast<float>(used) / static_cast<float>(total), 0.0f, 1.0f) : 0.0f;
}

void sleepStopped(std::stop_token token, std::chrono::milliseconds duration) {
    const auto end = std::chrono::steady_clock::now() + duration;
    while (!token.stop_requested() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

HWND addControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int width, int height, int id, HFONT font) {
    HWND control = CreateWindowExW(
        wcscmp(cls, WC_EDITW) == 0 ? WS_EX_CLIENTEDGE : 0, cls, text,
        WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

void drawText(ID2D1HwndRenderTarget* target, IDWriteTextFormat* format, ID2D1Brush* brush,
              const std::wstring& text, const D2D1_RECT_F& rect) {
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
}

App::App(HINSTANCE instance) : instance_(instance) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&writeFactory_));
    if (writeFactory_) {
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &titleFormat_);
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"zh-cn", &metricFormat_);
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn", &labelFormat_);
        if (labelFormat_) labelFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-cn", &compactFormat_);
        if (compactFormat_) {
            compactFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            compactFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    uiFont_ = CreateFontW(-pixels(15, GetDpiForSystem()), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

App::~App() {
    stopTransfer();
    stopMonitor();
    discardGraphics();
    release(titleFormat_); release(metricFormat_); release(labelFormat_); release(compactFormat_);
    release(writeFactory_); release(d2dFactory_);
    if (uiFont_) DeleteObject(uiFont_);
}

int App::run(int showCommand) {
    if (!registerClasses() || !createMainWindow(showCommand)) return 1;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

bool App::registerClasses() {
    WNDCLASSEXW mainClass{sizeof(mainClass)};
    mainClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    mainClass.lpfnWndProc = mainWindowProc;
    mainClass.hInstance = instance_;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hbrBackground = nullptr;
    mainClass.lpszClassName = MainClass;
    if (!RegisterClassExW(&mainClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW settingsClass{sizeof(settingsClass)};
    settingsClass.lpfnWndProc = settingsWindowProc;
    settingsClass.hInstance = instance_;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    settingsClass.lpszClassName = SettingsClass;
    return RegisterClassExW(&settingsClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool App::createMainWindow(int showCommand) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const int width = pixels(MainWidth, dpi);
    const int height = pixels(MainHeight, dpi);
    const int margin = pixels(24, dpi);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, MainClass, L"AirSFTP Monitor",
        WS_POPUP, work.right - width - margin, work.bottom - height - margin,
        width, height, nullptr, nullptr, instance_, this);
    if (!window_) return false;
    constexpr DWORD roundPreference = 2;
    constexpr auto cornerAttribute = static_cast<DWMWINDOWATTRIBUTE>(33);
    DwmSetWindowAttribute(window_, cornerAttribute, &roundPreference, sizeof(roundPreference));
    ShowWindow(window_, showCommand == SW_HIDE ? SW_SHOWNOACTIVATE : showCommand);
    UpdateWindow(window_);
    return true;
}

LRESULT CALLBACK App::mainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleMainMessage(window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleMainMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: paint(); return 0;
    case WM_SIZE: resizeGraphics(); return 0;
    case WM_ENTERSIZEMOVE:
        if (!isCollapsed()) {
            GetWindowRect(window_, &expandedRect_);
            hasExpandedRect_ = true;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        handleEdgeDock();
        return 0;
    case WM_NCHITTEST: {
        if (isCollapsed()) return HTCLIENT;
        const auto result = DefWindowProcW(window, message, wParam, lParam);
        if (result == HTCLIENT) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const UINT dpi = GetDpiForWindow(window);
            if (dips(point.y, dpi) < 48 && dips(point.x, dpi) < MainWidth - 120) return HTCAPTION;
        }
        return result;
    }
    case WM_LBUTTONDBLCLK:
        if (isCollapsed()) restoreExpanded();
        return 0;
    case WM_NCLBUTTONDBLCLK:
        if (wParam == HTCAPTION) toggleCollapsed();
        return 0;
    case WM_LBUTTONUP: {
        if (isCollapsed()) {
            restoreExpanded();
            return 0;
        }
        const UINT dpi = GetDpiForWindow(window);
        const int x = dips(GET_X_LPARAM(lParam), dpi), y = dips(GET_Y_LPARAM(lParam), dpi);
        if (y < 48 && x > MainWidth - 38) DestroyWindow(window_);
        else if (y < 48 && x > MainWidth - 112) showSettings();
        return 0;
    }
    case SnapshotMessage: {
        std::unique_ptr<TelemetrySnapshot> value(reinterpret_cast<TelemetrySnapshot*>(lParam));
        snapshot_ = std::move(*value);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case TransferMessage: {
        std::unique_ptr<TransferProgress> value(reinterpret_cast<TransferProgress*>(lParam));
        transfer_ = std::move(*value);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        window_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool App::ensureGraphics() {
    if (renderTarget_) return true;
    if (!d2dFactory_ || !writeFactory_ || !window_) return false;
    RECT rect{}; GetClientRect(window_, &rect);
    d2dFactory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(rect.right, rect.bottom)), &renderTarget_);
    if (!renderTarget_) return false;
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x1D1D1F), &textBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x74747B), &mutedBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x087FF5), &accentBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF), &surfaceBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xE4E4E8), &lineBrush_);
    return true;
}

void App::discardGraphics() {
    release(textBrush_); release(mutedBrush_); release(accentBrush_); release(surfaceBrush_); release(lineBrush_);
    release(renderTarget_);
}

void App::resizeGraphics() {
    if (!renderTarget_) return;
    RECT rect{}; GetClientRect(window_, &rect);
    renderTarget_->Resize(D2D1::SizeU(rect.right, rect.bottom));
}

void App::paint() {
    PAINTSTRUCT paint{};
    HDC paintDc = BeginPaint(window_, &paint);
    RECT client{}; GetClientRect(window_, &client);
    FillRect(paintDc, &client, GetSysColorBrush(COLOR_WINDOW));
    if (!ensureGraphics()) {
        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, RGB(29, 29, 31));
        const wchar_t* message = L"AirSFTP graphics initialization failed";
        TextOutW(paintDc, 18, 18, message, static_cast<int>(wcslen(message)));
        EndPaint(window_, &paint);
        return;
    }
    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(0xF6F6F8));

    const bool horizontal = collapseMode_ == CollapseMode::Horizontal;
    const bool side = collapseMode_ == CollapseMode::Side;
    const float layoutWidth = side ? static_cast<float>(SideWidth) :
        (horizontal ? static_cast<float>(CompactWidth) : static_cast<float>(MainWidth));
    const float layoutHeight = side ? static_cast<float>(SideHeight) :
        (horizontal ? static_cast<float>(CompactHeight) : static_cast<float>(MainHeight));
    const float corner = isCollapsed() ? 18.0f : 14.0f;
    const auto surface = D2D1::RoundedRect(D2D1::RectF(0, 0, layoutWidth, layoutHeight), corner, corner);
    renderTarget_->FillRoundedRectangle(surface, surfaceBrush_);
    renderTarget_->DrawRoundedRectangle(surface, lineBrush_, 1);

    const D2D1_COLOR_F statusColor = snapshot_.connected ? D2D1::ColorF(0x28A745) : D2D1::ColorF(0x9B9BA1);
    ID2D1SolidColorBrush* statusBrush = nullptr;
    renderTarget_->CreateSolidColorBrush(statusColor, &statusBrush);
    const std::wstring latency = snapshot_.connected ? numberText(snapshot_.sshLatencyMs) : L"—";
    IDWriteTextFormat* compact = compactFormat_ ? compactFormat_ : labelFormat_;

    if (side) {
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(22, 20), 4, 4), statusBrush);
        drawText(renderTarget_, compact, textBrush_, latency, D2D1::RectF(2, 42, 42, 68));
        drawText(renderTarget_, compact, mutedBrush_, L"ms", D2D1::RectF(2, 65, 42, 89));
    } else if (horizontal) {
        drawText(renderTarget_, titleFormat_, textBrush_, L"AirSFTP", D2D1::RectF(12, 7, 98, 35));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(109, 21), 4, 4), statusBrush);
        drawText(renderTarget_, compact, mutedBrush_, latency + L" ms", D2D1::RectF(120, 5, 201, 37));
    } else {
        drawText(renderTarget_, titleFormat_, textBrush_, L"AirSFTP", D2D1::RectF(18, 13, 130, 42));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(150, 27), 4, 4), statusBrush);
        drawText(renderTarget_, labelFormat_, mutedBrush_, wide(snapshot_.status), D2D1::RectF(160, 14, 265, 40));
        drawText(renderTarget_, labelFormat_, accentBrush_, L"设置", D2D1::RectF(286, 14, 342, 40));
        drawText(renderTarget_, titleFormat_, mutedBrush_, L"×", D2D1::RectF(348, 9, 378, 42));
        renderTarget_->DrawLine(D2D1::Point2F(0, 49), D2D1::Point2F(MainWidth, 49), lineBrush_, 1);
    }
    release(statusBrush);

    if (collapseMode_ == CollapseMode::Expanded) {
        auto card = [&](float x, float y, float width, const std::wstring& label, const std::wstring& value, float level) {
            const auto box = D2D1::RoundedRect(D2D1::RectF(x, y, x + width, y + 66), 11, 11);
            renderTarget_->FillRoundedRectangle(box, surfaceBrush_);
            renderTarget_->DrawRoundedRectangle(box, lineBrush_, 1);
            drawText(renderTarget_, labelFormat_, mutedBrush_, label, D2D1::RectF(x + 11, y + 8, x + width - 8, y + 27));
            drawText(renderTarget_, metricFormat_, textBrush_, value, D2D1::RectF(x + 10, y + 26, x + width - 8, y + 57));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 10, y + 58, x + width - 10, y + 61), 2, 2), lineBrush_);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 10, y + 58, x + 10 + (width - 20) * std::clamp(level, 0.0f, 1.0f), y + 61), 2, 2), accentBrush_);
        };

        std::wstring cpu = snapshot_.packageTemperature >= 0 ? numberText(snapshot_.packageTemperature, L"°C") : L"—";
        std::wstring gpu = !snapshot_.gpus.empty() && snapshot_.gpus[0].temperature >= 0 ? numberText(snapshot_.gpus[0].temperature, L"°C") : L"—";
        card(14, 62, 82, L"CPU 封装", cpu, snapshot_.packageTemperature > 0 ? static_cast<float>(snapshot_.packageTemperature / 100.0) : 0);
        card(104, 62, 82, L"GPU", gpu, !snapshot_.gpus.empty() ? static_cast<float>(snapshot_.gpus[0].temperature / 100.0) : 0);
        card(194, 62, 82, L"内存", snapshot_.memoryTotal ? numberText(ratio(snapshot_.memoryUsed, snapshot_.memoryTotal) * 100, L"%") : L"—", ratio(snapshot_.memoryUsed, snapshot_.memoryTotal));
        card(284, 62, 82, L"存储", snapshot_.diskTotal ? numberText(ratio(snapshot_.diskUsed, snapshot_.diskTotal) * 100, L"%") : L"—", ratio(snapshot_.diskUsed, snapshot_.diskTotal));

        const auto sampleSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            snapshot_.sampledAt.time_since_epoch()).count();
        std::wstring cores = L"核心 ";
        if (snapshot_.coreTemperatures.empty()) {
            cores += L"暂无各核传感器";
        } else {
            constexpr std::size_t pageSize = 4;
            const std::size_t count = snapshot_.coreTemperatures.size();
            const std::size_t pages = (count + pageSize - 1) / pageSize;
            const std::size_t begin = static_cast<std::size_t>((sampleSeconds / 6) % pages) * pageSize;
            const std::size_t end = std::min(begin + pageSize, count);
            if (pages > 1)
                cores += std::to_wstring(begin + 1) + L"-" + std::to_wstring(end) + L"/" + std::to_wstring(count) + L"  ";
            for (std::size_t i = begin; i < end; ++i) {
                if (i != begin) cores += L"  ";
                cores += wide(snapshot_.coreTemperatures[i].label) + L" " + numberText(snapshot_.coreTemperatures[i].value, L"°");
            }
        }
        drawText(renderTarget_, labelFormat_, mutedBrush_, cores, D2D1::RectF(18, 140, 362, 162));

        std::wstring fan = L"风扇 ";
        if (!snapshot_.fans.empty()) {
            const auto index = static_cast<std::size_t>((sampleSeconds / 6) % snapshot_.fans.size());
            fan += wide(snapshot_.fans[index].label) + L" " + numberText(snapshot_.fans[index].value, L" RPM");
        } else {
            fan += L"—";
        }
        if (!snapshot_.gpus.empty()) {
            const auto& gpuFan = snapshot_.gpus[0];
            if (gpuFan.fanRpm > 0)
                fan += L"    GPU " + numberText(gpuFan.fanRpm, L" RPM");
            else if (gpuFan.fanPercent > 0)
                fan += L"    GPU " + numberText(gpuFan.fanPercent, L"%");
        }
        drawText(renderTarget_, labelFormat_, mutedBrush_, fan, D2D1::RectF(18, 164, 260, 186));
        drawText(renderTarget_, labelFormat_, mutedBrush_, L"SSH " + numberText(snapshot_.sshLatencyMs, L" ms"), D2D1::RectF(270, 164, 360, 186));

        renderTarget_->DrawLine(D2D1::Point2F(14, 194), D2D1::Point2F(366, 194), lineBrush_, 1);
        drawText(renderTarget_, titleFormat_, textBrush_, L"↓ " + humanBytes(snapshot_.networkDown, true), D2D1::RectF(18, 204, 180, 232));
        drawText(renderTarget_, titleFormat_, textBrush_, L"↑ " + humanBytes(snapshot_.networkUp, true), D2D1::RectF(198, 204, 360, 232));

        if (transfer_.active || transfer_.completed || transfer_.failed) {
            const float progressValue = ratio(transfer_.transferred, transfer_.total);
            drawText(renderTarget_, labelFormat_, textBrush_, wide(transfer_.fileName), D2D1::RectF(18, 241, 210, 262));
            drawText(renderTarget_, labelFormat_, mutedBrush_, transfer_.active ? humanBytes(transfer_.bytesPerSecond, true) : wide(transfer_.message), D2D1::RectF(220, 241, 360, 262));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, 270, 362, 275), 3, 3), lineBrush_);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, 270, 18 + 344 * progressValue, 275), 3, 3), accentBrush_);
        } else {
            drawText(renderTarget_, labelFormat_, mutedBrush_, L"双击标题或拖到屏幕边缘收起 · 设置中可传输", D2D1::RectF(18, 246, 362, 274));
        }
    }
    const HRESULT result = renderTarget_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discardGraphics();
    EndPaint(window_, &paint);
}

void App::toggleCollapsed() {
    if (isCollapsed()) restoreExpanded();
    else collapseHorizontal(false);
}

void App::collapseHorizontal(bool dockToTop) {
    if (isCollapsed()) return;
    RECT current{}; GetWindowRect(window_, &current);
    if (!dockToTop || !hasExpandedRect_) {
        expandedRect_ = current;
        hasExpandedRect_ = true;
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = pixels(CompactWidth, dpi);
    const int height = pixels(CompactHeight, dpi);
    const int desiredX = current.left + ((current.right - current.left) - width) / 2;
    const int desiredY = dockToTop ? static_cast<int>(monitor.rcWork.top) : current.top;
    const int x = std::clamp(desiredX, static_cast<int>(monitor.rcWork.left),
        static_cast<int>(monitor.rcWork.right) - width);
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    collapseMode_ = CollapseMode::Horizontal;
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::collapseSide(bool left) {
    if (isCollapsed()) return;
    RECT current{}; GetWindowRect(window_, &current);
    if (!hasExpandedRect_) {
        expandedRect_ = current;
        hasExpandedRect_ = true;
    }
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = pixels(SideWidth, dpi);
    const int height = pixels(SideHeight, dpi);
    const int x = left ? static_cast<int>(monitor.rcWork.left) :
        static_cast<int>(monitor.rcWork.right) - width;
    const int desiredY = current.top + ((current.bottom - current.top) - height) / 2;
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    collapseMode_ = CollapseMode::Side;
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::restoreExpanded() {
    if (!isCollapsed()) return;
    RECT target = expandedRect_;
    if (!hasExpandedRect_) {
        GetWindowRect(window_, &target);
        const UINT dpi = GetDpiForWindow(window_);
        target.right = target.left + pixels(MainWidth, dpi);
        target.bottom = target.top + pixels(MainHeight, dpi);
    }
    collapseMode_ = CollapseMode::Expanded;
    SetWindowPos(window_, HWND_TOPMOST, target.left, target.top, target.right - target.left,
        target.bottom - target.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    hasExpandedRect_ = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::handleEdgeDock() {
    if (isCollapsed()) return;
    RECT rect{}; GetWindowRect(window_, &rect);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const int threshold = pixels(EdgeThreshold, GetDpiForWindow(window_));
    const int topDistance = std::abs(rect.top - static_cast<int>(monitor.rcWork.top));
    const int leftDistance = std::abs(rect.left - static_cast<int>(monitor.rcWork.left));
    const int rightDistance = std::abs(rect.right - static_cast<int>(monitor.rcWork.right));
    const int nearest = std::min({topDistance, leftDistance, rightDistance});
    if (nearest > threshold) {
        hasExpandedRect_ = false;
        return;
    }
    if (leftDistance == nearest) collapseSide(true);
    else if (rightDistance == nearest) collapseSide(false);
    else collapseHorizontal(true);
}

void App::createSettingsWindow() {
    const UINT dpi = GetDpiForSystem();
    auto p = [dpi](int value) { return pixels(value, dpi); };
    settingsWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, SettingsClass, L"AirSFTP 设置",
        WS_CAPTION | WS_SYSMENU | WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, p(500), p(455),
        window_, nullptr, instance_, this);
    if (!settingsWindow_) return;
    const int lx = p(22), ex = p(132), ew = p(330);
    auto label = [&](const wchar_t* text, int y) {
        addControl(settingsWindow_, WC_STATICW, text, 0, lx, p(y + 4), p(100), p(24), 0, uiFont_);
    };
    label(L"主机", 20); addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, ex, p(20), p(240), p(27), HostEdit, uiFont_);
    label(L"端口", 57); addControl(settingsWindow_, WC_EDITW, L"22", ES_NUMBER, ex, p(57), p(80), p(27), PortEdit, uiFont_);
    label(L"用户名", 94); addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, ex, p(94), p(240), p(27), UserEdit, uiFont_);
    label(L"密码/密钥口令", 131); addControl(settingsWindow_, WC_EDITW, L"", ES_PASSWORD | ES_AUTOHSCROLL, ex, p(131), ew, p(27), PasswordEdit, uiFont_);
    label(L"私钥文件", 168); addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, ex, p(168), ew, p(27), KeyEdit, uiFont_);
    label(L"本地文件", 215); addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, ex, p(215), p(270), p(27), LocalEdit, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"浏览", BS_PUSHBUTTON, p(408), p(215), p(54), p(27), BrowseLocal, uiFont_);
    label(L"远程文件", 252); addControl(settingsWindow_, WC_EDITW, L"/tmp/", ES_AUTOHSCROLL, ex, p(252), ew, p(27), RemoteEdit, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"保存并监控", BS_DEFPUSHBUTTON, p(132), p(310), p(110), p(34), SaveConnect, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"上传", BS_PUSHBUTTON, p(252), p(310), p(90), p(34), UploadButton, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"下载", BS_PUSHBUTTON, p(352), p(310), p(90), p(34), DownloadButton, uiFont_);
    addControl(settingsWindow_, WC_STATICW, L"密码不会保存；选择私钥时密码框作为密钥口令。远端需 Linux + lm-sensors。",
        0, p(22), p(372), p(440), p(24), 0, uiFont_);
}

void App::showSettings() {
    if (!settingsWindow_) createSettingsWindow();
    if (!settingsWindow_) return;
    SetWindowTextW(GetDlgItem(settingsWindow_, HostEdit), wide(config_.host).c_str());
    SetWindowTextW(GetDlgItem(settingsWindow_, PortEdit), std::to_wstring(config_.port).c_str());
    SetWindowTextW(GetDlgItem(settingsWindow_, UserEdit), wide(config_.username).c_str());
    SetWindowTextW(GetDlgItem(settingsWindow_, KeyEdit), wide(config_.privateKeyPath).c_str());
    SetWindowTextW(GetDlgItem(settingsWindow_, LocalEdit), wide(localPath_).c_str());
    SetWindowTextW(GetDlgItem(settingsWindow_, RemoteEdit), wide(remotePath_.empty() ? "/tmp/" : remotePath_).c_str());
    RECT owner{}, dialog{}; GetWindowRect(window_, &owner); GetWindowRect(settingsWindow_, &dialog);
    const UINT dpi = GetDpiForWindow(settingsWindow_);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const int dialogWidth = dialog.right - dialog.left;
    const int dialogHeight = dialog.bottom - dialog.top;
    const int desiredX = owner.left - (dialogWidth - pixels(MainWidth, dpi)) / 2;
    const int desiredY = owner.top - pixels(60, dpi);
    const int x = std::clamp(desiredX, static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - dialogWidth);
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - dialogHeight);
    SetWindowPos(settingsWindow_, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(settingsWindow_);
}

LRESULT CALLBACK App::settingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handleSettingsMessage(window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::handleSettingsMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case BrowseLocal: chooseLocalFile(false); return 0;
        case SaveConnect: {
            ConnectionConfig config; std::string local, remote;
            if (readSettings(config, local, remote)) {
                config_ = config; localPath_ = local; remotePath_ = remote;
                startMonitor(config_); ShowWindow(settingsWindow_, SW_HIDE);
            }
            return 0;
        }
        case UploadButton: startTransfer(TransferDirection::Upload); return 0;
        case DownloadButton: startTransfer(TransferDirection::Download); return 0;
        }
        break;
    case WM_CLOSE: ShowWindow(window, SW_HIDE); return 0;
    case WM_DESTROY: settingsWindow_ = nullptr; return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool App::readSettings(ConnectionConfig& config, std::string& localPath, std::string& remotePath) {
    config.host = utf8(controlText(settingsWindow_, HostEdit));
    config.username = utf8(controlText(settingsWindow_, UserEdit));
    config.password = utf8(controlText(settingsWindow_, PasswordEdit));
    config.privateKeyPath = utf8(controlText(settingsWindow_, KeyEdit));
    config.keyPassphrase = config.privateKeyPath.empty() ? std::string{} : config.password;
    localPath = utf8(controlText(settingsWindow_, LocalEdit));
    remotePath = utf8(controlText(settingsWindow_, RemoteEdit));
    try { config.port = std::stoi(controlText(settingsWindow_, PortEdit)); } catch (...) { config.port = 0; }
    if (config.host.empty() || config.username.empty() || config.port < 1 || config.port > 65535 ||
        (config.password.empty() && config.privateKeyPath.empty())) {
        MessageBoxW(settingsWindow_, L"请填写主机、端口、用户名，并提供密码或私钥。", L"AirSFTP", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void App::chooseLocalFile(bool saveDialog) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = settingsWindow_;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = L"所有文件\0*.*\0";
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (saveDialog ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if ((saveDialog ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)))
        SetWindowTextW(GetDlgItem(settingsWindow_, LocalEdit), path);
}

void App::postSnapshot(TelemetrySnapshot snapshot) {
    auto* value = new TelemetrySnapshot(std::move(snapshot));
    if (!window_ || !PostMessageW(window_, SnapshotMessage, 0, reinterpret_cast<LPARAM>(value))) delete value;
}

void App::postTransfer(TransferProgress progress) {
    auto* value = new TransferProgress(std::move(progress));
    if (!window_ || !PostMessageW(window_, TransferMessage, 0, reinterpret_cast<LPARAM>(value))) delete value;
}

void App::startMonitor(const ConnectionConfig& config) {
    stopMonitor();
    TelemetrySnapshot connecting;
    connecting.status = "Connecting";
    postSnapshot(connecting);
    monitorThread_ = std::jthread([this, config](std::stop_token token) {
        TelemetryParser parser;
        while (!token.stop_requested()) {
            SshSession session;
            std::string error;
            if (!session.connect(config, error)) {
                TelemetrySnapshot failed; failed.status = error;
                postSnapshot(std::move(failed));
                sleepStopped(token, std::chrono::seconds(3));
                continue;
            }
            while (!token.stop_requested()) {
                std::string output; double latency = 0.0;
                if (!session.executeScript(TelemetryParser::probeScript(), output, latency, error)) {
                    TelemetrySnapshot failed; failed.status = error;
                    postSnapshot(std::move(failed));
                    break;
                }
                auto value = parser.parse(output, latency);
                value.fingerprint = session.fingerprint();
                postSnapshot(std::move(value));
                sleepStopped(token, std::chrono::seconds(2));
            }
            session.disconnect();
            sleepStopped(token, std::chrono::seconds(2));
        }
    });
}

void App::stopMonitor() {
    if (monitorThread_.joinable()) {
        monitorThread_.request_stop();
        monitorThread_.join();
    }
}

void App::startTransfer(TransferDirection direction) {
    ConnectionConfig config; std::string local, remote;
    if (!readSettings(config, local, remote)) return;
    if (local.empty() || remote.empty()) {
        MessageBoxW(settingsWindow_, L"请填写本地和远程文件路径。", L"AirSFTP", MB_OK | MB_ICONWARNING);
        return;
    }
    config_ = config; localPath_ = local; remotePath_ = remote;
    startMonitor(config_);
    stopTransfer();
    transferCancelled_.store(false);
    ShowWindow(settingsWindow_, SW_HIDE);
    transferThread_ = std::jthread([this, config, local, remote, direction](std::stop_token) {
        SshSession session;
        std::string error;
        TransferProgress state; state.active = true; state.direction = direction;
        state.fileName = std::filesystem::u8path(direction == TransferDirection::Upload ? local : remote).filename().string();
        state.message = "Connecting"; postTransfer(state);
        if (!session.connect(config, error)) {
            state.active = false; state.failed = true; state.message = error; postTransfer(state); return;
        }
        session.transfer(direction, local, remote,
            [this](const TransferProgress& value) { postTransfer(value); }, transferCancelled_, error);
    });
}

void App::stopTransfer() {
    transferCancelled_.store(true);
    if (transferThread_.joinable()) transferThread_.join();
}
