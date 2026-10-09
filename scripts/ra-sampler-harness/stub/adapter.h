/* Host stand-ins for the loader headers ra.c includes. */
#ifndef STUB_ADAPTER_H
#define STUB_ADAPTER_H
typedef struct {
	unsigned char mac[6];
	unsigned char pad[2];
	int (*tx)(unsigned char *pkt, int len);
} adapter_t;
extern adapter_t *bb;
extern unsigned char *pkt_buf;
static inline unsigned int clk_since(unsigned int start) { (void)start; return 0; }
extern unsigned char tool_mac[6];
extern unsigned short tool_port;
extern unsigned int tool_ip;
#endif
