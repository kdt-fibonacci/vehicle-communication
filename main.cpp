#include <iostream>
#include <pthread.h>
#include <queue>

#include "state.h"
#include "struct.h"

extern void runOtaService();
extern void* ota_worker_thread(void* arg);

extern "C"
{
    void* lcd_thread(void* arg);
}

STATE current_state = OFF;
STATE prev_state    = (STATE)-1;

std::queue<UpdateItem> update_queue; // 업데이트 목록을 저장할 큐

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "   RPi OTA Manager (HTTP + MQTT Mode)   " << std::endl;
    std::cout << "========================================" << std::endl;

    pthread_t lcd_tid;
    pthread_t ota_worker_tid;

    pthread_create(&lcd_tid, NULL, lcd_thread, NULL);
    pthread_detach(lcd_tid);

    pthread_create(&ota_worker_tid, NULL, ota_worker_thread, NULL);
    pthread_detach(ota_worker_tid);

    runOtaService();

    return 0;
}