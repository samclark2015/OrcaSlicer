#ifndef __ELEGOO_PRINTER_AGENT_HPP__
#define __ELEGOO_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"
#include <memory>
#include <mutex>
#include <string>

namespace Slic3r {

/**
 * ElegooPrinterAgent - IPrinterAgent implementation backed by the proprietary
 * Elegoo networking DLL loaded through ElegooNetworkPlugin.
 *
 * All printer operations are delegated to the DLL via typed function pointers.
 * When the DLL is not loaded, operations return BAMBU_NETWORK_SUCCESS (no-op)
 * to allow graceful degradation.
 */
class ElegooPrinterAgent : public IPrinterAgent
{
public:
    explicit ElegooPrinterAgent(std::string log_dir);
    ~ElegooPrinterAgent() override;

    // ========================================================================
    // IPrinterAgent Interface
    // ========================================================================

    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username,
                        std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos,
                                int flag) override;

    // Certificates
    int  check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link,
             std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine selection
    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    // Print jobs
    int start_print(PrintParams params, OnUpdateStatusFn update_fn,
                    WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn,
                                      WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn,
                                   WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn,
                          WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn,
                           WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Agent info
    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

private:
    std::string                         m_log_dir;
    std::string                         m_selected_machine;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;

    // Registered callbacks (forwarded to DLL when available)
    OnMsgArrivedFn        m_on_ssdp_msg_fn;
    OnPrinterConnectedFn  m_on_printer_connected_fn;
    GetSubscribeFailureFn m_on_subscribe_failure_fn;
    OnMessageFn           m_on_message_fn;
    OnMessageFn           m_on_user_message_fn;
    OnLocalConnectedFn    m_on_local_connect_fn;
    OnMessageFn           m_on_local_message_fn;
    QueueOnMainFn         m_queue_on_main_fn;
    OnServerErrFn         m_on_server_err_fn;

    mutable std::mutex m_mutex;
};

} // namespace Slic3r

#endif // __ELEGOO_PRINTER_AGENT_HPP__
