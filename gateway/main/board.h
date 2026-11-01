#pragma once
// Original TTGO/LILYGO T-Display, ESP32-D0WD, ST7789 135x240, SPI.
// This profile is NOT for T-Display-S3 (which uses a different bus and pinout).
#define BOARD_UART_TX 21
#define BOARD_UART_RX 22
#define BOARD_TFT_MOSI 19
#define BOARD_TFT_CLOCK 18
#define BOARD_TFT_CS 5
#define BOARD_TFT_DC 16
#define BOARD_TFT_RESET 23
#define BOARD_TFT_BACKLIGHT 4
#define BOARD_SCREEN_BUTTON 35
#define BOARD_PRESET_BUTTON 0
#define BOARD_IR_RX 25 // Demodulated, idle HIGH / active LOW, 3.3 V logic.
#define BOARD_IR_GND 26 // LOW: return for the low-current IR receiver only.
#define BOARD_IR_POWER 27 // HIGH: receiver supply, approximately 3.3 V.
#define BOARD_LCD_WIDTH 240
#define BOARD_LCD_HEIGHT 135
