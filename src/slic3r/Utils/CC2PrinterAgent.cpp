// CC2PrinterAgent.cpp - Implementation of Elegoo Centauri Carbon 2 printer agent
// Copyright (c) 2025 OrcaSlicer
// Author: OrcaSlicer Team

#include "CC2PrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/connect.hpp>
#include <mqtt/async_client.h>
#include <mqtt/connect_options.h>
#include <mqtt/message.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <openssl/md5.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace Slic3r {

const std::string CC2PrinterAgent_VERSION = "1.0.0";
const int MQTT_PORT = 1883;  // MQTT over WebSocket port
const int HTTP_UPLOAD_PORT = 80;
const int HTTP_VIDEO_PORT = 8080;
const int HEARTBEAT_INTERVAL_MS = 10000;  // 10 seconds
const int COMMAND_TIMEOUT_MS = 10000;      // 10 seconds
const int REGISTRATION_TIMEOUT_MS = 3000;  // 3 seconds
const size_t UPLOAD_CHUNK_SIZE = 1024 * 1024;  // 1 MB

// ============================================================================
// MQTT Callback Handler
// ============================================================================

class CC2Callback : public mqtt::callback
{
private:
    CC2PrinterAgent* m_agent;

public:
    explicit CC2Callback(CC2PrinterAgent* agent) : m_agent(agent) {}

    void connected(const std::string& cause) override {
        BOOST_LOG_TRIVIAL(info) << "CC2: MQTT connected: " << cause;
    }

    void connection_lost(const std::string& cause) override {
        BOOST_LOG_TRIVIAL(warning) << "CC2: MQTT connection lost: " << cause;
    }

    void message_arrived(mqtt::const_message_ptr msg) override {
        m_agent->on_mqtt_message(msg);
    }

    void delivery_complete(mqtt::delivery_token_ptr token) override {
        // Message delivered successfully
    }
};

// ============================================================================
// Static Info
// ============================================================================

AgentInfo CC2PrinterAgent::get_agent_info_static()
{
    AgentInfo info;
    info.id = "cc2";
    info.name = "Elegoo CC2";
    info.version = CC2PrinterAgent_VERSION;
    info.description = "Elegoo Centauri Carbon 2 printer agent (MQTT over WebSocket)";
    return info;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

CC2PrinterAgent::CC2PrinterAgent(std::string log_dir)
    : m_log_dir(std::move(log_dir))
{
    BOOST_LOG_TRIVIAL(info) << "CC2PrinterAgent: Initialized with log_dir=" << m_log_dir;
}

CC2PrinterAgent::~CC2PrinterAgent()
{
    BOOST_LOG_TRIVIAL(info) << "CC2PrinterAgent: Shutting down";

    // Stop discovery thread first
    if (m_discovery_running) {
        BOOST_LOG_TRIVIAL(info) << "CC2: Stopping discovery thread from destructor...";
        m_discovery_running = false;
        if (m_discovery_thread.joinable()) {
            m_discovery_thread.join();
            BOOST_LOG_TRIVIAL(info) << "CC2: Discovery thread stopped";
        }
    }

    disconnect_printer();
}

// ============================================================================
// Cloud Agent Dependency
// ============================================================================

void CC2PrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(m_status_mutex);
    m_cloud_agent = cloud;
}

// ============================================================================
// Helper Functions
// ============================================================================

std::string CC2PrinterAgent::generate_client_id()
{
    // Format: "0cli" + 5_hex_timestamp + random_hex, truncated to 10 chars
    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::ostringstream oss;
    oss << std::hex << millis;
    std::string timestamp_hex = oss.str();

    // Get last 5 hex chars of timestamp
    if (timestamp_hex.length() > 5) {
        timestamp_hex = timestamp_hex.substr(timestamp_hex.length() - 5);
    }

    // Generate random hex (0-fff)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 4095);

    std::ostringstream rand_oss;
    rand_oss << std::hex << dis(gen);
    std::string random_hex = rand_oss.str();

    std::string client_id = "0cli" + timestamp_hex + random_hex;
    if (client_id.length() > 10) {
        client_id = client_id.substr(0, 10);
    }

    return client_id;
}

std::string CC2PrinterAgent::generate_request_id()
{
    // Format: UUID4-like (16 hex chars) + timestamp_hex
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);

    std::ostringstream uuid_oss;
    for (int i = 0; i < 16; ++i) {
        uuid_oss << std::hex << dis(gen);
    }

    auto now = std::chrono::system_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::ostringstream timestamp_oss;
    timestamp_oss << std::hex << millis;

    return uuid_oss.str() + timestamp_oss.str();
}

std::string CC2PrinterAgent::calculate_md5(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Failed to open file for MD5: " << filepath;
        return "";
    }

    MD5_CTX md5_ctx;
    MD5_Init(&md5_ctx);

    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        MD5_Update(&md5_ctx, buffer, file.gcount());
    }

    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &md5_ctx);

    std::ostringstream oss;
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    }

    return oss.str();
}

std::string CC2PrinterAgent::cc2_map_filament_type(const std::string& filament_type)
{
    // Map Canvas filament types to OrcaFilamentLibrary generic IDs
    // Based on resources/profiles/OrcaFilamentLibrary/filament/

    std::string upper = filament_type;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // PLA variants
    if (upper == "PLA")           return "OGFL99";
    if (upper == "PLA-CF")        return "OGFL98";
    if (upper == "PLA SILK" || upper == "PLA-SILK") return "OGFL96";
    if (upper == "PLA HIGH SPEED" || upper == "PLA-HS" || upper == "PLA HS") return "OGFL95";

    // ABS/ASA variants
    if (upper == "ABS")           return "OGFB99";
    if (upper == "ASA")           return "OGFB98";

    // PETG/PET variants
    if (upper == "PETG" || upper == "PET") return "OGFG99";
    if (upper == "PCTG")          return "OGFG97";

    // PA/Nylon variants
    if (upper == "PA" || upper == "NYLON") return "OGFN99";
    if (upper == "PA-CF")         return "OGFN98";
    if (upper == "PPA" || upper == "PPA-CF") return "OGFN97";
    if (upper == "PPA-GF")        return "OGFN96";

    // PC variants
    if (upper == "PC")            return "OGFC99";

    // PP/PE variants
    if (upper == "PE")            return "OGFP99";
    if (upper == "PP")            return "OGFP97";

    // Support materials
    if (upper == "PVA")           return "OGFS99";
    if (upper == "HIPS")          return "OGFS98";
    if (upper == "BVOH")          return "OGFS97";

    // TPU variants
    if (upper == "TPU")           return "OGFU99";

    // Other materials
    if (upper == "EVA")           return "OGFR99";
    if (upper == "PHA")           return "OGFR98";
    if (upper == "COPE")          return "OGFLC99";
    if (upper == "SBS")           return "OGFLSBS99";

    // Unknown material - return empty string
    return "";
}

// ============================================================================
// MQTT Communication
// ============================================================================

void CC2PrinterAgent::on_mqtt_message(mqtt::const_message_ptr msg)
{
    try {
        std::string topic = msg->get_topic();
        std::string payload = msg->to_string();

        BOOST_LOG_TRIVIAL(error) << "CC2: Received message on " << topic << ": " << payload;

        nlohmann::json json_payload = nlohmann::json::parse(payload);

        // Handle registration response
        if (topic.find("register_response") != std::string::npos) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Handling registration response";
            cc2_handle_registration_response(json_payload);
        }
        // Handle status updates
        else if (topic.find("api_status") != std::string::npos) {
            cc2_handle_status_update(json_payload);
        }
        // Handle command responses
        else if (topic.find("api_response") != std::string::npos) {
            cc2_handle_command_response(json_payload);
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Error processing MQTT message: " << e.what();
    }
}

bool CC2PrinterAgent::mqtt_connect(const std::string& host, int port, const std::string& client_id,
                                   const std::string& username, const std::string& password)
{
    BOOST_LOG_TRIVIAL(error) << "CC2: mqtt_connect ENTRY - host=" << host << " port=" << port << " client_id=" << client_id << " username=" << username << " password=" << password;

    try {
        // Paho MQTT will handle connection testing

        // Create TCP MQTT URI: tcp://<host>:<port>
        std::string server_uri = "tcp://" + host + ":" + std::to_string(port);

        BOOST_LOG_TRIVIAL(error) << "CC2: Creating MQTT client with TCP URI: " << server_uri;

        // Create MQTT client with no persistence
        m_mqtt_client = std::make_shared<mqtt::async_client>(server_uri, client_id, nullptr);

        BOOST_LOG_TRIVIAL(error) << "CC2: MQTT client created, setting up callback";

        // Create callback
        m_mqtt_callback = std::make_shared<CC2Callback>(this);
        m_mqtt_client->set_callback(*m_mqtt_callback);

        BOOST_LOG_TRIVIAL(error) << "CC2: Building connection options for TCP";

        // Build connect options for TCP connection
        auto connOpts = mqtt::connect_options_builder()
            .mqtt_version(MQTTVERSION_3_1_1)
            .user_name(username)
            .password(password)
            .keep_alive_interval(std::chrono::seconds(60))
            .clean_session(true)
            .finalize();

        BOOST_LOG_TRIVIAL(error) << "CC2: Attempting to connect...";

        // Connect
        auto tok = m_mqtt_client->connect(connOpts);

        BOOST_LOG_TRIVIAL(error) << "CC2: Waiting for connection result...";
        try {
            tok->wait();
            BOOST_LOG_TRIVIAL(error) << "CC2: Wait completed successfully";
        } catch (const mqtt::exception& wait_ex) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Exception during tok->wait(): " << wait_ex.what()
                                     << " | reason_code=" << wait_ex.get_reason_code()
                                     << " | error_str=" << wait_ex.get_error_str()
                                     << " | return_code=" << wait_ex.get_return_code();
            throw; // Re-throw to be caught by outer handler
        }

        BOOST_LOG_TRIVIAL(error) << "CC2: Checking token completion status...";
        if (!tok->is_complete()) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Token is not complete";
            return false;
        }

        int return_code = tok->get_return_code();
        BOOST_LOG_TRIVIAL(error) << "CC2: Token complete, return code: " << return_code;

        // Check for connection success (return code 0 = success)
        if (return_code != 0) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Connection failed with return code: " << return_code;
            return false;
        }

        // Start consuming messages (background thread for async message processing)
        // This is equivalent to Python's mqtt_client.loop_start()
        BOOST_LOG_TRIVIAL(error) << "CC2: Starting message processing loop...";
        m_mqtt_client->start_consuming();
        m_mqtt_running = true;

        m_connected = true;
        BOOST_LOG_TRIVIAL(error) << "CC2: MQTT connection successful";
        return true;

    } catch (const mqtt::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: MQTT exception: " << e.what()
                                 << " (reason_code=" << e.get_reason_code()
                                 << ", error_str=" << e.get_error_str() << ")";
        return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Standard exception during MQTT connect: " << e.what();
        return false;
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Unknown exception during MQTT connect";
        return false;
    }
}

bool CC2PrinterAgent::mqtt_disconnect()
{
    if (m_mqtt_client && m_mqtt_client->is_connected()) {
        try {
            // Stop message processing loop first (equivalent to Python's loop_stop())
            if (m_mqtt_running) {
                m_mqtt_client->stop_consuming();
                m_mqtt_running = false;
            }

            auto tok = m_mqtt_client->disconnect();
            tok->wait();
        } catch (const mqtt::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CC2: MQTT disconnect error: " << e.what();
        }
    }

    m_connected = false;
    return true;
}

bool CC2PrinterAgent::mqtt_subscribe(const std::string& topic)
{
    if (!m_mqtt_client || !m_mqtt_client->is_connected()) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Cannot subscribe, not connected";
        return false;
    }

    try {
        auto tok = m_mqtt_client->subscribe(topic, 1);
        tok->wait();
        BOOST_LOG_TRIVIAL(debug) << "CC2: Subscribed to " << topic;
        return true;
    } catch (const mqtt::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Subscribe failed: " << e.what();
        return false;
    }
}

bool CC2PrinterAgent::mqtt_publish(const std::string& topic, const std::string& payload)
{
    if (!m_mqtt_client || !m_mqtt_client->is_connected()) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Cannot publish, not connected";
        return false;
    }

    try {
        auto msg = mqtt::make_message(topic, payload);
        msg->set_qos(1);
        auto tok = m_mqtt_client->publish(msg);
        tok->wait();
        BOOST_LOG_TRIVIAL(debug) << "CC2: Published to " << topic << ": " << payload;
        return true;
    } catch (const mqtt::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Publish failed: " << e.what();
        return false;
    }
}

// ============================================================================
// CC2 Protocol Implementation
// ============================================================================

bool CC2PrinterAgent::cc2_register()
{
    BOOST_LOG_TRIVIAL(error) << "CC2: Registering client";

    // Subscribe to registration response
    std::string response_topic = "elegoo/" + m_device_info.dev_id + "/" + m_request_id + "/register_response";
    BOOST_LOG_TRIVIAL(error) << "CC2: Subscribing to registration response topic: " << response_topic;
    if (!mqtt_subscribe(response_topic)) {
        return false;
    }

    // Send registration request
    nlohmann::json register_msg = {
        {"client_id", m_client_id},
        {"request_id", m_request_id}
    };

    std::string register_topic = "elegoo/" + m_device_info.dev_id + "/api_register";
    BOOST_LOG_TRIVIAL(error) << "CC2: Publishing registration request to: " << register_topic;
    BOOST_LOG_TRIVIAL(error) << "CC2: Registration message: " << register_msg.dump();
    if (!mqtt_publish(register_topic, register_msg.dump())) {
        return false;
    }
    BOOST_LOG_TRIVIAL(error) << "CC2: Registration request sent successfully";

    // Wait for registration (timeout: 3 seconds)
    auto start = std::chrono::steady_clock::now();
    while (!m_registered) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > REGISTRATION_TIMEOUT_MS) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Registration timeout";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return true;
}

void CC2PrinterAgent::cc2_handle_registration_response(const nlohmann::json& payload)
{
    std::string error = payload.value("error", "fail");

    if (error == "ok") {
        BOOST_LOG_TRIVIAL(info) << "CC2: Registration successful";
        m_registered = true;
    } else {
        BOOST_LOG_TRIVIAL(error) << "CC2: Registration failed: " << error;
        m_registered = false;
    }
}

void CC2PrinterAgent::cc2_handle_command_response(const nlohmann::json& payload)
{
    std::lock_guard<std::mutex> lock(m_command_mutex);

    // Handle PONG (heartbeat response)
    std::string msg_type = payload.value("type", "");
    if (msg_type == "PONG") {
        BOOST_LOG_TRIVIAL(debug) << "CC2: Received PONG";
        return;
    }

    // Handle command response
    int cmd_id = payload.value("id", -1);
    if (cmd_id < 0 || m_pending_commands.find(cmd_id) == m_pending_commands.end()) {
        return;
    }

    auto& pending = m_pending_commands[cmd_id];
    pending.response = payload.value("result", nlohmann::json::object());
    pending.completed = true;
}

// Helper function to generate a user-friendly filename for uploads
std::string CC2PrinterAgent::cc2_generate_upload_filename(const PrintParams& params)
{
    std::string filename;
    
    // Try to use preset_name first (format: "ProjectName_plate_N")
    if (!params.preset_name.empty()) {
        filename = params.preset_name;
    }
    // Fall back to project_name with plate index
    else if (!params.project_name.empty()) {
        filename = params.project_name;
        if (params.plate_index > 0) {
            filename += "_plate_" + std::to_string(params.plate_index);
        }
    }
    // Last resort: use "print"
    else {
        filename = "print";
    }
    
    // Sanitize filename: replace invalid characters with underscores
    const char* invalid_chars = "<>:\"/\\|?*";
    for (char& c : filename) {
        if (strchr(invalid_chars, c) != nullptr || c < 32) {
            c = '_';
        }
    }
    
    // Replace multiple underscores with single underscore
    size_t pos = 0;
    while ((pos = filename.find("__", pos)) != std::string::npos) {
        filename.replace(pos, 2, "_");
    }
    
    // Ensure .gcode extension
    if (!boost::algorithm::ends_with(filename, ".gcode")) {
        filename += ".gcode";
    }
    
    BOOST_LOG_TRIVIAL(info) << "CC2: Generated upload filename: " << filename;
    return filename;
}

bool CC2PrinterAgent::cc2_send_command(int method, const nlohmann::json& params,
                                       nlohmann::json& response, int timeout_ms)
{
    int cmd_id = m_next_command_id++;

    // Build command
    nlohmann::json command = {
        {"id", cmd_id},
        {"method", method}
    };
    if (!params.is_null() && !params.empty()) {
        command["params"] = params;
    }

    // Track pending command
    {
        std::lock_guard<std::mutex> lock(m_command_mutex);
        PendingCommand pending;
        pending.completed = false;
        pending.sent_time = std::chrono::steady_clock::now();
        m_pending_commands[cmd_id] = pending;
    }

    // Send command
    std::string topic = "elegoo/" + m_device_info.dev_id + "/" + m_client_id + "/api_request";
    if (!mqtt_publish(topic, command.dump())) {
        std::lock_guard<std::mutex> lock(m_command_mutex);
        m_pending_commands.erase(cmd_id);
        return false;
    }

    // Wait for response
    auto start = std::chrono::steady_clock::now();
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_command_mutex);
            if (m_pending_commands.find(cmd_id) != m_pending_commands.end() &&
                m_pending_commands[cmd_id].completed) {
                response = m_pending_commands[cmd_id].response;
                m_pending_commands.erase(cmd_id);

                // Check for error
                int error_code = response.value("error_code", 0);
                if (error_code != 0) {
                    BOOST_LOG_TRIVIAL(error) << "CC2: Command failed with error " << error_code;
                    return false;
                }
                return true;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed > timeout_ms) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Command timeout";
            std::lock_guard<std::mutex> lock(m_command_mutex);
            m_pending_commands.erase(cmd_id);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void CC2PrinterAgent::cc2_start_heartbeat()
{
    m_heartbeat_running = true;
    m_heartbeat_thread = std::thread(&CC2PrinterAgent::cc2_heartbeat_loop, this);
    BOOST_LOG_TRIVIAL(info) << "CC2: Heartbeat started";
}

void CC2PrinterAgent::cc2_stop_heartbeat()
{
    m_heartbeat_running = false;
    if (m_heartbeat_thread.joinable()) {
        m_heartbeat_thread.join();
    }
    BOOST_LOG_TRIVIAL(info) << "CC2: Heartbeat stopped";
}

void CC2PrinterAgent::cc2_heartbeat_loop()
{
    while (m_heartbeat_running && m_connected) {
        try {
            // Send PING
            nlohmann::json ping = {{"type", "PING"}};
            std::string topic = "elegoo/" + m_device_info.dev_id + "/" + m_client_id + "/api_request";
            mqtt_publish(topic, ping.dump());

            BOOST_LOG_TRIVIAL(debug) << "CC2: Sent PING";

            std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Heartbeat error: " << e.what();
        }
    }
}

void CC2PrinterAgent::cc2_handle_status_update(const nlohmann::json& message)
{
    std::lock_guard<std::recursive_mutex> lock(m_status_mutex);

    int method = message.value("method", 0);
    if (method != ON_PRINTER_STATUS) {
        return;
    }

    nlohmann::json result = message.value("result", nlohmann::json::object());
    int msg_id = message.value("id", -1);

    // Check message continuity
    if (m_last_status_id >= 0 && msg_id >= 0) {
        if (msg_id != m_last_status_id + 1) {
            m_non_continuous_count++;
            BOOST_LOG_TRIVIAL(warning) << "CC2: Non-continuous status message";

            if (m_non_continuous_count >= 5) {
                BOOST_LOG_TRIVIAL(warning) << "CC2: Too many gaps, requesting full status";
                // Request full status
                nlohmann::json response;
                cc2_send_command(GET_STATUS, nlohmann::json::object(), response);
                m_cached_status = response;
                m_non_continuous_count = 0;
                return;
            }
        } else {
            m_non_continuous_count = 0;
        }
    }

    m_last_status_id = msg_id;

    // Deep merge delta update
    cc2_deep_merge(m_cached_status, result);

    // Wrap status in OrcaSlicer expected format
    nlohmann::json wrapped_status;
    wrapped_status["print"]["command"] = "push_status";
    wrapped_status["print"]["msg"] = 0;  // Full message (not diff)
    wrapped_status["print"]["sdcard"] = true;

    // Copy all fields from cached status to print section
    for (auto it = m_cached_status.begin(); it != m_cached_status.end(); ++it) {
        wrapped_status["print"][it.key()] = it.value();
    }

    // Dispatch to callback
    dispatch_message(m_device_info.dev_id, wrapped_status.dump());

    // Update MachineObject's last_push_time to keep is_info_ready() returning true
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (dev_manager) {
        MachineObject* obj = dev_manager->get_my_machine(m_device_info.dev_id);
        if (obj) {
            obj->last_push_time = std::chrono::system_clock::now();
        }
    }
}

void CC2PrinterAgent::cc2_deep_merge(nlohmann::json& base, const nlohmann::json& update)
{
    for (auto it = update.begin(); it != update.end(); ++it) {
        if (base.contains(it.key()) && base[it.key()].is_object() && it.value().is_object()) {
            cc2_deep_merge(base[it.key()], it.value());
        } else {
            base[it.key()] = it.value();
        }
    }
}

// ============================================================================
// File Upload
// ============================================================================

bool CC2PrinterAgent::cc2_upload_file(const std::string& local_path, const std::string& remote_filename,
                                     OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Uploading file: " << local_path << " -> " << remote_filename;

    // Get file size
    std::ifstream file(local_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Cannot open file: " << local_path;
        return false;
    }
    size_t file_size = file.tellg();
    file.close();

    // Calculate MD5
    std::string file_md5 = calculate_md5(local_path);
    if (file_md5.empty()) {
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "CC2: File size: " << file_size << " bytes, MD5: " << file_md5;

    // Upload in chunks
    std::ifstream upload_file(local_path, std::ios::binary);
    if (!upload_file.is_open()) {
        return false;
    }

    size_t offset = 0;
    std::vector<char> buffer(UPLOAD_CHUNK_SIZE);

    try {
        net::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);

        // Resolve and connect
        auto const results = resolver.resolve(m_device_info.dev_ip, std::to_string(HTTP_UPLOAD_PORT));
        stream.connect(results);

        while (offset < file_size) {
            // Check if cancelled
            if (cancel_fn && cancel_fn()) {
                BOOST_LOG_TRIVIAL(warning) << "CC2: Upload cancelled";
                return false;
            }

            // Read chunk
            upload_file.read(buffer.data(), UPLOAD_CHUNK_SIZE);
            size_t chunk_size = upload_file.gcount();

            // Build HTTP PUT request
            http::request<http::vector_body<char>> req{http::verb::put, "/upload", 11};
            req.set(http::field::host, m_device_info.dev_ip);
            req.set(http::field::content_type, "application/octet-stream");
            req.set("Content-Range", "bytes " + std::to_string(offset) + "-" +
                    std::to_string(offset + chunk_size - 1) + "/" + std::to_string(file_size));
            req.set("X-File-Name", remote_filename);
            req.set("X-File-MD5", file_md5);
            req.set("X-Token", m_device_info.access_code);
            req.body().assign(buffer.begin(), buffer.begin() + chunk_size);
            req.prepare_payload();

            // Send request
            http::write(stream, req);

            // Receive response
            beast::flat_buffer resp_buffer;
            http::response<http::string_body> res;
            http::read(stream, resp_buffer, res);

            if (res.result() == http::status::unauthorized) {
                BOOST_LOG_TRIVIAL(error) << "CC2: Upload failed - invalid access code";
                return false;
            }

            if (res.result() != http::status::ok) {
                BOOST_LOG_TRIVIAL(error) << "CC2: Upload failed - HTTP " << res.result_int();
                return false;
            }

            offset += chunk_size;

            // Update progress
            if (update_fn) {
                int progress = (offset * 100) / file_size;
                update_fn(progress, 0, "Uploading file");
            }

            BOOST_LOG_TRIVIAL(debug) << "CC2: Uploaded " << offset << "/" << file_size << " bytes";
        }

        // Graceful close
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        BOOST_LOG_TRIVIAL(info) << "CC2: File upload complete";
        return true;

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Upload error: " << e.what();
        return false;
    }
}

// ============================================================================
// Communication
// ============================================================================

int CC2PrinterAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    return send_message_to_printer(std::move(dev_id), std::move(json_str), qos, flag);
}

int CC2PrinterAgent::connect_printer(std::string dev_id, std::string dev_ip,
                                     std::string username, std::string password, bool use_ssl)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Connecting to printer " << dev_ip;

    // Check if dev_id is an IP address (contains dots) - if so, we need to discover the real serial number
    bool need_discovery = (dev_id.find('.') != std::string::npos);
    std::string actual_dev_id = dev_id;

    if (need_discovery) {
        BOOST_LOG_TRIVIAL(info) << "CC2: dev_id appears to be IP address, performing UDP discovery to find serial number";

        std::vector<CC2DeviceInfo> discovered_printers;
        if (udp_discover_printers(discovered_printers, 5000)) {
            // Find the printer matching our target IP
            bool found = false;
            for (const auto& printer : discovered_printers) {
                if (printer.dev_ip == dev_ip || printer.dev_ip == dev_id) {
                    actual_dev_id = printer.dev_id;
                    m_device_info.dev_name = printer.dev_name;
                    m_device_info.machine_model = printer.machine_model;
                    m_device_info.token_required = printer.token_required;
                    m_device_info.lan_only = printer.lan_only;
                    found = true;
                    BOOST_LOG_TRIVIAL(info) << "CC2: Found printer via discovery - SN: " << actual_dev_id
                        << ", Model: " << printer.machine_model;
                    break;
                }
            }

            if (!found) {
                BOOST_LOG_TRIVIAL(error) << "CC2: Printer at " << dev_ip << " not found in discovery results";
                return BAMBU_NETWORK_ERR_CONNECT_FAILED;
            }
        } else {
            BOOST_LOG_TRIVIAL(warning) << "CC2: UDP discovery failed or no printers found, attempting connection anyway with IP as dev_id";
            // Continue with IP as dev_id - it might work
        }
    }

    // Store device info
    m_device_info.dev_id = actual_dev_id;
    m_device_info.dev_ip = dev_ip;
    m_device_info.access_code = password.empty() ? "123456" : password;

    // Generate client and request IDs
    m_client_id = generate_client_id();
    m_request_id = generate_request_id();

    BOOST_LOG_TRIVIAL(error) << "CC2: Client ID: " << m_client_id << ", Request ID: " << m_request_id
        << ", Dev ID: " << m_device_info.dev_id;

    // Connect MQTT (MQTT over WebSocket on port 9001)
    if (!mqtt_connect(dev_ip, MQTT_PORT, m_client_id, "elegoo", m_device_info.access_code)) {
        BOOST_LOG_TRIVIAL(error) << "CC2: MQTT connection failed";
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }

    // Register client
    if (!cc2_register()) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Registration failed";
        mqtt_disconnect();
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }

    // Subscribe to topics
    mqtt_subscribe("elegoo/" + actual_dev_id + "/api_status");
    mqtt_subscribe("elegoo/" + actual_dev_id + "/" + m_client_id + "/api_response");

    // Start heartbeat
    cc2_start_heartbeat();

    // Get initial status to populate m_cached_status
    nlohmann::json initial_status;
    if (cc2_send_command(GET_STATUS, nlohmann::json::object(), initial_status, 5000)) {
        BOOST_LOG_TRIVIAL(info) << "CC2: Retrieved initial status";
        {
            std::lock_guard<std::recursive_mutex> lock(m_status_mutex);
            m_cached_status = initial_status;
        }

        // Send initial status in OrcaSlicer format
        nlohmann::json wrapped_status;
        wrapped_status["print"]["command"] = "push_status";
        wrapped_status["print"]["msg"] = 0;  // Full message
        wrapped_status["print"]["sdcard"] = true;
        for (auto it = initial_status.begin(); it != initial_status.end(); ++it) {
            wrapped_status["print"][it.key()] = it.value();
        }
        BOOST_LOG_TRIVIAL(info) << "CC2: Dispatching initial status, callback set=" << (on_local_message_fn ? "YES" : "NO");
        dispatch_message(actual_dev_id, wrapped_status.dump());
    } else {
        BOOST_LOG_TRIVIAL(warning) << "CC2: Failed to get initial status";
    }

    // Get printer attributes including firmware version
    nlohmann::json attributes_response;
    if (cc2_send_command(GET_ATTRIBUTES, nlohmann::json::object(), attributes_response, 5000)) {
        // Send version info in OrcaSlicer expected format
        nlohmann::json version_msg;
        version_msg["info"]["command"] = "get_version";

        // Create module list with firmware version
        nlohmann::json module;
        module["name"] = "ota";

        // Extract firmware version if available
        if (attributes_response.contains("firmware_version")) {
            module["sw_ver"] = attributes_response["firmware_version"].get<std::string>();
            m_device_info.firmware_version = attributes_response["firmware_version"].get<std::string>();
        } else {
            module["sw_ver"] = "1.0.0";  // Default if not available
        }

        // Add serial number if available
        if (!actual_dev_id.empty()) {
            module["sn"] = actual_dev_id;
        }

        version_msg["info"]["module"] = nlohmann::json::array({module});

        // Dispatch version info
        BOOST_LOG_TRIVIAL(info) << "CC2: Dispatching version info, callback set=" << (on_local_message_fn ? "YES" : "NO");
        dispatch_message(actual_dev_id, version_msg.dump());
        BOOST_LOG_TRIVIAL(info) << "CC2: Sent version info";
    } else {
        BOOST_LOG_TRIVIAL(warning) << "CC2: Failed to get attributes, sending minimal version info";

        // Send minimal version info even if GET_ATTRIBUTES fails
        nlohmann::json version_msg;
        version_msg["info"]["command"] = "get_version";
        nlohmann::json module;
        module["name"] = "ota";
        module["sw_ver"] = "1.0.0";
        if (!actual_dev_id.empty()) {
            module["sn"] = actual_dev_id;
        }
        version_msg["info"]["module"] = nlohmann::json::array({module});
        BOOST_LOG_TRIVIAL(info) << "CC2: Dispatching minimal version info, callback set=" << (on_local_message_fn ? "YES" : "NO");
        dispatch_message(actual_dev_id, version_msg.dump());
    }

    // Initialize MachineObject fields required for is_info_ready() to return true
    // This must be done after the messages are dispatched so the MachineObject exists
    initialize_machine_object(actual_dev_id);

    // Announce device to DeviceManager so it appears in device list
    announce_printhost_device();

    // Notify connection callback
    dispatch_local_connect(0, actual_dev_id, "Connected");

    BOOST_LOG_TRIVIAL(info) << "CC2: Connected successfully";
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::disconnect_printer()
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Disconnecting";

    cc2_stop_heartbeat();
    mqtt_disconnect();

    m_connected = false;
    m_registered = false;

    dispatch_local_connect(0, m_device_info.dev_id, "Disconnected");

    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    if (!m_connected || !m_registered) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Cannot send message - not connected";
        return BAMBU_NETWORK_ERR_CONNECT_FAILED;
    }

    try {
        nlohmann::json command = nlohmann::json::parse(json_str);
        std::string topic = "elegoo/" + dev_id + "/" + m_client_id + "/api_request";

        if (mqtt_publish(topic, command.dump())) {
            return BAMBU_NETWORK_SUCCESS;
        } else {
            return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Failed to send message: " << e.what();
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
}

// ============================================================================
// Certificates (Not Used by CC2)
// ============================================================================

int CC2PrinterAgent::check_cert()
{
    return BAMBU_NETWORK_SUCCESS;
}

void CC2PrinterAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    // CC2 does not use device certificates
}

// ============================================================================
// Discovery (Stub - UDP discovery would go here)
// ============================================================================

bool CC2PrinterAgent::start_discovery(bool start, bool sending)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Discovery " << (start ? "start requested" : "stop requested");

    if (start) {
        // Start discovery thread if not already running
        if (!m_discovery_running) {
            m_discovery_running = true;
            m_discovery_thread = std::thread(&CC2PrinterAgent::discovery_loop, this);
            BOOST_LOG_TRIVIAL(info) << "CC2: Discovery thread started";
        } else {
            BOOST_LOG_TRIVIAL(debug) << "CC2: Discovery thread already running";
        }

        // Also announce already-connected device if any
        if (m_connected) {
            announce_printhost_device();
        }
    } else {
        // Stop discovery thread if running
        if (m_discovery_running) {
            BOOST_LOG_TRIVIAL(info) << "CC2: Stopping discovery thread...";
            m_discovery_running = false;

            if (m_discovery_thread.joinable()) {
                m_discovery_thread.join();
                BOOST_LOG_TRIVIAL(info) << "CC2: Discovery thread stopped";
            }
        }
    }

    return true;
}

bool CC2PrinterAgent::udp_discover_printers(std::vector<CC2DeviceInfo>& printers, int timeout_ms)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Starting UDP discovery on port 52700 (timeout: " << timeout_ms << "ms)";

    try {
        net::io_context ioc;
        net::ip::udp::socket socket(ioc);

        // Open socket and enable broadcast
        socket.open(net::ip::udp::v4());
        socket.set_option(net::socket_base::broadcast(true));

        // Prepare discovery message
        nlohmann::json discovery_msg = {
            {"id", 0},
            {"method", DISCOVERY}
        };
        std::string msg_str = discovery_msg.dump();

        // Send broadcast to port 52700
        net::ip::udp::endpoint broadcast_endpoint(
            net::ip::address_v4::broadcast(), 52700);

        BOOST_LOG_TRIVIAL(info) << "CC2: Sending discovery broadcast: " << msg_str;
        socket.send_to(net::buffer(msg_str), broadcast_endpoint);

        // Receive responses with timeout
        auto start = std::chrono::steady_clock::now();
        printers.clear();

        while (true) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            int remaining_ms = timeout_ms - static_cast<int>(elapsed);
            if (remaining_ms <= 0) {
                break;
            }

            // Set timeout for this receive attempt (at least 100ms)
            int recv_timeout_ms = std::max(100, remaining_ms);

            // Try to receive with timeout using async operations
            char recv_buffer[1024];
            net::ip::udp::endpoint sender_endpoint;
            boost::system::error_code ec;
            bool received = false;
            std::size_t len = 0;

            // Use async_receive_from with timer
            socket.async_receive_from(
                net::buffer(recv_buffer, sizeof(recv_buffer)),
                sender_endpoint,
                [&](const boost::system::error_code& error, std::size_t bytes_transferred) {
                    ec = error;
                    len = bytes_transferred;
                    received = true;
                }
            );

            // Run with timeout
            ioc.restart();
            ioc.run_for(std::chrono::milliseconds(recv_timeout_ms));

            if (!received) {
                // Timeout - cancel pending operations and break
                socket.cancel(ec);
                break;
            }

            if (ec) {
                if (ec == net::error::operation_aborted) {
                    // Timeout
                    break;
                }
                BOOST_LOG_TRIVIAL(warning) << "CC2: Discovery receive error: " << ec.message();
                continue;
            }

            // Parse response
            try {
                std::string response_str(recv_buffer, len);
                BOOST_LOG_TRIVIAL(info) << "CC2: Received discovery response from "
                    << sender_endpoint.address().to_string() << ": " << response_str;

                nlohmann::json response = nlohmann::json::parse(response_str);
                nlohmann::json result = response.value("result", nlohmann::json::object());

                // Extract printer info
                CC2DeviceInfo info;
                info.dev_ip = sender_endpoint.address().to_string();
                info.dev_id = result.value("sn", "");
                info.dev_name = result.value("host_name", "Unknown");
                info.machine_model = result.value("machine_model", "Unknown");
                info.token_required = (result.value("token_status", 0) == 1);
                info.lan_only = (result.value("lan_status", 0) == 1);

                // Check if we already have this printer (by IP)
                bool found = false;
                for (const auto& p : printers) {
                    if (p.dev_ip == info.dev_ip) {
                        found = true;
                        break;
                    }
                }

                if (!found && !info.dev_id.empty()) {
                    BOOST_LOG_TRIVIAL(info) << "CC2: Discovered printer - IP: " << info.dev_ip
                        << ", SN: " << info.dev_id
                        << ", Model: " << info.machine_model
                        << ", Name: " << info.dev_name;
                    printers.push_back(info);
                }

            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "CC2: Failed to parse discovery response: " << e.what();
            }
        }

        BOOST_LOG_TRIVIAL(info) << "CC2: Discovery complete. Found " << printers.size() << " printer(s)";
        return !printers.empty();

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "CC2: Discovery failed: " << e.what();
        return false;
    }
}

// ============================================================================
// Binding (Not Used by CC2)
// ============================================================================

int CC2PrinterAgent::ping_bind(std::string ping_code)
{
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link,
                          std::string timezone, bool improved, OnUpdateStatusFn update_fn)
{
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::unbind(std::string dev_id)
{
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::request_bind_ticket(std::string* ticket)
{
    if (ticket)
        *ticket = "";
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_status_mutex);
    on_server_err_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Machine Selection
// ============================================================================

std::string CC2PrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(m_status_mutex);
    return m_selected_machine;
}

int CC2PrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_status_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Print Job Operations
// ============================================================================

int CC2PrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn,
                                 WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Starting print: " << params.dev_id << ", file: " << params.filename;


    // Build start print command
    nlohmann::json print_params = {
        {"storage_media", "local"},
        {"filename", params.filename},
        {"config", {
            {"delay_video", false},
            {"printer_check", false},
            {"print_layout", "A"},
            {"bedlevel_force", false},
            {"slot_map", nlohmann::json::array()}
        }}
    };

    nlohmann::json response;
    if (cc2_send_command(START_PRINT, print_params, response)) {
        BOOST_LOG_TRIVIAL(info) << "CC2: Print started successfully";
        return BAMBU_NETWORK_SUCCESS;
    } else {
        BOOST_LOG_TRIVIAL(error) << "CC2: Failed to start print";
        return BAMBU_NETWORK_ERR_SEND_MSG_FAILED;
    }
}

int CC2PrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn,
                                                   WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Starting local print with record: " << params.filename;

    // Upload file first (unless it's already a remote filename)
    // Only upload if the file path appears to be a local file (contains directory separators)
    if (params.filename.find('/') != std::string::npos || params.filename.find('\\') != std::string::npos) {
        // If params.filename is a .3mf file, we need to upload the corresponding .gcode file instead
        boost::filesystem::path local_path(params.filename);
        if (local_path.extension() == ".3mf") {
            // Replace .3mf extension with .gcode to get the actual gcode file
            local_path.replace_extension("gcode");
            BOOST_LOG_TRIVIAL(info) << "CC2: Converting 3MF path to GCode path: " << local_path.string();
        }

        // Check if the gcode file exists
        if (!boost::filesystem::exists(local_path)) {
            BOOST_LOG_TRIVIAL(error) << "CC2: GCode file does not exist: " << local_path.string();
            return BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
        }

        // Generate user-friendly remote filename
        std::string remote_filename = cc2_generate_upload_filename(params);
        
        BOOST_LOG_TRIVIAL(info) << "CC2: Uploading file before print: " << local_path.string();
        if (!cc2_upload_file(local_path.string(), remote_filename, update_fn, cancel_fn)) {
            BOOST_LOG_TRIVIAL(error) << "CC2: File upload failed";
            return BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
        }
        // Update filename to remote name
        params.filename = remote_filename;
    } else {
        BOOST_LOG_TRIVIAL(info) << "CC2: File appears to be remote filename, skipping upload: " << params.filename;
    }

    return start_print(std::move(params), update_fn, cancel_fn, wait_fn);
}

int CC2PrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn,
                                                WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Uploading file: " << params.filename;

    // Check if this is a verification job (access code check)
    bool is_verify_job = (params.project_name == "verify_job");

    if (is_verify_job) {
        BOOST_LOG_TRIVIAL(info) << "CC2: Access code verification - skipping upload (already validated via MQTT)";
        // For CC2, the access code is already validated during MQTT connection.
        // No need to upload a test file. Just return success.
        return BAMBU_NETWORK_SUCCESS;
    }

    // Upload file
    std::string remote_filename = boost::filesystem::path(params.filename).filename().string();
    if (!cc2_upload_file(params.filename, remote_filename, update_fn, cancel_fn)) {
        BOOST_LOG_TRIVIAL(error) << "CC2: File upload failed";
        return BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
    }

    // Update filename to remote name
    params.filename = remote_filename;

    // Start print
    return start_print(std::move(params), update_fn, cancel_fn, wait_fn);
}

int CC2PrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Starting local print: " << params.filename;

    // Upload file first (unless it's already a remote filename)
    // Only upload if the file path appears to be a local file (contains directory separators)
    if (params.filename.find('/') != std::string::npos || params.filename.find('\\') != std::string::npos) {
        // If params.filename is a .3mf file, we need to upload the corresponding .gcode file instead
        boost::filesystem::path local_path(params.filename);
        if (local_path.extension() == ".3mf") {
            // Replace .3mf extension with .gcode to get the actual gcode file
            local_path.replace_extension("gcode");
            BOOST_LOG_TRIVIAL(info) << "CC2: Converting 3MF path to GCode path: " << local_path.string();
        }

        // Check if the gcode file exists
        if (!boost::filesystem::exists(local_path)) {
            BOOST_LOG_TRIVIAL(error) << "CC2: GCode file does not exist: " << local_path.string();
            return BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
        }

        // Generate user-friendly remote filename
        std::string remote_filename = cc2_generate_upload_filename(params);
        
        BOOST_LOG_TRIVIAL(info) << "CC2: Uploading file before print: " << local_path.string();
        if (!cc2_upload_file(local_path.string(), remote_filename, update_fn, cancel_fn)) {
            BOOST_LOG_TRIVIAL(error) << "CC2: File upload failed";
            return BAMBU_NETWORK_ERR_FTP_UPLOAD_FAILED;
        }
        // Update filename to remote name
        params.filename = remote_filename;
    } else {
        BOOST_LOG_TRIVIAL(info) << "CC2: File appears to be remote filename, skipping upload: " << params.filename;
    }

    return start_print(std::move(params), update_fn, cancel_fn, nullptr);
}

int CC2PrinterAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return start_print(std::move(params), update_fn, cancel_fn, nullptr);
}

// ============================================================================
// Callbacks
// ============================================================================

int CC2PrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    on_ssdp_msg_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    on_printer_connected_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    on_subscribe_failure_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    on_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_on_user_message_fn(OnMessageFn fn)
{
    on_user_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    on_local_connect_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    on_local_message_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

int CC2PrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    queue_on_main_fn = fn;
    return BAMBU_NETWORK_SUCCESS;
}

// ============================================================================
// Helper Dispatch Functions
// ============================================================================

void CC2PrinterAgent::dispatch_local_connect(int state, const std::string& dev_id, const std::string& msg)
{
    if (on_local_connect_fn) {
        on_local_connect_fn(state, dev_id, msg);
    }
}

void CC2PrinterAgent::dispatch_message(const std::string& dev_id, const std::string& payload)
{
    if (on_local_message_fn) {
        BOOST_LOG_TRIVIAL(debug) << "CC2: Dispatching status message to UI for dev_id=" << dev_id;
        on_local_message_fn(dev_id, payload);
    } else {
        BOOST_LOG_TRIVIAL(warning) << "CC2: Cannot dispatch message - on_local_message_fn callback not set";
    }
}

void CC2PrinterAgent::initialize_machine_object(const std::string& dev_id)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Initializing MachineObject for dev_id=" << dev_id;

    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        BOOST_LOG_TRIVIAL(warning) << "CC2: DeviceManager not available during initialization";
        return;
    }

    MachineObject* obj = dev_manager->get_my_machine(dev_id);
    if (!obj) {
        BOOST_LOG_TRIVIAL(warning) << "CC2: MachineObject not found for dev_id: " << dev_id;
        return;
    }

    // Set push counters so is_info_ready() returns true
    if (obj->m_push_count == 0) {
        obj->m_push_count = 1;
        BOOST_LOG_TRIVIAL(info) << "CC2: Set m_push_count = 1";
    }
    if (obj->m_full_msg_count == 0) {
        obj->m_full_msg_count = 1;
        BOOST_LOG_TRIVIAL(info) << "CC2: Set m_full_msg_count = 1";
    }
    obj->last_push_time = std::chrono::system_clock::now();
    BOOST_LOG_TRIVIAL(info) << "CC2: Updated last_push_time";

    // Set storage state - CC2 printers have local storage
    obj->GetStorage()->set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);
    BOOST_LOG_TRIVIAL(info) << "CC2: Set storage state to HAS_SDCARD_NORMAL";

    // Populate module_vers so is_info_ready() passes the version check
    if (obj->module_vers.empty()) {
        DevFirmwareVersionInfo ota_info;
        ota_info.name = "ota";
        ota_info.sw_ver = m_device_info.firmware_version.empty() ? "1.0.0" : m_device_info.firmware_version;
        obj->module_vers.emplace("ota", ota_info);
        BOOST_LOG_TRIVIAL(info) << "CC2: Set module_vers['ota'].sw_ver = " << ota_info.sw_ver;
    }

    // Set printer_type so update_sync_status() can match it against the preset's printer type
    if (obj->printer_type.empty() && !m_device_info.machine_model.empty()) {
        obj->printer_type = m_device_info.machine_model;
        BOOST_LOG_TRIVIAL(info) << "CC2: Set printer_type = " << obj->printer_type;
    }

    BOOST_LOG_TRIVIAL(info) << "CC2: MachineObject initialization complete - is_info_ready should now return true";
}

void CC2PrinterAgent::announce_printhost_device()
{
    OnMsgArrivedFn ssdp_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        ssdp_fn = on_ssdp_msg_fn;
        if (!ssdp_fn) {
            BOOST_LOG_TRIVIAL(warning) << "CC2: Cannot announce device - SSDP callback not set";
            return;
        }
        // Skip if already announced this device
        if (m_ssdp_announced_id == m_device_info.dev_id &&
            m_ssdp_announced_ip == m_device_info.dev_ip &&
            !m_ssdp_announced_id.empty()) {
            BOOST_LOG_TRIVIAL(info) << "CC2: Device already announced: " << m_device_info.dev_id;
            return;
        }
    }

    // Use discovered device name or fall back to dev_id
    std::string dev_name = m_device_info.dev_name.empty() ? m_device_info.dev_id : m_device_info.dev_name;
    // Always use "CC2" as model_id to match the configuration file
    std::string model_id = "CC2";

    // Save access code to AppConfig so DeviceManager can retrieve it
    if (auto* app_config = GUI::wxGetApp().app_config) {
        const std::string access_code = m_device_info.access_code.empty() ? "123456" : m_device_info.access_code;
        app_config->set_str("access_code", m_device_info.dev_id, access_code);
        app_config->set_str("user_access_code", m_device_info.dev_id, access_code);
        BOOST_LOG_TRIVIAL(info) << "CC2: Saved access code to AppConfig for dev_id: " << m_device_info.dev_id;
    }

    // Create SSDP announcement payload matching DeviceManager expectations
    nlohmann::json payload;
    payload["dev_name"]     = dev_name;
    payload["dev_id"]       = m_device_info.dev_id;
    payload["dev_ip"]       = m_device_info.dev_ip;
    payload["dev_type"]     = model_id;
    payload["dev_signal"]   = "0";
    payload["connect_type"] = "lan";
    payload["bind_state"]   = "free";
    payload["sec_link"]     = "secure";
    payload["ssdp_version"] = "v1";

    BOOST_LOG_TRIVIAL(info) << "CC2: Announcing device - Name: " << dev_name
                           << ", ID: " << m_device_info.dev_id
                           << ", IP: " << m_device_info.dev_ip
                           << ", Model: " << model_id;

    // Call SSDP callback to notify DeviceManager
    ssdp_fn(payload.dump());

    {
        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
        m_ssdp_announced_id = m_device_info.dev_id;
        m_ssdp_announced_ip = m_device_info.dev_ip;

        // Set this as the selected machine if nothing is currently selected
        if (m_selected_machine.empty()) {
            m_selected_machine = m_device_info.dev_id;
        }
    }
}

void CC2PrinterAgent::discovery_loop()
{
    BOOST_LOG_TRIVIAL(info) << "CC2: Discovery loop started";

    while (m_discovery_running) {
        try {
            std::vector<CC2DeviceInfo> discovered_printers;

            // Run UDP discovery with 5 second timeout
            BOOST_LOG_TRIVIAL(debug) << "CC2: Running UDP discovery scan...";
            if (udp_discover_printers(discovered_printers, 5000)) {
                BOOST_LOG_TRIVIAL(info) << "CC2: Discovered " << discovered_printers.size() << " printer(s)";

                // Announce each discovered printer via SSDP
                for (const auto& printer : discovered_printers) {
                    if (!m_discovery_running) break;  // Exit early if discovery stopped

                    // Check if we haven't announced this printer yet or if IP changed
                    bool should_announce = false;
                    {
                        std::lock_guard<std::recursive_mutex> lock(m_state_mutex);
                        should_announce = (m_ssdp_announced_id != printer.dev_id ||
                                         m_ssdp_announced_ip != printer.dev_ip);
                    }

                    if (should_announce) {
                        // Temporarily store discovered printer info and announce
                        CC2DeviceInfo old_device_info = m_device_info;
                        m_device_info = printer;

                        BOOST_LOG_TRIVIAL(info) << "CC2: Announcing discovered printer - "
                                               << "Name: " << printer.dev_name
                                               << ", ID: " << printer.dev_id
                                               << ", IP: " << printer.dev_ip;

                        announce_printhost_device();

                        // Restore original device info if we were connected to a different device
                        if (!old_device_info.dev_id.empty()) {
                            m_device_info = old_device_info;
                        }
                    }
                }
            } else {
                BOOST_LOG_TRIVIAL(debug) << "CC2: Discovery scan found no printers";
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CC2: Discovery loop exception: " << e.what();
        }

        // Sleep for 30 seconds before next scan (if still running)
        for (int i = 0; i < 30 && m_discovery_running; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    BOOST_LOG_TRIVIAL(info) << "CC2: Discovery loop stopped";
}


// ============================================================================
// Filament Sync (Stub for future Canvas/AMS support)
// ============================================================================

bool CC2PrinterAgent::fetch_filament_info(std::string dev_id)
{
    BOOST_LOG_TRIVIAL(info) << "CC2: fetch_filament_info for dev_id=" << dev_id;

    // Look up MachineObject via DeviceManager
    auto* dev_manager = GUI::wxGetApp().getDeviceManager();
    if (!dev_manager) {
        BOOST_LOG_TRIVIAL(warning) << "CC2: DeviceManager not available";
        return false;
    }

    MachineObject* obj = dev_manager->get_my_machine(m_device_info.dev_id);
    if (!obj) {
        BOOST_LOG_TRIVIAL(warning) << "CC2: MachineObject not found for dev_id: " << m_device_info.dev_id;
        return false;
    }

    // Fetch Canvas status from printer
    nlohmann::json canvas_response;
    if (!cc2_send_command(GET_CANVAS_STATUS, nlohmann::json::object(), canvas_response, 5000)) {
        BOOST_LOG_TRIVIAL(info) << "CC2: No Canvas/AMS detected (GET_CANVAS_STATUS failed)";
        // Not an error - printer simply doesn't have Canvas attached
        return false;
    }

    // Extract canvas_info from response
    if (!canvas_response.contains("canvas_info")) {
        BOOST_LOG_TRIVIAL(info) << "CC2: Canvas response missing canvas_info field";
        return false;
    }

    nlohmann::json canvas_info = canvas_response["canvas_info"];
    if (!canvas_info.contains("canvas_list") || !canvas_info["canvas_list"].is_array()) {
        BOOST_LOG_TRIVIAL(info) << "CC2: Canvas response missing canvas_list";
        return false;
    }

    nlohmann::json canvas_list = canvas_info["canvas_list"];
    if (canvas_list.empty()) {
        BOOST_LOG_TRIVIAL(info) << "CC2: No Canvas units found";
        return false;
    }

    // Count total trays and find max index
    int tray_count = 0;
    int max_tray_index = -1;
    for (const auto& canvas : canvas_list) {
        if (!canvas.contains("tray_list") || !canvas["tray_list"].is_array()) {
            continue;
        }
        int canvas_id = canvas.value("canvas_id", 0);
        for (const auto& tray : canvas["tray_list"]) {
            int tray_id = tray.value("tray_id", 0);
            int global_tray_index = canvas_id * 4 + tray_id;  // 4 trays per Canvas
            max_tray_index = std::max(max_tray_index, global_tray_index);
            tray_count++;
        }
    }

    if (max_tray_index < 0) {
        BOOST_LOG_TRIVIAL(info) << "CC2: No trays found in Canvas units";
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "CC2: Found " << tray_count << " trays across " << canvas_list.size() << " Canvas units";

    // Build BBL-format JSON for DevFilaSystemParser::ParseV1_0
    nlohmann::json ams_json = nlohmann::json::object();
    nlohmann::json ams_array = nlohmann::json::array();

    // Calculate number of AMS units needed (4 trays per AMS)
    int ams_count = (max_tray_index + 4) / 4;
    unsigned long ams_exist_bits = 0;
    unsigned long tray_exist_bits = 0;

    // Mark all AMS units as existing
    for (int ams_id = 0; ams_id < ams_count; ++ams_id) {
        ams_exist_bits |= (1 << ams_id);
    }

    // Build AMS structure
    for (int ams_id = 0; ams_id < ams_count; ++ams_id) {
        nlohmann::json ams_unit = nlohmann::json::object();
        ams_unit["id"] = std::to_string(ams_id);
        ams_unit["info"] = "0002";  // Treat as AMS_LITE

        nlohmann::json tray_array = nlohmann::json::array();
        int max_slot_in_this_ams = std::min(3, max_tray_index - ams_id * 4);

        for (int slot_id = 0; slot_id <= max_slot_in_this_ams; ++slot_id) {
            int global_index = ams_id * 4 + slot_id;

            nlohmann::json tray_json = nlohmann::json::object();
            tray_json["id"] = std::to_string(slot_id);
            tray_json["tag_uid"] = "0000000000000000";

            // Find matching tray in Canvas data
            bool found_tray = false;
            for (const auto& canvas : canvas_list) {
                if (!canvas.contains("tray_list") || !canvas["tray_list"].is_array()) {
                    continue;
                }
                int canvas_id = canvas.value("canvas_id", 0);
                for (const auto& tray : canvas["tray_list"]) {
                    int tray_id = tray.value("tray_id", 0);
                    if (canvas_id * 4 + tray_id == global_index) {
                        int status = tray.value("status", 0);
                        if (status > 0) {  // 1=loaded, 2=active
                            found_tray = true;
                            tray_exist_bits |= (1 << global_index);

                            // Map filament_type to tray_info_idx (generic filament ID)
                            std::string filament_type = tray.value("filament_type", "");
                            std::string tray_info_idx = cc2_map_filament_type(filament_type);
                            tray_json["tray_info_idx"] = tray_info_idx;
                            tray_json["tray_type"] = filament_type;

                            // Parse color (remove # prefix if present)
                            std::string color = tray.value("filament_color", "#000000");
                            if (!color.empty() && color[0] == '#') {
                                color = color.substr(1);
                            }
                            // Convert to uppercase and ensure 8 chars (add FF for alpha if needed)
                            std::transform(color.begin(), color.end(), color.begin(), ::toupper);
                            if (color.length() == 6) {
                                color += "FF";  // Add full opacity
                            }
                            tray_json["tray_color"] = color;

                            // Temperature data
                            int min_temp = tray.value("min_nozzle_temp", 0);
                            int max_temp = tray.value("max_nozzle_temp", 0);
                            if (max_temp > 0) {
                                tray_json["nozzle_temp_max"] = std::to_string(max_temp);
                            }
                            if (min_temp > 0) {
                                tray_json["nozzle_temp_min"] = std::to_string(min_temp);
                            }

                            BOOST_LOG_TRIVIAL(info) << "CC2: Canvas " << canvas_id << " Tray " << tray_id
                                << " (global: " << global_index << ")"
                                << " - Type: " << filament_type
                                << ", tray_info_idx: " << tray_info_idx
                                << ", Color: " << color
                                << ", Status: " << status;
                        }
                        break;
                    }
                }
                if (found_tray) break;
            }

            if (!found_tray) {
                // Empty slot
                tray_json["tray_info_idx"] = "";
                tray_json["tray_type"] = "";
                tray_json["tray_color"] = "00000000";
                tray_json["tray_slot_placeholder"] = "1";
            }

            tray_array.push_back(tray_json);
        }

        ams_unit["tray"] = tray_array;
        ams_array.push_back(ams_unit);
    }

    // Format as hex strings (matching BBL protocol)
    std::ostringstream ams_exist_ss;
    ams_exist_ss << std::hex << std::uppercase << ams_exist_bits;
    std::ostringstream tray_exist_ss;
    tray_exist_ss << std::hex << std::uppercase << tray_exist_bits;

    ams_json["ams"] = ams_array;
    ams_json["ams_exist_bits"] = ams_exist_ss.str();
    ams_json["tray_exist_bits"] = tray_exist_ss.str();

    // Wrap in the expected structure for ParseV1_0
    nlohmann::json print_json = nlohmann::json::object();
    print_json["ams"] = ams_json;

    // Debug: Log the AMS JSON structure
    BOOST_LOG_TRIVIAL(info) << "CC2: AMS JSON structure:\n" << print_json.dump(2);

    // Call the parser to populate DevFilaSystem
    DevFilaSystemParser::ParseV1_0(print_json, obj, obj->GetFilaSystem(), false);
    BOOST_LOG_TRIVIAL(info) << "CC2: Parsed " << tray_count << " Canvas trays into " << ams_count << " AMS units";

    // Set printer_type so update_sync_status() can match it against the preset's printer type
    obj->printer_type = m_device_info.machine_model;

    // Set push counters so is_info_ready() returns true
    if (obj->m_push_count == 0) {
        obj->m_push_count = 1;
    }
    if (obj->m_full_msg_count == 0) {
        obj->m_full_msg_count = 1;
    }
    obj->last_push_time = std::chrono::system_clock::now();

    // Set storage state - CC2 printers have local storage
    obj->GetStorage()->set_sdcard_state(DevStorage::HAS_SDCARD_NORMAL);

    // Populate module_vers so is_info_ready() passes the version check
    if (obj->module_vers.empty()) {
        DevFirmwareVersionInfo ota_info;
        ota_info.name = "ota";
        ota_info.sw_ver = m_device_info.firmware_version.empty() ? "1.0.0" : m_device_info.firmware_version;
        obj->module_vers.emplace("ota", ota_info);
    }

    return true;
}

} // namespace Slic3r
