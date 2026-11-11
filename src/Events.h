#pragma once

#include <wx/event.h>
#include <wx/string.h>
#include <wx/thread.h>

#include <string>

// These events are always posted with wxQueueEvent from the worker thread.
wxDECLARE_EVENT(EVT_SYNC_LOG, wxThreadEvent);
wxDECLARE_EVENT(EVT_SYNC_PROGRESS, wxThreadEvent);
wxDECLARE_EVENT(EVT_SYNC_DONE, wxThreadEvent);

struct SyncProgressPayload {
    int current = 0;
    int total = 0;
};

struct SyncDonePayload {
    bool success = false;
    bool cancelled = false;
    wxString message;
};
