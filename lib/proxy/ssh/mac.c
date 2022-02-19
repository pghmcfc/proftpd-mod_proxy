/*
 * ProFTPD - mod_proxy SSH MACs
 * Copyright (c) 2021-2022 TJ Saunders
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 */

#include "mod_proxy.h"
#include "proxy/ssh/ssh2.h"
#include "proxy/ssh/msg.h"
#include "proxy/ssh/packet.h"
#include "proxy/ssh/crypto.h"
#include "proxy/ssh/mac.h"
#include "proxy/ssh/umac.h"
#include "proxy/ssh/session.h"
#include "proxy/ssh/disconnect.h"
#include "proxy/ssh/interop.h"

#if defined(PR_USE_OPENSSL)

#include <openssl/evp.h>
#include <openssl/hmac.h>

struct proxy_ssh_mac {
  pool *pool;
  const char *algo;
  int algo_type;

  const EVP_MD *digest;
  unsigned char *key;

  /* The keysz and key_len are usually the same; they can differ if, for
   * example, the client always truncates the MAC key len to 16 bits.
   */
  size_t keysz;
  uint32_t key_len;

  uint32_t mac_len;
};

#define PROXY_SSH_MAC_ALGO_TYPE_HMAC	1
#define PROXY_SSH_MAC_ALGO_TYPE_UMAC64	2
#define PROXY_SSH_MAC_ALGO_TYPE_UMAC128	3

#define PROXY_SSH_MAC_FL_READ_MAC	1
#define PROXY_SSH_MAC_FL_WRITE_MAC	2

/* We need to keep the old MACs around, so that we can handle N arbitrary
 * packets to/from the client using the old keys, as during rekeying.
 * Thus we have two read MAC contexts, two write MAC contexts.
 * The cipher idx variable indicates which of the MACs is currently in use.
 */

static struct proxy_ssh_mac read_macs[] = {
  { NULL, NULL, 0, NULL, NULL, 0, 0, 0 },
  { NULL, NULL, 0, NULL, NULL, 0, 0, 0 }
};
static HMAC_CTX *hmac_read_ctxs[2];
static struct umac_ctx *umac_read_ctxs[2];

static struct proxy_ssh_mac write_macs[] = {
  { NULL, NULL, 0, NULL, NULL, 0, 0, 0 },
  { NULL, NULL, 0, NULL, NULL, 0, 0, 0 }
};
static HMAC_CTX *hmac_write_ctxs[2];
static struct umac_ctx *umac_write_ctxs[2];

static size_t mac_blockszs[2] = { 0, 0 };

static unsigned int read_mac_idx = 0;
static unsigned int write_mac_idx = 0;

static void clear_mac(struct proxy_ssh_mac *);

static unsigned int get_next_read_index(void) {
  if (read_mac_idx == 1) {
    return 0;
  }

  return 1;
}

static unsigned int get_next_write_index(void) {
  if (write_mac_idx == 1) {
    return 0;
  }

  return 1;
}

static void switch_read_mac(void) {
  /* First we can clear the read MAC, kept from rekeying. */
  if (read_macs[read_mac_idx].key) {
    clear_mac(&(read_macs[read_mac_idx]));
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
    HMAC_CTX_reset(hmac_read_ctxs[read_mac_idx]);
#elif OPENSSL_VERSION_NUMBER > 0x000907000L
    HMAC_CTX_cleanup(hmac_read_ctxs[read_mac_idx]);
#else
    HMAC_cleanup(hmac_read_ctxs[read_mac_idx]);
#endif

    if (read_macs[read_mac_idx].algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC64) {
      proxy_ssh_umac_reset(umac_read_ctxs[read_mac_idx]);

    } else if (read_macs[read_mac_idx].algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC128) {
      proxy_ssh_umac128_reset(umac_read_ctxs[read_mac_idx]);
    }

    mac_blockszs[read_mac_idx] = 0; 

    /* Now we can switch the index. */
    if (read_mac_idx == 1) {
      read_mac_idx = 0;
      return;
    }

    read_mac_idx = 1;
  }
}

static void switch_write_mac(void) {
  /* First we can clear the write MAC, kept from rekeying. */
  if (write_macs[write_mac_idx].key) {
    clear_mac(&(write_macs[write_mac_idx]));
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
    HMAC_CTX_reset(hmac_write_ctxs[write_mac_idx]);
#elif OPENSSL_VERSION_NUMBER > 0x000907000L
    HMAC_CTX_cleanup(hmac_write_ctxs[write_mac_idx]);
#else
    HMAC_cleanup(hmac_write_ctxs[write_mac_idx]);
#endif

    if (write_macs[write_mac_idx].algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC64) {
      proxy_ssh_umac_reset(umac_write_ctxs[write_mac_idx]);

    } else if (write_macs[write_mac_idx].algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC128) {
      proxy_ssh_umac128_reset(umac_write_ctxs[write_mac_idx]);
    }

    /* Now we can switch the index. */
    if (write_mac_idx == 1) {
      write_mac_idx = 0;
      return;
    }

    write_mac_idx = 1;
  }
}

static void clear_mac(struct proxy_ssh_mac *mac) {
  if (mac->key != NULL) {
    pr_memscrub(mac->key, mac->keysz);
    free(mac->key);
    mac->key = NULL;
    mac->keysz = 0;
    mac->key_len = 0;
  }

  mac->digest = NULL;
  mac->algo = NULL;
}

static int init_mac(pool *p, struct proxy_ssh_mac *mac, HMAC_CTX *hmac_ctx,
    struct umac_ctx *umac_ctx) {
  /* Currently unused. */
  (void) p;

  if (strcmp(mac->algo, "none") == 0) {
    return 0;
  }

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
  HMAC_CTX_reset(hmac_ctx);
#elif OPENSSL_VERSION_NUMBER > 0x000907000L
  HMAC_CTX_init(hmac_ctx);
#else
  /* Reset the HMAC context. */
  HMAC_Init(hmac_ctx, NULL, 0, NULL);
#endif

  if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_HMAC) {
#if OPENSSL_VERSION_NUMBER > 0x000907000L
# if OPENSSL_VERSION_NUMBER >= 0x10000001L
    if (HMAC_Init_ex(hmac_ctx, mac->key, mac->key_len, mac->digest,
        NULL) != 1) {
      pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error initializing HMAC: %s", proxy_ssh_crypto_get_errors());
      errno = EPERM;
      return -1;
    }

# else
    HMAC_Init_ex(hmac_ctx, mac->key, mac->key_len, mac->digest, NULL);
# endif /* OpenSSL-1.0.0 and later */

#else
    HMAC_Init(hmac_ctx, mac->key, mac->key_len, mac->digest);
#endif

  } else if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC64) {
    proxy_ssh_umac_reset(umac_ctx);
    proxy_ssh_umac_init(umac_ctx, mac->key);

  } else if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC128) {
    proxy_ssh_umac128_reset(umac_ctx);
    proxy_ssh_umac128_init(umac_ctx, mac->key);
  }

  return 0;
}

static int get_mac(struct proxy_ssh_packet *pkt, struct proxy_ssh_mac *mac,
    HMAC_CTX *hmac_ctx, struct umac_ctx *umac_ctx, int flags) {
  unsigned char *mac_data;
  unsigned char *buf, *ptr;
  uint32_t buflen, bufsz = 0, mac_len = 0, len = 0;

  if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_HMAC) {
    /* Always leave a little extra room in the buffer. */
    bufsz = (sizeof(uint32_t) * 2) + pkt->packet_len + 64;
    mac_data = pcalloc(pkt->pool, EVP_MAX_MD_SIZE);

    buflen = bufsz;
    ptr = buf = palloc(pkt->pool, bufsz);

    len += proxy_ssh_msg_write_int(&buf, &buflen, pkt->seqno);
    len += proxy_ssh_msg_write_int(&buf, &buflen, pkt->packet_len);
    len += proxy_ssh_msg_write_byte(&buf, &buflen, pkt->padding_len);
    len += proxy_ssh_msg_write_data(&buf, &buflen, pkt->payload,
      pkt->payload_len, FALSE);
    len += proxy_ssh_msg_write_data(&buf, &buflen, pkt->padding,
      pkt->padding_len, FALSE);

#if OPENSSL_VERSION_NUMBER > 0x000907000L
# if OPENSSL_VERSION_NUMBER >= 0x10000001L
    if (HMAC_Init_ex(hmac_ctx, NULL, 0, NULL, NULL) != 1) {
      pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error resetting HMAC context: %s", proxy_ssh_crypto_get_errors());
      errno = EPERM;
      return -1;
    }
# else
    HMAC_Init_ex(hmac_ctx, NULL, 0, NULL, NULL);
# endif /* OpenSSL-1.0.0 and later */

#else
    HMAC_Init(hmac_ctx, NULL, 0, NULL);
#endif /* OpenSSL-0.9.7 and later */

#if OPENSSL_VERSION_NUMBER >= 0x10000001L
    if (HMAC_Update(hmac_ctx, ptr, len) != 1) {
      pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error adding %lu bytes of data to  HMAC context: %s",
        (unsigned long) len, proxy_ssh_crypto_get_errors());
      errno = EPERM;
      return -1;
    }

    if (HMAC_Final(hmac_ctx, mac_data, &mac_len) != 1) {
      pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error finalizing HMAC context: %s", proxy_ssh_crypto_get_errors());
      errno = EPERM;
      return -1;
    }
#else
    HMAC_Update(hmac_ctx, ptr, len);
    HMAC_Final(hmac_ctx, mac_data, &mac_len);
#endif /* OpenSSL-1.0.0 and later */

  } else if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC64 ||
             mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC128) {
    unsigned char nonce[8], *nonce_ptr;
    uint32_t nonce_len = 0;

    /* Always leave a little extra room in the buffer. */
    bufsz = sizeof(uint32_t) + pkt->packet_len + 64;
    mac_data = pcalloc(pkt->pool, EVP_MAX_MD_SIZE);

    buflen = bufsz;
    ptr = buf = palloc(pkt->pool, bufsz);

    len += proxy_ssh_msg_write_int(&buf, &buflen, pkt->packet_len);
    len += proxy_ssh_msg_write_byte(&buf, &buflen, pkt->padding_len);
    len += proxy_ssh_msg_write_data(&buf, &buflen, pkt->payload,
      pkt->payload_len, FALSE);
    len += proxy_ssh_msg_write_data(&buf, &buflen, pkt->padding,
      pkt->padding_len, FALSE);

    nonce_ptr = nonce;
    nonce_len = sizeof(nonce);
    len += proxy_ssh_msg_write_long(&nonce_ptr, &nonce_len, pkt->seqno);

    if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC64) {
      proxy_ssh_umac_reset(umac_ctx);
      proxy_ssh_umac_update(umac_ctx, ptr, (bufsz - buflen));
      proxy_ssh_umac_final(umac_ctx, mac_data, nonce);
      mac_len = 8;

    } else if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC128) {
      proxy_ssh_umac128_reset(umac_ctx);
      proxy_ssh_umac128_update(umac_ctx, ptr, (bufsz - buflen));
      proxy_ssh_umac128_final(umac_ctx, mac_data, nonce);
      mac_len = 16;
    }
  }

  if (mac_len == 0) {
    pkt->mac = NULL;
    pkt->mac_len = 0;

    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error computing MAC using %s: %s", mac->algo,
      proxy_ssh_crypto_get_errors());

    errno = EIO;
    return -1;
  }

  if (mac->mac_len != 0) {
    mac_len = mac->mac_len;
  }

  if (flags & PROXY_SSH_MAC_FL_READ_MAC) {
    if (memcmp(mac_data, pkt->mac, mac_len) != 0) {
      unsigned int i = 0;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "MAC from server differs from expected MAC using %s", mac->algo);

#ifdef SFTP_DEBUG_PACKET
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "client MAC (len %lu):", (unsigned long) pkt->mac_len);
      for (i = 0; i < mac_len;) {
        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "  %02x%02x %02x%02x %02x%02x %02x%02x",
          ((unsigned char *) pkt->mac)[i], ((unsigned char *) pkt->mac)[i+1],
          ((unsigned char *) pkt->mac)[i+2], ((unsigned char *) pkt->mac)[i+3],
          ((unsigned char *) pkt->mac)[i+4], ((unsigned char *) pkt->mac)[i+5],
          ((unsigned char *) pkt->mac)[i+6], ((unsigned char *) pkt->mac)[i+7]);
        i += 8;
      }

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "client MAC (len %lu):", (unsigned long) mac_len);
      for (i = 0; i < mac_len;) {
        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "  %02x%02x %02x%02x %02x%02x %02x%02x",
          ((unsigned char *) mac)[i], ((unsigned char *) mac)[i+1],
          ((unsigned char *) mac)[i+2], ((unsigned char *) mac)[i+3],
          ((unsigned char *) mac)[i+4], ((unsigned char *) mac)[i+5],
          ((unsigned char *) mac)[i+6], ((unsigned char *) mac)[i+7]);
        i += 8;
      }
#else
      /* Avoid compiler warning. */
      (void) i;
#endif

      errno = EINVAL;
      return -1;
    }

  } else if (flags & PROXY_SSH_MAC_FL_WRITE_MAC) {
    /* Debugging. */
#ifdef SFTP_DEBUG_PACKET
    if (pkt->mac_len > 0) {
      unsigned int i = 0;

      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "client MAC (len %lu, seqno %lu):",
        (unsigned long) pkt->mac_len, (unsigned long) pkt->seqno);
      for (i = 0; i < mac_len;) {
        (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
          "  %02x%02x %02x%02x %02x%02x %02x%02x",
          ((unsigned char *) pkt->mac)[i], ((unsigned char *) pkt->mac)[i+1],
          ((unsigned char *) pkt->mac)[i+2], ((unsigned char *) pkt->mac)[i+3],
          ((unsigned char *) pkt->mac)[i+4], ((unsigned char *) pkt->mac)[i+5],
          ((unsigned char *) pkt->mac)[i+6], ((unsigned char *) pkt->mac)[i+7]);
        i += 8;
      }
    }
#endif
  }

  pkt->mac_len = mac_len;
  pkt->mac = pcalloc(pkt->pool, pkt->mac_len);
  memcpy(pkt->mac, mac_data, mac_len);

  return 0;
}

static int set_mac_key(struct proxy_ssh_mac *mac, const EVP_MD *md,
    const unsigned char *k, uint32_t klen, const char *h,
    uint32_t hlen, char *letter, const unsigned char *id, uint32_t id_len) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
  EVP_MD_CTX ctx;
#endif /* prior to OpenSSL-1.1.0 */
  EVP_MD_CTX *pctx;
  unsigned char *key = NULL;
  size_t key_sz;
  uint32_t key_len = 0;

  key_sz = proxy_ssh_crypto_get_size(EVP_MD_block_size(mac->digest),
    EVP_MD_size(md)); 
  if (key_sz == 0) {
    if (strcmp(mac->algo, "none") == 0) {
      return 0;
    }

    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "unable to determine key length for MAC '%s'", mac->algo);
    errno = EINVAL;
    return -1;
  }

  key = malloc(key_sz);
  if (key == NULL) {
    pr_log_pri(PR_LOG_ALERT, MOD_PROXY_VERSION ": Out of memory!");
    _exit(1);
  }

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
  pctx = &ctx;
#else
  pctx = EVP_MD_CTX_new();
#endif /* prior to OpenSSL-1.1.0 */

  /* In OpenSSL 0.9.6, many of the EVP_Digest* functions returned void, not
   * int.  Without these ugly OpenSSL version preprocessor checks, the
   * compiler will error out with "void value not ignored as it ought to be".
   */

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
  if (EVP_DigestInit(pctx, md) != 1) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error initializing message digest: %s", proxy_ssh_crypto_get_errors());
    free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
    EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
    return -1;
  }
#else
  EVP_DigestInit(pctx, md);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
  if (EVP_DigestUpdate(pctx, k, klen) != 1) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error updating message digest with K: %s",
      proxy_ssh_crypto_get_errors());
    free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
    EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
    return -1;
  }
#else
  EVP_DigestUpdate(pctx, k, klen);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
  if (EVP_DigestUpdate(pctx, h, hlen) != 1) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error updating message digest with H: %s",
      proxy_ssh_crypto_get_errors());
    free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
    EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
    return -1;
  }
#else
  EVP_DigestUpdate(pctx, h, hlen);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
  if (EVP_DigestUpdate(pctx, letter, sizeof(char)) != 1) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error updating message digest with '%c': %s", *letter,
      proxy_ssh_crypto_get_errors());
    free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
    EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
    return -1;
  }
#else
  EVP_DigestUpdate(pctx, letter, sizeof(char));
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
  if (EVP_DigestUpdate(pctx, (char *) id, id_len) != 1) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error updating message digest with ID: %s",
      proxy_ssh_crypto_get_errors());
    free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
    EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
    return -1;
  }
#else
  EVP_DigestUpdate(pctx, (char *) id, id_len);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
  if (EVP_DigestFinal(pctx, key, &key_len) != 1) {
    (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
      "error finalizing message digest: %s", proxy_ssh_crypto_get_errors());
    pr_memscrub(key, key_sz);
    free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
    EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
    return -1;
  }
#else
  EVP_DigestFinal(pctx, key, &key_len);
#endif

  /* If we need more, keep hashing, as per RFC, until we have enough
   * material.
   */

  while (key_sz > key_len) {
    uint32_t len = key_len;

    pr_signals_handle();

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
    if (EVP_DigestInit(pctx, md) != 1) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error initializing message digest: %s", proxy_ssh_crypto_get_errors());
      pr_memscrub(key, key_sz);
      free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
      EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
      return -1;
    }
#else
    EVP_DigestInit(pctx, md);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
    if (EVP_DigestUpdate(pctx, k, klen) != 1) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error updating message digest with K: %s",
        proxy_ssh_crypto_get_errors());
      pr_memscrub(key, key_sz);
      free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
      EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
      return -1;
    }
#else
    EVP_DigestUpdate(pctx, k, klen);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
    if (EVP_DigestUpdate(pctx, h, hlen) != 1) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error updating message digest with H: %s",
        proxy_ssh_crypto_get_errors());
      pr_memscrub(key, key_sz);
      free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
      EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
      return -1;
    }
#else
    EVP_DigestUpdate(pctx, h, hlen);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
    if (EVP_DigestUpdate(pctx, key, len) != 1) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error updating message digest with data: %s",
        proxy_ssh_crypto_get_errors());
      pr_memscrub(key, key_sz);
      free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
      EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
      return -1;
    }
#else
    EVP_DigestUpdate(pctx, key, len);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x000907000L
    if (EVP_DigestFinal(pctx, key + len, &len) != 1) {
      (void) pr_log_writefile(proxy_logfd, MOD_PROXY_VERSION,
        "error finalizing message digest: %s", proxy_ssh_crypto_get_errors());
      pr_memscrub(key, key_sz);
      free(key);
# if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
     !defined(HAVE_LIBRESSL)
      EVP_MD_CTX_free(pctx);
# endif /* OpenSSL-1.1.0 and later */
      return -1;
    }
#else
    EVP_DigestFinal(pctx, key + len, &len);
#endif

    key_len += len;
  }

  mac->key = key;
  mac->keysz = key_sz;

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
  EVP_MD_CTX_free(pctx);
#endif /* OpenSSL-1.1.0 and later */

  if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_HMAC) {
    mac->key_len = EVP_MD_size(mac->digest);

  } else if (mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC64 ||
             mac->algo_type == PROXY_SSH_MAC_ALGO_TYPE_UMAC128) {
    mac->key_len = EVP_MD_block_size(mac->digest);
  }

  if (!proxy_ssh_interop_supports_feature(PROXY_SSH_FEAT_MAC_LEN)) {
    mac->key_len = 16;
  }

  return 0;
}

size_t proxy_ssh_mac_get_block_size(void) {
  return mac_blockszs[read_mac_idx];
}

void proxy_ssh_mac_set_block_size(size_t blocksz) {
  if (blocksz > mac_blockszs[read_mac_idx]) {
    mac_blockszs[read_mac_idx] = blocksz;
  }
}

const char *proxy_ssh_mac_get_read_algo(void) {
  if (read_macs[read_mac_idx].key != NULL ||
      strcmp(read_macs[read_mac_idx].algo, "none") == 0) {
    return read_macs[read_mac_idx].algo;
  }

  return NULL;
}

int proxy_ssh_mac_set_read_algo(pool *p, const char *algo) {
  uint32_t mac_len;
  unsigned int idx = read_mac_idx;

  if (read_macs[idx].key != NULL) {
    /* If we have an existing key, it means that we are currently rekeying. */
    idx = get_next_read_index();
  }

  /* Clear any potential UMAC contexts at this index. */
  if (umac_read_ctxs[idx] != NULL) {
    switch (read_macs[idx].algo_type) {
      case PROXY_SSH_MAC_ALGO_TYPE_UMAC64:
        proxy_ssh_umac_delete(umac_read_ctxs[idx]);
        umac_read_ctxs[idx] = NULL;
        break;

      case PROXY_SSH_MAC_ALGO_TYPE_UMAC128:
        proxy_ssh_umac128_delete(umac_read_ctxs[idx]);
        umac_read_ctxs[idx] = NULL;
        break;
    }
  }

  read_macs[idx].digest = proxy_ssh_crypto_get_digest(algo, &mac_len);
  if (read_macs[idx].digest == NULL) {
    return -1;
  }

  /* Note that we use a new pool, each time the algorithm is set (which
   * happens during key exchange) to prevent undue memory growth for
   * long-lived sessions with many rekeys.
   */
  if (read_macs[idx].pool != NULL) {
    destroy_pool(read_macs[idx].pool);
  }

  read_macs[idx].pool = make_sub_pool(p);
  pr_pool_tag(read_macs[idx].pool, "Proxy SFTP MAC read pool");
  read_macs[idx].algo = pstrdup(read_macs[idx].pool, algo);

  if (strcmp(read_macs[idx].algo, "umac-64@openssh.com") == 0) {
    read_macs[idx].algo_type = PROXY_SSH_MAC_ALGO_TYPE_UMAC64;
    umac_read_ctxs[idx] = proxy_ssh_umac_alloc();

  } else if (strcmp(read_macs[idx].algo, "umac-128@openssh.com") == 0) {
    read_macs[idx].algo_type = PROXY_SSH_MAC_ALGO_TYPE_UMAC128;
    umac_read_ctxs[idx] = proxy_ssh_umac128_alloc();

  } else {
    read_macs[idx].algo_type = PROXY_SSH_MAC_ALGO_TYPE_HMAC;
  }

  read_macs[idx].mac_len = mac_len;
  return 0;
}

int proxy_ssh_mac_set_read_key(pool *p, const EVP_MD *md,
    const unsigned char *k, uint32_t klen, const char *h, uint32_t hlen,
    int role) {
  const unsigned char *id = NULL;
  uint32_t id_len;
  char letter;
  size_t blocksz;
  struct proxy_ssh_mac *mac;
  HMAC_CTX *hmac_ctx;
  struct umac_ctx *umac_ctx;

  switch_read_mac();

  mac = &(read_macs[read_mac_idx]);
  hmac_ctx = hmac_read_ctxs[read_mac_idx];
  umac_ctx = umac_read_ctxs[read_mac_idx];

  id_len = proxy_ssh_session_get_id(&id);

  /* The letters used depend on the role; see:
   *  https://tools.ietf.org/html/rfc4253#section-7.2
   *
   * If we are the CLIENT, then we use the letters for the "server to client"
   * flows, since we are READING from the server.
   */

  /* client-to-server HASH(K || H || "E" || session_id)
   * server-to-client HASH(K || H || "F" || session_id)
   */
  letter = (role == PROXY_SSH_ROLE_CLIENT ? 'F' : 'E');
  set_mac_key(mac, md, k, klen, h, hlen, &letter, id, id_len);

  if (init_mac(p, mac, hmac_ctx, umac_ctx) < 0) {
    return -1;
  }

  if (mac->mac_len == 0) {
    blocksz = EVP_MD_size(mac->digest);

  } else {
    blocksz = mac->mac_len;
  }

  proxy_ssh_mac_set_block_size(blocksz);
  return 0;
}

int proxy_ssh_mac_read_data(struct proxy_ssh_packet *pkt) {
  struct proxy_ssh_mac *mac;
  HMAC_CTX *hmac_ctx;
  struct umac_ctx *umac_ctx;
  int res;

  mac = &(read_macs[read_mac_idx]);
  hmac_ctx = hmac_read_ctxs[read_mac_idx];
  umac_ctx = umac_read_ctxs[read_mac_idx];

  if (mac->key == NULL) {
    pkt->mac = NULL;
    pkt->mac_len = 0;

    return 0;
  }

  res = get_mac(pkt, mac, hmac_ctx, umac_ctx, PROXY_SSH_MAC_FL_READ_MAC);
  if (res < 0) {
    return -1;
  }

  return 0;
}

const char *proxy_ssh_mac_get_write_algo(void) {
  if (write_macs[write_mac_idx].key != NULL) {
    return write_macs[write_mac_idx].algo;
  }

  return NULL;
}

int proxy_ssh_mac_set_write_algo(pool *p, const char *algo) {
  uint32_t mac_len;
  unsigned int idx = write_mac_idx;

  if (write_macs[idx].key != NULL) {
    /* If we have an existing key, it means that we are currently rekeying. */
    idx = get_next_write_index();
  }

  /* Clear any potential UMAC contexts at this index. */
  if (umac_write_ctxs[idx] != NULL) {
    switch (write_macs[idx].algo_type) {
      case PROXY_SSH_MAC_ALGO_TYPE_UMAC64:
        proxy_ssh_umac_delete(umac_write_ctxs[idx]);
        umac_write_ctxs[idx] = NULL;
        break;

      case PROXY_SSH_MAC_ALGO_TYPE_UMAC128:
        proxy_ssh_umac128_delete(umac_write_ctxs[idx]);
        umac_write_ctxs[idx] = NULL;
        break;
    }
  }

  write_macs[idx].digest = proxy_ssh_crypto_get_digest(algo, &mac_len);
  if (write_macs[idx].digest == NULL) {
    return -1;
  }

  /* Note that we use a new pool, each time the algorithm is set (which
   * happens during key exchange) to prevent undue memory growth for
   * long-lived sessions with many rekeys.
   */
  if (write_macs[idx].pool != NULL) {
    destroy_pool(write_macs[idx].pool);
  }

  write_macs[idx].pool = make_sub_pool(p);
  pr_pool_tag(write_macs[idx].pool, "Proxy SFTP MAC write pool");
  write_macs[idx].algo = pstrdup(write_macs[idx].pool, algo);

  if (strcmp(write_macs[idx].algo, "umac-64@openssh.com") == 0) {
    write_macs[idx].algo_type = PROXY_SSH_MAC_ALGO_TYPE_UMAC64;
    umac_write_ctxs[idx] = proxy_ssh_umac_alloc();

  } else if (strcmp(write_macs[idx].algo, "umac-128@openssh.com") == 0) {
    write_macs[idx].algo_type = PROXY_SSH_MAC_ALGO_TYPE_UMAC128;
    umac_write_ctxs[idx] = proxy_ssh_umac128_alloc();

  } else {
    write_macs[idx].algo_type = PROXY_SSH_MAC_ALGO_TYPE_HMAC;
  }

  write_macs[idx].mac_len = mac_len;
  return 0;
}

int proxy_ssh_mac_set_write_key(pool *p, const EVP_MD *md,
    const unsigned char *k, uint32_t klen, const char *h, uint32_t hlen,
    int role) {
  const unsigned char *id = NULL;
  uint32_t id_len;
  char letter;
  struct proxy_ssh_mac *mac;
  HMAC_CTX *hmac_ctx;
  struct umac_ctx *umac_ctx;

  switch_write_mac();

  mac = &(write_macs[write_mac_idx]);
  hmac_ctx = hmac_write_ctxs[write_mac_idx];
  umac_ctx = umac_write_ctxs[write_mac_idx];

  id_len = proxy_ssh_session_get_id(&id);

  /* The letters used depend on the role; see:
   *  https://tools.ietf.org/html/rfc4253#section-7.2
   *
   * If we are the CLIENT, then we use the letters for the "client to server"
   * flows, since we are WRITING to the server.
   */

  /* client-to-server HASH(K || H || "E" || session_id)
   * server-to-client HASH(K || H || "F" || session_id)
   */
  letter = (role == PROXY_SSH_ROLE_CLIENT ? 'E' : 'F');
  set_mac_key(mac, md, k, klen, h, hlen, &letter, id, id_len);

  if (init_mac(p, mac, hmac_ctx, umac_ctx) < 0) {
    return -1;
  }

  return 0;
}

int proxy_ssh_mac_write_data(struct proxy_ssh_packet *pkt) {
  struct proxy_ssh_mac *mac;
  HMAC_CTX *hmac_ctx;
  struct umac_ctx *umac_ctx;
  int res;

  mac = &(write_macs[write_mac_idx]);
  hmac_ctx = hmac_write_ctxs[write_mac_idx];
  umac_ctx = umac_write_ctxs[write_mac_idx];

  if (mac->key == NULL) {
    pkt->mac = NULL;
    pkt->mac_len = 0;

    return 0;
  }

  res = get_mac(pkt, mac, hmac_ctx, umac_ctx, PROXY_SSH_MAC_FL_WRITE_MAC);
  if (res < 0) {
    return -1;
  }

  return 0;
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
/* In older versions of OpenSSL, there was not a way to dynamically allocate
 * an HMAC_CTX object.  Thus we have these static objects for those
 * older versions.
 */
static HMAC_CTX read_ctx1, read_ctx2;
static HMAC_CTX write_ctx1, write_ctx2;
#endif /* prior to OpenSSL-1.1.0 */

int proxy_ssh_mac_init(void) {
#if OPENSSL_VERSION_NUMBER < 0x10100000L || \
    defined(HAVE_LIBRESSL)
  hmac_read_ctxs[0] = &read_ctx1;
  hmac_read_ctxs[1] = &read_ctx2;
  hmac_write_ctxs[0] = &write_ctx1;
  hmac_write_ctxs[1] = &write_ctx2;
#else
  hmac_read_ctxs[0] = HMAC_CTX_new();
  hmac_read_ctxs[1] = HMAC_CTX_new();
  hmac_write_ctxs[0] = HMAC_CTX_new();
  hmac_write_ctxs[1] = HMAC_CTX_new();
#endif /* OpenSSL-1.1.0 and later */

  umac_read_ctxs[0] = NULL;
  umac_read_ctxs[1] = NULL;
  umac_write_ctxs[0] = NULL;
  umac_write_ctxs[1] = NULL;

  return 0;
}

int proxy_ssh_mac_free(void) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
    !defined(HAVE_LIBRESSL)
  HMAC_CTX_free(hmac_read_ctxs[0]);
  HMAC_CTX_free(hmac_read_ctxs[1]);
  HMAC_CTX_free(hmac_write_ctxs[0]);
  HMAC_CTX_free(hmac_write_ctxs[1]);
#endif /* OpenSSL-1.1.0 and later */
  return 0;
}
#endif /* PR_USE_OPENSSL */
