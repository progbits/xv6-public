//
// PCI system calls
//

#include "types.h"
#include "x86.h"
#include "defs.h"
#include "memlayout.h"
#include "traps.h"
#include "mmu.h"
#include "eth.h"

// PCI Constants.
const short PCI_CONFIG_ADDR = 0xCF8;
const short PCI_CONFIG_DATA = 0xCFC;

// Device constants.
const ushort VENDOR_ID = 0x8086; // Intel
const ushort DEVICE_ID = 0x100E; // 82540EM Gigabit Ethernet Controller

// E1000 registers.
const uint CTRL = 0x00000;
const uint STATUS = 0x00008;
const ushort EERD = 0x0014;
const uint ICR = 0x000C0;
const uint IMS = 0x000D0;
const uint RCTL = 0x00100;
const uint TIPG = 0x00410;
const uint RDBAL = 0x02800;
const uint RDBAH = 0x02804;
const uint RDLEN = 0x02808;
const uint RDH = 0x02810;
const uint RDT = 0x02818;
const uint TDFPC = 0x03430;
const uint TDBAL = 0x03800;
const uint TDBAH = 0x03804;
const uint TDLEN = 0x03808;
const uint TDH = 0x03810;
const uint TDT = 0x03818;
const uint TCTL = 0x00400;
const uint GPTC = 0x04080;
const uint TPT = 0x040D4;
const uint RAL = 0x05400;
const uint RAH = 0x05404;
const uint MTA_LOW = 0x05200;
const uint MTA_HIGH = 0x053FC;
const uint PBM_START = 0x10000;

// Ethernet constants.
// TODO - Should be in eth.h.
const ushort ETH_TYPE_IPV4 = 0x0800;
const ushort ETH_TYPE_IPV6 = 0x86DD;
const ushort ETH_TYPE_ARP = 0x0806;

// Represents the receive descriptor.
//
// Section 3.2.3 Receive Descriptor Format.
struct rx_desc {
  uint addr[2];
  uint fields[2];
} rx_desc;

static struct e1000 {
  uint mmio_base;     // The base address of the cards MMIO region.
  char mac[6];        // The cards EEPROM configured MAC address.
  struct rx_desc *rx; // Page sized buffer holding recieve descriptors.
  uint rx_count;      // The number of receive descriptors allocated.
  char **rx_buf;      // List of page sized receive data buffers.
  char *tx;           // Page sized buffer holding transmit descriptors.
  uint packet_count;
} e1000;

// Read a main function register.
uint read_reg(uint reg) { return *(uint *)(e1000.mmio_base + reg); }

// Write a main function register.
void write_reg(uint reg, uint value) {
  *(uint *)(e1000.mmio_base + reg) = value;
}

// Initialize an E1000 family ethernet card.
//
// By the end of this method, if successful, we will have:
//
//  - Located an attached Intel 8254x family ethernet card
//  - Stored the MMIO base address
//  - Stored the EEPROM based MAC address
//  - Configured the card as a bus master
//  - Setup receive functions
//  - Setup transmit functions
//  - Setup interrupts
//
// When reading the PCI configuration space, It is assumed that the memory
// mapped address is held in the first BAR register.
void e1000init() {
  const uint EEPROM_DONE = 0x00000010;

  // Because we tightly control the environment, assume that the ethernet
  // controller is on one of the first 4 pci devices on the first bus.
  int target_dev = -1; // Set when the target device is found.
  for (int dev = 0; dev < 4; dev++) {
    const uint addr = 0x80000000 | dev << 11;

    ushort vendor_id = 0x0;
    for (int i = 1; i >= 0; i--) {
      outdw(PCI_CONFIG_ADDR, addr | i);
      uchar data = inb(PCI_CONFIG_DATA);
      vendor_id = vendor_id | (data << (i * 8));
    }

    ushort device_id = 0x0;
    for (int i = 3, j = 1; i >= 2; i--, j--) {
      outdw(PCI_CONFIG_ADDR, addr | i);
      uchar data = inb(PCI_CONFIG_DATA);
      device_id = device_id | (data << (j * 8));
    }

    // Check vendor and device identifiers.
    if (vendor_id == VENDOR_ID && device_id == DEVICE_ID) {
      target_dev = dev;
      break;
    }
  }

  if (target_dev == -1) {
    // Failed to find an 8254x family card.
    return 0;
  }

  // Read the current command register, set the bus master bit and write back
  // the command register.
  uint command = 0x0;
  for (int i = 5, j = 1; i >= 4; i--, j--) {
    outdw(PCI_CONFIG_ADDR, (0x80000000 | target_dev << 11) | i);
    uchar data = inb(PCI_CONFIG_DATA);
    command = command | (data << (j * 8));
  }
  command |= 1 << 2;
  outdw(PCI_CONFIG_ADDR, (0x80000000 | target_dev << 11) | 4);
  outdw(PCI_CONFIG_DATA, command);

  // Assume the address we want is in the first BAR register.
  uint mmio_addr = 0x0;
  for (int i = 19, j = 3; i >= 16; i--, j--) {
    outdw(PCI_CONFIG_ADDR, (0x80000000 | target_dev << 11) | i);
    uchar data = inb(PCI_CONFIG_DATA);
    mmio_addr = mmio_addr | (data << (j * 8));
  }

  if (mmio_addr == 0) {
    panic("failed to determine base address");
  }
  e1000.mmio_base = mmio_addr;

  // MAC address is stored in first 6 bytes of EEPROM.
  for (int i = 0; i < 3; i++) {
    volatile uint addr = e1000.mmio_base + EERD;
    *(uint *)(addr) = 0x00000001 | i << 8;
    uint result = 0x0;
    while (!(result & EEPROM_DONE)) {
      result = *(uint *)(addr);
    };
    ushort part = (*(uint *)(addr)) >> 16;
    memcpy(e1000.mac + i * sizeof(ushort), &part, sizeof(ushort));
  }

  init_rx();
  init_tx();
  init_intr();
  ioapicenable(IRQ_PCI0, 0);
}

// Recieve initialization.
//
// Reference: Manual - Section 14.4
//
// - Program recieve address registers with MAC address.
// - Zero out the multicast table array.
// - Allocate a buffer to hold recieve descriptors.
// - Setup the recieve controler register.
void init_rx() {
  // Write MAC address.
  uint mac_low = 0x0;
  uint mac_high = 0x0;
  memcpy(&mac_low, e1000.mac, 4);
  memcpy(&mac_high, e1000.mac + 4, 2);
  write_reg(RAL, mac_low);
  write_reg(RAH, mac_high);

  // Recieve descriptor buffer should be 16B aligned. Its page aligned,
  // so this is fine.
  e1000.rx = kalloc();
  if (e1000.rx == 0) {
    panic("failed to allocate recieve descriptor buffer\n");
  }
  memset(e1000.rx, 0, PGSIZE);

  e1000.rx_count = PGSIZE / sizeof(struct rx_desc);

  // Setup the recieve descriptor buffer registers.
  write_reg(RDBAL, V2P(e1000.rx));
  write_reg(RDBAH, 0x0);
  write_reg(RDLEN, PGSIZE);
  write_reg(RDH, 0);
  write_reg(RDT, 0);

  // Allocate the receive data buffer list and then for each receive descriptor,
  // allocate a data buffer and write the descriptor.
  e1000.rx_buf = kalloc();
  for (uint i = 0; i < e1000.rx_count; i++) {
    e1000.rx_buf[i] = kalloc();
    if (e1000.rx_buf[i] == 0) {
      panic("failed to allocate buffer\n");
    }
    memset(e1000.rx_buf[i], 0, PGSIZE);

    // Write the descriptor for the buffer.
    struct rx_desc desc = {
        .addr = V2P(e1000.rx_buf[i]),
    };
    memcpy(&e1000.rx[i], &desc, sizeof(struct rx_desc));
  }
  write_reg(RDT, e1000.rx_count - 1); // One past last valid descriptor.

  // Setup the recieve control register (RCTL).
  uint rctl_reg = 0x0;
  rctl_reg |= (1 << 1);  // Reciever enable.
  rctl_reg |= (1 << 2);  // Store bad packets.
  rctl_reg |= (1 << 3);  // Recieve all unicast packets.
  rctl_reg |= (1 << 4);  // Recieve all multicast packets.
  rctl_reg |= (1 << 5);  // Recieve long packets.
  rctl_reg |= (1 << 15); // Accept broadcast packets.
  rctl_reg |= (3 << 16); // Buffer size (4096 bytes).
  rctl_reg |= (1 << 25); // Buffer size extension.
  write_reg(RCTL, rctl_reg);
}

// Transmission initialization.
//
// Reference: Manual - Section 14.5
//
// - Allocate a buffer to hold transmission descriptors.
// - Initialize the transmit descriptor buffer registers.
// - Setup the transmission control register.
// - Setup the transmission inter-packet gap register.
void init_tx() {
  // Transmit buffer should be 16B aligned. Its page aligned,
  // so this is fine
  e1000.tx = kalloc();
  if (e1000.tx == 0) {
    panic("failed to allocate transmission buffer\n");
  }
  memset(e1000.tx, 0, PGSIZE);

  // Setup the transmit descriptor buffer registers.
  write_reg(TDBAL, V2P(e1000.tx));
  write_reg(TDBAH, 0x0);
  write_reg(TDLEN, PGSIZE);
  write_reg(TDH, 0);
  write_reg(TDT, 0);

  // Setup the transmission control TCTL register.
  uint tctl_reg = 0x0;
  tctl_reg |= (1 << 1);
  tctl_reg |= (1 << 3);
  tctl_reg |= ((0xF) << 4);    // Collision threshold.
  tctl_reg |= ((0x200) << 12); // Collision distance.
  write_reg(TCTL, tctl_reg);

  // Setup the transmission inter-packet gap (TIPG) register.
  write_reg(TIPG, 0xA);
}

// Initialize interrupts.
void init_intr() {
  // Enable transmit descriptor write-back and recieve timer interrupts.
  write_reg(IMS,
            (1 << 0) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7));
}

// Main interrupt handler.
void e1000_intr() {
  const uint TXDW = 1 << 0;
  const uint RXT0 = 1 << 7;

  // Read the interrupt register and dispatch to the correct handler.
  const uint mask = read_reg(ICR);
  if (mask & TXDW) {
    cprintf("transmit descriptor writeback\n", mask);
  } else if (mask & RXT0) {
    read_packets();
  }
}

// Dump all incoming packets.
void read_packets() {
  const uint DD = 1 << 0;
  const uint EOP = 1 << 1;

  // Read the available descriptors.
  const uint tail = read_reg(RDT);
  const uint head = read_reg(RDH);
  uint i = tail % (e1000.rx_count - 1);
  uint n = head > tail ? head - tail : (e1000.rx_count - tail - 1) + head;
  for (uint j = 0; j < n; j++) {
    const uint idx = (i + j) % (e1000.rx_count - 1);
    const struct rx_desc desc = e1000.rx[idx];
    const uint packet_size = desc.fields[0] & 0xFF;
    const uint end_of_packet = ((desc.fields[1]) & EOP) >> 1;
    const char *buffer = (char *)(P2V(desc.addr[0]));

    cprintf("packet count: %d\n", e1000.packet_count);
    cprintf("buffer: 0x%x\n", buffer);
    cprintf("packet size: %d\n", desc.fields[0] & 0xFF);
    cprintf("end of packet: %d\n", ((desc.fields[1]) & EOP) >> 1);

    if (end_of_packet) {
      // Read the ethernet header.
      uint offset = 0;
      struct eth_hdr hdr;
      eth_hdr_from_buf(&hdr, buffer);
      offset += sizeof(struct eth_hdr);
      switch (hdr.ether_type) {
      case ETH_TYPE_IPV4: {
        cprintf("ETH_TYPE_IPV4\n");
        break;
      }
      case ETH_TYPE_IPV6: {
        cprintf("ETH_TYPE_IPV6\n");
        break;
      }
      case ETH_TYPE_ARP: {
        cprintf("ETH_TYPE_ARP\n");
        struct arp_packet packet;
        arp_packet_from_buf(&packet, buffer + offset);
        dump_arp_packet(&packet);
        offset += sizeof(struct arp_packet);
        break;
      }
      default: {
        cprintf("ETH_TYPE_UNKNOWN\n");
        break;
      }
      }
      dump_eth_hdr(&hdr);
    }
    cprintf("\n\n");
    ++e1000.packet_count;
  }

  write_reg(RDT, head == 0 ? e1000.rx_count - 1 : head - 1);
}