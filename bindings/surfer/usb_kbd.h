/* Minimal USB-host HID boot-protocol keyboard for the tulip P4 port. */
#ifndef SURFER_USB_KBD_H
#define SURFER_USB_KBD_H

#include "surfer_port.h"

void surfer_usb_kbd_init(void);
bool surfer_usb_kbd_poll(surfer_key *out);
int  surfer_usb_kbd_held(surfer_key *out, int max);

/* short aliases used by port_p4.c */
#define usb_kbd_init surfer_usb_kbd_init
#define usb_kbd_poll surfer_usb_kbd_poll

#endif /* SURFER_USB_KBD_H */
