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
#include <vector>
#include <shellapi.h>

class App {
public:
    explicit App(HINSTANCE instance);
    ~App();
    int run(int showCommand);

private:
    friend struct AppWindowTest;
    enum ControlId {
        HostEdit = 2001, PortEdit, UserEdit, PasswordEdit, KeyEdit,
        LocalEdit, RemoteEdit, BrowseLocal, SaveConnect, UploadButton, DownloadButton,
        EntireFolder, SelectedFiles, ServerName, NewServer, ReopenServers, DeleteServer,
        BackgroundColor, ForegroundColor, NextHotkey, PreviousHotkey, OpenLocalFolder, CancelTransfer,
        ConnectionTab, TransferTab, AppearanceTab, RefreshIntervalEdit
    };
    enum class CollapseMode { Expanded, Horizontal, Side };
    static LRESULT CALLBACK mainWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK settingsWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMainMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleSettingsMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    static LRESULT CALLBACK trayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    bool createTray();
    bool addTrayIcon();
    void removeTray();
    void showTrayMenu();
    void showConnectedServers();
    void showAtBottomRight();
    void hideToTray();
    void quitApplication();
    void restartApplication();
    HWND trayWindow_ = nullptr;
    bool trayAdded_ = false;
    bool exiting_ = false;
    bool showWhenConnected_ = false;

    bool registerClasses();
    bool createMainWindow(int showCommand);
    void createSettingsWindow();
    void showSettings();
    void hideSettings(bool animate = true);
    void toggleSettings();
    void startSettingsAnimation(bool opening);
    bool isSettingsRelatedWindow(HWND target) const;
    void selectSettingsTab(int tab);
    void positionSettings();
    void tickSettingsAnimation();
    void finishSettingsAnimation();
    int settingsTab_ = 0;
    UINT settingsLayoutDpi_ = 96;
    bool partialTransfer_ = false;
    bool settingsLoaded_ = false;
    bool settingsAnimating_ = false;
    bool settingsClosing_ = false;
    float settingsReveal_ = 0.0f;
    float settingsAnimationFrom_ = 0.0f;
    ULONGLONG settingsAnimationStarted_ = 0;
    POINT settingsAnimationOrigin_{};
    void paintSettings(HDC destination = nullptr);
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
    void collapseNearestSide();
    void snapCompactAfterDrag();
    void updateRestoreAnchor(HMONITOR monitor, const RECT& compactRect, CollapseMode mode);
    void startWindowAnimation(const RECT& target, CollapseMode targetMode, bool clearExpandedRect);
    void tickWindowAnimation();
    void finishWindowAnimation();
    void restoreExpanded();
    void handleEdgeDock();
    bool isCollapsed() const { return collapseMode_ != CollapseMode::Expanded; }

    bool readSettings(ConnectionConfig& config, std::string& localPath, std::string& remotePath);
    bool rememberConnection(const ConnectionConfig& config, const std::string& local, const std::string& remote);
    std::vector<App*> servers();
    void applyProfile(const ServerProfile& profile);
    ServerProfile profile() const;
    std::string windowTitle() const;
    bool saveServers();
    void addServer();
    void showLocalMonitor();
    void reopenServers();
    void switchServer(bool previous);
    bool setHotkeys(unsigned next, unsigned previous, bool reportError);
    void chooseTheme(bool background);
    void dropFiles(HDROP drop);
    void uploadDropped(std::vector<std::string> paths);
    void openLocalFolder();
    bool confirmTransfer(std::size_t count, std::uint64_t bytes, TransferDirection direction,
        const std::string& source, const std::string& destination, std::uintptr_t generation, std::stop_token token);
    struct TransferDecision {
        std::atomic<int> result{-1};
        std::size_t count;
        std::uint64_t bytes;
        TransferDirection direction;
        std::string source, destination;
    };
    std::string transferLocalFolder_;
    App* root_ = this;
    std::vector<std::unique_ptr<App>> children_;
    bool deleted_ = false;
    bool local_ = false;
    std::string serverName_;
    std::string detectedHostName_;
    std::uint32_t background_ = 0xFFFFFF, foreground_ = 0x1D1D1F;
    std::atomic<std::uint32_t> refreshIntervalSeconds_{2};
    unsigned nextHotkey_ = 0x064E, previousHotkey_ = 0x0650;
    bool hotkeysRegistered_ = false;
    std::size_t windowSlot_ = 0;
    bool readRefreshInterval(std::uint32_t& seconds) const;
    void waitRefreshInterval(std::stop_token token) const;
    void startMonitor(const ConnectionConfig& config);
    void stopMonitor();
    void startTransfer(TransferDirection direction);
    void stopTransfer();
    void postSnapshot(TelemetrySnapshot snapshot, std::uintptr_t generation);
    void postTransfer(TransferProgress progress, std::uintptr_t generation);
    void chooseLocalFolder();

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::mutex messageMutex_;
    HWND messageWindow_ = nullptr;
    std::uintptr_t monitorGeneration_ = 0;
    std::uintptr_t transferGeneration_ = 0;
    HWND settingsWindow_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT settingsTitleFont_ = nullptr;
    HFONT settingsLabelFont_ = nullptr;
    HFONT settingsButtonFont_ = nullptr;
    CollapseMode collapseMode_ = CollapseMode::Expanded;
    RECT expandedRect_{};
    bool hasExpandedRect_ = false;
    bool compactPointerDown_ = false;
    bool titlePointerDown_ = false;
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
    ID2D1DCRenderTarget* settingsRenderTarget_ = nullptr;
    ID2D1SolidColorBrush* textBrush_ = nullptr;
    ID2D1SolidColorBrush* mutedBrush_ = nullptr;
    ID2D1SolidColorBrush* accentBrush_ = nullptr;
    ID2D1SolidColorBrush* surfaceBrush_ = nullptr;
    ID2D1SolidColorBrush* lineBrush_ = nullptr;
    ID2D1SolidColorBrush* rankRedBrush_ = nullptr;
    ID2D1SolidColorBrush* rankYellowBrush_ = nullptr;
    ID2D1SolidColorBrush* rankGreenBrush_ = nullptr;
    IDWriteTextFormat* titleFormat_ = nullptr;
    IDWriteTextFormat* metricFormat_ = nullptr;
    IDWriteTextFormat* labelFormat_ = nullptr;
    IDWriteTextFormat* compactFormat_ = nullptr;
    IDWriteTextFormat* capacityFormat_ = nullptr;
    IDWriteTextFormat* settingsTitleFormat_ = nullptr;
    IDWriteTextFormat* settingsLabelFormat_ = nullptr;
    IDWriteTextFormat* settingsButtonFormat_ = nullptr;

    TelemetrySnapshot snapshot_;
    TransferProgress transfer_;
    ConnectionConfig config_;
    std::string localPath_;
    std::string remotePath_;
    std::jthread monitorThread_;
    std::jthread transferThread_;
};
