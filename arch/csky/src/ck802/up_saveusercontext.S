/**************************************************************************
 * arch/csky/src/ck802/up_saveusercontext.S
 *
 *   Copyright (C) 2007, 2009 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************/

/**************************************************************************
 * Included Files
 **************************************************************************/

#include <nuttx/irq.h>
#include <up_internal.h>

/**************************************************************************
 * Pre-processor Definitions
 **************************************************************************/

/**************************************************************************
 * Private Types
 **************************************************************************/

/**************************************************************************
 * Private Function Prototypes
 **************************************************************************/

/**************************************************************************
 * Public Data
 **************************************************************************/

/**************************************************************************
 * Private Variables
 **************************************************************************/

/**************************************************************************
 * Private Functions
 **************************************************************************/

/**************************************************************************
 * Public Functions
 **************************************************************************/

/**************************************************************************
 * Name: up_saveusercontext
 **************************************************************************/

	.text
	.globl	up_saveusercontext
	.type	up_saveusercontext, function
up_saveusercontext:
	/* On entry, a0 (r0) holds address of struct xcptcontext.
	 * Offset to the user region.
	 */

	/* Make sure that the return value will be non-zero (the
	 * value of the other volatile registers don't matter --
	 * r0-r3, r12-r13).  This function is called throught the
	 * noraml C calling conventions and the values of these
	 * registers cannot be assumed at the point of setjmp
	 * return.
	 */

	movi    r12, 1
	stw     r12, (r0, 4*REG_R0)

	/* Save the volatile registers (plus r12 which really
	 * doesn't need to be saved)
	 */

	addi    r1, r0, (4*REG_R4)
	stm     r4-r14, (r1)

	/* Save the current psr */

	mfcr    r2, psr
	addi    r1, r0, (4*REG_PSR)
	stw     r2, (r1)

	/* Finally save the return address as the PC so that we
	 * return to the exit from this function.
	 */

	addi    r1, r0, (4*REG_PC)
	stw     lr, (r1)

	/* Return 0 */

	movi    r0, 0      /* Return value == 0 */

	rts                /* Return */
	.size   up_saveusercontext, . - up_saveusercontext
