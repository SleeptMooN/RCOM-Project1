// Application layer protocol header.
// NOTE: This file must not be changed.

#ifndef APPLICATION_LAYER_H
#define APPLICATION_LAYER_H

#include "link_layer.h"

// Application layer main function.
// Arguments:
//   serialPort: Serial port name (e.g., /dev/ttyS0).
//   role: Application role {"tx", "rx"}.
//   baudrate: Baudrate of the serial port.
//   nTries: Maximum number of frame retries.
//   timeout: Frame timeout.
//   filename: Name of the file to send / receive.
void
applicationLayer(const char *serialPort, const char *role, int baudRate, int nTries, int timeout, const char *filename);

int getControlPacket(char *filename, int start, unsigned char *packet);

int getDataPacket(unsigned char *bytes, unsigned char *packet, int nSequence, int nBytes);

#endif // APPLICATION_LAYER_H
