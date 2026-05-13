#include <iostream>
#include <fstream>
#include <vector>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <iomanip>

const int DOIP_PORT = 13400;
const uint16_t RPI_SA = 0x0E00; 

struct DataChunk {
    uint32_t address;
    std::vector<uint8_t> data;
};

// --- [헬퍼 함수] ---
uint8_t hstob(const std::string& hex) { return (uint8_t)std::stoul(hex, nullptr, 16); }

uint32_t calculateChunkChecksum(const std::vector<uint8_t>& data) {
    uint32_t sum = 0;
    for (uint8_t b : data) sum += b;
    return sum;
}

// DoIP/UDS 패킷 송신
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

// 응답 분석 (NRC 추출)
int checkUdsResponse(uint8_t* res, int len, uint8_t expectedPositiveSid) {
    if (len < 13) {
        std::cout << "   [DEBUG] Error: Response too short or Timeout." << std::endl;
        return -1;
    }
    uint8_t sid = res[12];
    if (sid == expectedPositiveSid) {
        return 0; // 성공
    } else if (sid == 0x7F) {
        std::cout << "   [DEBUG] !!! Negative Response (NRC: 0x" << std::hex << (int)res[14] << std::dec << ") !!!" << std::endl;
        return res[14]; // NRC 번호 반환
    }
    return -2;
}

// --- [UDS 서비스 로직] ---

bool routingActivation(int sock) {
    uint8_t actReq[11] = {0x02, 0xFD, 0x00, 0x05, 0x00, 0x00, 0x00, 0x03, 0x0E, 0x00, 0x00};
    send(sock, actReq, sizeof(actReq), 0);
    uint8_t res[32];
    int len = recv(sock, res, sizeof(res), 0);
    return (len >= 8 && res[2] == 0x00 && res[3] == 0x06);
}

int enterProgrammingSession(int sock, uint16_t targetAddr) {
    std::cout << "[UDS] Entering Programming Session (0x10 03)..." << std::endl;
    sendUdsPacket(sock, targetAddr, 0x10, {0x03}); // 0x03: Extended/Programming
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

// --- [메인 엔진] ---
int startOtaTransfer(const std::string& targetAddrStr, const std::string& version, const std::string& gatewayIp) {
    uint16_t targetAddr = (uint16_t)std::stoul(targetAddrStr, nullptr, 16);
    std::string hexPath = targetAddrStr + "_" + version + ".hex";
    
    std::cout << "\n[ENGINE] Parsing HEX file: " << hexPath << std::endl;
    std::ifstream file(hexPath);
    if (!file.is_open()) { std::cerr << "[ERROR] Cannot open HEX file." << std::endl; return -1; }

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
            if (!chunks.empty() && chunks.back().address + chunks.back().data.size() == fullAddr) chunks.back().data.insert(chunks.back().data.end(), d.begin(), d.end());
            else chunks.push_back({fullAddr, d});
        } else if (type == 0x04) { baseAddr = (hstob(line.substr(9, 2)) << 24) | (hstob(line.substr(11, 2)) << 16); }
    }
    file.close();
    std::cout << "[ENGINE] Parsing complete. Total Sections: " << chunks.size() << std::endl;

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(DOIP_PORT);
    inet_pton(AF_INET, gatewayIp.c_str(), &serv_addr.sin_addr);

    std::cout << "[DOIP] Attempting TCP Connect to " << gatewayIp << "..." << std::endl;
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "[DOIP] TCP Connection Failed!" << std::endl;
        return -1;
    }
    std::cout << "[DOIP] TCP Connected. Sending Routing Activation..." << std::endl;

    if (!routingActivation(sock)) {
        std::cerr << "[DOIP] Routing Activation Failed (No Response)!" << std::endl;
        close(sock);
        return -1;
    }

    int nrc;
    if ((nrc = enterProgrammingSession(sock, targetAddr)) != 0) { close(sock); return nrc; }

    for (size_t i = 0; i < chunks.size(); ++i) {
        auto& chunk = chunks[i];
        std::cout << "\n--- Processing Section [" << i+1 << "/" << chunks.size() << "] ---" << std::endl;
        
        if ((nrc = requestDownload(sock, targetAddr, chunk.address, chunk.data.size())) != 0) { close(sock); return nrc; }
        if (chunk.data.empty()) {
            std::cout << "   [WARN] Section has no data. Skipping 0x36 transfer." << std::endl;
        } else {
            std::cout << "   [UDS] Starting 0x36 transfer. Total: " << chunk.data.size() << " bytes." << std::endl;
        }
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
                if (offset % 5120 == 0) std::cout << "   [UDS] Transmitting... (" << offset << "/" << chunk.data.size() << " bytes)" << std::endl;
            } else {
                std::cerr << "   [ERROR] 0x36 failed at offset " << offset << std::endl;
                close(sock); return transferRes;
            }
        }
        if ((nrc = exitTransfer(sock, targetAddr)) != 0) { close(sock); return nrc; }
        uint32_t checksum = calculateChunkChecksum(chunk.data);
        if ((nrc = verifyIntegrity(sock, targetAddr, checksum)) != 0) { close(sock); return nrc; }
    }

    std::cout << "\n✅ [ENGINE] All Sections Transferred and Verified Successfully!" << std::endl;
    close(sock);
    return 0;
}