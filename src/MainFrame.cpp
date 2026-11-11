#include "MainFrame.h"

#include "Events.h"
#include "SettingsDialog.h"
#include "SyncWorker.h"

#include <wx/button.h>
#include <wx/gauge.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

wxString ShellQuotePosix(const wxString& value)
{
    wxString quoted = "'";
    for (wxChar character : value) {
        if (character == '\'')
            quoted += "'\\''";
        else
            quoted += character;
    }
    quoted += "'";
    return quoted;
}

wxString EscapeAppleScriptString(const wxString& value)
{
    wxString escaped;
    for (wxChar character : value) {
        if (character == '\\' || character == '"')
            escaped += '\\';
        escaped += character;
    }
    return escaped;
}

wxArrayString BuildSshArguments(const Settings& settings)
{
    wxArrayString arguments;
    arguments.Add("ssh");
    arguments.Add("-p");
    arguments.Add(wxString::Format("%d", settings.port));
    if (settings.useKeyAuth) {
        arguments.Add("-i");
        arguments.Add(settings.privateKeyPath);
    }
    arguments.Add(settings.username + "@" + settings.host);
    return arguments;
}

// wxWidgets 3.2 does not provide wxExecute(wxArrayString, flags). Convert the
// argument list to the native argv form instead, preserving each argument as
// a separate value (including paths containing spaces).
long ExecuteArgv(const wxArrayString& arguments)
{
    if (arguments.empty())
        return 0;

#ifdef __WXMSW__
    std::vector<std::wstring> storage;
    storage.reserve(arguments.size());
    for (const wxString& argument : arguments)
        storage.push_back(argument.ToStdWstring());

    std::vector<const wchar_t*> argv;
    argv.reserve(storage.size() + 1);
    for (const std::wstring& argument : storage)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    return wxExecute(argv.data(), wxEXEC_ASYNC);
#else
    std::vector<std::string> storage;
    storage.reserve(arguments.size());
    for (const wxString& argument : arguments)
        storage.push_back(argument.ToStdString(wxConvUTF8));

    std::vector<const char*> argv;
    argv.reserve(storage.size() + 1);
    for (const std::string& argument : storage)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    return wxExecute(argv.data(), wxEXEC_ASYNC);
#endif
}

wxString BuildPosixSshCommand(const Settings& settings)
{
    wxString command = "ssh -p " + wxString::Format("%d", settings.port);
    if (settings.useKeyAuth)
        command += " -i " + ShellQuotePosix(settings.privateKeyPath);
    command += " " + ShellQuotePosix(settings.username + "@" + settings.host);
    return command;
}

bool LaunchSshTerminal(const Settings& settings)
{
#ifdef __WXMAC__
    const wxString command = BuildPosixSshCommand(settings);
    const wxString script = "tell application \"Terminal\" to do script \""
        + EscapeAppleScriptString(command) + "\"\n tell application \"Terminal\" to activate";
    wxArrayString arguments;
    arguments.Add("osascript");
    arguments.Add("-e");
    arguments.Add(script);
    return ExecuteArgv(arguments) != 0;
#elif defined(__WXGTK__)
    const wxArrayString ssh = BuildSshArguments(settings);
    const wxString terminals[] = {
        "x-terminal-emulator", "gnome-terminal", "konsole",
        "xfce4-terminal", "xterm"
    };
    for (const wxString& terminal : terminals) {
        wxArrayString arguments;
        arguments.Add(terminal);
        if (terminal == "gnome-terminal")
            arguments.Add("--");
        else if (terminal == "xfce4-terminal")
            arguments.Add("--execute");
        else
            arguments.Add("-e");
        for (size_t index = 0; index < ssh.size(); ++index)
            arguments.Add(ssh[index]);
        if (ExecuteArgv(arguments) != 0)
            return true;
    }
    return false;
#elif defined(__WXMSW__)
    wxArrayString ssh = BuildSshArguments(settings);
    // Windows Terminal keeps the argument vector intact and is preferred.
    wxArrayString terminalArgs;
    terminalArgs.Add("wt.exe");
    terminalArgs.Add("-w");
    terminalArgs.Add("new");
    terminalArgs.Add("new-tab");
    for (size_t index = 0; index < ssh.size(); ++index)
        terminalArgs.Add(ssh[index]);
    if (ExecuteArgv(terminalArgs) != 0)
        return true;

    // Fallback required on systems without Windows Terminal.
    wxString windowsCommand = "start \"\" cmd.exe /k ssh -p "
        + wxString::Format("%d", settings.port);
    if (settings.useKeyAuth)
        windowsCommand += " -i \"" + settings.privateKeyPath + "\"";
    windowsCommand += " \"" + settings.username + "@" + settings.host + "\"";
    wxArrayString cmdArgs;
    cmdArgs.Add("cmd.exe");
    cmdArgs.Add("/c");
    cmdArgs.Add(windowsCommand);
    return ExecuteArgv(cmdArgs) != 0;
#else
    (void)settings;
    return false;
#endif
}

} // namespace

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "ArmSync", wxDefaultPosition, wxSize(820, 580),
              wxDEFAULT_FRAME_STYLE)
{
    settings_ = Settings::Load();
    CreateMenuBar();
    CreateMainPanel();
    CreateStatusBar(1);
    SetStatusText("Ready");
    UpdateFolderLabels();

    Bind(EVT_SYNC_LOG, &MainFrame::OnSyncLog, this);
    Bind(EVT_SYNC_PROGRESS, &MainFrame::OnSyncProgress, this);
    Bind(EVT_SYNC_DONE, &MainFrame::OnSyncDone, this);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::OnClose, this);

    SetMinSize(wxSize(640, 420));
    Centre();
}

MainFrame::~MainFrame()
{
    // OnClose vetoes destruction while the detached worker is active.
    // This is a last-resort cancellation request for nonstandard shutdown paths.
    if (cancelFlag_)
        cancelFlag_->store(true, std::memory_order_relaxed);
}

void MainFrame::CreateMenuBar()
{
    auto* menuBar = new wxMenuBar();
    auto* fileMenu = new wxMenu();
    fileMenu->Append(ID_SETTINGS, "Settings...");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT, "Exit");

    auto* toolsMenu = new wxMenu();
    toolsMenu->Append(ID_SYNC_NOW, "Sync Now\tCtrl+R");
    toolsMenu->Append(ID_OPEN_SHELL, "Open SSH Shell");

    menuBar->Append(fileMenu, "File");
    menuBar->Append(toolsMenu, "Tools");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnSettings, this, ID_SETTINGS);
    Bind(wxEVT_MENU, &MainFrame::OnSyncNow, this, ID_SYNC_NOW);
    Bind(wxEVT_MENU, &MainFrame::OnOpenShell, this, ID_OPEN_SHELL);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(true); }, wxID_EXIT);
}

void MainFrame::CreateMainPanel()
{
    auto* panel = new wxPanel(this);
    auto* rootSizer = new wxBoxSizer(wxVERTICAL);

    auto* folderSizer = new wxFlexGridSizer(2, 8, 8);
    folderSizer->AddGrowableCol(1, 1);
    folderSizer->Add(new wxStaticText(panel, wxID_ANY, "Local folder:"), 0,
                     wxALIGN_CENTER_VERTICAL);
    localFolderLabel_ = new wxStaticText(panel, wxID_ANY, "Not configured");
    localFolderLabel_->SetMinSize(wxSize(450, -1));
    folderSizer->Add(localFolderLabel_, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    folderSizer->Add(new wxStaticText(panel, wxID_ANY, "Remote folder:"), 0,
                     wxALIGN_CENTER_VERTICAL);
    remoteFolderLabel_ = new wxStaticText(panel, wxID_ANY, "/home");
    folderSizer->Add(remoteFolderLabel_, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    rootSizer->Add(folderSizer, 0, wxEXPAND | wxALL, 12);

    auto* buttonSizer = new wxBoxSizer(wxHORIZONTAL);
    syncNowButton_ = new wxButton(panel, ID_SYNC_NOW, "Sync Now");
    cancelButton_ = new wxButton(panel, ID_CANCEL_SYNC, "Cancel");
    auto* openShellButton = new wxButton(panel, ID_OPEN_SHELL, "Open SSH Shell");
    auto* settingsButton = new wxButton(panel, ID_SETTINGS, "Settings");
    cancelButton_->Enable(false);
    buttonSizer->Add(syncNowButton_, 0, wxRIGHT, 8);
    buttonSizer->Add(cancelButton_, 0, wxRIGHT, 8);
    buttonSizer->Add(openShellButton, 0, wxRIGHT, 8);
    buttonSizer->Add(settingsButton, 0);
    rootSizer->Add(buttonSizer, 0, wxLEFT | wxRIGHT | wxBOTTOM, 12);

    progressGauge_ = new wxGauge(panel, wxID_ANY, 1, wxDefaultPosition,
                                 wxDefaultSize, wxGA_HORIZONTAL);
    progressGauge_->SetValue(0);
    rootSizer->Add(progressGauge_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    rootSizer->Add(new wxStaticText(panel, wxID_ANY, "Synchronization log:"),
                   0, wxLEFT | wxRIGHT | wxBOTTOM, 12);
    logCtrl_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString,
                              wxDefaultPosition, wxDefaultSize,
                              wxTE_MULTILINE | wxTE_READONLY | wxTE_RICH2);
    rootSizer->Add(logCtrl_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    panel->SetSizer(rootSizer);
    auto* frameSizer = new wxBoxSizer(wxVERTICAL);
    frameSizer->Add(panel, 1, wxEXPAND);
    SetSizer(frameSizer);

    syncNowButton_->Bind(wxEVT_BUTTON, &MainFrame::OnSyncNow, this);
    cancelButton_->Bind(wxEVT_BUTTON, &MainFrame::OnCancel, this);
    openShellButton->Bind(wxEVT_BUTTON, &MainFrame::OnOpenShell, this);
    settingsButton->Bind(wxEVT_BUTTON, &MainFrame::OnSettings, this);
}

void MainFrame::UpdateFolderLabels()
{
    localFolderLabel_->SetLabel(settings_.localFolder.empty()
        ? "Not configured" : settings_.localFolder);
    remoteFolderLabel_->SetLabel(settings_.remoteFolder.empty()
        ? "/" : settings_.remoteFolder);
    Layout();
}

bool MainFrame::EditSettings()
{
    SettingsDialog dialog(this, settings_);
    if (dialog.ShowModal() != wxID_OK || !dialog.TransferDataFromWindow())
        return false;

    settings_ = dialog.GetSettings();
    settings_.Save();
    UpdateFolderLabels();
    SetStatusText("Settings saved");
    return true;
}

void MainFrame::StartSync()
{
    if (syncRunning_) {
        SetStatusText("A synchronization is already running");
        return;
    }

    if (settings_.host.empty() || settings_.username.empty()
        || settings_.localFolder.empty() || settings_.remoteFolder.empty()
        || (settings_.useKeyAuth && settings_.privateKeyPath.empty())
        || (!settings_.useKeyAuth && settings_.password.empty())) {
        if (!EditSettings())
            return;
    }

    if (!wxDirExists(settings_.localFolder)) {
        wxMessageBox("The configured local folder does not exist. Update Settings first.",
                     "ArmSync", wxOK | wxICON_WARNING, this);
        return;
    }

    logCtrl_->Clear();
    progressGauge_->SetRange(1);
    progressGauge_->SetValue(0);
    syncNowButton_->Enable(false);
    GetMenuBar()->Enable(ID_SYNC_NOW, false);
    cancelButton_->Enable(true);
    SetStatusText("Connecting...");

    cancelFlag_ = std::make_shared<std::atomic_bool>(false);
    syncRunning_ = true;
    auto* worker = new SyncWorker(this, settings_, cancelFlag_);
    if (!worker->Start()) {
        // A detached wxThread that fails to start has not taken ownership of itself.
        delete worker;
        syncRunning_ = false;
        cancelFlag_.reset();
        syncNowButton_->Enable(true);
        GetMenuBar()->Enable(ID_SYNC_NOW, true);
        cancelButton_->Enable(false);
        SetStatusText("Could not start synchronization thread");
        wxMessageBox("Could not start the synchronization thread.", "ArmSync",
                     wxOK | wxICON_ERROR, this);
    }
}

void MainFrame::OnSyncNow(wxCommandEvent&)
{
    StartSync();
}

void MainFrame::OnCancel(wxCommandEvent&)
{
    if (syncRunning_ && cancelFlag_) {
        cancelFlag_->store(true, std::memory_order_relaxed);
        cancelButton_->Enable(false);
        SetStatusText("Cancellation requested...");
    }
}

void MainFrame::OnOpenShell(wxCommandEvent&)
{
    if (settings_.host.empty() || settings_.username.empty()) {
        if (!EditSettings())
            return;
    }
    if (settings_.host.empty() || settings_.username.empty())
        return;

    if (!LaunchSshTerminal(settings_)) {
        wxMessageBox("Could not launch a terminal with the system SSH client. "
                     "Check that OpenSSH and a supported terminal are installed.",
                     "Open SSH Shell", wxOK | wxICON_ERROR, this);
        return;
    }
    SetStatusText("SSH terminal launched");
}

void MainFrame::OnSettings(wxCommandEvent&)
{
    EditSettings();
}

void MainFrame::OnSyncLog(wxThreadEvent& event)
{
    if (logCtrl_) {
        logCtrl_->AppendText(event.GetString());
        logCtrl_->AppendText("\n");
    }
}

void MainFrame::OnSyncProgress(wxThreadEvent& event)
{
    const SyncProgressPayload payload = event.GetPayload<SyncProgressPayload>();
    const int range = std::max(1, payload.total);
    progressGauge_->SetRange(range);
    progressGauge_->SetValue(std::clamp(payload.current, 0, range));
    SetStatusText(wxString::Format("Synchronizing: %d / %d", payload.current,
                                   payload.total));
}

void MainFrame::OnSyncDone(wxThreadEvent& event)
{
    const SyncDonePayload payload = event.GetPayload<SyncDonePayload>();
    syncRunning_ = false; // The detached thread has queued its final event.
    cancelFlag_.reset();
    syncNowButton_->Enable(true);
    GetMenuBar()->Enable(ID_SYNC_NOW, true);
    cancelButton_->Enable(false);
    SetStatusText(payload.message);
    if (logCtrl_) {
        logCtrl_->AppendText(payload.message);
        logCtrl_->AppendText("\n");
    }

    if (closeWhenDone_) {
        CallAfter([this] { Close(true); });
    }
}

void MainFrame::OnClose(wxCloseEvent& event)
{
    if (syncRunning_) {
        closeWhenDone_ = true;
        if (cancelFlag_)
            cancelFlag_->store(true, std::memory_order_relaxed);
        cancelButton_->Enable(false);
        SetStatusText("Stopping synchronization before exit...");
        event.Veto();
        return;
    }
    event.Skip();
}
