/* sign.c - sign data
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
#include "iobuf.h"
#include "keydb.h"
#include "memory.h"
#include "util.h"
#include "main.h"
#include "filter.h"
#include "ttyio.h"
#include "i18n.h"


static int
do_sign( PKT_secret_key *sk, PKT_signature *sig,
	 MD_HANDLE md, int digest_algo )
{
    MPI frame;
    byte *dp;
    int rc;

    if( !digest_algo )
	digest_algo = md_get_algo(md);

    dp = md_read( md, digest_algo );
    sig->digest_algo = digest_algo;
    sig->digest_start[0] = dp[0];
    sig->digest_start[1] = dp[1];
    frame = encode_md_value( sk->pubkey_algo, md,
			     digest_algo, mpi_get_nbits(sk->skey[0]));
    rc = pubkey_sign( sk->pubkey_algo, sig->data, frame, sk->skey );
    mpi_free(frame);
    if( rc )
	log_error("pubkey_sign failed: %s\n", g10_errstr(rc) );
    else {
	if( opt.verbose ) {
	    char *ustr = get_user_id_string( sig->keyid );
	    log_info("%s signature from: %s\n",
		      pubkey_algo_to_string(sk->pubkey_algo), ustr );
	    m_free(ustr);
	}
    }
    return rc;
}



int
complete_sig( PKT_signature *sig, PKT_secret_key *sk, MD_HANDLE md )
{
    int rc=0;

    if( !(rc=check_secret_key( sk )) )
	rc = do_sign( sk, sig, md, 0 );

    /* fixme: should we check whether the signature is okay?
     * maybe by using an option */

    return rc;
}

static int
hash_for(int pubkey_algo )
{
    if( opt.def_digest_algo )
	return opt.def_digest_algo;
    if( pubkey_algo == PUBKEY_ALGO_DSA )
	return DIGEST_ALGO_SHA1;
    if( pubkey_algo == PUBKEY_ALGO_RSA )
	return DIGEST_ALGO_MD5;
    return DEFAULT_DIGEST_ALGO;
}

static int
only_old_style( SK_LIST sk_list )
{
    SK_LIST sk_rover = NULL;
    int old_style = 0;

    /* if there are only old style capable key we use the old sytle */
    for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	PKT_secret_key *sk = sk_rover->sk;
	if( sk->pubkey_algo == PUBKEY_ALGO_RSA && sk->version < 4 )
	    old_style = 1;
	else
	    return 0;
    }
    return old_style;
}

/****************
 * Sign the files whose names are in FILENAME.
 * If DETACHED has the value true,
 * make a detached signature.  If FILENAMES->d is NULL read from stdin
 * and ignore the detached mode.  Sign the file with all secret keys
 * which can be taken from LOCUSR, if this is NULL, use the default one
 * If ENCRYPT is true, use REMUSER (or ask if it is NULL) to encrypt the
 * signed data for these users.
 * If OUTFILE is not NULL; this file is used for output and the function
 * does not ask for overwrite permission; output is then always
 * uncompressed, non-armored and in binary mode.
 */
int
sign_file( STRLIST filenames, int detached, STRLIST locusr,
	   int encrypt, STRLIST remusr, const char *outfile )
{
    const char *fname;
    armor_filter_context_t afx;
    compress_filter_context_t zfx;
    md_filter_context_t mfx;
    text_filter_context_t tfx;
    encrypt_filter_context_t efx;
    IOBUF inp = NULL, out = NULL;
    PACKET pkt;
    PKT_plaintext *pt = NULL;
    u32 filesize;
    int rc = 0;
    PK_LIST pk_list = NULL;
    SK_LIST sk_list = NULL;
    SK_LIST sk_rover = NULL;
    int multifile = 0;
    int old_style = opt.rfc1991;


    memset( &afx, 0, sizeof afx);
    memset( &zfx, 0, sizeof zfx);
    memset( &mfx, 0, sizeof mfx);
    memset( &tfx, 0, sizeof tfx);
    memset( &efx, 0, sizeof efx);
    init_packet( &pkt );

    if( filenames ) {
	fname = filenames->d;
	multifile = !!filenames->next;
    }
    else
	fname = NULL;

    if( fname && filenames->next && (!detached || encrypt) )
	log_bug("multiple files can only be detached signed");

    if( (rc=build_sk_list( locusr, &sk_list, 1, 1 )) )
	goto leave;
    if( !old_style )
	old_style = only_old_style( sk_list );
    if( encrypt ) {
	if( (rc=build_pk_list( remusr, &pk_list, 2 )) )
	    goto leave;
    }

    /* prepare iobufs */
    if( multifile )  /* have list of filenames */
	inp = NULL; /* we do it later */
    else if( !(inp = iobuf_open(fname)) ) {
	log_error("can't open %s: %s\n", fname? fname: "[stdin]",
					strerror(errno) );
	rc = G10ERR_OPEN_FILE;
	goto leave;
    }

    if( outfile ) {
	if( !(out = iobuf_create( outfile )) ) {
	    log_error("can't create %s: %s\n", outfile, strerror(errno) );
	    rc = G10ERR_CREATE_FILE;
	    goto leave;
	}
	else if( opt.verbose )
	    log_info("writing to '%s'\n", outfile );
    }
    else if( !(out = open_outfile( fname, opt.armor? 1: detached? 2:0 )) ) {
	rc = G10ERR_CREATE_FILE;
	goto leave;
    }

    /* prepare to calculate the MD over the input */
    if( opt.textmode && !outfile )
	iobuf_push_filter( inp, text_filter, &tfx );
    mfx.md = md_open(0, 0);

    for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	PKT_secret_key *sk = sk_rover->sk;
	md_enable(mfx.md, hash_for(sk->pubkey_algo));
    }

    if( !multifile )
	iobuf_push_filter( inp, md_filter, &mfx );

    if( opt.armor && !outfile  )
	iobuf_push_filter( out, armor_filter, &afx );
    else
	write_comment( out, "#created by GNUPG v" VERSION " ("
					    PRINTABLE_OS_NAME ")");
    if( encrypt ) {
	efx.pk_list = pk_list;
	/* fixme: set efx.cfx.datalen if known */
	iobuf_push_filter( out, encrypt_filter, &efx );
    }

    if( opt.compress && !outfile ) {
	if( old_style )
	    zfx.algo = 1;
	iobuf_push_filter( out, compress_filter, &zfx );
    }

    if( !detached && !old_style ) {
	/* loop over the secret certificates and build headers */
	for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	    PKT_secret_key *sk;
	    PKT_onepass_sig *ops;

	    sk = sk_rover->sk;
	    ops = m_alloc_clear( sizeof *ops );
	    ops->sig_class = opt.textmode && !outfile ? 0x01 : 0x00;
	    ops->digest_algo = hash_for(sk->pubkey_algo);
	    ops->pubkey_algo = sk->pubkey_algo;
	    keyid_from_sk( sk, ops->keyid );
	    ops->last = !sk_rover->next;

	    init_packet(&pkt);
	    pkt.pkttype = PKT_ONEPASS_SIG;
	    pkt.pkt.onepass_sig = ops;
	    rc = build_packet( out, &pkt );
	    free_packet( &pkt );
	    if( rc ) {
		log_error("build onepass_sig packet failed: %s\n",
							g10_errstr(rc));
		goto leave;
	    }
	}
    }


    /* setup the inner packet */
    if( detached ) {
	if( multifile ) {
	    STRLIST sl;

	    if( opt.verbose )
		log_info("signing:" );
	    /* must walk reverse trough this list */
	    for( sl = strlist_last(filenames); sl;
			sl = strlist_prev( filenames, sl ) ) {
		if( !(inp = iobuf_open(sl->d)) ) {
		    log_error("can't open %s: %s\n", sl->d, strerror(errno) );
		    rc = G10ERR_OPEN_FILE;
		    goto leave;
		}
		if( opt.verbose )
		    fprintf(stderr, " '%s'", sl->d );
		iobuf_push_filter( inp, md_filter, &mfx );
		while( iobuf_get(inp) != -1 )
		    ;
		iobuf_close(inp); inp = NULL;
	    }
	    if( opt.verbose )
		putc( '\n', stderr );
	}
	else {
	    /* read, so that the filter can calculate the digest */
	    while( iobuf_get(inp) != -1 )
		;
	}
    }
    else {
	if( fname ) {
	    pt = m_alloc( sizeof *pt + strlen(fname) - 1 );
	    pt->namelen = strlen(fname);
	    memcpy(pt->name, fname, pt->namelen );
	    if( !(filesize = iobuf_get_filelength(inp)) )
		log_info("warning: '%s' is an empty file\n", fname );

	    /* because the text_filter modifies the length of the
	     * data, it is not possible to know the used length
	     * without a double read of the file - to avoid that
	     * we simple use partial length packets.
	     * FIXME: We have to do the double read when opt.rfc1991
	     *	      is active.
	     */
	    if( opt.textmode && !outfile )
		filesize = 0;
	}
	else { /* no filename */
	    pt = m_alloc( sizeof *pt - 1 );
	    pt->namelen = 0;
	    filesize = 0; /* stdin */
	}
	pt->timestamp = make_timestamp();
	pt->mode = opt.textmode && !outfile ? 't':'b';
	pt->len = filesize;
	pt->buf = inp;
	pkt.pkttype = PKT_PLAINTEXT;
	pkt.pkt.plaintext = pt;
	/*cfx.datalen = filesize? calc_packet_length( &pkt ) : 0;*/
	if( (rc = build_packet( out, &pkt )) )
	    log_error("build_packet(PLAINTEXT) failed: %s\n", g10_errstr(rc) );
	pt->buf = NULL;
    }

    /* loop over the secret certificates */
    for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	PKT_secret_key *sk;
	PKT_signature *sig;
	MD_HANDLE md;

	sk = sk_rover->sk;

	/* build the signature packet */
	/* fixme: this code is partly duplicated in make_keysig_packet */
	sig = m_alloc_clear( sizeof *sig );
	sig->version = sk->version;
	keyid_from_sk( sk, sig->keyid );
	sig->digest_algo = hash_for(sk->pubkey_algo);
	sig->pubkey_algo = sk->pubkey_algo;
	sig->timestamp = make_timestamp();
	sig->sig_class = opt.textmode && !outfile? 0x01 : 0x00;

	md = md_copy( mfx.md );

	if( sig->version >= 4 ) {
	    build_sig_subpkt_from_sig( sig );
	    md_putc( md, sig->version );
	}
	md_putc( md, sig->sig_class );
	if( sig->version < 4 ) {
	    u32 a = sig->timestamp;
	    md_putc( md, (a >> 24) & 0xff );
	    md_putc( md, (a >> 16) & 0xff );
	    md_putc( md, (a >>	8) & 0xff );
	    md_putc( md,  a	   & 0xff );
	}
	else {
	    byte buf[6];
	    size_t n;

	    md_putc( md, sig->pubkey_algo );
	    md_putc( md, sig->digest_algo );
	    if( sig->hashed_data ) {
		n = (sig->hashed_data[0] << 8) | sig->hashed_data[1];
		md_write( md, sig->hashed_data, n+2 );
		n += 6;
	    }
	    else
		n = 6;
	    /* add some magic */
	    buf[0] = sig->version;
	    buf[1] = 0xff;
	    buf[2] = n >> 24; /* hmmm, n is only 16 bit, so this is always 0 */
	    buf[3] = n >> 16;
	    buf[4] = n >>  8;
	    buf[5] = n;
	    md_write( md, buf, 6 );

	}
	md_final( md );

	rc = do_sign( sk, sig, md, hash_for(sig->pubkey_algo) );
	md_close( md );

	if( !rc ) { /* and write it */
	    init_packet(&pkt);
	    pkt.pkttype = PKT_SIGNATURE;
	    pkt.pkt.signature = sig;
	    rc = build_packet( out, &pkt );
	    free_packet( &pkt );
	    if( rc )
		log_error("build signature packet failed: %s\n", g10_errstr(rc) );
	}
	if( rc )
	    goto leave;
    }


  leave:
    if( rc )
	iobuf_cancel(out);
    else
	iobuf_close(out);
    iobuf_close(inp);
    md_close( mfx.md );
    release_sk_list( sk_list );
    release_pk_list( pk_list );
    return rc;
}



/****************
 * note: we do not count empty lines at the beginning
 */
static int
write_dash_escaped( IOBUF inp, IOBUF out, MD_HANDLE md )
{
    int c;
    int lastlf = 1;
    int skip_empty = 1;

    while( (c = iobuf_get(inp)) != -1 ) {
	/* Note: We don't escape "From " because the MUA should cope with it */
	if( lastlf ) {
	    if( c == '-' ) {
		iobuf_put( out, c );
		iobuf_put( out, ' ' );
		skip_empty = 0;
	    }
	    else if( skip_empty && c == '\r' )
		skip_empty = 2;
	    else
		skip_empty = 0;
	}

	if( !skip_empty )
	    md_putc(md, c );
	iobuf_put( out, c );
	lastlf = c == '\n';
	if( skip_empty == 2 )
	    skip_empty = lastlf ? 0 : 1;
    }
    return 0; /* fixme: add error handling */
}


/****************
 * make a clear signature. note that opt.armor is not needed
 */
int
clearsign_file( const char *fname, STRLIST locusr, const char *outfile )
{
    armor_filter_context_t afx;
    text_filter_context_t tfx;
    MD_HANDLE textmd = NULL;
    IOBUF inp = NULL, out = NULL;
    PACKET pkt;
    int rc = 0;
    SK_LIST sk_list = NULL;
    SK_LIST sk_rover = NULL;
    int old_style = opt.rfc1991;

    memset( &afx, 0, sizeof afx);
    memset( &tfx, 0, sizeof tfx);
    init_packet( &pkt );

    if( (rc=build_sk_list( locusr, &sk_list, 1, 1 )) )
	goto leave;
    if( !old_style )
	old_style = only_old_style( sk_list );

    /* prepare iobufs */
    if( !(inp = iobuf_open(fname)) ) {
	log_error("can't open %s: %s\n", fname? fname: "[stdin]",
					strerror(errno) );
	rc = G10ERR_OPEN_FILE;
	goto leave;
    }

    if( outfile ) {
	if( !(out = iobuf_create( outfile )) ) {
	    log_error("can't create %s: %s\n", outfile, strerror(errno) );
	    rc = G10ERR_CREATE_FILE;
	    goto leave;
	}
	else if( opt.verbose )
	    log_info("writing to '%s'\n", outfile );
    }
    else if( !(out = open_outfile( fname, 1 )) ) {
	rc = G10ERR_CREATE_FILE;
	goto leave;
    }

    /* FIXME: This stuff is not correct if multiple hash algos are used*/
    iobuf_writestr(out, "-----BEGIN PGP SIGNED MESSAGE-----\n" );
    if( old_style
	|| (opt.def_digest_algo?opt.def_digest_algo:DEFAULT_DIGEST_ALGO)
			      == DIGEST_ALGO_MD5 )
	iobuf_writestr(out, "\n" );
    else {
	const char *s = digest_algo_to_string(opt.def_digest_algo?
				    opt.def_digest_algo:DEFAULT_DIGEST_ALGO);
	assert(s);
	iobuf_writestr(out, "Hash: " );
	iobuf_writestr(out, s );
	iobuf_writestr(out, "\n\n" );
    }


    textmd = md_open(0, 0);
    for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	PKT_secret_key *sk = sk_rover->sk;
	md_enable(textmd, hash_for(sk->pubkey_algo));
    }

    iobuf_push_filter( inp, text_filter, &tfx );
    rc = write_dash_escaped( inp, out, textmd );
    if( rc )
	goto leave;

    iobuf_writestr(out, "\n" );
    afx.what = 2;
    iobuf_push_filter( out, armor_filter, &afx );

    /* loop over the secret certificates */
    for( sk_rover = sk_list; sk_rover; sk_rover = sk_rover->next ) {
	PKT_secret_key *sk;
	PKT_signature *sig;
	MD_HANDLE md;

	sk = sk_rover->sk;

	/* build the signature packet */
	/* fixme: this code is duplicated above */
	sig = m_alloc_clear( sizeof *sig );
	sig->version = sk->version;
	keyid_from_sk( sk, sig->keyid );
	sig->digest_algo = hash_for(sk->pubkey_algo);
	sig->pubkey_algo = sk->pubkey_algo;
	sig->timestamp = make_timestamp();
	sig->sig_class = 0x01;

	md = md_copy( textmd );
	if( sig->version >= 4 ) {
	    build_sig_subpkt_from_sig( sig );
	    md_putc( md, sig->version );
	}
	md_putc( md, sig->sig_class );
	if( sig->version < 4 ) {
	    u32 a = sig->timestamp;
	    md_putc( md, (a >> 24) & 0xff );
	    md_putc( md, (a >> 16) & 0xff );
	    md_putc( md, (a >>	8) & 0xff );
	    md_putc( md,  a	   & 0xff );
	}
	else {
	    byte buf[6];
	    size_t n;

	    md_putc( md, sig->pubkey_algo );
	    md_putc( md, sig->digest_algo );
	    if( sig->hashed_data ) {
		n = (sig->hashed_data[0] << 8) | sig->hashed_data[1];
		md_write( md, sig->hashed_data, n+2 );
		n += 6;
	    }
	    else
		n = 6;
	    /* add some magic */
	    buf[0] = sig->version;
	    buf[1] = 0xff;
	    buf[2] = n >> 24; /* hmmm, n is only 16 bit, so this is always 0 */
	    buf[3] = n >> 16;
	    buf[4] = n >>  8;
	    buf[5] = n;
	    md_write( md, buf, 6 );

	}
	md_final( md );

	rc = do_sign( sk, sig, md, hash_for(sig->pubkey_algo) );
	md_close( md );

	if( !rc ) { /* and write it */
	    init_packet(&pkt);
	    pkt.pkttype = PKT_SIGNATURE;
	    pkt.pkt.signature = sig;
	    rc = build_packet( out, &pkt );
	    free_packet( &pkt );
	    if( rc )
		log_error("build signature packet failed: %s\n", g10_errstr(rc) );
	}
	if( rc )
	    goto leave;
    }


  leave:
    if( rc )
	iobuf_cancel(out);
    else
	iobuf_close(out);
    iobuf_close(inp);
    md_close( textmd );
    release_sk_list( sk_list );
    return rc;
}



