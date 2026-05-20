#include <iostream>
#include <pthread.h>

#include "state.h"

extern void runOtaService();

STATE current_state = OFF;
STATE prev_state    = (STATE)-1;

extern "C"
{
    void* lcd_thread(void* arg);
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << "   RPi OTA Manager (HTTP + MQTT Mode)   " << std::endl;
    std::cout << "========================================" << std::endl;

    pthread_t lcd_tid;

    pthread_create(
        &lcd_tid,
        NULL,
        lcd_thread,
        NULL
    );

    pthread_detach(lcd_tid);

    runOtaService();

    return 0;
}