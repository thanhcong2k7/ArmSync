#include "App.h"

#include "Events.h"
#include "MainFrame.h"

#include <wx/config.h>

wxDEFINE_EVENT(EVT_SYNC_LOG, wxThreadEvent);
wxDEFINE_EVENT(EVT_SYNC_PROGRESS, wxThreadEvent);
wxDEFINE_EVENT(EVT_SYNC_DONE, wxThreadEvent);

wxIMPLEMENT_APP(App);

bool App::OnInit()
{
    if (!wxApp::OnInit())
        return false;

    SetAppName("ArmSync");
    SetVendorName("ArmSync");
    wxConfigBase::Set(new wxConfig("ArmSync"));

    auto* frame = new MainFrame();
    frame->Show(true);
    return true;
}

int App::OnExit()
{
    // wxWidgets owns the global config object after it is installed with Set().
    return wxApp::OnExit();
}
