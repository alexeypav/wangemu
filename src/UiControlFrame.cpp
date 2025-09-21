#include "UiControlFrame.h"
#include "UiSystem.h"
#include "UiDiskCtrlCfgDlg.h"
#include "UiTermMuxCfgDlg.h"
#include "UiPrinterFrame.h"
#include "UiSystemConfigDlg.h"
#include "UiDiskFactory.h"
#include "IoCardDisk.h"
#include "IoCardKeyboard.h"
#include "system2200.h"
#include "host.h"
#include "compile_options.h"

enum { TB_NONE = 0 }; // no toolbar/status bar needed

wxBEGIN_EVENT_TABLE(ControlFrame, wxFrame)
    EVT_MENU_OPEN(ControlFrame::OnMenuOpen)
wxEND_EVENT_TABLE()

ControlFrame::ControlFrame()
: wxFrame(nullptr, wxID_ANY, "WangEmu Control", wxDefaultPosition, wxDefaultSize,
          wxCAPTION | wxCLOSE_BOX | wxMINIMIZE_BOX | wxSYSTEM_MENU)
{
    buildMenus();
    SetMinSize(wxSize(420, 100));
    Show(true);
}

void ControlFrame::buildMenus()
{
    wxMenu *menu_file = new wxMenu;
    menu_file->Append(File_Quit, "E&xit    Alt-X", "Quit the program");

    wxMenu *menu_cpu = new wxMenu;
    menu_cpu->Append(CPU_HardReset, "Reboot CPU", "Perform a power-up reset");
    menu_cpu->Append(CPU_WarmReset, "Reset CPU    Ctrl+R", "Perform a state-preserving reset");
    menu_cpu->AppendSeparator();
    menu_cpu->AppendCheckItem(CPU_ActualSpeed,      "&Actual Speed",      "Run emulation at machine speed");
    menu_cpu->AppendCheckItem(CPU_UnregulatedSpeed, "&Unregulated Speed", "Run emulation at maximum speed");

    wxMenu *menu_disk = new wxMenu;   // entries filled dynamically like CrtFrame
    // populate standard items
    menu_disk->Append(Disk_New,     "New...",     "Create a new virtual disk image");
    menu_disk->Append(Disk_Inspect, "Inspect...", "Open the Disk Factory");
    menu_disk->Append(Disk_Format,  "Format...",  "Low-level format");
    menu_disk->AppendSeparator();
    // slot drive items handled in OnDisk (same pattern as CrtFrame)
    menu_disk->AppendSeparator();
    menu_disk->AppendCheckItem(Disk_Realtime,        "Realtime Speed",      "Regulate disk controller speed");
    menu_disk->AppendCheckItem(Disk_UnregulatedSpeed,"Unregulated Speed",   "Run disk controller flat out");

    wxMenu *menu_config = new wxMenu;
    menu_config->Append(Configure_Dialog, "&System...", "Configure CPU, RAM, and I/O cards");

    m_menubar = new wxMenuBar;
    m_menubar->Append(menu_file,  "&File");
    m_menubar->Append(menu_cpu,   "&CPU");
    m_menubar->Append(menu_disk,  "&Disk");
    m_menubar->Append(menu_config,"&Configure");
    m_menubar->Append(TheApp::makeHelpMenu(this), "&Help");
    SetMenuBar(m_menubar);

    // bindings (same handlers CrtFrame uses, but methods are on ControlFrame)
    Bind(wxEVT_MENU, &ControlFrame::OnQuit,        this, File_Quit);
    Bind(wxEVT_MENU, &ControlFrame::OnReset,       this, CPU_HardReset);
    Bind(wxEVT_MENU, &ControlFrame::OnReset,       this, CPU_WarmReset);
    Bind(wxEVT_MENU, &ControlFrame::OnCpuSpeed,    this, CPU_ActualSpeed);
    Bind(wxEVT_MENU, &ControlFrame::OnCpuSpeed,    this, CPU_UnregulatedSpeed);
    Bind(wxEVT_MENU, &ControlFrame::OnDiskFactory, this, Disk_New);
    Bind(wxEVT_MENU, &ControlFrame::OnDiskFactory, this, Disk_Inspect);
    Bind(wxEVT_MENU, &ControlFrame::OnDiskFactory, this, Disk_Format);
    Bind(wxEVT_COMMAND_MENU_SELECTED, &ControlFrame::OnDisk, this,
                            Disk_Insert, Disk_Insert+NUM_IOSLOTS*8-1);
    Bind(wxEVT_MENU, &ControlFrame::OnDiskSpeed,   this, Disk_Realtime);
    Bind(wxEVT_MENU, &ControlFrame::OnDiskSpeed,   this, Disk_UnregulatedSpeed);
    Bind(wxEVT_MENU, &ControlFrame::OnConfigureDialog, this, Configure_Dialog);
}

void ControlFrame::OnMenuOpen(wxMenuEvent &event)
{
    setMenuChecks(event.GetMenu());
}

void ControlFrame::setMenuChecks(const wxMenu *menu)
{
    if (!m_menubar) return;
    
    // Mirror logic from CrtFrame::setMenuChecks:
    if (menu == m_menubar->GetMenu(1)) { // CPU menu
        const bool regulated = system2200::isCpuSpeedRegulated();
        m_menubar->Check(CPU_ActualSpeed, regulated);
        m_menubar->Check(CPU_UnregulatedSpeed, !regulated);
    } else if (menu == m_menubar->GetMenu(2)) { // Disk menu
        const bool regulated = system2200::isDiskRealtime();
        m_menubar->Check(Disk_Realtime, regulated);
        m_menubar->Check(Disk_UnregulatedSpeed, !regulated);
        // If you want per-slot enabled/disabled, call into UiDiskFactory to refresh.
    }
}

void ControlFrame::OnQuit(wxCommandEvent&)            
{ 
    system2200::terminate(); 
}

void ControlFrame::OnReset(wxCommandEvent &e)         
{ 
    if (e.GetId()==CPU_HardReset) {
        system2200::reset(true);
    } else {
        system2200::dispatchKeystroke(0x01, 0, IoCardKeyboard::KEYCODE_RESET);
    }
}

void ControlFrame::OnCpuSpeed(wxCommandEvent &e)      
{ 
    system2200::regulateCpuSpeed(e.GetId()==CPU_ActualSpeed); 
}

void ControlFrame::OnDiskSpeed(wxCommandEvent &e)     
{ 
    system2200::setDiskRealtime(e.GetId()==Disk_Realtime); 
}

void ControlFrame::OnDiskFactory(wxCommandEvent &e)   
{ 
    std::string filename;
    if (e.GetId() == Disk_Inspect) {
        if (host::fileReq(host::FILEREQ_DISK, "Virtual Disk Name", true, &filename) !=
                          host::FILEREQ_OK) {
            return;     // canceled
        }
    } else if (e.GetId() == Disk_Format) {
        if (host::fileReq(host::FILEREQ_DISK, "Virtual Disk Name", true, &filename) !=
                          host::FILEREQ_OK) {
            return; // cancelled
        }
        // do format operation similar to CrtFrame::doFormat
        system2200::freezeEmu(true);    // halt emulation
        bool wp;
        bool ok = IoCardDisk::wvdGetWriteProtect(filename, &wp);
        if (ok && !wp) {
            DiskFactory dlg(this, filename);
            dlg.ShowModal();
        }
        system2200::freezeEmu(false);   // run emulation
        return;
    }
    
    // For inspect and new, do inspect operation similar to CrtFrame::doInspect
    system2200::freezeEmu(true);    // halt emulation

    int slot, drive;
    const bool in_use = system2200::findDisk(filename, &slot, &drive, nullptr);
    if (in_use) {
        // close filehandles to the specified drive
        IoCardDisk::wvdFlush(slot, drive);
    }

    DiskFactory dlg(this, filename);
    dlg.ShowModal();

    system2200::freezeEmu(false);   // run emulation
}

void ControlFrame::OnDisk(wxCommandEvent &e)          
{ 
    // Handle disk insert/remove operations like CrtFrame does
    // This is a placeholder - the actual implementation would need
    // the same logic as CrtFrame::OnDisk for slot/drive management
}

void ControlFrame::OnConfigureDialog(wxCommandEvent &) 
{ 
    system2200::reconfigure(); 
}