/* export.c
 * Copyright (C) 2002 Free Software Foundation, Inc.
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> 
#include <time.h>
#include <assert.h>

#include <gcrypt.h>
#include <ksba.h>

#include "gpgsm.h"
#include "keydb.h"

static void print_short_info (KsbaCert cert, FILE *fp);



/* Export all certificates or just those given in NAMES. */
void
gpgsm_export (CTRL ctrl, STRLIST names, FILE *fp)
{
  KEYDB_HANDLE hd;
  KEYDB_SEARCH_DESC *desc = NULL;
  int ndesc;
  Base64Context b64writer = NULL;
  KsbaWriter writer;
  STRLIST sl;
  KsbaCert cert = NULL;
  int rc=0;
  int count = 0;

  hd = keydb_new (0);
  if (!hd)
    {
      log_error ("keydb_new failed\n");
      goto leave;
    }

  if (!names)
    ndesc = 1;
  else
    {
      for (sl=names, ndesc=0; sl; sl = sl->next, ndesc++) 
        ;
    }

  desc = xtrycalloc (ndesc, sizeof *desc);
  if (!ndesc)
    {
      log_error ("%s\n", gnupg_strerror (GNUPG_Out_Of_Core));
      goto leave;
    }

  if (!names)
    desc[0].mode = KEYDB_SEARCH_MODE_FIRST;
  else 
    {
      for (ndesc=0, sl=names; sl; sl = sl->next) 
        {
          rc = keydb_classify_name (sl->d, desc+ndesc);
          if (rc)
            {
              log_error ("key `%s' not found: %s\n",
                         sl->d, gnupg_strerror (rc));
              rc = 0;
            }
          else
            ndesc++;
        }
    }


  while (!(rc = keydb_search (hd, desc, ndesc)))
    {
      const unsigned char *image;
      size_t imagelen;

      if (!names) 
        desc[0].mode = KEYDB_SEARCH_MODE_NEXT;

      rc = keydb_get_cert (hd, &cert);
      if (rc) 
        {
          log_error ("keydb_get_cert failed: %s\n", gnupg_strerror (rc));
          goto leave;
        }

      image = ksba_cert_get_image (cert, &imagelen);
      if (!image)
        {
          log_error ("ksba_cert_get_image failed\n");
          goto leave;
        }

      if (ctrl->create_pem)
        {
          if (count)
            putc ('\n', fp);
          print_short_info (cert, fp);
          putc ('\n', stdout);
        }
      count++;

      if (!b64writer)
        {
          ctrl->pem_name = "CERTIFICATE";
          rc = gpgsm_create_writer (&b64writer, ctrl, fp, &writer);
          if (rc)
            {
              log_error ("can't create writer: %s\n", gnupg_strerror (rc));
              goto leave;
            }
        }

      rc = ksba_writer_write (writer, image, imagelen);
      if (rc)
        {
          log_error ("write error: %s\n", ksba_strerror (rc));
          goto leave;
        }

      if (ctrl->create_pem)
        {
          /* We want one certificate per PEM block */
          rc = gpgsm_finish_writer (b64writer);
          if (rc) 
            {
              log_error ("write failed: %s\n", gnupg_strerror (rc));
              goto leave;
            }
          gpgsm_destroy_writer (b64writer);
          b64writer = NULL;
        }
      
      ksba_cert_release (cert); 
      cert = NULL;
    }
  if (rc && rc != -1)
    log_error ("keydb_search failed: %s\n", gnupg_strerror (rc));
  else if (b64writer)
    {
      rc = gpgsm_finish_writer (b64writer);
      if (rc) 
        {
          log_error ("write failed: %s\n", gnupg_strerror (rc));
          goto leave;
        }
    }
  
 leave:
  gpgsm_destroy_writer (b64writer);
  ksba_cert_release (cert);
  xfree (desc);
  keydb_release (hd);
}


/* Print some info about the certifciate CERT to FP */
static void
print_short_info (KsbaCert cert, FILE *fp)
{
  char *p;
  KsbaSexp sexp;
  int idx;

  fputs ("Issuer ...: ", fp); 
  p = ksba_cert_get_issuer (cert, 0);
  if (p)
    {
      print_sanitized_string (fp, p, '\n');
      xfree (p);
      for (idx=1; (p = ksba_cert_get_issuer (cert, idx)); idx++)
        {
          fputs ("\n   aka ...: ", fp); 
          print_sanitized_string (fp, p, '\n');
          xfree (p);
        }
    }
  putc ('\n', stdout);

  fputs ("Serial ...: ", fp); 
  sexp = ksba_cert_get_serial (cert);
  if (sexp)
    {
      int len;
      const unsigned char *s = sexp;
      
      if (*s == '(')
        {
          s++;
          for (len=0; *s && *s != ':' && digitp (s); s++)
            len = len*10 + atoi_1 (s);
          if (*s == ':')
            for (s++; len; len--, s++)
              fprintf (fp, "%02X", *s);
        }
      xfree (sexp);
    }
  putc ('\n', stdout);

  fputs ("Subject ..: ", fp); 
  p = ksba_cert_get_subject (cert, 0);
  if (p)
    {
      print_sanitized_string (fp, p, '\n');
      xfree (p);
      for (idx=1; (p = ksba_cert_get_subject (cert, idx)); idx++)
        {
          fputs ("\n    aka ..: ", fp); 
          print_sanitized_string (fp, p, '\n');
          xfree (p);
        }
    }
  else
    fputs ("none", fp);
  putc ('\n', stdout);
}


