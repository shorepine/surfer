#define MICROPY_HW_BOARD_NAME "surfer tulip (ESP32-P4-Function-EV)"
#define MICROPY_HW_MCU_NAME   "ESP32P4"

// no radio on the P4 itself (the WIFI variants use a hosted C5/C6)
#define MICROPY_PY_ESPNOW        (0)
#define MICROPY_PY_NETWORK_WLAN  (0)
#define MICROPY_PY_BLUETOOTH     (0)

#define MICROPY_HW_ENABLE_SDCARD (1)
#define MICROPY_HW_ENABLE_UART_REPL (1)

#ifndef USB_SERIAL_JTAG_PACKET_SZ_BYTES
#define USB_SERIAL_JTAG_PACKET_SZ_BYTES (64)
#endif
