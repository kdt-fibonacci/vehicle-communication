#include "lcd_ui.h"
#include "lcd_i2c_driver.h"

#include <stdio.h>

void lcd_idle_screen()
{
    lcd_clear();

    lcd_set_cursor(0, 0);
    lcd_print("NO UPDATE");

    lcd_set_cursor(1, 0);
    lcd_print("EVENT WAITING");
}

void lcd_pending_screen()
{
    lcd_clear();

    lcd_set_cursor(0, 0);
    lcd_print("UPDATE READY");

    lcd_set_cursor(1, 0);
    lcd_print("PRESS ENTER");
}

void lcd_ready_screen(
    const char* ecu_name,
    const char* version,
    int yes_selected
) {
    char line[17];

    lcd_clear();

    snprintf(
        line,
        sizeof(line),
        "%s v%s",
        ecu_name,
        version
    );

    lcd_set_cursor(0, 0);

    lcd_print(line);

    lcd_set_cursor(1, 0);

    if (yes_selected)
        lcd_print("[YES]   NO ");
    else
        lcd_print(" YES   [NO]");
}

void lcd_downloading_screen(int percent)
{
    char line[17];

    lcd_clear();

    lcd_set_cursor(0, 0);

    lcd_print("DOWNLOADING");

    snprintf(
        line,
        sizeof(line),
        "%d%%",
        percent
    );

    lcd_set_cursor(1, 0);

    lcd_print(line);
}

void lcd_flashing_screen(int percent)
{
    char line[17];

    lcd_clear();

    lcd_set_cursor(0, 0);

    lcd_print("FLASHING");

    snprintf(
        line,
        sizeof(line),
        "%d%%",
        percent
    );

    lcd_set_cursor(1, 0);

    lcd_print(line);
}

void lcd_verify_screen()
{
    lcd_clear();

    lcd_set_cursor(0, 0);

    lcd_print("VERIFY FILE");

    lcd_set_cursor(1, 0);

    lcd_print("PLEASE WAIT");
}

void lcd_success_screen()
{
    lcd_clear();

    lcd_set_cursor(0, 0);

    lcd_print("UPDATE OK");

    lcd_set_cursor(1, 0);

    lcd_print("REBOOT ECU");
}

void lcd_fail_screen()
{
    lcd_clear();

    lcd_set_cursor(0, 0);

    lcd_print("UPDATE FAIL");

    lcd_set_cursor(1, 0);

    lcd_print("TRY AGAIN");
}