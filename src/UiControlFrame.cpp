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
#include "Ui.h"

#include <cassert>

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

    wxMenu *menu_disk = new wxMenu;   // entries filled dynamically in setMenuChecks

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
        // Dynamically generate the disk menu each time
        int disk_menu_pos = m_menubar->FindMenu("Disk");
        if (disk_menu_pos >= 0 && (menu == m_menubar->GetMenu(disk_menu_pos))) {
            wxMenu *disk_menu = m_menubar->GetMenu(disk_menu_pos);
            const int items = disk_menu->GetMenuItemCount();

            // Remove all existing menu items
            for (int i=items-1; i>=0; i--) {
                wxMenuItem *item = disk_menu->FindItemByPosition(i);
                disk_menu->Delete(item);
            }

            // Regenerate the disk menu with current drive states
            rebuildDiskMenu(disk_menu);
        }
    }
}

void ControlFrame::rebuildDiskMenu(wxMenu *disk_menu)
{
    // See if there are any disk controllers and build dynamic menu items
    for (int controller=0; ; controller++) {
        int slot = 0, io_addr = 0;
        if (!system2200::findDiskController(controller, &slot)) {
            break;
        }
        const bool ok = system2200::getSlotInfo(slot, nullptr, &io_addr);
        assert(ok);
        for (int d=0; d < 4; d++) {
            const int stat = IoCardDisk::wvdDriveStatus(slot, d);
            if ((stat & IoCardDisk::WVD_STAT_DRIVE_EXISTENT) == 0) {
                break;
            }
            const char drive_ch = ((d & 1) == 0) ? 'F' : 'R';
            const int  addr_off = ((d & 2) == 0) ? 0x00 : 0x40;
            const int  eff_addr = io_addr + addr_off;
            if ((stat & IoCardDisk::WVD_STAT_DRIVE_OCCUPIED) != 0) {
                wxString str1, str2;
                str1.Printf("Drive %c/%03X: Remove", drive_ch, eff_addr);
                str2.Printf("Remove the disk from drive %d, unit /%03X", d, eff_addr);
                disk_menu->Append(Disk_Remove+8*slot+2*d, str1, str2, wxITEM_CHECK);
            } else {
                wxString str1, str2;
                str1.Printf("Drive %c/%03X: Insert", drive_ch, eff_addr);
                str2.Printf("Insert a disk into drive %d, unit /%03X", d, eff_addr);
                disk_menu->Append(Disk_Insert+8*slot+2*d, str1, str2, wxITEM_CHECK);
            }
        }
        disk_menu->AppendSeparator();
    }
    
    // Add the static menu items
    disk_menu->Append(Disk_New,     "&New Disk...",     "Create virtual disk");
    disk_menu->Append(Disk_Inspect, "&Inspect Disk...", "Inspect/modify virtual disk");
    disk_menu->Append(Disk_Format,  "&Format Disk...",  "Format existing virtual disk");

    const bool disk_realtime = system2200::isDiskRealtime();
    disk_menu->AppendSeparator();
    disk_menu->Append(Disk_Realtime,         "Realtime Disk Speed",  "Emulate actual disk timing",             wxITEM_CHECK);
    disk_menu->Append(Disk_UnregulatedSpeed, "Unregulated Speed",    "Make disk accesses as fast as possible", wxITEM_CHECK);
    disk_menu->Check(Disk_Realtime,          disk_realtime);
    disk_menu->Check(Disk_UnregulatedSpeed, !disk_realtime);
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
    // Each controller manages two drives, each has three possible actions
    const int menu_id = e.GetId();
    const int slot  =  (menu_id - Disk_Insert) / 8;
    const int drive = ((menu_id - Disk_Insert) % 8) / 2;
    const int type  =  (menu_id - Disk_Insert) % 2;

    bool ok = true;
    switch (type) {

        case 0: // insert disk
        {   std::string full_path;
            if (host::fileReq(host::FILEREQ_DISK, "Disk to load", true, &full_path) ==
                              host::FILEREQ_OK) {
                int drive2, io_addr2;
                const bool b = system2200::findDisk(full_path, nullptr, &drive2, &io_addr2);
                const int eff_addr = io_addr2 + ((drive2 < 2) ? 0x00 : 0x40);
                if (b) {
                    UI_warn("Disk already in drive %c /%03x", "FRFR"[drive2], eff_addr);
                    return;
                }
                ok = IoCardDisk::wvdInsertDisk(slot, drive, full_path);
            }
        }   break;

        case 1: // remove disk
            ok = IoCardDisk::wvdRemoveDisk(slot, drive);
            break;

        default:
            ok = false;
            assert(false);
            break;
    }

    if (!ok) {
        UI_error("Error: operation failed");
    }
}

void ControlFrame::OnConfigureDialog(wxCommandEvent &) 
{ 
    system2200::reconfigure(); 
}