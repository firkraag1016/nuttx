/****************************************************************************
 * arch/risc-v/src/song/song_irq.c
 *
 *   Copyright (C) 2018 Pinecone Inc. All rights reserved.
 *   Author: Xiang Xiao <xiaoxiang@pinecone.net>
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
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <arch/board/board.h>
#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/config.h>
#include <nuttx/irq.h>

#include <assert.h>
#include <stdint.h>

#include "group/group.h"
#include "up_arch.h"
#include "up_internal.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/
volatile uint32_t *g_current_regs;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_irqinitialize
 ****************************************************************************/

void weak_function up_irqinitialize(void)
{
  up_irq_enable();
}

/****************************************************************************
 * Name: up_irq_save
 *
 * Description:
 *   Return the current interrupt state and disable interrupts
 *
 ****************************************************************************/

irqstate_t weak_function up_irq_save(void)
{
  irqstate_t flags;

  __asm__ volatile("csrrci %0, %1, %2" : "=r"(flags) : "i"(0x300), "i"(0x8));
  return flags & 0x8;
}

/****************************************************************************
 * Name: up_irq_restore
 *
 * Description:
 *   Restore previous IRQ mask state
 *
 ****************************************************************************/

void weak_function up_irq_restore(irqstate_t flags)
{
  __asm__ volatile("csrs %0, %1" :: "i"(0x300), "r"(flags));
}

/****************************************************************************
 * Name: up_get_newintctx
 *
 * Description:
 *   Acknowledge the IRQ
 *
 ****************************************************************************/

uint32_t up_get_newintctx(void)
{
  return 0x1880;
}

/****************************************************************************
 * Name: up_irq_enable
 *
 * Description:
 *   Return the current interrupt state and enable interrupts
 *
 ****************************************************************************/

irqstate_t up_irq_enable(void)
{
  irqstate_t flags;

  __asm__ volatile("csrrsi %0, %1, %2" : "=r"(flags) : "i"(0x300), "i"(0x8));
  return flags & 0x8;
}

/****************************************************************************
 * Name: song_dispatch_irqs
 *
 * Description:
 * Call interrupt controller to dispatch irqs
 *
 ****************************************************************************/

void weak_function song_dispatch_irqs(int irq, FAR void *context)
{
  irq_dispatch(irq, context);
}

/****************************************************************************
 * irq_dispatch_all
 ****************************************************************************/

uint32_t * irq_dispatch_all(uint32_t *regs)
{
  uint32_t cause;

  __asm__ volatile("csrr %0, %1" : "=r"(cause) : "i"(0x342));

  /* Current regs non-zero indicates that we are processing an interrupt;
   * g_current_regs is also used to manage interrupt level context switches.
   *
   * Nested interrupts are not supported
   */

  DEBUGASSERT(g_current_regs == NULL);
  g_current_regs = regs;

  if (cause & 0x80000000)
    {
      cause &= 0x7fffffff;

      /* Deliver the IRQ */

      song_dispatch_irqs(cause, regs);
    }
  else if (cause == 11)
    {
      up_swint(11, regs, NULL);
    }

#if defined(CONFIG_ARCH_FPU)
  if (regs != g_current_regs)
    {
      up_restorefpu((uint32_t *)g_current_regs);
    }
#endif

  /* If a context switch occurred while processing the interrupt then
   * g_current_regs may have change value.  If we return any value different
   * from the input regs, then the lower level will know that a context
   * switch occurred during interrupt processing.
   */

  regs = (uint32_t *) g_current_regs;
  g_current_regs = NULL;

  /* Return the stack pointer */

  return regs;
}