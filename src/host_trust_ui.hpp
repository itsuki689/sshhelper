#pragma once
#include "host_trust.hpp"
#include <windows.h>
#include <map>

// All window methods run on the UI thread; confirmation waits run on workers.
// A message-only window also receives timers inside native modal picker loops.
class HostTrustUi {
public:
    explicit HostTrustUi(HINSTANCE instance);
    ~HostTrustUi();
    HostTrustConfirmation confirmation(const void* owner);
    void cancelOwner(const void* owner);
    void shutdown();
    bool translate(MSG& message);
private:
    friend struct HostTrustUiTest;
    struct Dialog {
        HostTrustUi* ui;
        HostTrustCoordinator::RequestPtr request;
        HWND window = nullptr;
    };
    static LRESULT CALLBACK dispatchProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK dialogProc(HWND, UINT, WPARAM, LPARAM);
    void refresh();
    HINSTANCE instance_;
    HWND dispatcher_ = nullptr;
    HostTrustCoordinator coordinator_;
    std::map<std::uint64_t, std::unique_ptr<Dialog>> dialogs_;
};
