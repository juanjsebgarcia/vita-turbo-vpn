/*
 * tcp_rst.h - the TCP reset that answers a stranded connection (tcp_rst.c).
 */
#ifndef VT_TCP_RST_H
#define VT_TCP_RST_H

#include <stdint.h>

#define TCP_RST_LEN 40   /* IPv4 header + TCP header, no options */

/*
 * Build, in `out`, the reset the far end would send for the IPv4 packet
 * `ip` (`len` bytes, from the IP header): addresses and ports swapped,
 * sequence and acknowledgement per RFC 793, both checksums set. Returns 1,
 * or 0 if the packet is not a TCP segment that may be answered (not TCP,
 * malformed, a later fragment, or itself a reset).
 */
int tcp_rst_for(const uint8_t *ip, uint32_t len, uint8_t out[TCP_RST_LEN]);

#endif /* VT_TCP_RST_H */
