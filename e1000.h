#ifndef E1000_H
#define E1000_H

struct e1000 {
  uint mmio_base;     // The base address of the cards MMIO region.
  char mac[6];        // The cards EEPROM configured MAC address.
  struct rx_desc *rx; // Page sized buffer holding recieve descriptors.
  uint rx_count;      // The number of receive descriptors allocated.
  char **rx_buf;      // List of page sized receive data buffers.
  char *tx;           // Page sized buffer holding transmit descriptors.
  uint packet_count;
};

void e1000_read();
void e1000_write(char *buf, uint size);

#endif
