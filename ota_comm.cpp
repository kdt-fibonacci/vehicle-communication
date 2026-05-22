// ota_comm.cpp
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <mqtt/async_client.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <mutex>

#include "state.h"
#include "struct.h"


using json = nlohmann::json;

std::mutex queue_mutex;

extern STATE current_state;
extern STATE prev_state;

extern int current_progress;

extern char update_ecu_name[16];
extern char update_ecu_version[16];

extern std::queue<UpdateItem> update_queue;

struct EcuVersion {
    std::string address;
    std::string version;
};

const std::string VERSION_FILE = "./ecu_versions.json";
const std::string CHECK_URL = "http://192.168.203.213:4321/ota/check";
const std::string REPORT_URL = "http://192.168.203.213:4321/ota/report";
const std::string MQTT_ADDRESS = "tcp://192.168.203.213:1883";
const std::string CLIENT_ID = "RPi_OTA_Client";
const std::string TOPIC = "ota/update";
const std::string DEVICE_ID = "0001";
const std::string LOCAL_PUBLIC_KEY_PATH = "./public.pem";
const std::string GATEWAY_IP = "192.168.1.20";

// --- 외부 모듈 함수 순수 참조 선언 ---
extern bool verifyFirmwareSecurity(const std::string& addr, const std::string& version, const std::string& publicKeyPath);
extern int startOtaTransfer(const std::string& targetAddrStr, const std::string& version, const std::string& gatewayIp);

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

long getLocalFileSize(const std::string& filename) {
    struct stat stat_buf;
    int rc = stat(filename.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : 0;
}

long getServerFileSize(const std::string& url) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); // 헤더만 슥 요청
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    if (curl_easy_perform(curl) == CURLE_OK) {
        curl_off_t cl; 
        if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK) {
            curl_easy_cleanup(curl);
            return (long)cl;
        }
    }
    curl_easy_cleanup(curl);
    return -1;
}

bool downloadFile(const std::string& url, const std::string& save_path) {
    long local_bytes = getLocalFileSize(save_path);
    long server_bytes = getServerFileSize(url);

    // 1. 💡 [핵심 방어]: 만약 로컬에 이미 받아둔 크기가 서버 원본 크기와 '같거나 더 크다면'
    // 이미 100% 온전하게 다 받은 파일이므로, 서버를 찌르지 않고 즉시 완료(true) 처리합니다.
    if (server_bytes > 0 && local_bytes >= server_bytes) {
        std::cout << "🎯 [DOWNLOAD SKIP] " << save_path << " 파일은 이미 100% 다운로드 완료되어 있습니다. (스킵)" << std::endl;
        return true; 
    }

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    // 2. 파일 오픈 모드를 "ab"로 개방 (지우지 않고 이어붙이기)
    FILE *fp = fopen(save_path.c_str(), "ab");
    if (!fp) { curl_easy_cleanup(curl); return false; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

// 3. 로컬에 받아둔 데이터 조각이 원본보다 작을 때만 안전하게 이어받기(Resume) 가동
    if (local_bytes > 0 && local_bytes < server_bytes) {
        std::cout << "[이어받기] 기존 파일 조각(" << local_bytes << " / " << server_bytes << " bytes) 발견. 이어서 받습니다." << std::endl;
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)local_bytes);
    }

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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);

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
    /* ========================= */
    /* LCD 표시용 정보 설정 */
    /* ========================= */
    strncpy(update_ecu_name, addr.c_str(), sizeof(update_ecu_name) - 1);
    update_ecu_name[sizeof(update_ecu_name) - 1] = '\0';

    strncpy(update_ecu_version, ver.c_str(), sizeof(update_ecu_version) - 1);
    update_ecu_version[sizeof(update_ecu_version) - 1] = '\0';
    
    /* ========================= */
    /* 상태 변경 및 승인 대기  */
    /* ========================= */
    current_state = READY;

    while (current_state == READY)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    /* 사용자가 NO 선택 */
    if (current_state == PENDING)
    {
        return;
    }
    
    /* ========================= */
    /* DOWNLOAD */
    /* ========================= */
    current_state = DOWNLOAD;
    reportStatusToServer(addr, ver, "DOWNLOADING");
    std::string hex_file = addr + "_" + ver + ".hex";
    std::string sig_file = addr + "_" + ver + ".sig";
    while (true) {
        current_progress = 0; 
        std::cout << "📥 [DOWNLOAD] 펌웨어 및 서명 패키지 다운로드 다운링크 활성화..." << std::endl;
        
        // 두 전송 연산이 모두 무사히 true(완료 혹은 완전 스킵)를 뱉어야 탈출 조건 충족
        if (downloadFile(f_url, hex_file) && downloadFile(s_url, sig_file)) {
            std::cout << "✅ [DOWNLOAD SUCCESS] 패키지 무결성 조각 병합 100% 안착 완료!" << std::endl;
            current_progress = 100;
            break; 
        }
        
        // 여기에 걸렸다는 건 중간에 전송 소켓이 파괴되었거나, 피어가 다운되었다는 증거
        std::cerr << "⚠️ [DOWNLOAD INTERRUPT] 백엔드 데이터 전송 노드가 끊겼거나 닫혔습니다." << std::endl;
        std::cerr << "⏳ [자동 복구 지연] 5초 후 기존에 저장된 바이트 조각 끝점부터 자동으로 이어받기를 가동합니다...\n" << std::endl;
        
        // 커널 버퍼 비우기 시간 및 네트워크 어댑터 재정렬을 위해 5초 휴식 (CPU 소모 없음)
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    /* ========================= */
    /* VERIFICATION */
    /* ========================= */
    current_state = VERIFICATION;

    if (!verifyFirmwareSecurity(addr, ver, LOCAL_PUBLIC_KEY_PATH)) {
        current_state = REPORTING;
        reportStatusToServer(addr, ver, "AUTH_FAILED");
        return;
    }

    /* ========================= */
    /* INSTALL */
    /* ========================= */
    current_state = INSTALL;
    reportStatusToServer(addr, ver, "FLASHING");
    
    int result = startOtaTransfer(addr, ver, GATEWAY_IP);

    // State 11: REPORTING
    current_state = REPORTING;
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L); // 연결 제한 3초
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        try {
            auto res_json = json::parse(response_string);
            for (const auto& update : res_json["updates"]) {
                if (update.value("update", false)) {
                    // 업데이트할 목록이 있으면 Queue에 넣기
                    queue_mutex.lock();
                    update_queue.push({update["address"], update["version"], update["firmware_url"], update["signature_url"]});
                    queue_mutex.unlock();

                    current_state = READY;

                    //executeUpdate(update["addr"], update["ver"], update["firware_url"], update["signature_url"]);
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
                // 업데이트할 목록이 생으면 Queue에 넣기
                queue_mutex.lock();
                update_queue.push({addr, ver, data["firmware_url"], data["signature_url"]});
                queue_mutex.unlock();

                current_state = READY;

                //executeUpdate(update["addr"], update["ver"], update["firmware_url"], update["signature_url"]);
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
    current_state = IDLE; // State 2: IDLE
    curl_global_init(CURL_GLOBAL_ALL);

    performInitialSync();

    current_state = WAIT; // State 3: WAIT
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

// 업데이트 큐 검사하며 업데이트 지시를 내리는 스레드 함수
void* ota_worker_thread(void* arg)
{
    while (true)
    {
        if (current_state == IDLE || current_state == OFF) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if(current_state == PENDING) continue;
        queue_mutex.lock();

        if (update_queue.empty())
        {
            queue_mutex.unlock();

            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000)
            );

            continue;
        }

        UpdateItem item = update_queue.front();

        queue_mutex.unlock();

        executeUpdate(item.addr, item.ver, item.firmware_url, item.signature_url);

        // 사용자가 NO 누른 경우
        if (current_state == PENDING)
        {
            std::this_thread::sleep_for(
                std::chrono::seconds(1)
            );
            
            queue_mutex.lock();
            update_queue.pop();
            update_queue.push({item.addr, item.ver, item.firmware_url, item.signature_url});
            queue_mutex.unlock();
            
            continue;
        }

        // 성공/실패 완료된 경우만 제거
        queue_mutex.lock();

        if (!update_queue.empty())
        {
            update_queue.pop();
        }

        queue_mutex.unlock();

        if (update_queue.empty())
            current_state = WAIT;
        else
            current_state = PENDING;
    }

    return NULL;
}