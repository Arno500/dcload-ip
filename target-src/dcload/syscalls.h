/*
* This file is part of the dcload Dreamcast ethernet loader
*
* Copyright (C) 2001 Andrew Kieschnick <andrewk@austin.rr.com>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*
*/

#ifndef __SYSCALLS_H__
#define __SYSCALLS_H__

#define CMD_EXIT     "DC00"
#define CMD_FSTAT    "DC01"
#define CMD_WRITE_OLD    "DD02"
#define CMD_WRITE    "DC02"
#define CMD_READ     "DC03"
#define CMD_OPEN     "DC04"
#define CMD_CLOSE    "DC05"
#define CMD_CREAT    "DC06"
#define CMD_LINK     "DC07"
#define CMD_UNLINK   "DC08"
#define CMD_CHDIR    "DC09"
#define CMD_CHMOD    "DC10"
#define CMD_LSEEK    "DC11"
#define CMD_TIME     "DC12"
#define CMD_STAT     "DC13"
#define CMD_UTIME    "DC14"
#define CMD_BAD      "DC15"
#define CMD_OPENDIR  "DC16"
#define CMD_CLOSEDIR "DC17"
#define CMD_READDIR  "DC18"
#define CMD_CDFSREAD "DC19"
#define CMD_GDBPACKET "DC20"
#define CMD_REWINDDIR "DC21"
#define CMD_CDFSTOC "DC22"
/* CD-DA: read raw 2352-byte AUDIO sectors (signed 16-bit stereo PCM; an audio
 * track has no 2048-byte user area, hence a command separate from
 * CMD_CDFSREAD). value0 = LBA, value1 = destination, value2 = bytes.
 * Served by dcload-ip-rs; dc-tool-ip does not implement DC23/DC24.
 *
 * The answer is a LoadBinary/PartBinary transfer to value1 followed by a
 * ReturnValue whose address is the LBA served (the loader refuses any other
 * value once the host has echoed once) and whose size is the host's clock trim
 * in parts per million (1000000 = none; see cdda_scale_from_host). The host
 * sends it without acknowledgement round trips, and does not send it at all if
 * producing it took longer than its give-up time. */
#define CMD_CDDAREAD "DC23"
/* CD-DA as 4-bit Yamaha ADPCM, already split into channels. value0 = LBA,
 * value1 = destination, value2 = frames (= bytes), with bit 31 set to restart
 * the encoder. The sector count is value2 / 588. The payload is the left
 * channel's value2/2 bytes followed by the right channel's. Answered like DC23.
 *
 * It is a stream, not independent blocks: the AICA decodes in long-stream mode
 * and keeps its predictor across the ring wrap, so the host's encoder state
 * must follow the decoder's. The encoder carries on from request to request,
 * answers a repeated request from the bytes it already sent, and resets only
 * on bit 31, which the loader sets on the first fetch after a key-on (the only
 * event that resets the AICA's decoder). */
#define CMD_CDDAREAD_ADPCM "DC24"
/* Console text from a running title, without an answer: value0 = fd (1 or
 * 2), then the bytes, as many as the datagram holds. The host prints them and
 * replies nothing. Sent by dcload only under dcload-ip-rs (g_gd_kos);
 * dc-tool-ip does not implement it. */
#define CMD_CONSOLE "DC25"

extern unsigned short dcload_syscall_port;

extern unsigned int syscall_retval;
extern unsigned char* syscall_data;
extern unsigned int syscall_retsize;

typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned char id[4];
	unsigned int value0;
	unsigned int value1;
	unsigned int value2;
} command_3int_t;

typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned char id[4];
	unsigned int value0;
	unsigned int value1;
	unsigned char string[1];
} command_2int_string_t;

typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned char id[4];
	unsigned int value0;
} command_int_t;

typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned char id[4];
	unsigned int value0;
	unsigned char string[1];
} command_int_string_t;

typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned char id[4];
	unsigned char string[1];
} command_string_t;

typedef struct __attribute__ ((packed, aligned(4))) {
	unsigned char id[4];
	unsigned int value0;
	unsigned int value1;
	unsigned int value2;
	unsigned char string[1];
} command_3int_string_t;

// Functions that are not in unistd.h, but are used by other parts of dcload
// (exempting dcload-crt0.s, which uses all of the syscalls in assembly code and doesn't need prototypes in a header)
void build_send_packet(int command_len);
void dcexit(void);

#endif
