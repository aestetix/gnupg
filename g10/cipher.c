/* cipher.c - En-/De-ciphering filter
 *	Copyright (C) 1998 Free Software Foundation, Inc.
 *
 * This file is part of GNUPG.
 *
 * GNUPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GNUPG is distributed in the hope that it will be useful,
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
#include <assert.h>

#include "errors.h"
#include "iobuf.h"
#include "memory.h"
#include "util.h"
#include "filter.h"
#include "packet.h"
#include "options.h"




/****************
 * This filter is used to en/de-cipher data with a conventional algorithm
 */
int
cipher_filter( void *opaque, int control,
	       IOBUF a, byte *buf, size_t *ret_len)
{
    size_t size = *ret_len;
    cipher_filter_context_t *cfx = opaque;
    int rc=0;

    if( control == IOBUFCTRL_UNDERFLOW ) { /* decrypt */
	rc = -1; /* not yet used */
    }
    else if( control == IOBUFCTRL_FLUSH ) { /* encrypt */
	assert(a);
	if( !cfx->header ) {
	    PACKET pkt;
	    PKT_encrypted ed;
	    byte temp[10];

	    memset( &ed, 0, sizeof ed );
	    ed.len = cfx->datalen;
	    init_packet( &pkt );
	    pkt.pkttype = PKT_ENCRYPTED;
	    pkt.pkt.encrypted = &ed;
	    if( build_packet( a, &pkt ))
		log_bug("build_packet(ENCR_DATA) failed\n");
	    randomize_buffer( temp, 8, 1 );
	    temp[8] = temp[6];
	    temp[9] = temp[7];
	    cfx->cipher_hd = cipher_open( cfx->dek->algo,
					  CIPHER_MODE_AUTO_CFB, 1 );
	    cipher_setkey( cfx->cipher_hd, cfx->dek->key, cfx->dek->keylen );
	    cipher_setiv( cfx->cipher_hd, NULL );
	    cipher_encrypt( cfx->cipher_hd, temp, temp, 10);
	    cipher_sync( cfx->cipher_hd );
	    iobuf_write(a, temp, 10);
	    cfx->header=1;
	}
	cipher_encrypt( cfx->cipher_hd, buf, buf, size);
	if( iobuf_write( a, buf, size ) )
	    rc = G10ERR_WRITE_FILE;
    }
    else if( control == IOBUFCTRL_FREE ) {
	cipher_close(cfx->cipher_hd);
    }
    else if( control == IOBUFCTRL_DESC ) {
	*(char**)buf = "cipher_filter";
    }
    return rc;
}




