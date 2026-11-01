#pragma once
// Seeed XIAO ESP32-C6: GPIO3 belongs to the RF switch, NEVER to a potentiometer.
#define LAMP_RF_POWER_GPIO 3
#define LAMP_RF_SELECT_GPIO 14
#define LAMP_EXTERNAL_ANTENNA 0
#define LAMP_PAIR_GPIO 9
#define LAMP_UART_TX_GPIO 16 // D6 -> T-Display GPIO22 RX
#define LAMP_UART_RX_GPIO 17 // D7 <- T-Display GPIO21 TX
#define LAMP_ENDPOINT 1
#define LAMP_ZIGBEE_CHANNEL 20
#define LAMP_JOIN_SECONDS 180
#define LAMP_TRANSITION_DS 1
