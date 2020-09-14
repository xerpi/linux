/*
 *  Copyright (C) 2020 Sergi Granell
 *
 * Debugging macro include header
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
*/

#define ARM9_FW_SHARED_BUF_PA 0x27000000
#define ARM9_FW_SHARED_BUF_VA 0xE0000000

.macro	addruart,rp,rv,tmp
	mov	\rp, #ARM9_FW_SHARED_BUF_PA
	mov	\rv, #ARM9_FW_SHARED_BUF_VA
.endm

.macro	senduart,rd,rx
	strb	\rd, [\rx]
.endm

.macro	waituart,rd,rx
.endm

.macro	busyuart,rd,rx
99:
	ldrb	\rd, [\rx]
	cmp	\rd, #0
	bne	99b
.endm
