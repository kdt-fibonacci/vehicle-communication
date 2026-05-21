// ota_uds_engine.cpp
#include <iostream>
#include <fstream>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <iomanip>
#include <thread> 

#include "state.h"

extern STATE current_state;

const int DOIP_PORT = 13400;
const uint16_t RPI_SA = 0x0E00; 

struct DataChunk {
    uint32_t address;
    std::vector<uint8_t> data;
};

uint8_t hstob(const std::string& hex) { return (uint8_t)std::stoul(hex, nullptr, 16); }

uint32_t calculateChunkChecksum(const std::vector<uint8_t>& data) {
    uint32_t sum = 0;
    for (uint8_t b : data) sum += b;
    return sum;
}

void sendUdsPacket(int sock, uint16_t targetAddr, uint8_t sid, const std::vector<uint8_t>& payload) {
    uint32_t udsLen = payload.size() + 1;
    uint32_t doipPayloadLen = udsLen + 4;
    std::vector<uint8_t> pkt;
    pkt.push_back(0x02); pkt.push_back(0xFD);
    pkt.push_back(0x80); pkt.push_back(0x01);
    pkt.push_back(0x00); pkt.push_back(0x00);
    pkt.push_back((doipPayloadLen >> 8) & 0xFF); pkt.push_back(doipPayloadLen & 0xFF);
    pkt.push_back((RPI_SA >> 8) & 0xFF); pkt.push_back(RPI_SA & 0xFF);
    pkt.push_back((targetAddr >> 8) & 0xFF); pkt.push_back(targetAddr & 0xFF);
    pkt.push_back(sid);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    send(sock, pkt.data(), pkt.size(), 0);
}

int checkUdsResponse(uint8_t* res, int len, uint8_t expectedPositiveSid, uint16_t expectedRid = 0) {
    if (len < 13) return -100;
    uint8_t sid = res[12];

    if (sid == expectedPositiveSid) {
        if (expectedRid != 0) {
            uint16_t receivedRid = (res[14] << 8) | res[15];
            if (receivedRid != expectedRid) return -101;
        }
        return 0;
    } 
    else if (sid == 0x7F) {
        uint8_t nrc = res[14];
        if (nrc == 0x78) return 0x78;
        return nrc;
    }
    return -102;
}

bool routingActivation(int sock) {
    uint8_t actReq[11] = {0x02, 0xFD, 0x00, 0x05, 0x00, 0x00, 0x00, 0x03, 0x0E, 0x00, 0x00};
    send(sock, actReq, sizeof(actReq), 0);
    uint8_t res[32];
    int len = recv(sock, res, sizeof(res), 0);
    return (len >= 8 && res[2] == 0x00 && res[3] == 0x06);
}

int enterProgrammingSession(int sock, uint16_t targetAddr) {
    std::cout << "[UDS] Entering Programming Session (0x10 03)..." << std::endl;
    sendUdsPacket(sock, targetAddr, 0x10, {0x03});
    uint8_t res[64];
    int len = recv(sock, res, sizeof(res), 0);
    return checkUdsResponse(res, len, 0x50);
}

int changeDiagnosticSession(int sock, uint16_t targetAddr, uint8_t sessionType) {
    std::string sessionName = (sessionType == 0x02) ? "Programming Session (0x02)" : "Extended Session (0x03)";
    std::cout << "[UDS] Requesting Session Switch to " << sessionName << "..." << std::endl;
    
    sendUdsPacket(sock, targetAddr, 0x10, { sessionType });
    uint8_t res[64];
    int len = recv(sock, res, sizeof(res), 0);
    return checkUdsResponse(res, len, 0x50);
}

int requestDownload(int sock, uint16_t targetAddr, uint32_t addr, uint32_t size) {
    std::cout << "[UDS] Requesting Download (0x34) to Addr: 0x" << std::hex << addr << std::dec << std::endl;
    std::vector<uint8_t> p = { 0x00, 0x44, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size };
    sendUdsPacket(sock, targetAddr, 0x34, p);
    uint8_t res[64];
    int len = recv(sock, res, sizeof(res), 0);
    return checkUdsResponse(res, len, 0x74);
}

int exitTransfer(int sock, uint16_t targetAddr) {
    std::cout << "[UDS] Transfer Exit (0x37)..." << std::endl;
    sendUdsPacket(sock, targetAddr, 0x37, {});
    uint8_t res[64];
    int len = recv(sock, res, sizeof(res), 0);
    return checkUdsResponse(res, len, 0x77);
}

int verifyIntegrity(int sock, uint16_t targetAddr, uint32_t checksum) {
    std::cout << "[UDS] Verifying Integrity (0x31) Checksum: 0x" << std::hex << checksum << std::dec << std::endl;
    std::vector<uint8_t> p = { 0x01, 0xFF, 0x01, (uint8_t)(checksum >> 24), (uint8_t)(checksum >> 16), (uint8_t)(checksum >> 8), (uint8_t)checksum };
    sendUdsPacket(sock, targetAddr, 0x31, p);
    uint8_t res[64];
    int len = recv(sock, res, sizeof(res), 0);
    return checkUdsResponse(res, len, 0x71);
}

int requestBankSwap(int sock, uint16_t targetAddr) {
    std::cout << "\n[UDS] Target Activation Phase. Sending A/B Bank Swap (0x31 01 FF 02)..." << std::endl;
    std::vector<uint8_t> p = { 0x01, 0xFF, 0x02 }; 
    sendUdsPacket(sock, targetAddr, 0x31, p);
    
    uint8_t res[64];
    int len = recv(sock, res, sizeof(res), 0);
    return checkUdsResponse(res, len, 0x71);
}

int startOtaTransfer(const std::string& targetAddrStr, const std::string& version, const std::string& gatewayIp) {
    uint16_t targetAddr = (uint16_t)std::stoul(targetAddrStr, nullptr, 16);
    std::string hexPath = targetAddrStr + "_" + version + ".hex";
    
    std::cout << "\n[ENGINE] Parsing HEX file: " << hexPath << std::endl;
    std::ifstream file(hexPath);
    if (!file.is_open()) { std::cerr << "[ERROR] Cannot open HEX file." << std::endl; return -1; }

    // return 0; // 임시로 바로 통과되도록

    std::vector<DataChunk> chunks;
    uint32_t baseAddr = 0; std::string line;
    while (std::getline(file, line)) {
        if (line[0] != ':') continue;
        uint8_t len = hstob(line.substr(1, 2));
        uint16_t offset = (hstob(line.substr(3, 2)) << 8) | hstob(line.substr(5, 2));
        uint8_t type = hstob(line.substr(7, 2));
        if (type == 0x00) {
            uint32_t fullAddr = baseAddr + offset;
            std::vector<uint8_t> d;
            for (int i = 0; i < len; i++) d.push_back(hstob(line.substr(9 + (i * 2), 2)));
            if (!chunks.empty() && chunks.back().address + chunks.back().data.size() == fullAddr) 
                chunks.back().data.insert(chunks.back().data.end(), d.begin(), d.end());
            else chunks.push_back({fullAddr, d});
        } else if (type == 0x04) { 
            baseAddr = (hstob(line.substr(9, 2)) << 24) | (hstob(line.substr(11, 2)) << 16); 
        }
    }
    file.close();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DOIP_PORT);
    inet_pton(AF_INET, gatewayIp.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "[DOIP] TCP Connection Failed!" << std::endl;
        return -1;
    }
    if (!routingActivation(sock)) {
        std::cerr << "[DOIP] Routing Activation Failed!" << std::endl;
        close(sock); return -1;
    }

    std::cout << "\n[INSTALL Phase] Executing Flashing via UDS..." << std::endl;
    int nrc;

    if ((nrc = changeDiagnosticSession(sock, targetAddr, 0x03)) != 0) {
        std::cerr << "[ERROR] Failed to enter Extended Session (0x03)" << std::endl;
        close(sock); return nrc;
    }

    for (size_t i = 0; i < chunks.size(); ++i) {
        auto& chunk = chunks[i];
        if ((nrc = requestDownload(sock, targetAddr, chunk.address, chunk.data.size())) != 0) { close(sock); return nrc; }
        
        uint8_t sn = 1;
        uint32_t offset = 0;
        while (offset < chunk.data.size()) {
            uint32_t currLen = (chunk.data.size() - offset > 1024) ? 1024 : (chunk.data.size() - offset);
            std::vector<uint8_t> udsPayload = { sn };
            udsPayload.insert(udsPayload.end(), chunk.data.begin() + offset, chunk.data.begin() + offset + currLen);
            
            sendUdsPacket(sock, targetAddr, 0x36, udsPayload);
            uint8_t res_buf[1500];
            int rLen = recv(sock, res_buf, sizeof(res_buf), 0);
            int transferRes = checkUdsResponse(res_buf, rLen, 0x76);
            
            if (transferRes == 0) {
                offset += currLen;
                sn = (sn == 0xFF) ? 0x00 : sn + 1;
            } else { close(sock); return transferRes; }
        }
        if ((nrc = exitTransfer(sock, targetAddr)) != 0) { close(sock); return nrc; }
        
        uint32_t checksum = calculateChunkChecksum(chunk.data);
        if ((nrc = verifyIntegrity(sock, targetAddr, checksum)) != 0) { close(sock); return nrc; }
    }

    std::cout << "\n✅ Data flashing completed in background." << std::endl;

    // State 8: WAIT_ACTIVATION
current_state = WAIT_ACTIVATION; // state.h 전역 변수 동기화
    
    bool safeStateAchieved = false;
    int retryCounter = 0;
    const int MAX_RETRIES = 120; // 3초 간격으로 최대 120번 수행 = 총 6분 대기

    std::cout << "\n[WAIT] Vehicle data verified. Monitoring vehicle for Safe State (Stop & Gear P)..." << std::endl;

    while (!safeStateAchieved && retryCounter < MAX_RETRIES) {
        // Programming Session(0x10 02) 권한 격상 요청을 통한 정차 상태 감지 폴링
        nrc = changeDiagnosticSession(sock, targetAddr, 0x02);

        if (nrc == 0) {
            // ① 대기 탈출 성공: ECU가 수락(0x50)함 -> 차가 완벽히 안전 정차 상태 도달!
            std::cout << "✅ [UDS] Safe State Confirmed by ECU. Programming Session (0x10 02) Opened!" << std::endl;
            safeStateAchieved = true;
        } 
        else if (nrc == 0x22) {
            // ② 대기 유지: ECU가 거부(0x7F 10 22 ConditionsNotCorrect)함 -> 차가 아직 주행 중임!
            std::cout << "⚠️ [UDS] ECU Response: Conditions Not Correct (0x22). Vehicle is moving. Retrying in 3 seconds... [" 
                      << retryCounter + 1 << "/" << MAX_RETRIES << "]" << std::endl;
            
            retryCounter++;
            std::this_thread::sleep_for(std::chrono::seconds(3)); // 소켓 유지한 채 3초 대기
        } 
        else {
            // ③ 치명적 에러: 다른 규격 에러 발생 시 기능안전(Functional Safety)을 위해 탈출 및 예외처리
            std::cerr << "❌ [CRITICAL] Unexpected UDS Session Error: 0x" << std::hex << nrc << std::dec << std::endl;
            close(sock); return nrc;
        }
    }

    // 타임아웃 예외 처리 (장시간 주행으로 정차하지 않은 경우 활성화 연기)
    if (!safeStateAchieved) {
        std::cerr << "❌ [TIMEOUT] Vehicle did not enter safe state within timeout. Postponing activation." << std::endl;
        close(sock); 
        return 0x22; // 호출부 상위 레이어로 거부 코드 반환하여 주행 우선순위 보장
    }

    // 4단계: 최고 보안 등급(0x10 02) 도달 확인 후 원자적 A/B 뱅크 스왑 최종 실행
    current_state = ACTIVATION; // state.h 전역 변수 동기화
    int swapResult = requestBankSwap(sock, targetAddr);

    if (swapResult != 0) {
        // 리커버리 상태 진입 (뱅크 스왑 시퀀스 실패 시 기존 오리지널 뱅크 유지)
        current_state = RECOVERY;
        std::cerr << "⚠️ [RECOVERY] Critical error during bank swap. Rolled back to stable bank." << std::endl;
        close(sock); return swapResult;
    }

    std::cout << "🚀 [SUCCESS] Bank swap command accepted. Target ECU is rebooting with new firmware..." << std::endl;
    close(sock);
    return 0;
}