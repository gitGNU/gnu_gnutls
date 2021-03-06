/*
 * Copyright (C) 2003-2012 Free Software Foundation, Inc.
 *
 * This file is part of GnuTLS.
 *
 * GnuTLS is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuTLS is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#include <gnutls/openpgp.h>
#include <gnutls/pkcs12.h>
#include <gnutls/pkcs11.h>
#include <gnutls/abstract.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <error.h>
#include <common.h>
#include "certtool-common.h"
#include "certtool-cfg.h"

/* Gnulib portability files. */
#include <read-file.h>

unsigned char buffer[64 * 1024];
const int buffer_size = sizeof (buffer);


FILE *
safe_open_rw (const char *file, int privkey_op)
{
  mode_t omask = 0;
  FILE *fh;

  if (privkey_op != 0)
    {
      omask = umask (S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    }

  fh = fopen (file, "wb");

  if (privkey_op != 0)
    {
      umask (omask);
    }

  return fh;
}

gnutls_datum_t *
load_secret_key (int mand, common_info_st * info)
{
  char raw_key[64];
  size_t raw_key_size = sizeof (raw_key);
  static gnutls_datum_t key;
  gnutls_datum_t hex_key;
  int ret;

  if (info->verbose)
    fprintf (stderr, "Loading secret key...\n");

  if (info->secret_key == NULL)
    {
      if (mand)
        error (EXIT_FAILURE, 0, "missing --secret-key");
      else
        return NULL;
    }

  hex_key.data = (void *) info->secret_key;
  hex_key.size = strlen (info->secret_key);

  ret = gnutls_hex_decode (&hex_key, raw_key, &raw_key_size);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "hex_decode: %s", gnutls_strerror (ret));

  key.data = (void*)raw_key;
  key.size = raw_key_size;

  return &key;
}

const char* get_password(common_info_st * cinfo, unsigned int *flags, int confirm)
{
  if (cinfo->null_password)
    {
      if (flags) *flags |= GNUTLS_PKCS_NULL_PASSWORD;
      return NULL;
    }
  else if (cinfo->password)
    {
      if (cinfo->password[0] == 0 && flags)
        *flags |= GNUTLS_PKCS_PLAIN;
      return cinfo->password;
    }
  else
    {
      if (confirm)
        return get_confirmed_pass (true);
      else
        return get_pass ();
    }
}

static gnutls_privkey_t _load_privkey(gnutls_datum_t *dat, common_info_st * info)
{
int ret;
gnutls_privkey_t key;
unsigned int flags = 0;
const char* pass;

  ret = gnutls_privkey_init (&key);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "privkey_init: %s", gnutls_strerror (ret));

  ret = gnutls_privkey_import_x509_raw (key, dat, info->incert_format, NULL, 0);
  if (ret == GNUTLS_E_DECRYPTION_FAILED)
    {
      pass = get_password (info, &flags, 0);
      ret = gnutls_privkey_import_x509_raw (key, dat, info->incert_format, pass, flags);
    }

  if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR)
    {
      error (EXIT_FAILURE, 0,
             "import error: could not find a valid PEM header; "
             "check if your key is PKCS #12 encoded");
    }

  if (ret < 0)
    error (EXIT_FAILURE, 0, "importing --load-privkey: %s: %s",
           info->privkey, gnutls_strerror (ret));

  return key;
}

static gnutls_privkey_t _load_url_privkey(const char* url)
{
int ret;
gnutls_privkey_t key;

  ret = gnutls_privkey_init (&key);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "privkey_init: %s", gnutls_strerror (ret));

  ret = gnutls_privkey_import_url(key, url, 0);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "importing key: %s: %s",
           url, gnutls_strerror (ret));

  return key;
}

static gnutls_pubkey_t _load_url_pubkey(const char* url)
{
int ret;
gnutls_pubkey_t pubkey;
unsigned int obj_flags = 0;

  ret = gnutls_pubkey_init (&pubkey);
  if (ret < 0)
    {
      fprintf (stderr, "Error in %s:%d: %s\n", __func__, __LINE__,
               gnutls_strerror (ret));
      exit (1);
    }

  ret = gnutls_pubkey_import_url (pubkey, url, obj_flags);
  if (ret < 0)
    {
      fprintf (stderr, "Error in %s:%d: %s: %s\n", __func__, __LINE__,
               gnutls_strerror (ret), url);
      exit (1);
    }

  return pubkey;
}

/* Load the private key.
 * @mand should be non zero if it is required to read a private key.
 */
gnutls_privkey_t
load_private_key (int mand, common_info_st * info)
{
  gnutls_privkey_t key;
  gnutls_datum_t dat;
  size_t size;

  if (!info->privkey && !mand)
    return NULL;

  if (info->privkey == NULL)
    error (EXIT_FAILURE, 0, "missing --load-privkey");

  if (gnutls_url_is_supported(info->privkey) != 0)
    return _load_url_privkey(info->privkey);

  dat.data = (void*)read_binary_file (info->privkey, &size);
  dat.size = size;

  if (!dat.data)
    error (EXIT_FAILURE, errno, "reading --load-privkey: %s", info->privkey);

  key = _load_privkey(&dat, info);

  free (dat.data);

  return key;
}

/* Load the private key.
 * @mand should be non zero if it is required to read a private key.
 */
gnutls_x509_privkey_t
load_x509_private_key (int mand, common_info_st * info)
{
  gnutls_x509_privkey_t key;
  int ret;
  gnutls_datum_t dat;
  size_t size;
  unsigned int flags = 0;
  const char* pass;

  if (!info->privkey && !mand)
    return NULL;

  if (info->privkey == NULL)
    error (EXIT_FAILURE, 0, "missing --load-privkey");

  ret = gnutls_x509_privkey_init (&key);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "privkey_init: %s", gnutls_strerror (ret));

  dat.data = (void*)read_binary_file (info->privkey, &size);
  dat.size = size;

  if (!dat.data)
    error (EXIT_FAILURE, errno, "reading --load-privkey: %s", info->privkey);

  if (info->pkcs8)
    {
      pass = get_password (info, &flags, 0);
      ret =
        gnutls_x509_privkey_import_pkcs8 (key, &dat, info->incert_format,
                                          pass, flags);
    }
  else
    {
      ret = gnutls_x509_privkey_import2 (key, &dat, info->incert_format, NULL, 0);
      if (ret == GNUTLS_E_DECRYPTION_FAILED)
        {
          pass = get_password (info, &flags, 0);
          ret = gnutls_x509_privkey_import2 (key, &dat, info->incert_format, pass, flags);
        }
    }

  free (dat.data);

  if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR)
    {
      error (EXIT_FAILURE, 0,
             "import error: could not find a valid PEM header; "
             "check if your key is PKCS #12 encoded");
    }

  if (ret < 0)
    error (EXIT_FAILURE, 0, "importing --load-privkey: %s: %s",
           info->privkey, gnutls_strerror (ret));

  return key;
}


/* Loads the certificate
 * If mand is non zero then a certificate is mandatory. Otherwise
 * null will be returned if the certificate loading fails.
 */
gnutls_x509_crt_t
load_cert (int mand, common_info_st * info)
{
  gnutls_x509_crt_t *crt;
  size_t size;

  crt = load_cert_list (mand, &size, info);

  return crt ? crt[0] : NULL;
}

#define MAX_CERTS 256

/* Loads a certificate list
 */
gnutls_x509_crt_t *
load_cert_list (int mand, size_t * crt_size, common_info_st * info)
{
  FILE *fd;
  static gnutls_x509_crt_t crt[MAX_CERTS];
  char *ptr;
  int ret, i;
  gnutls_datum_t dat;
  size_t size;
  int ptr_size;

  *crt_size = 0;
  if (info->verbose)
    fprintf (stderr, "Loading certificate list...\n");

  if (info->cert == NULL)
    {
      if (mand)
        error (EXIT_FAILURE, 0, "missing --load-certificate");
      else
        return NULL;
    }

  fd = fopen (info->cert, "r");
  if (fd == NULL)
    error (EXIT_FAILURE, errno, "%s", info->cert);

  size = fread (buffer, 1, sizeof (buffer) - 1, fd);
  buffer[size] = 0;

  fclose (fd);

  ptr = (void*)buffer;
  ptr_size = size;

  for (i = 0; i < MAX_CERTS; i++)
    {
      ret = gnutls_x509_crt_init (&crt[i]);
      if (ret < 0)
        error (EXIT_FAILURE, 0, "crt_init: %s", gnutls_strerror (ret));

      dat.data = (void*)ptr;
      dat.size = ptr_size;

      ret = gnutls_x509_crt_import (crt[i], &dat, info->incert_format);
      if (ret < 0 && *crt_size > 0)
        break;
      if (ret < 0)
        error (EXIT_FAILURE, 0, "crt_import: %s", gnutls_strerror (ret));

      ptr = strstr (ptr, "---END");
      if (ptr == NULL)
        break;
      ptr++;

      ptr_size = size;
      ptr_size -=
        (unsigned int) ((unsigned char *) ptr - (unsigned char *) buffer);

      if (ptr_size < 0)
        break;

      (*crt_size)++;
    }
  if (info->verbose)
    fprintf (stderr, "Loaded %d certificates.\n", (int) *crt_size);

  return crt;
}

/* Load the Certificate Request.
 */
gnutls_x509_crq_t
load_request (common_info_st * info)
{
  gnutls_x509_crq_t crq;
  int ret;
  gnutls_datum_t dat;
  size_t size;

  if (!info->request)
    return NULL;

  ret = gnutls_x509_crq_init (&crq);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "crq_init: %s", gnutls_strerror (ret));

  dat.data = (void*)read_binary_file (info->request, &size);
  dat.size = size;

  if (!dat.data)
    error (EXIT_FAILURE, errno, "reading --load-request: %s", info->request);

  ret = gnutls_x509_crq_import (crq, &dat, info->incert_format);
  if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR)
    {
      error (EXIT_FAILURE, 0,
             "import error: could not find a valid PEM header");
    }

  free (dat.data);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "importing --load-request: %s: %s",
           info->request, gnutls_strerror (ret));

  return crq;
}

/* Load the CA's private key.
 */
gnutls_privkey_t
load_ca_private_key (common_info_st * info)
{
  gnutls_privkey_t key;
  gnutls_datum_t dat;
  size_t size;

  if (info->ca_privkey == NULL)
    error (EXIT_FAILURE, 0, "missing --load-ca-privkey");

  if (gnutls_url_is_supported(info->ca_privkey) != 0)
    return _load_url_privkey(info->ca_privkey);

  dat.data = (void*)read_binary_file (info->ca_privkey, &size);
  dat.size = size;

  if (!dat.data)
    error (EXIT_FAILURE, errno, "reading --load-ca-privkey: %s",
           info->ca_privkey);

  key = _load_privkey(&dat, info);

  free (dat.data);

  return key;
}

/* Loads the CA's certificate
 */
gnutls_x509_crt_t
load_ca_cert (common_info_st * info)
{
  gnutls_x509_crt_t crt;
  int ret;
  gnutls_datum_t dat;
  size_t size;

  if (info->ca == NULL)
    error (EXIT_FAILURE, 0, "missing --load-ca-certificate");

  ret = gnutls_x509_crt_init (&crt);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "crt_init: %s", gnutls_strerror (ret));

  dat.data = (void*)read_binary_file (info->ca, &size);
  dat.size = size;

  if (!dat.data)
    error (EXIT_FAILURE, errno, "reading --load-ca-certificate: %s",
           info->ca);

  ret = gnutls_x509_crt_import (crt, &dat, info->incert_format);
  free (dat.data);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "importing --load-ca-certificate: %s: %s",
           info->ca, gnutls_strerror (ret));

  return crt;
}

/* Load a public key.
 * @mand should be non zero if it is required to read a public key.
 */
gnutls_pubkey_t
load_pubkey (int mand, common_info_st * info)
{
  gnutls_pubkey_t key;
  int ret;
  gnutls_datum_t dat;
  size_t size;

  if (!info->pubkey && !mand)
    return NULL;

  if (info->pubkey == NULL)
    error (EXIT_FAILURE, 0, "missing --load-pubkey");

  if (gnutls_url_is_supported(info->pubkey) != 0)
    return _load_url_pubkey(info->pubkey);

  ret = gnutls_pubkey_init (&key);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "privkey_init: %s", gnutls_strerror (ret));

  dat.data = (void*)read_binary_file (info->pubkey, &size);
  dat.size = size;

  if (!dat.data)
    error (EXIT_FAILURE, errno, "reading --load-pubkey: %s", info->pubkey);

  ret = gnutls_pubkey_import (key, &dat, info->incert_format);

  free (dat.data);

  if (ret == GNUTLS_E_BASE64_UNEXPECTED_HEADER_ERROR)
    {
      error (EXIT_FAILURE, 0,
             "import error: could not find a valid PEM header; "
             "check if your key has the PUBLIC KEY header");
    }

  if (ret < 0)
    error (EXIT_FAILURE, 0, "importing --load-pubkey: %s: %s",
           info->pubkey, gnutls_strerror (ret));

  return key;
}

gnutls_pubkey_t load_public_key_or_import(int mand, gnutls_privkey_t privkey, common_info_st * info)
{
gnutls_pubkey_t pubkey;
int ret;

  ret = gnutls_pubkey_init(&pubkey);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "gnutls_pubkey_init: %s",
           gnutls_strerror (ret));

  if (!privkey || (ret = gnutls_pubkey_import_privkey(pubkey, privkey, 0, 0)) < 0)
    { /* could not get (e.g. on PKCS #11 */
      gnutls_pubkey_deinit(pubkey);
      return load_pubkey(mand, info);
    }

  return pubkey;
}

int
get_bits (gnutls_pk_algorithm_t key_type, int info_bits, const char* info_sec_param, int warn)
{
  int bits;

  if (info_bits != 0)
    {
      static int warned = 0;

      if (warned == 0 && warn != 0)
        {
          warned = 1;
          fprintf (stderr,
                   "** Note: Please use the --sec-param instead of --bits\n");
        }
      bits = info_bits;
    }
  else
    {
      if (info_sec_param)
        {
          bits =
            gnutls_sec_param_to_pk_bits (key_type,
                                         str_to_sec_param (info_sec_param));
        }
      else
        bits =
          gnutls_sec_param_to_pk_bits (key_type, GNUTLS_SEC_PARAM_NORMAL);
    }

  return bits;
}

gnutls_sec_param_t str_to_sec_param (const char *str)
{
  if (strcasecmp (str, "low") == 0)
    {
      return GNUTLS_SEC_PARAM_LOW;
    }
  else if (strcasecmp (str, "legacy") == 0)
    {
      return GNUTLS_SEC_PARAM_LEGACY;
    }
  else if (strcasecmp (str, "normal") == 0)
    {
      return GNUTLS_SEC_PARAM_NORMAL;
    }
  else if (strcasecmp (str, "high") == 0)
    {
      return GNUTLS_SEC_PARAM_HIGH;
    }
  else if (strcasecmp (str, "ultra") == 0)
    {
      return GNUTLS_SEC_PARAM_ULTRA;
    }
  else
    {
      fprintf (stderr, "Unknown security parameter string: %s\n", str);
      exit (1);
    }

}

static void
print_hex_datum (FILE* outfile, gnutls_datum_t * dat)
{
  unsigned int j;
#define SPACE "\t"
  fprintf (outfile, "\n" SPACE);
  for (j = 0; j < dat->size; j++)
    {
      fprintf (outfile, "%.2x:", (unsigned char) dat->data[j]);
      if ((j + 1) % 15 == 0)
        fprintf (outfile, "\n" SPACE);
    }
  fprintf (outfile, "\n");
}

void
print_dsa_pkey (FILE* outfile, gnutls_datum_t * x, gnutls_datum_t * y, gnutls_datum_t * p,
                gnutls_datum_t * q, gnutls_datum_t * g)
{
  if (x)
    {
      fprintf (outfile, "private key:");
      print_hex_datum (outfile, x);
    }
  fprintf (outfile, "public key:");
  print_hex_datum (outfile, y);
  fprintf (outfile, "p:");
  print_hex_datum (outfile, p);
  fprintf (outfile, "q:");
  print_hex_datum (outfile, q);
  fprintf (outfile, "g:");
  print_hex_datum (outfile, g);
}

void
print_ecc_pkey (FILE* outfile, gnutls_ecc_curve_t curve, gnutls_datum_t* k, gnutls_datum_t * x, gnutls_datum_t * y)
{
  fprintf (outfile, "curve:\t%s\n", gnutls_ecc_curve_get_name(curve));
  if (k)
    {
      fprintf (outfile, "private key:");
      print_hex_datum (outfile, k);
    }
  fprintf (outfile, "x:");
  print_hex_datum (outfile, x);
  fprintf (outfile, "y:");
  print_hex_datum (outfile, y);
}

void
print_rsa_pkey (FILE* outfile, gnutls_datum_t * m, gnutls_datum_t * e, gnutls_datum_t * d,
                gnutls_datum_t * p, gnutls_datum_t * q, gnutls_datum_t * u,
                gnutls_datum_t * exp1, gnutls_datum_t * exp2)
{
  fprintf (outfile, "modulus:");
  print_hex_datum (outfile, m);
  fprintf (outfile, "public exponent:");
  print_hex_datum (outfile, e);
  if (d)
    {
      fprintf (outfile, "private exponent:");
      print_hex_datum (outfile, d);
      fprintf (outfile, "prime1:");
      print_hex_datum (outfile, p);
      fprintf (outfile, "prime2:");
      print_hex_datum (outfile, q);
      fprintf (outfile, "coefficient:");
      print_hex_datum (outfile, u);
      if (exp1 && exp2)
        {
          fprintf (outfile, "exp1:");
          print_hex_datum (outfile, exp1);
          fprintf (outfile, "exp2:");
          print_hex_datum (outfile, exp2);
        }
    }
}

void _pubkey_info(FILE* outfile, gnutls_certificate_print_formats_t format, gnutls_pubkey_t pubkey)
{
gnutls_datum_t data;
int ret;
size_t size;

  ret = gnutls_pubkey_print(pubkey, format, &data);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "pubkey_print error: %s", gnutls_strerror (ret));

  fprintf (outfile, "%s\n", data.data);
  gnutls_free (data.data);

  size = buffer_size;
  ret = gnutls_pubkey_export (pubkey, GNUTLS_X509_FMT_PEM, buffer, &size);
  if (ret < 0)
    error (EXIT_FAILURE, 0, "export error: %s", gnutls_strerror (ret));

  fprintf (outfile, "\n%s\n", buffer);
}
