#include "packet.h"
#include "memfuncs.h"

/*
 * The Internet checksum, fast (shared by packet.c and the host-side test).
 *
 * The old loops added one 16-bit word at a time and tested for a carry after
 * each: ~5 cycles for 2 bytes, ~18 us over a 1440-byte payload, on every frame
 * of a disc read, inside the interrupt hook. A ones' complement sum does not
 * need the carries until the end: 2^16 = 1 (mod 65535), so the words may be
 * added as plain integers and folded once. A 32-bit accumulator holds the sum
 * of up to 65537 words (a UDP datagram has at most 32754).
 *
 * The bulk is read four bytes at a time when the buffer allows it and its two
 * halves are added apart. Byte order is the old one's: the words are read as
 * the CPU reads them, and the result is compared, or stored, the same way.
 */
static unsigned int csum_sum16(const unsigned short *p, unsigned int n)
{
	unsigned int s = 0;

	if (n >= 16u && !((unsigned int)p & 3u))
	{
		const unsigned int *w = (const unsigned int *)p;
		unsigned int lo = 0, hi = 0, blocks = n >> 3;

		n &= 7u;
		while (blocks--)
		{
			unsigned int a = w[0], b = w[1], c = w[2], d = w[3];

			lo += (a & 0xffffu) + (b & 0xffffu) + (c & 0xffffu) + (d & 0xffffu);
			hi += (a >> 16) + (b >> 16) + (c >> 16) + (d >> 16);
			w += 4;
		}
		s = lo + hi;
		p = (const unsigned short *)w;
	}
	while (n--)
	{
		s += *p++;
	}
	return s;
}

static unsigned short csum_fold(unsigned int s)
{
	s = (s & 0xffffu) + (s >> 16);
	s = (s & 0xffffu) + (s >> 16);
	return (unsigned short)~s;
}

// The two checksums here are different because the UDP one needs a "pseudo-header," while the IP one doesn't
unsigned short checksum(unsigned short *buf, int count, int is_odd)
{
	unsigned int sum = csum_sum16(buf, (unsigned int)count);

	if(is_odd)
	{
		sum += (unsigned short)(*((unsigned char*)(buf + count))); // The sum is a little-endian sum, so an odd byte will be an 8-bit int
	}

	return csum_fold(sum);
}

// Pass odd as length%2 where datacount is length/2.
unsigned short checksum_udp(unsigned short *buf_pseudo, unsigned short *buf_data, int datacount, int is_odd)
{
	unsigned int sum = csum_sum16(buf_pseudo, PSEUDO_H_LEN/2) + csum_sum16(buf_data, (unsigned int)datacount);

	if(is_odd)
	{
		sum += (unsigned short)(*((unsigned char*)(buf_data + datacount))); // The sum is a little-endian sum, so an odd byte will be an 8-bit int
	}

	return csum_fold(sum);
}

void make_ether(unsigned char *dest, unsigned char *src, ether_header_t *ether)
{
	memcpy_16bit(ether->dest, dest, 6/2);
	memcpy_16bit(ether->src, src, 6/2);
	ether->type[0] = 8;
	ether->type[1] = 0;
}

void make_ip(int dest, int src, int length, char protocol, ip_header_t *ip, unsigned short pkt_id)
{
	ip->version_ihl = 0x45;
	ip->tos = 0;
	ip->length = htons(20 + length);
	ip->packet_id = pkt_id;
	ip->flags_frag_offset = htons(0x4000);
	ip->ttl = 64; // 0x40 is a hop count of 64...
	ip->protocol = protocol;
	ip->checksum = 0;
	ip->src = htonl(src);
	ip->dest = htonl(dest);

	ip->checksum = checksum((unsigned short *)ip, IP_H_LEN/2, 0);
}

__attribute__((aligned(4))) unsigned char pseudo_array[PSEUDO_H_LEN]; // Here's a global array (not really global, but... search terms)

// UDP packet length should always be an even number. It's the length of the UDP payload data specified by the 'data' variable.
void make_udp(unsigned short dest, unsigned short src, int length, ip_header_t *ip, udp_header_t *udp)
{
	ip_udp_pseudo_header_t * pseudo = (ip_udp_pseudo_header_t*)pseudo_array;

	udp->src = htons(src);
	udp->dest = htons(dest);
	udp->length = htons(length + UDP_H_LEN);
	udp->checksum = 0;

	pseudo->src_ip = ip->src;
	pseudo->dest_ip = ip->dest;
	pseudo->zero = 0;
	pseudo->protocol = ip->protocol;
	pseudo->udp_length = udp->length;
	pseudo->src_port = udp->src;
	pseudo->dest_port = udp->dest;
	pseudo->length = udp->length;
	pseudo->checksum = 0;

	udp->checksum = checksum_udp((unsigned short *)pseudo, (unsigned short *)udp->data, length/2, length%2);
	if (udp->checksum == 0)
		udp->checksum = 0xffff;
}
