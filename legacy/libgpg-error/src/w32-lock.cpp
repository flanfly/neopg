/* w32-lock.c - GPGRT lock functions for Windows
   Copyright (C) 2014 g10 Code GmbH

   This file is part of libgpg-error.

   libgpg-error is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License
   as published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   libgpg-error is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <gpg-error.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lock.h"



/*
 * Functions called before and after blocking syscalls.
 * gpgrt_set_syscall_clamp is used to set them.
 */
static void (*pre_lock_func)(void);
static void (*post_lock_func)(void);


/* Helper to set the clamp functions.  This is called as a helper from
 * _gpgrt_set_syscall_clamp to keep the function pointers local. */
void
_gpgrt_lock_set_lock_clamp (void (*pre)(void), void (*post)(void))
{
  pre_lock_func = pre;
  post_lock_func = post;
}



static _gpgrt_lock_t *
get_lock_object (gpgrt_lock_t *lockhd)
{
  _gpgrt_lock_t *lock = (_gpgrt_lock_t*)lockhd;

  if (lock->vers != LOCK_ABI_VERSION)
    abort ();

  return lock;
}


gpg_err_code_t
_gpgrt_lock_init (gpgrt_lock_t *lockhd)
{
  _gpgrt_lock_t *lock = (_gpgrt_lock_t*)lockhd;

  /* If VERS is zero we assume that no static initialization has been
     done, so we setup our ABI version right here.  The caller might
     have called us to test whether lock support is at all available. */
  if (!lock->vers)
    {
      if (sizeof (gpgrt_lock_t) < sizeof (_gpgrt_lock_t))
        abort ();
      lock->vers = LOCK_ABI_VERSION;
    }
  else /* Run the usual check.  */
    {
      lock = get_lock_object (lockhd);
      if (sizeof (gpgrt_lock_t) < sizeof (_gpgrt_lock_t))
        abort ();
    }

  InitializeCriticalSection (&lock->csec);
  lock->initdone = 1;
  return 0;
}


gpg_err_code_t
_gpgrt_lock_lock (gpgrt_lock_t *lockhd)
{
  _gpgrt_lock_t *lock = get_lock_object (lockhd);

  if (!lock->initdone)
    {
      if (!InterlockedIncrement (&lock->started))
        {
          /* The new value of started is 0.  Because the initial value
             if the variable was -1 we known that this thread is the
             first who needs this lock.  Thus we initialize now.  All
             other threads won't get 0 back from InterlockedIncrement
             and thus fall into the wait loop below.  We ignore that
             STARTED may in theory overflow if this thread starves for
             too long.  */
          gpgrt_lock_init (lockhd);
        }
      else
        {
          while (!lock->initdone)
            Sleep (0);
        }
    }

  if (pre_lock_func)
    pre_lock_func ();
  EnterCriticalSection (&lock->csec);
  if (post_lock_func)
    post_lock_func ();
  return 0;
}


gpg_err_code_t
_gpgrt_lock_trylock (gpgrt_lock_t *lockhd)
{
  _gpgrt_lock_t *lock = get_lock_object (lockhd);

  if (!lock->initdone)
    {
      if (!InterlockedIncrement (&lock->started))
        {
          gpgrt_lock_init (lockhd);
        }
      else
        {
          while (!lock->initdone)
            Sleep (0);
        }
    }

  if (!TryEnterCriticalSection (&lock->csec))
    return GPG_ERR_EBUSY;
  return 0;
}


gpg_err_code_t
_gpgrt_lock_unlock (gpgrt_lock_t *lockhd)
{
  _gpgrt_lock_t *lock = get_lock_object (lockhd);

  if (!lock->initdone)
    return GPG_ERR_INV_LOCK_OBJ;
  LeaveCriticalSection (&lock->csec);
  return 0;
}


/* Note: Use this function only if no other thread holds or waits for
   this lock.  */
gpg_err_code_t
_gpgrt_lock_destroy (gpgrt_lock_t *lockhd)
{
  _gpgrt_lock_t *lock = get_lock_object (lockhd);

  if (!lock->initdone)
    return GPG_ERR_INV_LOCK_OBJ;
  DeleteCriticalSection (&lock->csec);
  lock->initdone = 0;
  lock->started = -1;
  return 0;
}
