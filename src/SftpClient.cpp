#include <openssl/evp.h>

#include <array>
#include <iomanip>
//#include <sstream>

#include "SftpClient.h"

#include <wx/file.h>
#include <wx/filename.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <sstream>


#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using armsync_socket_t = SOCKET;
    static constexpr armsync_socket_t kInvalidSocket = INVALID_SOCKET;
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <unistd.h>
    using armsync_socket_t = int;
    static constexpr armsync_socket_t kInvalidSocket = -1;
#endif

namespace {

bool EnsureNetworkLibraries(wxString& error)
{
#ifdef _WIN32
    static std::once_flag winsockOnce;
    static int winsockResult = WSASYSNOTREADY;
    std::call_once(winsockOnce, [] {
        WSADATA data{};
        winsockResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    if (winsockResult != 0) {
        error = wxString::Format("WSAStartup failed (%d)", winsockResult);
        return false;
    }
#endif

    static std::once_flag libssh2Once;
    static int libssh2Result = -1;
    std::call_once(libssh2Once, [] {
        libssh2Result = libssh2_init(0);
    });
    if (libssh2Result != 0) {
        error = wxString::Format("libssh2 initialization failed (%d)", libssh2Result);
        return false;
    }
    return true;
}

std::string ToUtf8(const wxString& value)
{
    const wxScopedCharBuffer buffer = value.utf8_str();
    return buffer.data() ? std::string(buffer.data(), buffer.length()) : std::string();
}

wxString ErrorText(const char* operation, int code)
{
    return wxString::Format("%s failed (code %d)", wxString::FromUTF8(operation), code);
}

bool SetSocketTimeout(armsync_socket_t socketHandle)
{
#ifdef _WIN32
    const DWORD timeoutMs = 15000;
    return setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                      reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == 0
        && setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
                      reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs)) == 0;
#else
    timeval timeout{};
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    return setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0
        && setsockopt(socketHandle, SOL_SOCKET, SO_SNDTIMEO,
                      &timeout, sizeof(timeout)) == 0;
#endif
}

void CloseSocket(armsync_socket_t socketHandle)
{
    if (socketHandle == kInvalidSocket)
        return;
#ifdef _WIN32
    closesocket(socketHandle);
#else
    close(socketHandle);
#endif
}

armsync_socket_t ConnectSocket(const std::string& host, int port, wxString& error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* addresses = nullptr;
    const std::string portText = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses);
    if (lookup != 0) {
#ifdef _WIN32
        error = wxString::Format("Could not resolve host '%s' (error %d)",
                                 wxString::FromUTF8(host.c_str()), lookup);
#else
        error = wxString::Format("Could not resolve host '%s': %s",
                                 wxString::FromUTF8(host.c_str()),
                                 wxString::FromUTF8(gai_strerror(lookup)));
#endif
        return kInvalidSocket;
    }

    armsync_socket_t connected = kInvalidSocket;
    int lastError = 0;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        armsync_socket_t candidate = socket(address->ai_family, address->ai_socktype,
                                            address->ai_protocol);
        if (candidate == kInvalidSocket)
            continue;

        if (connect(candidate, address->ai_addr,
                    static_cast<int>(address->ai_addrlen)) == 0) {
            connected = candidate;
            break;
        }
#ifdef _WIN32
        lastError = WSAGetLastError();
#else
        lastError = errno;
#endif
        CloseSocket(candidate);
    }
    freeaddrinfo(addresses);

    if (connected == kInvalidSocket) {
        error = wxString::Format("Could not connect to %s:%d (socket error %d)",
                                 wxString::FromUTF8(host.c_str()), port, lastError);
        return kInvalidSocket;
    }

    SetSocketTimeout(connected);
    return connected;
}

std::string NormalizeRemotePath(const std::string& input)
{
    std::string path = input.empty() ? "/" : input;
    std::replace(path.begin(), path.end(), '\\', '/');

    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    std::stringstream stream(path);
    std::string part;
    while (std::getline(stream, part, '/')) {
        if (part.empty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.empty())
                parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }

    std::string result = absolute ? "/" : "";
    for (const auto& item : parts) {
        if (!result.empty() && result.back() != '/')
            result += '/';
        result += item;
    }
    if (result.empty())
        result = absolute ? "/" : ".";
    return result;
}

} // namespace

SftpClient::SftpClient() = default;

SftpClient::~SftpClient()
{
    Disconnect();
}

bool SftpClient::Connect(const Settings& settings, wxString& error)
{
    Disconnect();
    if (IsCancelled()) {
        error = "Sync cancelled.";
        return false;
    }
    if (!EnsureNetworkLibraries(error))
        return false;

    const std::string host = ToUtf8(settings.host);
    const std::string username = ToUtf8(settings.username);
    if (host.empty() || username.empty() || settings.port < 1 || settings.port > 65535) {
        error = "Host, username, and a valid port are required.";
        return false;
    }

    const armsync_socket_t socketHandle = ConnectSocket(host, settings.port, error);
    if (socketHandle == kInvalidSocket)
        return false;
    socketHandle_ = static_cast<std::intptr_t>(socketHandle);
    if (IsCancelled()) {
        error = "Sync cancelled.";
        Disconnect();
        return false;
    }

    session_ = libssh2_session_init();
    if (!session_) {
        error = "Could not create an SSH session.";
        Disconnect();
        return false;
    }

    libssh2_session_set_blocking(session_, 1);
    libssh2_session_set_timeout(session_, 15000);

    const int handshake = libssh2_session_handshake(
        session_, static_cast<libssh2_socket_t>(socketHandle));
    if (handshake != 0) {
        error = ErrorText("SSH handshake", handshake);
        Disconnect();
        return false;
    }
    if (IsCancelled()) {
        error = "Sync cancelled.";
        Disconnect();
        return false;
    }

    int authResult = LIBSSH2_ERROR_AUTHENTICATION_FAILED;
    if (settings.useKeyAuth) {
        const std::string privateKey = ToUtf8(settings.privateKeyPath);
        if (privateKey.empty()) {
            error = "Private-key authentication is enabled, but no key path was supplied.";
            Disconnect();
            return false;
        }
        authResult = libssh2_userauth_publickey_fromfile(
            session_, username.c_str(), nullptr, privateKey.c_str(), nullptr);
    } else {
        const std::string password = ToUtf8(settings.password);
        authResult = libssh2_userauth_password(session_, username.c_str(), password.c_str());
    }

    if (authResult != 0) {
        error = ErrorText("SSH authentication", authResult);
        Disconnect();
        return false;
    }
    if (IsCancelled()) {
        error = "Sync cancelled.";
        Disconnect();
        return false;
    }

    sftpSession_ = libssh2_sftp_init(session_);
    if (!sftpSession_) {
        error = "Could not initialize the SFTP subsystem.";
        Disconnect();
        return false;
    }

    return true;
}

void SftpClient::Disconnect()
{
    if (sftpSession_) {
        libssh2_sftp_shutdown(sftpSession_);
        sftpSession_ = nullptr;
    }
    if (session_) {
        libssh2_session_disconnect(session_, "ArmSync disconnect");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (socketHandle_ != -1) {
        CloseSocket(static_cast<armsync_socket_t>(socketHandle_));
        socketHandle_ = -1;
    }
}

bool SftpClient::MakeDirRecursive(const std::string& remotePath, wxString& error)
{
    if (IsCancelled()) {
        error = "Sync cancelled.";
        return false;
    }
    if (!IsConnected()) {
        error = "SFTP is not connected.";
        return false;
    }

    const std::string normalized = NormalizeRemotePath(remotePath);
    const bool absolute = !normalized.empty() && normalized.front() == '/';
    std::string current = absolute ? "/" : "";
    std::stringstream stream(normalized);
    std::string part;

    while (std::getline(stream, part, '/')) {
        if (IsCancelled()) {
            error = "Sync cancelled.";
            return false;
        }
        if (part.empty())
            continue;
        if (!current.empty() && current.back() != '/')
            current += '/';
        current += part;

        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (libssh2_sftp_stat(sftpSession_, current.c_str(), &attributes) == 0) {
            if ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                && !LIBSSH2_SFTP_S_ISDIR(attributes.permissions)) {
                error = wxString::Format("Remote path exists but is not a directory: %s",
                                         wxString::FromUTF8(current.c_str()));
                return false;
            }
            continue;
        }

        if (libssh2_sftp_mkdir(sftpSession_, current.c_str(), 0755) == 0)
            continue;

        // A concurrent creator may have made the directory between stat and mkdir.
        LIBSSH2_SFTP_ATTRIBUTES verify{};
        if (libssh2_sftp_stat(sftpSession_, current.c_str(), &verify) == 0
            && (!(verify.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                || LIBSSH2_SFTP_S_ISDIR(verify.permissions))) {
            continue;
        }

        error = wxString::Format("Could not create remote directory '%s' (SFTP status %lu).",
                                 wxString::FromUTF8(current.c_str()),
                                 libssh2_sftp_last_error(sftpSession_));
        return false;
    }
    return true;
}

bool SftpClient::UploadFile(const wxString& localPath,
                            const std::string& remotePath, wxString& error)
{
    if (IsCancelled()) {
        error = "Sync cancelled.";
        return false;
    }
    if (!IsConnected()) {
        error = "SFTP is not connected.";
        return false;
    }

    wxFile localFile(localPath, wxFile::read);
    if (!localFile.IsOpened()) {
        error = wxString::Format("Could not open local file: %s", localPath);
        return false;
    }

    LIBSSH2_SFTP_HANDLE* remoteFile = libssh2_sftp_open(
        sftpSession_, remotePath.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0644);
    if (!remoteFile) {
        error = wxString::Format("Could not open remote file '%s' (SFTP status %lu).",
                                 wxString::FromUTF8(remotePath.c_str()),
                                 libssh2_sftp_last_error(sftpSession_));
        return false;
    }

    char buffer[64 * 1024];
    bool success = true;
    while (true) {
        if (IsCancelled()) {
            error = "Sync cancelled.";
            success = false;
            break;
        }

        const wxFileOffset bytesRead = localFile.Read(buffer, sizeof(buffer));
        if (bytesRead < 0) {
            error = wxString::Format("Error reading local file: %s", localPath);
            success = false;
            break;
        }
        if (bytesRead == 0)
            break;

        wxFileOffset offset = 0;
        while (offset < bytesRead) {
            if (IsCancelled()) {
                error = "Sync cancelled.";
                success = false;
                break;
            }
            const ssize_t written = libssh2_sftp_write(
                remoteFile, buffer + offset,
                static_cast<size_t>(bytesRead - offset));
            if (written <= 0) {
                error = wxString::Format("Error writing remote file '%s' (SFTP status %lu).",
                                         wxString::FromUTF8(remotePath.c_str()),
                                         libssh2_sftp_last_error(sftpSession_));
                success = false;
                break;
            }
            offset += static_cast<wxFileOffset>(written);
        }
        if (!success)
            break;
    }

    const int closeResult = libssh2_sftp_close(remoteFile);
    if (success && closeResult != 0) {
        error = ErrorText("Closing uploaded remote file", closeResult);
        success = false;
    }

    if (success) {
        wxFileName localInfo(localPath);
        const wxDateTime modified = localInfo.GetModificationTime();
        if (modified.IsValid()) {
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            attributes.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
            attributes.atime = static_cast<unsigned long>(modified.GetTicks());
            attributes.mtime = static_cast<unsigned long>(modified.GetTicks());
            LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(
                sftpSession_, remotePath.c_str(), LIBSSH2_FXF_WRITE, 0);
            if (handle) {
                libssh2_sftp_fsetstat(handle, &attributes);
                libssh2_sftp_close(handle);
            }
        }
    }
    return success;
}

bool SftpClient::GetRemoteStat(const std::string& remotePath,
                              uint64_t& size, time_t& mtime)
{
    if (!IsConnected() || IsCancelled())
        return false;

    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    if (libssh2_sftp_stat(sftpSession_, remotePath.c_str(), &attributes) != 0)
        return false;
    if ((attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
        && LIBSSH2_SFTP_S_ISDIR(attributes.permissions))
        return false;

    size = (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE)
        ? static_cast<uint64_t>(attributes.filesize) : 0;
    mtime = (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        ? static_cast<time_t>(attributes.mtime) : 0;
    return true;
}

bool SftpClient::GetRemoteFileHash(
    const std::string& remotePath,
    std::string& hash,
    wxString& error)
{
    hash.clear();
    error.clear();

    if (!IsConnected()) {
        error = "SFTP is not connected.";
        return false;
    }

    if (IsCancelled()) {
        error = "Sync cancelled.";
        return false;
    }

    // Open the remote file for reading.
    LIBSSH2_SFTP_HANDLE* remoteFile = libssh2_sftp_open(
        sftpSession_,
        remotePath.c_str(),
        LIBSSH2_FXF_READ,
        0
    );

    if (!remoteFile) {
        error = wxString::Format(
            "Could not open remote file '%s' (SFTP status %lu).",
            wxString::FromUTF8(remotePath.c_str()),
            libssh2_sftp_last_error(sftpSession_)
        );
        return false;
    }

    // Initialize SHA-256 context.
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();

    if (!ctx) {
        error = "Failed to allocate SHA-256 context.";
        libssh2_sftp_close(remoteFile);
        return false;
    }

    bool success = true;

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        error = "Failed to initialize SHA-256.";
        success = false;
    }

    // Read remote content in 64 KB chunks.
    std::array<unsigned char, 65536> buffer{};

    while (success) {
        if (IsCancelled()) {
            error = "Sync cancelled.";
            success = false;
            break;
        }

        const ssize_t bytesRead = libssh2_sftp_read(
            remoteFile,
            reinterpret_cast<char*>(buffer.data()),
            buffer.size()
        );

        if (bytesRead == 0) {
            // End of file.
            break;
        }

        if (bytesRead < 0) {
            error = wxString::Format(
                "Failed to read remote file '%s' (SFTP status %lu).",
                wxString::FromUTF8(remotePath.c_str()),
                libssh2_sftp_last_error(sftpSession_)
            );

            success = false;
            break;
        }

        if (EVP_DigestUpdate(
                ctx,
                buffer.data(),
                static_cast<size_t>(bytesRead)) != 1) {

            error = "Failed to update SHA-256.";
            success = false;
            break;
        }
    }

    // Generate the final SHA-256 digest.
    if (success) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;

        if (EVP_DigestFinal_ex(
                ctx,
                digest,
                &digestLength) != 1) {

            error = "Failed to finalize SHA-256.";
            success = false;
        } else {
            std::ostringstream result;

            result << std::hex << std::setfill('0');

            for (unsigned int i = 0; i < digestLength; ++i) {
                result << std::setw(2)
                       << static_cast<unsigned int>(digest[i]);
            }

            hash = result.str();
        }
    }

    // Release resources.
    EVP_MD_CTX_free(ctx);

    const int closeResult = libssh2_sftp_close(remoteFile);

    if (success && closeResult != 0) {
        error = "Failed to close remote file after hashing.";
        success = false;
    }

    if (!success)
        hash.clear();

    return success;
}

bool SftpClient::ListRemote(const std::string& dir,
                            std::vector<std::string>& names,
                            std::vector<bool>& isDir)
{
    names.clear();
    isDir.clear();
    if (!IsConnected() || IsCancelled())
        return false;

    LIBSSH2_SFTP_HANDLE* directory = libssh2_sftp_opendir(sftpSession_, dir.c_str());
    if (!directory)
        return false;

    bool success = true;
    char nameBuffer[4096];
    while (true) {
        if (IsCancelled()) {
            success = false;
            break;
        }

        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const int count = libssh2_sftp_readdir_ex(
            directory, nameBuffer, sizeof(nameBuffer) - 1, nullptr, 0, &attributes);
        if (count == 0)
            break;
        if (count < 0) {
            success = false;
            break;
        }

        nameBuffer[count] = '\0';
        const std::string name(nameBuffer, static_cast<size_t>(count));
        if (name == "." || name == "..")
            continue;

        const bool directoryEntry = (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
            && LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
        names.push_back(name);
        isDir.push_back(directoryEntry);
    }

    if (libssh2_sftp_closedir(directory) != 0)
        success = false;
    return success;
}

bool SftpClient::RemoveFile(const std::string& remotePath, wxString& error)
{
    if (!IsConnected() || IsCancelled()) {
        error = IsCancelled() ? "Sync cancelled." : "SFTP is not connected.";
        return false;
    }
    if (libssh2_sftp_unlink(sftpSession_, remotePath.c_str()) == 0)
        return true;

    error = wxString::Format("Could not remove remote file '%s' (SFTP status %lu).",
                             wxString::FromUTF8(remotePath.c_str()),
                             libssh2_sftp_last_error(sftpSession_));
    return false;
}

bool SftpClient::RemoveDirectory(const std::string& remotePath, wxString& error)
{
    if (!IsConnected() || IsCancelled()) {
        error = IsCancelled() ? "Sync cancelled." : "SFTP is not connected.";
        return false;
    }
    if (libssh2_sftp_rmdir(sftpSession_, remotePath.c_str()) == 0)
        return true;

    error = wxString::Format("Could not remove remote directory '%s' (SFTP status %lu).",
                             wxString::FromUTF8(remotePath.c_str()),
                             libssh2_sftp_last_error(sftpSession_));
    return false;
}

void SftpClient::SetCancellationFlag(const std::atomic_bool* cancelFlag)
{
    cancelFlag_ = cancelFlag;
}

bool SftpClient::IsCancelled() const
{
    return cancelFlag_ && cancelFlag_->load(std::memory_order_relaxed);
}

bool SftpClient::IsConnected() const
{
    return session_ != nullptr && sftpSession_ != nullptr && socketHandle_ != -1;
}
