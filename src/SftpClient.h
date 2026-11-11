#pragma once

#include "Settings.h"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

class SftpClient {
public:
    SftpClient();
    ~SftpClient();

    SftpClient(const SftpClient&) = delete;
    SftpClient& operator=(const SftpClient&) = delete;

    bool Connect(const Settings& settings, wxString& error);
    void Disconnect();

    bool MakeDirRecursive(const std::string& remotePath, wxString& error);
    bool UploadFile(const wxString& localPath, const std::string& remotePath,
                    wxString& error);
    bool GetRemoteStat(const std::string& remotePath, uint64_t& size,
                       time_t& mtime);
    bool ListRemote(const std::string& dir, std::vector<std::string>& names,
                    std::vector<bool>& isDir);
    bool RemoveFile(const std::string& remotePath, wxString& error);
    bool RemoveDirectory(const std::string& remotePath, wxString& error);

    void SetCancellationFlag(const std::atomic_bool* cancelFlag);
    bool GetRemoteFileHash(
        const std::string& remotePath,
        std::string& hash,
        wxString& error
    );

private:
    bool IsCancelled() const;
    bool IsConnected() const;

    LIBSSH2_SESSION* session_ = nullptr;
    LIBSSH2_SFTP* sftpSession_ = nullptr;
    std::intptr_t socketHandle_ = -1;
    const std::atomic_bool* cancelFlag_ = nullptr;
};
