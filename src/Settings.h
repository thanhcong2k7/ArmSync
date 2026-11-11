#pragma once

#include <wx/string.h>

struct Settings {
    wxString host;
    int port = 22;
    wxString username;
    wxString password;
    wxString privateKeyPath;
    bool useKeyAuth = false;
    wxString localFolder;
    wxString remoteFolder = "/home";
    bool deleteExtraneous = false;

    static Settings Load();
    void Save() const;
};
