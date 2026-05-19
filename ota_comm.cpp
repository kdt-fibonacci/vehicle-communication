// ota_comm.cpp
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <mqtt/async_client.h>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;

// --- [상태 머신 정의] ---
enum class OtaState {
    OFF,
    IDLE,
    WAIT,
    READY,
    DOWNLOAD,
    VERIFICATION,
    INSTALL,
    WAIT_ACTIVATION,
    ACTIVATION,
    RECOVERY,
    REPORTING
};

const std::string StateStrings[] = {
    "OFF", "IDLE", "WAIT", "READY", "DOWNLOAD", "VERIFICATION", 
    "INSTALL", "WAIT_ACTIVATION", "ACTIVATION", "RECOVERY", "REPORTING"
};

OtaState currentOtaState = OtaState::OFF;

void changeState(OtaState newState) {
    std::cout << "\n==================================================" << std::endl;
    std::cout << "[STATE CHANGED] " << StateStrings[static_cast<int>(currentOtaState)] 
              << " ➡️ " << StateStrings[static_cast<int>(newState)] << std::endl;
    std::cout << "==================================================" << std::endl;
    currentOtaState = newState;
}

struct EcuVersion {
    std::string address;
    std::string version;
};

const std::string VERSION_FILE = "./ecu_versions.json";
const std::string CHECK_URL = "http://192.168.200.135:4321/ota/check";
const std::string REPORT_URL = "http://192.168.200.135:4321/ota/report";
const std::string MQTT_ADDRESS = "tcp://192.168.200.135:1883";
const std::string CLIENT_ID = "RPi_OTA_Client";
const std::string TOPIC = "ota/update";
const std::string DEVICE_ID = "0001";
const std::string LOCAL_PUBLIC_KEY_PATH = "./public.pem";
const std::string GATEWAY_IP = "192.168.1.20";

// --- 외부 모듈 함수 순수 참조 선언 ---
extern bool verifyFirmwareSecurity(const std::string& addr, const std::string& version, const std::string& publicKeyPath);
extern int startOtaTransfer(const std::string& targetAddrStr, const std::string& version, const std::string& gatewayIp, void (*stateCallback)(OtaState));

std::string toHexStr(int val) {
    std::stringstream ss;
    ss << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << val;
    return ss.str();
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    if (userp) ((std::string*)userp)->append((char*)contents, realsize);
    return realsize;
}

bool downloadFile(const std::string& url, const std::string& save_path) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    FILE *fp = fopen(save_path.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(curl); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "[Download Error] " << curl_easy_strerror(res) << std::endl;
        return false;
    }
    return true;
}

void reportStatusToServer(std::string addr, std::string ver, std::string status, std::string nrc = "0x00") {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    json report;
    report["device_id"] = DEVICE_ID;
    report["ecu_address"] = addr;
    report["version"] = ver;
    report["status"] = status;
    report["error_code"] = nrc;

    std::string data = report.dump();
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, REPORT_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    std::cout << "[Report Content] Status: " << status << " (NRC: " << nrc << ") Forwarding to backend server." << std::endl;
    curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

std::vector<EcuVersion> loadLocalVersions() {
    std::vector<EcuVersion> ecus;
    std::ifstream file(VERSION_FILE);
    if (!file.is_open()) return {{"1234", "1.0"}, {"5678", "1.0"}};
    try {
        json j; file >> j;
        for (auto& item : j["ecus"]) ecus.push_back({item["address"], item["version"]});
    } catch (...) { return {{"1234", "1.0"}, {"5678", "1.0"}}; }
    return ecus;
}

void saveLocalVersion(const std::string& addr, const std::string& new_ver) {
    auto ecus = loadLocalVersions();
    json j;
    for (auto& ecu : ecus) {
        if (ecu.address == addr) ecu.version = new_ver;
        j["ecus"].push_back({{"address", ecu.address}, {"version", ecu.version}});
    }
    std::ofstream file(VERSION_FILE);
    if (file.is_open()) file << j.dump(4);
}

void executeUpdate(const std::string& addr, const std::string& ver, const std::string& f_url, const std::string& s_url) {
    std::cout << "\n----------------------------------------" << std::endl;
    std::cout << ">> [OTA Engine] Processing ECU 0x" << addr << " v" << ver << std::endl;

    // 💡 [개선 포인트] DOWNLOAD 상태로 가기 전 사용자의 수락 여부를 점검하는 블로킹 필터
    std::cout << "\n [USER PROMPT] ota 새로운 업데이트 파일이 있습니다." << std::endl;
    std::cout << " 업데이트를 하시겠습니까? (y/n) [기본값: y]: ";
    
    std::string userInput;
    std::getline(std::cin, userInput);

    // 사용자가 'n' 또는 'N'을 누르면 업데이트 세션을 취소하고 대기 상태로 이탈
    if (userInput == "n" || userInput == "N") {
        std::cout << " [OTA Engine] Update canceled by user. Returning to monitor mode." << std::endl;
        changeState(OtaState::WAIT);
        std::cout << "----------------------------------------" << std::endl;
        return;
    }

    // State 5: DOWNLOAD
    changeState(OtaState::DOWNLOAD);
    reportStatusToServer(addr, ver, "DOWNLOADING");

    std::string hex_file = addr + "_" + ver + ".hex";
    std::string sig_file = addr + "_" + ver + ".sig";

    if (!downloadFile(f_url, hex_file) || !downloadFile(s_url, sig_file)) {
        changeState(OtaState::REPORTING);
        reportStatusToServer(addr, ver, "FAILED", "0xFF");
        changeState(OtaState::WAIT);
        return;
    }

    // State 6: VERIFICATION
    changeState(OtaState::VERIFICATION);
    if (!verifyFirmwareSecurity(addr, ver, LOCAL_PUBLIC_KEY_PATH)) {
        changeState(OtaState::REPORTING);
        reportStatusToServer(addr, ver, "AUTH_FAILED");
        changeState(OtaState::WAIT);
        return;
    }

    // State 7: INSTALL
    changeState(OtaState::INSTALL);
    reportStatusToServer(addr, ver, "FLASHING");
    
    int result = startOtaTransfer(addr, ver, GATEWAY_IP, changeState);

    // State 11: REPORTING
    changeState(OtaState::REPORTING);
    if (result == 0) {
        std::cout << "✅ [Success] Update sequence finished!" << std::endl;
        reportStatusToServer(addr, ver, "SUCCESS");
        saveLocalVersion(addr, ver);
    } else {
        std::string nrc_hex = toHexStr(result);
        std::cerr << "❌ [Error] Failed with NRC: " << nrc_hex << std::endl;
        reportStatusToServer(addr, ver, "FAILED", nrc_hex);
    }
    
    changeState(OtaState::WAIT);
    std::cout << "----------------------------------------" << std::endl;
}

void performInitialSync() {
    auto localEcus = loadLocalVersions();
    CURL *curl = curl_easy_init();
    if (!curl) return;

    std::string response_string;
    json req;
    req["device_id"] = DEVICE_ID;
    for (const auto& ecu : localEcus) {
        req["ecus"].push_back({{"address", ecu.address}, {"version", ecu.version}});
    }
    std::string req_data = req.dump();

    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, CHECK_URL.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        try {
            auto res_json = json::parse(response_string);
            for (const auto& update : res_json["updates"]) {
                if (update.value("update", false)) {
                    changeState(OtaState::READY); // State 4: READY
                    executeUpdate(update["address"], update["version"], 
                                  update["firmware_url"], update["signature_url"]);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Error] Initial Sync JSON Error: " << e.what() << std::endl;
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

class ota_callback : public virtual mqtt::callback {
    void message_arrived(mqtt::const_message_ptr msg) override {
        std::string payload = msg->get_payload_str();
        std::cout << "\n[MQTT] Push Notification Received: " << payload << std::endl;

        try {
            auto data = json::parse(payload);
            if (data.contains("address") && data.contains("version")) {
                std::string addr = data["address"];
                std::string ver = data["version"];

                auto localEcus = loadLocalVersions();
                for (const auto& ecu : localEcus) {
                    if (ecu.address == addr && ecu.version == ver) {
                        std::cout << ">> [Skip] ECU 0x" << addr << " is already up-to-date." << std::endl;
                        return;
                    }
                }
                
                changeState(OtaState::READY); // State 4: READY
                executeUpdate(addr, ver, data["firmware_url"], data["signature_url"]);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Error] MQTT Payload Parse Error: " << e.what() << std::endl;
        }
    }

    void connection_lost(const std::string& cause) override {
        std::cout << "[MQTT] Connection lost: " << cause << std::endl;
    }
};

void runOtaService() {
    changeState(OtaState::IDLE); // State 2: IDLE
    curl_global_init(CURL_GLOBAL_ALL);

    performInitialSync();

    changeState(OtaState::WAIT); // State 3: WAIT
    try {
        mqtt::async_client client(MQTT_ADDRESS, CLIENT_ID);
        ota_callback cb;
        client.set_callback(cb);

        auto connOpts = mqtt::connect_options_builder()
            .clean_session(true)
            .keep_alive_interval(std::chrono::seconds(30))
            .finalize();

        std::cout << "[MQTT] Connecting to " << MQTT_ADDRESS << "..." << std::endl;
        client.connect(connOpts)->wait();
        client.subscribe(TOPIC, 1)->wait();
        std::cout << "[MQTT] Subscribed to [" << TOPIC << "]. Monitoring for updates..." << std::endl;

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const mqtt::exception& e) {
        std::cerr << "[MQTT Critical] Error: " << e.what() << std::endl;
    }
    curl_global_cleanup();
}