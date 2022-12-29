use crate::net::FromBuffer;

/// An ethernet (MAC) address.
#[derive(Debug, Copy, Clone, Eq, Ord, PartialEq, PartialOrd)]
pub struct EthernetAddress([u8; 6]);

impl EthernetAddress {
    pub fn from_slice(buf: &[u8]) -> EthernetAddress {
        EthernetAddress([buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]])
    }

    pub fn as_bytes(&self) -> [u8; 6] {
        self.0
    }
}

/// The small subset of Ethertype values we care about.
#[derive(Debug, Copy, Clone)]
pub enum Ethertype {
    IPV4 = 0x0800,        // Internet Protocol version 4 (IPv4).
    ARP = 0x0806,         // Address Resolution Protocol (ARP).
    WAKE_ON_LAN = 0x0842, // Wake-on-LAN.
    RARP = 0x8035,        // Reverse Address Resolution Protocol (RARP).
    SLPP = 0x8103,        // Virtual Link Aggregation Control Protocol (VLACP).
    IPV6 = 0x86DD,        // Internet Protocol Version 6 (IPv6).
    UNKNOWN = 0xFFFF,
}

impl Ethertype {
    pub fn from_slice(buf: &[u8]) -> Ethertype {
        let mut raw: [u8; 2] = [0; 2];
        raw.clone_from_slice(&buf);
        match u16::from_be_bytes(raw) {
            0x0800 => Ethertype::IPV4,
            0x0806 => Ethertype::ARP,
            0x0842 => Ethertype::WAKE_ON_LAN,
            0x8035 => Ethertype::RARP,
            0x8103 => Ethertype::SLPP,
            0x86DD => Ethertype::IPV6,
            _ => Ethertype::IPV6,
        }
    }
}

/// An ethernet frame header.
///
/// Represents an Ethernet frame header.
#[derive(Debug)]
#[repr(C)]
pub struct EthernetFrame {
    destination: EthernetAddress,
    source: EthernetAddress,
    pub ethertype: Ethertype,
}

impl EthernetFrame {
    /// Creates a new EthernetHeader from a slice of bytes.
    pub fn from_slice(buf: &[u8]) -> EthernetFrame {
        EthernetFrame {
            destination: EthernetAddress::from_slice(&buf[0..6]),
            source: EthernetAddress::from_slice(&buf[6..12]),
            ethertype: Ethertype::from_slice(&buf[12..14]),
        }
    }
}

impl FromBuffer for EthernetFrame {
    fn from_buffer(buf: &[u8]) -> EthernetFrame {
        EthernetFrame::from_slice(&buf)
    }

    fn size(&self) -> usize {
        14
    }
}