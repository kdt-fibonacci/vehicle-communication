#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>

#include "monitor.h"
#include "lcd_i2c_driver.h"
#include "lcd_ui.h"
#include "state.h"

#define BTN_LEFT   17
#define BTN_RIGHT  27
#define BTN_ENTER  22

/* ========================= */
/* GLOBAL */
/* ========================= */

extern STATE current_state;
extern STATE prev_state;

typedef enum
{
    MENU_YES,
    MENU_NO

} MENU_STATE;

MENU_STATE current_menu = MENU_YES;
MENU_STATE prev_menu    = -1;

int current_progress = 0;
int prev_progress    = -1;

char update_ecu_name[16]    = "MOTOR";
char update_ecu_version[16] = "1.2";

/* ========================= */
/* 화면 렌더 */
/* ========================= */

void render_screen()
{
    switch (current_state)
    {
        case OFF:
            break;

        case IDLE:
            break;

        case WAIT: // MQTT 수신 대기 중 

            lcd_idle_screen();

            break;

        case READY: // 업데이트 준비 중 (예 /아니오 선택)

            lcd_update_screen(
                update_ecu_name,
                update_ecu_version,
                current_menu == MENU_YES
            );

            break;

        case PENDING: // 업데이트 대기 중 (READY에서 아니오를 누름)

            lcd_ready_screen();

            break;

        case DOWNLOAD:

            lcd_downloading_screen(
                current_progress
            );

            break;

        case VERIFICATION:

            lcd_verify_screen();

            break;

        case INSTALL:

            

            break;

        case WAIT_ACTIVATION:

            

            break;

        case ACTIVATION:
            
            break;
        
        case RECOVERY:
            break;
        
        case REPORTING:
            break;

        default:
            break;
    }
}


/* ========================= */
/* BUTTON ISR */
/* ========================= */

void left_button_interrupt()
{
    if (current_state != READY)
        return;

    current_menu = MENU_YES;
}

void right_button_interrupt()
{
    if (current_state != READY)
        return;

    current_menu = MENU_NO;
}

void enter_button_interrupt()
{
    if (current_state == PENDING)
    {
        current_state = READY;

        return;
    }

    if (current_state != READY)
        return;

    if (current_menu == MENU_YES)
    {
        current_state = DOWNLOAD;
    }
    else
    {
        current_state = PENDING;

        current_menu = MENU_YES;
    }
}

/* ========================= */
/* LCD THREAD */
/* ========================= */

void* lcd_thread(void* arg)
{
    lcd_driver_init();

    wiringPiSetupGpio();

    /* INPUT */
    pinMode(BTN_LEFT, INPUT);
    pinMode(BTN_RIGHT, INPUT);
    pinMode(BTN_ENTER, INPUT);

    /* INTERNAL PULL-UP */
    pullUpDnControl(BTN_LEFT, PUD_UP);
    pullUpDnControl(BTN_RIGHT, PUD_UP);
    pullUpDnControl(BTN_ENTER, PUD_UP);

    /* INTERRUPT */
    wiringPiISR(
        BTN_LEFT,
        INT_EDGE_FALLING,
        &left_button_interrupt
    );

    wiringPiISR(
        BTN_RIGHT,
        INT_EDGE_FALLING,
        &right_button_interrupt
    );

    wiringPiISR(
        BTN_ENTER,
        INT_EDGE_FALLING,
        &enter_button_interrupt
    );

    /* 초기 화면 */
    current_state = IDLE;

    render_screen();

    while (1)
    {
        if (prev_state    != current_state ||
            prev_menu     != current_menu  ||
            prev_progress != current_progress)
        {
            render_screen();

            prev_state    = current_state;
            prev_menu     = current_menu;
            prev_progress = current_progress;
        }

        usleep(300000);
    }

    return NULL;
}