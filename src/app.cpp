#include "app.hpp"

#include "credential_store.hpp"
#include "resource.h"
#include "ssh_client.hpp"
#include "telemetry.hpp"
#include "local_telemetry.hpp"
#include "transfer_picker.hpp"
#include <shobjidl.h>

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <climits>
#include <filesystem>

#include <sstream>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t MainClass[] = L"sshhelperFloatingWindow";
constexpr wchar_t SettingsClass[] = L"sshhelperSettingsWindow";
constexpr UINT SnapshotMessage = WM_APP + 1;
constexpr UINT TransferMessage = WM_APP + 2;
constexpr UINT ConfirmTransferMessage = WM_APP + 3;
constexpr int MainWidth = 380;
constexpr int MainHeight = 300;
constexpr float HeaderCenterY = 27.0f;
constexpr float HeaderStatusX = 150.0f;
// Every host uses the same header center, including fitted long hostnames.
D2D1_RECT_F headerTextRect(float left, float right) {
    return D2D1::RectF(left, HeaderCenterY - 14, right, HeaderCenterY + 14);
}
bool titleHit(int x, int y) { return x >= 18 && x <= 140 && y >= 13 && y <= 42; }
constexpr int CompactWidth = 100;
constexpr int CompactHeight = 32;
constexpr int SideWidth = 36;
constexpr int SideHeight = 78;
constexpr int EdgeThreshold = 20;
constexpr int CompactSnapThreshold = 36;
constexpr int RestoreGap = 8;
constexpr UINT_PTR AnimationTimerId = 1;
constexpr UINT AnimationFrameMs = 15;
constexpr ULONGLONG AnimationDurationMs = 180;
constexpr int SettingsWidth = 480;
constexpr int SettingsHeight = 548;
constexpr UINT_PTR SettingsAnimationTimerId = 2;
constexpr int SettingsHeaderHeight = 52;

COLORREF settingsColor(std::uint32_t rgb) {
    return RGB(rgb >> 16, (rgb >> 8) & 255, rgb & 255);
}

COLORREF blendColor(COLORREF a, COLORREF b, float amount) {
    auto mix = [amount](int x, int y) { return int(x + (y - x) * amount); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
        mix(GetBValue(a), GetBValue(b)));
}

int pixels(int dips, UINT dpi) {
    return MulDiv(dips, static_cast<int>(dpi), 96);
}

int dips(int value, UINT dpi) {
    return MulDiv(value, 96, static_cast<int>(dpi));
}

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

std::wstring powerLabel(const wchar_t* device, const wchar_t* fallback, double watts) {
    if (!std::isfinite(watts) || watts < 0.0) return fallback;
    wchar_t buffer[64]{};
    swprintf(buffer, 64, watts >= 100.0 ? L"%ls·%.0fW" : L"%ls·%.1fW", device, watts);
    return buffer;
}

std::wstring storageText(std::uint64_t bytes) {
    constexpr double BytesPerGigabyte = 1'000'000'000.0;
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%.1f GB", static_cast<double>(bytes) / BytesPerGigabyte);
    return buffer;
}

std::wstring linkSpeedText(std::uint64_t mbps) {
    if (!mbps) return L"—";
    if (mbps < 1000) return std::to_wstring(mbps) + L" Mbps";
    wchar_t buffer[64]{};
    swprintf(buffer, 64, L"%.3g Gbps", static_cast<double>(mbps) / 1000.0);
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

void drawFittedText(ID2D1HwndRenderTarget* target, IDWriteFactory* factory,
                    IDWriteTextFormat* format, ID2D1Brush* brush,
                    const std::wstring& text, const D2D1_RECT_F& rect) {
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
            rect.right - rect.left, rect.bottom - rect.top, &layout))) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.width > rect.right - rect.left)
        layout->SetFontSize(format->GetFontSize() * (rect.right - rect.left) / metrics.width,
            DWRITE_TEXT_RANGE{0, static_cast<UINT32>(text.size())});
    target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    release(layout);
}

void drawParameterRow(ID2D1HwndRenderTarget* target, IDWriteFactory* factory,
                      IDWriteTextFormat* labelFormat, IDWriteTextFormat* valueFormat,
                      ID2D1Brush* brush, float top, float bottom,
                      const std::array<std::wstring, 4>& values,
                      const std::array<ID2D1Brush*, 4>& brushes = {}) {
    // Each parameter row shares the label and three left-aligned value columns.
    constexpr float left[] = {18, 96, 182, 268};
    constexpr float right[] = {84, 174, 260, 354};
    for (std::size_t column = 0; column < values.size(); ++column) {
        auto* format = column ? valueFormat : labelFormat;
        const auto alignment = format->GetTextAlignment();
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        drawFittedText(target, factory, format, brushes[column] ? brushes[column] : brush, values[column],
            D2D1::RectF(left[column], top, right[column], bottom));
        format->SetTextAlignment(alignment);
    }
}

class SettingsCanvas {
public:
    SettingsCanvas(ID2D1Factory* factory, IDWriteFactory* textFactory, ID2D1DCRenderTarget*& target,
                   HDC dc, const RECT& bounds, UINT dpi, bool modern)
        : target_(target), textFactory_(textFactory), dc_(dc), bounds_(bounds), scale_(96.0f / dpi) {
        if (!modern || !factory || !textFactory) return;
        if (!target_) {
            const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(factory->CreateDCRenderTarget(&properties, &target_))) return;
        }
        GdiFlush();
        if (FAILED(target_->BindDC(dc, &bounds)) ||
            FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0x000000), &brush_))) {
            release(brush_); release(target_); return;
        }
        target_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target_->BeginDraw();
        modern_ = true;
    }
    ~SettingsCanvas() { if (!finished_) finish(); }

    void clear(COLORREF color) {
        if (modern_) target_->Clear(d2dColor(color));
        else {
            SetDCBrushColor(dc_, color);
            FillRect(dc_, &bounds_, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
    }
    void rounded(const RECT& rect, int diameter, COLORREF fill, COLORREF border, int width) {
        if (modern_) {
            const auto shape = D2D1::RoundedRect(relative(rect), diameter * scale_ / 2, diameter * scale_ / 2);
            brush_->SetColor(d2dColor(fill)); target_->FillRoundedRectangle(shape, brush_);
            brush_->SetColor(d2dColor(border)); target_->DrawRoundedRectangle(shape, brush_, width * scale_);
        } else {
            auto brush = CreateSolidBrush(fill); auto pen = CreatePen(PS_SOLID, width, border);
            const auto oldBrush = SelectObject(dc_, brush), oldPen = SelectObject(dc_, pen);
            RoundRect(dc_, rect.left, rect.top, rect.right, rect.bottom, diameter, diameter);
            SelectObject(dc_, oldBrush); SelectObject(dc_, oldPen); DeleteObject(brush); DeleteObject(pen);
        }
    }
    void ellipse(const RECT& rect, COLORREF fill, COLORREF border, int width) {
        if (modern_) {
            const auto box = relative(rect);
            const auto shape = D2D1::Ellipse(D2D1::Point2F((box.left + box.right) / 2, (box.top + box.bottom) / 2),
                (box.right - box.left) / 2, (box.bottom - box.top) / 2);
            brush_->SetColor(d2dColor(fill)); target_->FillEllipse(shape, brush_);
            brush_->SetColor(d2dColor(border)); target_->DrawEllipse(shape, brush_, width * scale_);
        } else {
            auto brush = CreateSolidBrush(fill); auto pen = CreatePen(PS_SOLID, width, border);
            const auto oldBrush = SelectObject(dc_, brush), oldPen = SelectObject(dc_, pen);
            Ellipse(dc_, rect.left, rect.top, rect.right, rect.bottom);
            SelectObject(dc_, oldBrush); SelectObject(dc_, oldPen); DeleteObject(brush); DeleteObject(pen);
        }
    }
    void line(int x1, int y1, int x2, int y2, COLORREF color, int width) {
        if (modern_) {
            brush_->SetColor(d2dColor(color));
            target_->DrawLine(D2D1::Point2F((x1 - bounds_.left) * scale_, (y1 - bounds_.top) * scale_),
                D2D1::Point2F((x2 - bounds_.left) * scale_, (y2 - bounds_.top) * scale_), brush_, width * scale_);
        } else {
            auto pen = CreatePen(PS_SOLID, width, color); const auto old = SelectObject(dc_, pen);
            MoveToEx(dc_, x1, y1, nullptr); LineTo(dc_, x2, y2);
            SelectObject(dc_, old); DeleteObject(pen);
        }
    }
    void text(const std::wstring& value, const RECT& rect, IDWriteTextFormat* format,
              HFONT fallback, COLORREF color, DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (value.empty()) return;
        if (modern_) {
            const auto box = relative(rect);
            IDWriteTextLayout* layout = nullptr;
            if (!format || FAILED(textFactory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                    format, box.right - box.left, box.bottom - box.top, &layout))) { failed_ = true; return; }
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            layout->SetTextAlignment(alignment);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            IDWriteInlineObject* ellipsis = nullptr;
            if (SUCCEEDED(textFactory_->CreateEllipsisTrimmingSign(format, &ellipsis))) {
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                layout->SetTrimming(&trimming, ellipsis); release(ellipsis);
            }
            brush_->SetColor(d2dColor(color));
            target_->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout, brush_, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            release(layout);
        } else {
            const auto old = SelectObject(dc_, fallback); SetBkMode(dc_, TRANSPARENT); SetTextColor(dc_, color);
            auto box = rect;
            DrawTextW(dc_, value.c_str(), -1, &box, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS |
                (alignment == DWRITE_TEXT_ALIGNMENT_CENTER ? DT_CENTER : DT_LEFT));
            SelectObject(dc_, old);
        }
    }
    bool finish() {
        if (finished_) return !failed_;
        finished_ = true;
        if (modern_ && FAILED(target_->EndDraw())) failed_ = true;
        release(brush_);
        if (failed_) release(target_);
        return !failed_;
    }
private:
    static D2D1_COLOR_F d2dColor(COLORREF color) {
        return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
    }
    D2D1_RECT_F relative(const RECT& rect) const {
        return D2D1::RectF((rect.left - bounds_.left) * scale_, (rect.top - bounds_.top) * scale_,
            (rect.right - bounds_.left) * scale_, (rect.bottom - bounds_.top) * scale_);
    }
    ID2D1DCRenderTarget*& target_;
    IDWriteFactory* textFactory_;
    ID2D1SolidColorBrush* brush_ = nullptr;
    HDC dc_;
    RECT bounds_;
    float scale_;
    bool modern_ = false, failed_ = false, finished_ = false;
};

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

App::App(HINSTANCE instance, std::filesystem::path credentials)
    : credentialPath_(credentials.empty() ? defaultCredentialPath() : std::move(credentials)), instance_(instance) {
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
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-cn", &capacityFormat_);
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 17.0f, L"zh-cn", &settingsTitleFormat_);
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-cn", &settingsLabelFormat_);
        writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn", &settingsButtonFormat_);
        if (capacityFormat_) {
            capacityFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            capacityFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (compactFormat_) {
            compactFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            compactFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

App::~App() {
    removeTray();
    stopTransfer();
    stopMonitor();
    if (transferThread_.joinable()) transferThread_.join();
    if (monitorThread_.joinable()) monitorThread_.join();
    discardGraphics();
    release(settingsRenderTarget_);
    release(settingsTitleFormat_); release(settingsLabelFormat_); release(settingsButtonFormat_);
    release(titleFormat_); release(metricFormat_); release(labelFormat_); release(compactFormat_); release(capacityFormat_);
    release(writeFactory_); release(d2dFactory_);
    if (uiFont_) DeleteObject(uiFont_);
    if (settingsTitleFont_) DeleteObject(settingsTitleFont_);
    if (settingsLabelFont_) DeleteObject(settingsLabelFont_);
    if (settingsButtonFont_) DeleteObject(settingsButtonFont_);
}

int App::run(int showCommand) {
    ServerSettings saved;
    std::string error;
    loadServerSettings(credentialPath_, saved, error);
    if (std::none_of(saved.servers.begin(), saved.servers.end(), [](const auto& p) { return p.local; })) {
        ServerProfile local; local.local = true; local.name = "本机";
        saved.servers.push_back(std::move(local));
    }
    if (!saved.servers.empty()) applyProfile(saved.servers.front());
    if (!registerClasses() || !createMainWindow(SW_HIDE)) return 1;
    const bool trayReady = createTray();
    showWhenConnected_ = local_ || !config_.host.empty();
    if (!error.empty()) { snapshot_.status = error; InvalidateRect(window_, nullptr, FALSE); }
    for (std::size_t i = 1; i < saved.servers.size(); ++i) {
        auto child = std::make_unique<App>(instance_);
        child->root_ = this;
        child->windowSlot_ = i;
        child->applyProfile(saved.servers[i]);
        child->showWhenConnected_ = true;
        child->createMainWindow(SW_HIDE);
        children_.push_back(std::move(child));
    }
    for (auto* server : servers())
        if (server->window_ && (server->local_ || !server->config_.host.empty())) server->startMonitor(server->config_);
    if (!local_ && config_.host.empty()) showSettings();
    if (!trayReady) {
        // Keep settings reachable if Explorer cannot create a tray icon.
        for (auto* server : servers()) if (server->window_) ShowWindow(server->window_, showCommand);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool handled = hostTrustUi_ && hostTrustUi_->translate(message);
        if (handled) continue;
        for (auto* server : servers()) {
            if (server->settingsWindow_ && IsWindowVisible(server->settingsWindow_) &&
                IsDialogMessageW(server->settingsWindow_, &message)) { handled = true; break; }
        }
        if (!handled) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    for (auto* server : servers()) {
        server->stopMonitor();
        server->stopTransfer();
    }
    quitApplication();
    return static_cast<int>(message.wParam);
}

bool App::registerClasses() {
    WNDCLASSEXW mainClass{sizeof(mainClass)};
    mainClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    mainClass.lpfnWndProc = mainWindowProc;
    mainClass.hInstance = instance_;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_SSHHELPER));
    mainClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_SSHHELPER),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    mainClass.hbrBackground = nullptr;
    mainClass.lpszClassName = MainClass;
    if (!RegisterClassExW(&mainClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    WNDCLASSEXW settingsClass{sizeof(settingsClass)};
    settingsClass.style = CS_DROPSHADOW | CS_HREDRAW | CS_VREDRAW;
    settingsClass.lpfnWndProc = settingsWindowProc;
    settingsClass.hInstance = instance_;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hIcon = mainClass.hIcon;
    settingsClass.hIconSm = mainClass.hIconSm;
    settingsClass.hbrBackground = nullptr;
    settingsClass.lpszClassName = SettingsClass;
    return RegisterClassExW(&settingsClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool App::createMainWindow(int showCommand) {
    transfer_ = {};
    snapshot_ = {};
    collapseMode_ = CollapseMode::Expanded;
    animating_ = false;
    hasExpandedRect_ = false;
    compactPointerDown_ = false;
    titlePointerDown_ = false;
    compactDragging_ = false;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = GetDpiForSystem();
    const int width = pixels(MainWidth, dpi);
    const int height = pixels(MainHeight, dpi);
    const int margin = pixels(24, dpi);
    const int rows = std::max(1, (int(work.bottom - work.top) - margin) / (height + margin));
    const int columns = std::max(1, (int(work.right - work.left) - margin) / (width + margin));
    const int slot = static_cast<int>(windowSlot_) % (rows * columns);
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, MainClass,
        wide(windowTitle()).c_str(),
        WS_POPUP, std::max(int(work.left), int(work.right) - (slot / rows + 1) * (width + margin)),
        std::max(int(work.top), int(work.bottom) - (slot % rows + 1) * (height + margin)),
        width, height, nullptr, nullptr, instance_, this);
    if (!window_) return false;
    {
        std::lock_guard lock(messageMutex_);
        messageWindow_ = window_;
    }
    DragAcceptFiles(window_, local_ ? FALSE : TRUE);
    constexpr DWORD roundPreference = 2;
    constexpr auto cornerAttribute = static_cast<DWMWINDOWATTRIBUTE>(33);
    DwmSetWindowAttribute(window_, cornerAttribute, &roundPreference, sizeof(roundPreference));
    ShowWindow(window_, showCommand);
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
    case WM_DROPFILES: dropFiles(reinterpret_cast<HDROP>(wParam)); return 0;
    case WM_PAINT: paint(); return 0;
    // Direct2D presents a complete frame; do not erase the front buffer first.
    case WM_ERASEBKGND: return 1;
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
        if (renderTarget_) renderTarget_->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
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
        {
            POINT cursor{};
            if (GetCursorPos(&cursor) && ScreenToClient(window, &cursor)) {
                const UINT dpi = GetDpiForWindow(window);
                if (titleHit(dips(cursor.x, dpi), dips(cursor.y, dpi))) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;
    case WM_NCHITTEST: {
        if (animating_ || isCollapsed()) return HTCLIENT;
        const auto result = DefWindowProcW(window, message, wParam, lParam);
        if (result == HTCLIENT) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(window, &point);
            const UINT dpi = GetDpiForWindow(window);
            if (titleHit(dips(point.x, dpi), dips(point.y, dpi))) return HTCLIENT;
            if (dips(point.y, dpi) < 48 && dips(point.x, dpi) < MainWidth - 120) return HTCAPTION;
        }
        return result;
    }
    case WM_NCLBUTTONDOWN:
        hideSettings();
        break;
    case WM_LBUTTONDOWN: {
        const UINT dpi = GetDpiForWindow(window);
        const int x = dips(GET_X_LPARAM(lParam), dpi), y = dips(GET_Y_LPARAM(lParam), dpi);
        const bool gear = !isCollapsed() && y < 48 && x > MainWidth - 112 && x <= MainWidth - 38;
        if (!gear) hideSettings();
        if (animating_) return 0;
        if (isCollapsed() || titleHit(x, y)) {
            GetCursorPos(&compactPressCursor_);
            GetWindowRect(window_, &compactPressRect_);
            compactPointerDown_ = true;
            titlePointerDown_ = !isCollapsed();
            compactDragging_ = false;
            SetCapture(window_);
            return 0;
        }
        break;
    }
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
            titlePointerDown_ = false;
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
            const bool title = titlePointerDown_;
            compactPointerDown_ = false;
            compactDragging_ = false;
            titlePointerDown_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            if (title) {
                if (dragged) {
                    GetWindowRect(window_, &expandedRect_);
                    hasExpandedRect_ = true;
                    handleEdgeDock();
                } else {
                    const UINT dpi = GetDpiForWindow(window);
                    if (titleHit(dips(GET_X_LPARAM(lParam), dpi), dips(GET_Y_LPARAM(lParam), dpi))) collapseNearestSide();
                }
            } else if (dragged) snapCompactAfterDrag();
            else restoreExpanded();
            return 0;
        }
        if (isCollapsed()) return 0;
        const UINT dpi = GetDpiForWindow(window);
        const int x = dips(GET_X_LPARAM(lParam), dpi), y = dips(GET_Y_LPARAM(lParam), dpi);
        if (y < 48 && x > MainWidth - 38) hideToTray();
        else if (y < 48 && x > MainWidth - 112) toggleSettings();
        else if (y >= 266 && y < 288 && x < 210) openLocalFolder();
        return 0;
    }
    case SnapshotMessage: {
        std::unique_ptr<TelemetrySnapshot> value(reinterpret_cast<TelemetrySnapshot*>(lParam));
        if (wParam != monitorGeneration_) return 0;
        snapshot_ = std::move(*value);
        if (snapshot_.connected && showWhenConnected_) {
            showWhenConnected_ = false;
            showAtBottomRight();
        }
        if (snapshot_.connected && !snapshot_.hostName.empty() &&
            detectedHostName_ != snapshot_.hostName) {
            detectedHostName_ = snapshot_.hostName;
            SetWindowTextW(window_, wide(windowTitle()).c_str());
        }
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case ConfirmTransferMessage: {
        std::unique_ptr<std::shared_ptr<TransferDecision>> payload(
            reinterpret_cast<std::shared_ptr<TransferDecision>*>(lParam));
        auto decision = *payload;
        if (wParam != transferGeneration_ || !transfer_.active) { decision->result = 0; return 0; }
        const auto message = std::wstring(decision->direction == TransferDirection::Upload ? L"上传 " : L"下载 ") +
            std::to_wstring(decision->count) + L" 个文件（" + humanBytes(decision->bytes) + L"）？\n\n" +
            L"来源：" + wide(decision->source) + L"\n目标：" + wide(decision->destination) +
            L"\n\n完成后替换同名文件；未完成文件保留为临时文件。";
        const int answer = MessageBoxW(window, message.c_str(), L"确认批量传输",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        decision->result = answer == IDYES && wParam == transferGeneration_ ? 1 : 0;
        return 0;
    }
    case TransferMessage: {
        std::unique_ptr<TransferProgress> value(reinterpret_cast<TransferProgress*>(lParam));
        if (wParam != transferGeneration_) return 0;
        transfer_ = std::move(*value);
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE: hideToTray(); return 0;
    case WM_DESTROY: {
        stopMonitor();
        stopTransfer();
        cancelPendingHostTrust();
        {
            std::lock_guard lock(messageMutex_);
            messageWindow_ = nullptr;
        }
        // No producer can post now; release payloads already queued for this HWND.
        MSG pending{};
        while (PeekMessageW(&pending, window, SnapshotMessage, ConfirmTransferMessage, PM_REMOVE)) {
            if (pending.message == SnapshotMessage)
                delete reinterpret_cast<TelemetrySnapshot*>(pending.lParam);
            else if (pending.message == TransferMessage)
                delete reinterpret_cast<TransferProgress*>(pending.lParam);
            else {
                auto* decision = reinterpret_cast<std::shared_ptr<TransferDecision>*>(pending.lParam);
                (*decision)->result = 0; delete decision;
            }
        }
        KillTimer(window_, AnimationTimerId);
        window_ = nullptr;
        discardGraphics();
        if (!local_ && config_.host.empty()) deleted_ = true;
        const auto open = servers();
        if (!root_->trayWindow_ && std::none_of(open.begin(), open.end(), [](App* app) { return app->window_ != nullptr; }))
            PostQuitMessage(0);
        return 0;
    }
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
    const auto dpi = static_cast<float>(GetDpiForWindow(window_));
    renderTarget_->SetDpi(dpi, dpi);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(foreground_), &textBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(foreground_), &mutedBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(0x087FF5), &accentBrush_);
    renderTarget_->CreateSolidColorBrush(D2D1::ColorF(background_), &surfaceBrush_);
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
    if (!ensureGraphics()) {
        // GDI is only a fallback. Clearing the visible HWND before EndDraw exposes
        // a blank frame on every telemetry/progress update.
        HBRUSH background = CreateSolidBrush(RGB(background_ >> 16, (background_ >> 8) & 255, background_ & 255));
        FillRect(paintDc, &client, background);
        DeleteObject(background);
        SetBkMode(paintDc, TRANSPARENT);
        SetTextColor(paintDc, RGB(foreground_ >> 16, (foreground_ >> 8) & 255, foreground_ & 255));
        const wchar_t* message = L"sshhelper graphics initialization failed";
        TextOutW(paintDc, 18, 18, message, static_cast<int>(wcslen(message)));
        EndPaint(window_, &paint);
        return;
    }
    renderTarget_->BeginDraw();
    renderTarget_->Clear(D2D1::ColorF(background_));

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
    const std::wstring latency = local_ ? L"本机" : (snapshot_.connected ? numberText(snapshot_.sshLatencyMs) : L"—");
    IDWriteTextFormat* compact = compactFormat_ ? compactFormat_ : labelFormat_;

    if (side) {
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(18, 15), 4, 4), statusBrush);
        drawText(renderTarget_, compact, textBrush_, latency, D2D1::RectF(2, 30, 34, 53));
        drawText(renderTarget_, compact, mutedBrush_, local_ ? L"" : L"ms", D2D1::RectF(2, 50, 34, 72));
    } else if (horizontal) {
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(14, 16), 4, 4), statusBrush);
        drawText(renderTarget_, compact, mutedBrush_, latency + (local_ ? L"" : L" ms"), D2D1::RectF(24, 1, 96, 31));
    } else {
        const auto title = local_ && !detectedHostName_.empty() ? detectedHostName_ : windowTitle();
        drawFittedText(renderTarget_, writeFactory_, titleFormat_, textBrush_, wide(title), headerTextRect(18, 140));
        renderTarget_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(HeaderStatusX, HeaderCenterY), 4, 4), statusBrush);
        drawText(renderTarget_, labelFormat_, mutedBrush_, wide(snapshot_.status), headerTextRect(160, 236));
        drawText(renderTarget_, compact, mutedBrush_, local_ ? L"本机" : (snapshot_.connected ? latency + L" ms" : L"—"),
            headerTextRect(238, 300));
        drawGearIcon(renderTarget_, d2dFactory_, mutedBrush_, surfaceBrush_, D2D1::Point2F(314, HeaderCenterY));
        drawText(renderTarget_, titleFormat_, mutedBrush_, L"×", D2D1::RectF(348, 9, 378, 42));
        renderTarget_->DrawLine(D2D1::Point2F(0, 49), D2D1::Point2F(MainWidth, 49), lineBrush_, 1);
    }
    release(statusBrush);

    if (collapseMode_ == CollapseMode::Expanded) {
        auto card = [&](float x, float y, float width, const std::wstring& label,
                        const std::wstring& value, float level, IDWriteTextFormat* valueFormat = nullptr,
                        const std::wstring& percentage = {}) {
            const auto box = D2D1::RoundedRect(D2D1::RectF(x, y, x + width, y + 66), 11, 11);
            renderTarget_->FillRoundedRectangle(box, surfaceBrush_);
            renderTarget_->DrawRoundedRectangle(box, lineBrush_, 1);
            drawText(renderTarget_, labelFormat_, mutedBrush_, label,
                D2D1::RectF(x + 6, y + 8, x + width - (percentage.empty() ? 6 : 40), y + 27));
            if (!percentage.empty()) {
                const auto alignment = labelFormat_->GetTextAlignment();
                labelFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                drawText(renderTarget_, labelFormat_, mutedBrush_, percentage,
                    D2D1::RectF(x + width - 40, y + 8, x + width - 6, y + 27));
                labelFormat_->SetTextAlignment(alignment);
            }
            const auto valueBox = D2D1::RectF(valueFormat ? x + 2 : x + 10, y + 26,
                valueFormat ? x + width - 2 : x + width - 8, y + 57);
            if (valueFormat)
                drawFittedText(renderTarget_, writeFactory_, valueFormat, textBrush_, value, valueBox);
            else drawText(renderTarget_, metricFormat_, textBrush_, value, valueBox);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 10, y + 58, x + width - 10, y + 61), 2, 2), lineBrush_);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 10, y + 58, x + 10 + (width - 20) * std::clamp(level, 0.0f, 1.0f), y + 61), 2, 2), accentBrush_);
        };

        const bool thermalZone = local_ && snapshot_.packageTemperature < 0 && snapshot_.thermalZoneTemperature >= 0;
        const double temperature = thermalZone ? snapshot_.thermalZoneTemperature : snapshot_.packageTemperature;
        const bool cpuLoad = local_ && temperature < 0 && snapshot_.cpuUsagePercent >= 0;
        const std::wstring cpuLabel = thermalZone ? L"CPU温区" : (cpuLoad ? L"CPU负载" : powerLabel(L"CPU", L"CPU·—W", snapshot_.packagePowerWatts));
        std::wstring cpu = temperature >= 0 ? numberText(temperature, L"°C") :
            (cpuLoad ? numberText(snapshot_.cpuUsagePercent, L"%") : L"—");
        std::wstring gpu = !snapshot_.gpus.empty() && snapshot_.gpus[0].temperature >= 0 ? numberText(snapshot_.gpus[0].temperature, L"°C") : L"—";
        const double gpuPower = snapshot_.gpus.empty() ? -1.0 : snapshot_.gpus[0].powerWatts;
        card(14, 62, 82, cpuLabel, cpu,
            static_cast<float>(cpuLoad ? snapshot_.cpuUsagePercent / 100.0 : std::max(0.0, temperature) / 100.0));
        card(104, 62, 82, powerLabel(L"GPU", L"GPU", gpuPower), gpu,
            !snapshot_.gpus.empty() ? static_cast<float>(snapshot_.gpus[0].temperature / 100.0) : 0);
        card(194, 62, 82, L"RAM", snapshot_.memoryTotal ? storageText(snapshot_.memoryUsed) : L"—",
            ratio(snapshot_.memoryUsed, snapshot_.memoryTotal), capacityFormat_ ? capacityFormat_ : compactFormat_,
            snapshot_.memoryTotal ? numberText(ratio(snapshot_.memoryUsed, snapshot_.memoryTotal) * 100, L"%") : L"—");
        card(284, 62, 82, L"SSD", snapshot_.diskTotal ? storageText(snapshot_.diskUsed) : L"—",
            ratio(snapshot_.diskUsed, snapshot_.diskTotal), capacityFormat_ ? capacityFormat_ : compactFormat_,
            snapshot_.diskTotal ? numberText(ratio(snapshot_.diskUsed, snapshot_.diskTotal) * 100, L"%") : L"—");

        std::vector<double> coreTemperatures;
        coreTemperatures.reserve(snapshot_.coreTemperatures.size());
        for (const auto& sensor : snapshot_.coreTemperatures) {
            if (std::isfinite(sensor.value) && sensor.value >= 0)
                coreTemperatures.push_back(sensor.value);
        }
        std::sort(coreTemperatures.begin(), coreTemperatures.end(),
            [](double left, double right) { return left > right; });
        if (local_) {
            const auto topology = snapshot_.cpuPhysicalCores && snapshot_.cpuLogicalProcessors ?
                std::to_wstring(snapshot_.cpuPhysicalCores) + L"C/" + std::to_wstring(snapshot_.cpuLogicalProcessors) + L"T" : L"—";
            drawParameterRow(renderTarget_, writeFactory_, labelFormat_, compact, mutedBrush_, 139, 163,
                {L"CPU:", snapshot_.cpuUsagePercent >= 0 ? numberText(snapshot_.cpuUsagePercent, L"%") : L"—",
                 snapshot_.cpuClockMHz > 0 ? numberText(snapshot_.cpuClockMHz, L"MHz") : L"—", topology});
        } else {
            std::array<std::wstring, 4> coreValues{L"Cores", L"—", L"", L""};
            const std::size_t count = std::min<std::size_t>(3, coreTemperatures.size());
            for (std::size_t index = 0; index < count; ++index)
                coreValues[index + 1] = numberText(coreTemperatures[index], L"°C");
            drawParameterRow(renderTarget_, writeFactory_, labelFormat_, compact, mutedBrush_, 139, 163,
                coreValues, {mutedBrush_, count ? rankRedBrush_ : mutedBrush_, rankYellowBrush_, rankGreenBrush_});
        }

        const double fanPercent = snapshot_.gpus.empty() ? -1.0 : snapshot_.gpus[0].fanPercent;
        const std::wstring fan = L"FAN:" + (std::isfinite(fanPercent) && fanPercent >= 0.0 && fanPercent <= 100.0
            ? numberText(fanPercent, L"%") : L"—");
        const std::wstring ssd = std::isfinite(snapshot_.ssdTemperature) && snapshot_.ssdTemperature >= 0.0
            ? numberText(snapshot_.ssdTemperature, L"°C") : L"—";
        std::wstring vramPercent = L"—%";
        double memoryClock = -1.0;
        if (!snapshot_.gpus.empty()) {
            const auto& device = snapshot_.gpus[0];
            memoryClock = device.memoryClockMHz;
            if (device.memoryTotal && device.memoryUsed <= device.memoryTotal) {
                vramPercent = numberText(100.0 * static_cast<double>(device.memoryUsed) / device.memoryTotal, L"%");
            }
        }
        const std::wstring memoryRate = snapshot_.memoryRateMTs
            ? std::to_wstring(snapshot_.memoryRateMTs) + L"MT/s" : L"—";
        const std::wstring vramClock = std::wstring{} + (std::isfinite(memoryClock) && memoryClock >= 0.0
            ? numberText(memoryClock, L"MHz") : L"—");
        const std::wstring swap = snapshot_.swapAvailable
            ? (snapshot_.swapUsed ? humanBytes(snapshot_.swapUsed, false) : std::wstring(L"0 B")) : L"—";
        drawParameterRow(renderTarget_, writeFactory_, labelFormat_, labelFormat_, mutedBrush_, 164, 186,
            {L"GPU:", vramPercent, vramClock, fan});
        drawParameterRow(renderTarget_, writeFactory_, labelFormat_, labelFormat_, mutedBrush_, 188, 210,
            {L"SSD", ssd, memoryRate, swap});

        renderTarget_->DrawLine(D2D1::Point2F(14, 216), D2D1::Point2F(366, 216), lineBrush_, 1);
        const auto valueAlignment = compact->GetTextAlignment();
        compact->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        drawText(renderTarget_, compact, textBrush_, L"↓ " + humanBytes(snapshot_.networkDown, true), D2D1::RectF(18, 222, 128, 254));
        drawText(renderTarget_, compact, textBrush_, L"↑ " + humanBytes(snapshot_.networkUp, true), D2D1::RectF(136, 222, 246, 254));
        compact->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        drawText(renderTarget_, compact, textBrush_, snapshot_.connected ? linkSpeedText(snapshot_.networkLinkMbps) : L"—",
            D2D1::RectF(254, 222, 362, 254));
        compact->SetTextAlignment(valueAlignment);

        if (transfer_.active || transfer_.completed || transfer_.failed) {
            const float progressValue = transfer_.completed ? 1.0f : ratio(transfer_.transferred, transfer_.total);
            drawText(renderTarget_, labelFormat_, textBrush_, (transfer_.direction == TransferDirection::Upload ? L"↑ " : L"↓ ") + wide((transfer_.filesTotal ? std::to_string(transfer_.filesCompleted) + "/" +
                std::to_string(transfer_.filesTotal) + " " : "") + transfer_.fileName), D2D1::RectF(18, 266, 210, 283));
            drawText(renderTarget_, labelFormat_, mutedBrush_, transfer_.active ? (transfer_.message == "Verifying partial file" ? L"校验断点…" : humanBytes(transfer_.bytesPerSecond, true)) : wide(transfer_.message), D2D1::RectF(220, 266, 360, 283));
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, 288, 362, 293), 3, 3), lineBrush_);
            renderTarget_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(18, 288, 18 + 344 * progressValue, 293), 3, 3), accentBrush_);
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

void App::collapseNearestSide() {
    if (isCollapsed() || animating_) return;
    RECT current{};
    GetWindowRect(window_, &current);
    const auto targetMonitor = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(targetMonitor, &monitor)) return;
    const auto leftDistance = std::abs(current.left - monitor.rcWork.left);
    const auto rightDistance = std::abs(current.right - monitor.rcWork.right);
    collapseSide(leftDistance <= rightDistance, targetMonitor);
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
    RECT compact{};
    GetWindowRect(window_, &compact);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const UINT dpi = GetDpiForWindow(window_);
    const int width = pixels(MainWidth, dpi);
    const int height = pixels(MainHeight, dpi);
    const int centerX = compact.left + (compact.right - compact.left) / 2;
    const int centerY = compact.top + (compact.bottom - compact.top) / 2;
    const int maxX = std::max(static_cast<int>(monitor.rcWork.left),
        static_cast<int>(monitor.rcWork.right) - width);
    const int maxY = std::max(static_cast<int>(monitor.rcWork.top),
        static_cast<int>(monitor.rcWork.bottom) - height);
    const int x = std::clamp(centerX - width / 2,
        static_cast<int>(monitor.rcWork.left), maxX);
    const int y = std::clamp(centerY - height / 2,
        static_cast<int>(monitor.rcWork.top), maxY);
    const RECT target{x, y, x + width, y + height};
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
    uiFont_ = makeFont(13, FW_NORMAL);
    settingsTitleFont_ = makeFont(17, FW_SEMIBOLD);
    settingsLabelFont_ = makeFont(12, FW_NORMAL);
    settingsButtonFont_ = makeFont(13, FW_NORMAL);

    if (settingsWindow_) {
        constexpr int edits[] = {HostEdit, PortEdit, UserEdit, PasswordEdit, KeyEdit, LocalEdit, RemoteEdit, ServerName, RefreshIntervalEdit};
        constexpr int buttons[] = {BrowseLocal, SaveConnect, UploadButton, DownloadButton, NewServer, ReopenServers, DeleteServer, BackgroundColor, ForegroundColor, OpenLocalFolder, CancelTransfer, ConnectionTab, TransferTab, AppearanceTab};
        for (int id : edits)
            SendMessageW(GetDlgItem(settingsWindow_, id), WM_SETFONT,
                reinterpret_cast<WPARAM>(uiFont_), TRUE);
        for (int id : buttons)
            SendMessageW(GetDlgItem(settingsWindow_, id), WM_SETFONT,
                reinterpret_cast<WPARAM>(settingsButtonFont_), TRUE);
        for (int id : {EntireFolder, SelectedFiles})
            SendMessageW(GetDlgItem(settingsWindow_, id), WM_SETFONT,
                reinterpret_cast<WPARAM>(uiFont_), TRUE);
    }
    if (oldUi) DeleteObject(oldUi);
    if (oldTitle) DeleteObject(oldTitle);
    if (oldLabel) DeleteObject(oldLabel);
    if (oldButton) DeleteObject(oldButton);
}

void App::layoutSettings(UINT dpi) {
    if (!settingsWindow_) return;
    auto p = [dpi](int v) { return pixels(v, dpi); };
    auto move = [&](int id, int x, int y, int w, int h) {
        MoveWindow(GetDlgItem(settingsWindow_, id), p(x), p(y), p(w), p(h), FALSE);
    };
    move(ConnectionTab, 24, 76, 140, 32);
    move(TransferTab, 170, 76, 140, 32);
    move(AppearanceTab, 316, 76, 140, 32);
    move(ServerName, 36, 161, 408, 22);
    move(HostEdit, 36, 231, 286, 22);
    move(PortEdit, 352, 231, 92, 22);
    move(UserEdit, 36, 301, 180, 22);
    move(PasswordEdit, 260, 301, 184, 22);
    move(KeyEdit, 36, 371, 408, 22);
    move(NewServer, 24, 433, 132, 32);
    move(ReopenServers, 174, 433, 132, 32);
    move(DeleteServer, 324, 433, 132, 32);
    move(LocalEdit, 36, 161, 238, 22);
    move(BrowseLocal, 294, 155, 72, 34);
    move(OpenLocalFolder, 378, 155, 66, 34);
    move(RemoteEdit, 36, 241, 408, 22);
    move(EntireFolder, 30, 310, 174, 34);
    move(SelectedFiles, 212, 310, 238, 34);
    move(BackgroundColor, 30, 156, 198, 46);
    move(ForegroundColor, 252, 156, 198, 46);
    move(RefreshIntervalEdit, 344, 260, 100, 22);
    move(CancelTransfer, 24, 496, 116, 34);
    move(UploadButton, 234, 496, 104, 34);
    move(DownloadButton, 350, 496, 106, 34);
    move(SaveConnect, 300, 496, 156, 34);
    constexpr int edits[] = {HostEdit, PortEdit, UserEdit, PasswordEdit, KeyEdit,
        LocalEdit, RemoteEdit, ServerName, RefreshIntervalEdit};
    for (int id : edits) SendDlgItemMessageW(settingsWindow_, id, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(p(2), p(2)));
    selectSettingsTab(settingsTab_);
}

void App::selectSettingsTab(int tab) {
    settingsTab_ = local_ && tab == 1 ? 0 : std::clamp(tab, 0, 2);
    auto visible = [&](int id, bool show) {
        ShowWindow(GetDlgItem(settingsWindow_, id), show ? SW_SHOWNA : SW_HIDE);
    };
    visible(ServerName, settingsTab_ == 0);
    for (int id : {HostEdit, PortEdit, UserEdit, PasswordEdit, KeyEdit, DeleteServer})
        visible(id, settingsTab_ == 0 && !local_);
    for (int id : {NewServer, ReopenServers}) visible(id, settingsTab_ == 0);
    EnableWindow(GetDlgItem(settingsWindow_, TransferTab), !local_);
    for (int id : {LocalEdit, RemoteEdit, BrowseLocal, OpenLocalFolder, EntireFolder,
        SelectedFiles, UploadButton, DownloadButton, CancelTransfer}) visible(id, settingsTab_ == 1);
    for (int id : {BackgroundColor, ForegroundColor, RefreshIntervalEdit})
        visible(id, settingsTab_ == 2);
    visible(SaveConnect, settingsTab_ != 1);
    RedrawWindow(settingsWindow_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void App::paintSettings(HDC destination) {
    PAINTSTRUCT paint{};
    HDC target = destination ? destination : BeginPaint(settingsWindow_, &paint);
    RECT client{};
    GetClientRect(settingsWindow_, &client);
    HDC buffer = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    const UINT dpi = settingsLayoutDpi_;
    auto p = [dpi](int v) { return pixels(v, dpi); };
    const auto bg = settingsColor(background_), fg = settingsColor(foreground_);
    const auto line = blendColor(bg, fg, 0.14f), muted = blendColor(bg, fg, 0.62f);
    // Paint a complete buffered frame. GDI is retained only if Direct2D cannot render.
    for (const bool modern : {true, false}) {
        SettingsCanvas canvas(d2dFactory_, writeFactory_, settingsRenderTarget_, buffer, client, dpi,
            modern && settingsTitleFormat_ && settingsLabelFormat_);
        canvas.clear(bg);
        canvas.rounded(client, p(24), bg, line, std::max(1, p(1)));
        auto* format = settingsTitleFormat_;
        HFONT fallback = settingsTitleFont_;
        auto text = [&](const std::wstring& value, int x, int y, int w, int h, COLORREF color) {
            const RECT rect{p(x), p(y), p(x + w), p(y + h)};
            canvas.text(value, rect, format, fallback, color);
        };
        text(local_ ? L"本机监控设置" : L"服务器设置", 24, 14, 370, 26, fg);
        format = settingsLabelFormat_; fallback = settingsLabelFont_;
        text(wide(windowTitle()), 24, 42, 392, 20, muted);
        canvas.text(L"×", RECT{p(430), p(16), p(456), p(44)}, format, fallback, muted,
            DWRITE_TEXT_ALIGNMENT_CENTER);
        auto field = [&](int x, int y, int w, int h = 34) {
            canvas.rounded(RECT{p(x), p(y), p(x + w), p(y + h)}, p(12),
                blendColor(bg, fg, 0.035f), line, std::max(1, p(1)));
        };
        if (settingsTab_ == 0 && local_) {
            text(L"显示名称", 30, 132, 408, 20, muted); field(30, 155, 420);
            text(L"当前 Windows 主机 · 自动监控", 30, 216, 420, 28, fg);
            text(L"无需填写 SSH 地址、账号或密码", 30, 256, 420, 24, muted);
            text(L"温度与功耗取决于硬件和传感器支持", 30, 292, 420, 24, muted);
            text(L"显示“CPU温区”时，数值来自 Windows 温区", 30, 328, 420, 24, muted);
        } else if (settingsTab_ == 0) {
            text(L"显示名称", 30, 132, 408, 20, muted); field(30, 155, 420);
            text(L"主机地址", 30, 202, 290, 20, muted); field(30, 225, 298);
            text(L"端口", 346, 202, 100, 20, muted); field(346, 225, 104);
            text(L"用户名", 30, 272, 190, 20, muted); field(30, 295, 192);
            text(L"密码 / 密钥口令", 254, 272, 196, 20, muted); field(254, 295, 196);
            text(L"私钥路径 · 可选", 30, 342, 420, 20, muted); field(30, 365, 420);
        } else if (settingsTab_ == 1) {
            text(L"本机文件夹", 30, 132, 420, 20, muted); field(30, 155, 250);
            text(L"服务器文件夹", 30, 212, 420, 20, muted); field(30, 235, 420);
            text(L"传输范围", 30, 284, 420, 20, muted);
            text(L"上传：本机 → 服务器", 30, 366, 420, 24, fg);
            text(L"下载：服务器 → 本机", 30, 394, 420, 24, fg);
            text(L"支持断点续传，超过 5 个文件时再次确认", 30, 436, 420, 22, muted);
        } else {
            text(L"窗口配色", 30, 128, 420, 20, muted);
            text(L"浮窗、收起状态与设置页使用同一配色", 30, 212, 420, 20, muted);
            text(L"监控刷新间隔（1–3600 秒）", 30, 260, 300, 22, muted); field(338, 254, 112, 36);
        }
        canvas.line(p(24), p(480), p(456), p(480), line, std::max(1, p(1)));
        if (settingsTab_ != 1) text(settingsTab_ == 2 ? L"监控设置仅用于当前服务器" :
            (local_ ? L"本机监控无需 SSH 连接" : L"连接信息仅用于当前服务器"), 24, 500, 264, 26, muted);
        if (canvas.finish()) break;
    }
    BitBlt(target, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap); DeleteObject(bitmap); DeleteDC(buffer);
    if (!destination) EndPaint(settingsWindow_, &paint);
}

void App::drawSettingsButton(const DRAWITEMSTRUCT& item) {
    const bool tab = item.CtlID >= ConnectionTab && item.CtlID <= AppearanceTab;
    const bool choice = item.CtlID == EntireFolder || item.CtlID == SelectedFiles;
    const bool selected = (tab && int(item.CtlID - ConnectionTab) == settingsTab_) ||
        (choice && ((item.CtlID == SelectedFiles) == partialTransfer_));
    const bool primary = item.CtlID == SaveConnect;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    auto p = [this](int v) { return pixels(v, settingsLayoutDpi_); };
    const auto bg = settingsColor(background_), fg = settingsColor(foreground_);
    const COLORREF accent = RGB(8, 127, 245);
    COLORREF fill = primary ? accent : blendColor(bg, selected ? accent : fg, selected ? 0.14f : 0.035f);
    COLORREF border = primary || focused || selected ? accent : blendColor(bg, fg, 0.14f);
    COLORREF color = primary ? RGB(255, 255, 255) : fg;
    if (item.CtlID == DeleteServer) color = RGB(218, 76, 76);
    if (pressed) fill = blendColor(fill, fg, 0.10f);
    if (disabled) { fill = bg; color = blendColor(bg, fg, 0.35f); }
    wchar_t text[64]{};
    GetWindowTextW(item.hwndItem, text, 64);
    for (const bool modern : {true, false}) {
        SettingsCanvas canvas(d2dFactory_, writeFactory_, settingsRenderTarget_, item.hDC, item.rcItem,
            settingsLayoutDpi_, modern && settingsButtonFormat_);
        canvas.clear(bg);
        canvas.rounded(item.rcItem, p(12), fill, border, std::max(1, p(focused ? 2 : 1)));
        RECT textRect = item.rcItem;
        if (item.CtlID == BackgroundColor || item.CtlID == ForegroundColor) {
            canvas.ellipse(RECT{item.rcItem.left + p(16), item.rcItem.top + p(14),
                item.rcItem.left + p(34), item.rcItem.top + p(32)},
                item.CtlID == BackgroundColor ? bg : fg, border, std::max(1, p(1)));
            textRect.left += p(26);
        }
        if (pressed) OffsetRect(&textRect, 0, p(1));
        canvas.text(text, textRect, settingsButtonFormat_, settingsButtonFont_, color,
            DWRITE_TEXT_ALIGNMENT_CENTER);
        if (canvas.finish()) break;
    }
}

void App::createSettingsWindow() {
    const UINT dpi = GetDpiForWindow(window_);
    settingsLayoutDpi_ = dpi;
    settingsLoaded_ = false;
    settingsWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, SettingsClass, L"sshhelper 设置",
        WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        pixels(SettingsWidth, dpi), pixels(SettingsHeight, dpi),
        window_, nullptr, instance_, this);
    if (!settingsWindow_) return;
    constexpr DWORD roundPreference = 2;
    constexpr auto cornerAttribute = static_cast<DWMWINDOWATTRIBUTE>(33);
    DwmSetWindowAttribute(settingsWindow_, cornerAttribute, &roundPreference, sizeof(roundPreference));
    rebuildSettingsFonts(dpi);

    addControl(settingsWindow_, WC_BUTTONW, local_ ? L"本机" : L"连接", BS_OWNERDRAW, 0, 0, 1, 1, ConnectionTab, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"文件传输", BS_OWNERDRAW, 0, 0, 1, 1, TransferTab, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"外观与监控", BS_OWNERDRAW, 0, 0, 1, 1, AppearanceTab, settingsButtonFont_);
    HWND host = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, HostEdit, uiFont_);
    HWND port = addControl(settingsWindow_, WC_EDITW, L"22", ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 1, 1, PortEdit, uiFont_);
    HWND user = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, UserEdit, uiFont_);
    HWND password = addControl(settingsWindow_, WC_EDITW, L"", ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 1, 1, PasswordEdit, uiFont_);
    HWND key = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, KeyEdit, uiFont_);
    HWND local = addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, LocalEdit, uiFont_);
    HWND remote = addControl(settingsWindow_, WC_EDITW, L"/tmp/", ES_AUTOHSCROLL, 0, 0, 1, 1, RemoteEdit, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"浏览", BS_OWNERDRAW, 0, 0, 1, 1, BrowseLocal, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"打开", BS_OWNERDRAW, 0, 0, 1, 1, OpenLocalFolder, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"停止传输", BS_OWNERDRAW, 0, 0, 1, 1, CancelTransfer, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, local_ ? L"保存并监控" : L"连接并监控", BS_OWNERDRAW, 0, 0, 1, 1, SaveConnect, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"上传", BS_OWNERDRAW, 0, 0, 1, 1, UploadButton, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"下载", BS_OWNERDRAW, 0, 0, 1, 1, DownloadButton, settingsButtonFont_);

    addControl(settingsWindow_, WC_BUTTONW, L"整个文件夹", WS_GROUP | BS_OWNERDRAW,
        0, 0, 1, 1, EntireFolder, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"选择文件 / 子目录", BS_OWNERDRAW,
        0, 0, 1, 1, SelectedFiles, uiFont_);
    partialTransfer_ = false;

    SendMessageW(host, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"192.168.1.10"));
    SendMessageW(port, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"22"));
    SendMessageW(user, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"root"));
    SendMessageW(password, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"密码或私钥口令"));
    SendMessageW(key, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"可选：私钥文件路径"));
    SendMessageW(local, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"选择本地文件夹"));
    SendMessageW(remote, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"/tmp/"));
    addControl(settingsWindow_, WC_EDITW, L"", ES_AUTOHSCROLL, 0, 0, 1, 1, ServerName, uiFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"页面颜色", BS_OWNERDRAW, 0, 0, 1, 1, BackgroundColor, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"文字颜色", BS_OWNERDRAW, 0, 0, 1, 1, ForegroundColor, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"新增服务器", BS_OWNERDRAW, 0, 0, 1, 1, NewServer, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"打开全部窗口", BS_OWNERDRAW, 0, 0, 1, 1, ReopenServers, settingsButtonFont_);
    addControl(settingsWindow_, WC_BUTTONW, L"删除此服务器", BS_OWNERDRAW, 0, 0, 1, 1, DeleteServer, settingsButtonFont_);
    addControl(settingsWindow_, WC_EDITW, L"2", ES_NUMBER | ES_AUTOHSCROLL, 0, 0, 1, 1, RefreshIntervalEdit, uiFont_);
    SendDlgItemMessageW(settingsWindow_, RefreshIntervalEdit, EM_SETLIMITTEXT, 4, 0);
    layoutSettings(dpi);
}
void App::positionSettings() {
    RECT owner{};
    GetWindowRect(window_, &owner);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto work = monitor.rcWork;
    const UINT nativeDpi = GetDpiForWindow(window_);
    settingsLayoutDpi_ = std::max(48, std::min({int(nativeDpi),
        MulDiv(int(work.right - work.left) - 24, 96, SettingsWidth),
        MulDiv(int(work.bottom - work.top) - 24, 96, SettingsHeight)}));
    const int w = pixels(SettingsWidth, settingsLayoutDpi_), h = pixels(SettingsHeight, settingsLayoutDpi_);
    const int gap = pixels(12, settingsLayoutDpi_);
    POINT gear{pixels(314, nativeDpi), pixels(27, nativeDpi)};
    if (isCollapsed()) gear = {int(owner.right - owner.left) / 2, int(owner.bottom - owner.top) / 2};
    ClientToScreen(window_, &gear);
    const POINT candidates[] = {
        {owner.right + gap, owner.bottom - h}, {owner.left - gap - w, owner.bottom - h},
        {owner.right - w, owner.top - gap - h}, {owner.right - w, owner.bottom + gap}
    };
    RECT chosen{};
    long long best = LLONG_MAX;
    for (const auto& point : candidates) {
        const int x = std::clamp(int(point.x), int(work.left), std::max(int(work.left), int(work.right) - w));
        const int y = std::clamp(int(point.y), int(work.top), std::max(int(work.top), int(work.bottom) - h));
        RECT box{x, y, x + w, y + h};
        long long score = std::abs(x - int(point.x)) + std::abs(y - int(point.y));
        auto penalize = [&](HWND window) {
            if (!window || !IsWindowVisible(window)) return;
            RECT bounds{}, overlap{};
            GetWindowRect(window, &bounds);
            if (IntersectRect(&overlap, &bounds, &box))
                score += 100LL * (overlap.right - overlap.left) * (overlap.bottom - overlap.top);
        };
        for (auto* server : servers()) {
            penalize(server->window_);
            if (server->settingsWindow_ != settingsWindow_) penalize(server->settingsWindow_);
        }
        if (score < best) { best = score; chosen = box; }
    }
    SetWindowPos(settingsWindow_, HWND_TOP, chosen.left, chosen.top, w, h, SWP_NOACTIVATE);
    // Moving across displays may dispatch WM_DPICHANGED; apply the fitted layout afterward.
    settingsLayoutDpi_ = std::max(48, std::min({int(nativeDpi),
        MulDiv(int(work.right - work.left) - 24, 96, SettingsWidth),
        MulDiv(int(work.bottom - work.top) - 24, 96, SettingsHeight)}));
    rebuildSettingsFonts(settingsLayoutDpi_);
    layoutSettings(settingsLayoutDpi_);
    settingsAnimationOrigin_ = {std::clamp(int(gear.x - chosen.left), 0, w),
        std::clamp(int(gear.y - chosen.top), 0, h)};
}

void App::finishSettingsAnimation() {
    if (!settingsWindow_) return;
    KillTimer(settingsWindow_, SettingsAnimationTimerId);
    const bool closing = settingsClosing_;
    settingsAnimating_ = false;
    settingsClosing_ = false;
    settingsReveal_ = closing ? 0.0f : 1.0f;
    // Hide before removing the clip, otherwise a closing panel flashes full-size.
    if (closing) ShowWindow(settingsWindow_, SW_HIDE);
    SetWindowRgn(settingsWindow_, nullptr, TRUE);
}

void App::tickSettingsAnimation() {
    if (!settingsAnimating_ || !settingsWindow_) return;
    const float t = std::min(1.0f, float(GetTickCount64() - settingsAnimationStarted_) / 200.0f);
    if (t >= 1.0f) { finishSettingsAnimation(); return; }
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    const float target = settingsClosing_ ? 0.0f : 1.0f;
    settingsReveal_ = settingsAnimationFrom_ + (target - settingsAnimationFrom_) * eased;
    const float reveal = settingsReveal_;
    RECT client{}; GetClientRect(settingsWindow_, &client);
    const int left = int(settingsAnimationOrigin_.x * (1.0f - reveal));
    const int top = int(settingsAnimationOrigin_.y * (1.0f - reveal));
    const int right = std::max(left + 1, int(settingsAnimationOrigin_.x +
        (client.right - settingsAnimationOrigin_.x) * reveal));
    const int bottom = std::max(top + 1, int(settingsAnimationOrigin_.y +
        (client.bottom - settingsAnimationOrigin_.y) * reveal));
    HRGN clip = CreateRoundRectRgn(left, top, right, bottom,
        pixels(18, settingsLayoutDpi_), pixels(18, settingsLayoutDpi_));
    if (!SetWindowRgn(settingsWindow_, clip, TRUE)) DeleteObject(clip);
}

void App::startSettingsAnimation(bool opening) {
    if (!settingsWindow_) return;
    if (settingsAnimating_) tickSettingsAnimation();
    settingsClosing_ = !opening;
    settingsAnimationFrom_ = settingsReveal_;
    settingsAnimationStarted_ = GetTickCount64();
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    if (!animations) { finishSettingsAnimation(); return; }
    settingsAnimating_ = true;
    tickSettingsAnimation();
    if (!SetTimer(settingsWindow_, SettingsAnimationTimerId, 15, nullptr)) finishSettingsAnimation();
}

void App::hideSettings(bool animate) {
    if (!settingsWindow_ || !IsWindowVisible(settingsWindow_)) return;
    if (!animate) {
        settingsClosing_ = true;
        finishSettingsAnimation();
    } else if (!settingsClosing_) {
        startSettingsAnimation(false);
    }
}

void App::toggleSettings() {
    if (settingsWindow_ && IsWindowVisible(settingsWindow_) && !settingsClosing_)
        hideSettings();
    else showSettings();
}

bool App::isSettingsRelatedWindow(HWND target) const {
    // Native color/file pickers and message boxes are owned by the panel.
    for (HWND current = target; current; current = GetWindow(current, GW_OWNER))
        if (current == settingsWindow_ || IsChild(settingsWindow_, current)) return true;
    return false;
}

void App::showSettings() {
    if (!settingsWindow_) createSettingsWindow();
    if (!settingsWindow_) return;
    if (IsWindowVisible(settingsWindow_)) {
        if (settingsClosing_) {
            startSettingsAnimation(true);
            ShowWindow(settingsWindow_, SW_SHOWNORMAL);
        } else finishSettingsAnimation();
        SetForegroundWindow(settingsWindow_);
        return;
    }
    if (!settingsLoaded_) {
        SetDlgItemInt(settingsWindow_, RefreshIntervalEdit, refreshIntervalSeconds_.load(), FALSE);
        SetWindowTextW(GetDlgItem(settingsWindow_, ServerName), wide(serverName_).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, HostEdit), wide(config_.host).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, PortEdit), std::to_wstring(config_.port).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, UserEdit), wide(config_.username).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, PasswordEdit), wide(config_.password).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, KeyEdit), wide(config_.privateKeyPath).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, LocalEdit), wide(localPath_).c_str());
        SetWindowTextW(GetDlgItem(settingsWindow_, RemoteEdit), wide(remotePath_.empty() ? "/tmp/" : remotePath_).c_str());
        settingsLoaded_ = true;
    }
    positionSettings();
    settingsReveal_ = 0.0f;
    startSettingsAnimation(true);
    ShowWindow(settingsWindow_, SW_SHOWNORMAL);
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
    case WM_TIMER:
        if (wParam == SettingsAnimationTimerId) { tickSettingsAnimation(); return 0; }
        break;
    case WM_SHOWWINDOW:
        if (!wParam) {
            KillTimer(window, SettingsAnimationTimerId);
            settingsAnimating_ = false;
            settingsClosing_ = false;
            settingsReveal_ = 0.0f;
            SetWindowRgn(window, nullptr, FALSE);
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && IsWindowEnabled(window) &&
            reinterpret_cast<HWND>(lParam) != window_ &&
            !isSettingsRelatedWindow(reinterpret_cast<HWND>(lParam))) hideSettings();
        break;
    case WM_ENTERSIZEMOVE: finishSettingsAnimation(); return 0;
    case WM_PRINTCLIENT: paintSettings(reinterpret_cast<HDC>(wParam)); return 0;
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
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const auto fill = blendColor(settingsColor(background_), settingsColor(foreground_), 0.035f);
        SetBkColor(dc, fill);
        SetTextColor(dc, settingsColor(foreground_));
        SetDCBrushColor(dc, fill);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    case WM_DPICHANGED: {
        finishSettingsAnimation();
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto work = monitor.rcWork;
        settingsLayoutDpi_ = std::max(48, std::min({int(HIWORD(wParam)),
            MulDiv(int(work.right - work.left) - 24, 96, SettingsWidth),
            MulDiv(int(work.bottom - work.top) - 24, 96, SettingsHeight)}));
        const int width = pixels(SettingsWidth, settingsLayoutDpi_), height = pixels(SettingsHeight, settingsLayoutDpi_);
        SetWindowPos(window, nullptr,
            std::clamp(int(suggested->left), int(work.left), std::max(int(work.left), int(work.right) - width)),
            std::clamp(int(suggested->top), int(work.top), std::max(int(work.top), int(work.bottom) - height)),
            width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        rebuildSettingsFonts(settingsLayoutDpi_);
        layoutSettings(settingsLayoutDpi_);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        const UINT dpi = settingsLayoutDpi_;
        const int x = dips(point.x, dpi);
        const int y = dips(point.y, dpi);
        if (y >= 0 && y < SettingsHeaderHeight && x < SettingsWidth - 50) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_LBUTTONUP: {
        const UINT dpi = settingsLayoutDpi_;
        const int x = dips(GET_X_LPARAM(lParam), dpi);
        const int y = dips(GET_Y_LPARAM(lParam), dpi);
        if (y >= 0 && y < SettingsHeaderHeight && x >= SettingsWidth - 50)
            hideSettings();
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            hideSettings();
            return 0;
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDCANCEL: hideSettings(); return 0;
        case ConnectionTab: selectSettingsTab(0); return 0;
        case TransferTab: selectSettingsTab(1); return 0;
        case AppearanceTab: selectSettingsTab(2); return 0;
        case EntireFolder:
        case SelectedFiles:
            partialTransfer_ = LOWORD(wParam) == SelectedFiles;
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        case NewServer: addServer(); return 0;
        case ReopenServers: reopenServers(); return 0;
        case BackgroundColor: chooseTheme(true); return 0;
        case ForegroundColor: chooseTheme(false); return 0;
        case DeleteServer: {
            if (local_) return 0;
            if (MessageBoxW(settingsWindow_, L"删除此服务器的已保存配置并关闭窗口？",
                L"sshhelper", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return 0;
            stopMonitor(); stopTransfer(); cancelPendingHostTrust();
            deleted_ = true;
            if (!saveServers()) { deleted_ = false; return 0; }
            DestroyWindow(window_);
            return 0;
        }
        case OpenLocalFolder: openLocalFolder(); return 0;
        case CancelTransfer:
            if (transfer_.active) {
                stopTransfer();
                transfer_.active = false; transfer_.failed = true;
                transfer_.message = "Cancelled; .part retained";
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        case BrowseLocal: chooseLocalFolder(); return 0;
        case SaveConnect: {
            ConnectionConfig config; std::string local, remote;
            if (readSettings(config, local, remote)) {
                if (!rememberConnection(config, local, remote)) return 0;
                if (!IsWindowVisible(window_)) showWhenConnected_ = true;
                startMonitor(config_); hideSettings();
            }
            return 0;
        }
        case UploadButton: startTransfer(TransferDirection::Upload); return 0;
        case DownloadButton: startTransfer(TransferDirection::Download); return 0;
        }
        break;
    case WM_CLOSE: hideSettings(); return 0;
    case WM_DESTROY:
        KillTimer(window, SettingsAnimationTimerId);
        settingsAnimating_ = false;
        settingsWindow_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
bool App::readSettings(ConnectionConfig& config, std::string& localPath, std::string& remotePath) {
    if (local_) { config = {}; localPath = localPath_; remotePath = remotePath_; return true; }
    config.host = utf8(controlText(settingsWindow_, HostEdit));
    config.username = utf8(controlText(settingsWindow_, UserEdit));
    config.password = utf8(controlText(settingsWindow_, PasswordEdit));
    config.privateKeyPath = utf8(controlText(settingsWindow_, KeyEdit));
    config.keyPassphrase = config.privateKeyPath.empty() ? std::string{} : config.password;
    localPath = utf8(controlText(settingsWindow_, LocalEdit));
    remotePath = utf8(controlText(settingsWindow_, RemoteEdit));
    try {
        const auto value = controlText(settingsWindow_, PortEdit);
        std::size_t used = 0;
        config.port = std::stoi(value, &used);
        if (used != value.size()) config.port = 0;
    } catch (...) { config.port = 0; }
    if (config.host.empty() || config.username.empty() || config.port < 1 || config.port > 65535 ||
        (config.password.empty() && config.privateKeyPath.empty())) {
        MessageBoxW(settingsWindow_, L"请填写主机、端口、用户名，并提供密码或私钥。", L"sshhelper", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

bool App::readRefreshInterval(std::uint32_t& seconds) const {
    const auto input = controlText(settingsWindow_, RefreshIntervalEdit);
    if (input.empty() || input.size() > 4 ||
        !std::all_of(input.begin(), input.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) return false;
    seconds = 0;
    for (wchar_t c : input) seconds = seconds * 10 + static_cast<unsigned>(c - L'0');
    return seconds >= 1 && seconds <= 3600;
}

bool App::rememberConnection(const ConnectionConfig& config, const std::string& local, const std::string& remote) {
    std::uint32_t interval = 2;
    if (!readRefreshInterval(interval)) {
        MessageBoxW(settingsWindow_, L"监控刷新间隔请输入 1–3600 的整数，单位为秒。", L"sshhelper", MB_OK | MB_ICONWARNING);
        selectSettingsTab(2);
        SetFocus(GetDlgItem(settingsWindow_, RefreshIntervalEdit));
        return false;
    }
    const auto old = profile();
    const auto& c = old.connection;
    if (c.host != config.host || c.port != config.port || c.username != config.username ||
        c.password != config.password || c.privateKeyPath != config.privateKeyPath || c.keyPassphrase != config.keyPassphrase) {
        stopMonitor(); stopTransfer(); cancelPendingHostTrust();
    }
    config_ = config;
    localPath_ = local;
    remotePath_ = remote;
    serverName_ = utf8(controlText(settingsWindow_, ServerName));
    if (serverName_.empty()) serverName_ = config_.host;
    refreshIntervalSeconds_ = interval;
    if (!saveServers()) {
        applyProfile(old);
        return false;
    }
    if (old.connection.host != config_.host || old.connection.port != config_.port)
        detectedHostName_.clear();
    SetWindowTextW(window_, wide(windowTitle()).c_str());
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void App::chooseLocalFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择本地文件夹");
    const auto current = controlText(settingsWindow_, LocalEdit);
    IShellItem* initial = nullptr;
    if (!current.empty() && SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
        dialog->SetFolder(initial);
        initial->Release();
    }
    if (SUCCEEDED(dialog->Show(settingsWindow_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetWindowTextW(GetDlgItem(settingsWindow_, LocalEdit), path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void App::postSnapshot(TelemetrySnapshot snapshot, std::uintptr_t generation) {
    auto value = std::make_unique<TelemetrySnapshot>(std::move(snapshot));
    std::lock_guard lock(messageMutex_);
    if (messageWindow_ && PostMessageW(messageWindow_, SnapshotMessage, generation,
        reinterpret_cast<LPARAM>(value.get()))) value.release();
}

void App::postTransfer(TransferProgress progress, std::uintptr_t generation) {
    auto value = std::make_unique<TransferProgress>(std::move(progress));
    std::lock_guard lock(messageMutex_);
    if (messageWindow_ && PostMessageW(messageWindow_, TransferMessage, generation,
        reinterpret_cast<LPARAM>(value.get()))) value.release();
}

void App::waitRefreshInterval(std::stop_token token) const {
    const auto start = std::chrono::steady_clock::now();
    while (!token.stop_requested() && std::chrono::steady_clock::now() - start <
        std::chrono::seconds(refreshIntervalSeconds_.load()))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void App::startMonitor(const ConnectionConfig& config) {
    if (root_->exiting_ || deleted_) return;
    stopMonitor();
    const auto generation = monitorGeneration_;
    auto confirm = hostTrustConfirmation();
    const auto pins = root_->credentialPath_.parent_path() / L"AirSFTP.known_hosts";
    TelemetrySnapshot connecting;
    connecting.status = "Connecting";
    postSnapshot(connecting, generation);
    // Join the cancelled predecessor on the replacement worker, never in a UI handler.
    monitorThread_ = std::jthread([this, config, generation, confirm, pins,
        previous = std::move(monitorThread_)](std::stop_token token) mutable {
        if (previous.joinable()) previous.join();
        if (local_) {
            LocalTelemetry sampler;
            while (!token.stop_requested()) {
                postSnapshot(sampler.sample(), generation);
                waitRefreshInterval(token);
            }
            return;
        }
        while (!token.stop_requested()) {
            SshSession session(token, 15000, pins, confirm);
            TelemetryParser parser; // Reconnects must not reuse counters from an old boot.
            std::string error;
            if (!session.connect(config, error)) {
                TelemetrySnapshot failed; failed.status = error;
                postSnapshot(std::move(failed), generation);
                if (session.requiresUserAction()) break;
                sleepStopped(token, std::chrono::seconds(3));
                continue;
            }
            while (!token.stop_requested()) {
                std::string output; double latency = 0.0;
                if (!session.executeScript(TelemetryParser::probeScript(), output, latency, error)) {
                    TelemetrySnapshot failed; failed.status = error;
                    postSnapshot(std::move(failed), generation);
                    break;
                }
                auto value = parser.parse(output, latency);
                value.fingerprint = session.fingerprint();
                postSnapshot(std::move(value), generation);
                waitRefreshInterval(token);
            }
            session.disconnect();
            sleepStopped(token, std::chrono::seconds(2));
        }
    });
}

void App::stopMonitor() {
    ++monitorGeneration_;
    if (monitorThread_.joinable()) monitorThread_.request_stop();
}

void App::startTransfer(TransferDirection direction) {
    if (local_) return;
    if (transfer_.active) {
        MessageBoxW(settingsWindow_, L"当前正在传输，请先停止或等待完成。", L"sshhelper", MB_OK);
        return;
    }
    ConnectionConfig config; std::string local, remote;
    if (!readSettings(config, local, remote)) return;
    if (local.empty() || remote.empty()) {
        MessageBoxW(settingsWindow_, L"请填写本地和远程文件夹路径。", L"sshhelper", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!std::filesystem::u8path(local).is_absolute() || remote.front() != '/') {
        MessageBoxW(settingsWindow_, L"请使用完整文件夹路径，例如 D:\\data 和 /home/user/data。",
                    L"sshhelper", MB_OK | MB_ICONWARNING);
        return;
    }
    auto confirm = hostTrustConfirmation();
    const auto pins = root_->credentialPath_.parent_path() / L"AirSFTP.known_hosts";
    std::optional<std::vector<std::string>> selection;
    if (partialTransfer_) {
        selection.emplace();
        if (!chooseTransferEntries(settingsWindow_, config, direction,
            direction == TransferDirection::Upload ? local : remote, *selection, confirm, pins)) return;
    }
    if (root_->exiting_ || deleted_ || !window_) return;
    if (!rememberConnection(config, local, remote)) return;
    startMonitor(config_);
    stopTransfer();
    const auto generation = transferGeneration_;
    transferLocalFolder_ = local;
    transfer_ = {}; transfer_.active = true; transfer_.direction = direction;
    transfer_.message = "Scanning / connecting";
    hideSettings();
    transferThread_ = std::jthread([this, config, local, remote, direction, generation, selection, confirm, pins,
        previous = std::move(transferThread_)](std::stop_token token) mutable {
        // Prevent two successive transfers from writing the same destination concurrently.
        if (previous.joinable()) previous.join();
        if (token.stop_requested()) return;
        SshSession session(token, 15000, pins, confirm);
        std::string error;
        TransferProgress state; state.active = true; state.direction = direction;
        const auto& path = direction == TransferDirection::Upload ? local : remote;
        state.fileName = path.substr(path.find_last_of("/\\") + 1);
        state.message = "Connecting"; postTransfer(state, generation);
        if (!session.connect(config, error)) {
            state.active = false; state.failed = true; state.message = error;
            postTransfer(state, generation);
            return;
        }
        session.transferFolder(direction, local, remote, selection,
            [this, generation](const TransferProgress& value) { postTransfer(value, generation); }, error,
            [this, generation, token, direction, local, remote](std::size_t count, std::uint64_t bytes) {
                return confirmTransfer(count, bytes, direction,
                    direction == TransferDirection::Upload ? local : remote,
                    direction == TransferDirection::Upload ? remote : local, generation, token);
            });
    });
}

void App::stopTransfer() {
    ++transferGeneration_;
    if (transferThread_.joinable()) transferThread_.request_stop();
}

HostTrustConfirmation App::hostTrustConfirmation() {
    if (!root_->hostTrustUi_) root_->hostTrustUi_ = std::make_unique<HostTrustUi>(instance_);
    return root_->hostTrustUi_->confirmation(this);
}
void App::cancelPendingHostTrust() {
    if (root_->hostTrustUi_) root_->hostTrustUi_->cancelOwner(this);
}
