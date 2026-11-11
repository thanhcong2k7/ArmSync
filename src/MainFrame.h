#pragma once

#include "Settings.h"

#include <wx/frame.h>

#include <atomic>
#include <memory>

class wxButton;
class wxGauge;
class wxStaticText;
class wxTextCtrl;
class wxCommandEvent;
class wxCloseEvent;
class wxThreadEvent;

class MainFrame final : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

private:
    enum : int {
        ID_SYNC_NOW = wxID_HIGHEST + 101,
        ID_CANCEL_SYNC,
        ID_OPEN_SHELL,
        ID_SETTINGS
    };

    void CreateMenuBar();
    void CreateMainPanel();
    void UpdateFolderLabels();
    bool EditSettings();
    void StartSync();

    void OnSyncNow(wxCommandEvent& event);
    void OnCancel(wxCommandEvent& event);
    void OnOpenShell(wxCommandEvent& event);
    void OnSettings(wxCommandEvent& event);
    void OnSyncLog(wxThreadEvent& event);
    void OnSyncProgress(wxThreadEvent& event);
    void OnSyncDone(wxThreadEvent& event);
    void OnClose(wxCloseEvent& event);

    Settings settings_;
    bool syncRunning_ = false;
    std::shared_ptr<std::atomic_bool> cancelFlag_;
    bool closeWhenDone_ = false;

    wxStaticText* localFolderLabel_ = nullptr;
    wxStaticText* remoteFolderLabel_ = nullptr;
    wxButton* syncNowButton_ = nullptr;
    wxButton* cancelButton_ = nullptr;
    wxGauge* progressGauge_ = nullptr;
    wxTextCtrl* logCtrl_ = nullptr;
};
