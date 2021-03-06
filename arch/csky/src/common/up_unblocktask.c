/************************************************************************************
 *arch/csky/src/common/up_unblocktask.c
 *
 * Copyright (C) 2015 The YunOS Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ************************************************************************************/


/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sched.h>
#include <debug.h>
#include <nuttx/arch.h>
#include <nuttx/sched.h>

#include "sched/sched.h"
#include "group/group.h"
#include "clock/clock.h"
#include "up_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_unblock_task
 *
 * Description:
 *   A task is currently in an inactive task list
 *   but has been prepped to execute.  Move the TCB to the
 *   ready-to-run list, restore its context, and start execution.
 *
 * Inputs:
 *   tcb: Refers to the tcb to be unblocked.  This tcb is
 *     in one of the waiting tasks lists.  It must be moved to
 *     the ready-to-run list and, if it is the highest priority
 *     ready to run task, executed.
 *
 ****************************************************************************/

void up_unblock_task(struct tcb_s *tcb)
{
    struct tcb_s *rtcb = (struct tcb_s *)g_readytorun.head;

    /* Verify that the context switch can be performed */

    ASSERT((tcb->task_state >= FIRST_BLOCKED_STATE) &&
           (tcb->task_state <= LAST_BLOCKED_STATE));

    /* Remove the task from the blocked task list */

    sched_removeblocked(tcb);

    /* Add the task in the correct location in the prioritized
     * g_readytorun task list
     */

    if (sched_addreadytorun(tcb)) {
        /* The currently active task has changed! We need to do
         * a context switch to the new task.
         */

        /* Update scheduler parameters */

        sched_suspend_scheduler(rtcb);

        /* Are we in an interrupt handler? */

        if (current_regs) {
            /* Yes, then we have to do things differently.
             * Just copy the current_regs into the OLD rtcb.
             */

            up_savestate(rtcb->xcp.regs);

            /* Restore the exception context of the rtcb at the (new) head
             * of the g_readytorun task list.
             */

            rtcb = (struct tcb_s *)g_readytorun.head;

            /* Update scheduler parameters */

            sched_resume_scheduler(rtcb);

            /* Then switch contexts.  Any necessary address environment
             * changes will be made when the interrupt returns.
             */

            up_restorestate(rtcb->xcp.regs);
        }

        /* We are not in an interrupt handler.  Copy the user C context
         * into the TCB of the task that was previously active.  if
         * up_saveusercontext returns a non-zero value, then this is really the
         * previously running task restarting!
         */

        else if (!up_saveusercontext(rtcb->xcp.regs)) {
            /* Restore the exception context of the new task that is ready to
             * run (probably tcb).  This is the new rtcb at the head of the
             * g_readytorun task list.
             */

            rtcb = (struct tcb_s *)g_readytorun.head;

#ifdef CONFIG_ARCH_ADDRENV
            /* Make sure that the address environment for the previously
             * running task is closed down gracefully (data caches dump,
             * MMU flushed) and set up the address environment for the new
             * thread at the head of the ready-to-run list.
             */

            (void)group_addrenv(rtcb);
#endif
            /* Update scheduler parameters */

            sched_resume_scheduler(rtcb);

            /* Then switch contexts */

            up_fullcontextrestore(rtcb->xcp.regs);
        }
    }
}
