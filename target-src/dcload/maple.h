/* Maple Bus command and response codes */

/* Written into the response header before every DMA cycle, so that "the
 * controller wrote nothing" is distinguishable from a real answer and from the
 * -1 the controller itself writes on a timeout. See maple.c. */
#define MAPLE_NO_REPLY         0xeeeeeeee
#define MAPLE_RESPONSE_NO_REPLY -18  /* 0xee as a signed response code */

#define MAPLE_RESPONSE_FILEERR -5
#define MAPLE_RESPONSE_AGAIN   -4  /* request should be retransmitted */
#define MAPLE_RESPONSE_BADCMD  -3
#define MAPLE_RESPONSE_BADFUNC -2
#define MAPLE_RESPONSE_NONE    -1  /* unit didn't respond at all */
#define MAPLE_COMMAND_DEVINFO  1
#define MAPLE_COMMAND_ALLINFO  2
#define MAPLE_COMMAND_RESET    3
#define MAPLE_COMMAND_KILL     4
#define MAPLE_RESPONSE_DEVINFO 5
#define MAPLE_RESPONSE_ALLINFO 6
#define MAPLE_RESPONSE_OK      7
#define MAPLE_RESPONSE_DATATRF 8
#define MAPLE_COMMAND_GETCOND  9
#define MAPLE_COMMAND_GETMINFO 10
#define MAPLE_COMMAND_BREAD    11
#define MAPLE_COMMAND_BWRITE   12
#define MAPLE_COMMAND_SETCOND  14


/* Function codes */

#define MAPLE_FUNC_CONTROLLER 0x001
#define MAPLE_FUNC_MEMCARD    0x002
#define MAPLE_FUNC_LCD        0x004
#define MAPLE_FUNC_CLOCK      0x008
#define MAPLE_FUNC_MICROPHONE 0x010
#define MAPLE_FUNC_ARGUN      0x020
#define MAPLE_FUNC_KEYBOARD   0x040
#define MAPLE_FUNC_LIGHTGUN   0x080
#define MAPLE_FUNC_PURUPURU   0x100


struct maple_devinfo {
  unsigned int func;              /* big endian! */
  unsigned int function_data[3];  /* big endian! */
  unsigned char area_code;
  unsigned char connector_direction;
  char product_name[30];
  char product_license[60];
  unsigned short standby_power;   /* little endian */
  unsigned short max_power;       /* little endian */
};


extern unsigned int g_maple_dma_timeouts;
extern unsigned int g_maple_dma_empty;

void maple_init(void);
void maple_wait_dma(void);
void *maple_docmd(int port, int unit, int cmd, int datalen, void *data);

#define MAPLE_DMA_SIZE (1024 + 1024 + 4 + 4 + 4)

extern volatile unsigned char *const dmabuffer;   /* fixed high RAM, see maple.c */
