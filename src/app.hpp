#pragma once

#include "models.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();
    int run(int showCommand);

private:
    enum class CollapseMode { Expanded, Horizontal, Side };
    static LRESULT CALLBACK mainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK settingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMainMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleSettingsMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool registerClasses();
    bool createMainWindow(int showCommand);
    void createSettingsWindow();
    void showSettings();
    void paintSettings();
    void drawSettingsButton(const DRAWITEMSTRUCT& item);
    void rebuildSettingsFonts(UINT dpi);
    void layoutSettings(UINT dpi);
    void paint();
    void discardGraphics();
    bool ensureGraphics();
    void resizeGraphics();
    void toggleCollapsed();
    void collapseHorizontal(bool dockToTop, HMONITOR monitor = nullptr);
    void collapseSide(bool left, HMONITOR monitor = nullptr);
    void snapCompactAfterDrag();
    void updateRestoreAnchor(HMONITOR monitor, const RECT& compactRect, CollapseMode mode);
    void startWindowAnimation(const RECT& target, CollapseMode targetMode, bool clearExpandedRect);
    void tickWindowAnimation();
    void finishWindowAnimation();
    void restoreExpanded();
    void handleEdgeDock();
    bool isCollapsed() const { return collapseMode_ != CollapseMode::Expanded; }

    bool readSettings(ConnectionConfig& config, std::string& localPath, std::string& remotePath);
    void startMonitor(const ConnectionConfig& config);
    void stopMonitor();
    void startTransfer(TransferDirection direction);
    void stopTransfer();
    void postSnapshot(TelemetrySnapshot snapshot);
    void postTransfer(TransferProgress progress);
    void chooseLocalFile(bool saveDialog);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT settingsTitleFont_ = nullptr;
    HFONT settingsLabelFont_ = nullptr;
    HFONT settingsButtonFont_ = nullptr;
    CollapseMode collapseMode_ = CollapseMode::Expanded;
    RECT expandedRect_{};
    bool hasExpandedRect_ = false;
    bool compactPointerDown_ = false;
    bool compactDragging_ = false;
    POINT compactPressCursor_{};
    RECT compactPressRect_{};
    CollapseMode animationFromMode_ = CollapseMode::Expanded;
    CollapseMode animationTargetMode_ = CollapseMode::Expanded;
    RECT animationStartRect_{};
    RECT animationTargetRect_{};
    ULONGLONG animationStartTime_ = 0;
    float animationProgress_ = 1.0f;
    bool animating_ = false;
    bool animationClearsExpandedRect_ = false;

    ID2D1Factory* d2dFactory_ = nullptr;
    IDWriteFactory* writeFactory_ = nullptr;
    ID2D1HwndRenderTarget* renderTarget_ = nullptr;
    ID2D1SolidColorBrush* textBrush_ = nullptr;
    ID2D1SolidColorBrush* mutedBrush_ = nullptr;
    ID2D1SolidColorBrush* accentBrush_ = nullptr;
    ID2D1SolidColorBrush* surfaceBrush_ = nullptr;
    ID2D1SolidColorBrush* lineBrush_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* metricFormat_ = nullptr;
    IDWriteTextFormat* labelFormat_ = nullptr;
    IDWriteTextFormat* compactFormat_ = nullptr;

    TelemetrySnapshot snapshot_;
    TransferProgress transfer_;
    ConnectionConfig config_;
    std::string localPath_;
    std::string remotePath_;
    std::jthread monitorThread_;
    std::jthread transferThread_;
    std::atomic_bool transferCancelled_{false};
};
