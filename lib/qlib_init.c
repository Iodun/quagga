/* Quagga library initialise/closedown -- functions
 * Copyright (C) 2009 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2, or (at your
 * option) any later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#include "misc.h"

#include <errno.h>
#include <stdio.h>

#include "qlib_init.h"
#include "zassert.h"
#include "memory.h"
#include "qpnexus.h"
#include "qpthreads.h"
#include "qpselect.h"
#include "thread.h"
#include "privs.h"
#include "mqueue.h"
#include "pthread_safe.h"
#include "log_local.h"
#include "qiovec.h"

/*==============================================================================
 * Quagga Library Initialise/Closedown
 *
 * This gathers together the essential initialisation and closedown for the
 * library.  This ensures that any changes in the library are contained here,
 * and do not require changes in all users of the library.
 *
 * There are two stages of initialisation:
 *
 *   1) first stage
 *
 *      this is expected to be called before the program does anything at all.
 *
 *      Collects a small number of useful system parameters -- see below.
 *
 *      This performs all initialisation required to support asserts, logging,
 *      basic I/O (but not the remote console), trap signals... and so on.
 *
 *      After this has been done, the system is in good shape to deal with
 *      command line options, configuration files and so on.
 *
 *   2) second stage
 *
 *      this is expected to be called before the program does any serious work.
 *
 *      This performs all initialisation required to support socket I/O,
 *      thread handling, timers, and so on.
 *
 *      NB: at this stage the system is set into pthread mode, if required.
 *
 *          No pthreads may be started before this.  Up to this point
 *          the system operates in non-pthread mode -- all mutexes are
 *          implicitly free.
 *
 * There is one stage of closedown.  This is expected to be called last, and
 * is passed the exit code.
 *
 *==============================================================================
 * System parameters:
 *
 *   iov_max   -- _SC_IOV_MAX
 *
 *   open_max  -- _SC_OPEN_MAX
 */

int qlib_iov_max ;
int qlib_open_max ;
int qlib_pagesize ;

struct
{
  int*        p_var ;
  int         sc ;
  const char* name ;
  long        min ;
  long        max ;
} qlib_vars[] =
{
    { .p_var = &qlib_iov_max,   .sc = _SC_IOV_MAX,   .name = "_SC_IOV_MAX",
                                        .min =  16, .max = INT_MAX            },
    { .p_var = &qlib_open_max,  .sc = _SC_OPEN_MAX,  .name = "_SC_OPEN_MAX",
                                        .min = 256, .max = INT_MAX            },
    { .p_var = &qlib_pagesize,  .sc = _SC_PAGESIZE,  .name = "_SC_PAGESIZE",
                                        .min = 256, .max = (INT_MAX >> 1) + 1 },
    { .p_var = NULL }
} ;

extern void
qlib_init_first_stage(void)
{
  int   i ;

  for (i = 0 ; qlib_vars[i].p_var != NULL ; ++i)
    {
      long  val ;

      errno = 0 ;
      val = sysconf(qlib_vars[i].sc) ;

      if (val == -1)
        {
          if (errno == 0)
            val = INT_MAX ;
          else
            {
              fprintf(stderr, "Failed to sysconf(%s): %s\n",
                                  qlib_vars[i].name, errtoa(errno, 0).str) ;
              exit(1) ;
            } ;
        } ;

      if ((val < qlib_vars[i].min) || (val > qlib_vars[i].max))
        {
          fprintf(stderr, "sysconf(%s) = %ld: which is < %ld or > %ld\n",
                   qlib_vars[i].name, val, qlib_vars[i].min, qlib_vars[i].max) ;
          exit(1) ;
        } ;

      *(qlib_vars[i].p_var) = (int)val ;
    } ;

  qps_start_up() ;
  memory_start() ;
  qiovec_start_up() ;
}

extern void
qlib_init_second_stage(bool pthreads)
{
  qpt_set_qpthreads_enabled(pthreads);
  qpn_init() ;
  memory_init_r();
  thread_init_r();
  log_init_r() ;
  zprivs_init_r();
  mqueue_initialise();
  safe_init_r();
}

extern void
qexit(int exit_code)
{
  safe_finish();
  mqueue_finish();
  zprivs_finish();
  log_finish();
  thread_finish();
  memory_finish();
  exit (exit_code);
}


