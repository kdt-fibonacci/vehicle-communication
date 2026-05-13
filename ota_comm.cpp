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

// --- 구조체 정의 ---
struct EcuVersion {
    std::string address;
    std::string version;
};

// --- 전역 설정 및 상수 ---
const std::string VERSION_FILE = "./ecu_versions.json";
const std::string CHECK_URL = "http://192.168.201.54:4321/ota/check";
const std::string REPORT_URL = "http://192.168.201.54:4321/ota/report"; // 리포트 전용 주소
const std::string MQTT_ADDRESS = "tcp://192.168.201.54:1883";
const std::string CLIENT_ID = "RPi_OTA_Client";
const std::string TOPIC = "ota/update";
const std::string DEVICE_ID = "0001";
const std::string LOCAL_PUBLIC_KEY_PATH = "./public.pem";
const std::string GATEWAY_IP = "192.168.1.20";

// --- 외부 모듈 함수 선언 (NRC 처리를 위해 int로 변경) ---
extern bool verifyFirmwareSecurity(const std::string& addr, const std::string& version, const std::string& publicKeyPath);
extern int startOtaTransfer(const std::string& targetAddrStr, const std::string& version, const std::string& gatewayIp);

// --- [헬퍼 함수] ---
std::string toHexStr(int val) {
    std::stringstream ss;
    ss << "0x" << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << val;
    return ss.str();
}

// CURL용 쓰기 콜백
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    if (userp) ((std::string*)userp)->append((char*)contents, realsize);
    return realsize;
}

// 파일 다운로드 함수 (중요: 정의가 포함되어야 함)
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

// --- [1] 서버 상태 보고 함수 ---
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

    std::cout << "[Report] Status: " << status << " (NRC: " << nrc << ") Sent to server." << std::endl;
    curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

// --- [2] 버전 관리 시스템 ---
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

// --- [3] 핵심 업데이트 실행 로직 ---
void executeUpdate(const std::string& addr, const std::string& ver, const std::string& f_url, const std::string& s_url) {
    std::cout << "\n----------------------------------------" << std::endl;
    std::cout << ">> [OTA Engine] Processing ECU 0x" << addr << " v" << ver << std::endl;

    reportStatusToServer(addr, ver, "DOWNLOADING");

    std::string hex_file = addr + "_" + ver + ".hex";
    std::string sig_file = addr + "_" + ver + ".sig";

    // 1. 다운로드
    if (!downloadFile(f_url, hex_file) || !downloadFile(s_url, sig_file)) {
        reportStatusToServer(addr, ver, "FAILED", "0xFF"); // 0xFF: 다운로드 에러 코드(임의)
        return;
    }

    // 2. 보안 검증
    if (!verifyFirmwareSecurity(addr, ver, LOCAL_PUBLIC_KEY_PATH)) {
        reportStatusToServer(addr, ver, "AUTH_FAILED");
        return;
    }

    // 3. UDS 전송 (NRC 대응)
    reportStatusToServer(addr, ver, "FLASHING");
    int result = startOtaTransfer(addr, ver, GATEWAY_IP);

    if (result == 0) {
        std::cout << "✅ [Success] Update sequence finished!" << std::endl;
        reportStatusToServer(addr, ver, "SUCCESS");
        saveLocalVersion(addr, ver);
    } else {
        std::string nrc_hex = toHexStr(result);
        std::cerr << "❌ [Error] Failed with NRC: " << nrc_hex << std::endl;
        reportStatusToServer(addr, ver, "FAILED", nrc_hex);
    }
    std::cout << "----------------------------------------" << std::endl;
}

// --- [4] 초기 동기화 및 MQTT 콜백 ---

void performInitialSync() {
    std::cout << "[Init] Checking for updates via HTTP..." << std::endl;
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
                    executeUpdate(update["address"], update["version"], 
                                  update["firmware_url"], update["signature_url"]);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Error] Initial Sync JSON Error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "[Error] Initial Sync HTTP Failed: " << curl_easy_strerror(res) << std::endl;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
}

class ota_callback : public virtual mqtt::callback {
    void message_arrived(mqtt::const_message_ptr msg) override {
        std::string payload = msg->get_payload_str();
        std::cout << "\n[MQTT] Push Notification: " << payload << std::endl;

        try {
            auto data = json::parse(payload);
            if (data.contains("address") && data.contains("version")) {
                std::string addr = data["address"];
                std::string ver = data["version"];

                // 중복 체크
                auto localEcus = loadLocalVersions();
                for (const auto& ecu : localEcus) {
                    if (ecu.address == addr && ecu.version == ver) {
                        std::cout << ">> [Skip] ECU 0x" << addr << " is already version " << ver << std::endl;
                        return;
                    }
                }
                
                // 즉시 업데이트 수행 (MQTT 페이로드 데이터 활용)
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

// --- [5] 서비스 시작점 ---

void runOtaService() {
    curl_global_init(CURL_GLOBAL_ALL);

    // 1. 초기 1회 HTTP 체크
    performInitialSync();

    // 2. MQTT 대기 모드 진입
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
    } catch (const std::exception& e) {
        std::cerr << "[System Critical] Runtime Error: " << e.what() << std::endl;
    }

    curl_global_cleanup();
}