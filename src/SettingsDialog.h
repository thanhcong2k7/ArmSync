#pragma once

#include "Settings.h"

#include <wx/wx.h>

class SettingsDialog final : public wxDialog {
public:
    SettingsDialog(wxWindow* parent, const Settings& settings);

    const Settings& GetSettings() const { return settings_; }

    bool TransferDataToWindow() override;
    bool TransferDataFromWindow() override;

private:
    void OnBrowseLocalDir(wxCommandEvent& event);
    void OnBrowseKeyFile(wxCommandEvent& event);
    void OnUseKeyToggle(wxCommandEvent& event);
    void UpdateKeyControls();

    Settings settings_;

    wxTextCtrl* hostCtrl_ = nullptr;
    wxTextCtrl* portCtrl_ = nullptr;
    wxTextCtrl* usernameCtrl_ = nullptr;
    wxTextCtrl* passwordCtrl_ = nullptr;
    wxCheckBox* useKeyCheck_ = nullptr;
    wxTextCtrl* keyPathCtrl_ = nullptr;
    wxButton* browseKeyButton_ = nullptr;
    wxTextCtrl* localFolderCtrl_ = nullptr;
    wxTextCtrl* remoteFolderCtrl_ = nullptr;
    wxCheckBox* deleteExtraneousCheck_ = nullptr;
};
