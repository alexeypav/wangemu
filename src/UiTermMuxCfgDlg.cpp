
#include "IoCard.h"
#include "TermMuxCfgState.h"
#include "Ui.h"                 // emulator interface
#include "UiSystem.h"           // sharing info between UI_wxgui modules
#include "UiTermMuxCfgDlg.h"
#include "host.h"
#include "system2200.h"

#include <cassert>

// ----------------------------------------------------------------------------
// a simple static dialog to provide help on the TermMuxCfgDlg options
// ----------------------------------------------------------------------------

class TermMuxCfgHelpDlg : public wxDialog
{
public:
    CANT_ASSIGN_OR_COPY_CLASS(TermMuxCfgHelpDlg);
    explicit TermMuxCfgHelpDlg(wxWindow *parent);
    //DECLARE_EVENT_TABLE()
};


TermMuxCfgHelpDlg::TermMuxCfgHelpDlg(wxWindow *parent)
        : wxDialog(parent, -1, "Terminal Mux Controller Configuration Help",
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    wxTextCtrl *txt = new wxTextCtrl(this, wxID_ANY, "",
                               wxDefaultPosition, wxSize(480, 400),
                               wxTE_RICH2 | wxTE_MULTILINE | wxTE_READONLY |
                               wxBORDER_NONE);

    txt->SetBackgroundColour(// wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE)
                                wxColour(0xec, 0xe9, 0xd8));

    // font for section headings
    wxFont section_font(12, wxFONTFAMILY_DEFAULT,
                            wxFONTSTYLE_NORMAL,
                            wxFONTWEIGHT_BOLD);

    wxColor section_color("BLACK");
    wxTextAttr section_attr;
    section_attr.SetTextColour(section_color);
    section_attr.SetFont(section_font);
    section_attr.SetLeftIndent(12);
    section_attr.SetRightIndent(12);

    // font for body of text
    wxFont body_font(10, wxFONTFAMILY_DEFAULT,
                         wxFONTSTYLE_NORMAL,
                         wxFONTWEIGHT_NORMAL);
    wxColor body_color(wxColour(0x00, 0x00, 0xC0));
    wxTextAttr body_attr;
    body_attr.SetTextColour(body_color);
    body_attr.SetFont(body_font);
    body_attr.SetLeftIndent(50);
    body_attr.SetRightIndent(12);

    // create the message
    txt->SetDefaultStyle(section_attr);
    txt->AppendText("Number of Terminals\n");

    txt->SetDefaultStyle(body_attr);
    txt->AppendText(
        "\n"
        "Each 2236MXD controller supports from one to four terminals. "
        "Each terminal can be configured to either display as a GUI window "
        "or connect to a host COM port for use with external terminal programs."
        "\n\n");

    txt->SetDefaultStyle(section_attr);
    txt->AppendText("COM Port Configuration\n");

    txt->SetDefaultStyle(body_attr);
    txt->AppendText(
        "\n"
        "For each terminal, you can:\n"
        "• Enable \"Use COM Port\" to redirect the terminal to a host serial port\n"
        "• Set the COM port name (COM1, COM2, etc.)\n"
        "• Configure the baud rate (9600, 19200, 38400, 57600, or 115200)\n"
        "• Enable XON/XOFF flow control for proper data pacing (recommended for Wang terminals)\n"
        "\n"
        "When a terminal uses a COM port, no GUI window will be created for it. "
        "Instead, you can connect external terminal software to the specified "
        "COM port to interact with the emulated Wang system."
        "\n\n");

    txt->SetDefaultStyle(section_attr);
    txt->AppendText("Compatibility\n");

    txt->SetDefaultStyle(body_attr);
    txt->AppendText(
        "\n"
        "The MXD can be used by Wang VP and Wang MVP OS's, though "
        "multiple terminals are supported by only the MVP OS's."
        "\n\n"
        "The MXD can be used in a 2200B or 2200T as it mimics a "
        "keyboard at I/O 001 and a CRT controller at I/O 005, though "
        "the character set won't be exactly the same as a dumb "
        "controller.  Also, because the link to the serial terminal "
        "runs at 19200 baud, throughput can sometimes lag as compared "
        "to a dumb CRT controller."
        "\n\n");

    // make sure the start of text is at the top
    txt->SetInsertionPoint(0);
    txt->ShowPosition(0);

    // make it fill the window, and show it
    wxBoxSizer *sz = new wxBoxSizer(wxVERTICAL);
    sz->Add(txt, 1, wxEXPAND);
    SetSizerAndFit(sz);
}

// ----------------------------------------------------------------------------
// TermMuxCfgDlg implementation
// ----------------------------------------------------------------------------

enum
{
    ID_RB_NUM_TERMINALS = 100,          // radio box
    ID_CB_COM_PORT_1,                   // COM port checkboxes
    ID_CB_COM_PORT_2,
    ID_CB_COM_PORT_3,
    ID_CB_COM_PORT_4,
    ID_TC_COM_PORT_1,                   // COM port text controls
    ID_TC_COM_PORT_2,
    ID_TC_COM_PORT_3,
    ID_TC_COM_PORT_4,
    ID_CH_BAUD_RATE_1,                  // baud rate choices
    ID_CH_BAUD_RATE_2,
    ID_CH_BAUD_RATE_3,
    ID_CH_BAUD_RATE_4,
    ID_CB_FLOW_CONTROL_1,               // hardware flow control checkboxes
    ID_CB_FLOW_CONTROL_2,
    ID_CB_FLOW_CONTROL_3,
    ID_CB_FLOW_CONTROL_4,
    ID_CB_SW_FLOW_CONTROL_1,            // software flow control checkboxes
    ID_CB_SW_FLOW_CONTROL_2,
    ID_CB_SW_FLOW_CONTROL_3,
    ID_CB_SW_FLOW_CONTROL_4,
    ID_BTN_HELP   = 300,
    ID_BTN_REVERT
};


// Layout:
//      top_sizer (V)
//      |
//      +-- num drives radiobox (H)
//      +-- button_sizer (H)
//          |
//          +-- m_btn_help
//          +-- m_btn_revert
//          +-- m_btn_ok
//          +-- m_btn_cancel
TermMuxCfgDlg::TermMuxCfgDlg(wxFrame *parent, CardCfgState &cfg) :
        wxDialog(parent, -1, "Terminal Mux Controller Configuration",
                 wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
        m_cfg(dynamic_cast<TermMuxCfgState&>(cfg)),     // edited version
        m_old_cfg(dynamic_cast<TermMuxCfgState&>(cfg))  // copy of original
{
    const wxString choicesNumTerminals[] = { "1", "2", "3", "4" };
    m_rb_num_terminals = new wxRadioBox(this, ID_RB_NUM_TERMINALS,
                                        "Number of terminals",
                                        wxDefaultPosition, wxDefaultSize,
                                        4, &choicesNumTerminals[0],
                                        1, wxRA_SPECIFY_ROWS);

    // Create COM port configuration section
    m_sb_terminals = new wxStaticBox(this, wxID_ANY, "Terminal Configuration");
    wxStaticBoxSizer *terminal_sizer = new wxStaticBoxSizer(m_sb_terminals, wxVERTICAL);
    
    // Header row
    wxBoxSizer *header_sizer = new wxBoxSizer(wxHORIZONTAL);
    header_sizer->Add(new wxStaticText(this, wxID_ANY, "Terminal"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
    header_sizer->Add(new wxStaticText(this, wxID_ANY, "Use COM Port"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
    header_sizer->Add(new wxStaticText(this, wxID_ANY, "Port Name"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
    header_sizer->Add(new wxStaticText(this, wxID_ANY, "Baud Rate"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
    header_sizer->Add(new wxStaticText(this, wxID_ANY, "XON/XOFF Flow"), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
    terminal_sizer->Add(header_sizer, 0, wxEXPAND | wxALL, 2);
    
    // Create controls for each terminal
    const wxString baudChoices[] = { "9600", "19200", "38400", "57600", "115200" };
    for (int i = 0; i < 4; i++) {
        wxBoxSizer *term_sizer = new wxBoxSizer(wxHORIZONTAL);
        
        // Terminal number label
        wxString termLabel = wxString::Format("Terminal %d", i + 1);
        term_sizer->Add(new wxStaticText(this, wxID_ANY, termLabel), 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        // Use COM port checkbox
        m_cb_com_port[i] = new wxCheckBox(this, ID_CB_COM_PORT_1 + i, "");
        term_sizer->Add(m_cb_com_port[i], 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        // COM port name text control
        m_tc_com_port[i] = new wxTextCtrl(this, ID_TC_COM_PORT_1 + i, "COM1", wxDefaultPosition, wxSize(80, -1));
        term_sizer->Add(m_tc_com_port[i], 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        // Baud rate choice
        m_ch_baud_rate[i] = new wxChoice(this, ID_CH_BAUD_RATE_1 + i, wxDefaultPosition, wxSize(80, -1), 5, baudChoices);
        m_ch_baud_rate[i]->SetSelection(1); // default to 19200
        term_sizer->Add(m_ch_baud_rate[i], 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        // Software flow control checkbox (XON/XOFF for Wang terminals)
        // Note: Hardware flow control (RTS/CTS) is not used since Wang terminals don't support it
        m_cb_sw_flow_control[i] = new wxCheckBox(this, ID_CB_SW_FLOW_CONTROL_1 + i, "");
        term_sizer->Add(m_cb_sw_flow_control[i], 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);
        
        terminal_sizer->Add(term_sizer, 0, wxEXPAND | wxALL, 2);
    }

    // put three buttons side by side
    m_btn_help   = new wxButton(this, ID_BTN_HELP,   "Help");
    m_btn_revert = new wxButton(this, ID_BTN_REVERT, "Revert");
    m_btn_ok     = new wxButton(this, wxID_OK,       "OK");
    m_btn_cancel = new wxButton(this, wxID_CANCEL,   "Cancel");

    wxBoxSizer *button_sizer = new wxBoxSizer(wxHORIZONTAL);
    button_sizer->Add(m_btn_help,   0, wxALL, 10);
    button_sizer->Add(m_btn_revert, 0, wxALL, 10);
    button_sizer->Add(m_btn_ok,     0, wxALL, 10);
    button_sizer->Add(m_btn_cancel, 0, wxALL, 10);
#ifdef __WXMAC__
    // the cancel button was running into the window resizing grip
    button_sizer->AddSpacer(10);
#endif
    m_btn_revert->Disable();      // until something changes

    // all of it is stacked vertically
    wxBoxSizer *top_sizer = new wxBoxSizer(wxVERTICAL);
    top_sizer->Add(m_rb_num_terminals, 0, wxALIGN_LEFT  | wxALL, 5);
    top_sizer->Add(terminal_sizer,     0, wxEXPAND | wxALL, 5);
    top_sizer->Add(button_sizer,       0, wxALIGN_RIGHT | wxALL, 5);

    updateDlg();                        // select current options

    // tell the thing to get to work
    SetSizer(top_sizer);                 // use the sizer for layout
    top_sizer->SetSizeHints(this);       // set size hints to honor minimum size

    getDefaults();  // get default size & location

    // event routing table
    Bind(wxEVT_RADIOBOX, &TermMuxCfgDlg::OnNumTerminals, this, ID_RB_NUM_TERMINALS);
    
    // Bind COM port configuration events
    for (int i = 0; i < 4; i++) {
        Bind(wxEVT_CHECKBOX, &TermMuxCfgDlg::OnComPortChange, this, ID_CB_COM_PORT_1 + i);
        Bind(wxEVT_TEXT, &TermMuxCfgDlg::OnComPortChange, this, ID_TC_COM_PORT_1 + i);
        Bind(wxEVT_CHOICE, &TermMuxCfgDlg::OnBaudRateChange, this, ID_CH_BAUD_RATE_1 + i);
        Bind(wxEVT_CHECKBOX, &TermMuxCfgDlg::OnSwFlowControlChange, this, ID_CB_SW_FLOW_CONTROL_1 + i);
    }
    
    Bind(wxEVT_BUTTON,   &TermMuxCfgDlg::OnButton,       this, -1);
}


// update the display to reflect the current state
void
TermMuxCfgDlg::updateDlg()
{
    m_rb_num_terminals->SetSelection(m_cfg.getNumTerminals()-1);
    
    // Update COM port configuration for each terminal
    for (int i = 0; i < 4; i++) {
        bool useCom = m_cfg.isTerminalComPort(i);
        std::string comPort = m_cfg.getTerminalComPort(i);
        int baudRate = m_cfg.getTerminalBaudRate(i);
        bool swFlowControl = m_cfg.getTerminalSwFlowControl(i);
        
        // Set checkbox state
        m_cb_com_port[i]->SetValue(useCom);
        
        // Set COM port name (default to COM1 if empty)
        if (comPort.empty()) {
            comPort = "COM1";
        }
        m_tc_com_port[i]->SetValue(comPort);
        
        // Set baud rate selection
        wxString baudStr = wxString::Format("%d", baudRate);
        int baudIndex = m_ch_baud_rate[i]->FindString(baudStr);
        if (baudIndex != wxNOT_FOUND) {
            m_ch_baud_rate[i]->SetSelection(baudIndex);
        } else {
            m_ch_baud_rate[i]->SetSelection(1); // default to 19200
        }
        
        // Set software flow control (hardware flow control is disabled for Wang terminals)
        m_cb_sw_flow_control[i]->SetValue(swFlowControl);
        
        // Enable/disable controls based on number of terminals and COM port checkbox
        bool terminalEnabled = (i < m_cfg.getNumTerminals());
        m_cb_com_port[i]->Enable(terminalEnabled);
        m_tc_com_port[i]->Enable(terminalEnabled && useCom);
        m_ch_baud_rate[i]->Enable(terminalEnabled && useCom);
        m_cb_sw_flow_control[i]->Enable(terminalEnabled && useCom);
    }
}


void
TermMuxCfgDlg::OnNumTerminals(wxCommandEvent& WXUNUSED(event))
{
    switch (m_rb_num_terminals->GetSelection()) {
        case 0: m_cfg.setNumTerminals(1); break;
        case 1: m_cfg.setNumTerminals(2); break;
        case 2: m_cfg.setNumTerminals(3); break;
        case 3: m_cfg.setNumTerminals(4); break;
        default: assert(false); break;
    }
    
    // Update the control enable states based on new number of terminals
    updateDlg();
    
    m_btn_revert->Enable(m_cfg != m_old_cfg);
}


// handle COM port checkbox and text changes
void
TermMuxCfgDlg::OnComPortChange(wxCommandEvent &event)
{
    int termIndex = -1;
    int controlId = event.GetId();
    
    // Determine which terminal this event is for
    if (controlId >= ID_CB_COM_PORT_1 && controlId <= ID_CB_COM_PORT_4) {
        termIndex = controlId - ID_CB_COM_PORT_1;
        
        // Update the COM port setting
        bool useCom = m_cb_com_port[termIndex]->GetValue();
        std::string comPort = useCom ? m_tc_com_port[termIndex]->GetValue().ToStdString() : "";
        m_cfg.setTerminalComPort(termIndex, comPort);
        
        // Enable/disable related controls
        m_tc_com_port[termIndex]->Enable(useCom);
        m_ch_baud_rate[termIndex]->Enable(useCom);
        m_cb_sw_flow_control[termIndex]->Enable(useCom);
        
    } else if (controlId >= ID_TC_COM_PORT_1 && controlId <= ID_TC_COM_PORT_4) {
        termIndex = controlId - ID_TC_COM_PORT_1;
        
        // Update the COM port name if checkbox is checked
        if (m_cb_com_port[termIndex]->GetValue()) {
            std::string comPort = m_tc_com_port[termIndex]->GetValue().ToStdString();
            m_cfg.setTerminalComPort(termIndex, comPort);
        }
    }
    
    m_btn_revert->Enable(m_cfg != m_old_cfg);
}

// handle baud rate changes
void
TermMuxCfgDlg::OnBaudRateChange(wxCommandEvent &event)
{
    int termIndex = event.GetId() - ID_CH_BAUD_RATE_1;
    if (termIndex >= 0 && termIndex < 4) {
        wxString baudStr = m_ch_baud_rate[termIndex]->GetStringSelection();
        long baudRate;
        if (baudStr.ToLong(&baudRate)) {
            m_cfg.setTerminalBaudRate(termIndex, (int)baudRate);
        }
    }
    
    m_btn_revert->Enable(m_cfg != m_old_cfg);
}

// handle software flow control changes
void
TermMuxCfgDlg::OnSwFlowControlChange(wxCommandEvent &event)
{
    int termIndex = event.GetId() - ID_CB_SW_FLOW_CONTROL_1;
    if (termIndex >= 0 && termIndex < 4) {
        bool swFlowControl = m_cb_sw_flow_control[termIndex]->GetValue();
        m_cfg.setTerminalSwFlowControl(termIndex, swFlowControl);
    }
    
    m_btn_revert->Enable(m_cfg != m_old_cfg);
}


// used for all dialog button presses
void
TermMuxCfgDlg::OnButton(wxCommandEvent &event)
{
    switch (event.GetId()) {

        case ID_BTN_HELP:
            {
                TermMuxCfgHelpDlg *helpDlg = new TermMuxCfgHelpDlg(this);
                helpDlg->ShowModal();
            }
            break;

        case ID_BTN_REVERT:
            m_cfg = m_old_cfg;          // revert state
            updateDlg();                // select current options
            m_btn_revert->Disable();
            break;

        case wxID_OK:
        {
            // make sure all io addresses have been selected
            // see if config mgr is happy with things
            if (m_cfg.configOk(true)) {
                saveDefaults();         // save location & size of dlg
                EndModal(0);
            }
            break;
        }

        case wxID_CANCEL:
            saveDefaults();             // save location & size of dlg
            EndModal(1);
            break;

        default:
            event.Skip();
            break;
    }
}


// save dialog options to the config file
void
TermMuxCfgDlg::saveDefaults()
{
    // TODO: we should specify the MXD-nn-CRT-m prefix subgroup
    //       otherwise all such dialogs will share one position state
    const std::string subgroup("ui/termmuxcfgdlg");

    // save position and size
    host::configWriteWinGeom(this, subgroup);
}


void
TermMuxCfgDlg::getDefaults()
{
    // see if we've established a favored location and size
    // TODO: we should specify the MXD-nn-CRT-m prefix subgroup
    //       otherwise all such dialogs will share one position state
    const std::string subgroup("ui/termmuxcfgdlg");
    host::configReadWinGeom(this, subgroup);
}

// vim: ts=8:et:sw=4:smarttab
