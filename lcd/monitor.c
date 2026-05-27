#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>
#include <sys/time.h>

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

static unsigned int last_left_time  = 0;
static unsigned int last_right_time = 0;
static unsigned int last_enter_time = 0;

typedef enum
{
    MENU_YES,
    MENU_NO

} MENU_STATE;

MENU_STATE current_menu = MENU_YES;
MENU_STATE prev_menu    = -1;

int current_download_progress = 0;
int current_install_progress = 0;
int prev_download_progress    = -1;
int prev_install_progress = -1;

char update_ecu_name[16];
char update_ecu_version[16];

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

            lcd_ready_screen(
                update_ecu_name,
                update_ecu_version,
                current_menu == MENU_YES
            );

            break;

        case PENDING: // 업데이트 대기 중 (READY에서 아니오를 누름)

            lcd_pending_screen();

            break;

        case DOWNLOAD:

            lcd_downloading_screen(current_download_progress);

            break;

        case INSTALL:
            lcd_flashing_screen(current_install_progress);
            break;

        case VERIFICATION:

            lcd_verify_screen();

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
    struct timeval tv;
    gettimeofday(&tv, NULL);

    unsigned int now = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    // 200ms 이내 중복 입력 무시
    if (now - last_left_time < 200)
        return;

    last_left_time = now;

    puts("LEFT");
    if (current_state != READY)
        return;

    current_menu = MENU_YES;
}

void right_button_interrupt()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    unsigned int now = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    // 200ms 이내 중복 입력 무시
    if (now - last_right_time < 200)
        return;

    last_right_time = now;

    puts("RIGHT");
    if (current_state != READY)
        return;

    current_menu = MENU_NO;
}

void enter_button_interrupt()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    unsigned int now = tv.tv_sec * 1000 + tv.tv_usec / 1000;

    // 200ms 이내 중복 입력 무시
    if (now - last_enter_time < 200)
        return;

    last_enter_time = now;

    puts("ENTER");

    if (current_state == PENDING)
    {
        current_state = READY;
    }

    else if (current_state == READY)
    {
        if (current_menu == MENU_YES)
        {
            current_state = DOWNLOAD;
        }
        else // current_menu == MENU_NO
        {
            current_state = PENDING;

            current_menu = MENU_YES;
        }
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


    render_screen();

    while (1)
    {
        if (prev_state    != current_state ||
            prev_menu     != current_menu  ||
            prev_download_progress != current_download_progress ||
            prev_install_progress != current_install_progress)
        {
            render_screen();

            prev_state    = current_state;
            prev_menu     = current_menu;
            prev_download_progress = current_download_progress;
            prev_install_progress = current_install_progress;
        }

        usleep(300000);
    }

    return NULL;
}