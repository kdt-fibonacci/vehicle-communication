#include "lcd_i2c_driver.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

#define I2C_ADDR 0x27

#define LCD_BACKLIGHT 0x08
#define ENABLE         0x04

static int lcd_fd;

/* ========================= */
/* LOW LEVEL */
/* ========================= */

void lcd_write_byte(char data)
{
    write(lcd_fd, &data, 1);
}

void lcd_toggle_enable(char data)
{
    lcd_write_byte(data | ENABLE);

    usleep(500);

    lcd_write_byte(data & ~ENABLE);

    usleep(500);
}

void lcd_send_nibble(char nibble, char mode)
{
    char data;

    data = nibble | mode | LCD_BACKLIGHT;

    lcd_write_byte(data);

    lcd_toggle_enable(data);
}

void lcd_send_byte(char value, char mode)
{
    char high = value & 0xF0;
    char low  = (value << 4) & 0xF0;

    lcd_send_nibble(high, mode);
    lcd_send_nibble(low, mode);
}

void lcd_command(char cmd)
{
    lcd_send_byte(cmd, 0x00);
}

void lcd_data(char data)
{
    lcd_send_byte(data, 0x01);
}

/* ========================= */
/* PUBLIC */
/* ========================= */

void lcd_print(const char* str)
{
    while (*str)
    {
        lcd_data(*str++);
    }
}

void lcd_set_cursor(int row, int col)
{
    int addr;

    if (row == 0)
        addr = 0x80 + col;
    else
        addr = 0xC0 + col;

    lcd_command(addr);
}

void lcd_clear()
{
    lcd_command(0x01);

    usleep(3000);
}

void lcd_driver_init()
{
    lcd_fd = open("/dev/i2c-1", O_RDWR);

    if (lcd_fd < 0)
    {
        perror("open");

        return;
    }

    if (ioctl(lcd_fd, I2C_SLAVE, I2C_ADDR) < 0)
    {
        perror("ioctl");

        return;
    }

    usleep(50000);

    lcd_send_nibble(0x30, 0);
    usleep(5000);

    lcd_send_nibble(0x30, 0);
    usleep(5000);

    lcd_send_nibble(0x30, 0);
    usleep(5000);

    lcd_send_nibble(0x20, 0);
    usleep(5000);

    lcd_command(0x28);
    lcd_command(0x0C);
    lcd_command(0x01);

    usleep(5000);
}