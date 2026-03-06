#include "ElegooPrinterDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "slic3r/Utils/ElegooNetworkPlugin.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {

// ============================================================================
// Construction
// ============================================================================

ElegooPrinterDialog::ElegooPrinterDialog(wxWindow* parent, wxWindowID id,
                                         const wxString& title,
                                         const wxPoint& pos, const wxSize& size,
                                         long style)
    : DPIDialog(parent, id,
                title.IsEmpty() ? _L("Manage Elegoo Printers") : title,
                pos, size, style)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Top separator line
    auto* line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line_top->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(line_top, 0, wxEXPAND);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    // Description
    auto* desc = new wxStaticText(this, wxID_ANY,
        _L("Add and manage Elegoo printers on your local network. "
           "Enter the printer IP address and access code printed on the device label."));
    desc->SetFont(Label::Body_13);
    desc->Wrap(FromDIP(460));
    main_sizer->Add(desc, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    build_add_printer_section(main_sizer);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    build_printer_list_section(main_sizer);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    // Status label
    m_status_label = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_status_label->SetFont(Label::Body_12);
    main_sizer->Add(m_status_label, 0, wxLEFT | wxRIGHT, FromDIP(25));
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    // Bottom separator
    auto* line_bottom = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line_bottom->SetBackgroundColour(wxColour(166, 169, 170));
    main_sizer->Add(line_bottom, 0, wxEXPAND);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    build_button_row(main_sizer);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(15));

    SetSizer(main_sizer);
    Layout();
    Fit();
    SetMinSize(wxSize(FromDIP(500), FromDIP(400)));
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    update_button_states();
}

ElegooPrinterDialog::~ElegooPrinterDialog() = default;

// ============================================================================
// UI builders
// ============================================================================

void ElegooPrinterDialog::build_add_printer_section(wxBoxSizer* parent_sizer)
{
    auto* section_label = new wxStaticText(this, wxID_ANY, _L("Add Printer"));
    section_label->SetFont(Label::Head_14);
    parent_sizer->Add(section_label, 0, wxLEFT | wxRIGHT, FromDIP(25));
    parent_sizer->Add(0, 0, 0, wxTOP, FromDIP(8));

    // Grid: label | widget  across two columns
    auto* grid = new wxFlexGridSizer(4, 2, FromDIP(6), FromDIP(12));
    grid->AddGrowableCol(1, 1);

    auto make_label = [&](const wxString& text) {
        auto* lbl = new wxStaticText(this, wxID_ANY, text);
        lbl->SetFont(Label::Body_13);
        return lbl;
    };

    // Name
    grid->Add(make_label(_L("Name:")), 0, wxALIGN_CENTER_VERTICAL);
    m_input_name = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString,
                                 wxDefaultPosition, wxSize(FromDIP(200), FromDIP(24)),
                                 wxTE_PROCESS_ENTER);
    m_input_name->GetTextCtrl()->SetHint(_L("e.g. My Elegoo Saturn 4 Ultra"));
    grid->Add(m_input_name, 1, wxEXPAND);

    // IP
    grid->Add(make_label(_L("IP Address:")), 0, wxALIGN_CENTER_VERTICAL);
    m_input_ip = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString,
                               wxDefaultPosition, wxSize(FromDIP(200), FromDIP(24)),
                               wxTE_PROCESS_ENTER);
    m_input_ip->GetTextCtrl()->SetHint(_L("e.g. 192.168.1.100"));
    grid->Add(m_input_ip, 1, wxEXPAND);

    // Port
    grid->Add(make_label(_L("Port:")), 0, wxALIGN_CENTER_VERTICAL);
    m_input_port = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString,
                                 wxDefaultPosition, wxSize(FromDIP(80), FromDIP(24)),
                                 wxTE_PROCESS_ENTER);
    m_input_port->GetTextCtrl()->SetHint("3000");
    grid->Add(m_input_port, 0);

    // Access code
    grid->Add(make_label(_L("Access Code:")), 0, wxALIGN_CENTER_VERTICAL);
    m_input_access_code = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString,
                                        wxDefaultPosition, wxSize(FromDIP(200), FromDIP(24)),
                                        wxTE_PROCESS_ENTER | wxTE_PASSWORD);
    m_input_access_code->GetTextCtrl()->SetHint(_L("Found on printer label"));
    grid->Add(m_input_access_code, 1, wxEXPAND);

    parent_sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(25));
    parent_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    m_btn_add = new Button(this, _L("Add Printer"));
    m_btn_add->SetMinSize(wxSize(FromDIP(120), FromDIP(30)));
    m_btn_add->Bind(wxEVT_BUTTON, &ElegooPrinterDialog::on_add_printer, this);
    parent_sizer->Add(m_btn_add, 0, wxLEFT, FromDIP(25));
}

void ElegooPrinterDialog::build_printer_list_section(wxBoxSizer* parent_sizer)
{
    auto* section_label = new wxStaticText(this, wxID_ANY, _L("Configured Printers"));
    section_label->SetFont(Label::Head_14);
    parent_sizer->Add(section_label, 0, wxLEFT | wxRIGHT, FromDIP(25));
    parent_sizer->Add(0, 0, 0, wxTOP, FromDIP(8));

    m_printer_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition,
                                    wxSize(FromDIP(460), FromDIP(150)),
                                    wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);

    m_printer_list->InsertColumn(0, _L("Name"),     wxLIST_FORMAT_LEFT, FromDIP(140));
    m_printer_list->InsertColumn(1, _L("IP"),       wxLIST_FORMAT_LEFT, FromDIP(130));
    m_printer_list->InsertColumn(2, _L("Port"),     wxLIST_FORMAT_CENTER, FromDIP(60));
    m_printer_list->InsertColumn(3, _L("Status"),   wxLIST_FORMAT_CENTER, FromDIP(100));

    m_printer_list->Bind(wxEVT_LIST_ITEM_SELECTED,
                         &ElegooPrinterDialog::on_list_selection_changed, this);
    m_printer_list->Bind(wxEVT_LIST_ITEM_DESELECTED,
                         &ElegooPrinterDialog::on_list_selection_changed, this);

    parent_sizer->Add(m_printer_list, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(25));
    parent_sizer->Add(0, 0, 0, wxTOP, FromDIP(8));

    // Action buttons for the list
    auto* action_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_btn_remove = new Button(this, _L("Remove"));
    m_btn_remove->SetMinSize(wxSize(FromDIP(100), FromDIP(30)));
    m_btn_remove->Bind(wxEVT_BUTTON, &ElegooPrinterDialog::on_remove_printer, this);
    action_sizer->Add(m_btn_remove, 0, wxRIGHT, FromDIP(10));

    m_btn_connect = new Button(this, _L("Connect"));
    m_btn_connect->SetMinSize(wxSize(FromDIP(100), FromDIP(30)));
    m_btn_connect->Bind(wxEVT_BUTTON, &ElegooPrinterDialog::on_connect, this);
    action_sizer->Add(m_btn_connect, 0, wxRIGHT, FromDIP(10));

    m_btn_disconnect = new Button(this, _L("Disconnect"));
    m_btn_disconnect->SetMinSize(wxSize(FromDIP(110), FromDIP(30)));
    m_btn_disconnect->Bind(wxEVT_BUTTON, &ElegooPrinterDialog::on_disconnect, this);
    action_sizer->Add(m_btn_disconnect);

    parent_sizer->Add(action_sizer, 0, wxLEFT, FromDIP(25));
}

void ElegooPrinterDialog::build_button_row(wxBoxSizer* parent_sizer)
{
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->AddStretchSpacer();
    m_btn_close = new Button(this, _L("Close"));
    m_btn_close->SetMinSize(wxSize(FromDIP(100), FromDIP(30)));
    m_btn_close->Bind(wxEVT_BUTTON, &ElegooPrinterDialog::on_close, this);
    row->Add(m_btn_close, 0, wxRIGHT, FromDIP(25));
    parent_sizer->Add(row, 0, wxEXPAND);
}

// ============================================================================
// Event handlers
// ============================================================================

void ElegooPrinterDialog::on_add_printer(wxCommandEvent& /*evt*/)
{
    const wxString name   = m_input_name->GetTextCtrl()->GetValue().Trim();
    const wxString ip     = m_input_ip->GetTextCtrl()->GetValue().Trim();
    const wxString port_s = m_input_port->GetTextCtrl()->GetValue().Trim();
    const wxString code   = m_input_access_code->GetTextCtrl()->GetValue();

    if (ip.IsEmpty()) {
        m_status_label->SetLabel(_L("Error: IP address is required."));
        m_status_label->SetForegroundColour(*wxRED);
        return;
    }

    PrinterEntry entry;
    entry.ip          = ip.ToStdString();
    entry.name        = name.IsEmpty() ? ("Elegoo @ " + entry.ip) : name.ToStdString();
    entry.port        = port_s.IsEmpty() ? 3000 : wxAtoi(port_s);
    entry.access_code = code.ToStdString();
    entry.connected   = false;

    m_entries.push_back(entry);
    refresh_list();
    update_button_states();

    // Clear inputs
    m_input_name->GetTextCtrl()->Clear();
    m_input_ip->GetTextCtrl()->Clear();
    m_input_port->GetTextCtrl()->Clear();
    m_input_access_code->GetTextCtrl()->Clear();

    m_status_label->SetLabel(wxString::Format(_L("Added printer: %s"),
                                              wxString::FromUTF8(entry.name)));
    m_status_label->SetForegroundColour(wxColour(0, 150, 0));
}

void ElegooPrinterDialog::on_remove_printer(wxCommandEvent& /*evt*/)
{
    const int idx = get_selected_index();
    if (idx < 0 || idx >= static_cast<int>(m_entries.size()))
        return;

    const std::string name = m_entries[idx].name;
    if (m_entries[idx].connected) {
        auto& plugin = ElegooNetworkPlugin::instance();
        auto  fn     = plugin.get_disconnect_printer();
        auto  agent  = plugin.get_agent();
        if (fn && agent)
            fn(agent);
    }
    m_entries.erase(m_entries.begin() + idx);
    refresh_list();
    update_button_states();
    m_status_label->SetLabel(wxString::Format(_L("Removed printer: %s"),
                                              wxString::FromUTF8(name)));
    m_status_label->SetForegroundColour(wxColour(100, 100, 100));
}

void ElegooPrinterDialog::on_connect(wxCommandEvent& /*evt*/)
{
    const int idx = get_selected_index();
    if (idx < 0 || idx >= static_cast<int>(m_entries.size()))
        return;

    PrinterEntry& entry = m_entries[idx];

    auto& plugin = ElegooNetworkPlugin::instance();
    if (!plugin.is_loaded()) {
        m_status_label->SetLabel(
            _L("Elegoo network library not loaded. Please install the Elegoo plugin."));
        m_status_label->SetForegroundColour(*wxRED);
        return;
    }

    if (!plugin.has_agent())
        plugin.create_agent(std::string{});

    auto fn    = plugin.get_connect_printer();
    auto agent = plugin.get_agent();
    if (fn && agent) {
        // Use the IP address as device ID for Elegoo printers (no separate dev_id scheme)
        const int result = fn(agent, entry.ip.c_str(), entry.ip.c_str(),
                              entry.port, entry.access_code.c_str());
        if (result == 0) {
            entry.connected = true;
            m_status_label->SetLabel(
                wxString::Format(_L("Connected to %s (%s:%d)"),
                                 wxString::FromUTF8(entry.name),
                                 wxString::FromUTF8(entry.ip),
                                 entry.port));
            m_status_label->SetForegroundColour(wxColour(0, 150, 0));
        } else {
            m_status_label->SetLabel(
                wxString::Format(_L("Failed to connect to %s (error %d)"),
                                 wxString::FromUTF8(entry.name), result));
            m_status_label->SetForegroundColour(*wxRED);
        }
    } else {
        m_status_label->SetLabel(_L("Connection function unavailable in the Elegoo library."));
        m_status_label->SetForegroundColour(*wxRED);
    }

    refresh_list();
    update_button_states();
}

void ElegooPrinterDialog::on_disconnect(wxCommandEvent& /*evt*/)
{
    const int idx = get_selected_index();
    if (idx < 0 || idx >= static_cast<int>(m_entries.size()))
        return;

    PrinterEntry& entry = m_entries[idx];

    auto& plugin = ElegooNetworkPlugin::instance();
    auto  fn     = plugin.get_disconnect_printer();
    auto  agent  = plugin.get_agent();
    if (fn && agent)
        fn(agent);

    entry.connected = false;
    m_status_label->SetLabel(
        wxString::Format(_L("Disconnected from %s"), wxString::FromUTF8(entry.name)));
    m_status_label->SetForegroundColour(wxColour(100, 100, 100));

    refresh_list();
    update_button_states();
}

void ElegooPrinterDialog::on_list_selection_changed(wxListEvent& /*evt*/)
{
    update_button_states();
}

void ElegooPrinterDialog::on_close(wxCommandEvent& /*evt*/)
{
    EndModal(wxID_OK);
}

// ============================================================================
// Helpers
// ============================================================================

void ElegooPrinterDialog::refresh_list()
{
    m_printer_list->DeleteAllItems();
    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        const PrinterEntry& e = m_entries[i];
        long item = m_printer_list->InsertItem(i, wxString::FromUTF8(e.name));
        m_printer_list->SetItem(item, 1, wxString::FromUTF8(e.ip));
        m_printer_list->SetItem(item, 2, wxString::Format("%d", e.port));
        m_printer_list->SetItem(item, 3, e.connected ? _L("Connected") : _L("Disconnected"));
    }
}

void ElegooPrinterDialog::update_button_states()
{
    const int  idx      = get_selected_index();
    const bool has_sel  = (idx >= 0 && idx < static_cast<int>(m_entries.size()));
    const bool connected = has_sel && m_entries[idx].connected;

    m_btn_remove->Enable(has_sel);
    m_btn_connect->Enable(has_sel && !connected);
    m_btn_disconnect->Enable(has_sel && connected);
}

int ElegooPrinterDialog::get_selected_index() const
{
    return static_cast<int>(m_printer_list->GetNextItem(-1, wxLIST_NEXT_ALL,
                                                        wxLIST_STATE_SELECTED));
}

// ============================================================================
// DPI handling
// ============================================================================

void ElegooPrinterDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
