#include "Settings.h"

#include <wx/config.h>
#include <wx/string.h>

#if defined(wxUSE_SECRETSTORE) && wxUSE_SECRETSTORE
    #include <wx/secretstore.h>
#endif

namespace {

wxString SecretService(const wxString& host, int port)
{
    return wxString::Format("ArmSync/%s:%d", host, port);
}

} // namespace

Settings Settings::Load()
{
    Settings settings;
    wxConfigBase* config = wxConfigBase::Get(false);
    if (!config)
        return settings;

    config->Read("host", &settings.host, wxEmptyString);
    long port = 22;
    config->Read("port", &port, 22L);
    settings.port = (port >= 1 && port <= 65535) ? static_cast<int>(port) : 22;
    config->Read("username", &settings.username, wxEmptyString);
    config->Read("privateKeyPath", &settings.privateKeyPath, wxEmptyString);
    config->Read("useKeyAuth", &settings.useKeyAuth, false);
    config->Read("localFolder", &settings.localFolder, wxEmptyString);
    config->Read("remoteFolder", &settings.remoteFolder, "/home");
    config->Read("deleteExtraneous", &settings.deleteExtraneous, false);

    // The marker prevents a stale secret-store entry from overriding the
    // wxConfig fallback when a later secret-store write failed. For older
    // configurations without the marker, infer the legacy behavior from the
    // presence (or absence) of the fallback password entry.
    bool useSecretStore = false;
    if (config->HasEntry("passwordInSecretStore")) {
        config->Read("passwordInSecretStore", &useSecretStore, false);
    } else {
        useSecretStore = !config->HasEntry("password");
    }

    bool loadedFromSecretStore = false;
#if defined(wxUSE_SECRETSTORE) && wxUSE_SECRETSTORE
    if (useSecretStore) {
        wxSecretStore store = wxSecretStore::GetDefault();
        if (store.IsOk() && !settings.host.empty()) {
            wxString storedUser;
            wxSecretValue secret;
            if (store.Load(SecretService(settings.host, settings.port), storedUser, secret)
                && storedUser == settings.username) {
                settings.password = secret.GetAsString();
                loadedFromSecretStore = true;
            }
        }
    }
#endif

    if (!loadedFromSecretStore)
        config->Read("password", &settings.password, wxEmptyString);

    return settings;
}

void Settings::Save() const
{
    wxConfigBase* config = wxConfigBase::Get(false);
    if (!config)
        return;

    config->Write("host", host);
    config->Write("port", static_cast<long>(port));
    config->Write("username", username);
    config->Write("privateKeyPath", privateKeyPath);
    config->Write("useKeyAuth", useKeyAuth);
    config->Write("localFolder", localFolder);
    config->Write("remoteFolder", remoteFolder);
    config->Write("deleteExtraneous", deleteExtraneous);

    bool storedInSecretStore = false;
#if defined(wxUSE_SECRETSTORE) && wxUSE_SECRETSTORE
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk() && !host.empty() && !username.empty()) {
        storedInSecretStore = store.Save(SecretService(host, port), username,
                                         wxSecretValue(password));
    }
#endif

    // Keep a config fallback for platforms where wxSecretStore is unavailable
    // or its backend refuses the write. The marker avoids loading a stale
    // credential left in the secret store after such a failure.
    config->Write("passwordInSecretStore", storedInSecretStore);
    if (storedInSecretStore)
        config->DeleteEntry("password");
    else
        config->Write("password", password);

    config->Flush();
}
