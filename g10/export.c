/* export.c
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

#include "options.h"
#include "packet.h"
#include "errors.h"
#include "keydb.h"
#include "memory.h"
#include "util.h"
#include "main.h"

static int do_export( STRLIST users, int secret );

/****************
 * Export the public keys (to standard out or --output).
 * Depending on opt.armor the output is armored.
 * If USERS is NULL, the complete ring will be exported.
 */
int
export_pubkeys( STRLIST users )
{
    return do_export( users, 0 );
}

int
export_seckeys( STRLIST users )
{
    return do_export( users, 1 );
}

static int
do_export( STRLIST users, int secret )
{
    int rc = 0;
    armor_filter_context_t afx;
    compress_filter_context_t zfx;
    IOBUF out = NULL;
    PACKET pkt;
    KBNODE keyblock = NULL;
    KBNODE kbctx, node;
    KBPOS kbpos;
    STRLIST sl;
    int all = !users;
    int any=0;

    memset( &afx, 0, sizeof afx);
    memset( &zfx, 0, sizeof zfx);
    init_packet( &pkt );

    if( !(out = open_outfile( NULL, 0 )) ) {
	rc = G10ERR_CREATE_FILE;
	goto leave;
    }

    if( opt.armor ) {
	afx.what = secret?5:1;
	iobuf_push_filter( out, armor_filter, &afx );
    }
    if( opt.compress_keys && opt.compress )
	iobuf_push_filter( out, compress_filter, &zfx );

    if( all ) {
	rc = enum_keyblocks( secret?5:0, &kbpos, &keyblock );
	if( rc ) {
	    if( rc != -1 )
		log_error("enum_keyblocks(open) failed: %s\n", g10_errstr(rc) );
	    goto leave;
	}
	all = 2;
    }

    /* use the correct sequence. strlist_last,prev do work correctly with
     * NULL pointers :-) */
    for( sl=strlist_last(users); sl || all ; sl=strlist_prev( users, sl )) {
	if( all ) { /* get the next user */
	    rc = enum_keyblocks( 1, &kbpos, &keyblock );
	    if( rc == -1 )  /* EOF */
		break;
	    if( rc ) {
		log_error("enum_keyblocks(read) failed: %s\n", g10_errstr(rc));
		break;
	    }
	}
	else {
	    /* search the userid */
	    rc = secret? find_secret_keyblock_byname( &kbpos, sl->d )
		       : find_keyblock_byname( &kbpos, sl->d );
	    if( rc ) {
		log_error("%s: user not found: %s\n", sl->d, g10_errstr(rc) );
		rc = 0;
		continue;
	    }
	    /* read the keyblock */
	    rc = read_keyblock( &kbpos, &keyblock );
	}

	if( rc ) {
	    log_error("certificate read problem: %s\n", g10_errstr(rc));
	    goto leave;
	}

	/* and write it */
	for( kbctx=NULL; (node = walk_kbnode( keyblock, &kbctx, 0 )); ) {
	    if( (rc = build_packet( out, node->pkt )) ) {
		log_error("build_packet(%d) failed: %s\n",
			    node->pkt->pkttype, g10_errstr(rc) );
		rc = G10ERR_WRITE_FILE;
		goto leave;
	    }
	}
	any++;
    }
    if( rc == -1 )
	rc = 0;

  leave:
    if( all == 2 )
	enum_keyblocks( 2, &kbpos, &keyblock ); /* close */
    release_kbnode( keyblock );
    if( rc || !any )
	iobuf_cancel(out);
    else
	iobuf_close(out);
    if( !any )
	log_info("warning: nothing exported\n");
    return rc;
}


