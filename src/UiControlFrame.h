#ifndef _INCLUDE_UI_CONTROL_FRAME_H_
#define _INCLUDE_UI_CONTROL_FRAME_H_

#include "w2200.h"
#include "wx/wx.h"

class ControlFrame : public wxFrame {
public:
    ControlFrame();
    ~ControlFrame() override = default;

private:
    wxMenuBar *m_menubar = nullptr;

    // menu IDs (copy the few you need from UiCrtFrame.cpp or move them to a shared header)
    enum {
        File_Quit = wxID_EXIT,

        CPU_HardReset = 10001,
        CPU_WarmReset,
        CPU_ActualSpeed,
        CPU_UnregulatedSpeed,

        Disk_New,
        Disk_Inspect,
        Disk_Format,
        Disk_Insert,   // handled dynamically like in CrtFrame
        Disk_Remove,   // handled dynamically like in CrtFrame
        Disk_Realtime,
        Disk_UnregulatedSpeed,

        Configure_Dialog
    };

    void buildMenus();
    void setMenuChecks(const wxMenu *menu);
    void rebuildDiskMenu(wxMenu *disk_menu);

    // handlers (mirror the ones in UiCrtFrame.cpp but without CRT specifics)
    void OnQuit(wxCommandEvent &);
    void OnReset(wxCommandEvent &);
    void OnCpuSpeed(wxCommandEvent &);
    void OnDiskFactory(wxCommandEvent &);
    void OnDisk(wxCommandEvent &);
    void OnDiskSpeed(wxCommandEvent &);
    void OnConfigureDialog(wxCommandEvent &);
    void OnMenuOpen(wxMenuEvent &);

    wxDECLARE_EVENT_TABLE();
};

#endif