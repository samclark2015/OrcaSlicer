#ifndef __CC2_PRINTER_AGENT_HPP__
#define __CC2_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "ICloudServiceAgent.hpp"

#include <string>
#include <memory>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>
#include <chrono>

#include <nlohmann/json.hpp>
#include <mqtt/async_client.h>

namespace Slic3r {

/**
 * CC2PrinterAgent - Elegoo Centauri Carbon 2 (CC2) protocol implementation
 *
 * Protocol characteristics:
 * - Discovery: UDP broadcast on port 52700
 * - Communication: MQTT 3.1.1 over WebSocket (port 9001)
 * - File upload: Chunked HTTP PUT to port 80
 * - Video stream: MJPEG on port 8080
 * - Authentication: Username "elegoo" + password/access code
 * - Heartbeat: Required every 10 seconds (65s timeout)
 * - Status: Delta updates with deep merge
 *
 * Based on CC2_SPEC.md and cc2_manager.py reference implementation.
 */
class CC2PrinterAgent : public IPrinterAgent
{
public:
    explicit CC2PrinterAgent(std::string log_dir);
    ~CC2PrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // ========================================================================
    // IPrinterAgent Interface Implementation
    // ========================================================================

    // Cloud Agent Dependency
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    // Communication
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override;
    int disconnect_printer() override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;

    // Certificates
    int check_cert() override;
    void install_device_cert(std::string dev_id, bool lan_only) override;

    // Discovery
    bool start_discovery(bool start, bool sending) override;

    // Binding
    int ping_bind(std::string ping_code) override;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override;
    int bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override;
    int unbind(std::string dev_id) override;
    int request_bind_ticket(std::string* ticket) override;
    int set_server_callback(OnServerErrFn fn) override;

    // Machine Selection
    std::string get_user_selected_machine() override;
    int set_user_selected_machine(std::string dev_id) override;

    // Print Job Operations
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    // Callbacks
    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Filament sync (pull mode - fetch on demand)
    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::pull; }
    bool fetch_filament_info(std::string dev_id) override;

    // MQTT message handler (public for callback access)
    void on_mqtt_message(mqtt::const_message_ptr msg);

protected:
    // ========================================================================
    // CC2 Protocol Structures
    // ========================================================================

    struct CC2DeviceInfo {
        std::string dev_id;         // Serial number
        std::string dev_ip;
        std::string dev_name;       // Host name
        std::string machine_model;
        std::string access_code;    // Password (default: "123456")
        bool        token_required; // token_status from discovery
        bool        lan_only;       // lan_status from discovery
        std::string firmware_version;
    };

    // CC2 Method codes (from spec)
    static constexpr int DISCOVERY = 7000;
    static constexpr int GET_ATTRIBUTES = 1001;
    static constexpr int GET_STATUS = 1002;
    static constexpr int START_PRINT = 1020;
    static constexpr int PAUSE_PRINT = 1021;
    static constexpr int STOP_PRINT = 1022;
    static constexpr int RESUME_PRINT = 1023;
    static constexpr int HOME_AXES = 1026;
    static constexpr int MOVE_AXES = 1027;
    static constexpr int SET_TEMPERATURE = 1028;
    static constexpr int SET_LIGHT = 1029;
    static constexpr int SET_FAN_SPEED = 1030;
    static constexpr int SET_PRINT_SPEED = 1031;
    static constexpr int PRINT_TASK_LIST = 1036;
    static constexpr int DELETE_FILE = 1047;
    static constexpr int GET_FILE_LIST = 1044;
    static constexpr int GET_FILE_THUMBNAIL = 1045;
    static constexpr int GET_FILE_DETAIL = 1046;
    static constexpr int VIDEO_STREAM = 1042;
    static constexpr int GET_CANVAS_STATUS = 2005;
    static constexpr int SET_AUTO_REFILL = 2004;
    static constexpr int ON_PRINTER_STATUS = 6000;
    static constexpr int ON_PRINTER_ATTRIBUTES = 6008;

    // ========================================================================
    // MQTT over WebSocket Communication
    // ========================================================================

    bool mqtt_connect(const std::string& host, int port, const std::string& client_id,
                      const std::string& username, const std::string& password);
    bool mqtt_disconnect();
    bool mqtt_subscribe(const std::string& topic);
    bool mqtt_publish(const std::string& topic, const std::string& payload);

    // ========================================================================
    // CC2 Protocol Implementation
    // ========================================================================

    bool cc2_register();
    void cc2_handle_registration_response(const nlohmann::json& payload);
    void cc2_handle_command_response(const nlohmann::json& payload);
    bool cc2_send_command(int method, const nlohmann::json& params, nlohmann::json& response, int timeout_ms = 10000);
    void cc2_start_heartbeat();
    void cc2_stop_heartbeat();
    void cc2_heartbeat_loop();
    void cc2_handle_status_update(const nlohmann::json& message);
    void cc2_deep_merge(nlohmann::json& base, const nlohmann::json& update);

    // File operations
    bool cc2_upload_file(const std::string& local_path, const std::string& remote_filename,
                        OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
    std::string calculate_md5(const std::string& filepath);

    // Discovery
    bool udp_discover_printers(std::vector<CC2DeviceInfo>& printers, int timeout_ms = 10000);

    // Helper functions
    std::string generate_client_id();
    std::string generate_request_id();
    std::string cc2_generate_upload_filename(const PrintParams& params);
    void update_machine_status(const nlohmann::json& status);
    void initialize_machine_object(const std::string& dev_id);
    void announce_printhost_device();
    void dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg);
    void dispatch_message(const std::string& dev_id, const std::string& payload);
    std::string cc2_map_filament_type(const std::string& filament_type);

private:
    std::string m_log_dir;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::string m_selected_machine;

    // Connection state
    CC2DeviceInfo m_device_info;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_registered{false};
    std::string m_client_id;
    std::string m_request_id;

    // MQTT state
    std::shared_ptr<mqtt::async_client> m_mqtt_client;  // MQTT over WebSocket client
    std::shared_ptr<mqtt::callback> m_mqtt_callback;
    std::atomic<bool> m_mqtt_running{false};

    // Heartbeat
    std::atomic<bool> m_heartbeat_running{false};
    std::thread m_heartbeat_thread;
    std::chrono::steady_clock::time_point m_last_heartbeat;

    // Status cache
    mutable std::recursive_mutex m_status_mutex;
    nlohmann::json m_cached_status;
    int m_last_status_id{-1};
    int m_non_continuous_count{0};

    // Command tracking
    std::atomic<int> m_next_command_id{1};
    std::mutex m_command_mutex;
    struct PendingCommand {
        bool completed = false;
        nlohmann::json response;
        std::chrono::steady_clock::time_point sent_time;
    };
    std::map<int, PendingCommand> m_pending_commands;

    // Callbacks
    OnMsgArrivedFn on_ssdp_msg_fn;
    OnPrinterConnectedFn on_printer_connected_fn;
    GetSubscribeFailureFn on_subscribe_failure_fn;
    OnMessageFn on_message_fn;
    OnMessageFn on_user_message_fn;
    OnLocalConnectedFn on_local_connect_fn;
    OnMessageFn on_local_message_fn;
    QueueOnMainFn queue_on_main_fn;
    OnServerErrFn on_server_err_fn;

    // SSDP announcement tracking
    std::string m_ssdp_announced_id;
    std::string m_ssdp_announced_ip;

    // Discovery thread
    std::atomic<bool> m_discovery_running{false};
    std::thread m_discovery_thread;
    void discovery_loop();

    mutable std::recursive_mutex m_state_mutex;
};

} // namespace Slic3r

#endif // __CC2_PRINTER_AGENT_HPP__
