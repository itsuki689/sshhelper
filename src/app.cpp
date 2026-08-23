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
constexpr int MainHeight = 270;
constexpr int CompactWidth = 176;
constexpr int CompactHeight = 36;
constexpr int SideWidth = 44;
constexpr int SideHeight = 104;
constexpr int EdgeThreshold = 20;
constexpr int CompactSnapThreshold = 36;
constexpr int RestoreGap = 8;
constexpr UINT_PTR AnimationTimerId = 1;
constexpr UINT AnimationFrameMs = 15;
constexpr ULONGLONG AnimationDurationMs = 180;
constexpr int SettingsWidth = 480;
constexpr int SettingsHeight = 456;
constexpr int SettingsHeaderHeight = 52;

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
    DWORD windowStyle = WS_CHILD | WS_VISIBLE | style;
    if (wcscmp(cls, WC_EDITW) == 0 || wcscmp(cls, WC_BUTTONW) == 0)
        windowStyle |= WS_TABSTOP;
    HWND control = CreateWindowExW(0, cls, text, windowStyle, x, y, width, height, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

void drawText(ID2D1HwndRenderTarget* target, IDWriteTextFormat* format, ID2D1Brush* brush,
              const std::wstring& text, const D2D1_RECT_F& rect) {
    target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void drawGearIcon(ID2D1HwndRenderTarget* target, ID2D1Factory* factory,
                  ID2D1Brush* foreground, ID2D1Brush* background, D2D1_POINT_2F center) {
    ID2D1PathGeometry* geometry = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) {
        release(sink);
        release(geometry);
        return;
    }
    constexpr int points = 32;
    constexpr float pi = 3.14159265f;
    for (int index = 0; index < points; ++index) {
        const float angle = -pi / 2.0f + 2.0f * pi * static_cast<float>(index) / points;
        const int phase = index % 4;
        const float radius = (phase == 1 || phase == 2) ? 9.5f : 7.1f;
        const auto point = D2D1::Point2F(center.x + std::cos(angle) * radius,
            center.y + std::sin(angle) * radius);
        if (index == 0) sink->BeginFigure(point, D2D1_FIGURE_BEGIN_FILLED);
        else sink->AddLine(point);
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    target->FillGeometry(geometry, foreground);
    target->FillEllipse(D2D1::Ellipse(center, 2.8f, 2.8f), background);
    release(sink);
    release(geometry);
}
}

App::App(HINSTANCE instance) : instance_(instance) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&writeFactory_));
    if (writeFactory_) {
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &titleFormat_);
        if (titleFormat_) titleFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
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
}

App::~App() {
    stopTransfer();
    stopMonitor();
    discardGraphics();
    release(titleFormat_); release(metricFormat_); release(labelFormat_); release(compactFormat_);
    release(writeFactory_); release(d2dFactory_);
    if (uiFont_) DeleteObject(uiFont_);
    if (settingsTitleFont_) DeleteObject(settingsTitleFont_);
    if (settingsLabelFont_) DeleteObject(settingsLabelFont_);
    if (settingsButtonFont_) DeleteObject(settingsButtonFont_);
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
    settingsClass.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    settingsClass.lpfnWndProc = settingsWindowProc;
    settingsClass.hInstance = instance_;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = nullptr;
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
    case WM_TIMER:
        if (wParam == AnimationTimerId) {
            tickWindowAnimation();
            return 0;
        }
        break;
    case WM_ENTERSIZEMOVE:
        if (!isCollapsed()) {
            GetWindowRect(window_, &expandedRect_);
            hasExpandedRect_ = true;
        }
        return 0;
    case WM_EXITSIZEMOVE:
        handleEdgeDock();
        return 0;
    case WM_DPICHANGED: {
        if (animating_) finishWindowAnimation();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        const UINT dpi = HIWORD(wParam);
        const int width = pixels(collapseMode_ == CollapseMode::Side ? SideWidth :
            (collapseMode_ == CollapseMode::Horizontal ? CompactWidth : MainWidth), dpi);
        const int height = pixels(collapseMode_ == CollapseMode::Side ? SideHeight :
            (collapseMode_ == CollapseMode::Horizontal ? CompactHeight : MainHeight), dpi);
        SetWindowPos(window_, nullptr, suggested->left, suggested->top, width, height,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SETCURSOR:
        if (isCollapsed() || (animating_ && animationTargetMode_ != CollapseMode::Expanded)) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_NCHITTEST: {
        if (animating_ || isCollapsed()) return HTCLIENT;
        const auto result = DefWindowProcW(window, message, wParam, lParam);
        if (result == HTCLIENT) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const UINT dpi = GetDpiForWindow(window);
            if (dips(point.y, dpi) < 48 && dips(point.x, dpi) < MainWidth - 120) return HTCAPTION;
        }
        return result;
    }
    case WM_LBUTTONDOWN:
        if (animating_) return 0;
        if (isCollapsed()) {
            GetCursorPos(&compactPressCursor_);
            GetWindowRect(window_, &compactPressRect_);
            compactPointerDown_ = true;
            compactDragging_ = false;
            SetCapture(window_);
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (compactPointerDown_) {
            if (!(wParam & MK_LBUTTON)) return 0;
            POINT cursor{};
            GetCursorPos(&cursor);
            const int dx = cursor.x - compactPressCursor_.x;
            const int dy = cursor.y - compactPressCursor_.y;
            if (!compactDragging_) {
                const int dragX = std::max(2, GetSystemMetrics(SM_CXDRAG));
                const int dragY = std::max(2, GetSystemMetrics(SM_CYDRAG));
                if (std::abs(dx) < dragX && std::abs(dy) < dragY) return 0;
                compactDragging_ = true;
            }
            RECT current{};
            GetWindowRect(window_, &current);
            const int width = current.right - current.left;
            const int height = current.bottom - current.top;
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
            const int maxX = std::max(static_cast<int>(monitor.rcWork.left),
                static_cast<int>(monitor.rcWork.right) - width);
            const int maxY = std::max(static_cast<int>(monitor.rcWork.top),
                static_cast<int>(monitor.rcWork.bottom) - height);
            const int x = std::clamp(static_cast<int>(compactPressRect_.left) + dx,
                static_cast<int>(monitor.rcWork.left), maxX);
            const int y = std::clamp(static_cast<int>(compactPressRect_.top) + dy,
                static_cast<int>(monitor.rcWork.top), maxY);
            SetWindowPos(window_, HWND_TOPMOST, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (compactPointerDown_) {
            compactPointerDown_ = false;
            compactDragging_ = false;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (animating_ || isCollapsed()) return 0;
        break;
    case WM_NCLBUTTONDBLCLK:
        if (!animating_ && wParam == HTCAPTION) toggleCollapsed();
        return 0;
    case WM_LBUTTONUP: {
        if (animating_) return 0;
        if (compactPointerDown_) {
            const bool dragged = compactDragging_;
            compactPointerDown_ = false;
            compactDragging_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            if (dragged) snapCompactAfterDrag();
            else restoreExpanded();
            return 0;
        }
        if (isCollapsed()) return 0;
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
        KillTimer(window_, AnimationTimerId);
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
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFF5F57), &rankRedBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0xFFBD2E), &rankYellowBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x28C840), &rankGreenBrush_);
    return true;
}

void App::discardGraphics() {
    release(textBrush_); release(mutedBrush_); release(accentBrush_); release(surfaceBrush_); release(lineBrush_);
    release(rankRedBrush_); release(rankYellowBrush_); release(rankGreenBrush_);
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
    const UINT dpi = GetDpiForWindow(window_);
    const float layoutWidth = animating_ ? static_cast<float>(dips(client.right, dpi)) :
        (side ? static_cast<float>(SideWidth) :
            (horizontal ? static_cast<float>(CompactWidth) : static_cast<float>(MainWidth)));
    const float layoutHeight = animating_ ? static_cast<float>(dips(client.bottom, dpi)) :
        (side ? static_cast<float>(SideHeight) :
            (horizontal ? static_cast<float>(CompactHeight) : static_cast<float>(MainHeight)));
    const float startCorner = animationFromMode_ == CollapseMode::Expanded ? 14.0f : 18.0f;
    const float targetCorner = animationTargetMode_ == CollapseMode::Expanded ? 14.0f : 18.0f;
    const float corner = animating_ ?
        startCorner + (targetCorner - startCorner) * animationProgress_ :
        (isCollapsed() ? 18.0f : 14.0f);
    const auto surface = D2D1::RoundedRect(D2D1::RectF(0, 0, layoutWidth, layoutHeight), corner, corner);
    renderTarget_->FillRoundedRectangle(surface, surfaceBrush_);
    renderTarget_->DrawRoundedRectangle(surface, lineBrush_, 1);

    const bool modeChanging = animating_ && animationFromMode_ != animationTargetMode_;
    const float contentOpacity = modeChanging ?
        std::max(0.12f, std::abs(animationProgress_ * 2.0f - 1.0f)) : 1.0f;
    textBrush_->SetOpacity(contentOpacity);
    mutedBrush_->SetOpacity(contentOpacity);
    accentBrush_->SetOpacity(contentOpacity);
    surfaceBrush_->SetOpacity(contentOpacity);
    lineBrush_->SetOpacity(contentOpacity);
    rankRedBrush_->SetOpacity(contentOpacity);
    rankYellowBrush_->SetOpacity(contentOpacity);
    rankGreenBrush_->SetOpacity(contentOpacity);

    const D2D1_COLOR_F statusColor = snapshot_.connected ? D2D1::ColorF(0x28A745) : D2D1::ColorF(0x9B9BA1);
    ID2D1SolidColorBrush* statusBrush = nullptr;
    renderTarget_->CreateSolidColorBrush(statusColor, &statusBrush);
    if (statusBrush) statusBrush->SetOpacity(contentOpacity);
    const std::wstring latency = snapshot_.connected ? numberText(snapshot_.sshLatencyMs) : L"—";
    IDWriteTextFormat* compact = compactFormat_ ? compactFormat_ : labelFormat_;

    if (side) {
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(22, 20), 4, 4), statusBrush);
        drawText(renderTarget_, compact, textBrush_, latency, D2D1::RectF(2, 42, 42, 68));
        drawText(renderTarget_, compact, mutedBrush_, L"ms", D2D1::RectF(2, 65, 42, 89));
    } else if (horizontal) {
        drawText(renderTarget_, titleFormat_, textBrush_, L"AirSFTP", D2D1::RectF(10, 4, 88, 32));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(97, 18), 4, 4), statusBrush);
        drawText(renderTarget_, compact, mutedBrush_, latency + L" ms", D2D1::RectF(106, 2, 171, 34));
    } else {
        drawText(renderTarget_, titleFormat_, textBrush_, L"AirSFTP", D2D1::RectF(18, 13, 130, 42));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(150, 27), 4, 4), statusBrush);
        drawText(renderTarget_, labelFormat_, mutedBrush_, wide(snapshot_.status), D2D1::RectF(160, 14, 265, 40));
        drawGearIcon(renderTarget_, d2dFactory_, mutedBrush_, surfaceBrush_, D2D1::Point2F(314, 27));
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
        std::vector<double> coreTemperatures;
        coreTemperatures.reserve(snapshot_.coreTemperatures.size());
        for (const auto& sensor : snapshot_.coreTemperatures) {
            if (std::isfinite(sensor.value) && sensor.value >= 0)
                coreTemperatures.push_back(sensor.value);
        }
        std::sort(coreTemperatures.begin(), coreTemperatures.end(),
            [](double left, double right) { return left > right; });
        drawText(renderTarget_, labelFormat_, mutedBrush_, L"Cores", D2D1::RectF(18, 139, 84, 163));
        if (coreTemperatures.empty()) {
            drawText(renderTarget_, compact, mutedBrush_, L"—", D2D1::RectF(96, 139, 182, 163));
        } else {
            ID2D1Brush* rankBrushes[] = {rankRedBrush_, rankYellowBrush_, rankGreenBrush_};
            const std::size_t count = std::min<std::size_t>(3, coreTemperatures.size());
            constexpr float slotWidth = 86.0f;
            constexpr float startX = 96.0f;
            for (std::size_t index = 0; index < count; ++index) {
                const float x = startX + slotWidth * static_cast<float>(index);
                drawText(renderTarget_, compact, rankBrushes[index],
                    numberText(coreTemperatures[index], L"°C"),
                    D2D1::RectF(x, 139, x + slotWidth, 163));
            }
        }

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
            drawText(renderTarget_, labelFormat_, textBrush_, wide(transfer_.fileName), D2D1::RectF(18, 236, 210, 253));
            drawText(renderTarget_, labelFormat_, mutedBrush_, transfer_.active ? humanBytes(transfer_.bytesPerSecond, true) : wide(transfer_.message), D2D1::RectF(220, 236, 360, 253));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, 258, 362, 263), 3, 3), lineBrush_);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, 258, 18 + 344 * progressValue, 263), 3, 3), accentBrush_);
        }
    }
    textBrush_->SetOpacity(1.0f);
    mutedBrush_->SetOpacity(1.0f);
    accentBrush_->SetOpacity(1.0f);
    surfaceBrush_->SetOpacity(1.0f);
    lineBrush_->SetOpacity(1.0f);
    rankRedBrush_->SetOpacity(1.0f);
    rankYellowBrush_->SetOpacity(1.0f);
    rankGreenBrush_->SetOpacity(1.0f);
    const HRESULT result = renderTarget_->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) discardGraphics();
    EndPaint(window_, &paint);
}

void App::startWindowAnimation(const RECT& target, CollapseMode targetMode, bool clearExpandedRect) {
    if (!window_) return;
    if (animating_) KillTimer(window_, AnimationTimerId);

    GetWindowRect(window_, &animationStartRect_);
    animationTargetRect_ = target;
    animationFromMode_ = collapseMode_;
    animationTargetMode_ = targetMode;
    animationClearsExpandedRect_ = clearExpandedRect;
    animationProgress_ = 0.0f;

    BOOL animationsEnabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animationsEnabled, 0);
    const bool unchanged = animationStartRect_.left == target.left &&
        animationStartRect_.top == target.top && animationStartRect_.right == target.right &&
        animationStartRect_.bottom == target.bottom && collapseMode_ == targetMode;
    if (!animationsEnabled || unchanged) {
        collapseMode_ = targetMode;
        SetWindowPos(window_, HWND_TOPMOST, target.left, target.top,
            target.right - target.left, target.bottom - target.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (clearExpandedRect) hasExpandedRect_ = false;
        animationFromMode_ = targetMode;
        animationProgress_ = 1.0f;
        animating_ = false;
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    animating_ = true;
    animationStartTime_ = GetTickCount64();
    if (!SetTimer(window_, AnimationTimerId, AnimationFrameMs, nullptr)) {
        finishWindowAnimation();
        return;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void App::tickWindowAnimation() {
    if (!animating_) return;
    const ULONGLONG elapsed = GetTickCount64() - animationStartTime_;
    if (elapsed >= AnimationDurationMs) {
        finishWindowAnimation();
        return;
    }

    const float raw = std::clamp(static_cast<float>(elapsed) /
        static_cast<float>(AnimationDurationMs), 0.0f, 1.0f);
    animationProgress_ = raw;
    const float eased = raw * raw * (3.0f - 2.0f * raw);
    const auto interpolate = [eased](LONG start, LONG end) {
        return static_cast<int>(std::lround(static_cast<float>(start) +
            static_cast<float>(end - start) * eased));
    };
    if (animationFromMode_ != animationTargetMode_) {
        const CollapseMode displayMode = raw < 0.5f ? animationFromMode_ : animationTargetMode_;
        if (collapseMode_ != displayMode) collapseMode_ = displayMode;
    }

    const int left = interpolate(animationStartRect_.left, animationTargetRect_.left);
    const int top = interpolate(animationStartRect_.top, animationTargetRect_.top);
    const int right = interpolate(animationStartRect_.right, animationTargetRect_.right);
    const int bottom = interpolate(animationStartRect_.bottom, animationTargetRect_.bottom);
    SetWindowPos(window_, HWND_TOPMOST, left, top, std::max(1, right - left),
        std::max(1, bottom - top), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(window_, nullptr, FALSE);
}

void App::finishWindowAnimation() {
    if (!animating_) return;
    KillTimer(window_, AnimationTimerId);
    animating_ = false;
    animationProgress_ = 1.0f;
    collapseMode_ = animationTargetMode_;
    SetWindowPos(window_, HWND_TOPMOST, animationTargetRect_.left, animationTargetRect_.top,
        animationTargetRect_.right - animationTargetRect_.left,
        animationTargetRect_.bottom - animationTargetRect_.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (animationClearsExpandedRect_) hasExpandedRect_ = false;
    animationFromMode_ = collapseMode_;
    InvalidateRect(window_, nullptr, FALSE);
}

void App::toggleCollapsed() {
    if (isCollapsed()) restoreExpanded();
    else collapseHorizontal(false);
}

void App::collapseHorizontal(bool dockToTop, HMONITOR targetMonitor) {
    if (isCollapsed() || animating_) return;
    RECT current{}; GetWindowRect(window_, &current);
    if (!dockToTop || !hasExpandedRect_) {
        expandedRect_ = current;
        hasExpandedRect_ = true;
    }
    MONITORINFO monitor{sizeof(monitor)};
    if (!targetMonitor) targetMonitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(targetMonitor, &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = pixels(CompactWidth, dpi);
    const int height = pixels(CompactHeight, dpi);
    const int desiredX = current.left + ((current.right - current.left) - width) / 2;
    const int desiredY = dockToTop ? static_cast<int>(monitor.rcWork.top) : current.top;
    const int x = std::clamp(desiredX, static_cast<int>(monitor.rcWork.left),
        static_cast<int>(monitor.rcWork.right) - width);
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    startWindowAnimation(RECT{x, y, x + width, y + height},
        CollapseMode::Horizontal, false);
}

void App::collapseSide(bool left, HMONITOR targetMonitor) {
    if (isCollapsed() || animating_) return;
    RECT current{}; GetWindowRect(window_, &current);
    if (!hasExpandedRect_) {
        expandedRect_ = current;
        hasExpandedRect_ = true;
    }
    MONITORINFO monitor{sizeof(monitor)};
    if (!targetMonitor) targetMonitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(targetMonitor, &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = pixels(SideWidth, dpi);
    const int height = pixels(SideHeight, dpi);
    const int x = left ? static_cast<int>(monitor.rcWork.left) :
        static_cast<int>(monitor.rcWork.right) - width;
    const int desiredY = current.top + ((current.bottom - current.top) - height) / 2;
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    startWindowAnimation(RECT{x, y, x + width, y + height},
        CollapseMode::Side, false);
}

void App::snapCompactAfterDrag() {
    if (!isCollapsed() || animating_) return;

    RECT current{};
    GetWindowRect(window_, &current);
    POINT cursor{};
    const bool hasCursor = GetCursorPos(&cursor);
    HMONITOR targetMonitor = hasCursor
        ? MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(targetMonitor, &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int threshold = pixels(CompactSnapThreshold, dpi);
    const int topDistance = std::abs(current.top - static_cast<int>(monitor.rcWork.top));
    const int leftDistance = std::abs(current.left - static_cast<int>(monitor.rcWork.left));
    const int rightDistance = std::abs(current.right - static_cast<int>(monitor.rcWork.right));
    const int nearest = std::min({topDistance, leftDistance, rightDistance});
    const int centerX = current.left + (current.right - current.left) / 2;
    const int centerY = current.top + (current.bottom - current.top) / 2;

    int width = pixels(CompactWidth, dpi);
    int height = pixels(CompactHeight, dpi);
    int desiredX = centerX - width / 2;
    int desiredY = centerY - height / 2;
    CollapseMode targetMode = CollapseMode::Horizontal;

    if (nearest <= threshold && topDistance == nearest) {
        desiredY = monitor.rcWork.top;
    } else if (nearest <= threshold) {
        targetMode = CollapseMode::Side;
        width = pixels(SideWidth, dpi);
        height = pixels(SideHeight, dpi);
        desiredX = leftDistance == nearest ? static_cast<int>(monitor.rcWork.left) :
            static_cast<int>(monitor.rcWork.right) - width;
        desiredY = centerY - height / 2;
    }

    const int maxX = std::max(static_cast<int>(monitor.rcWork.left),
        static_cast<int>(monitor.rcWork.right) - width);
    const int maxY = std::max(static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    const int x = std::clamp(desiredX, static_cast<int>(monitor.rcWork.left), maxX);
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top), maxY);
    const RECT target{x, y, x + width, y + height};
    updateRestoreAnchor(targetMonitor, target, targetMode);
    startWindowAnimation(target, targetMode, false);
}

void App::updateRestoreAnchor(HMONITOR targetMonitor, const RECT& compact, CollapseMode mode) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(targetMonitor, &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = pixels(MainWidth, dpi);
    const int height = pixels(MainHeight, dpi);
    const int gap = pixels(RestoreGap, dpi);
    const int centerX = compact.left + (compact.right - compact.left) / 2;
    const int centerY = compact.top + (compact.bottom - compact.top) / 2;

    int desiredX = centerX - width / 2;
    int desiredY = compact.bottom + gap;
    if (mode == CollapseMode::Side) {
        const bool onLeft = std::abs(compact.left - static_cast<int>(monitor.rcWork.left)) <=
            std::abs(compact.right - static_cast<int>(monitor.rcWork.right));
        desiredX = onLeft ? compact.right + gap : compact.left - gap - width;
        desiredY = centerY - height / 2;
    } else if (desiredY + height > monitor.rcWork.bottom) {
        desiredY = compact.top - gap - height;
    }

    const int maxX = std::max(static_cast<int>(monitor.rcWork.left),
        static_cast<int>(monitor.rcWork.right) - width);
    const int maxY = std::max(static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    const int x = std::clamp(desiredX, static_cast<int>(monitor.rcWork.left), maxX);
    const int y = std::clamp(desiredY, static_cast<int>(monitor.rcWork.top), maxY);
    expandedRect_ = RECT{x, y, x + width, y + height};
    hasExpandedRect_ = true;
}

void App::restoreExpanded() {
    if (!isCollapsed() || animating_) return;
    RECT target = expandedRect_;
    if (!hasExpandedRect_) {
        GetWindowRect(window_, &target);
        const UINT dpi = GetDpiForWindow(window_);
        target.right = target.left + pixels(MainWidth, dpi);
        target.bottom = target.top + pixels(MainHeight, dpi);
    }
    startWindowAnimation(target, CollapseMode::Expanded, true);
}

void App::handleEdgeDock() {
    if (isCollapsed() || animating_) return;
    RECT rect{}; GetWindowRect(window_, &rect);
    POINT cursor{};
    const bool hasCursor = GetCursorPos(&cursor);
    HMONITOR targetMonitor = hasCursor
        ? MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST)
        : MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(targetMonitor, &monitor);
    const int threshold = pixels(EdgeThreshold, GetDpiForWindow(window_));
    const int topDistance = hasCursor
        ? std::abs(cursor.y - static_cast<int>(monitor.rcWork.top))
        : std::abs(rect.top - static_cast<int>(monitor.rcWork.top));
    const int leftDistance = hasCursor
        ? std::abs(cursor.x - static_cast<int>(monitor.rcWork.left))
        : std::abs(rect.left - static_cast<int>(monitor.rcWork.left));
    const int rightDistance = hasCursor
        ? std::abs(cursor.x - static_cast<int>(monitor.rcWork.right))
        : std::abs(rect.right - static_cast<int>(monitor.rcWork.right));
    const int nearest = std::min({topDistance, leftDistance, rightDistance});
    if (nearest > threshold) {
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int maxX = std::max(static_cast<int>(monitor.rcWork.left),
            static_cast<int>(monitor.rcWork.right) - width);
        const int maxY = std::max(static_cast<int>(monitor.rcWork.top),
            static_cast<int>(monitor.rcWork.bottom) - height);
        const int x = std::clamp(static_cast<int>(rect.left),
            static_cast<int>(monitor.rcWork.left), maxX);
        const int y = std::clamp(static_cast<int>(rect.top),
            static_cast<int>(monitor.rcWork.top), maxY);
        if (x != rect.left || y != rect.top) {
            SetWindowPos(window_, HWND_TOPMOST, x, y, 0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        hasExpandedRect_ = false;
        return;
    }
    if (leftDistance == nearest) collapseSide(true, targetMonitor);
    else if (rightDistance == nearest) collapseSide(false, targetMonitor);
    else collapseHorizontal(true, targetMonitor);
}

void App::rebuildSettingsFonts(UINT dpi) {
    auto makeFont = [dpi](int size, int weight) {
        return CreateFontW(-pixels(size, dpi), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Microsoft YaHei UI");
    };
    HFONT oldUi = uiFont_;
    HFONT oldTitle = settingsTitleFont_;
    HFONT oldLabel = settingsLabelFont_;
    HFONT oldButton = settingsButtonFont_;
    uiFont_ = makeFont(14, FW_NORMAL);
    settingsTitleFont_ = makeFont(18, FW_SEMIBOLD);
    settingsLabelFont_ = makeFont(12, FW_NORMAL);
    settingsButtonFont_ = makeFont(13, FW_SEMIBOLD);

    if (settingsWindow_) {
        constexpr int edits[] = {HostEdit, PortEdit, UserEdit, PasswordEdit, KeyEdit, LocalEdit, RemoteEdit};
        constexpr int buttons[] = {BrowseLocal, SaveConnect, UploadButton, DownloadButton};
        for (int id : edits)
            SendMessageW(GetDlgItem(settingsWindow_, id), WM_SETFONT,
                reinterpret_cast<WPARAM>(uiFont_), TRUE);
        for (int id : buttons)
            SendMessageW(GetDlgItem(settingsWindow_, id), WM_SETFONT,
                reinterpret_cast<WPARAM>(settingsButtonFont_), TRUE);
    }
    if (oldUi) DeleteObject(oldUi);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldLabel) DeleteObject(oldLabel);
    if (oldButton) DeleteObject(oldButton);
}

void App::layoutSettings(UINT dpi) {
    if (!settingsWindow_) return;
    auto p = [dpi](int value) { return pixels(value, dpi); };
    auto move = [&](int id, int x, int y, int width, int height) {
        MoveWindow(GetDlgItem(settingsWindow_, id), p(x), p(y), p(width), p(height), TRUE);
    };
    move(HostEdit, 34, 88, 294, 22);
    move(PortEdit, 360, 88, 86, 22);
    move(UserEdit, 34, 150, 156, 22);
    move(PasswordEdit, 222, 150, 224, 22);
    move(KeyEdit, 34, 212, 412, 22);
    move(LocalEdit, 34, 274, 326, 22);
    move(BrowseLocal, 380, 268, 76, 34);
    move(RemoteEdit, 34, 336, 412, 22);
    move(SaveConnect, 162, 401, 126, 38);
    move(UploadButton, 296, 401, 76, 38);
    move(DownloadButton, 380, 401, 76, 38);

    constexpr int edits[] = {HostEdit, PortEdit, UserEdit, PasswordEdit, KeyEdit, LocalEdit, RemoteEdit};
    for (int id : edits) {
        HWND edit = GetDlgItem(settingsWindow_, id);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(p(2), p(2)));
    }
}

void App::paintSettings() {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(settingsWindow_, &paint);
    RECT client{};
    GetClientRect(settingsWindow_, &client);
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    const UINT dpi = GetDpiForWindow(settingsWindow_);
    auto p = [dpi](int value) { return pixels(value, dpi); };

    HBRUSH background = CreateSolidBrush(RGB(246, 246, 248));
    FillRect(buffer, &client, background);
    DeleteObject(background);
    RECT header{0, 0, client.right, p(SettingsHeaderHeight)};
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(buffer, &header, white);

    HPEN separator = CreatePen(PS_SOLID, std::max(1, p(1)), RGB(229, 229, 234));
    HGDIOBJ oldPen = SelectObject(buffer, separator);
    MoveToEx(buffer, 0, p(SettingsHeaderHeight) - 1, nullptr);
    LineTo(buffer, client.right, p(SettingsHeaderHeight) - 1);

    HPEN fieldPen = CreatePen(PS_SOLID, std::max(1, p(1)), RGB(218, 218, 223));
    SelectObject(buffer, fieldPen);
    HGDIOBJ oldBrush = SelectObject(buffer, white);
    auto field = [&](int x, int y, int width, int height) {
        RoundRect(buffer, p(x), p(y), p(x + width), p(y + height), p(10), p(10));
    };
    field(24, 82, 314, 34);
    field(350, 82, 106, 34);
    field(24, 144, 176, 34);
    field(212, 144, 244, 34);
    field(24, 206, 432, 34);
    field(24, 268, 346, 34);
    field(24, 330, 432, 34);

    SetBkMode(buffer, TRANSPARENT);
    SetTextColor(buffer, RGB(29, 29, 31));
    HGDIOBJ oldFont = SelectObject(buffer, settingsTitleFont_);
    RECT title{p(22), p(9), p(340), p(45)};
    DrawTextW(buffer, L"连接与传输", -1, &title, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    HBRUSH closeBrush = CreateSolidBrush(RGB(241, 241, 244));
    SelectObject(buffer, GetStockObject(NULL_PEN));
    SelectObject(buffer, closeBrush);
    Ellipse(buffer, p(437), p(12), p(465), p(40));
    SelectObject(buffer, settingsButtonFont_);
    SetTextColor(buffer, RGB(110, 110, 115));
    RECT closeRect{p(437), p(11), p(465), p(40)};
    DrawTextW(buffer, L"×", -1, &closeRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(buffer, white);
    DeleteObject(closeBrush);

    SelectObject(buffer, settingsLabelFont_);
    SetTextColor(buffer, RGB(110, 110, 115));
    auto label = [&](const wchar_t* value, int x, int y, int width) {
        RECT rect{p(x), p(y), p(x + width), p(y + 18)};
        DrawTextW(buffer, value, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    };
    label(L"主机", 24, 61, 314);
    label(L"端口", 350, 61, 106);
    label(L"用户名", 24, 123, 176);
    label(L"密码 / 密钥口令", 212, 123, 244);
    label(L"私钥文件（可选）", 24, 185, 432);
    label(L"本地文件", 24, 247, 346);
    label(L"远程文件", 24, 309, 432);

    SetTextColor(buffer, RGB(142, 142, 147));
    RECT note{p(24), p(371), p(456), p(393)};
    DrawTextW(buffer, L"密码不落盘 · 私钥口令填在密码框 · 远端需 lm-sensors", -1, &note,
        DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(buffer, oldFont);
    SelectObject(buffer, oldBrush);
    SelectObject(buffer, oldPen);
    DeleteObject(fieldPen);
    DeleteObject(separator);
    DeleteObject(white);
    BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(settingsWindow_, &paint);
}

void App::drawSettingsButton(const DRAWITEMSTRUCT& item) {
    const bool primary = item.CtlID == SaveConnect;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const UINT dpi = GetDpiForWindow(settingsWindow_);
    auto p = [dpi](int value) { return pixels(value, dpi); };
    COLORREF fill = primary ? RGB(0, 122, 255) : RGB(255, 255, 255);
    COLORREF border = primary ? RGB(0, 122, 255) : RGB(209, 209, 214);
    COLORREF textColor = primary ? RGB(255, 255, 255) : RGB(0, 113, 227);
    if (pressed) fill = primary ? RGB(0, 101, 214) : RGB(236, 243, 252);
    if (disabled) {
        fill = RGB(235, 235, 238);
        border = RGB(222, 222, 226);
        textColor = RGB(160, 160, 165);
    }
    if (focused && !primary) border = RGB(0, 122, 255);

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, focused ? std::max(1, p(2)) : std::max(1, p(1)), border);
    HGDIOBJ oldBrush = SelectObject(item.hDC, brush);
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom,
        p(10), p(10));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    HGDIOBJ oldFont = SelectObject(item.hDC, settingsButtonFont_);
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    RECT textRect = item.rcItem;
    if (pressed) OffsetRect(&textRect, 0, p(1));
    DrawTextW(item.hDC, text, -1, &textRect,
        DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void App::createSettingsWindow() {
    const UINT dpi = GetDpiForWindow(window_);
    settingsWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, SettingsClass, L"AirSFTP 设置",
        WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        pixels(SettingsWidth, dpi), pixels(SettingsHeight, dpi),
        window_, nullptr, instance_, this);
    if (!settingsWindow_) return;
    constexpr DWORD roundPreference = 2;
    constexpr auto cornerAttribute = static_cast<DWMWINDOWATTRIBUTE>(33);
    DwmSetWindowAttribute(settingsWindow_, cornerAttribute, &roundPreference, sizeof(roundPreference));
    rebuildSettingsFonts(dpi);

    HWND host = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, HostEdit, uiFont_);
    HWND port = addControl(settingsWindow_, WC_EDITW, L"22", ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 1, 1, PortEdit, uiFont_);
    HWND user = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, UserEdit, uiFont_);
    HWND password = addControl(settingsWindow_, WC_EDITW, L"", ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 1, 1, PasswordEdit, uiFont_);
    HWND key = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, KeyEdit, uiFont_);
    HWND local = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, LocalEdit, uiFont_);
    HWND remote = addControl(settingsWindow_, WC_EDITW, L"/tmp/", ES_AUTOHSCROLL, 0, 0, 1, 1, RemoteEdit, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"浏览", BS_OWNERDRAW, 0, 0, 1, 1, BrowseLocal, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"连接并监控", BS_OWNERDRAW, 0, 0, 1, 1, SaveConnect, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"上传", BS_OWNERDRAW, 0, 0, 1, 1, UploadButton, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"下载", BS_OWNERDRAW, 0, 0, 1, 1, DownloadButton, settingsButtonFont_);

    SendMessageW(host, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"192.168.1.10"));
    SendMessageW(port, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"22"));
    SendMessageW(user, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"root"));
    SendMessageW(password, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"密码或私钥口令"));
    SendMessageW(key, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"可选：私钥文件路径"));
    SendMessageW(local, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"选择本地文件"));
    SendMessageW(remote, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"/tmp/file"));
    layoutSettings(dpi);
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
    const int desiredX = owner.left + ((owner.right - owner.left) - dialogWidth) / 2;
    const int desiredY = owner.top + ((owner.bottom - owner.top) - dialogHeight) / 2;
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
    case WM_PAINT:
        paintSettings();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item && item->CtlType == ODT_BUTTON) {
            drawSettingsButton(*item);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(29, 29, 31));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_DPICHANGED: {
        const UINT dpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(window, nullptr, suggested->left, suggested->top,
            pixels(SettingsWidth, dpi), pixels(SettingsHeight, dpi),
            SWP_NOZORDER | SWP_NOACTIVATE);
        rebuildSettingsFonts(dpi);
        layoutSettings(dpi);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        const UINT dpi = GetDpiForWindow(window);
        const int x = dips(point.x, dpi);
        const int y = dips(point.y, dpi);
        if (y >= 0 && y < SettingsHeaderHeight && x < SettingsWidth - 50) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_LBUTTONUP: {
        const UINT dpi = GetDpiForWindow(window);
        const int x = dips(GET_X_LPARAM(lParam), dpi);
        const int y = dips(GET_Y_LPARAM(lParam), dpi);
        if (y >= 0 && y < SettingsHeaderHeight && x >= SettingsWidth - 50)
            ShowWindow(window, SW_HIDE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            ShowWindow(window, SW_HIDE);
            return 0;
        }
        break;
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
