#include "ElegooNetworkPlugin.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <mutex>
#include "libslic3r/Utils.hpp"

#if !defined(_MSC_VER) && !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace Slic3r {

static constexpr const char ELEGOO_SOURCE_LIBRARY[] = "ElegooSource";

// ============================================================================
// Singleton
// ============================================================================

ElegooNetworkPlugin* ElegooNetworkPlugin::s_instance = nullptr;

ElegooNetworkPlugin& ElegooNetworkPlugin::instance()
{
    static std::once_flag flag;
    std::call_once(flag, [] { s_instance = new ElegooNetworkPlugin(); });
    return *s_instance;
}

void ElegooNetworkPlugin::shutdown()
{
    if (s_instance) {
        delete s_instance;
        s_instance = nullptr;
    }
}

ElegooNetworkPlugin::ElegooNetworkPlugin() = default;

ElegooNetworkPlugin::~ElegooNetworkPlugin()
{
    destroy_agent();
    unload();
}

// ============================================================================
// Module lifecycle
// ============================================================================

int ElegooNetworkPlugin::initialize(bool using_backup, const std::string& version)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    clear_load_error();

    if (version.empty()) {
        BOOST_LOG_TRIVIAL(error) << "ElegooNetworkPlugin: version is required but not provided";
        set_load_error("Elegoo network library version not specified",
                       "A version must be specified to load the Elegoo network library", "");
        return -1;
    }

    std::string                 data_dir_str = data_dir();
    boost::filesystem::path     data_dir_path(data_dir_str);
    auto                        plugin_folder = data_dir_path / "plugins";
    if (using_backup)
        plugin_folder = plugin_folder / "backup";

    // Build platform-specific library path
    std::string library;
#if defined(_MSC_VER) || defined(_WIN32)
    library = plugin_folder.string() + "\\" + ELEGOO_SOURCE_LIBRARY + "_" + version + ".dll";
#elif defined(__WXMAC__)
    library = plugin_folder.string() + "/lib" + ELEGOO_SOURCE_LIBRARY + "_" + version + ".dylib";
#else
    library = plugin_folder.string() + "/lib" + ELEGOO_SOURCE_LIBRARY + "_" + version + ".so";
#endif

    BOOST_LOG_TRIVIAL(info) << "ElegooNetworkPlugin: loading library from: " << library;

#if defined(_MSC_VER) || defined(_WIN32)
    const int required_wchars = ::MultiByteToWideChar(CP_UTF8, 0, library.c_str(), -1, nullptr, 0);
    std::wstring lib_wstr(required_wchars, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, library.c_str(), -1, lib_wstr.data(),
                          static_cast<int>(lib_wstr.size()));
    m_module = LoadLibrary(lib_wstr.c_str());
    if (!m_module) {
        DWORD err = GetLastError();
        set_load_error("Failed to load Elegoo network library",
                       "LoadLibrary failed with error code " + std::to_string(err), library);
        return -1;
    }
#else
    m_module = dlopen(library.c_str(), RTLD_LAZY);
    if (!m_module) {
        const char* err = dlerror();
        set_load_error("Failed to load Elegoo network library",
                       err ? std::string(err) : "Unknown dlopen error", library);
        return -1;
    }
#endif

    resolve_functions();
    BOOST_LOG_TRIVIAL(info) << "ElegooNetworkPlugin: library loaded successfully";
    return 0;
}

void ElegooNetworkPlugin::unload()
{
    if (!m_module)
        return;

    // Clear all function pointers before unloading
    m_fn_create_agent        = nullptr;
    m_fn_destroy_agent       = nullptr;
    m_fn_get_version         = nullptr;
    m_fn_connect_printer     = nullptr;
    m_fn_disconnect_printer  = nullptr;
    m_fn_send_command        = nullptr;
    m_fn_start_print         = nullptr;
    m_fn_set_on_message      = nullptr;
    m_fn_set_on_local_connect = nullptr;
    m_fn_set_on_ssdp_msg     = nullptr;
    m_fn_start_discovery     = nullptr;

#if defined(_MSC_VER) || defined(_WIN32)
    FreeLibrary(m_module);
#else
    dlclose(m_module);
#endif
    m_module = nullptr;
    BOOST_LOG_TRIVIAL(info) << "ElegooNetworkPlugin: library unloaded";
}

// ============================================================================
// Function resolution
// ============================================================================

void* ElegooNetworkPlugin::resolve_symbol(const char* name)
{
#if defined(_MSC_VER) || defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(m_module, name));
#else
    return dlsym(m_module, name);
#endif
}

void ElegooNetworkPlugin::resolve_functions()
{
    m_fn_create_agent         = reinterpret_cast<elegoo_func_create_agent>(resolve_symbol("elegoo_create_agent"));
    m_fn_destroy_agent        = reinterpret_cast<elegoo_func_destroy_agent>(resolve_symbol("elegoo_destroy_agent"));
    m_fn_get_version          = reinterpret_cast<elegoo_func_get_version>(resolve_symbol("elegoo_get_version"));
    m_fn_connect_printer      = reinterpret_cast<elegoo_func_connect_printer>(resolve_symbol("elegoo_connect_printer"));
    m_fn_disconnect_printer   = reinterpret_cast<elegoo_func_disconnect_printer>(resolve_symbol("elegoo_disconnect_printer"));
    m_fn_send_command         = reinterpret_cast<elegoo_func_send_command>(resolve_symbol("elegoo_send_command"));
    m_fn_start_print          = reinterpret_cast<elegoo_func_start_print>(resolve_symbol("elegoo_start_print"));
    m_fn_set_on_message       = reinterpret_cast<elegoo_func_set_on_message_fn>(resolve_symbol("elegoo_set_on_message_fn"));
    m_fn_set_on_local_connect = reinterpret_cast<elegoo_func_set_on_local_connect_fn>(resolve_symbol("elegoo_set_on_local_connect_fn"));
    m_fn_set_on_ssdp_msg      = reinterpret_cast<elegoo_func_set_on_ssdp_msg_fn>(resolve_symbol("elegoo_set_on_ssdp_msg_fn"));
    m_fn_start_discovery      = reinterpret_cast<elegoo_func_start_discovery>(resolve_symbol("elegoo_start_discovery"));

    if (m_fn_get_version) {
        const char* ver = m_fn_get_version();
        BOOST_LOG_TRIVIAL(info) << "ElegooNetworkPlugin: library version = " << (ver ? ver : "(null)");
    } else {
        BOOST_LOG_TRIVIAL(warning) << "ElegooNetworkPlugin: elegoo_get_version symbol not found – "
                                      "library may be incompatible or partially loaded";
    }
}

// ============================================================================
// Agent management
// ============================================================================

void ElegooNetworkPlugin::create_agent(const std::string& log_dir)
{
    if (!m_module || !m_fn_create_agent) {
        BOOST_LOG_TRIVIAL(error) << "ElegooNetworkPlugin: cannot create agent – library not loaded";
        return;
    }
    if (m_agent) {
        BOOST_LOG_TRIVIAL(warning) << "ElegooNetworkPlugin: agent already exists";
        return;
    }
    m_agent = m_fn_create_agent(log_dir.c_str());
    if (!m_agent) {
        BOOST_LOG_TRIVIAL(error) << "ElegooNetworkPlugin: elegoo_create_agent returned null";
    } else {
        BOOST_LOG_TRIVIAL(info) << "ElegooNetworkPlugin: agent created";
    }
}

void ElegooNetworkPlugin::destroy_agent()
{
    if (!m_agent)
        return;
    if (m_fn_destroy_agent)
        m_fn_destroy_agent(m_agent);
    m_agent = nullptr;
    BOOST_LOG_TRIVIAL(info) << "ElegooNetworkPlugin: agent destroyed";
}

// ============================================================================
// Error helpers
// ============================================================================

void ElegooNetworkPlugin::clear_load_error()
{
    m_load_error = {};
}

void ElegooNetworkPlugin::set_load_error(const std::string& msg,
                                         const std::string& details,
                                         const std::string& path)
{
    m_load_error.has_error        = true;
    m_load_error.message          = msg;
    m_load_error.technical_details = details;
    m_load_error.attempted_path   = path;
    BOOST_LOG_TRIVIAL(error) << "ElegooNetworkPlugin: " << msg << " | " << details;
}

} // namespace Slic3r
