/* call-scd.c - fork of the scdaemon to do SC operations
 *	Copyright (C) 2001, 2002, 2005 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef HAVE_W32_SYSTEM
#include <sys/wait.h>
#endif
#include <pth.h>

#include "agent.h"
#include <assuan.h>

#ifdef _POSIX_OPEN_MAX
#define MAX_OPEN_FDS _POSIX_OPEN_MAX
#else
#define MAX_OPEN_FDS 20
#endif

/* Definition of module local data of the CTRL structure.  */
struct scd_local_s
{
  /* We keep a list of all allocated context with a an achnor at
     SCD_LOCAL_LIST (see below). */
  struct scd_local_s *next_local;

  /* We need to get back to the ctrl object actually referencing this
     structure.  This is really an awkward way of enumerint the lcoal
     contects.  A much cleaner way would be to keep a global list of
     ctrl objects to enumerate them.  */
  ctrl_t ctrl_backlink;

  assuan_context_t ctx; /* NULL or session context for the SCdaemon
                           used with this connection. */
  int locked;           /* This flag is used to assert proper use of
                           start_scd and unlock_scd. */

};


/* Callback parameter for learn card */
struct learn_parm_s
{
  void (*kpinfo_cb)(void*, const char *);
  void *kpinfo_cb_arg;
  void (*certinfo_cb)(void*, const char *);
  void *certinfo_cb_arg;
  void (*sinfo_cb)(void*, const char *, size_t, const char *);
  void *sinfo_cb_arg;
};

struct inq_needpin_s 
{
  assuan_context_t ctx;
  int (*getpin_cb)(void *, const char *, char*, size_t);
  void *getpin_cb_arg;
};


/* To keep track of all active SCD contexts, we keep a linked list
   anchored at this variable. */
static struct scd_local_s *scd_local_list;

/* A Mutex used inside the start_scd function. */
static pth_mutex_t start_scd_lock;

/* A malloced string with the name of the socket to be used for
   additional connections.  May be NULL if not provided by
   SCdaemon. */
static char *socket_name;

/* The context of the primary connection.  This is also used as a flag
   to indicate whether the scdaemon has been started. */
static assuan_context_t primary_scd_ctx;

/* To allow reuse of the primary connection, the following flag is set
   to true if the primary context has been reset and is not in use by
   any connection. */
static int primary_scd_ctx_reusable;



/* Local prototypes.  */
static assuan_error_t membuf_data_cb (void *opaque,
                                      const void *buffer, size_t length);




/* This function must be called once to initialize this module.  This
   has to be done before a second thread is spawned.  We can't do the
   static initialization because Pth emulation code might not be able
   to do a static init; in particular, it is not possible for W32. */
void
initialize_module_call_scd (void)
{
  static int initialized;

  if (!initialized)
    {
      if (!pth_mutex_init (&start_scd_lock))
        log_fatal ("error initializing mutex: %s\n", strerror (errno));
      initialized = 1;
    }
}


static void
dump_mutex_state (pth_mutex_t *m)
{
  if (!(m->mx_state & PTH_MUTEX_INITIALIZED))
    log_printf ("not_initialized");
  else if (!(m->mx_state & PTH_MUTEX_LOCKED))
    log_printf ("not_locked");
  else
    log_printf ("locked tid=0x%lx count=%lu", (long)m->mx_owner, m->mx_count);
}


/* This function may be called to print infromation pertaining to the
   current state of this module to the log. */
void
agent_scd_dump_state (void)
{
  log_info ("agent_scd_dump_state: scd_lock=");
  dump_mutex_state (&start_scd_lock);
  log_printf ("\n");
  log_info ("agent_scd_dump_state: primary_scd_ctx=%p pid=%ld reusable=%d\n",
            primary_scd_ctx, 
            (long)assuan_get_pid (primary_scd_ctx),
            primary_scd_ctx_reusable);
  if (socket_name)
    log_info ("agent_scd_dump_state: socket=`%s'\n", socket_name);
}


/* The unlock_scd function shall be called after having accessed the
   SCD.  It is currently not very useful but gives an opportunity to
   keep track of connections currently calling SCD.  Note that the
   "lock" operation is done by the start_scd() function which must be
   called and error checked before any SCD operation.  CTRL is the
   usual connection context and RC the error code to be passed trhough
   the function. */
static int 
unlock_scd (ctrl_t ctrl, int rc)
{
  if (ctrl->scd_local->locked != 1)
    {
      log_error ("unlock_scd: invalid lock count (%d)\n",
                 ctrl->scd_local->locked);
      if (!rc)
        rc = gpg_error (GPG_ERR_INTERNAL);
    }
  ctrl->scd_local->locked = 0;
  return rc;
}

/* To make sure we leave no secrets in our image after forking of the
   scdaemon, we use this callback. */
static void
atfork_cb (void *opaque, int where)
{
  if (!where)
    gcry_control (GCRYCTL_TERM_SECMEM);
}


/* Fork off the SCdaemon if this has not already been done.  Lock the
   daemon and make sure that a proper context has been setup in CTRL.
   Thsi fucntion might also lock the daemon, which means that the
   caller must call unlock_scd after this fucntion has returned
   success and the actual Assuan transaction been done. */
static int
start_scd (ctrl_t ctrl)
{
  gpg_error_t err = 0;
  const char *pgmname;
  assuan_context_t ctx;
  const char *argv[3];
  int no_close_list[3];
  int i;
  int rc;

  if (opt.disable_scdaemon)
    return gpg_error (GPG_ERR_NOT_SUPPORTED);

  /* If this is the first call for this session, setup the local data
     structure. */
  if (!ctrl->scd_local)
    {
      ctrl->scd_local = xtrycalloc (1, sizeof *ctrl->scd_local);
      if (!ctrl->scd_local)
        return gpg_error_from_errno (errno);
      ctrl->scd_local->ctrl_backlink = ctrl;
      ctrl->scd_local->next_local = scd_local_list;
      scd_local_list = ctrl->scd_local;
    }


  /* Assert that the lock count is as expected. */
  if (ctrl->scd_local->locked)
    {
      log_error ("start_scd: invalid lock count (%d)\n",
                 ctrl->scd_local->locked);
      return gpg_error (GPG_ERR_INTERNAL);
    }
  ctrl->scd_local->locked++;

  if (ctrl->scd_local->ctx)
    return 0; /* Okay, the context is fine.  We used to test for an
                 alive context here and do an disconnect.  Now that we
                 have a ticker function to check for it, it is easier
                 not to check here but to let the connection run on an
                 error instead. */


  /* We need to protect the following code. */
  if (!pth_mutex_acquire (&start_scd_lock, 0, NULL))
    {
      log_error ("failed to acquire the start_scd lock: %s\n",
                 strerror (errno));
      return gpg_error (GPG_ERR_INTERNAL);
    }

  /* Check whether the pipe server has already been started and in
     this case either reuse a lingering pipe connection or establish a
     new socket based one. */
  if (primary_scd_ctx && primary_scd_ctx_reusable)
    {
      ctx = primary_scd_ctx;
      primary_scd_ctx_reusable = 0;
      if (opt.verbose)
        log_info ("new connection to SCdaemon established (reusing)\n");
      goto leave;
    }

  if (socket_name)
    {
      rc = assuan_socket_connect (&ctx, socket_name, 0);
      if (rc)
        {
          log_error ("can't connect to socket `%s': %s\n",
                     socket_name, gpg_strerror (rc));
          err = gpg_error (GPG_ERR_NO_SCDAEMON);
          goto leave;
        }

      if (opt.verbose)
        log_info ("new connection to SCdaemon established\n");
      goto leave;
    }

  if (primary_scd_ctx)
    {
      log_info ("SCdaemon is running but won't accept further connections\n");
      err = gpg_error (GPG_ERR_NO_SCDAEMON);
      goto leave;
    }

  /* Nope, it has not been started.  Fire it up now. */
  if (opt.verbose)
    log_info ("no running SCdaemon - starting it\n");
      
  if (fflush (NULL))
    {
      err = gpg_error (gpg_err_code_from_errno (errno));
      log_error ("error flushing pending output: %s\n", strerror (errno));
      goto leave;
    }

  if (!opt.scdaemon_program || !*opt.scdaemon_program)
    opt.scdaemon_program = GNUPG_DEFAULT_SCDAEMON;
  if ( !(pgmname = strrchr (opt.scdaemon_program, '/')))
    pgmname = opt.scdaemon_program;
  else
    pgmname++;

  argv[0] = pgmname;
  argv[1] = "--multi-server";
  argv[2] = NULL;

  i=0;
  if (!opt.running_detached)
    {
      if (log_get_fd () != -1)
        no_close_list[i++] = log_get_fd ();
      no_close_list[i++] = fileno (stderr);
    }
  no_close_list[i] = -1;

  /* Connect to the pinentry and perform initial handshaking */
  rc = assuan_pipe_connect2 (&ctx, opt.scdaemon_program, argv,
                             no_close_list, atfork_cb, NULL);
  if (rc)
    {
      log_error ("can't connect to the SCdaemon: %s\n",
                 gpg_strerror (rc));
      err = gpg_error (GPG_ERR_NO_SCDAEMON);
      goto leave;
    }

  if (opt.verbose)
    log_debug ("first connection to SCdaemon established\n");

  /* Get the name of the additional socket opened by scdaemon. */
  {
    membuf_t data;
    unsigned char *databuf;
    size_t datalen;

    xfree (socket_name);
    socket_name = NULL;
    init_membuf (&data, 256);
    assuan_transact (ctx, "GETINFO socket_name",
                     membuf_data_cb, &data, NULL, NULL, NULL, NULL);

    databuf = get_membuf (&data, &datalen);
    if (databuf && datalen)
      {
        socket_name = xtrymalloc (datalen + 1);
        if (!socket_name)
          log_error ("warning: can't store socket name: %s\n",
                     strerror (errno));
        else
          {
            memcpy (socket_name, databuf, datalen);
            socket_name[datalen] = 0;
            if (DBG_ASSUAN)
              log_debug ("additional connections at `%s'\n", socket_name);
          }
      }
    xfree (databuf);
  }

  /* Tell the scdaemon we want him to send us an event signal. */
#ifndef HAVE_W32_SYSTEM
  {
    char buf[100];

    sprintf (buf, "OPTION event-signal=%d", SIGUSR2);
    assuan_transact (ctx, buf, NULL, NULL, NULL, NULL, NULL, NULL);
  }
#endif

  primary_scd_ctx = ctx;
  primary_scd_ctx_reusable = 0;

 leave:
  if (err)
    {
      unlock_scd (ctrl, err);
    } 
  else
    {
      ctrl->scd_local->ctx = ctx;
    }
  if (!pth_mutex_release (&start_scd_lock))
    log_error ("failed to release the start_scd lock: %s\n", strerror (errno));
  return err;
}


/* Check whether the Scdaemon is still alive and clean it up if not. */
void
agent_scd_check_aliveness (void)
{
  pth_event_t evt;
  pid_t pid;
  int rc;

  if (!primary_scd_ctx)
    return; /* No scdaemon running. */

  /* This is not a critical function so we use a short timeout while
     acquiring the lock.  */
  evt = pth_event (PTH_EVENT_TIME, pth_timeout (1, 0));
  if (!pth_mutex_acquire (&start_scd_lock, 0, evt))
    {
      if (pth_event_occurred (evt))
        {
          if (opt.verbose > 1)
            log_info ("failed to acquire the start_scd lock while"
                      " doing an aliveness check: %s\n", "timeout");
        }
      else
        log_error ("failed to acquire the start_scd lock while"
                   " doing an aliveness check: %s\n", strerror (errno));
      pth_event_free (evt, PTH_FREE_THIS);
      return;
    }
  pth_event_free (evt, PTH_FREE_THIS);

  if (primary_scd_ctx)
    {
      pid = assuan_get_pid (primary_scd_ctx);
      if (pid != (pid_t)(-1) && pid
          && ((rc=waitpid (pid, NULL, WNOHANG))==-1 || (rc == pid)) )
        {
          /* Okay, scdaemon died.  Disconnect the primary connection
             now but take care that it won't do another wait. Also
             cleanup all other connections and release their
             resources.  The next use will start a new daemon then.
             Due to the use of the START_SCD_LOCAL we are sure that
             none of these context are actually in use. */
          struct scd_local_s *sl;

          assuan_set_flag (primary_scd_ctx, ASSUAN_NO_WAITPID, 1);
          assuan_disconnect (primary_scd_ctx);

          for (sl=scd_local_list; sl; sl = sl->next_local)
            {
              if (sl->ctx)
                {
                  if (sl->ctx != primary_scd_ctx)
                    assuan_disconnect (sl->ctx);
                  sl->ctx = NULL;
                }
            }
          
          primary_scd_ctx = NULL;
          primary_scd_ctx_reusable = 0;

          xfree (socket_name);
          socket_name = NULL;
        }
    }

  if (!pth_mutex_release (&start_scd_lock))
    log_error ("failed to release the start_scd lock while"
               " doing the aliveness check: %s\n", strerror (errno));
}



/* Reset the SCD if it has been used.  Actually it is not a reset but
   a cleanup of resources used by the current connection. */
int
agent_reset_scd (ctrl_t ctrl)
{
  if (ctrl->scd_local)
    {
      if (ctrl->scd_local->ctx)
        {
          /* We can't disconnect the primary context because libassuan
             does a waitpid on it and thus the system would hang.
             Instead we send a reset and keep that connection for
             reuse. */
          if (ctrl->scd_local->ctx == primary_scd_ctx)
            {
              /* Send a RESTART to the SCD.  This is required for the
                 primary connection as a kind of virtual EOF; we don't
                 have another way to tell it that the next command
                 should be viewed as if a new connection has been
                 made.  For the non-primary connections this is not
                 needed as we simply close the socket.  We don't check
                 for an error here because the RESTART may fail for
                 example if the scdaemon has already been terminated.
                 Anyway, we need to set the reusable flag to make sure
                 that the aliveness check can clean it up. */
              assuan_transact (primary_scd_ctx, "RESTART",
                               NULL, NULL, NULL, NULL, NULL, NULL);
              primary_scd_ctx_reusable = 1;
            }
          else
            assuan_disconnect (ctrl->scd_local->ctx);
          ctrl->scd_local->ctx = NULL;
        }
      
      /* Remove the local context from our list and release it. */
      if (!scd_local_list)
        BUG ();
      else if (scd_local_list == ctrl->scd_local)
        scd_local_list = ctrl->scd_local->next_local;
      else
        {
          struct scd_local_s *sl;
      
          for (sl=scd_local_list; sl->next_local; sl = sl->next_local)
            if (sl->next_local == ctrl->scd_local)
              break;
          if (!sl->next_local)
            BUG ();
          sl->next_local = ctrl->scd_local->next_local;
        }
      xfree (ctrl->scd_local);
      ctrl->scd_local = NULL;
    }

  return 0;
}



/* Return a new malloced string by unescaping the string S.  Escaping
   is percent escaping and '+'/space mapping.  A binary Nul will
   silently be replaced by a 0xFF.  Function returns NULL to indicate
   an out of memory status. */
static char *
unescape_status_string (const unsigned char *s)
{
  char *buffer, *d;

  buffer = d = xtrymalloc (strlen ((const char*)s)+1);
  if (!buffer)
    return NULL;
  while (*s)
    {
      if (*s == '%' && s[1] && s[2])
        { 
          s++;
          *d = xtoi_2 (s);
          if (!*d)
            *d = '\xff';
          d++;
          s += 2;
        }
      else if (*s == '+')
        {
          *d++ = ' ';
          s++;
        }
      else
        *d++ = *s++;
    }
  *d = 0; 
  return buffer;
}



static int
learn_status_cb (void *opaque, const char *line)
{
  struct learn_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;
  if (keywordlen == 8 && !memcmp (keyword, "CERTINFO", keywordlen))
    {
      parm->certinfo_cb (parm->certinfo_cb_arg, line);
    }
  else if (keywordlen == 11 && !memcmp (keyword, "KEYPAIRINFO", keywordlen))
    {
      parm->kpinfo_cb (parm->kpinfo_cb_arg, line);
    }
  else if (keywordlen && *line)
    {
      parm->sinfo_cb (parm->sinfo_cb_arg, keyword, keywordlen, line);
    }
  
  return 0;
}

/* Perform the LEARN command and return a list of all private keys
   stored on the card. */
int
agent_card_learn (ctrl_t ctrl,
                  void (*kpinfo_cb)(void*, const char *),
                  void *kpinfo_cb_arg,
                  void (*certinfo_cb)(void*, const char *),
                  void *certinfo_cb_arg,
                  void (*sinfo_cb)(void*, const char *, size_t, const char *),
                  void *sinfo_cb_arg)
{
  int rc;
  struct learn_parm_s parm;

  rc = start_scd (ctrl);
  if (rc)
    return rc;

  memset (&parm, 0, sizeof parm);
  parm.kpinfo_cb = kpinfo_cb;
  parm.kpinfo_cb_arg = kpinfo_cb_arg;
  parm.certinfo_cb = certinfo_cb;
  parm.certinfo_cb_arg = certinfo_cb_arg;
  parm.sinfo_cb = sinfo_cb;
  parm.sinfo_cb_arg = sinfo_cb_arg;
  rc = assuan_transact (ctrl->scd_local->ctx, "LEARN --force",
                        NULL, NULL, NULL, NULL,
                        learn_status_cb, &parm);
  if (rc)
    return unlock_scd (ctrl, rc);

  return unlock_scd (ctrl, 0);
}



static int
get_serialno_cb (void *opaque, const char *line)
{
  char **serialno = opaque;
  const char *keyword = line;
  const char *s;
  int keywordlen, n;

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == 8 && !memcmp (keyword, "SERIALNO", keywordlen))
    {
      if (*serialno)
        return gpg_error (GPG_ERR_CONFLICT); /* Unexpected status line. */
      for (n=0,s=line; hexdigitp (s); s++, n++)
        ;
      if (!n || (n&1)|| !(spacep (s) || !*s) )
        return gpg_error (GPG_ERR_ASS_PARAMETER);
      *serialno = xtrymalloc (n+1);
      if (!*serialno)
        return out_of_core ();
      memcpy (*serialno, line, n);
      (*serialno)[n] = 0;
    }
  
  return 0;
}

/* Return the serial number of the card or an appropriate error.  The
   serial number is returned as a hexstring. */
int
agent_card_serialno (ctrl_t ctrl, char **r_serialno)
{
  int rc;
  char *serialno = NULL;

  rc = start_scd (ctrl);
  if (rc)
    return rc;

  rc = assuan_transact (ctrl->scd_local->ctx, "SERIALNO",
                        NULL, NULL, NULL, NULL,
                        get_serialno_cb, &serialno);
  if (rc)
    {
      xfree (serialno);
      return unlock_scd (ctrl, rc);
    }
  *r_serialno = serialno;
  return unlock_scd (ctrl, 0);
}




static int
membuf_data_cb (void *opaque, const void *buffer, size_t length)
{
  membuf_t *data = opaque;

  if (buffer)
    put_membuf (data, buffer, length);
  return 0;
}
  
/* Handle the NEEDPIN inquiry. */
static int
inq_needpin (void *opaque, const char *line)
{
  struct inq_needpin_s *parm = opaque;
  char *pin;
  size_t pinlen;
  int rc;

  if (!strncmp (line, "NEEDPIN", 7) && (line[7] == ' ' || !line[7]))
    {
      line += 7;
      while (*line == ' ')
        line++;
      
      pinlen = 90;
      pin = gcry_malloc_secure (pinlen);
      if (!pin)
        return out_of_core ();

      rc = parm->getpin_cb (parm->getpin_cb_arg, line, pin, pinlen);
      if (!rc)
        rc = assuan_send_data (parm->ctx, pin, pinlen);
      xfree (pin);
    }
  else if (!strncmp (line, "KEYPADINFO", 10) && (line[10] == ' ' || !line[10]))
    {
      size_t code;
      char *endp;

      code = strtoul (line+10, &endp, 10);
      line = endp;
      while (*line == ' ')
        line++;
      
      rc = parm->getpin_cb (parm->getpin_cb_arg, line, NULL, code);
    }
  else
    {
      log_error ("unsupported inquiry `%s'\n", line);
      rc = gpg_error (GPG_ERR_ASS_UNKNOWN_INQUIRE);
    }

  return rc;
}



/* Create a signature using the current card */
int
agent_card_pksign (ctrl_t ctrl,
                   const char *keyid,
                   int (*getpin_cb)(void *, const char *, char*, size_t),
                   void *getpin_cb_arg,
                   const unsigned char *indata, size_t indatalen,
                   unsigned char **r_buf, size_t *r_buflen)
{
  int rc, i;
  char *p, line[ASSUAN_LINELENGTH];
  membuf_t data;
  struct inq_needpin_s inqparm;
  size_t len;
  unsigned char *sigbuf;
  size_t sigbuflen;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  if (indatalen*2 + 50 > DIM(line))
    return unlock_scd (ctrl, gpg_error (GPG_ERR_GENERAL));

  sprintf (line, "SETDATA ");
  p = line + strlen (line);
  for (i=0; i < indatalen ; i++, p += 2 )
    sprintf (p, "%02X", indata[i]);
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc)
    return unlock_scd (ctrl, rc);

  init_membuf (&data, 1024);
  inqparm.ctx = ctrl->scd_local->ctx;
  inqparm.getpin_cb = getpin_cb;
  inqparm.getpin_cb_arg = getpin_cb_arg;
  snprintf (line, DIM(line)-1, 
            ctrl->use_auth_call? "PKAUTH %s":"PKSIGN %s", keyid);
  line[DIM(line)-1] = 0;
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        membuf_data_cb, &data,
                        inq_needpin, &inqparm,
                        NULL, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  sigbuf = get_membuf (&data, &sigbuflen);

  /* Create an S-expression from it which is formatted like this:
     "(7:sig-val(3:rsa(1:sSIGBUFLEN:SIGBUF)))" */
  *r_buflen = 21 + 11 + sigbuflen + 4;
  p = xtrymalloc (*r_buflen);
  *r_buf = (unsigned char*)p;
  if (!p)
    return unlock_scd (ctrl, out_of_core ());
  p = stpcpy (p, "(7:sig-val(3:rsa(1:s" );
  sprintf (p, "%u:", (unsigned int)sigbuflen);
  p += strlen (p);
  memcpy (p, sigbuf, sigbuflen);
  p += sigbuflen;
  strcpy (p, ")))");
  xfree (sigbuf);

  assert (gcry_sexp_canon_len (*r_buf, *r_buflen, NULL, NULL));
  return unlock_scd (ctrl, 0);
}

/* Decipher INDATA using the current card. Note that the returned value is */
int
agent_card_pkdecrypt (ctrl_t ctrl,
                      const char *keyid,
                      int (*getpin_cb)(void *, const char *, char*, size_t),
                      void *getpin_cb_arg,
                      const unsigned char *indata, size_t indatalen,
                      char **r_buf, size_t *r_buflen)
{
  int rc, i;
  char *p, line[ASSUAN_LINELENGTH];
  membuf_t data;
  struct inq_needpin_s inqparm;
  size_t len;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  /* FIXME: use secure memory where appropriate */
  if (indatalen*2 + 50 > DIM(line))
    return unlock_scd (ctrl, gpg_error (GPG_ERR_GENERAL));

  sprintf (line, "SETDATA ");
  p = line + strlen (line);
  for (i=0; i < indatalen ; i++, p += 2 )
    sprintf (p, "%02X", indata[i]);
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        NULL, NULL, NULL, NULL, NULL, NULL);
  if (rc)
    return unlock_scd (ctrl, rc);

  init_membuf (&data, 1024);
  inqparm.ctx = ctrl->scd_local->ctx;
  inqparm.getpin_cb = getpin_cb;
  inqparm.getpin_cb_arg = getpin_cb_arg;
  snprintf (line, DIM(line)-1, "PKDECRYPT %s", keyid);
  line[DIM(line)-1] = 0;
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        membuf_data_cb, &data,
                        inq_needpin, &inqparm,
                        NULL, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  *r_buf = get_membuf (&data, r_buflen);
  if (!*r_buf)
    return unlock_scd (ctrl, gpg_error (GPG_ERR_ENOMEM));

  return unlock_scd (ctrl, 0);
}



/* Read a certificate with ID into R_BUF and R_BUFLEN. */
int
agent_card_readcert (ctrl_t ctrl,
                     const char *id, char **r_buf, size_t *r_buflen)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  init_membuf (&data, 1024);
  snprintf (line, DIM(line)-1, "READCERT %s", id);
  line[DIM(line)-1] = 0;
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        membuf_data_cb, &data,
                        NULL, NULL,
                        NULL, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  *r_buf = get_membuf (&data, r_buflen);
  if (!*r_buf)
    return unlock_scd (ctrl, gpg_error (GPG_ERR_ENOMEM));

  return unlock_scd (ctrl, 0);
}



/* Read a key with ID and return it in an allocate buffer pointed to
   by r_BUF as a valid S-expression. */
int
agent_card_readkey (ctrl_t ctrl, const char *id, unsigned char **r_buf)
{
  int rc;
  char line[ASSUAN_LINELENGTH];
  membuf_t data;
  size_t len, buflen;

  *r_buf = NULL;
  rc = start_scd (ctrl);
  if (rc)
    return rc;

  init_membuf (&data, 1024);
  snprintf (line, DIM(line)-1, "READKEY %s", id);
  line[DIM(line)-1] = 0;
  rc = assuan_transact (ctrl->scd_local->ctx, line,
                        membuf_data_cb, &data,
                        NULL, NULL,
                        NULL, NULL);
  if (rc)
    {
      xfree (get_membuf (&data, &len));
      return unlock_scd (ctrl, rc);
    }
  *r_buf = get_membuf (&data, &buflen);
  if (!*r_buf)
    return unlock_scd (ctrl, gpg_error (GPG_ERR_ENOMEM));

  if (!gcry_sexp_canon_len (*r_buf, buflen, NULL, NULL))
    {
      xfree (*r_buf); *r_buf = NULL;
      return unlock_scd (ctrl, gpg_error (GPG_ERR_INV_VALUE));
    }

  return unlock_scd (ctrl, 0);
}



/* Type used with the card_getattr_cb.  */
struct card_getattr_parm_s {
  const char *keyword;  /* Keyword to look for.  */
  size_t keywordlen;    /* strlen of KEYWORD.  */
  char *data;           /* Malloced and unescaped data.  */
  int error;            /* ERRNO value or 0 on success. */
};

/* Callback function for agent_card_getattr.  */
static assuan_error_t
card_getattr_cb (void *opaque, const char *line)
{
  struct card_getattr_parm_s *parm = opaque;
  const char *keyword = line;
  int keywordlen;

  if (parm->data)
    return 0; /* We want only the first occurrence.  */

  for (keywordlen=0; *line && !spacep (line); line++, keywordlen++)
    ;
  while (spacep (line))
    line++;

  if (keywordlen == parm->keywordlen
      && !memcmp (keyword, parm->keyword, keywordlen))
    {
      parm->data = unescape_status_string ((const unsigned char*)line);
      if (!parm->data)
        parm->error = errno;
    }
  
  return 0;
}


/* Call the agent to retrieve a single line data object. On success
   the object is malloced and stored at RESULT; it is guaranteed that
   NULL is never stored in this case.  On error an error code is
   returned and NULL stored at RESULT. */
gpg_error_t
agent_card_getattr (ctrl_t ctrl, const char *name, char **result)
{
  int err;
  struct card_getattr_parm_s parm;
  char line[ASSUAN_LINELENGTH];

  *result = NULL;

  if (!*name)
    return gpg_error (GPG_ERR_INV_VALUE);

  memset (&parm, 0, sizeof parm);
  parm.keyword = name;
  parm.keywordlen = strlen (name);

  /* We assume that NAME does not need escaping. */
  if (8 + strlen (name) > DIM(line)-1)
    return gpg_error (GPG_ERR_TOO_LARGE);
  stpcpy (stpcpy (line, "GETATTR "), name); 

  err = start_scd (ctrl);
  if (err)
    return err;

  err = assuan_transact (ctrl->scd_local->ctx, line,
                         NULL, NULL, NULL, NULL,
                         card_getattr_cb, &parm);
  if (!err && parm.error)
    err = gpg_error_from_errno (parm.error);
  
  if (!err && !parm.data)
    err = gpg_error (GPG_ERR_NO_DATA);
  
  if (!err)
    *result = parm.data;
  else
    xfree (parm.data);

  return unlock_scd (ctrl, err);
}




static int
pass_status_thru (void *opaque, const char *line)
{
  assuan_context_t ctx = opaque;
  char keyword[200];
  int i;

  for (i=0; *line && !spacep (line) && i < DIM(keyword)-1; line++, i++)
    keyword[i] = *line;
  keyword[i] = 0;
  /* truncate any remaining keyword stuff. */
  for (; *line && !spacep (line); line++)
    ;
  while (spacep (line))
    line++;

  assuan_write_status (ctx, keyword, line);
  return 0;
}

static int
pass_data_thru (void *opaque, const void *buffer, size_t length)
{
  assuan_context_t ctx = opaque;

  assuan_send_data (ctx, buffer, length);
  return 0;
}


/* Send the line CMDLINE with command for the SCDdaemon to it and send
   all status messages back.  This command is used as a general quoting
   mechanism to pass everything verbatim to SCDAEMOPN.  The PIN
   inquirey is handled inside gpg-agent. */
int
agent_card_scd (ctrl_t ctrl, const char *cmdline,
                int (*getpin_cb)(void *, const char *, char*, size_t),
                void *getpin_cb_arg, void *assuan_context)
{
  int rc;
  struct inq_needpin_s inqparm;

  rc = start_scd (ctrl);
  if (rc)
    return rc;

  inqparm.ctx = ctrl->scd_local->ctx;
  inqparm.getpin_cb = getpin_cb;
  inqparm.getpin_cb_arg = getpin_cb_arg;
  rc = assuan_transact (ctrl->scd_local->ctx, cmdline,
                        pass_data_thru, assuan_context,
                        inq_needpin, &inqparm,
                        pass_status_thru, assuan_context);
  if (rc)
    {
      return unlock_scd (ctrl, rc);
    }

  return unlock_scd (ctrl, 0);
}

