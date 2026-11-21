#ifndef REBANE_LINUX_X11_H
#define REBANE_LINUX_X11_H

/* One local X11 byte stream. Call with the kernel address space active. */
void linux_x11_reset(void);
int linux_x11_connect(const void *address, unsigned length);
long linux_x11_write(const void *data, unsigned long length);
long linux_x11_read(void *data, unsigned long length);
unsigned long linux_x11_available(void);
/* Composite retained X windows after desktop redraws, before display_present. */
void linux_x11_refresh(void);

#endif
