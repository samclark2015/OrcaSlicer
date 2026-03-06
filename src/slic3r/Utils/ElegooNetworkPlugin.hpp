#ifndef __ELEGOO_NETWORK_PLUGIN_HPP__
#define __ELEGOO_NETWORK_PLUGIN_HPP__

#include "bambu_networking.hpp"
#include <string>
#include <memory>
#include <mutex>

#if defined(_MSC_VER) || defined(_WIN32)
#include <Windows.h>
#endif

namespace Slic3r {

// ============================================================================
// Elegoo DLL function pointer types
// ============================================================================

/// Create an Elegoo agent instance. Returns opaque handle or nullptr on failure.
typedef void* (*elegoo_func_create_agent)(const char* log_dir);

/// Destroy a previously created agent.
typedef void (*elegoo_func_destroy_agent)(void* agent);

/// Return the version string of the loaded Elegoo networking library.
typedef const char* (*elegoo_func_get_version)(void);

/// Establish a connection to an Elegoo printer at the given IP / port.
typedef int (*elegoo_func_connect_printer)(void* agent, const char* dev_id, const char* dev_ip,
                                           int port, const char* access_code);

/// Tear down the active printer connection.
typedef int (*elegoo_func_disconnect_printer)(void* agent);

/// Send a JSON command to the connected printer.
typedef int (*elegoo_func_send_command)(void* agent, const char* dev_id, const char* json_str);

/// Upload a G-code file and optionally start printing.
typedef int (*elegoo_func_start_print)(void* agent, PrintParams params,
                                       OnUpdateStatusFn update_fn,
                                       WasCancelledFn   cancel_fn);

/// Register a callback for status/message events from the printer.
typedef int (*elegoo_func_set_on_message_fn)(void* agent, OnMessageFn fn);

/// Register a callback for local connection state changes.
typedef int (*elegoo_func_set_on_local_connect_fn)(void* agent, OnLocalConnectedFn fn);

/// Register a callback for SSDP discovery messages.
typedef int (*elegoo_func_set_on_ssdp_msg_fn)(void* agent, OnMsgArrivedFn fn);

/// Start or stop LAN discovery.
typedef bool (*elegoo_func_start_discovery)(void* agent, bool start);

// ============================================================================
// Load error info
// ============================================================================

struct ElegooLibraryLoadError
{
    bool        has_error{false};
    std::string message;
    std::string technical_details;
    std::string attempted_path;
};

// ============================================================================
// ElegooNetworkPlugin
//
// Singleton that loads Elegoo's proprietary networking DLL at runtime and
// exposes typed function pointers to ElegooPrinterAgent.
//
// Library naming convention (mirrors the BBLNetworkPlugin convention):
//   Windows : ElegooSource_<version>.dll
//   macOS   : libElegooSource_<version>.dylib
//   Linux   : libElegooSource_<version>.so
//
// The library is searched in:
//   1. {data_dir}/plugins/
//   2. {data_dir}/plugins/backup/  (when using_backup == true)
// ============================================================================

class ElegooNetworkPlugin
{
public:
    static ElegooNetworkPlugin& instance();
    static void                 shutdown();

    ~ElegooNetworkPlugin();

    // ========================================================================
    // Lifecycle
    // ========================================================================

    /**
     * Load the Elegoo networking library for the specified version.
     * @return 0 on success, non-zero on failure.
     */
    int  initialize(bool using_backup, const std::string& version);
    void unload();

    bool is_loaded() const { return m_module != nullptr; }

    // ========================================================================
    // Agent management
    // ========================================================================

    bool has_agent() const { return m_agent != nullptr; }
    void create_agent(const std::string& log_dir);
    void destroy_agent();
    void* get_agent() const { return m_agent; }

    // ========================================================================
    // Error reporting
    // ========================================================================

    const ElegooLibraryLoadError& get_load_error() const { return m_load_error; }
    bool has_load_error() const { return m_load_error.has_error; }

    // ========================================================================
    // Function pointer accessors
    // ========================================================================

    elegoo_func_connect_printer       get_connect_printer()        const { return m_fn_connect_printer; }
    elegoo_func_disconnect_printer    get_disconnect_printer()     const { return m_fn_disconnect_printer; }
    elegoo_func_send_command          get_send_command()           const { return m_fn_send_command; }
    elegoo_func_start_print           get_start_print()            const { return m_fn_start_print; }
    elegoo_func_set_on_message_fn     get_set_on_message_fn()      const { return m_fn_set_on_message; }
    elegoo_func_set_on_local_connect_fn get_set_on_local_connect_fn() const { return m_fn_set_on_local_connect; }
    elegoo_func_set_on_ssdp_msg_fn    get_set_on_ssdp_msg_fn()    const { return m_fn_set_on_ssdp_msg; }
    elegoo_func_start_discovery       get_start_discovery()        const { return m_fn_start_discovery; }

private:
    ElegooNetworkPlugin();
    ElegooNetworkPlugin(const ElegooNetworkPlugin&)            = delete;
    ElegooNetworkPlugin& operator=(const ElegooNetworkPlugin&) = delete;

    void resolve_functions();
    void* resolve_symbol(const char* name);
    void  clear_load_error();
    void  set_load_error(const std::string& msg, const std::string& details, const std::string& path);

    static ElegooNetworkPlugin* s_instance;

#if defined(_MSC_VER) || defined(_WIN32)
    HMODULE m_module{nullptr};
#else
    void* m_module{nullptr};
#endif

    void* m_agent{nullptr};

    // Resolved function pointers
    elegoo_func_create_agent          m_fn_create_agent{nullptr};
    elegoo_func_destroy_agent         m_fn_destroy_agent{nullptr};
    elegoo_func_get_version           m_fn_get_version{nullptr};
    elegoo_func_connect_printer       m_fn_connect_printer{nullptr};
    elegoo_func_disconnect_printer    m_fn_disconnect_printer{nullptr};
    elegoo_func_send_command          m_fn_send_command{nullptr};
    elegoo_func_start_print           m_fn_start_print{nullptr};
    elegoo_func_set_on_message_fn     m_fn_set_on_message{nullptr};
    elegoo_func_set_on_local_connect_fn m_fn_set_on_local_connect{nullptr};
    elegoo_func_set_on_ssdp_msg_fn    m_fn_set_on_ssdp_msg{nullptr};
    elegoo_func_start_discovery       m_fn_start_discovery{nullptr};

    ElegooLibraryLoadError m_load_error;
    mutable std::mutex     m_mutex;
};

} // namespace Slic3r

#endif // __ELEGOO_NETWORK_PLUGIN_HPP__
