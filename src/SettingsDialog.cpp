#include "SettingsDialog.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/sizer.h>

SettingsDialog::SettingsDialog(wxWindow* parent, const Settings& settings)
    : wxDialog(parent, wxID_ANY, "ArmSync Settings", wxDefaultPosition,
               wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      settings_(settings)
{
    auto* grid = new wxFlexGridSizer(3, 8, 8);
    grid->AddGrowableCol(1, 1);

    const auto addLabel = [this, grid](const wxString& label) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0,
                  wxALIGN_CENTER_VERTICAL);
    };

    addLabel("Host / IP:");
    hostCtrl_ = new wxTextCtrl(this, wxID_ANY);
    grid->Add(hostCtrl_, 1, wxEXPAND);
    grid->AddSpacer(1);

    addLabel("Port:");
    portCtrl_ = new wxTextCtrl(this, wxID_ANY);
    grid->Add(portCtrl_, 1, wxEXPAND);
    grid->AddSpacer(1);

    addLabel("Username:");
    usernameCtrl_ = new wxTextCtrl(this, wxID_ANY);
    grid->Add(usernameCtrl_, 1, wxEXPAND);
    grid->AddSpacer(1);

    addLabel("Password:");
    passwordCtrl_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                   wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    grid->Add(passwordCtrl_, 1, wxEXPAND);
    grid->AddSpacer(1);

    grid->AddSpacer(1);
    useKeyCheck_ = new wxCheckBox(this, wxID_ANY,
                                  "Use private-key authentication");
    grid->Add(useKeyCheck_, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(1);

    addLabel("Private key:");
    keyPathCtrl_ = new wxTextCtrl(this, wxID_ANY);
    grid->Add(keyPathCtrl_, 1, wxEXPAND);
    browseKeyButton_ = new wxButton(this, wxID_ANY, "Browse...");
    grid->Add(browseKeyButton_, 0);

    addLabel("Local folder:");
    localFolderCtrl_ = new wxTextCtrl(this, wxID_ANY);
    grid->Add(localFolderCtrl_, 1, wxEXPAND);
    auto* browseLocalButton = new wxButton(this, wxID_ANY, "Browse...");
    grid->Add(browseLocalButton, 0);

    addLabel("Remote folder:");
    remoteFolderCtrl_ = new wxTextCtrl(this, wxID_ANY);
    grid->Add(remoteFolderCtrl_, 1, wxEXPAND);
    grid->AddSpacer(1);

    grid->AddSpacer(1);
    deleteExtraneousCheck_ = new wxCheckBox(
        this, wxID_ANY, "Delete remote files and directories not present locally");
    grid->Add(deleteExtraneousCheck_, 0, wxALIGN_CENTER_VERTICAL);
    grid->AddSpacer(1);

    auto* mainSizer = new wxBoxSizer(wxVERTICAL);
    mainSizer->Add(grid, 1, wxEXPAND | wxALL, 12);
    mainSizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0,
                   wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
    SetSizerAndFit(mainSizer);
    SetMinSize(wxSize(590, GetSize().GetHeight()));

    browseLocalButton->Bind(wxEVT_BUTTON, &SettingsDialog::OnBrowseLocalDir, this);
    browseKeyButton_->Bind(wxEVT_BUTTON, &SettingsDialog::OnBrowseKeyFile, this);
    useKeyCheck_->Bind(wxEVT_CHECKBOX, &SettingsDialog::OnUseKeyToggle, this);

    TransferDataToWindow();
}

bool SettingsDialog::TransferDataToWindow()
{
    hostCtrl_->SetValue(settings_.host);
    portCtrl_->SetValue(wxString::Format("%d", settings_.port));
    usernameCtrl_->SetValue(settings_.username);
    passwordCtrl_->SetValue(settings_.password);
    useKeyCheck_->SetValue(settings_.useKeyAuth);
    keyPathCtrl_->SetValue(settings_.privateKeyPath);
    localFolderCtrl_->SetValue(settings_.localFolder);
    remoteFolderCtrl_->SetValue(settings_.remoteFolder);
    deleteExtraneousCheck_->SetValue(settings_.deleteExtraneous);
    UpdateKeyControls();
    return true;
}

bool SettingsDialog::TransferDataFromWindow()
{
    const wxString host = hostCtrl_->GetValue().Trim().Trim(false);
    const wxString username = usernameCtrl_->GetValue().Trim().Trim(false);

    if (host.empty()) {
        wxMessageBox("Host must not be empty.", "ArmSync Settings",
                     wxOK | wxICON_WARNING, this);
        hostCtrl_->SetFocus();
        return false;
    }

    long port = 0;
    if (!portCtrl_->GetValue().Trim().ToLong(&port) || port < 1 || port > 65535) {
        wxMessageBox("Port must be a number between 1 and 65535.",
                     "ArmSync Settings", wxOK | wxICON_WARNING, this);
        portCtrl_->SetFocus();
        return false;
    }

    if (username.empty()) {
        wxMessageBox("Username must not be empty.", "ArmSync Settings",
                     wxOK | wxICON_WARNING, this);
        usernameCtrl_->SetFocus();
        return false;
    }

    const bool useKey = useKeyCheck_->GetValue();
    if (!useKey && passwordCtrl_->GetValue().empty()) {
        wxMessageBox("Enter a password or enable private-key authentication.",
                     "ArmSync Settings", wxOK | wxICON_WARNING, this);
        passwordCtrl_->SetFocus();
        return false;
    }
    const wxString keyPath = keyPathCtrl_->GetValue().Trim().Trim(false);
    if (useKey && (keyPath.empty() || !wxFileExists(keyPath))) {
        wxMessageBox("Choose an existing private-key file.", "ArmSync Settings",
                     wxOK | wxICON_WARNING, this);
        keyPathCtrl_->SetFocus();
        return false;
    }

    const wxString localFolder = localFolderCtrl_->GetValue().Trim().Trim(false);
    if (localFolder.empty() || !wxDirExists(localFolder)) {
        wxMessageBox("Choose an existing local folder.", "ArmSync Settings",
                     wxOK | wxICON_WARNING, this);
        localFolderCtrl_->SetFocus();
        return false;
    }

    wxString remoteFolder = remoteFolderCtrl_->GetValue().Trim().Trim(false);
    if (remoteFolder.empty())
        remoteFolder = "/";

    settings_.host = host;
    settings_.port = static_cast<int>(port);
    settings_.username = username;
    settings_.password = passwordCtrl_->GetValue();
    settings_.useKeyAuth = useKey;
    settings_.privateKeyPath = keyPath;
    settings_.localFolder = localFolder;
    settings_.remoteFolder = remoteFolder;
    settings_.deleteExtraneous = deleteExtraneousCheck_->GetValue();
    return true;
}

void SettingsDialog::UpdateKeyControls()
{
    const bool enabled = useKeyCheck_->GetValue();
    keyPathCtrl_->Enable(enabled);
    browseKeyButton_->Enable(enabled);
    passwordCtrl_->Enable(!enabled);
}

void SettingsDialog::OnUseKeyToggle(wxCommandEvent&)
{
    UpdateKeyControls();
}

void SettingsDialog::OnBrowseLocalDir(wxCommandEvent&)
{
    wxDirDialog dialog(this, "Select local folder", localFolderCtrl_->GetValue(),
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK)
        localFolderCtrl_->SetValue(dialog.GetPath());
}

void SettingsDialog::OnBrowseKeyFile(wxCommandEvent&)
{
    wxFileDialog dialog(this, "Select private key", wxEmptyString,
                        keyPathCtrl_->GetValue(), "All files (*)|*",
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() == wxID_OK)
        keyPathCtrl_->SetValue(dialog.GetPath());
}
