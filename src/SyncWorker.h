#pragma once

#include "Settings.h"
#include "SftpClient.h"

#include <wx/thread.h>

#include <atomic>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class wxEvtHandler;

class SyncWorker final : public wxThread {
public:
    SyncWorker(wxEvtHandler* eventTarget, Settings settings,
               std::shared_ptr<std::atomic_bool> cancelFlag);
    ~SyncWorker() override;

    SyncWorker(const SyncWorker&) = delete;
    SyncWorker& operator=(const SyncWorker&) = delete;

    // Detached threads delete themselves when Entry() returns.
    bool Start();
    void Stop();

protected:
    ExitCode Entry() override;

private:
    struct LocalFile {
        wxString fullPath;
        std::string relativePath;
        uint64_t size = 0;
        time_t mtime = 0;
    };

    bool GatherLocalTree(const wxString& root, const wxString& current,
                         std::vector<LocalFile>& files,
                         std::vector<std::string>& directories);
    bool CreateLocalDirectories(const std::string& remoteRoot,
                                const std::vector<std::string>& directories);
    bool UploadLocalFiles(const std::vector<LocalFile>& files,
                          const std::string& remoteRoot);
    bool PruneRemoteDirectory(const std::string& remoteDirectory,
                              const std::string& relativeDirectory,
                              const std::unordered_set<std::string>& localFiles,
                              const std::unordered_set<std::string>& localDirectories);
    bool RemoveRemoteTree(const std::string& remotePath);

    void PostLog(const wxString& message);
    void PostProgress(int current, int total);
    void PostDone(bool success, bool cancelled, const wxString& message);
    bool IsCancelled() const;

    wxEvtHandler* eventTarget_ = nullptr; // MainFrame stays alive until EVT_SYNC_DONE.
    const Settings settings_;
    std::shared_ptr<std::atomic_bool> cancelFlag_;
    SftpClient sftp_;
};
