#include "SyncWorker.h"

#include "Events.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/utils.h>

#include <algorithm>
#include <exception>
#include <sstream>
#include <utility>
#include <unordered_set>
#include <wx/file.h>
#include <memory>

#include <openssl/evp.h>

#include <array>
#include <iomanip>
//#include <sstream>

namespace {
static bool ComputeLocalSha256(
    const wxString& filePath,
    std::string& hash,
    wxString& error)
{
    wxFile file;

    if (!file.Open(filePath, wxFile::read)) {
        error = "Cannot open local file";
        return false;
    }

    using ContextPtr =
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

    ContextPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);

    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
        error = "Failed to initialize SHA-256";
        return false;
    }

    std::array<unsigned char, 65536> buffer{};

    while (true) {
        const auto bytesRead = file.Read(buffer.data(), buffer.size());

        if (bytesRead < 0) {
            error = "Failed to read local file";
            return false;
        }

        if (bytesRead == 0)
            break;

        if (EVP_DigestUpdate(
                ctx.get(), buffer.data(),
                static_cast<size_t>(bytesRead)) != 1) {
            error = "Failed to update SHA-256";
            return false;
        }
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digestLength = 0;

    if (EVP_DigestFinal_ex(ctx.get(), digest, &digestLength) != 1) {
        error = "Failed to finalize SHA-256";
        return false;
    }

    std::ostringstream result;

    result << std::hex << std::setfill('0');

    for (unsigned int i = 0; i < digestLength; ++i) {
        result << std::setw(2)
               << static_cast<unsigned int>(digest[i]);
    }

    hash = result.str();

    return true;
}

std::string ToUtf8(const wxString& value)
{
    const wxScopedCharBuffer buffer = value.utf8_str();
    return buffer.data() ? std::string(buffer.data(), buffer.length()) : std::string();
}

std::string NormalizeRelativePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}

std::string NormalizeRemoteRoot(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty())
        return "/";

    const bool absolute = path.front() == '/';
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

std::string JoinRemote(const std::string& base, const std::string& relative)
{
    if (relative.empty())
        return base;
    if (base.empty() || base == ".")
        return relative;
    if (base.back() == '/')
        return base + relative;
    return base + "/" + relative;
}

wxString EnsureTrailingSeparator(wxString path)
{
    const wxChar separator = wxFileName::GetPathSeparator();
    if (!path.empty() && path.Last() != separator
        && path.Last() != '/' && path.Last() != '\\') {
        path += separator;
    }
    return path;
}

} // namespace

SyncWorker::SyncWorker(wxEvtHandler* eventTarget, Settings settings,
                       std::shared_ptr<std::atomic_bool> cancelFlag)
    : wxThread(wxTHREAD_DETACHED), eventTarget_(eventTarget),
      settings_(std::move(settings)), cancelFlag_(std::move(cancelFlag))
{
    if (!cancelFlag_)
        cancelFlag_ = std::make_shared<std::atomic_bool>(false);
}

SyncWorker::~SyncWorker() = default;

bool SyncWorker::Start()
{
    if (Create() != wxTHREAD_NO_ERROR)
        return false;
    return Run() == wxTHREAD_NO_ERROR;
}

void SyncWorker::Stop()
{
    cancelFlag_->store(true, std::memory_order_relaxed);
}

wxThread::ExitCode SyncWorker::Entry()
{
    bool success = false;
    wxString finalMessage = "Synchronization failed.";

    try {
        if (IsCancelled()) {
            finalMessage = "Synchronization cancelled.";
        } else if (settings_.localFolder.empty() || !wxDirExists(settings_.localFolder)) {
            finalMessage = "The configured local folder does not exist.";
            PostLog(finalMessage);
        } else {
            wxString error;
            sftp_.SetCancellationFlag(cancelFlag_.get());
            if (!sftp_.Connect(settings_, error)) {
                finalMessage = "SFTP connection failed: " + error;
                PostLog(finalMessage);
            } else {
                const wxString rootWithSeparator = EnsureTrailingSeparator(settings_.localFolder);
                std::vector<LocalFile> files;
                std::vector<std::string> directories;

                PostLog("Connected. Scanning local folder...");
                if (!GatherLocalTree(rootWithSeparator, rootWithSeparator,
                                     files, directories)) {
                    finalMessage = IsCancelled() ? "Synchronization cancelled."
                                                 : "Could not scan the local folder.";
                    PostLog(finalMessage);
                } else if (IsCancelled()) {
                    finalMessage = "Synchronization cancelled.";
                } else {
                    std::sort(files.begin(), files.end(), [](const LocalFile& a,
                                                             const LocalFile& b) {
                        return a.relativePath < b.relativePath;
                    });
                    std::sort(directories.begin(), directories.end());

                    std::string remoteRoot = NormalizeRemoteRoot(ToUtf8(settings_.remoteFolder));
                    if (settings_.deleteExtraneous
                        && (remoteRoot == "/" || remoteRoot == ".")) {
                        finalMessage = "Deletion is disabled when the remote folder is the "
                                       "filesystem root or the SFTP starting directory. "
                                       "Choose a dedicated remote directory instead.";
                        PostLog(finalMessage);
                    } else {
                        PostLog(wxString::Format("Found %lu file(s) and %lu director(y/ies).",
                                                 static_cast<unsigned long>(files.size()),
                                                 static_cast<unsigned long>(directories.size())));
                        if (!sftp_.MakeDirRecursive(remoteRoot, error)) {
                            finalMessage = "Could not create remote root: " + error;
                            PostLog(finalMessage);
                        } else if (!CreateLocalDirectories(remoteRoot, directories)) {
                            finalMessage = IsCancelled() ? "Synchronization cancelled."
                                                         : "Could not create one or more remote directories.";
                        } else if (!UploadLocalFiles(files, remoteRoot)) {
                            finalMessage = IsCancelled() ? "Synchronization cancelled."
                                                         : "One or more files could not be synchronized.";
                        } else {
                            bool pruneOk = true;
                            if (settings_.deleteExtraneous && !IsCancelled()) {
                                PostLog("Checking remote files for items to delete...");
                                std::unordered_set<std::string> localFiles;
                                std::unordered_set<std::string> localDirectories;
                                for (const auto& file : files)
                                    localFiles.insert(file.relativePath);
                                for (const auto& directory : directories)
                                    localDirectories.insert(directory);
                                pruneOk = PruneRemoteDirectory(remoteRoot, "",
                                                               localFiles, localDirectories);
                            }

                            if (IsCancelled()) {
                                finalMessage = "Synchronization cancelled.";
                            } else if (!pruneOk) {
                                finalMessage = "Some extraneous remote files could not be deleted.";
                            } else {
                                success = true;
                                finalMessage = "Synchronization completed successfully.";
                            }
                        }
                    }
                }
            }
        }
    } catch (const std::exception& exception) {
        finalMessage = wxString::Format("Unexpected sync error: %s",
                                        wxString::FromUTF8(exception.what()));
        PostLog(finalMessage);
    } catch (...) {
        finalMessage = "An unknown error occurred during synchronization.";
        PostLog(finalMessage);
    }

    sftp_.Disconnect();
    const bool cancelled = IsCancelled();
    if (cancelled) {
        success = false;
        finalMessage = "Synchronization cancelled.";
    }
    PostDone(success, cancelled, finalMessage);
    return nullptr;
}

bool SyncWorker::GatherLocalTree(const wxString& root, const wxString& current,
                                 std::vector<LocalFile>& files,
                                 std::vector<std::string>& directories)
{
    if (IsCancelled())
        return false;

    wxDir directory(current);
    if (!directory.IsOpened()) {
        PostLog("Could not open local directory: " + current);
        return false;
    }

    wxString name;
    const int flags = wxDIR_FILES | wxDIR_DIRS | wxDIR_HIDDEN;
    bool hasEntry = directory.GetFirst(&name, wxEmptyString, flags);
    while (hasEntry) {
        if (IsCancelled())
            return false;
        if (name != "." && name != "..") {
            const wxString fullPath = wxFileName(current, name).GetFullPath();
            wxFileName info(fullPath);
            // Do not follow local symbolic links: a directory link can point
            // back to an ancestor and otherwise create an infinite walk.
            if (info.Exists(wxFILE_EXISTS_SYMLINK)) {
                PostLog("Skipping symbolic link: " + fullPath);
            } else if (wxDirExists(fullPath)) {
                const wxString relative = fullPath.Mid(root.length());
                const std::string relativeUtf8 = NormalizeRelativePath(ToUtf8(relative));
                if (!relativeUtf8.empty())
                    directories.push_back(relativeUtf8);
                if (!GatherLocalTree(root, EnsureTrailingSeparator(fullPath),
                                     files, directories)) {
                    return false;
                }
            } else if (info.FileExists()) {
                const wxString relative = fullPath.Mid(root.length());
                const std::string relativeUtf8 = NormalizeRelativePath(ToUtf8(relative));
                if (!relativeUtf8.empty()) {
                    LocalFile file;
                    file.fullPath = fullPath;
                    file.relativePath = relativeUtf8;
                    const wxULongLong fileSize = info.GetSize();
                    file.size = fileSize.GetValue();
                    const wxDateTime modified = info.GetModificationTime();
                    file.mtime = modified.IsValid()
                        ? static_cast<time_t>(modified.GetTicks()) : 0;
                    files.push_back(std::move(file));
                }
            }
        }
        hasEntry = directory.GetNext(&name);
    }
    return true;
}

bool SyncWorker::CreateLocalDirectories(const std::string& remoteRoot,
                                        const std::vector<std::string>& directories)
{
    for (const auto& relative : directories) {
        if (IsCancelled())
            return false;
        wxString error;
        const std::string remotePath = JoinRemote(remoteRoot, relative);
        if (!sftp_.MakeDirRecursive(remotePath, error)) {
            PostLog("Could not create remote directory '"
                    + wxString::FromUTF8(remotePath.c_str()) + "': " + error);
            return false;
        }
        PostLog("Directory ready: " + wxString::FromUTF8(remotePath.c_str()));
    }
    return true;
}


bool SyncWorker::UploadLocalFiles(const std::vector<LocalFile>& files,
                                  const std::string& remoteRoot)
{
    const int total = static_cast<int>(files.size());
    PostProgress(0, total);

    int current = 0;

    for (const auto& file : files) {
        if (IsCancelled())
            return false;

        const std::string remotePath =
            JoinRemote(remoteRoot, file.relativePath);

        uint64_t remoteSize = 0;
        time_t remoteMtime = 0;

        const bool exists =
            sftp_.GetRemoteStat(remotePath, remoteSize, remoteMtime);

        bool shouldUpload = !exists || remoteSize != file.size;

        // If sizes match, compare actual file contents.
        if (exists && remoteSize == file.size) {
            std::string localHash;
            std::string remoteHash;
            wxString error;

            if (!ComputeLocalSha256(file.fullPath, localHash, error)) {
                PostLog("Local hash failed: " +
                        wxString::FromUTF8(file.relativePath.c_str()) +
                        " — " + error);
                return false;
            }

            if (!sftp_.GetRemoteFileHash(remotePath, remoteHash, error)) {
                if (!IsCancelled()) {
                    PostLog("Remote hash failed: " +
                            wxString::FromUTF8(file.relativePath.c_str()) +
                            " — " + error);
                }
                return false;
            }

            shouldUpload = (localHash != remoteHash);
        }

        if (shouldUpload) {
            wxString error;

            PostLog("Uploading: " +
                    wxString::FromUTF8(file.relativePath.c_str()));

            if (!sftp_.UploadFile(file.fullPath, remotePath, error)) {
                if (!IsCancelled()) {
                    PostLog("Upload failed: " +
                            wxString::FromUTF8(file.relativePath.c_str()) +
                            " — " + error);
                }
                return false;
            }

            PostLog("Uploaded: " +
                    wxString::FromUTF8(file.relativePath.c_str()));
        } else {
            PostLog("Up to date: " +
                    wxString::FromUTF8(file.relativePath.c_str()));
        }

        ++current;
        PostProgress(current, total);
    }

    return true;
}


bool SyncWorker::PruneRemoteDirectory(
    const std::string& remoteDirectory, const std::string& relativeDirectory,
    const std::unordered_set<std::string>& localFiles,
    const std::unordered_set<std::string>& localDirectories)
{
    if (IsCancelled())
        return false;

    std::vector<std::string> names;
    std::vector<bool> isDirectory;
    if (!sftp_.ListRemote(remoteDirectory, names, isDirectory)) {
        PostLog("Could not list remote directory: " + wxString::FromUTF8(remoteDirectory.c_str()));
        return false;
    }

    bool success = true;
    for (size_t index = 0; index < names.size(); ++index) {
        if (IsCancelled())
            return false;

        const std::string relativePath = relativeDirectory.empty()
            ? names[index] : relativeDirectory + "/" + names[index];
        const std::string remotePath = JoinRemote(remoteDirectory, names[index]);

        if (isDirectory[index]) {
            if (localDirectories.find(relativePath) == localDirectories.end()) {
                PostLog("Removing extraneous directory: " + wxString::FromUTF8(relativePath.c_str()));
                if (!RemoveRemoteTree(remotePath))
                    success = false;
            } else if (!PruneRemoteDirectory(remotePath, relativePath,
                                              localFiles, localDirectories)) {
                success = false;
            }
        } else if (localFiles.find(relativePath) == localFiles.end()) {
            wxString error;
            PostLog("Removing extraneous file: " + wxString::FromUTF8(relativePath.c_str()));
            if (!sftp_.RemoveFile(remotePath, error)) {
                PostLog("Could not remove '" + wxString::FromUTF8(relativePath.c_str())
                        + "': " + error);
                success = false;
            }
        }
    }
    return success;
}

bool SyncWorker::RemoveRemoteTree(const std::string& remotePath)
{
    if (IsCancelled())
        return false;

    std::vector<std::string> names;
    std::vector<bool> isDirectory;
    if (!sftp_.ListRemote(remotePath, names, isDirectory)) {
        PostLog("Could not inspect remote directory: " + wxString::FromUTF8(remotePath.c_str()));
        return false;
    }

    bool success = true;
    for (size_t index = 0; index < names.size(); ++index) {
        if (IsCancelled())
            return false;
        const std::string childPath = JoinRemote(remotePath, names[index]);
        if (isDirectory[index]) {
            if (!RemoveRemoteTree(childPath))
                success = false;
        } else {
            wxString error;
            if (!sftp_.RemoveFile(childPath, error)) {
                PostLog("Could not remove remote file '" + wxString::FromUTF8(childPath.c_str())
                        + "': " + error);
                success = false;
            }
        }
    }

    if (success) {
        wxString error;
        if (!sftp_.RemoveDirectory(remotePath, error)) {
            PostLog("Could not remove remote directory '" + wxString::FromUTF8(remotePath.c_str())
                    + "': " + error);
            success = false;
        }
    }
    return success;
}

void SyncWorker::PostLog(const wxString& message)
{
    if (!eventTarget_)
        return;
    auto* event = new wxThreadEvent(EVT_SYNC_LOG);
    event->SetString(message);
    wxQueueEvent(eventTarget_, event);
}

void SyncWorker::PostProgress(int current, int total)
{
    if (!eventTarget_)
        return;
    auto* event = new wxThreadEvent(EVT_SYNC_PROGRESS);
    event->SetPayload(SyncProgressPayload{current, total});
    wxQueueEvent(eventTarget_, event);
}

void SyncWorker::PostDone(bool success, bool cancelled, const wxString& message)
{
    if (!eventTarget_)
        return;
    auto* event = new wxThreadEvent(EVT_SYNC_DONE);
    event->SetPayload(SyncDonePayload{success, cancelled, message});
    wxQueueEvent(eventTarget_, event);
}

bool SyncWorker::IsCancelled() const
{
    return cancelFlag_->load(std::memory_order_relaxed);
}
