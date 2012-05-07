/* VTY interface to logging
 * Copyright (C) 1997, 98 Kunihiro Ishiguro
 *
 * Revisions: Copyright (C) 2010 Chris Hall (GMCH), Highwayman
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "misc.h"

#include "vty_log.h"
#include "vty_local.h"
#include "vty_io_term.h"
#include "log_local.h"
#include "list_util.h"
#include "vio_fifo.h"
#include "mqueue.h"

/*==============================================================================
 * This supports the "vty monitor" facility -- which reflects logging
 * information to one or more VTY_TERMINAL vty.
 *
 * NB: this *only* only to the base_vout of a VTY_TERMINAL.
 *
 * There are a number of issues:
 *
 *   a) output of logging information should not be held up any longer than
 *      is absolutely necessary.
 *
 *   b) console may be busy doing other things, so logging information needs
 *      to be buffered.
 *
 *   c) zlog() et al, hold the LOG_LOCK(), which is at a lower level than the
 *      VTY_LOCK().
 *
 *      MUST NOT require the VTY_LOCK() in order to complete a zlog() operation,
 *      hence the buffering and other mechanisms.
 *
 *   d) may have one or more monitor vty, possibly at different levels of
 *      message.
 *
 *   e) must avoid logging I/O error log messages for given vty on that vty !
 *
 *      The I/O error handling turns off log monitoring for the vty if the
 *      vin_base or the vout_base is the locus of the error.
 *
 * The list of monitor vio is handled under the LOG_LOCK *and* the VTY_LOCK.
 *
 * Each vio has an mbuf FIFO, which is written to and read from under the
 * LOG_LOCK -- so is set up and discarded under the same lock.
 *
 * To send a log message to a log monitor, the logging code (under the
 * LOG_LOCK) appends to the relevant mbuf(s).  It must then alert the CLI
 * thread to cause it to empty out the mbuf(s) -- which is done by sending a
 * message to the CLI nexus.  The mon_kicked flag is set when the message is
 * sent, and cleared when the CLI thread has emptied the buffers.
 *
 * If not running multi-nexus, the process is simpler.  The messages are
 * queued in the mbuf(s), but there is no need to send any message, and the
 * buffers are then promptly emptied -- as if a message had been sent, and
 * immediately received.
 */
static vio_fifo monitor_buffer   = NULL ;
static uint     monitor_count    = 0 ;

static bool         mon_kicked   = false ;
static mqueue_block mon_mqb      = NULL ;

static void vty_monitor_action(mqueue_block mqb, mqb_flag_t flag) ;
static void uty_monitor_update(int delta) ;

/*------------------------------------------------------------------------------
 * Initialise the vty monitor facility.
 *
 * This runs before any pthreads or nexus stuff starts -- so no lock required.
 *
 * Sets everything *off*.
 *
 * NB: can be used by vtysh !
 */
extern void
uty_monitor_init(void)
{
  qassert(!qpthreads_enabled) ;

  ddl_init(vio_monitor_list) ;
  monitor_buffer   = NULL ;

  mon_kicked       = false ;
  mon_mqb          = NULL ;
} ;

/*------------------------------------------------------------------------------
 * Set/Clear "monitor" state:
 *
 *   set:   if VTY_TERM and not already "monitor" (and write_open !)
 *   clear: if is "monitor"
 *
 * Note that we need the VTY_LOCK *and* the LOG_LOCK to change the list of
 * monitors -- so we can walk the list with either locked.  But we only need
 * the VTY_LOCK to set/clear the
 */
extern void
uty_monitor_set(vty_io vio, on_off_b how)
{
  int delta ;

  VTY_ASSERT_LOCKED() ;

  LOG_LOCK() ;

  if ((vio->vout_base->vout_type != VOUT_TERM) ||
                             (vio->vout_base->vout_state & (vf_cease | vf_end)))
    how = off ;

  if (vio->monitor)
    qassert(vio->vout_base->vout_type == VOUT_TERM) ;

  delta = 0 ;

  if      ((how == on) && !vio->monitor)
    {
      /* Note that in the unlikely even that there is something pending in
       * an existing mbuf, then that will be emptied out by the pselect()
       * process.
       */
      delta        = +1 ;
      vio->monitor = true ;

      vio->maxlvl  = uzlog_get_monitor_lvl(NULL) ;

      if (vio->mbuf == NULL)
        vio->mbuf  = vio_fifo_new(8 * 1024) ;

      ddl_append(vio_monitor_list, vio, mon_list) ;
   }
  else if ((how == off) && vio->monitor)
    {
      /* Note that if there is anything pending in the mbuf, then that will
       * be emptied out by the pselect() process.
       */
      vio->maxlvl  = ZLOG_DISABLED ;

      ddl_del(vio_monitor_list, vio, mon_list) ;

      vio->monitor = false ;
      delta        = -1 ;
    } ;

  uty_monitor_update(delta) ;   /* sort out effective log monitor level */

  LOG_UNLOCK() ;
} ;

/*------------------------------------------------------------------------------
 * If the current VTY is a log monitor, set a new level
 */
extern void
vty_monitor_set_level(vty vty, int level)
{
  VTY_LOCK() ;

  if (vty->vio->monitor)
    {
      LOG_LOCK() ;

      vty->vio->maxlvl = level ;
      uty_monitor_update(0) ;

      LOG_UNLOCK() ;
    } ;

  VTY_UNLOCK() ;
} ;

/*------------------------------------------------------------------------------
 * Establish the maximum level of all monitors and tell the logging levels.
 *
 * This is used when a monitor is enabled or disabled, and when a VTY's monitor
 * level is changed.
 */
static void
uty_monitor_update(int delta)
{
  int    level ;
  uint   count ;
  vty_io vio ;

  VTY_ASSERT_LOCKED() ;
  LOG_ASSERT_LOCKED() ;

  monitor_count += delta ;

  vio   = ddl_head(vio_monitor_list) ;
  count = 0 ;
  level = ZLOG_DISABLED ;
  while (vio != NULL)
    {
      ++count ;
      qassert(vio->vout_base->vout_type == VOUT_TERM) ;

      if (level <= vio->maxlvl)
        level = vio->maxlvl ;

      vio = ddl_next(vio, mon_list) ;
    } ;

  qassert(monitor_count == count) ;

  uzlog_set_monitor(NULL, level) ;
} ;

/*------------------------------------------------------------------------------
 * Put logging message to all suitable monitors.
 *
 * All we can do here is to shovel stuff into buffers and then kick the VTY
 * to do something.  If running multi-nexus, then the kick takes the form of
 * a message sent to the cli nexus, otherwise can call the message action
 * function here and now.
 *
 * NB: expect incoming line to NOT include '\n' or any other line ending.
 */
extern void
vty_monitor_log(int priority, const char* line, uint len)
{
  vty_io vio ;
  bool   kick ;

  LOG_ASSERT_LOCKED() ;

  vio  = ddl_head(vio_monitor_list) ;
  kick = false ;
  while (vio != NULL)
    {
      qassert(vio->vout_base->vout_type == VOUT_TERM) ;

      if (priority <= vio->maxlvl)
        {
          vio_fifo_put_bytes(vio->mbuf, line, len) ;
          vio_fifo_put_bytes(vio->mbuf, "\r\n", 2) ;

          kick = true ;
        } ;

      vio = ddl_next(vio, mon_list) ;
    } ;

  if (kick)
    {
      if (vty_multi_nexus)
        {
          if (!mon_kicked)
            {
              if (mon_mqb == NULL)
                mon_mqb = mqb_init_new(NULL, vty_monitor_action, &mon_mqb) ;

              mon_kicked = true ;
              mqueue_enqueue(vty_cli_nexus->queue, mon_mqb, mqb_ordinary) ;
            } ;
        }
      else
        vty_monitor_action(NULL, mqb_action) ;
    } ;
} ;

/*------------------------------------------------------------------------------
 * Action routine to kick all the monitor vty to empty out their mbuf(s).
 *
 * Note that for multi-nexus this is the action associated with an actual
 * mqueue message.  So, we VTY_LOCK and LOG_LOCK (in that order) before
 * proceeding.  Note also that uty_term_mon_write() will LOG_LOCK() again,
 * so it must be a recursive lock.
 *
 * For single nexus or legacy threads, this is called directly, when a log
 * message is put into one or more mbufs.  Technically that violates the
 * locking order, because will be LOG_LOCKed already -- but we don't care,
 * since the locking is empty in this case !
 *
 * To minimise the time spent with the LOG_LOCK, we step through the monitors,
 * and check for non-empty mbuf -- for which we need the LOG_LOCK.  Then we
 * release the lock, and then step through the monitors, calling the write
 * operation for each one that needs it.  That will LOG_LOCK again, for each
 * one, as required.
 */
static void
vty_monitor_action(mqueue_block mqb, mqb_flag_t flag)
{
  VTY_LOCK() ;

  LOG_LOCK() ;          /* IN THIS ORDER !!!                            */

  if (flag == mqb_action)
    {
      vty_io vio ;

      vio  = ddl_head(vio_monitor_list) ;
      while (vio != NULL)
        {
          qassert(vio->vout_base->vout_type == VOUT_TERM) ;

          vio->mwrite = !vio_fifo_is_empty(vio->mbuf) ;

          vio = ddl_next(vio, mon_list) ;
        } ;
    }
  else
    mqb_free(mqb) ;     /* Suicide              */

  mon_kicked = false ;  /* If anything else happens, need to kick again */

  LOG_UNLOCK() ;

  if (flag == mqb_action)
    {
      vty_io vio ;

      vio  = ddl_head(vio_monitor_list) ;
      while (vio != NULL)
        {
          qassert(vio->vout_base->vout_type == VOUT_TERM) ;

          if (vio->mwrite)
            uty_term_mon_write(vio->vout_base) ;

          vio = ddl_next(vio, mon_list) ;
        } ;
    } ;

  VTY_UNLOCK() ;
} ;

/*------------------------------------------------------------------------------
 * Async-signal-safe version of vty_monitor_log for fixed strings.
 *
 * This is last gasp operation.
 */
extern void
vty_monitor_log_fixed (const char *buf, uint len)
{
  vty_io  vio ;

  /* Write to all known "monitor" vty
   *
   * Forget all the niceties -- about to die in any case.
   */
  vio = ddl_head(vio_monitor_list) ;
  while (vio != NULL)
    {
      qassert(vio->vout_base->vout_type == VOUT_TERM) ;

      write(vio_vfd_fd(vio->vout_base->vfd), buf, len) ;
      write(vio_vfd_fd(vio->vout_base->vfd), "\r\n", 2) ;

      vio = ddl_next(vio, mon_list) ;
    } ;
} ;