/*
 * Copyright (C) 2011 Free Software Foundation, Inc.
 *
 * Author: Nikos Mavrogiannopoulos
 *
 * This file is part of GnuTLS.
 *
 * The GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 *
 */

/* This file contains the types and prototypes for the X.509
 * certificate and CRL handling functions.
 */

#ifndef GNUTLS_DTLS_H
#define GNUTLS_DTLS_H

#include <gnutls/gnutls.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define GNUTLS_COOKIE_KEY_SIZE 16

void gnutls_dtls_set_timeouts (gnutls_session_t session, unsigned int retrans_timeout,
  unsigned int total_timeout);

unsigned int gnutls_dtls_get_mtu (gnutls_session_t session);
unsigned int gnutls_dtls_get_data_mtu (gnutls_session_t session);

void gnutls_dtls_set_mtu (gnutls_session_t session, unsigned int mtu);

typedef struct {
  unsigned int record_seq;
  unsigned int hsk_read_seq;
  unsigned int hsk_write_seq;
} gnutls_dtls_prestate_st;

int gnutls_dtls_cookie_send(gnutls_datum_t* key, void* client_data, size_t client_data_size, 
  gnutls_dtls_prestate_st* state,
  gnutls_transport_ptr_t ptr, gnutls_push_func push_func);


int gnutls_dtls_cookie_verify(gnutls_datum_t* key, 
  void* client_data, size_t client_data_size, 
  void* _msg, size_t msg_size, gnutls_dtls_prestate_st* state);

void gnutls_dtls_prestate_set(gnutls_session_t session, gnutls_dtls_prestate_st* st);

#ifdef __cplusplus
}
#endif

#endif                          /* GNUTLS_DTLS_H */
