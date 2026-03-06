#ifndef slic3r_GUI_ElegooPrinterDialog_hpp_
#define slic3r_GUI_ElegooPrinterDialog_hpp_

#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/Label.hpp"

#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

/**
 * ElegooPrinterDialog - GUI dialog for adding and managing Elegoo printers.
 *
 * Provides:
 *   - Manual printer registration by IP address and access code
 *   - A list of configured Elegoo printers with live connection status
 *   - Connect / Disconnect controls
 */
class ElegooPrinterDialog : public DPIDialog
{
public:
    ElegooPrinterDialog(wxWindow* parent,
                        wxWindowID id    = wxID_ANY,
                        const wxString& title = wxEmptyString,
                        const wxPoint&  pos   = wxDefaultPosition,
                        const wxSize&   size  = wxDefaultSize,
                        long style = wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    ~ElegooPrinterDialog() override;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // UI construction helpers
    void build_add_printer_section(wxBoxSizer* parent_sizer);
    void build_printer_list_section(wxBoxSizer* parent_sizer);
    void build_button_row(wxBoxSizer* parent_sizer);

    // Event handlers
    void on_add_printer(wxCommandEvent& evt);
    void on_remove_printer(wxCommandEvent& evt);
    void on_connect(wxCommandEvent& evt);
    void on_disconnect(wxCommandEvent& evt);
    void on_list_selection_changed(wxListEvent& evt);
    void on_close(wxCommandEvent& evt);

    // Helpers
    void refresh_list();
    void update_button_states();
    int  get_selected_index() const;

    // ---- Widgets ----
    TextInput*  m_input_ip{nullptr};
    TextInput*  m_input_port{nullptr};
    TextInput*  m_input_access_code{nullptr};
    TextInput*  m_input_name{nullptr};
    Button*     m_btn_add{nullptr};
    Button*     m_btn_remove{nullptr};
    Button*     m_btn_connect{nullptr};
    Button*     m_btn_disconnect{nullptr};
    Button*     m_btn_close{nullptr};
    wxListCtrl* m_printer_list{nullptr};
    wxStaticText* m_status_label{nullptr};

    // Per-entry data stored alongside the list
    struct PrinterEntry
    {
        std::string name;
        std::string ip;
        int         port{3000};
        std::string access_code;
        bool        connected{false};
    };
    std::vector<PrinterEntry> m_entries;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ElegooPrinterDialog_hpp_
