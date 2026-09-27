
/*
 * Simple Maple Bus implementation
 *
 * NOTE: The functions here are designed for simplicity, not
 *       efficiency.  For good performance, requests should be
 *       parallelized (each DMA burst can contain one message to
 *       each device), and interrupts should be used to detect
 *       DMA completion rather than the busy-polling shown here.
 */

#include "maple.h"
//#include <string.h>
#include "memfuncs.h"

#define MAPLE(x) (*((volatile unsigned long *)(0xa05f6c00+(x))))


/*
 * Initialize the Maple Bus to reasonable defaults.
 * No end-DMA interrupts are registered.
 */
void maple_init()
{
  /* Reset hardware */
  MAPLE(0x8c) = 0x6155404f;
  MAPLE(0x10) = 0;
  /* Select 2Mbps bitrate, and a timeout of 50000 */
  MAPLE(0x80) = (50000<<16)|0;
  /* Enable bus */
  MAPLE(0x14) = 1;
}


/*
 * Wait for Maple DMA to finish.
 *
 * BOUNDED (AGENTS.md 4.8: every hardware wait is). This used to spin forever
 * on a bit the Maple controller owns, in a function the command loop calls
 * with the network standing still -- a device that wedged the bus took the
 * loader with it, with no counter to say so.
 *
 * The limit is deliberately huge: a Maple cycle is tens of microseconds and
 * the controller's own timeout is 50000 of its ticks, so nothing healthy comes
 * close. `g_maple_dma_timeouts` is the only thing that can tell a wedged bus
 * from a quiet one.
 */
#define MAPLE_DMA_SPIN_LIMIT 2000000

/* How many times to run a cycle that produced nothing. See maple_docmd(). */
#define MAPLE_DMA_TRIES 3

/*
 * How long to wait for the ANSWER, once the controller reports idle.
 *
 * The busy bit is not a completion signal you can poll immediately after the
 * trigger: it may not be set yet, and then the wait above returns at once with
 * the buffer untouched. KOS never has to care -- it takes the DMA completion
 * interrupt (maple_dma_irq_hnd) and gates the next burst on dma_in_progress --
 * but this driver polls, so it needs a positive signal of its own. The answer
 * itself is that signal, and it always comes: when no device is there the
 * controller writes -1 into the buffer when its own 50000-tick timeout expires.
 */
#define MAPLE_ANSWER_SPIN_LIMIT 2000000

unsigned int g_maple_dma_timeouts = 0;
unsigned int g_maple_dma_empty = 0;

void maple_wait_dma()
{
  unsigned int spins = MAPLE_DMA_SPIN_LIMIT;

  while((MAPLE(0x18) & 1) && --spins)
    ;

  if(!spins)
    g_maple_dma_timeouts++;
}


/* Since we're only going to do one request at a time in this
   simple design, the buffer need only to be large enough to
   hold one maximal request frame (1024 bytes), one maximal
   response frame (1024 bytes), and the two control longwords
   and header for the single transfer. In addition, DMA
   addresses need to be aligned to a 32 byte boundary.
*/

// Make GCC 32-byte align it in the .data section since GCC aligns the binary relative to 0x8c010000.
/*
 * OUT OF THE LOW IMAGE ON PURPOSE.
 *
 * This used to be a 2 KB array in dcload's BSS, and it sat exactly where Sonic
 * Adventure puts a stack: the title enters GD syscalls with SP = 0x8c00b9d0,
 * which landed inside this buffer. Its stack then grows DOWN through the rest
 * of dcload's state -- including `bb`, the adapter pointer -- and once that is
 * clobbered dcload's next bb->loop() jumps through garbage, which is how the
 * guest ends up executing at address zero.
 *
 * 0x8cfe8000 is what isoldr's own heap heuristic picks as free high RAM
 * (ARCHITECTURE.md 10.2). Maple DMA only needs RAM the controller can write
 * and 32-byte alignment, so it does not care where it lives.
 *
 * The Makefile supplies the address, because 0x8cfe8000 is also a base a
 * title can ask the loader to move to -- and then this buffer would be inside
 * the image it is supposed to be clear of. See the layout table there.
 *
 * NAMED, NOT SPELLED OUT. The address comes from the linker script as a
 * symbol (dcload.x.in: PROVIDE (_maple_dma_buffer = DCLOAD_MAPLE)), the same
 * way the base itself is reached through _dcload_base -- and for a second
 * reason beyond the one in AGENTS.md 4.11. A -D reaches the compiler as a
 * NUMBER: -Os folds it straight into this function's literal pool, and a
 * number in a literal pool carries no relocation. Measured 2026-08-27 by
 * relinking the loader at two high bases and diffing: 841 words differ, all by
 * exactly the delta, and 832 of them are described by the relocations `ld -q`
 * emits. The nine that were not were all in _maple_docmd's pool, and all of
 * them were this constant. With it named, relocating an `ld -q` image by a flat
 * delta reproduces a native build at the new base BYTE FOR BYTE -- checked
 * against three of them, including a negative delta and a base no one had ever
 * linked at. Naming it makes the loader relocatable by the host without a
 * rebuild; spelling it out makes it silently un-relocatable.
 *
 * It costs 16 bytes of `_end` (0x8c00a5e8 -> 0x8c00a5f8 at the stock base):
 * the compiler can no longer fold the address in as a constant. Cheap against
 * AGENTS.md 4.6's margin, and stated here because that section is the reason
 * anyone would object.
 */
extern unsigned char maple_dma_buffer[];
volatile unsigned char *const dmabuffer = (volatile unsigned char *)maple_dma_buffer;


/*
 * Send a command to a device and wait for the response.
 *
 * port    - controller port (0-3)
 * unit    - unit number on port (0 = main unit, 1-5 = sub units)
 * cmd     - command number
 * datalen - number of longwords of parameter data
 * data    - parameter data (NB: big endian!)
 *
 */
void *maple_docmd(int port, int unit, int cmd, int datalen, void *data)
{
  unsigned int *sendbuf, *recvbuf, *dmalist;
  int to, from, tries;

  port &= 3;

  /* Compute sender and recipient address */
  from = port << 6;
  to = (port << 6) | (unit>0? ((1<<(unit-1))&0x1f) : 0x20);

  /* Max data length = 255 longs = 1020 bytes */
  if(datalen > 255)
    datalen = 255;
  else if(datalen < 0)
    datalen = 0;

  /* Allocate a 1024 byte receieve buffer at the beginning of
     dmabuffer, with proper alignment.  Also mark the buffer as
     uncacheable.                                               */
  recvbuf =
    (unsigned int *) ((unsigned int)dmabuffer | 0xa0000000);

  /* Place the send buffer right after the receive buffer.  This
     automatically gives proper alignment and uncacheability.    */
  sendbuf =
    (unsigned int *) ((unsigned int)recvbuf + 1024);

  /* Kept, because the three writes below advance `sendbuf` past it and the
   * cycle may have to be run again. */
  dmalist = sendbuf;

  /* Make sure no DMA operation is currently in progress */
  maple_wait_dma();

  /* Setup DMA data.  Each message consists of two control words followed
     by the request frame.  The first control word determines the port,
     the length of the request transmission, and a flag bit marking the
     last message in the burst.  The second control word specifies the
     address where the response frame will be stored.  If no response is
     received within the timeout period, -1 will be written to this address. */

  /* Here we know only one frame should be send and received, so
     the final message control bit will always be set...          */
  *sendbuf++ = datalen | (port << 16) | 0x80000000; // NOTE: These 3 writes use the uncacheable area

  /* Write address to receive buffer where the response frame should be put */
  *sendbuf++ = ((unsigned int)recvbuf & 0x0fffffff);

  /* Create the frame header.  The fields are assembled "backwards"
     because of the Maple Bus big-endianness.                       */
  *sendbuf++ = (cmd & 0xff) | (to << 8) | (from << 16) | (datalen << 24);

  /* Copy parameter data, if any.
   *
   * THROUGH P2, LIKE THE THREE CONTROL WORDS ABOVE.
   *
   * This used to copy through P1 and then CacheBlockWriteBack the region, for
   * the speed of the copy-back area. Two things were wrong with that, and both
   * only bite a payload longer than one longword -- which nothing in the tree
   * sent until the VM2/VMUPro game ID (AGENTS.md 8, CMD_MAPLE):
   *
   *  1. `datalen` is LONGWORDS (it is shifted into the frame header as such),
   *     but SH4_aligned_memcpy's third argument is BYTES. It was handed
   *     `datalen - 1`, so three quarters of the payload never left `data`: a
   *     12-byte VM2 ID arrived as 3 bytes. The line it replaced in 2025 -- the
   *     commented-out memcpy just below -- had it right with `datalen << 2`.
   *
   *  2. The write-back covered `4*datalen` bytes from the 32-byte block that
   *     CONTAINS the payload's start, not from the payload's start, so it fell
   *     up to 12 bytes short whenever 4*datalen was a multiple of 32 (or 24 or
   *     28 past one). Worse, that first block also holds the three control
   *     words, which are written through P2: a cache line left resident by the
   *     previous call is not re-read, so the write-back pushed the PREVIOUS
   *     call's port, receive address and frame header back over the ones this
   *     call had just written. Two MAPL commands to different ports in a row
   *     is all it takes.
   *
   * An uncached copy needs neither argument reasoned about. A Maple payload is
   * at most 1020 bytes and this path is host-driven, so the copy-back area buys
   * nothing here. sendbuf is 4-byte aligned (base + 1024 + 12) and so is `data`
   * (pkt_buf is raw_pkt_buf + 2, and command->data + 4 lands on a multiple of
   * 4), which is all memcpy_32bit asks for.
   */
  if(datalen > 0)
  {
    memcpy_32bit(sendbuf, data, datalen);
  }

  /*
   * RUN THE CYCLE UNTIL IT PRODUCES SOMETHING, AT MOST MAPLE_DMA_TRIES TIMES.
   *
   * A cycle that writes nothing is not the same as a device that says nothing:
   * the controller writes -1 itself when a device does not answer in time, so
   * an untouched buffer means the cycle did not happen. Measured 2026-09-20 on
   * a console with a VMUPro in port A: the first two commands of the session
   * both came back as the same stale RAM, and every command after them worked.
   *
   * Re-running it is the bounded answer to a measurable condition, not a guess
   * at the cause -- `g_maple_dma_empty` counts how often it was needed, so the
   * cause stays visible. The DMA list pointer is re-armed each time because the
   * controller consumes it.
   *
   * STAMP THE RESPONSE HEADER BEFORE STARTING THE DMA.
   *
   * Nothing clears this buffer, so a cycle in which the controller writes
   * nothing leaves whatever was in RAM -- and the caller reads it as a Maple
   * response. That is not theoretical: the first MAPL command of a session
   * came back as "response 0, 255 longwords", which the loader duly served as
   * a 1024-byte frame, and there was no way to tell it from a device that had
   * genuinely said that (measured 2026-09-20, a VMUPro in port A).
   *
   * 0xee in the first byte is a negative response code, so a caller that
   * checks the sign already treats it as a failure, and the host can name it:
   * "the DMA wrote nothing" is a different fault from -1, "the device did not
   * answer in time", which the controller itself writes here.
   */
  for(tries = MAPLE_DMA_TRIES; tries; tries--)
  {
    unsigned int i;

    /*
     * CLEAR THE WHOLE BUFFER, as KOS does in maple_frame_init():
     *   memset(frame->recv_buf, 0, 1024);
     * A device that answers with a short frame leaves everything past it as it
     * was, and the caller reads that as part of the answer. Uncached, in a
     * plain loop: memset_zeroes_64bit() forces its destination to P1, which
     * would put dirty cache lines over a buffer the Maple DMA writes.
     */
    for(i = 0; i < 1024 / 4; i++)
      recvbuf[i] = 0;
    *recvbuf = MAPLE_NO_REPLY;

    /* Set hardware DMA pointer to beginning of the send list */
    MAPLE(0x04) = (unsigned int)dmalist & 0x0fffffff;

    /* Frame is finished, and DMA list is terminated with the flag bit.
       Time to activate the DMA channel.                                */
    MAPLE(0x18) = 1;

    /* Idle, as far as the controller is concerned... */
    maple_wait_dma();

    /* ...and then the answer itself, which is the signal that can be
       trusted. See MAPLE_ANSWER_SPIN_LIMIT. */
    i = MAPLE_ANSWER_SPIN_LIMIT;
    while((*recvbuf == MAPLE_NO_REPLY) && --i)
      ;

    if(*recvbuf != MAPLE_NO_REPLY)
      break;

    /*
     * NEVER RE-TRIGGER ON TOP OF A RUNNING CYCLE. That is what the old code
     * did whenever the busy bit had not come up yet, and starting a second
     * Maple cycle over the first is the shape of abuse that leaves devices
     * unable to answer until they are physically unplugged (reported on
     * hardware 2026-09-20: after a scan, the console could not see the VMUs
     * again, not even from the BIOS, until the controller was replugged).
     */
    g_maple_dma_empty++;
    maple_wait_dma();
  }

  /* Return a pointer to the response frame */
  return recvbuf;
}
