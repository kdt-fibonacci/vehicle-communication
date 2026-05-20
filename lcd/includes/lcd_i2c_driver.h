#ifndef LCD_I2C_DRIVER_H
#define LCD_I2C_DRIVER_H

void lcd_driver_init();

void lcd_clear();

void lcd_set_cursor(int row, int col);

void lcd_print(const char* str);

#endif