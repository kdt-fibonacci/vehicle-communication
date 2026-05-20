#ifndef MONITOR_H
#define MONITOR_H

void lcd_idle_screen();

void lcd_ready_screen();

void lcd_update_screen(
    const char* ecu_name,
    const char* version,
    int yes_selected
);

void lcd_downloading_screen(int percent);

void lcd_verify_screen();

void lcd_success_screen();

void lcd_fail_screen();

#endif