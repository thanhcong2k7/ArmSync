#pragma once

#include <wx/app.h>

class App final : public wxApp {
public:
    bool OnInit() override;
    int OnExit() override;
};
