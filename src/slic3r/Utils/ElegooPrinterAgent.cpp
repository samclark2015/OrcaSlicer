#include "ElegooPrinterAgent.hpp"
#include "ElegooNetworkPlugin.hpp"
#include "NetworkAgentFactory.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r {

static const std::string ElegooPrinterAgent_VERSION = "0.0.1";

ElegooPrinterAgent::ElegooPrinterAgent(std::string log_dir)
    : m_log_dir(std::move(log_dir))
{
    auto& plugin = ElegooNetworkPlugin::instance();
    if (!plugin.is_loaded()) {
        BOOST_LOG_TRIVIAL(warning) << "ElegooPrinterAgent: DLL not loaded – running in stub mode";
    } else if (!plugin.has_agent()) {
        plugin.create_agent(m_log_dir);
    }
}

ElegooPrinterAgent::~ElegooPrinterAgent()
{
    disconnect_printer();
}

void ElegooPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cloud_agent = cloud;
}

// ============================================================================
// Communication
// ============================================================================

int ElegooPrinterAgent::send_message(std::string dev_id, std::string json_str, int /*qos*/, int /*flag*/)
{
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  fn     = plugin.get_send_command();
    auto  agent  = plugin.get_agent();
    if (fn && agent)
        return fn(agent, dev_id.c_str(), json_str.c_str());
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::connect_printer(std::string dev_id, std::string dev_ip,
                                        std::string username, std::string password,
                                        bool /*use_ssl*/)
{
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  fn     = plugin.get_connect_printer();
    auto  agent  = plugin.get_agent();
    if (fn && agent) {
        // Elegoo printers use access_code (password) on port 3000 by default
        constexpr int default_port = 3000;
        return fn(agent, dev_id.c_str(), dev_ip.c_str(), default_port, password.c_str());
    }
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::disconnect_printer()
{
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  fn     = plugin.get_disconnect_printer();
    auto  agent  = plugin.get_agent();
    if (fn && agent)
        return fn(agent);
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str,
                                                int /*qos*/, int /*flag*/)
{
    // Same channel for Elegoo – forward to send_command
    return send_message(dev_id, json_str, 0, 0);
}

// ============================================================================
// Certificates (not used by Elegoo – stub implementations)
// ============================================================================

int ElegooPrinterAgent::check_cert() { return BAMBU_NETWORK_SUCCESS; }

void ElegooPrinterAgent::install_device_cert(std::string /*dev_id*/, bool /*lan_only*/) {}

// ============================================================================
// Discovery
// ============================================================================

bool ElegooPrinterAgent::start_discovery(bool start, bool /*sending*/)
{
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  fn     = plugin.get_start_discovery();
    auto  agent  = plugin.get_agent();
    if (fn && agent)
        return fn(agent, start);
    return true;
}

// ============================================================================
// Binding (not applicable for Elegoo – stub implementations)
// ============================================================================

int ElegooPrinterAgent::ping_bind(std::string /*ping_code*/) { return BAMBU_NETWORK_SUCCESS; }

int ElegooPrinterAgent::bind_detect(std::string /*dev_ip*/, std::string /*sec_link*/,
                                    detectResult& /*detect*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::bind(std::string /*dev_ip*/, std::string /*dev_id*/,
                             std::string /*sec_link*/, std::string /*timezone*/,
                             bool /*improved*/, OnUpdateStatusFn /*update_fn*/)
{
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::unbind(std::string /*dev_id*/) { return BAMBU_NETWORK_SUCCESS; }

int ElegooPrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket)
        *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine selection
// ============================================================================

std::string ElegooPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected_machine;
}

int ElegooPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Agent information
// ============================================================================

AgentInfo ElegooPrinterAgent::get_agent_info_static()
{
    return AgentInfo{ELEGOO_PRINTER_AGENT_ID, "Elegoo", ElegooPrinterAgent_VERSION,
                     "Elegoo printer agent"};
}

// ============================================================================
// Print job operations
// ============================================================================

int ElegooPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn,
                                    WasCancelledFn cancel_fn, OnWaitFn /*wait_fn*/)
{
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  fn     = plugin.get_start_print();
    auto  agent  = plugin.get_agent();
    if (fn && agent)
        return fn(agent, params, update_fn, cancel_fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::start_local_print_with_record(PrintParams params,
                                                      OnUpdateStatusFn update_fn,
                                                      WasCancelledFn   cancel_fn,
                                                      OnWaitFn /*wait_fn*/)
{
    // Elegoo does not distinguish cloud vs. local-with-record; delegate to start_print
    return start_print(params, update_fn, cancel_fn, nullptr);
}

int ElegooPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn,
                                                   WasCancelledFn cancel_fn,
                                                   OnWaitFn /*wait_fn*/)
{
    return start_print(params, update_fn, cancel_fn, nullptr);
}

int ElegooPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn,
                                          WasCancelledFn cancel_fn)
{
    return start_print(params, update_fn, cancel_fn, nullptr);
}

int ElegooPrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn,
                                           WasCancelledFn cancel_fn)
{
    return start_print(params, update_fn, cancel_fn, nullptr);
}

// ============================================================================
// Callback registration
// ============================================================================

int ElegooPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_ssdp_msg_fn = fn;
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  reg_fn = plugin.get_set_on_ssdp_msg_fn();
    auto  agent  = plugin.get_agent();
    if (reg_fn && agent)
        return reg_fn(agent, fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_message_fn = fn;
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  reg_fn = plugin.get_set_on_message_fn();
    auto  agent  = plugin.get_agent();
    if (reg_fn && agent)
        return reg_fn(agent, fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_local_connect_fn = fn;
    auto& plugin = ElegooNetworkPlugin::instance();
    auto  reg_fn = plugin.get_set_on_local_connect_fn();
    auto  agent  = plugin.get_agent();
    if (reg_fn && agent)
        return reg_fn(agent, fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int ElegooPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
