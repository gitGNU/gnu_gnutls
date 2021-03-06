/*
 * Copyright (C) 2012 Free Software Foundation, Inc.
 *
 * Author: David Woodhouse
 *
 * This file is part of GnuTLS.
 *
 * The GnuTLS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 *
 */

#include <gnutls_int.h>

#include <gnutls_datum.h>
#include <gnutls_global.h>
#include <gnutls_errors.h>
#include <gnutls_rsa_export.h>
#include <common.h>
#include <gnutls_x509.h>
#include <x509_b64.h>
#include "x509_int.h"
#include <algorithms.h>
#include <gnutls_num.h>
#include <random.h>
#include <pbkdf2-sha1.h>

static int
openssl_hash_password (const char *pass, gnutls_datum_t * key, gnutls_datum_t * salt)
{
  unsigned char md5[16];
  gnutls_hash_hd_t hash;
  unsigned int count = 0;
  int err;

  while (count < key->size)
    {
      err = gnutls_hash_init (&hash, GNUTLS_DIG_MD5);
      if (err)
        {
          gnutls_assert ();
          return err;
        }
      if (count)
        {
          err = gnutls_hash (hash, md5, sizeof (md5));
          if (err)
            {
            hash_err:
              gnutls_hash_deinit (hash, NULL);
              gnutls_assert();
              return err;
            }
        }
      if (pass)
        {
          err = gnutls_hash (hash, pass, strlen (pass));
          if (err)
            {
              gnutls_assert();
              goto hash_err;
            }
        }
      err = gnutls_hash (hash, salt->data, 8);
      if (err)
        {
          gnutls_assert();
          goto hash_err;
        }

      gnutls_hash_deinit (hash, md5);

      if (key->size - count <= sizeof (md5))
        {
          memcpy (&key->data[count], md5, key->size - count);
          break;
        }

      memcpy (&key->data[count], md5, sizeof (md5));
      count += sizeof (md5);
    }

  return 0;
}

static const struct pem_cipher {
  const char *name;
  gnutls_cipher_algorithm_t cipher;
} pem_ciphers[] = {
  { "DES-CBC", GNUTLS_CIPHER_DES_CBC },
  { "DES-EDE3-CBC", GNUTLS_CIPHER_3DES_CBC },
  { "AES-128-CBC", GNUTLS_CIPHER_AES_128_CBC },
  { "AES-192-CBC", GNUTLS_CIPHER_AES_192_CBC },
  { "AES-256-CBC", GNUTLS_CIPHER_AES_256_CBC },
  { "CAMELLIA-128-CBC", GNUTLS_CIPHER_CAMELLIA_128_CBC },
  { "CAMELLIA-192-CBC", GNUTLS_CIPHER_CAMELLIA_192_CBC },
  { "CAMELLIA-256-CBC", GNUTLS_CIPHER_CAMELLIA_256_CBC },
};

/**
 * gnutls_x509_privkey_import_openssl:
 * @key: The structure to store the parsed key
 * @data: The DER or PEM encoded key.
 * @password: the password to decrypt the key (if it is encrypted).
 *
 * This function will convert the given PEM encrypted to 
 * the native gnutls_x509_privkey_t format. The
 * output will be stored in @key.  
 *
 * The @password should be in ASCII. If the password is not provided
 * or wrong then %GNUTLS_E_DECRYPTION_FAILED will be returned.
 *
 * If the Certificate is PEM encoded it should have a header of
 * "PRIVATE KEY" and the "DEK-Info" header. 
 *
 * Returns: On success, %GNUTLS_E_SUCCESS (0) is returned, otherwise a
 *   negative error value.
 **/
int
gnutls_x509_privkey_import_openssl (gnutls_x509_privkey_t key,
                                    const gnutls_datum_t *data, const char* password)
{
  gnutls_cipher_hd_t handle;
  gnutls_cipher_algorithm_t cipher = GNUTLS_CIPHER_UNKNOWN;
  gnutls_datum_t b64_data;
  gnutls_datum_t salt, enc_key;
  unsigned char *key_data;
  const char *pem_header = (void*)data->data;
  const char *pem_header_start = (void*)data->data;
  ssize_t pem_header_size;
  int ret;
  unsigned int i, iv_size, l;

  pem_header_size = data->size;

  pem_header = memmem(pem_header, pem_header_size, "PRIVATE KEY---", 14);
  if (pem_header == NULL)
    {
      gnutls_assert();
      return GNUTLS_E_PARSING_ERROR;
    }
    
  pem_header_size -= (ptrdiff_t)(pem_header-pem_header_start);

  pem_header = memmem(pem_header, pem_header_size, "DEK-Info: ", 10);
  if (pem_header == NULL)
    {
      gnutls_assert();
      return GNUTLS_E_PARSING_ERROR;
    }
  
  pem_header_size = data->size - (ptrdiff_t)(pem_header-pem_header_start) - 10;
  pem_header += 10;

  for (i = 0; i < sizeof(pem_ciphers)/sizeof(pem_ciphers[0]); i++) 
    {
      l = strlen(pem_ciphers[i].name);
      if (!strncmp(pem_header, pem_ciphers[i].name, l) &&
          pem_header[l] == ',') 
        {
          pem_header += l + 1;
          cipher = pem_ciphers[i].cipher;
          break;
        }
    }

  if (cipher == GNUTLS_CIPHER_UNKNOWN)
    {
      _gnutls_debug_log ("Unsupported PEM encryption type: %.10s\n", pem_header);
      gnutls_assert();
      return GNUTLS_E_INVALID_REQUEST;
    }

  iv_size = _gnutls_cipher_get_iv_size(cipher);
  salt.size = iv_size;
  salt.data = gnutls_malloc (salt.size);
  if (!salt.data)
    return gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);
    
  for (i = 0; i < salt.size * 2; i++)
    {
      unsigned char x;
      const char *c = &pem_header[i];

      if (*c >= '0' && *c <= '9')
        x = (*c) - '0';
      else if (*c >= 'A' && *c <= 'F')
        x = (*c) - 'A' + 10;
      else
        {
          gnutls_assert();
          /* Invalid salt in encrypted PEM file */
          ret = GNUTLS_E_INVALID_REQUEST;
          goto out_salt;
        }
      if (i & 1)
        salt.data[i / 2] |= x;
      else
        salt.data[i / 2] = x << 4;
    }

  pem_header += salt.size * 2;
  if (*pem_header != '\r' && *pem_header != '\n')
    {
      gnutls_assert();
      ret = GNUTLS_E_INVALID_REQUEST;
      goto out_salt;
    }
  while (*pem_header == '\n' || *pem_header == '\r')
    pem_header++;

  ret = _gnutls_base64_decode((const void*)pem_header, pem_header_size, &b64_data);
  if (ret < 0)
    {
      gnutls_assert();
      goto out_salt;
    }

  if (b64_data.size < 16)
    {
      /* Just to be sure our parsing is OK */
      gnutls_assert();
      ret = GNUTLS_E_PARSING_ERROR;
      goto out_b64;
    }

  ret = GNUTLS_E_MEMORY_ERROR;
  enc_key.size = gnutls_cipher_get_key_size (cipher);
  enc_key.data = gnutls_malloc (enc_key.size);
  if (!enc_key.data)
    {
      ret = gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);
      goto out_b64;
    }

  key_data = gnutls_malloc (b64_data.size);
  if (!key_data)
    {
      ret = gnutls_assert_val(GNUTLS_E_MEMORY_ERROR);
      goto out_enc_key;
    }

  while (1)
    {
      memcpy (key_data, b64_data.data, b64_data.size);

      ret = openssl_hash_password (password, &enc_key, &salt);
      if (ret < 0)
        {
          gnutls_assert();
          goto out;
        }

      ret = gnutls_cipher_init (&handle, cipher, &enc_key, &salt);
      if (ret < 0)
        {
          gnutls_assert();
          gnutls_cipher_deinit (handle);
          goto out;
        }

      ret = gnutls_cipher_decrypt (handle, key_data, b64_data.size);
      gnutls_cipher_deinit (handle);

      if (ret < 0)
        {
          gnutls_assert();
          goto out;
        }

      /* We have to strip any padding to accept it.
         So a bit more ASN.1 parsing for us.*/
      if (key_data[0] == 0x30)
        {
          gnutls_datum_t key_datum;
          unsigned int blocksize = gnutls_cipher_get_block_size (cipher);
          unsigned int keylen = key_data[1];
          unsigned int ofs = 2;

          if (keylen & 0x80)
            {
              int lenlen = keylen & 0x7f;
              keylen = 0;

              if (lenlen > 3)
                {
                  gnutls_assert();
                  goto fail;
                }

              while (lenlen)
                {
                  keylen <<= 8;
                  keylen |= key_data[ofs++];
                  lenlen--;
                }
            }
          keylen += ofs;

          /* If there appears to be more padding than required, fail */
          if (b64_data.size - keylen > blocksize)
            {
              gnutls_assert();
              goto fail;
            }

          /* If the padding bytes aren't all equal to the amount of padding, fail */
          ofs = keylen;
          while (ofs < b64_data.size)
            {
              if (key_data[ofs] != b64_data.size - keylen)
                {
                  gnutls_assert();
                  goto fail;
                }
              ofs++;
            }

          key_datum.data = key_data;
          key_datum.size = keylen;
          ret =
              gnutls_x509_privkey_import (key, &key_datum,
                                          GNUTLS_X509_FMT_DER);
          if (ret == 0)
            goto out;
        }
    fail:
      ret = GNUTLS_E_DECRYPTION_FAILED;
      goto out;
    }
out:
  gnutls_free (key_data);
out_enc_key:
  gnutls_free (enc_key.data);
out_b64:
  gnutls_free (b64_data.data);
out_salt:
  gnutls_free (salt.data);
  return ret;
}
