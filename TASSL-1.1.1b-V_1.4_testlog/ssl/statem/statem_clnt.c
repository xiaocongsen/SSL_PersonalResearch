/*
 * Copyright 1995-2019 The OpenSSL Project Authors. All Rights Reserved.
 * Copyright (c) 2002, Oracle and/or its affiliates. All rights reserved
 * Copyright 2005 Nokia. All rights reserved.
 *
 * Licensed under the OpenSSL license (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <stdio.h>
#include <time.h>
#include <assert.h>
#include "../ssl_locl.h"
#include "statem_locl.h"
#include <openssl/buffer.h>
#include <openssl/rand.h>
#include <openssl/objects.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/dh.h>
#include <openssl/bn.h>
#include <openssl/engine.h>
#include <internal/cryptlib.h>
#include <openssl/x509v3.h>
#include <openssl/sm2.h>

static MSG_PROCESS_RETURN tls_process_as_hello_retry_request(SSL *s, PACKET *pkt);
static MSG_PROCESS_RETURN tls_process_encrypted_extensions(SSL *s, PACKET *pkt);

static ossl_inline int cert_req_allowed(SSL *s);
static int key_exchange_expected(SSL *s);
static int GMold_key_exchange_expected(SSL *s);
static int ssl_cipher_list_to_bytes(SSL *s, STACK_OF(SSL_CIPHER) *sk,
                                    WPACKET *pkt);
static int GMold_ssl_cipher_list_to_bytes(SSL *s, STACK_OF(SSL_CIPHER) *sk,
                                    unsigned char *p);


#ifndef OPENSSL_NO_CNSM

int ssl_derive_SM2(SSL *s, EVP_PKEY *privkey, EVP_PKEY *pubkey,  int gensecret);
uint16_t tls1_nid2group_id(int nid);

#endif
/*
 * Is a CertificateRequest message allowed at the moment or not?
 *
 *  Return values are:
 *  1: Yes
 *  0: No
 */
static ossl_inline int cert_req_allowed(SSL *s)
{
    /* TLS does not like anon-DH with client cert */
    if ((s->version > SSL3_VERSION
         && (s->s3->tmp.new_cipher->algorithm_auth & SSL_aNULL))
        || (s->s3->tmp.new_cipher->algorithm_auth & (SSL_aSRP | SSL_aPSK)))
        return 0;

    return 1;
}

/*
 * Should we expect the ServerKeyExchange message or not?
 *
 *  Return values are:
 *  1: Yes
 *  0: No
 */
static int key_exchange_expected(SSL *s)
{
    printf("dddddddd key_exchange_expected\n");
    long alg_k = s->s3->tmp.new_cipher->algorithm_mkey;

    /*
     * Can't skip server key exchange if this is an ephemeral
     * ciphersuite or for SRP
     */
    #ifndef OPENSSL_NO_CNSM
	    if (alg_k & (SSL_kDHE | SSL_kECDHE | SSL_kDHEPSK | SSL_kECDHEPSK | SSL_kECC |SSL_kSM2DH  //add SSL_kECC  SSL_kSM2DH need ske
    #else
        if (alg_k & (SSL_kDHE | SSL_kECDHE | SSL_kDHEPSK | SSL_kECDHEPSK
    #endif
                 | SSL_kSRP)) {
        return 1;
    }

    return 0;
}

static int GMold_key_exchange_expected(SSL *s)
{
        return 1;
}

/*
 * ossl_statem_client_read_transition() encapsulates the logic for the allowed
 * handshake state transitions when a TLS1.3 client is reading messages from the
 * server. The message type that the server has sent is provided in |mt|. The
 * current state is in |s->statem.hand_state|.
 *
 * Return values are 1 for success (transition allowed) and  0 on error
 * (transition not allowed)
 */
static int ossl_statem_client13_read_transition(SSL *s, int mt)
{
    OSSL_STATEM *st = &s->statem;

    /*
     * Note: There is no case for TLS_ST_CW_CLNT_HELLO, because we haven't
     * yet negotiated TLSv1.3 at that point so that is handled by
     * ossl_statem_client_read_transition()
     */

    switch (st->hand_state) {
    default:
        break;

    case TLS_ST_CW_CLNT_HELLO:
        /*
         * This must a ClientHello following a HelloRetryRequest, so the only
         * thing we can get now is a ServerHello.
         */
        if (mt == SSL3_MT_SERVER_HELLO) {
            st->hand_state = TLS_ST_CR_SRVR_HELLO;
            return 1;
        }
        break;

    case TLS_ST_CR_SRVR_HELLO:
        if (mt == SSL3_MT_ENCRYPTED_EXTENSIONS) {
            st->hand_state = TLS_ST_CR_ENCRYPTED_EXTENSIONS;
            return 1;
        }
        break;

    case TLS_ST_CR_ENCRYPTED_EXTENSIONS:
        if (s->hit) {
            if (mt == SSL3_MT_FINISHED) {
                st->hand_state = TLS_ST_CR_FINISHED;
                return 1;
            }
        } else {
            if (mt == SSL3_MT_CERTIFICATE_REQUEST) {
                st->hand_state = TLS_ST_CR_CERT_REQ;
                return 1;
            }
            if (mt == SSL3_MT_CERTIFICATE) {
                st->hand_state = TLS_ST_CR_CERT;
                return 1;
            }
        }
        break;

    case TLS_ST_CR_CERT_REQ:
        if (mt == SSL3_MT_CERTIFICATE) {
            st->hand_state = TLS_ST_CR_CERT;
            return 1;
        }
        break;

    case TLS_ST_CR_CERT:
        if (mt == SSL3_MT_CERTIFICATE_VERIFY) {
            st->hand_state = TLS_ST_CR_CERT_VRFY;
            return 1;
        }
        break;

    case TLS_ST_CR_CERT_VRFY:
        if (mt == SSL3_MT_FINISHED) {
            st->hand_state = TLS_ST_CR_FINISHED;
            return 1;
        }
        break;

    case TLS_ST_OK:
        if (mt == SSL3_MT_NEWSESSION_TICKET) {
            st->hand_state = TLS_ST_CR_SESSION_TICKET;
            return 1;
        }
        if (mt == SSL3_MT_KEY_UPDATE) {
            st->hand_state = TLS_ST_CR_KEY_UPDATE;
            return 1;
        }
        if (mt == SSL3_MT_CERTIFICATE_REQUEST) {
#if DTLS_MAX_VERSION != DTLS1_2_VERSION
# error TODO(DTLS1.3): Restore digest for PHA before adding message.
#endif
            if (!SSL_IS_DTLS(s) && s->post_handshake_auth == SSL_PHA_EXT_SENT) {
                s->post_handshake_auth = SSL_PHA_REQUESTED;
                /*
                 * In TLS, this is called before the message is added to the
                 * digest. In DTLS, this is expected to be called after adding
                 * to the digest. Either move the digest restore, or add the
                 * message here after the swap, or do it after the clientFinished?
                 */
                if (!tls13_restore_handshake_digest_for_pha(s)) {
                    /* SSLfatal() already called */
                    return 0;
                }
                st->hand_state = TLS_ST_CR_CERT_REQ;
                return 1;
            }
        }
        break;
    }

    /* No valid transition found */
    return 0;
}

/*
 * ossl_statem_client_read_transition() encapsulates the logic for the allowed
 * handshake state transitions when the client is reading messages from the
 * server. The message type that the server has sent is provided in |mt|. The
 * current state is in |s->statem.hand_state|.
 *
 * Return values are 1 for success (transition allowed) and  0 on error
 * (transition not allowed)
 */
int ossl_statem_client_read_transition(SSL *s, int mt)
{
    printf("ddddddddddd ossl_statem_client_read_transition \n");
    OSSL_STATEM *st = &s->statem;
    int ske_expected;

    /*
     * Note that after writing the first ClientHello we don't know what version
     * we are going to negotiate yet, so we don't take this branch until later.
     */
    if (SSL_IS_TLS13(s)) {
        if (!ossl_statem_client13_read_transition(s, mt))
            goto err;
        return 1;
    }

    switch (st->hand_state) {
    default:
        break;

    case TLS_ST_CW_CLNT_HELLO:
        if (mt == SSL3_MT_SERVER_HELLO) {
            st->hand_state = TLS_ST_CR_SRVR_HELLO;
            return 1;
        }

        if (SSL_IS_DTLS(s)) {
            if (mt == DTLS1_MT_HELLO_VERIFY_REQUEST) {
                st->hand_state = DTLS_ST_CR_HELLO_VERIFY_REQUEST;
                return 1;
            }
        }
        break;

    case TLS_ST_EARLY_DATA:
        /*
         * We've not actually selected TLSv1.3 yet, but we have sent early
         * data. The only thing allowed now is a ServerHello or a
         * HelloRetryRequest.
         */
        if (mt == SSL3_MT_SERVER_HELLO) {
            st->hand_state = TLS_ST_CR_SRVR_HELLO;
            return 1;
        }
        break;

    case TLS_ST_CR_SRVR_HELLO:
        if (s->hit) {
            if (s->ext.ticket_expected) {
                if (mt == SSL3_MT_NEWSESSION_TICKET) {
                    st->hand_state = TLS_ST_CR_SESSION_TICKET;
                    return 1;
                }
            } else if (mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
                st->hand_state = TLS_ST_CR_CHANGE;
                return 1;
            }
        } else {
            if (SSL_IS_DTLS(s) && mt == DTLS1_MT_HELLO_VERIFY_REQUEST) {
                st->hand_state = DTLS_ST_CR_HELLO_VERIFY_REQUEST;
                return 1;
            } else if (s->version >= TLS1_VERSION
                       && s->ext.session_secret_cb != NULL
                       && s->session->ext.tick != NULL
                       && mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
                /*
                 * Normally, we can tell if the server is resuming the session
                 * from the session ID. EAP-FAST (RFC 4851), however, relies on
                 * the next server message after the ServerHello to determine if
                 * the server is resuming.
                 */
                s->hit = 1;
                st->hand_state = TLS_ST_CR_CHANGE;
                return 1;
            } else if (!(s->s3->tmp.new_cipher->algorithm_auth
                         & (SSL_aNULL | SSL_aSRP | SSL_aPSK))) {
                if (mt == SSL3_MT_CERTIFICATE) {
                    st->hand_state = TLS_ST_CR_CERT;
                    return 1;
                }
            } else {
                ske_expected = key_exchange_expected(s);
                /* SKE is optional for some PSK ciphersuites */
                if (ske_expected
                    || ((s->s3->tmp.new_cipher->algorithm_mkey & SSL_PSK)
                        && mt == SSL3_MT_SERVER_KEY_EXCHANGE)) {
                    if (mt == SSL3_MT_SERVER_KEY_EXCHANGE) {
                        st->hand_state = TLS_ST_CR_KEY_EXCH;
                        return 1;
                    }
                } else if (mt == SSL3_MT_CERTIFICATE_REQUEST
                           && cert_req_allowed(s)) {
                    st->hand_state = TLS_ST_CR_CERT_REQ;
                    return 1;
                } else if (mt == SSL3_MT_SERVER_DONE) {
                    st->hand_state = TLS_ST_CR_SRVR_DONE;
                    return 1;
                }
            }
        }
        break;

    case TLS_ST_CR_CERT:
        /*
         * The CertificateStatus message is optional even if
         * |ext.status_expected| is set
         */
        if (s->ext.status_expected && mt == SSL3_MT_CERTIFICATE_STATUS) {
            st->hand_state = TLS_ST_CR_CERT_STATUS;
            return 1;
        }
        /* Fall through */

    case TLS_ST_CR_CERT_STATUS:
        ske_expected = key_exchange_expected(s);
        /* SKE is optional for some PSK ciphersuites */
        if (ske_expected || ((s->s3->tmp.new_cipher->algorithm_mkey & SSL_PSK)
                             && mt == SSL3_MT_SERVER_KEY_EXCHANGE)) {
            if (mt == SSL3_MT_SERVER_KEY_EXCHANGE) {
                st->hand_state = TLS_ST_CR_KEY_EXCH;
                return 1;
            }
            goto err;
        }
        /* Fall through */

    case TLS_ST_CR_KEY_EXCH:
        if (mt == SSL3_MT_CERTIFICATE_REQUEST) {
            if (cert_req_allowed(s)) {
                st->hand_state = TLS_ST_CR_CERT_REQ;
                return 1;
            }
            goto err;
        }
        /* Fall through */

    case TLS_ST_CR_CERT_REQ:
        if (mt == SSL3_MT_SERVER_DONE) {
            st->hand_state = TLS_ST_CR_SRVR_DONE;
            return 1;
        }
        break;

    case TLS_ST_CW_FINISHED:
        if (s->ext.ticket_expected) {
            if (mt == SSL3_MT_NEWSESSION_TICKET) {
                st->hand_state = TLS_ST_CR_SESSION_TICKET;
                return 1;
            }
        } else if (mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
            st->hand_state = TLS_ST_CR_CHANGE;
            return 1;
        }
        break;

    case TLS_ST_CR_SESSION_TICKET:
        if (mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
            st->hand_state = TLS_ST_CR_CHANGE;
            return 1;
        }
        break;

    case TLS_ST_CR_CHANGE:
        if (mt == SSL3_MT_FINISHED) {
            st->hand_state = TLS_ST_CR_FINISHED;
            return 1;
        }
        break;

    case TLS_ST_OK:
        if (mt == SSL3_MT_HELLO_REQUEST) {
            st->hand_state = TLS_ST_CR_HELLO_REQ;
            return 1;
        }
        break;
    }

 err:
    /* No valid transition found */
    if (SSL_IS_DTLS(s) && mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
        BIO *rbio;

        /*
         * CCS messages don't have a message sequence number so this is probably
         * because of an out-of-order CCS. We'll just drop it.
         */
        s->init_num = 0;
        s->rwstate = SSL_READING;
        rbio = SSL_get_rbio(s);
        BIO_clear_retry_flags(rbio);
        BIO_set_retry_read(rbio);
        return 0;
    }
    SSLfatal(s, SSL3_AD_UNEXPECTED_MESSAGE,
             SSL_F_OSSL_STATEM_CLIENT_READ_TRANSITION,
             SSL_R_UNEXPECTED_MESSAGE);
    return 0;
}

int GMold_ossl_statem_client_read_transition(SSL *s, int mt)
{
    OSSL_STATEM *st = &s->statem;
    int ske_expected;
    
    printf("GMold_ossl statem_client_read_transition 1111111 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_CW_CLNT_HELLO:
        if (mt == SSL3_MT_SERVER_HELLO) {
            st->hand_state = TLS_ST_CR_SRVR_HELLO;
            return 1;
        }

        if (SSL_IS_DTLS(s)) {
            if (mt == DTLS1_MT_HELLO_VERIFY_REQUEST) {
                st->hand_state = DTLS_ST_CR_HELLO_VERIFY_REQUEST;
                return 1;
            }
        }
        break;

    case TLS_ST_CR_SRVR_HELLO:
        if (s->hit) {
            if (s->ext.ticket_expected) {
                if (mt == SSL3_MT_NEWSESSION_TICKET) {
                    st->hand_state = TLS_ST_CR_SESSION_TICKET;
                    return 1;
                }
            } else if (mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
                st->hand_state = TLS_ST_CR_CHANGE;
                return 1;
            }
        } else {
            if (SSL_IS_DTLS(s) && mt == DTLS1_MT_HELLO_VERIFY_REQUEST) {
                st->hand_state = DTLS_ST_CR_HELLO_VERIFY_REQUEST;
                return 1;
            } else if (s->version >= TLS1_VERSION
                       && s->ext.session_secret_cb != NULL
                       && s->session->ext.tick != NULL
                       && mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
                /*
                 * Normally, we can tell if the server is resuming the session
                 * from the session ID. EAP-FAST (RFC 4851), however, relies on
                 * the next server message after the ServerHello to determine if
                 * the server is resuming.
                 */
                s->hit = 1;
                st->hand_state = TLS_ST_CR_CHANGE;
                return 1;
            } else if (!(s->s3->tmp.new_cipher->algorithm_auth
                         & (SSL_aNULL | SSL_aSRP | SSL_aPSK))) {
                if (mt == SSL3_MT_CERTIFICATE) {
                    st->hand_state = TLS_ST_CR_CERT;
                    return 1;
                }
            } else {
                ske_expected = GMold_key_exchange_expected(s);
                /* SKE is optional for some PSK ciphersuites */
                if (ske_expected
                    || ((s->s3->tmp.new_cipher->algorithm_mkey & SSL_PSK)
                        && mt == SSL3_MT_SERVER_KEY_EXCHANGE)) {
                    if (mt == SSL3_MT_SERVER_KEY_EXCHANGE) {
                        st->hand_state = TLS_ST_CR_KEY_EXCH;
                        return 1;
                    }
                } else if (mt == SSL3_MT_CERTIFICATE_REQUEST
                           && cert_req_allowed(s)) {
                    st->hand_state = TLS_ST_CR_CERT_REQ;
                    return 1;
                } else if (mt == SSL3_MT_SERVER_DONE) {
                    st->hand_state = TLS_ST_CR_SRVR_DONE;
                    return 1;
                }
            }
        }
        break;

    case TLS_ST_CR_CERT:
        /*
         * The CertificateStatus message is optional even if
         * |status_expected| is set
         */
        if (s->ext.status_expected && mt == SSL3_MT_CERTIFICATE_STATUS) {
            st->hand_state = TLS_ST_CR_CERT_STATUS;
            return 1;
        }
        /* Fall through */

    case TLS_ST_CR_CERT_STATUS:
        ske_expected = GMold_key_exchange_expected(s);
        /* SKE is optional for some PSK ciphersuites */
        if (ske_expected || ((s->s3->tmp.new_cipher->algorithm_mkey & SSL_PSK)
                             && mt == SSL3_MT_SERVER_KEY_EXCHANGE)) {
            if (mt == SSL3_MT_SERVER_KEY_EXCHANGE) {
                st->hand_state = TLS_ST_CR_KEY_EXCH;
                return 1;
            }
            goto err;
        }
        /* Fall through */

    case TLS_ST_CR_KEY_EXCH:
        if (mt == SSL3_MT_CERTIFICATE_REQUEST) {
            if (cert_req_allowed(s)) {
                st->hand_state = TLS_ST_CR_CERT_REQ;
                return 1;
            }
            goto err;
        }
        /* Fall through */

    case TLS_ST_CR_CERT_REQ:
        if (mt == SSL3_MT_SERVER_DONE) {
            st->hand_state = TLS_ST_CR_SRVR_DONE;
            return 1;
        }
        break;

    case TLS_ST_CW_FINISHED:
        if (s->ext.ticket_expected) {
            if (mt == SSL3_MT_NEWSESSION_TICKET) {
                st->hand_state = TLS_ST_CR_SESSION_TICKET;
                return 1;
            }
        } else if (mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
            st->hand_state = TLS_ST_CR_CHANGE;
            return 1;
        }
        break;

    case TLS_ST_CR_SESSION_TICKET:
        if (mt == SSL3_MT_CHANGE_CIPHER_SPEC) {
            st->hand_state = TLS_ST_CR_CHANGE;
            return 1;
        }
        break;

    case TLS_ST_CR_CHANGE:
        if (mt == SSL3_MT_FINISHED) {
            st->hand_state = TLS_ST_CR_FINISHED;
            return 1;
        }
        break;

    default:
        break;
    }

 err:
    /* No valid transition found */
    ssl3_send_alert(s, SSL3_AL_FATAL, SSL3_AD_UNEXPECTED_MESSAGE);
    return 0;
}

/*
 * ossl_statem_client13_write_transition() works out what handshake state to
 * move to next when the TLSv1.3 client is writing messages to be sent to the
 * server.
 */
static WRITE_TRAN ossl_statem_client13_write_transition(SSL *s)
{
    OSSL_STATEM *st = &s->statem;

    /*
     * Note: There are no cases for TLS_ST_BEFORE because we haven't negotiated
     * TLSv1.3 yet at that point. They are handled by
     * ossl_statem_client_write_transition().
     */
    switch (st->hand_state) {
    default:
        /* Shouldn't happen */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_OSSL_STATEM_CLIENT13_WRITE_TRANSITION,
                 ERR_R_INTERNAL_ERROR);
        return WRITE_TRAN_ERROR;

    case TLS_ST_CR_CERT_REQ:
        if (s->post_handshake_auth == SSL_PHA_REQUESTED) {
            st->hand_state = TLS_ST_CW_CERT;
            return WRITE_TRAN_CONTINUE;
        }
        /*
         * We should only get here if we received a CertificateRequest after
         * we already sent close_notify
         */
        if (!ossl_assert((s->shutdown & SSL_SENT_SHUTDOWN) != 0)) {
            /* Shouldn't happen - same as default case */
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_OSSL_STATEM_CLIENT13_WRITE_TRANSITION,
                     ERR_R_INTERNAL_ERROR);
            return WRITE_TRAN_ERROR;
        }
        st->hand_state = TLS_ST_OK;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CR_FINISHED:
        if (s->early_data_state == SSL_EARLY_DATA_WRITE_RETRY
                || s->early_data_state == SSL_EARLY_DATA_FINISHED_WRITING)
            st->hand_state = TLS_ST_PENDING_EARLY_DATA_END;
        else if ((s->options & SSL_OP_ENABLE_MIDDLEBOX_COMPAT) != 0
                 && s->hello_retry_request == SSL_HRR_NONE)
            st->hand_state = TLS_ST_CW_CHANGE;
        else
            st->hand_state = (s->s3->tmp.cert_req != 0) ? TLS_ST_CW_CERT
                                                        : TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_PENDING_EARLY_DATA_END:
        if (s->ext.early_data == SSL_EARLY_DATA_ACCEPTED) {
            st->hand_state = TLS_ST_CW_END_OF_EARLY_DATA;
            return WRITE_TRAN_CONTINUE;
        }
        /* Fall through */

    case TLS_ST_CW_END_OF_EARLY_DATA:
    case TLS_ST_CW_CHANGE:
        st->hand_state = (s->s3->tmp.cert_req != 0) ? TLS_ST_CW_CERT
                                                    : TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CERT:
        /* If a non-empty Certificate we also send CertificateVerify */
        st->hand_state = (s->s3->tmp.cert_req == 1) ? TLS_ST_CW_CERT_VRFY
                                                    : TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CERT_VRFY:
        st->hand_state = TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CR_KEY_UPDATE:
        if (s->key_update != SSL_KEY_UPDATE_NONE) {
            st->hand_state = TLS_ST_CW_KEY_UPDATE;
            return WRITE_TRAN_CONTINUE;
        }
        /* Fall through */

    case TLS_ST_CW_KEY_UPDATE:
    case TLS_ST_CR_SESSION_TICKET:
    case TLS_ST_CW_FINISHED:
        st->hand_state = TLS_ST_OK;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_OK:
        if (s->key_update != SSL_KEY_UPDATE_NONE) {
            st->hand_state = TLS_ST_CW_KEY_UPDATE;
            return WRITE_TRAN_CONTINUE;
        }

        /* Try to read from the server instead */
        return WRITE_TRAN_FINISHED;
    }
}

/*
 * ossl_statem_client_write_transition() works out what handshake state to
 * move to next when the client is writing messages to be sent to the server.
 */
WRITE_TRAN ossl_statem_client_write_transition(SSL *s)
{
    printf("ddddddddddd ossl_statem_client_write_transition \n");
    OSSL_STATEM *st = &s->statem;

    /*
     * Note that immediately before/after a ClientHello we don't know what
     * version we are going to negotiate yet, so we don't take this branch until
     * later
     */
    if (SSL_IS_TLS13(s))
        return ossl_statem_client13_write_transition(s);

    switch (st->hand_state) {
    default:
        /* Shouldn't happen */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_OSSL_STATEM_CLIENT_WRITE_TRANSITION,
                 ERR_R_INTERNAL_ERROR);
        return WRITE_TRAN_ERROR;

    case TLS_ST_OK:
        if (!s->renegotiate) {
            /*
             * We haven't requested a renegotiation ourselves so we must have
             * received a message from the server. Better read it.
             */
            return WRITE_TRAN_FINISHED;
        }
        /* Renegotiation */
        /* fall thru */
    case TLS_ST_BEFORE:
        st->hand_state = TLS_ST_CW_CLNT_HELLO;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CLNT_HELLO:
        if (s->early_data_state == SSL_EARLY_DATA_CONNECTING) {
            /*
             * We are assuming this is a TLSv1.3 connection, although we haven't
             * actually selected a version yet.
             */
            if ((s->options & SSL_OP_ENABLE_MIDDLEBOX_COMPAT) != 0)
                st->hand_state = TLS_ST_CW_CHANGE;
            else
                st->hand_state = TLS_ST_EARLY_DATA;
            return WRITE_TRAN_CONTINUE;
        }
        /*
         * No transition at the end of writing because we don't know what
         * we will be sent
         */
        return WRITE_TRAN_FINISHED;

    case TLS_ST_CR_SRVR_HELLO:
        /*
         * We only get here in TLSv1.3. We just received an HRR, so issue a
         * CCS unless middlebox compat mode is off, or we already issued one
         * because we did early data.
         */
        if ((s->options & SSL_OP_ENABLE_MIDDLEBOX_COMPAT) != 0
                && s->early_data_state != SSL_EARLY_DATA_FINISHED_WRITING)
            st->hand_state = TLS_ST_CW_CHANGE;
        else
            st->hand_state = TLS_ST_CW_CLNT_HELLO;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_EARLY_DATA:
        return WRITE_TRAN_FINISHED;

    case DTLS_ST_CR_HELLO_VERIFY_REQUEST:
        st->hand_state = TLS_ST_CW_CLNT_HELLO;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CR_SRVR_DONE:
        if (s->s3->tmp.cert_req)
            st->hand_state = TLS_ST_CW_CERT;
        else
            st->hand_state = TLS_ST_CW_KEY_EXCH;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CERT:
        st->hand_state = TLS_ST_CW_KEY_EXCH;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_KEY_EXCH:
        /*
         * For TLS, cert_req is set to 2, so a cert chain of nothing is
         * sent, but no verify packet is sent
         */
        /*
         * XXX: For now, we do not support client authentication in ECDH
         * cipher suites with ECDH (rather than ECDSA) certificates. We
         * need to skip the certificate verify message when client's
         * ECDH public key is sent inside the client certificate.
         */
        if (s->s3->tmp.cert_req == 1) {
            st->hand_state = TLS_ST_CW_CERT_VRFY;
        } else {
            st->hand_state = TLS_ST_CW_CHANGE;
        }
        if (s->s3->flags & TLS1_FLAGS_SKIP_CERT_VERIFY) {
            st->hand_state = TLS_ST_CW_CHANGE;
        }
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CERT_VRFY:
        st->hand_state = TLS_ST_CW_CHANGE;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CHANGE:
        if (s->hello_retry_request == SSL_HRR_PENDING) {
            st->hand_state = TLS_ST_CW_CLNT_HELLO;
        } else if (s->early_data_state == SSL_EARLY_DATA_CONNECTING) {
            st->hand_state = TLS_ST_EARLY_DATA;
        } else {
#if defined(OPENSSL_NO_NEXTPROTONEG)
            st->hand_state = TLS_ST_CW_FINISHED;
#else
            if (!SSL_IS_DTLS(s) && s->s3->npn_seen)
                st->hand_state = TLS_ST_CW_NEXT_PROTO;
            else
                st->hand_state = TLS_ST_CW_FINISHED;
#endif
        }
        return WRITE_TRAN_CONTINUE;

#if !defined(OPENSSL_NO_NEXTPROTONEG)
    case TLS_ST_CW_NEXT_PROTO:
        st->hand_state = TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;
#endif

    case TLS_ST_CW_FINISHED:
        if (s->hit) {
            st->hand_state = TLS_ST_OK;
            return WRITE_TRAN_CONTINUE;
        } else {
            return WRITE_TRAN_FINISHED;
        }

    case TLS_ST_CR_FINISHED:
        if (s->hit) {
            st->hand_state = TLS_ST_CW_CHANGE;
            return WRITE_TRAN_CONTINUE;
        } else {
            st->hand_state = TLS_ST_OK;
            return WRITE_TRAN_CONTINUE;
        }

    case TLS_ST_CR_HELLO_REQ:
        /*
         * If we can renegotiate now then do so, otherwise wait for a more
         * convenient time.
         */
        if (ssl3_renegotiate_check(s, 1)) {
            if (!tls_setup_handshake(s)) {
                /* SSLfatal() already called */
                return WRITE_TRAN_ERROR;
            }
            st->hand_state = TLS_ST_CW_CLNT_HELLO;
            return WRITE_TRAN_CONTINUE;
        }
        st->hand_state = TLS_ST_OK;
        return WRITE_TRAN_CONTINUE;
    }
}

WRITE_TRAN GMold_ossl_statem_client_write_transition(SSL *s)
{
    OSSL_STATEM *st = &s->statem;
    printf("zzz GMold_ossl_statem client_write_transition 555555555 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_OK:
        /* Renegotiation - fall through */
    case TLS_ST_BEFORE:
        st->hand_state = TLS_ST_CW_CLNT_HELLO;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CLNT_HELLO:
        /*
         * No transition at the end of writing because we don't know what
         * we will be sent
         */
        return WRITE_TRAN_FINISHED;

    case DTLS_ST_CR_HELLO_VERIFY_REQUEST:
        st->hand_state = TLS_ST_CW_CLNT_HELLO;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CR_SRVR_DONE:
        if (s->s3->tmp.cert_req)
            st->hand_state = TLS_ST_CW_CERT;
        else
            st->hand_state = TLS_ST_CW_KEY_EXCH;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CERT:
        st->hand_state = TLS_ST_CW_KEY_EXCH;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_KEY_EXCH:
        /*
         * For TLS, cert_req is set to 2, so a cert chain of nothing is
         * sent, but no verify packet is sent
         */
        /*
         * XXX: For now, we do not support client authentication in ECDH
         * cipher suites with ECDH (rather than ECDSA) certificates. We
         * need to skip the certificate verify message when client's
         * ECDH public key is sent inside the client certificate.
         */
        if (s->s3->tmp.cert_req == 1) {
            st->hand_state = TLS_ST_CW_CERT_VRFY;
        } else {
            st->hand_state = TLS_ST_CW_CHANGE;
        }
        if (s->s3->flags & TLS1_FLAGS_SKIP_CERT_VERIFY) {
            st->hand_state = TLS_ST_CW_CHANGE;
        }
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CERT_VRFY:
        st->hand_state = TLS_ST_CW_CHANGE;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_CHANGE:
        if (!SSL_IS_DTLS(s) && s->s3->npn_seen)
            st->hand_state = TLS_ST_CW_NEXT_PROTO;
        else
            st->hand_state = TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_NEXT_PROTO:
        st->hand_state = TLS_ST_CW_FINISHED;
        return WRITE_TRAN_CONTINUE;

    case TLS_ST_CW_FINISHED:
        if (s->hit) {
            st->hand_state = TLS_ST_OK;
            ossl_statem_set_in_init(s, 0);
            return WRITE_TRAN_CONTINUE;
        } else {
            return WRITE_TRAN_FINISHED;
        }

    case TLS_ST_CR_FINISHED:
        if (s->hit) {
            st->hand_state = TLS_ST_CW_CHANGE;
            return WRITE_TRAN_CONTINUE;
        } else {
            st->hand_state = TLS_ST_OK;
            ossl_statem_set_in_init(s, 0);
            return WRITE_TRAN_CONTINUE;
        }

    default:
        /* Shouldn't happen */
        return WRITE_TRAN_ERROR;
    }
}

/*
 * Perform any pre work that needs to be done prior to sending a message from
 * the client to the server.
 */
WORK_STATE ossl_statem_client_pre_work(SSL *s, WORK_STATE wst)
{
    printf("ddddddddddd ossl_statem_client_pre_work \n");
    OSSL_STATEM *st = &s->statem;

    switch (st->hand_state) {
    default:
        /* No pre work to be done */
        break;

    case TLS_ST_CW_CLNT_HELLO:
        s->shutdown = 0;
        if (SSL_IS_DTLS(s)) {
            /* every DTLS ClientHello resets Finished MAC */
            if (!ssl3_init_finished_mac(s)) {
                /* SSLfatal() already called */
                return WORK_ERROR;
            }
        }
        break;

    case TLS_ST_CW_CHANGE:
        if (SSL_IS_DTLS(s)) {
            if (s->hit) {
                /*
                 * We're into the last flight so we don't retransmit these
                 * messages unless we need to.
                 */
                st->use_timer = 0;
            }
#ifndef OPENSSL_NO_SCTP
            if (BIO_dgram_is_sctp(SSL_get_wbio(s))) {
                /* Calls SSLfatal() as required */
                return dtls_wait_for_dry(s);
            }
#endif
        }
        break;

    case TLS_ST_PENDING_EARLY_DATA_END:
        /*
         * If we've been called by SSL_do_handshake()/SSL_write(), or we did not
         * attempt to write early data before calling SSL_read() then we press
         * on with the handshake. Otherwise we pause here.
         */
        if (s->early_data_state == SSL_EARLY_DATA_FINISHED_WRITING
                || s->early_data_state == SSL_EARLY_DATA_NONE)
            return WORK_FINISHED_CONTINUE;
        /* Fall through */

    case TLS_ST_EARLY_DATA:
        return tls_finish_handshake(s, wst, 0, 1);

    case TLS_ST_OK:
        /* Calls SSLfatal() as required */
        return tls_finish_handshake(s, wst, 1, 1);
    }

    return WORK_FINISHED_CONTINUE;
}

WORK_STATE GMold_ossl_statem_client_pre_work(SSL *s, WORK_STATE wst)
{
    OSSL_STATEM *st = &s->statem;
    printf("zzz GMold_ossl_statem client_pre_work 666666666 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_CW_CLNT_HELLO:
        s->shutdown = 0;
        if (SSL_IS_DTLS(s)) {
            /* every DTLS ClientHello resets Finished MAC */
            if (!ssl3_init_finished_mac(s)) {
                s->statem.state = MSG_FLOW_ERROR;
                return WORK_ERROR;
            }
        }
        break;

    case TLS_ST_CW_CHANGE:
        if (SSL_IS_DTLS(s)) {
            if (s->hit) {
                /*
                 * We're into the last flight so we don't retransmit these
                 * messages unless we need to.
                 */
                st->use_timer = 0;
            }
        }
        return WORK_FINISHED_CONTINUE;

    case TLS_ST_OK:
        return GMold_tls_finish_handshake(s, wst); 

    default:
        /* No pre work to be done */
        break;
    }

    return WORK_FINISHED_CONTINUE;
}

/*
 * Perform any work that needs to be done after sending a message from the
 * client to the server.
 */
WORK_STATE ossl_statem_client_post_work(SSL *s, WORK_STATE wst)
{
    printf("ddddddddddd ossl_statem_client_post_work \n");

    OSSL_STATEM *st = &s->statem;

    s->init_num = 0;

    switch (st->hand_state) {
    default:
        /* No post work to be done */
        break;

    case TLS_ST_CW_CLNT_HELLO:
        if (s->early_data_state == SSL_EARLY_DATA_CONNECTING
                && s->max_early_data > 0) {
            /*
             * We haven't selected TLSv1.3 yet so we don't call the change
             * cipher state function associated with the SSL_METHOD. Instead
             * we call tls13_change_cipher_state() directly.
             */
            if ((s->options & SSL_OP_ENABLE_MIDDLEBOX_COMPAT) == 0) {
                if (!tls13_change_cipher_state(s,
                            SSL3_CC_EARLY | SSL3_CHANGE_CIPHER_CLIENT_WRITE)) {
                    /* SSLfatal() already called */
                    return WORK_ERROR;
                }
            }
            /* else we're in compat mode so we delay flushing until after CCS */
        } else if (!statem_flush(s)) {
            return WORK_MORE_A;
        }

        if (SSL_IS_DTLS(s)) {
            /* Treat the next message as the first packet */
            s->first_packet = 1;
        }
        break;

    case TLS_ST_CW_END_OF_EARLY_DATA:
        /*
         * We set the enc_write_ctx back to NULL because we may end up writing
         * in cleartext again if we get a HelloRetryRequest from the server.
         */
        EVP_CIPHER_CTX_free(s->enc_write_ctx);
        s->enc_write_ctx = NULL;
        break;

    case TLS_ST_CW_KEY_EXCH:
        if (tls_client_key_exchange_post_work(s) == 0) {
            /* SSLfatal() already called */
            return WORK_ERROR;
        }
        break;

    case TLS_ST_CW_CHANGE:
        if (SSL_IS_TLS13(s) || s->hello_retry_request == SSL_HRR_PENDING)
            break;
        if (s->early_data_state == SSL_EARLY_DATA_CONNECTING
                    && s->max_early_data > 0) {
            /*
             * We haven't selected TLSv1.3 yet so we don't call the change
             * cipher state function associated with the SSL_METHOD. Instead
             * we call tls13_change_cipher_state() directly.
             */
            if (!tls13_change_cipher_state(s,
                        SSL3_CC_EARLY | SSL3_CHANGE_CIPHER_CLIENT_WRITE))
                return WORK_ERROR;
            break;
        }
        s->session->cipher = s->s3->tmp.new_cipher;
#ifdef OPENSSL_NO_COMP
        s->session->compress_meth = 0;
#else
        if (s->s3->tmp.new_compression == NULL)
            s->session->compress_meth = 0;
        else
            s->session->compress_meth = s->s3->tmp.new_compression->id;
#endif
        if (!s->method->ssl3_enc->setup_key_block(s)) {
            /* SSLfatal() already called */
            return WORK_ERROR;
        }

        if (!s->method->ssl3_enc->change_cipher_state(s,
                                          SSL3_CHANGE_CIPHER_CLIENT_WRITE)) {
            /* SSLfatal() already called */
            return WORK_ERROR;
        }

        if (SSL_IS_DTLS(s)) {
#ifndef OPENSSL_NO_SCTP
            if (s->hit) {
                /*
                 * Change to new shared key of SCTP-Auth, will be ignored if
                 * no SCTP used.
                 */
                BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_SCTP_NEXT_AUTH_KEY,
                         0, NULL);
            }
#endif

            dtls1_reset_seq_numbers(s, SSL3_CC_WRITE);
        }
        break;

    case TLS_ST_CW_FINISHED:
#ifndef OPENSSL_NO_SCTP
        if (wst == WORK_MORE_A && SSL_IS_DTLS(s) && s->hit == 0) {
            /*
             * Change to new shared key of SCTP-Auth, will be ignored if
             * no SCTP used.
             */
            BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_SCTP_NEXT_AUTH_KEY,
                     0, NULL);
        }
#endif
        if (statem_flush(s) != 1)
            return WORK_MORE_B;

        if (SSL_IS_TLS13(s)) {
            if (!tls13_save_handshake_digest_for_pha(s)) {
                /* SSLfatal() already called */
                return WORK_ERROR;
            }
            if (s->post_handshake_auth != SSL_PHA_REQUESTED) {
                if (!s->method->ssl3_enc->change_cipher_state(s,
                        SSL3_CC_APPLICATION | SSL3_CHANGE_CIPHER_CLIENT_WRITE)) {
                    /* SSLfatal() already called */
                    return WORK_ERROR;
                }
            }
        }
        break;

    case TLS_ST_CW_KEY_UPDATE:
        if (statem_flush(s) != 1)
            return WORK_MORE_A;
        if (!tls13_update_key(s, 1)) {
            /* SSLfatal() already called */
            return WORK_ERROR;
        }
        break;
    }

    return WORK_FINISHED_CONTINUE;
}

WORK_STATE GMold_ossl_statem_client_post_work(SSL *s, WORK_STATE wst)
{
    OSSL_STATEM *st = &s->statem;

    s->init_num = 0;
    printf("zzz GMold_ossl_statem client_post_work 7777777777 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_CW_CLNT_HELLO:
        if (wst == WORK_MORE_A && GMold_statem_flush(s) != 1)
            return WORK_MORE_A;

        if (SSL_IS_DTLS(s)) {
            /* Treat the next message as the first packet */
            s->first_packet = 1;
        }
        break;

    case TLS_ST_CW_KEY_EXCH:
        if (GMold_tls_client_key_exchange_post_work(s) == 0)
        {
            printf("err GMold_ossl_statem client_post_work 11111111111\n");
            return WORK_ERROR;
        }
            
        break;

    case TLS_ST_CW_CHANGE:
        s->session->cipher = s->s3->tmp.new_cipher;

        if (s->s3->tmp.new_compression == NULL)
            s->session->compress_meth = 0;
        else
            s->session->compress_meth = s->s3->tmp.new_compression->id;
        if (!s->method->ssl3_enc->setup_key_block(s))
        {
            printf("err GMold_ossl_statem client_post_work 222222222\n");
            return WORK_ERROR;
        }
        if (!s->method->ssl3_enc->change_cipher_state(s, SSL3_CHANGE_CIPHER_CLIENT_WRITE))
        {
            printf("err GMold_ossl_statem client_post_work 3333333333\n");
            return WORK_ERROR;
        } 
        break;
    case TLS_ST_CW_FINISHED:
        if (GMold_statem_flush(s) != 1)
        {
            return WORK_MORE_B;
        }
        break;

    default:
        /* No post work to be done */
        break;
    }

    return WORK_FINISHED_CONTINUE;
}

/*
 * Get the message construction function and message type for sending from the
 * client
 *
 * Valid return values are:
 *   1: Success
 *   0: Error
 */
int ossl_statem_client_construct_message(SSL *s, WPACKET *pkt,
                                         confunc_f *confunc, int *mt)
{
    printf("ddddddddddd ossl_statem_client_construct_message \n");

    OSSL_STATEM *st = &s->statem;

    switch (st->hand_state) {
    default:
        /* Shouldn't happen */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_OSSL_STATEM_CLIENT_CONSTRUCT_MESSAGE,
                 SSL_R_BAD_HANDSHAKE_STATE);
        return 0;

    case TLS_ST_CW_CHANGE:
        if (SSL_IS_DTLS(s))
            *confunc = dtls_construct_change_cipher_spec;
        else
            *confunc = tls_construct_change_cipher_spec;
        *mt = SSL3_MT_CHANGE_CIPHER_SPEC;
        break;

    case TLS_ST_CW_CLNT_HELLO:
        *confunc = tls_construct_client_hello;
        *mt = SSL3_MT_CLIENT_HELLO;
        break;

    case TLS_ST_CW_END_OF_EARLY_DATA:
        *confunc = tls_construct_end_of_early_data;
        *mt = SSL3_MT_END_OF_EARLY_DATA;
        break;

    case TLS_ST_PENDING_EARLY_DATA_END:
        *confunc = NULL;
        *mt = SSL3_MT_DUMMY;
        break;

    case TLS_ST_CW_CERT:
        *confunc = tls_construct_client_certificate;
        *mt = SSL3_MT_CERTIFICATE;
        break;

    case TLS_ST_CW_KEY_EXCH:
        *confunc = tls_construct_client_key_exchange;
        *mt = SSL3_MT_CLIENT_KEY_EXCHANGE;
        break;

    case TLS_ST_CW_CERT_VRFY:
        *confunc = tls_construct_cert_verify;
        *mt = SSL3_MT_CERTIFICATE_VERIFY;
        break;

#if !defined(OPENSSL_NO_NEXTPROTONEG)
    case TLS_ST_CW_NEXT_PROTO:
        *confunc = tls_construct_next_proto;
        *mt = SSL3_MT_NEXT_PROTO;
        break;
#endif
    case TLS_ST_CW_FINISHED:
        *confunc = tls_construct_finished;
        *mt = SSL3_MT_FINISHED;
        break;

    case TLS_ST_CW_KEY_UPDATE:
        *confunc = tls_construct_key_update;
        *mt = SSL3_MT_KEY_UPDATE;
        break;
    }

    return 1;
}

int GMold_ossl_statem_client_construct_message(SSL *s)
{
    OSSL_STATEM *st = &s->statem;
    printf("zzz GMold_ossl_statem client_construct_message 8888888888 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_CW_CLNT_HELLO:
        return GMold_tls_construct_client_hello(s);

    case TLS_ST_CW_CERT:
        return GMold_construct_client_certificate(s);

    case TLS_ST_CW_KEY_EXCH:
        return GMold_construct_client_key_exchange(s);

    case TLS_ST_CW_CERT_VRFY:
        return GMold_tls_construct_client_verify(s);

    case TLS_ST_CW_CHANGE:
        return GMold_tls_construct_change_cipher_spec(s);

    case TLS_ST_CW_NEXT_PROTO:
        return GMold_tls_construct_next_proto(s);
    case TLS_ST_CW_FINISHED:
        return GMold_tls_construct_finished(s,
                                      s->method->
                                      ssl3_enc->client_finished_label,
                                      s->method->
                                      ssl3_enc->client_finished_label_len);

    default:
        /* Shouldn't happen */
        break;
    }

    return 0;
}

/*
 * Returns the maximum allowed length for the current message that we are
 * reading. Excludes the message header.
 */
size_t ossl_statem_client_max_message_size(SSL *s)
{
    OSSL_STATEM *st = &s->statem;
    printf("zzz ossl_statem client_max_message_size 3333333 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    default:
        /* Shouldn't happen */
        return 0;

    case TLS_ST_CR_SRVR_HELLO:
        return SERVER_HELLO_MAX_LENGTH;

    case DTLS_ST_CR_HELLO_VERIFY_REQUEST:
        return HELLO_VERIFY_REQUEST_MAX_LENGTH;

    case TLS_ST_CR_CERT:
        return s->max_cert_list;

    case TLS_ST_CR_CERT_VRFY:
        return SSL3_RT_MAX_PLAIN_LENGTH;

    case TLS_ST_CR_CERT_STATUS:
        return SSL3_RT_MAX_PLAIN_LENGTH;

    case TLS_ST_CR_KEY_EXCH:
        return SERVER_KEY_EXCH_MAX_LENGTH;

    case TLS_ST_CR_CERT_REQ:
        /*
         * Set to s->max_cert_list for compatibility with previous releases. In
         * practice these messages can get quite long if servers are configured
         * to provide a long list of acceptable CAs
         */
        return s->max_cert_list;

    case TLS_ST_CR_SRVR_DONE:
        return SERVER_HELLO_DONE_MAX_LENGTH;

    case TLS_ST_CR_CHANGE:
        if (s->version == DTLS1_BAD_VER)
            return 3;
        return CCS_MAX_LENGTH;

    case TLS_ST_CR_SESSION_TICKET:
        return SSL3_RT_MAX_PLAIN_LENGTH;

    case TLS_ST_CR_FINISHED:
        return FINISHED_MAX_LENGTH;

    case TLS_ST_CR_ENCRYPTED_EXTENSIONS:
        return ENCRYPTED_EXTENSIONS_MAX_LENGTH;

    case TLS_ST_CR_KEY_UPDATE:
        return KEY_UPDATE_MAX_LENGTH;
    }
}

/*
 * Process a message that the client has been received from the server.
 */
MSG_PROCESS_RETURN ossl_statem_client_process_message(SSL *s, PACKET *pkt)
{
    printf("ddddddddddd ossl_statem_client_process_message \n");

    OSSL_STATEM *st = &s->statem;

    switch (st->hand_state) {
    default:
        /* Shouldn't happen */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_OSSL_STATEM_CLIENT_PROCESS_MESSAGE,
                 ERR_R_INTERNAL_ERROR);
        return MSG_PROCESS_ERROR;

    case TLS_ST_CR_SRVR_HELLO:
        return tls_process_server_hello(s, pkt);

    case DTLS_ST_CR_HELLO_VERIFY_REQUEST:
        return dtls_process_hello_verify(s, pkt);

    case TLS_ST_CR_CERT:
        return tls_process_server_certificate(s, pkt);

    case TLS_ST_CR_CERT_VRFY:
        return tls_process_cert_verify(s, pkt);

    case TLS_ST_CR_CERT_STATUS:
        return tls_process_cert_status(s, pkt);

    case TLS_ST_CR_KEY_EXCH:
        return tls_process_key_exchange(s, pkt);

    case TLS_ST_CR_CERT_REQ:
        return tls_process_certificate_request(s, pkt);

    case TLS_ST_CR_SRVR_DONE:
        return tls_process_server_done(s, pkt);

    case TLS_ST_CR_CHANGE:
        return tls_process_change_cipher_spec(s, pkt);

    case TLS_ST_CR_SESSION_TICKET:
        return tls_process_new_session_ticket(s, pkt);

    case TLS_ST_CR_FINISHED:
        return tls_process_finished(s, pkt);

    case TLS_ST_CR_HELLO_REQ:
        return tls_process_hello_req(s, pkt);

    case TLS_ST_CR_ENCRYPTED_EXTENSIONS:
        return tls_process_encrypted_extensions(s, pkt);

    case TLS_ST_CR_KEY_UPDATE:
        return tls_process_key_update(s, pkt);
    }
}

MSG_PROCESS_RETURN GMold_ossl_statem_client_process_message(SSL *s, PACKET *pkt)
{
    OSSL_STATEM *st = &s->statem;
    printf("zzz GMold_ossl_statem client_process_message 222222 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_CR_SRVR_HELLO:
        return GMold_tls_process_server_hello(s, pkt);

    case DTLS_ST_CR_HELLO_VERIFY_REQUEST:
        return dtls_process_hello_verify(s, pkt);

    case TLS_ST_CR_CERT:
            return GMold_tls_process_server_certificate(s, pkt);

    case TLS_ST_CR_CERT_STATUS:
        return tls_process_cert_status(s, pkt);

    case TLS_ST_CR_KEY_EXCH:
            return GMold_process_server_key_exchange(s, pkt);

    case TLS_ST_CR_CERT_REQ:
        return tls_process_certificate_request(s, pkt);

    case TLS_ST_CR_SRVR_DONE:
        return GMold_tls_process_server_done(s, pkt);

    case TLS_ST_CR_CHANGE:
        return tls_process_change_cipher_spec(s, pkt);

    case TLS_ST_CR_SESSION_TICKET:
        return tls_process_new_session_ticket(s, pkt);

    case TLS_ST_CR_FINISHED:
        return tls_process_finished(s, pkt);

    default:
        /* Shouldn't happen */
        break;
    }

    return MSG_PROCESS_ERROR;
}
/*
 * Perform any further processing required following the receipt of a message
 * from the server
 */
WORK_STATE ossl_statem_client_post_process_message(SSL *s, WORK_STATE wst)
{
    printf("ddddddddddd ossl_statem_client_post_process_message \n");
    OSSL_STATEM *st = &s->statem;
    switch (st->hand_state) {
    default:
        /* Shouldn't happen */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_OSSL_STATEM_CLIENT_POST_PROCESS_MESSAGE,
                 ERR_R_INTERNAL_ERROR);
        return WORK_ERROR;

    case TLS_ST_CR_CERT_VRFY:
    case TLS_ST_CR_CERT_REQ:
        return tls_prepare_client_certificate(s, wst);
    }
}


WORK_STATE GMold_ossl_statem_client_post_process_message(SSL *s, WORK_STATE wst)
{
    OSSL_STATEM *st = &s->statem;
    printf("zzz ossl_statem_client_post_process_message 444444 hand_state:%d\n",st->hand_state);
    switch (st->hand_state) {
    case TLS_ST_CR_CERT_REQ:
        return GMold_tls_prepare_client_certificate(s, wst);

    default:
        break;
    }

    /* Shouldn't happen */
    return WORK_ERROR;
}

int tls_construct_client_hello(SSL *s, WPACKET *pkt)
{
    printf("ddddddddddd tls_construct_client_hello \n");

    unsigned char *p;
    size_t sess_id_len;
    int i, protverr;
#ifndef OPENSSL_NO_COMP
    SSL_COMP *comp;
#endif
    SSL_SESSION *sess = s->session;
    unsigned char *session_id;

    /* Work out what SSL/TLS/DTLS version to use */
    protverr = ssl_set_client_hello_version(s);
    if (protverr != 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 protverr);
        return 0;
    }

    if (sess == NULL
            || !ssl_version_supported(s, sess->ssl_version, NULL)
            || !SSL_SESSION_is_resumable(sess)) {
        if (s->hello_retry_request == SSL_HRR_NONE
                && !ssl_get_new_session(s, 0)) {
            /* SSLfatal() already called */
            return 0;
        }
    }
    /* else use the pre-loaded session */

    p = s->s3->client_random;

    /*
     * for DTLS if client_random is initialized, reuse it, we are
     * required to use same upon reply to HelloVerify
     */
    if (SSL_IS_DTLS(s)) {
        size_t idx;
        i = 1;
        for (idx = 0; idx < sizeof(s->s3->client_random); idx++) {
            if (p[idx]) {
                i = 0;
                break;
            }
        }
    } else {
        i = (s->hello_retry_request == SSL_HRR_NONE);
    }

    if (i && ssl_fill_hello_random(s, 0, p, sizeof(s->s3->client_random),
                                   DOWNGRADE_NONE) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /*-
     * version indicates the negotiated version: for example from
     * an SSLv2/v3 compatible client hello). The client_version
     * field is the maximum version we permit and it is also
     * used in RSA encrypted premaster secrets. Some servers can
     * choke if we initially report a higher version then
     * renegotiate to a lower one in the premaster secret. This
     * didn't happen with TLS 1.0 as most servers supported it
     * but it can with TLS 1.1 or later if the server only supports
     * 1.0.
     *
     * Possible scenario with previous logic:
     *      1. Client hello indicates TLS 1.2
     *      2. Server hello says TLS 1.0
     *      3. RSA encrypted premaster secret uses 1.2.
     *      4. Handshake proceeds using TLS 1.0.
     *      5. Server sends hello request to renegotiate.
     *      6. Client hello indicates TLS v1.0 as we now
     *         know that is maximum server supports.
     *      7. Server chokes on RSA encrypted premaster secret
     *         containing version 1.0.
     *
     * For interoperability it should be OK to always use the
     * maximum version we support in client hello and then rely
     * on the checking of version to ensure the servers isn't
     * being inconsistent: for example initially negotiating with
     * TLS 1.0 and renegotiating with TLS 1.2. We do this by using
     * client_version in client hello and not resetting it to
     * the negotiated version.
     *
     * For TLS 1.3 we always set the ClientHello version to 1.2 and rely on the
     * supported_versions extension for the real supported versions.
     */
    if (!WPACKET_put_bytes_u16(pkt, s->client_version)
            || !WPACKET_memcpy(pkt, s->s3->client_random, SSL3_RANDOM_SIZE)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /* Session ID */
    session_id = s->session->session_id;
    if (s->new_session || s->session->ssl_version == TLS1_3_VERSION) {
        if (s->version == TLS1_3_VERSION
                && (s->options & SSL_OP_ENABLE_MIDDLEBOX_COMPAT) != 0) {
            sess_id_len = sizeof(s->tmp_session_id);
            s->tmp_session_id_len = sess_id_len;
            session_id = s->tmp_session_id;
            if (s->hello_retry_request == SSL_HRR_NONE
                    && RAND_bytes(s->tmp_session_id, sess_id_len) <= 0) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                         ERR_R_INTERNAL_ERROR);
                return 0;
            }
        } else {
            sess_id_len = 0;
        }
    } else {
        assert(s->session->session_id_length <= sizeof(s->session->session_id));
        sess_id_len = s->session->session_id_length;
        if (s->version == TLS1_3_VERSION) {
            s->tmp_session_id_len = sess_id_len;
            memcpy(s->tmp_session_id, s->session->session_id, sess_id_len);
        }
    }
    if (!WPACKET_start_sub_packet_u8(pkt)
            || (sess_id_len != 0 && !WPACKET_memcpy(pkt, session_id,
                                                    sess_id_len))
            || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /* cookie stuff for DTLS */
    if (SSL_IS_DTLS(s)) {
        if (s->d1->cookie_len > sizeof(s->d1->cookie)
                || !WPACKET_sub_memcpy_u8(pkt, s->d1->cookie,
                                          s->d1->cookie_len)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                     ERR_R_INTERNAL_ERROR);
            return 0;
        }
    }

    /* Ciphers supported */
    if (!WPACKET_start_sub_packet_u16(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    if (!ssl_cipher_list_to_bytes(s, SSL_get_ciphers(s), pkt)) {
        /* SSLfatal() already called */
        return 0;
    }
    if (!WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /* COMPRESSION */
    if (!WPACKET_start_sub_packet_u8(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
#ifndef OPENSSL_NO_COMP
    if (ssl_allow_compression(s)
            && s->ctx->comp_methods
            && (SSL_IS_DTLS(s) || s->s3->tmp.max_ver < TLS1_3_VERSION)) {
        int compnum = sk_SSL_COMP_num(s->ctx->comp_methods);
        for (i = 0; i < compnum; i++) {
            comp = sk_SSL_COMP_value(s->ctx->comp_methods, i);
            if (!WPACKET_put_bytes_u8(pkt, comp->id)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                         ERR_R_INTERNAL_ERROR);
                return 0;
            }
        }
    }
#endif
    /* Add the NULL method */
    if (!WPACKET_put_bytes_u8(pkt, 0) || !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CLIENT_HELLO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    /* TLS extensions */
    if (!tls_construct_extensions(s, pkt, SSL_EXT_CLIENT_HELLO, NULL, 0)) {
        /* SSLfatal() already called */
        return 0;
    }

    return 1;
}

int GMold_tls_construct_client_hello(SSL *s)
{
    unsigned char *buf;
    unsigned char *p, *d;
    int i;
    int protverr;
    unsigned long l;
    int al = 0;
    int j;
    SSL_COMP *comp;
    SSL_SESSION *sess = s->session;

    buf = (unsigned char *)s->init_buf->data;
    /* 确定要使用的SSL/TLS/DTLS版本 */
    protverr = ssl_set_client_hello_version(s);
    if (protverr != 0) 
    {
        printf("GMold_tls_construct_client_hello 11111111111111111\n");
        goto err;
    }

    if ((sess == NULL) || !GMold_ssl_version_supported(s, sess->ssl_version) ||
        /*
         * 在EAP-FAST的情况下，我们可以有一个预共享的“票证”，而不需要会话ID。
         */
        (!sess->session_id_length && !sess->ext.tick) ||
        (sess->not_resumable)) {
        if (!ssl_get_new_session(s, 0))
        {
            printf("GMold_tls_construct_client_hello 222222222222222222222\n");
            goto err;
        }
    }
    /* 否则请使用预加载的会话 */

    p = s->s3->client_random;

    /*
     * 对于DTL，如果初始化了client_random，请重用它，我们需要在回复HelloVerify时使用相同的
     */
    if (SSL_IS_DTLS(s)) {
        size_t idx;
        i = 1;
        for (idx = 0; idx < sizeof(s->s3->client_random); idx++) {
            if (p[idx]) {
                i = 0;
                break;
            }
        }
    } else
        i = 1;

    if (i && GMold_ssl_fill_hello_random(s, 0, p, sizeof(s->s3->client_random)) <= 0)
    {
        printf("GMold_tls_construct_client_hello 33333333333333333333333\n");
        goto err;
    }
//    memset(p,1,sizeof (s->s3->client_random));  //TODO xcst

    /* 消息类型和长度是否持续 */
    d = p = (((unsigned char *)s->init_buf->data) + 4); //ssl握手启动

    /*-
     * version indicates the negotiated version: for example from
     * an SSLv2/v3 compatible client hello). The client_version
     * field is the maximum version we permit and it is also
     * used in RSA encrypted premaster secrets. Some servers can
     * choke if we initially report a higher version then
     * renegotiate to a lower one in the premaster secret. This
     * didn't happen with TLS 1.0 as most servers supported it
     * but it can with TLS 1.1 or later if the server only supports
     * 1.0.
     *
     * Possible scenario with previous logic:
     *      1. Client hello indicates TLS 1.2
     *      2. Server hello says TLS 1.0
     *      3. RSA encrypted premaster secret uses 1.2.
     *      4. Handshake proceeds using TLS 1.0.
     *      5. Server sends hello request to renegotiate.
     *      6. Client hello indicates TLS v1.0 as we now
     *         know that is maximum server supports.
     *      7. Server chokes on RSA encrypted premaster secret
     *         containing version 1.0.
     * For interoperability it should be OK to always use the
     * maximum version we support in client hello and then rely
     * on the checking of version to ensure the servers isn't
     * being inconsistent: for example initially negotiating with
     * TLS 1.0 and renegotiating with TLS 1.2. We do this by using
     * client_version in client hello and not resetting it to
     * the negotiated version.
    version表示协商的版本：例如，来自SSLv2/v3兼容的客户端（hello）。client_version字段是我们允许的最大版本，
    也是用于RSA加密的主密钥。有些服务器可以如果我们最初报告的是更高版本重新协商一个较低级别的premaster机密。
    这TLS 1.0没有出现，因为大多数服务器都支持它但是，如果服务器只支持TLS 1.1或更高版本，它可以使用TLS 1.1或更高版本1.0.
    之前逻辑的可能场景：
    1.客户端hello表示TLS 1.2
    2.服务器hello表示TLS 1.0
    3.RSA加密的premaster secret使用1.2。
    4.使用TLS 1.0进行握手。
    5.服务器发送hello请求以重新协商。
    6.客户端hello表示TLS v1。就像我们现在一样知道这是最大的服务器支持。
    7.服务器被RSA加密的premaster secret阻塞包含1.0版。
    对于互操作性，始终使用我们在客户端hello中支持的最大版本，然后依靠检查版本以确保服务器不受影响不一致：
    例如最初与TLS 1.0并与TLS 1.2重新协商。我们通过使用客户端hello中的客户端_版本，不将其重置为协商版本。
     */
    *(p++) = s->client_version >> 8;     //1个字节
    *(p++) = s->client_version & 0xff;  //2个字节

    /* 随机东西 */
    memcpy(p, s->s3->client_random, SSL3_RANDOM_SIZE); //34个字节
    p += SSL3_RANDOM_SIZE;
    
    /* 会话ID */
    if (s->new_session)
    {
        i = 0;
    }
    else
    {
        i = s->session->session_id_length;
    }
    *(p++) = i;             //35个字节
    if (i != 0) 
    {
        printf("xxxxxxxxxxxxxxx tls construct_client_hello i == %d\n",i);
        if (i > (int)sizeof(s->session->session_id)) 
        {
            printf("GMold_tls_construct_client_hello 44444444444444444444444\n");
            goto err;
        }
        memcpy(p, s->session->session_id, i);       //+32字节
        p += i;
    }

    /* DTLS的cookie材料 DTLS协议在UDP提供的socket之上实现了客户机与服务器双方的握手连接*/
    if (SSL_IS_DTLS(s)) 
    {
        printf("tls construct_client_hello i == %d\n",i);
        if (s->d1->cookie_len > sizeof(s->d1->cookie)) 
        {
            printf("GMold_tls_construct_client_hello 55555555555555555555\n");
            goto err;
        }
        *(p++) = s->d1->cookie_len;
        memcpy(p, s->d1->cookie, s->d1->cookie_len);    //+256个字节
        p += s->d1->cookie_len;
    }

    /* 支持的密码套件 */
    i = GMold_ssl_cipher_list_to_bytes(s, SSL_get_ciphers(s), &(p[2]));
    if (i == 0) 
    {
        printf("GMold_tls_construct_client_hello 66666666666666666666\n");
        goto err;
    }

    s2n(i, p);
    p += i;

    /* 压缩 */

    
    if (!GMold_ssl_allow_compression(s) || !s->ctx->comp_methods)
        j = 0;
    else
        j = sk_SSL_COMP_num(s->ctx->comp_methods);
    *(p++) = 1 + j;
    for (i = 0; i < j; i++) {
        comp = sk_SSL_COMP_value(s->ctx->comp_methods, i);
        *(p++) = comp->id;
    }
    *(p++) = 0;                 /* 添加NULL方法 */
    /* TLS扩展 */
    s->s3->alpn_sent = 0;

    l = p - d;
    if (!GMold_ssl3_set_handshake_header(s, SSL3_MT_CLIENT_HELLO, l)) {
        // ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_HANDSHAKE_FAILURE);
        printf("GMold_tls_construct_client_hello 777777777777777777777777\n");
        goto err;
    }
    return 1;
 err:
    s->statem.state = MSG_FLOW_ERROR;
    return 0;
}

int GMold_construct_client_certificate(SSL *s)
{
	int al = -1;
	unsigned long alg_a = s->s3->tmp.new_cipher->algorithm_auth;
	unsigned char *p;
	int l = 3 + 4;

    if (!GMold_output_cert_chain(s, &l,GMold_SSL_PKEY_SM2,SSL_PKEY_ECC_ENC)) {
			goto err;
	}

	if (!GMold_ssl3_set_handshake_header(s, SSL3_MT_CERTIFICATE, l)) {
		return 0;
	}

	return 1;

err:
	ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
	s->statem.state = MSG_FLOW_ERROR;
	return 0;
}

int GMold_output_cert_chain(SSL *s, int *len, int a_idx, int k_idx)
{
	unsigned char *p;
	unsigned long l = *len;
	BUF_MEM *buf = s->init_buf;
	int i;
	STACK_OF(X509) *extra_certs;
	STACK_OF(X509) *chain = NULL;
	X509_STORE *chain_store;
	CERT_PKEY *a_cpk;
	CERT_PKEY *k_cpk;

	if (!BUF_MEM_grow_clean(buf, 10)) {
		return 0;
	}

	a_cpk = &s->cert->pkeys[a_idx];
	k_cpk = &s->cert->pkeys[k_idx];

	if (a_cpk->chain)
		extra_certs = a_cpk->chain;
	else if (k_cpk->chain)
		extra_certs = k_cpk->chain;
	else
		extra_certs = s->ctx->extra_certs;

	if ((s->mode & SSL_MODE_NO_AUTO_CHAIN) || extra_certs)
		chain_store = NULL;
	else if (s->cert->chain_store)
		chain_store = s->cert->chain_store;
	else
		chain_store = s->ctx->cert_store;

	if (chain_store) {
		X509_STORE_CTX *xs_ctx = X509_STORE_CTX_new();

		if (xs_ctx == NULL) {
			return (0);
		}
                if (!GMold_X509_STORE_CTX_init(xs_ctx, chain_store, a_cpk->x509, NULL)) {
			X509_STORE_CTX_free(xs_ctx);
			return (0);
		}
		/*
		* It is valid for the chain not to be complete (because normally we
		* don't include the root cert in the chain). Therefore we deliberately
		* ignore the error return from this call. We're not actually verifying
		* the cert - we're just building as much of the chain as we can
		*/
                (void)GMold_X509_verify_cert(xs_ctx);
		/* Don't leave errors in the queue */
		ERR_clear_error();
		chain = X509_STORE_CTX_get0_chain(xs_ctx);

                i = GMold_ssl_security_cert_chain(s, chain, NULL, 0);
		if (i != 1) {
			X509_STORE_CTX_free(xs_ctx);
			return 0;
		}

		/* add signing certificate */
                if (!GMold_ssl_add_cert_to_buf(buf, &l, s->cert->pkeys[a_idx].x509)) {
			return 0;
		}
		/* add key exchange certificate */
                if (!GMold_ssl_add_cert_to_buf(buf, &l, s->cert->pkeys[k_idx].x509)) {
			return 0;
		}
		/* add the following chain */
		for (i = 1; i < sk_X509_num(chain); i++) {
			X509 *x = sk_X509_value(chain, i);
                        if (!GMold_ssl_add_cert_to_buf(buf, &l, x)) {
				X509_STORE_CTX_free(xs_ctx);
				return 0;
			}
		}
		X509_STORE_CTX_free(xs_ctx);

	} else {

                i = GMold_ssl_security_cert_chain(s, extra_certs, a_cpk->x509, 0);
		if (i != 1) {
			return 0;
		}

		/* output sign cert and exch cert */
                if (!GMold_ssl_add_cert_to_buf(buf, &l, s->cert->pkeys[a_idx].x509)) {
			return 0;
		}
                if (!GMold_ssl_add_cert_to_buf(buf, &l, s->cert->pkeys[k_idx].x509)) {
			return 0;
		}
		/* output the following chain */
		for (i = 0; i < sk_X509_num(extra_certs); i++) {
			X509 *x = sk_X509_value(extra_certs, i);
                        if (!GMold_ssl_add_cert_to_buf(buf, &l, x)) {
				return 0;
			}
		}
	}

	l -= 3 + 4;
	p = (((unsigned char *)s->init_buf->data) + 4);
	l2n3(l, p);
	l += 3;

    *len = (int)l;
    return 1;
}
int GMold_construct_cke_sm2(SSL *s, unsigned char **p, int *l, int *al);
int GMold_construct_client_key_exchange(SSL *s)
{
	int al = -1;
	unsigned long alg_k = s->s3->tmp.new_cipher->algorithm_mkey;
	unsigned char *p = (((unsigned char *)s->init_buf->data) + 4);
	int l;

	if (alg_k & SSL_kECC) {
		
		if (!GMold_construct_cke_sm2(s, &p, &l, &al)) {
            printf("gmtls_construct_client_key_exchange 111111111111111111111\n");
			goto err;
		}
	} else {
		if (!GMold_construct_cke_sm2(s, &p, &l, &al)) {
            printf("gmtls_construct_client_key_exchange 2222222222222222222222\n");
			goto err;
		}
	}

	if (!GMold_ssl3_set_handshake_header(s, SSL3_MT_CLIENT_KEY_EXCHANGE, l)) {
        printf("gmtls_construct_client_key_exchange 33333333333333333333\n");
		ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_HANDSHAKE_FAILURE);
		goto err;
	}

	return 1;
err:
	if (al != -1)
		ssl3_send_alert(s, SSL3_AL_FATAL, al);
	OPENSSL_clear_free(s->s3->tmp.pms, s->s3->tmp.pmslen);				
	s->s3->tmp.pms = NULL;				
	s->statem.state = MSG_FLOW_ERROR;
	return 0;
}

int GMold_construct_cke_sm2(SSL *s, unsigned char **p, int *l, int *al)
{
	int ret = 0;
	unsigned char *d;
	EVP_PKEY *pkey = NULL;
	EVP_PKEY_CTX *pctx = NULL;
	size_t enclen;
	unsigned char *pms = NULL;
	size_t pmslen = 0;
	X509 *x509;

	*al = SSL_AD_INTERNAL_ERROR;

	/* get sm2 encryption key from enc cert从enc证书获取sm2加密密钥 */
	if (!(s->session->peer_chain)) {
        printf("GMold_construct cke_sm2 111111111111111111111\n");
		return 0;
	}
        if (!(x509 = sk_X509_value(s->session->peer_chain, 1))) {
        printf("GMold_construct cke_sm2 22222222222222222222\n");
		return 0;
	}
        pkey = GMold_X509_get0_pubkey(x509);
	if (!EVP_PKEY_get0_EC_KEY(pkey)) {
        printf("GMold_construct cke_sm2 3333333333333333\n");
		return 0;
	}

	/* generate pre_master_secret 生成预主密钥*/
	pmslen = SSL_MAX_MASTER_KEY_LENGTH;
	if (!(pms = OPENSSL_malloc(pmslen))) {
        printf("GMold_construct cke_sm2 444444444444444\n");
		return 0;
	}

	pms[0] = s->client_version >> 8;
	pms[1] = s->client_version & 0xff;
        if (GMold_RAND_bytes(pms + 2, pmslen - 2) <= 0) {
        printf("GMold_construct cke_sm2 5555555555555555555555\n");
                goto end;
        }
//        memset(pms + 2,3,pmslen - 2);       //TODO xcst
#ifdef CIPHER_DEBUG
    {
        int gi = 0;
        printf("client --->the pre mask key =[");
        for(gi=0; gi<pmslen; gi++){
                printf("%02X-", pms[gi]);
        }
        printf("]\n");
    }       //TODO xcs test
#endif
	/* encrypt pre_master_secret 加密预主密钥*/
	if (!(pctx =  GMold_EVP_PKEY_CTX_new(pkey, NULL))) {
        printf("GMold_construct cke_sm2  666666666666666666\n");
		goto end;
	}

    if(EVP_PKEY_encrypt_init(pctx) <= 0)
    {
        printf("GMold_construct cke_sm2 7777777777777777777777\n");
        goto end;
    }  
    if(!GMold_EVP_PKEY_CTX_ctrl(pctx, EVP_PKEY_EC, EVP_PKEY_OP_SIGN|EVP_PKEY_OP_SIGNCTX| EVP_PKEY_OP_VERIFY|EVP_PKEY_OP_VERIFYCTX| EVP_PKEY_OP_ENCRYPT|EVP_PKEY_OP_DECRYPT| EVP_PKEY_OP_DERIVE, EVP_PKEY_CTRL_EC_SCHEME, GMold_NID_sm_scheme, NULL))
    {
        printf("GMold_construct cke_sm2 7777777777777777777777.3\n");
        goto end;
    }
        if (!GMold_EVP_PKEY_CTX_ctrl(pctx, EVP_PKEY_EC, EVP_PKEY_OP_ENCRYPT|EVP_PKEY_OP_DECRYPT, EVP_PKEY_CTRL_EC_ENCRYPT_PARAM, GMold_NID_sm3, NULL)){
            printf("GMold_construct cke_sm2 7777777777777777777777.6\n");
                goto end;
        }
        if (GMold_EVP_PKEY_encrypt(pctx, NULL, &enclen, pms, pmslen) <= 0) {
        printf("GMold_construct cke_sm2 88888888888888888888888888\n");
		goto end;
	}

	d = *p;
	if (GMold_EVP_PKEY_encrypt(pctx, &(d[2]), &enclen, pms, pmslen) <= 0) {
        printf("GMold_construct cke_sm2 9999999999999999999\n");
		goto end;
	}

#ifdef CIPHER_DEBUG
        {
            int gi = 0;
            printf("client encrypt data--->the pre mask key =[");
            for(gi=0; gi<enclen; gi++){
                    printf("%02X-", d[gi]);
            }
            printf("]\n");
        }           //TODO xcs test
#endif
	s2n(enclen, d);
	d += enclen;
        /* save pre_master_secret */
	if (s->s3->tmp.pms) {
        printf("GMold_construct cke_sm2 aaaaaaaaaaaaaaaaa\n");
		goto end;
	}
	s->s3->tmp.pms = pms;
	s->s3->tmp.pmslen = pmslen;
	pms = NULL;

	*p = d;
	*l = 2 + enclen;
	*al = -1;
	ret = 1;
end:
	OPENSSL_clear_free(pms, pmslen);
	EVP_PKEY_CTX_free(pctx);
	return ret;
}

int GMold_tls_construct_client_verify(SSL *s)
{
    unsigned char *p;
    EVP_PKEY *pkey;
    const EVP_MD *md = GMold_EVP_sm3();
    EVP_MD_CTX *mctx;
    unsigned u = 0;
    unsigned long n = 0;
    long hdatalen = 0;
    void *hdata;

    mctx = EVP_MD_CTX_new();
    if (mctx == NULL) {
        goto err;
    }
    p = (((unsigned char *)s->init_buf->data) + 4);
    if (s->cert->pkeys[GMold_SSL_PKEY_SM2].privatekey)
        pkey = s->cert->pkeys[GMold_SSL_PKEY_SM2].privatekey;
    else
        pkey = s->cert->key->privatekey;

    GMold_X509_print_fp(stderr, s->cert->pkeys[GMold_SSL_PKEY_SM2].x509);
    GMold_X509_print_fp(stderr, s->cert->pkeys[SSL_PKEY_ECC_ENC].x509);
    hdatalen = GMold_BIO_ctrl(s->s3->handshake_buffer,BIO_CTRL_INFO,0,(char *)(&hdata));
    if (hdatalen <= 0) {
        goto err;
    }
    if (SSL_USE_SIGALGS(s)) {
        if (!GMold_tls12_get_sigandhash(p, pkey, md)) {
            goto err;
        }
        p += 2;
        n = 2;
    }
    fprintf(stderr, "xxxxxxxxxxx Using client alg %s\n", GMold_EVP_MD_name(md));
    if (!GMold_EVP_DigestInit_ex(mctx, md, NULL)
        || !GMold_EVP_DigestUpdate(mctx, hdata, hdatalen)
        || (s->version == SSL3_VERSION
            && !EVP_MD_CTX_ctrl(mctx, EVP_CTRL_SSL3_MASTER_SECRET,
                                s->session->master_key_length,
                                s->session->master_key))
        || !GMold_EVP_SignFinal(mctx, p + 2, &u, pkey)) {
        goto err;
    }
#ifndef OPENSSL_NO_GOST
    {
        int pktype = EVP_PKEY_id(pkey);
        if (pktype == GMold_NID_id_GostR3410_2001
            || pktype == GMold_NID_id_GostR3410_2012_256
            || pktype == GMold_NID_id_GostR3410_2012_512)
            BUF_reverse(p + 2, NULL, u);
    }
#endif
    // {
    //     int gi = 0;
    //     printf("GMold tls construct_client_verify data =[");
    //     for(gi=0; gi<u; gi++){
    //             printf("%02X-", p[gi+2]);
    //     }
    //     printf("]\n");
    // }                //TODO xcs test
    s2n(u, p);
    n += u + 2;
    /* Digest cached records and discard handshake buffer */
    if (!GMold_ssl3_digest_cached_records(s, 0))
        goto err;
    if (!GMold_ssl3_set_handshake_header(s, SSL3_MT_CERTIFICATE_VERIFY, n)) {
        goto err;
    }

    EVP_MD_CTX_free(mctx);
    return 1;
 err:
    EVP_MD_CTX_free(mctx);
    return 0;
}

MSG_PROCESS_RETURN dtls_process_hello_verify(SSL *s, PACKET *pkt)
{
    size_t cookie_len;
    PACKET cookiepkt;

    if (!PACKET_forward(pkt, 2)
        || !PACKET_get_length_prefixed_1(pkt, &cookiepkt)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_DTLS_PROCESS_HELLO_VERIFY,
                 SSL_R_LENGTH_MISMATCH);
        return MSG_PROCESS_ERROR;
    }

    cookie_len = PACKET_remaining(&cookiepkt);
    if (cookie_len > sizeof(s->d1->cookie)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_DTLS_PROCESS_HELLO_VERIFY,
                 SSL_R_LENGTH_TOO_LONG);
        return MSG_PROCESS_ERROR;
    }

    if (!PACKET_copy_bytes(&cookiepkt, s->d1->cookie, cookie_len)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_DTLS_PROCESS_HELLO_VERIFY,
                 SSL_R_LENGTH_MISMATCH);
        return MSG_PROCESS_ERROR;
    }
    s->d1->cookie_len = cookie_len;

    return MSG_PROCESS_FINISHED_READING;
}

static int set_client_ciphersuite(SSL *s, const unsigned char *cipherchars)
{
    STACK_OF(SSL_CIPHER) *sk;
    const SSL_CIPHER *c;
    int i;

    c = ssl_get_cipher_by_char(s, cipherchars, 0);
    if (c == NULL) {
        /* unknown cipher */
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_SET_CLIENT_CIPHERSUITE,
                 SSL_R_UNKNOWN_CIPHER_RETURNED);
        return 0;
    }
    /*
     * If it is a disabled cipher we either didn't send it in client hello,
     * or it's not allowed for the selected protocol. So we return an error.
     */
    if (ssl_cipher_disabled(s, c, SSL_SECOP_CIPHER_CHECK, 1)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_SET_CLIENT_CIPHERSUITE,
                 SSL_R_WRONG_CIPHER_RETURNED);
        return 0;
    }

    sk = ssl_get_ciphers_by_id(s);
    i = sk_SSL_CIPHER_find(sk, c);
    if (i < 0) {
        /* we did not say we would use this cipher */
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_SET_CLIENT_CIPHERSUITE,
                 SSL_R_WRONG_CIPHER_RETURNED);
        return 0;
    }

    if (SSL_IS_TLS13(s) && s->s3->tmp.new_cipher != NULL
            && s->s3->tmp.new_cipher->id != c->id) {
        /* ServerHello selected a different ciphersuite to that in the HRR */
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_SET_CLIENT_CIPHERSUITE,
                 SSL_R_WRONG_CIPHER_RETURNED);
        return 0;
    }

    /*
     * Depending on the session caching (internal/external), the cipher
     * and/or cipher_id values may not be set. Make sure that cipher_id is
     * set and use it for comparison.
     */
    if (s->session->cipher != NULL)
        s->session->cipher_id = s->session->cipher->id;
    if (s->hit && (s->session->cipher_id != c->id)) {
        if (SSL_IS_TLS13(s)) {
            /*
             * In TLSv1.3 it is valid for the server to select a different
             * ciphersuite as long as the hash is the same.
             */
            if (ssl_md(c->algorithm2)
                    != ssl_md(s->session->cipher->algorithm2)) {
                SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                         SSL_F_SET_CLIENT_CIPHERSUITE,
                         SSL_R_CIPHERSUITE_DIGEST_HAS_CHANGED);
                return 0;
            }
        } else {
            /*
             * Prior to TLSv1.3 resuming a session always meant using the same
             * ciphersuite.
             */
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_SET_CLIENT_CIPHERSUITE,
                     SSL_R_OLD_SESSION_CIPHER_NOT_RETURNED);
            return 0;
        }
    }
    s->s3->tmp.new_cipher = c;

    return 1;
}

MSG_PROCESS_RETURN tls_process_server_hello(SSL *s, PACKET *pkt)
{
    printf("ddddddddddd tls_process_server_hello \n");
    
    PACKET session_id, extpkt;
    size_t session_id_len;
    const unsigned char *cipherchars;
    int hrr = 0;
    unsigned int compression;
    unsigned int sversion;
    unsigned int context;
    RAW_EXTENSION *extensions = NULL;
#ifndef OPENSSL_NO_COMP
    SSL_COMP *comp;
#endif

    if (!PACKET_get_net_2(pkt, &sversion)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    /* load the server random */
    if (s->version == TLS1_3_VERSION
            && sversion == TLS1_2_VERSION
            && PACKET_remaining(pkt) >= SSL3_RANDOM_SIZE
            && memcmp(hrrrandom, PACKET_data(pkt), SSL3_RANDOM_SIZE) == 0) {
        s->hello_retry_request = SSL_HRR_PENDING;
        hrr = 1;
        if (!PACKET_forward(pkt, SSL3_RANDOM_SIZE)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                     SSL_R_LENGTH_MISMATCH);
            goto err;
        }
    } else {
        if (!PACKET_copy_bytes(pkt, s->s3->server_random, SSL3_RANDOM_SIZE)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                     SSL_R_LENGTH_MISMATCH);
            goto err;
        }
    }

    /* Get the session-id. */
    if (!PACKET_get_length_prefixed_1(pkt, &session_id)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }
    session_id_len = PACKET_remaining(&session_id);
    if (session_id_len > sizeof(s->session->session_id)
        || session_id_len > SSL3_SESSION_ID_SIZE) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_SSL3_SESSION_ID_TOO_LONG);
        goto err;
    }

    if (!PACKET_get_bytes(pkt, &cipherchars, TLS_CIPHER_LEN)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    if (!PACKET_get_1(pkt, &compression)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    /* TLS extensions */
    if (PACKET_remaining(pkt) == 0 && !hrr) {
        PACKET_null_init(&extpkt);
    } else if (!PACKET_as_length_prefixed_2(pkt, &extpkt)
               || PACKET_remaining(pkt) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_BAD_LENGTH);
        goto err;
    }

    if (!hrr) {
        if (!tls_collect_extensions(s, &extpkt,
                                    SSL_EXT_TLS1_2_SERVER_HELLO
                                    | SSL_EXT_TLS1_3_SERVER_HELLO,
                                    &extensions, NULL, 1)) {
            /* SSLfatal() already called */
            goto err;
        }

        if (!ssl_choose_client_version(s, sversion, extensions)) {
            /* SSLfatal() already called */
            goto err;
        }
    }

    if (SSL_IS_TLS13(s) || hrr) {
        if (compression != 0) {
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                     SSL_F_TLS_PROCESS_SERVER_HELLO,
                     SSL_R_INVALID_COMPRESSION_ALGORITHM);
            goto err;
        }

        if (session_id_len != s->tmp_session_id_len
                || memcmp(PACKET_data(&session_id), s->tmp_session_id,
                          session_id_len) != 0) {
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                     SSL_F_TLS_PROCESS_SERVER_HELLO, SSL_R_INVALID_SESSION_ID);
            goto err;
        }
    }

    if (hrr) {
        if (!set_client_ciphersuite(s, cipherchars)) {
            /* SSLfatal() already called */
            goto err;
        }

        return tls_process_as_hello_retry_request(s, &extpkt);
    }

    /*
     * Now we have chosen the version we need to check again that the extensions
     * are appropriate for this version.
     */
    context = SSL_IS_TLS13(s) ? SSL_EXT_TLS1_3_SERVER_HELLO
                              : SSL_EXT_TLS1_2_SERVER_HELLO;
    if (!tls_validate_all_contexts(s, context, extensions)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_BAD_EXTENSION);
        goto err;
    }

    s->hit = 0;

    if (SSL_IS_TLS13(s)) {
        /*
         * In TLSv1.3 a ServerHello message signals a key change so the end of
         * the message must be on a record boundary.
         */
        if (RECORD_LAYER_processed_read_pending(&s->rlayer)) {
            SSLfatal(s, SSL_AD_UNEXPECTED_MESSAGE,
                     SSL_F_TLS_PROCESS_SERVER_HELLO,
                     SSL_R_NOT_ON_RECORD_BOUNDARY);
            goto err;
        }

        /* This will set s->hit if we are resuming */
        if (!tls_parse_extension(s, TLSEXT_IDX_psk,
                                 SSL_EXT_TLS1_3_SERVER_HELLO,
                                 extensions, NULL, 0)) {
            /* SSLfatal() already called */
            goto err;
        }
    } else {
        /*
         * Check if we can resume the session based on external pre-shared
         * secret. EAP-FAST (RFC 4851) supports two types of session resumption.
         * Resumption based on server-side state works with session IDs.
         * Resumption based on pre-shared Protected Access Credentials (PACs)
         * works by overriding the SessionTicket extension at the application
         * layer, and does not send a session ID. (We do not know whether
         * EAP-FAST servers would honour the session ID.) Therefore, the session
         * ID alone is not a reliable indicator of session resumption, so we
         * first check if we can resume, and later peek at the next handshake
         * message to see if the server wants to resume.
         */
        if (s->version >= TLS1_VERSION
                && s->ext.session_secret_cb != NULL && s->session->ext.tick) {
            const SSL_CIPHER *pref_cipher = NULL;
            /*
             * s->session->master_key_length is a size_t, but this is an int for
             * backwards compat reasons
             */
            int master_key_length;
            master_key_length = sizeof(s->session->master_key);
            if (s->ext.session_secret_cb(s, s->session->master_key,
                                         &master_key_length,
                                         NULL, &pref_cipher,
                                         s->ext.session_secret_cb_arg)
                     && master_key_length > 0) {
                s->session->master_key_length = master_key_length;
                s->session->cipher = pref_cipher ?
                    pref_cipher : ssl_get_cipher_by_char(s, cipherchars, 0);
            } else {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_PROCESS_SERVER_HELLO, ERR_R_INTERNAL_ERROR);
                goto err;
            }
        }

        if (session_id_len != 0
                && session_id_len == s->session->session_id_length
                && memcmp(PACKET_data(&session_id), s->session->session_id,
                          session_id_len) == 0)
            s->hit = 1;
    }

    if (s->hit) {
        if (s->sid_ctx_length != s->session->sid_ctx_length
                || memcmp(s->session->sid_ctx, s->sid_ctx, s->sid_ctx_length)) {
            /* actually a client application bug */
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                     SSL_F_TLS_PROCESS_SERVER_HELLO,
                     SSL_R_ATTEMPT_TO_REUSE_SESSION_IN_DIFFERENT_CONTEXT);
            goto err;
        }
    } else {
        /*
         * If we were trying for session-id reuse but the server
         * didn't resume, make a new SSL_SESSION.
         * In the case of EAP-FAST and PAC, we do not send a session ID,
         * so the PAC-based session secret is always preserved. It'll be
         * overwritten if the server refuses resumption.
         */
        if (s->session->session_id_length > 0
                || (SSL_IS_TLS13(s)
                    && s->session->ext.tick_identity
                       != TLSEXT_PSK_BAD_IDENTITY)) {
            tsan_counter(&s->session_ctx->stats.sess_miss);
            if (!ssl_get_new_session(s, 0)) {
                /* SSLfatal() already called */
                goto err;
            }
        }

        s->session->ssl_version = s->version;
        /*
         * In TLSv1.2 and below we save the session id we were sent so we can
         * resume it later. In TLSv1.3 the session id we were sent is just an
         * echo of what we originally sent in the ClientHello and should not be
         * used for resumption.
         */
        if (!SSL_IS_TLS13(s)) {
            s->session->session_id_length = session_id_len;
            /* session_id_len could be 0 */
            if (session_id_len > 0)
                memcpy(s->session->session_id, PACKET_data(&session_id),
                       session_id_len);
        }
    }

    /* Session version and negotiated protocol version should match */
    if (s->version != s->session->ssl_version) {
        SSLfatal(s, SSL_AD_PROTOCOL_VERSION, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_SSL_SESSION_VERSION_MISMATCH);
        goto err;
    }
    /*
     * Now that we know the version, update the check to see if it's an allowed
     * version.
     */
    s->s3->tmp.min_ver = s->version;
    s->s3->tmp.max_ver = s->version;

    if (!set_client_ciphersuite(s, cipherchars)) {
        /* SSLfatal() already called */
        goto err;
    }

#ifdef OPENSSL_NO_COMP
    if (compression != 0) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_UNSUPPORTED_COMPRESSION_ALGORITHM);
        goto err;
    }
    /*
     * If compression is disabled we'd better not try to resume a session
     * using compression.
     */
    if (s->session->compress_meth != 0) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_INCONSISTENT_COMPRESSION);
        goto err;
    }
#else
    if (s->hit && compression != s->session->compress_meth) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_OLD_SESSION_COMPRESSION_ALGORITHM_NOT_RETURNED);
        goto err;
    }
    if (compression == 0)
        comp = NULL;
    else if (!ssl_allow_compression(s)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_COMPRESSION_DISABLED);
        goto err;
    } else {
        comp = ssl3_comp_find(s->ctx->comp_methods, compression);
    }

    if (compression != 0 && comp == NULL) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SERVER_HELLO,
                 SSL_R_UNSUPPORTED_COMPRESSION_ALGORITHM);
        goto err;
    } else {
        s->s3->tmp.new_compression = comp;
    }
#endif

    if (!tls_parse_all_extensions(s, context, extensions, NULL, 0, 1)) {
        /* SSLfatal() already called */
        goto err;
    }

#ifndef OPENSSL_NO_SCTP
    if (SSL_IS_DTLS(s) && s->hit) {
        unsigned char sctpauthkey[64];
        char labelbuffer[sizeof(DTLS1_SCTP_AUTH_LABEL)];
        size_t labellen;

        /*
         * Add new shared key for SCTP-Auth, will be ignored if
         * no SCTP used.
         */
        memcpy(labelbuffer, DTLS1_SCTP_AUTH_LABEL,
               sizeof(DTLS1_SCTP_AUTH_LABEL));

        /* Don't include the terminating zero. */
        labellen = sizeof(labelbuffer) - 1;
        if (s->mode & SSL_MODE_DTLS_SCTP_LABEL_LENGTH_BUG)
            labellen += 1;

        if (SSL_export_keying_material(s, sctpauthkey,
                                       sizeof(sctpauthkey),
                                       labelbuffer,
                                       labellen, NULL, 0, 0) <= 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SERVER_HELLO,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }

        BIO_ctrl(SSL_get_wbio(s),
                 BIO_CTRL_DGRAM_SCTP_ADD_AUTH_KEY,
                 sizeof(sctpauthkey), sctpauthkey);
    }
#endif

    /*
     * In TLSv1.3 we have some post-processing to change cipher state, otherwise
     * we're done with this message
     */
    if (SSL_IS_TLS13(s)
            && (!s->method->ssl3_enc->setup_key_block(s)
                || !s->method->ssl3_enc->change_cipher_state(s,
                    SSL3_CC_HANDSHAKE | SSL3_CHANGE_CIPHER_CLIENT_READ))) {
        /* SSLfatal() already called */
        goto err;
    }

    OPENSSL_free(extensions);
    return MSG_PROCESS_CONTINUE_READING;
 err:
    OPENSSL_free(extensions);
    return MSG_PROCESS_ERROR;
}

MSG_PROCESS_RETURN GMold_tls_process_server_hello(SSL *s, PACKET *pkt)
{
    STACK_OF(SSL_CIPHER) *sk;
    const SSL_CIPHER *c;
    PACKET session_id;
    size_t session_id_len;
    const unsigned char *cipherchars;
    int i, al = SSL_AD_INTERNAL_ERROR;
    unsigned int compression;
    unsigned int sversion;
    int protverr;
#ifndef OPENSSL_NO_COMP
    SSL_COMP *comp;
#endif

    if (!PACKET_get_net_2(pkt, &sversion)) {
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }

    protverr = GMold_ssl_choose_client_version(s, sversion);
    if (protverr != 0) {
        al = SSL_AD_PROTOCOL_VERSION;
        goto f_err;
    }

    /* load the server hello data */
    /* load the server random */
    if (!PACKET_copy_bytes(pkt, s->s3->server_random, SSL3_RANDOM_SIZE)) {
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }

    s->hit = 0;

    /* Get the session-id. */
    if (!PACKET_get_length_prefixed_1(pkt, &session_id)) {
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }
    session_id_len = PACKET_remaining(&session_id);
    if (session_id_len > sizeof s->session->session_id
        || session_id_len > SSL3_SESSION_ID_SIZE) {
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }

    if (!PACKET_get_bytes(pkt, &cipherchars, TLS_CIPHER_LEN)) {
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }

    /*
     * Check if we can resume the session based on external pre-shared secret.
     * EAP-FAST (RFC 4851) supports two types of session resumption.
     * Resumption based on server-side state works with session IDs.
     * Resumption based on pre-shared Protected Access Credentials (PACs)
     * works by overriding the SessionTicket extension at the application
     * layer, and does not send a session ID. (We do not know whether EAP-FAST
     * servers would honour the session ID.) Therefore, the session ID alone
     * is not a reliable indicator of session resumption, so we first check if
     * we can resume, and later peek at the next handshake message to see if the
     * server wants to resume.
     */
    // if (s->version >= TLS1_VERSION && s->tls_session_secret_cb &&
    //     s->session->tlsext_tick) {
    //     const SSL_CIPHER *pref_cipher = NULL;
    //     s->session->master_key_length = sizeof(s->session->master_key);
    //     if (s->tls_session_secret_cb(s, s->session->master_key,
    //                                  &s->session->master_key_length,
    //                                  NULL, &pref_cipher,
    //                                  s->tls_session_secret_cb_arg)) {
    //         s->session->cipher = pref_cipher ?
    //             pref_cipher : ssl_get_cipher_by_char(s, cipherchars);
    //     } else {
    //         al = SSL_AD_INTERNAL_ERROR;
    //         goto f_err;
    //     }
    // }

    if (session_id_len != 0 && session_id_len == s->session->session_id_length
        && memcmp(PACKET_data(&session_id), s->session->session_id,
                  session_id_len) == 0) {
        if (s->sid_ctx_length != s->session->sid_ctx_length
            || memcmp(s->session->sid_ctx, s->sid_ctx, s->sid_ctx_length)) {
            /* actually a client application bug */
            al = SSL_AD_ILLEGAL_PARAMETER;
            goto f_err;
        }
        s->hit = 1;
    } else {
        /*
         * If we were trying for session-id reuse but the server
         * didn't echo the ID, make a new SSL_SESSION.
         * In the case of EAP-FAST and PAC, we do not send a session ID,
         * so the PAC-based session secret is always preserved. It'll be
         * overwritten if the server refuses resumption.
         */
        if (s->session->session_id_length > 0) {
            s->ctx->stats.sess_miss++;
            if (!ssl_get_new_session(s, 0)) {
                goto f_err;
            }
        }

        s->session->ssl_version = s->version;
        s->session->session_id_length = session_id_len;
        /* session_id_len could be 0 */
        if (session_id_len > 0)
            memcpy(s->session->session_id, PACKET_data(&session_id),
                   session_id_len);
    }

    /* Session version and negotiated protocol version should match */
    if (s->version != s->session->ssl_version) {
        al = SSL_AD_PROTOCOL_VERSION;
        goto f_err;
    }

    c = GMold_ssl_get_cipher_by_char(s, cipherchars);
    if (c == NULL) {
        /* unknown cipher */
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }
    /*
     * Now that we know the version, update the check to see if it's an allowed
     * version.
     */
    s->s3->tmp.min_ver = s->version;
    s->s3->tmp.max_ver = s->version;
    /*
     * If it is a disabled cipher we either didn't send it in client hello,
     * or it's not allowed for the selected protocol. So we return an error.
     */
    if (GMold_ssl_cipher_disabled(s, c, SSL_SECOP_CIPHER_CHECK,1)) {
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }

    sk = ssl_get_ciphers_by_id(s);
    i = sk_SSL_CIPHER_find(sk, c);
    if (i < 0) {
        /* we did not say we would use this cipher */
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }

    /*
     * Depending on the session caching (internal/external), the cipher
     * and/or cipher_id values may not be set. Make sure that cipher_id is
     * set and use it for comparison.
     */
    if (s->session->cipher)
        s->session->cipher_id = s->session->cipher->id;
    if (s->hit && (s->session->cipher_id != c->id)) {
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }
    s->s3->tmp.new_cipher = c;
    /* lets get the compression algorithm */
    /* COMPRESSION */
    if (!PACKET_get_1(pkt, &compression)) {
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }

    if (s->hit && compression != s->session->compress_meth) {
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }
    if (compression == 0)
        comp = NULL;
    else if (!ssl_allow_compression(s)) {
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    } else {
        comp = ssl3_comp_find(s->ctx->comp_methods, compression);
    }

    if (compression != 0 && comp == NULL) {
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    } else {
        s->s3->tmp.new_compression = comp;
    }

    /* TLS extensions */
    // if (!ssl_parse_serverhello_tlsext(s, pkt)) {
    //     goto err;
    // }

    if (PACKET_remaining(pkt) != 0) {
        /* wrong packet length */
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }

    return MSG_PROCESS_CONTINUE_READING;
 f_err:
    ssl3_send_alert(s, SSL3_AL_FATAL, al);
 err:
    s->statem.state = MSG_FLOW_ERROR;
    return MSG_PROCESS_ERROR;
}

static MSG_PROCESS_RETURN tls_process_as_hello_retry_request(SSL *s,
                                                             PACKET *extpkt)
{
    RAW_EXTENSION *extensions = NULL;

    /*
     * If we were sending early_data then the enc_write_ctx is now invalid and
     * should not be used.
     */
    EVP_CIPHER_CTX_free(s->enc_write_ctx);
    s->enc_write_ctx = NULL;

    if (!tls_collect_extensions(s, extpkt, SSL_EXT_TLS1_3_HELLO_RETRY_REQUEST,
                                &extensions, NULL, 1)
            || !tls_parse_all_extensions(s, SSL_EXT_TLS1_3_HELLO_RETRY_REQUEST,
                                         extensions, NULL, 0, 1)) {
        /* SSLfatal() already called */
        goto err;
    }

    OPENSSL_free(extensions);
    extensions = NULL;

    if (s->ext.tls13_cookie_len == 0
#if !defined(OPENSSL_NO_EC) || !defined(OPENSSL_NO_DH)
        && s->s3->tmp.pkey != NULL
#endif
        ) {
        /*
         * We didn't receive a cookie or a new key_share so the next
         * ClientHello will not change
         */
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                 SSL_F_TLS_PROCESS_AS_HELLO_RETRY_REQUEST,
                 SSL_R_NO_CHANGE_FOLLOWING_HRR);
        goto err;
    }

    /*
     * Re-initialise the Transcript Hash. We're going to prepopulate it with
     * a synthetic message_hash in place of ClientHello1.
     */
    if (!create_synthetic_message_hash(s, NULL, 0, NULL, 0)) {
        /* SSLfatal() already called */
        goto err;
    }

    /*
     * Add this message to the Transcript Hash. Normally this is done
     * automatically prior to the message processing stage. However due to the
     * need to create the synthetic message hash, we defer that step until now
     * for HRR messages.
     */
    if (!ssl3_finish_mac(s, (unsigned char *)s->init_buf->data,
                                s->init_num + SSL3_HM_HEADER_LENGTH)) {
        /* SSLfatal() already called */
        goto err;
    }

    return MSG_PROCESS_FINISHED_READING;
 err:
    OPENSSL_free(extensions);
    return MSG_PROCESS_ERROR;
}

MSG_PROCESS_RETURN tls_process_server_certificate(SSL *s, PACKET *pkt)
{
    printf("ddddddddddd tls_process_server_certificate \n");
    int i;
    MSG_PROCESS_RETURN ret = MSG_PROCESS_ERROR;
    unsigned long cert_list_len, cert_len;
    X509 *x = NULL;
    const unsigned char *certstart, *certbytes;
    STACK_OF(X509) *sk = NULL;
    EVP_PKEY *pkey = NULL;
    size_t chainidx, certidx;
    unsigned int context = 0;
    const SSL_CERT_LOOKUP *clu;

    if ((sk = sk_X509_new_null()) == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    if ((SSL_IS_TLS13(s) && !PACKET_get_1(pkt, &context))
            || context != 0
            || !PACKET_get_net_3(pkt, &cert_list_len)
            || PACKET_remaining(pkt) != cert_list_len
            || PACKET_remaining(pkt) == 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }
    for (chainidx = 0; PACKET_remaining(pkt); chainidx++) {
        if (!PACKET_get_net_3(pkt, &cert_len)
            || !PACKET_get_bytes(pkt, &certbytes, cert_len)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                     SSL_R_CERT_LENGTH_MISMATCH);
            goto err;
        }

        certstart = certbytes;
        x = d2i_X509(NULL, (const unsigned char **)&certbytes, cert_len);
        if (x == NULL) {
            SSLfatal(s, SSL_AD_BAD_CERTIFICATE,
                     SSL_F_TLS_PROCESS_SERVER_CERTIFICATE, ERR_R_ASN1_LIB);
            goto err;
        }
        if (certbytes != (certstart + cert_len)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                     SSL_R_CERT_LENGTH_MISMATCH);
            goto err;
        }

        if (SSL_IS_TLS13(s)) {
            RAW_EXTENSION *rawexts = NULL;
            PACKET extensions;

            if (!PACKET_get_length_prefixed_2(pkt, &extensions)) {
                SSLfatal(s, SSL_AD_DECODE_ERROR,
                         SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                         SSL_R_BAD_LENGTH);
                goto err;
            }
            if (!tls_collect_extensions(s, &extensions,
                                        SSL_EXT_TLS1_3_CERTIFICATE, &rawexts,
                                        NULL, chainidx == 0)
                || !tls_parse_all_extensions(s, SSL_EXT_TLS1_3_CERTIFICATE,
                                             rawexts, x, chainidx,
                                             PACKET_remaining(pkt) == 0)) {
                OPENSSL_free(rawexts);
                /* SSLfatal already called */
                goto err;
            }
            OPENSSL_free(rawexts);
        }

        if (!sk_X509_push(sk, x)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                     ERR_R_MALLOC_FAILURE);
            goto err;
        }
        x = NULL;
    }

    i = ssl_verify_cert_chain(s, sk);
    /*
     * The documented interface is that SSL_VERIFY_PEER should be set in order
     * for client side verification of the server certificate to take place.
     * However, historically the code has only checked that *any* flag is set
     * to cause server verification to take place. Use of the other flags makes
     * no sense in client mode. An attempt to clean up the semantics was
     * reverted because at least one application *only* set
     * SSL_VERIFY_FAIL_IF_NO_PEER_CERT. Prior to the clean up this still caused
     * server verification to take place, after the clean up it silently did
     * nothing. SSL_CTX_set_verify()/SSL_set_verify() cannot validate the flags
     * sent to them because they are void functions. Therefore, we now use the
     * (less clean) historic behaviour of performing validation if any flag is
     * set. The *documented* interface remains the same.
     */
    if (s->verify_mode != SSL_VERIFY_NONE && i <= 0) {
        SSLfatal(s, ssl_x509err2alert(s->verify_result),
                 SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                 SSL_R_CERTIFICATE_VERIFY_FAILED);
        goto err;
    }
    ERR_clear_error();          /* but we keep s->verify_result */
    if (i > 1) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_TLS_PROCESS_SERVER_CERTIFICATE, i);
        goto err;
    }

    s->session->peer_chain = sk;
    /*
     * Inconsistency alert: cert_chain does include the peer's certificate,
     * which we don't include in statem_srvr.c
     */
    x = sk_X509_value(sk, 0);
    sk = NULL;

    pkey = X509_get0_pubkey(x);

    if (pkey == NULL || EVP_PKEY_missing_parameters(pkey)) {
        x = NULL;
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                 SSL_R_UNABLE_TO_FIND_PUBLIC_KEY_PARAMETERS);
        goto err;
    }

    if ((clu = ssl_cert_lookup_by_pkey(pkey, &certidx)) == NULL) {
        x = NULL;
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                 SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                 SSL_R_UNKNOWN_CERTIFICATE_TYPE);
        goto err;
    }
    /*
     * Check certificate type is consistent with ciphersuite. For TLS 1.3
     * skip check since TLS 1.3 ciphersuites can be used with any certificate
     * type.
     */
    if (!SSL_IS_TLS13(s)) {
        if ((clu->amask & s->s3->tmp.new_cipher->algorithm_auth) == 0) {
            x = NULL;
            SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER,
                     SSL_F_TLS_PROCESS_SERVER_CERTIFICATE,
                     SSL_R_WRONG_CERTIFICATE_TYPE);
            goto err;
        }
    }
    s->session->peer_type = certidx;

    X509_free(s->session->peer);
    X509_up_ref(x);
    s->session->peer = x;
    s->session->verify_result = s->verify_result;
    x = NULL;

    /* Save the current hash state for when we receive the CertificateVerify */
    if (SSL_IS_TLS13(s)
            && !ssl_handshake_hash(s, s->cert_verify_hash,
                                   sizeof(s->cert_verify_hash),
                                   &s->cert_verify_hash_len)) {
        /* SSLfatal() already called */;
        goto err;
    }

    ret = MSG_PROCESS_CONTINUE_READING;

 err:
    X509_free(x);
    sk_X509_pop_free(sk, X509_free);
    return ret;
}
int GMold_ssl_cert_type(const X509 *x, const EVP_PKEY *pk);
MSG_PROCESS_RETURN GMold_tls_process_server_certificate(SSL *s, PACKET *pkt)
{
    
    int al, i, ret = MSG_PROCESS_ERROR, exp_idx;
    unsigned long cert_list_len, cert_len;
    X509 *x = NULL;
    const unsigned char *certstart, *certbytes;
    STACK_OF(X509) *sk = NULL;
    EVP_PKEY *pkey = NULL;

    if ((sk = sk_X509_new_null()) == NULL) {
        goto err;
    }
    if (!PACKET_get_net_3(pkt, &cert_list_len)      //获取证书链表长度
        || PACKET_remaining(pkt) != cert_list_len) {    //判断长度是否正常
        al = SSL_AD_DECODE_ERROR;
        goto f_err;
    }
    while (PACKET_remaining(pkt)) {                 //读取证书链
        if (!PACKET_get_net_3(pkt, &cert_len)       //读取证书长度
            || !PACKET_get_bytes(pkt, &certbytes, cert_len)) {  //读取证书
            al = SSL_AD_DECODE_ERROR;
            goto f_err;
        }

        certstart = certbytes;
        x = GMold_d2i_X509(NULL, (const unsigned char **)&certbytes, cert_len);
        if (x == NULL) {
            al = SSL_AD_BAD_CERTIFICATE;
            goto f_err;
        }
        if (certbytes != (certstart + cert_len)) {
            al = SSL_AD_DECODE_ERROR;
            goto f_err;
        }
        if (!sk_X509_push(sk, x)) {
            goto err;
        }
        x = NULL;
    }

    i = GMold_ssl_verify_cert_chain(s, sk);
    /*
     * The documented interface is that SSL_VERIFY_PEER should be set in order
     * for client side verification of the server certificate to take place.
     * However, historically the code has only checked that *any* flag is set
     * to cause server verification to take place. Use of the other flags makes
     * no sense in client mode. An attempt to clean up the semantics was
     * reverted because at least one application *only* set
     * SSL_VERIFY_FAIL_IF_NO_PEER_CERT. Prior to the clean up this still caused
     * server verification to take place, after the clean up it silently did
     * nothing. SSL_CTX_set_verify()/SSL_set_verify() cannot validate the flags
     * sent to them because they are void functions. Therefore, we now use the
     * (less clean) historic behaviour of performing validation if any flag is
     * set. The *documented* interface remains the same.
     */
    if (s->verify_mode != SSL_VERIFY_NONE && i <= 0) {
        al = GMold_ssl_verify_alarm_type(s->verify_result);
        goto f_err;
    }
    ERR_clear_error();          /* but we keep s->verify_result */
    if (i > 1) {
        al = SSL_AD_HANDSHAKE_FAILURE;
        goto f_err;
    }

    s->session->peer_chain = sk;
    /*
     * Inconsistency alert: cert_chain does include the peer's certificate,
     * which we don't include in statem_srvr.c
     */
    x = sk_X509_value(sk, 0);
    sk = NULL;
    /*
     * VRS 19990621: possible memory leak; sk=null ==> !sk_pop_free() @end
     */

    pkey = GMold_X509_get0_pubkey(x);

    if (pkey == NULL || EVP_PKEY_missing_parameters(pkey)) {
        x = NULL;
        al = SSL3_AL_FATAL;
        goto f_err;
    }

    i = GMold_ssl_cert_type(x, pkey); //TODO xcs
    if (i < 0) {
        x = NULL;
        al = SSL3_AL_FATAL;
        goto f_err;
    }
    exp_idx = GMold_ssl_cipher_get_cert_index(s->s3->tmp.new_cipher);
    if (exp_idx >= 0 && i != exp_idx
        && (exp_idx != GMold_SSL_PKEY_GOST_EC ||
            (i != SSL_PKEY_GOST12_512 && i != SSL_PKEY_GOST12_256
             && i != SSL_PKEY_GOST01))) {
        x = NULL;
        al = SSL_AD_ILLEGAL_PARAMETER;
        goto f_err;
    }
    s->session->peer_type = i;

    X509_free(s->session->peer);
    X509_up_ref(x);
    s->session->peer = x;
    s->session->verify_result = s->verify_result;

    x = NULL;
    ret = MSG_PROCESS_CONTINUE_READING;
    goto done;

 f_err:
    ssl3_send_alert(s, SSL3_AL_FATAL, al);
 err:
    s->statem.state = MSG_FLOW_ERROR;
 done:
    X509_free(x);
    sk_X509_pop_free(sk, X509_free);
    return ret;
}

static int tls_process_ske_psk_preamble(SSL *s, PACKET *pkt)
{
#ifndef OPENSSL_NO_PSK
    PACKET psk_identity_hint;

    /* PSK ciphersuites are preceded by an identity hint */

    if (!PACKET_get_length_prefixed_2(pkt, &psk_identity_hint)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SKE_PSK_PREAMBLE,
                 SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    /*
     * Store PSK identity hint for later use, hint is used in
     * tls_construct_client_key_exchange.  Assume that the maximum length of
     * a PSK identity hint can be as long as the maximum length of a PSK
     * identity.
     */
    if (PACKET_remaining(&psk_identity_hint) > PSK_MAX_IDENTITY_LEN) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_TLS_PROCESS_SKE_PSK_PREAMBLE,
                 SSL_R_DATA_LENGTH_TOO_LONG);
        return 0;
    }

    if (PACKET_remaining(&psk_identity_hint) == 0) {
        OPENSSL_free(s->session->psk_identity_hint);
        s->session->psk_identity_hint = NULL;
    } else if (!PACKET_strndup(&psk_identity_hint,
                               &s->session->psk_identity_hint)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_PSK_PREAMBLE,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    return 1;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_PSK_PREAMBLE,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_process_ske_srp(SSL *s, PACKET *pkt, EVP_PKEY **pkey)
{
#ifndef OPENSSL_NO_SRP
    PACKET prime, generator, salt, server_pub;

    if (!PACKET_get_length_prefixed_2(pkt, &prime)
        || !PACKET_get_length_prefixed_2(pkt, &generator)
        || !PACKET_get_length_prefixed_1(pkt, &salt)
        || !PACKET_get_length_prefixed_2(pkt, &server_pub)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SKE_SRP,
                 SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    /* TODO(size_t): Convert BN_bin2bn() calls */
    if ((s->srp_ctx.N =
         BN_bin2bn(PACKET_data(&prime),
                   (int)PACKET_remaining(&prime), NULL)) == NULL
        || (s->srp_ctx.g =
            BN_bin2bn(PACKET_data(&generator),
                      (int)PACKET_remaining(&generator), NULL)) == NULL
        || (s->srp_ctx.s =
            BN_bin2bn(PACKET_data(&salt),
                      (int)PACKET_remaining(&salt), NULL)) == NULL
        || (s->srp_ctx.B =
            BN_bin2bn(PACKET_data(&server_pub),
                      (int)PACKET_remaining(&server_pub), NULL)) == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_SRP,
                 ERR_R_BN_LIB);
        return 0;
    }

    if (!srp_verify_server_param(s)) {
        /* SSLfatal() already called */
        return 0;
    }

    /* We must check if there is a certificate */
    if (s->s3->tmp.new_cipher->algorithm_auth & (SSL_aRSA | SSL_aDSS))
        *pkey = X509_get0_pubkey(s->session->peer);

    return 1;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_SRP,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_process_ske_dhe(SSL *s, PACKET *pkt, EVP_PKEY **pkey)
{
#ifndef OPENSSL_NO_DH
    PACKET prime, generator, pub_key;
    EVP_PKEY *peer_tmp = NULL;

    DH *dh = NULL;
    BIGNUM *p = NULL, *g = NULL, *bnpub_key = NULL;

    int check_bits = 0;

    if (!PACKET_get_length_prefixed_2(pkt, &prime)
        || !PACKET_get_length_prefixed_2(pkt, &generator)
        || !PACKET_get_length_prefixed_2(pkt, &pub_key)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
                 SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    peer_tmp = EVP_PKEY_new();
    dh = DH_new();

    if (peer_tmp == NULL || dh == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    /* TODO(size_t): Convert these calls */
    p = BN_bin2bn(PACKET_data(&prime), (int)PACKET_remaining(&prime), NULL);
    g = BN_bin2bn(PACKET_data(&generator), (int)PACKET_remaining(&generator),
                  NULL);
    bnpub_key = BN_bin2bn(PACKET_data(&pub_key),
                          (int)PACKET_remaining(&pub_key), NULL);
    if (p == NULL || g == NULL || bnpub_key == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
                 ERR_R_BN_LIB);
        goto err;
    }

    /* test non-zero pubkey */
    if (BN_is_zero(bnpub_key)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SKE_DHE,
                 SSL_R_BAD_DH_VALUE);
        goto err;
    }

    if (!DH_set0_pqg(dh, p, NULL, g)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
                 ERR_R_BN_LIB);
        goto err;
    }
    p = g = NULL;

    if (DH_check_params(dh, &check_bits) == 0 || check_bits != 0) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SKE_DHE,
                 SSL_R_BAD_DH_VALUE);
        goto err;
    }

    if (!DH_set0_key(dh, bnpub_key, NULL)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
                 ERR_R_BN_LIB);
        goto err;
    }
    bnpub_key = NULL;

    if (!ssl_security(s, SSL_SECOP_TMP_DH, DH_security_bits(dh), 0, dh)) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE, SSL_F_TLS_PROCESS_SKE_DHE,
                 SSL_R_DH_KEY_TOO_SMALL);
        goto err;
    }

    if (EVP_PKEY_assign_DH(peer_tmp, dh) == 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
                 ERR_R_EVP_LIB);
        goto err;
    }

    s->s3->peer_tmp = peer_tmp;

    /*
     * FIXME: This makes assumptions about which ciphersuites come with
     * public keys. We should have a less ad-hoc way of doing this
     */
    if (s->s3->tmp.new_cipher->algorithm_auth & (SSL_aRSA | SSL_aDSS))
        *pkey = X509_get0_pubkey(s->session->peer);
    /* else anonymous DH, so no certificate or pkey. */

    return 1;

 err:
    BN_free(p);
    BN_free(g);
    BN_free(bnpub_key);
    DH_free(dh);
    EVP_PKEY_free(peer_tmp);

    return 0;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_DHE,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_process_ske_ecdhe(SSL *s, PACKET *pkt, EVP_PKEY **pkey)
{
#ifndef OPENSSL_NO_EC
    PACKET encoded_pt;
    unsigned int curve_type, curve_id;

    /*
     * Extract elliptic curve parameters and the server's ephemeral ECDH
     * public key. We only support named (not generic) curves and
     * ECParameters in this case is just three bytes.
     */
    if (!PACKET_get_1(pkt, &curve_type) || !PACKET_get_net_2(pkt, &curve_id)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SKE_ECDHE,
                 SSL_R_LENGTH_TOO_SHORT);
        return 0;
    }
    //At present, because there is no definite explanation, when the protocol is CNTLS, the default 249 will be used as sm2 curve ID
    if( s->version == SM1_1_VERSION && curve_id != 249)
	    curve_id = 249;    //if none curve id ,set it to sm2 249 defined by tass
    /*
     * Check curve is named curve type and one of our preferences, if not
     * server has sent an invalid curve.
     */
    if (curve_type != NAMED_CURVE_TYPE
            || !tls1_check_group_id(s, curve_id, 1)) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SKE_ECDHE,
                 SSL_R_WRONG_CURVE);
        return 0;
    }

    if ((s->s3->peer_tmp = ssl_generate_param_group(curve_id)) == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_ECDHE,
                 SSL_R_UNABLE_TO_FIND_ECDH_PARAMETERS);
        return 0;
    }

    if (!PACKET_get_length_prefixed_1(pkt, &encoded_pt)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SKE_ECDHE,
                 SSL_R_LENGTH_MISMATCH);
        return 0;
    }

    if (!EVP_PKEY_set1_tls_encodedpoint(s->s3->peer_tmp,
                                        PACKET_data(&encoded_pt),
                                        PACKET_remaining(&encoded_pt))) {
        SSLfatal(s, SSL_AD_ILLEGAL_PARAMETER, SSL_F_TLS_PROCESS_SKE_ECDHE,
                 SSL_R_BAD_ECPOINT);
        return 0;
    }

    /*
     * The ECC/TLS specification does not mention the use of DSA to sign
     * ECParameters in the server key exchange message. We do support RSA
     * and ECDSA.
     */
    if (s->s3->tmp.new_cipher->algorithm_auth & SSL_aECDSA)
        *pkey = X509_get0_pubkey(s->session->peer);
    else if (s->s3->tmp.new_cipher->algorithm_auth & SSL_aRSA)
        *pkey = X509_get0_pubkey(s->session->peer);
    /* else anonymous ECDH, so no certificate or pkey. */

    return 1;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SKE_ECDHE,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

MSG_PROCESS_RETURN GMold_process_server_key_exchange(SSL *s, PACKET *pkt)
{
	int al = -1;
	unsigned long alg_k = s->s3->tmp.new_cipher->algorithm_mkey;

    if (!GMold_process_ske_sm2(s, pkt, &al)) {      
		goto err;
	}

	return MSG_PROCESS_CONTINUE_READING;

err:
	if (al != -1)
		ssl3_send_alert(s, SSL3_AL_FATAL, al);
	s->statem.state = MSG_FLOW_ERROR;
	return MSG_PROCESS_ERROR;
}

int GMold_process_ske_sm2(SSL *s, PACKET *pkt, int *al)
{
	int ret = 0;
	EVP_PKEY *pkey;
	X509 *x509;
	unsigned char *buf = NULL;
	int n;
	PACKET signature;
	int maxsig;
	EVP_MD_CTX *md_ctx = NULL;
	char *id = NULL;
	unsigned char z[EVP_MAX_MD_SIZE];
	size_t zlen;

	*al = SSL_AD_INTERNAL_ERROR;

	/* get peer's signing pkey 通过服务端的证书获取公钥 下标为0的*/
        if (!(pkey = GMold_X509_get0_pubkey(s->session->peer))) {
		return 0;
	}

	/* get peer's encryption cert 通过服务端的证书链获取加密证书，为下标为1的*/
	if (!(x509 = sk_X509_value(s->session->peer_chain, 1))) {
		return 0;
	}
	if (!(buf = GMold_new_cert_packet(x509, &n))) {
		return 0;
	}

	/* get signature packet, check no data remaining  获取签名包，检查无剩余数据*/
	if (!PACKET_get_length_prefixed_2(pkt, &signature)
		|| PACKET_remaining(pkt) != 0) {
		goto end;
	}
	maxsig = EVP_PKEY_size(pkey);
	if (maxsig < 0) {
		goto end;
	}
	if (PACKET_remaining(&signature) > (size_t)maxsig) {
		goto end;
	}

	/* verify the signature 核实签名*/
	if (!(md_ctx = EVP_MD_CTX_new())) {
		goto end;
	}
	if (GMold_EVP_DigestInit_ex(md_ctx, GMold_EVP_sm3(), NULL) <= 0) {
		goto end;
	}

	/* prepare sm2 z value 准备sm2 z值*/
        if (!(id = GMold_X509_NAME_oneline(X509_get_subject_name(x509), NULL, 0))) {
		goto end;
	}
	zlen = sizeof(z);
	if (!GMold_SM2_compute_id_digest(GMold_EVP_sm3(), id, strlen(id), z, &zlen,
		EVP_PKEY_get0_EC_KEY(pkey))) {
		goto end;
	}
#ifdef CIPHER_DEBUG
	{
        int i;
        printf("Z=");
        for (i = 0; i< zlen; i++)
            printf("%02X",z[i]);
        printf("\n");

        printf("C=");
        for (i = 0; i < n; i++)
            printf("%02X",buf[i]);
        printf("\n");
    }
#endif
	if (GMold_EVP_VerifyUpdate(md_ctx, z, zlen) <= 0
		|| GMold_EVP_VerifyUpdate(md_ctx, &(s->s3->client_random[0]),
			SSL3_RANDOM_SIZE) <= 0
		|| GMold_EVP_VerifyUpdate(md_ctx, &(s->s3->server_random[0]),
			SSL3_RANDOM_SIZE) <= 0
		|| GMold_EVP_VerifyUpdate(md_ctx, buf, n) <= 0) {
		goto end;
	}

	if (GMold_EVP_VerifyFinal(md_ctx, PACKET_data(&signature),
		PACKET_remaining(&signature), pkey) <= 0) {
		*al = SSL_AD_DECRYPT_ERROR;
		ERR_print_errors_fp(stderr);
		goto end;
	}

	*al = -1;
	ret = 1;

end:
	OPENSSL_free(buf);
	EVP_MD_CTX_free(md_ctx);
	OPENSSL_free(id);
	return ret;
}

unsigned char *GMold_new_cert_packet(X509 *x, int *l)
{
	unsigned char *ret = NULL;
	unsigned char *p;
	int n;

        if ((n = GMold_i2d_X509(x, NULL)) <= 0) {
		return NULL;
	}
	if (!(ret = OPENSSL_malloc(n + 3))) {
		return 0;
	}

	p = &(ret[3]);
        if ((n = GMold_i2d_X509(x, &p)) <= 0) {
		goto end;
	}

	p = ret;
	l2n3(n, p);
	*l = n;

end:
	return ret;
}

MSG_PROCESS_RETURN tls_process_key_exchange(SSL *s, PACKET *pkt)
{
#ifndef OPENSSL_NO_CNSM
    int i = 0;
#endif
    long alg_k;
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    PACKET save_param_start, signature;
    #ifndef OPENSSL_NO_CNSM
    BUF_MEM *sm2_certs = NULL;
    long alg_a = 0;
    size_t sm2_certs_len = 0;
    EVP_PKEY_CTX *engine_pctx = NULL;
    #endif
 
    alg_k = s->s3->tmp.new_cipher->algorithm_mkey;
    #ifndef OPENSSL_NO_CNSM
    alg_a = s->s3->tmp.new_cipher->algorithm_auth;
    #endif

    save_param_start = *pkt;

#if !defined(OPENSSL_NO_EC) || !defined(OPENSSL_NO_DH)
    EVP_PKEY_free(s->s3->peer_tmp);
    s->s3->peer_tmp = NULL;
#endif

    if (alg_k & SSL_PSK) {
        if (!tls_process_ske_psk_preamble(s, pkt)) {
            /* SSLfatal() already called */
            goto err;
        }
    }

    /* Nothing else to do for plain PSK or RSAPSK */
    if (alg_k & (SSL_kPSK | SSL_kRSAPSK)) {
    } else if (alg_k & SSL_kSRP) {
        if (!tls_process_ske_srp(s, pkt, &pkey)) {
            /* SSLfatal() already called */
            goto err;
        }
    } else if (alg_k & (SSL_kDHE | SSL_kDHEPSK)) {
        if (!tls_process_ske_dhe(s, pkt, &pkey)) {
            /* SSLfatal() already called */
            goto err;
        }
    #ifndef OPENSSL_NO_CNSM
        } else if (alg_k & (SSL_kECDHE | SSL_kECDHEPSK |SSL_kSM2DH)) {  //add SSL_kSM2DH for ECDHE-SM4-SM3 ciphersuit by tass gujq on 20181126
    #else
        } else if (alg_k & (SSL_kECDHE | SSL_kECDHEPSK)) {
    #endif
        if (!tls_process_ske_ecdhe(s, pkt, &pkey)) {
            /* SSLfatal() already called */
            goto err;
        }
#ifndef OPENSSL_NO_CNSM	
    } else if (alg_k & SSL_kECC) {
    	alg_a = s->s3->tmp.new_cipher->algorithm_auth;
        if (alg_a & SSL_aSM2DSA){
            pkey = X509_get_pubkey((sk_X509_value(s->session->peer_chain,0)));
            engine_pctx = EVP_PKEY_CTX_new_pkey_id(pkey, NID_sm2, NULL);
            EVP_PKEY_verify_init(engine_pctx);
          }
	     
        else {
            SSLerr(SSL_F_TLS_PROCESS_KEY_EXCHANGE, ERR_R_INTERNAL_ERROR);
            goto err;
        }
        sm2_certs = BUF_MEM_new();
        if (!sm2_certs)
            goto err;
        sm2_certs_len = 0;
        
        /*从链表最后开始，查找第一个数据加密功能的证书,作为加密证书使用，跟排列顺序无关*/
        for(i=sk_X509_num(s->session->peer_chain)-1; i>=0; i--){
            if((X509_get_extension_flags(sk_X509_value(s->session->peer_chain, i)) & EXFLAG_KUSAGE) && (X509_get_key_usage(sk_X509_value(s->session->peer_chain, i)) & X509v3_KU_DATA_ENCIPHERMENT))
                break;
        }
        
        if (!ssl_add_cert_to_buf(sm2_certs, &sm2_certs_len, sk_X509_value(s->session->peer_chain, i)))
            goto err;
#endif
    } else if (alg_k) {
        SSLfatal(s, SSL_AD_UNEXPECTED_MESSAGE, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                 SSL_R_UNEXPECTED_MESSAGE);
        goto err;
    }

    /* if it was signed, check the signature */
    if (pkey != NULL) {
        PACKET params;
        int maxsig;
        const EVP_MD *md = NULL;
        unsigned char *tbs;
        size_t tbslen;
        int rv;

        /*
         * |pkt| now points to the beginning of the signature, so the difference
         * equals the length of the parameters.
         */
        if (!PACKET_get_sub_packet(&save_param_start, &params,
                                   PACKET_remaining(&save_param_start) -
                                   PACKET_remaining(pkt))) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }

        if (SSL_USE_SIGALGS(s)) {
            unsigned int sigalg;

            if (!PACKET_get_net_2(pkt, &sigalg)) {
                SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                         SSL_R_LENGTH_TOO_SHORT);
                goto err;
            }
            if (tls12_check_peer_sigalg(s, sigalg, pkey) <=0) {
                /* SSLfatal() already called */
                goto err;
            }
        } else if (!tls1_set_peer_legacy_sigalg(s, pkey)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }

        if (!tls1_lookup_md(s->s3->tmp.peer_sigalg, &md)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }
#ifdef SSL_DEBUG
        if (SSL_USE_SIGALGS(s))
            fprintf(stderr, "USING TLSv1.2 HASH %s\n",
                    md == NULL ? "n/a" : EVP_MD_name(md));
#endif

        if (!PACKET_get_length_prefixed_2(pkt, &signature)
            || PACKET_remaining(pkt) != 0) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     SSL_R_LENGTH_MISMATCH);
            goto err;
        }
        maxsig = EVP_PKEY_size(pkey);
        if (maxsig < 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }

        /*
         * Check signature length
         */
        if (PACKET_remaining(&signature) > (size_t)maxsig) {
            /* wrong packet length */
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                   SSL_R_WRONG_SIGNATURE_LENGTH);
            goto err;
        }

        md_ctx = EVP_MD_CTX_new();
        if (md_ctx == NULL) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     ERR_R_MALLOC_FAILURE);
            goto err;
        }

        if (EVP_DigestVerifyInit(md_ctx, &pctx, md, NULL, pkey) <= 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     ERR_R_EVP_LIB);
            goto err;
        }
        if (SSL_USE_PSS(s)) {
            if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) <= 0
                || EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx,
                                                RSA_PSS_SALTLEN_DIGEST) <= 0) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_PROCESS_KEY_EXCHANGE, ERR_R_EVP_LIB);
                goto err;
            }
        }
        
        #ifndef OPENSSL_NO_CNSM
		    if(s->s3->tmp.new_cipher->id == TLS1_CK_ECC_WITH_SM4_SM3){
        	    tbslen = construct_key_exchange_tbs(s, &tbs, sm2_certs ? sm2_certs->data : NULL,
                                            sm2_certs_len);
            }
            else{
        	    tbslen = construct_key_exchange_tbs(s, &tbs, PACKET_data(&params),
                                            PACKET_remaining(&params));
            }
        #else
        tbslen = construct_key_exchange_tbs(s, &tbs, PACKET_data(&params),
                                            PACKET_remaining(&params));
        #endif
        
        if (tbslen == 0) {
            /* SSLfatal() already called */
            goto err;
        }
#ifndef OPENSSL_NO_CNSM
        if(engine_pctx){
            EVP_MD_CTX_set_pkey_ctx(md_ctx, engine_pctx);
        }
#endif

        rv = EVP_DigestVerify(md_ctx, PACKET_data(&signature),
                              PACKET_remaining(&signature), tbs, tbslen);
        OPENSSL_free(tbs);
        if (rv <= 0) {
            SSLfatal(s, SSL_AD_DECRYPT_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     SSL_R_BAD_SIGNATURE);
            goto err;
        }
#ifndef OPENSSL_NO_CNSM
        if(alg_k & SSL_kECC){
            if(sm2_certs)
                BUF_MEM_free(sm2_certs);
            if(pkey)
                EVP_PKEY_free(pkey);
            if(engine_pctx)
                EVP_PKEY_CTX_free(engine_pctx);
    	}
#endif
        EVP_MD_CTX_free(md_ctx);
        md_ctx = NULL;
    } else {
        /* aNULL, aSRP or PSK do not need public keys */
        if (!(s->s3->tmp.new_cipher->algorithm_auth & (SSL_aNULL | SSL_aSRP))
            && !(alg_k & SSL_PSK)) {
            /* Might be wrong key type, check it */
            if (ssl3_check_cert_and_algorithm(s)) {
                SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                         SSL_R_BAD_DATA);
            }
            /* else this shouldn't happen, SSLfatal() already called */
            goto err;
        }
        /* still data left over */
        if (PACKET_remaining(pkt) != 0) {
            SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_KEY_EXCHANGE,
                     SSL_R_EXTRA_DATA_IN_MESSAGE);
            goto err;
        }
    }

    return MSG_PROCESS_CONTINUE_READING;
 err:
#ifndef OPENSSL_NO_CNSM
    if(alg_k & SSL_kECC){
        if(sm2_certs)
            BUF_MEM_free(sm2_certs);
        if(pkey)
            EVP_PKEY_free(pkey);
        if(engine_pctx)
        	EVP_PKEY_CTX_free(engine_pctx);
    }
#endif
    EVP_MD_CTX_free(md_ctx);
    return MSG_PROCESS_ERROR;
}

MSG_PROCESS_RETURN tls_process_certificate_request(SSL *s, PACKET *pkt)
{
    size_t i;

    /* Clear certificate validity flags */
    for (i = 0; i < SSL_PKEY_NUM; i++)
        s->s3->tmp.valid_flags[i] = 0;

    if (SSL_IS_TLS13(s)) {
        PACKET reqctx, extensions;
        RAW_EXTENSION *rawexts = NULL;

        if ((s->shutdown & SSL_SENT_SHUTDOWN) != 0) {
            /*
             * We already sent close_notify. This can only happen in TLSv1.3
             * post-handshake messages. We can't reasonably respond to this, so
             * we just ignore it
             */
            return MSG_PROCESS_FINISHED_READING;
        }

        /* Free and zero certificate types: it is not present in TLS 1.3 */
        OPENSSL_free(s->s3->tmp.ctype);
        s->s3->tmp.ctype = NULL;
        s->s3->tmp.ctype_len = 0;
        OPENSSL_free(s->pha_context);
        s->pha_context = NULL;

        if (!PACKET_get_length_prefixed_1(pkt, &reqctx) ||
            !PACKET_memdup(&reqctx, &s->pha_context, &s->pha_context_len)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                     SSL_R_LENGTH_MISMATCH);
            return MSG_PROCESS_ERROR;
        }

        if (!PACKET_get_length_prefixed_2(pkt, &extensions)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                     SSL_R_BAD_LENGTH);
            return MSG_PROCESS_ERROR;
        }
        if (!tls_collect_extensions(s, &extensions,
                                    SSL_EXT_TLS1_3_CERTIFICATE_REQUEST,
                                    &rawexts, NULL, 1)
            || !tls_parse_all_extensions(s, SSL_EXT_TLS1_3_CERTIFICATE_REQUEST,
                                         rawexts, NULL, 0, 1)) {
            /* SSLfatal() already called */
            OPENSSL_free(rawexts);
            return MSG_PROCESS_ERROR;
        }
        OPENSSL_free(rawexts);
        if (!tls1_process_sigalgs(s)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                     SSL_R_BAD_LENGTH);
            return MSG_PROCESS_ERROR;
        }
    } else {
        PACKET ctypes;

        /* get the certificate types */
        if (!PACKET_get_length_prefixed_1(pkt, &ctypes)) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                     SSL_R_LENGTH_MISMATCH);
            return MSG_PROCESS_ERROR;
        }

        if (!PACKET_memdup(&ctypes, &s->s3->tmp.ctype, &s->s3->tmp.ctype_len)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                     ERR_R_INTERNAL_ERROR);
            return MSG_PROCESS_ERROR;
        }

        if (SSL_USE_SIGALGS(s)) {
            PACKET sigalgs;

            if (!PACKET_get_length_prefixed_2(pkt, &sigalgs)) {
                SSLfatal(s, SSL_AD_DECODE_ERROR,
                         SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                         SSL_R_LENGTH_MISMATCH);
                return MSG_PROCESS_ERROR;
            }

            /*
             * Despite this being for certificates, preserve compatibility
             * with pre-TLS 1.3 and use the regular sigalgs field.
             */
            if (!tls1_save_sigalgs(s, &sigalgs, 0)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                         SSL_R_SIGNATURE_ALGORITHMS_ERROR);
                return MSG_PROCESS_ERROR;
            }
            if (!tls1_process_sigalgs(s)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                         ERR_R_MALLOC_FAILURE);
                return MSG_PROCESS_ERROR;
            }
        }

        /* get the CA RDNs */
        if (!parse_ca_names(s, pkt)) {
            /* SSLfatal() already called */
            return MSG_PROCESS_ERROR;
        }
    }

    if (PACKET_remaining(pkt) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR,
                 SSL_F_TLS_PROCESS_CERTIFICATE_REQUEST,
                 SSL_R_LENGTH_MISMATCH);
        return MSG_PROCESS_ERROR;
    }

    /* we should setup a certificate to return.... */
    s->s3->tmp.cert_req = 1;

    /*
     * In TLSv1.3 we don't prepare the client certificate yet. We wait until
     * after the CertificateVerify message has been received. This is because
     * in TLSv1.3 the CertificateRequest arrives before the Certificate message
     * but in TLSv1.2 it is the other way around. We want to make sure that
     * SSL_get_peer_certificate() returns something sensible in
     * client_cert_cb.
     */
    if (SSL_IS_TLS13(s) && s->post_handshake_auth != SSL_PHA_REQUESTED)
        return MSG_PROCESS_CONTINUE_READING;

    return MSG_PROCESS_CONTINUE_PROCESSING;
}

MSG_PROCESS_RETURN tls_process_new_session_ticket(SSL *s, PACKET *pkt)
{
    unsigned int ticklen;
    unsigned long ticket_lifetime_hint, age_add = 0;
    unsigned int sess_len;
    RAW_EXTENSION *exts = NULL;
    PACKET nonce;

    PACKET_null_init(&nonce);

    if (!PACKET_get_net_4(pkt, &ticket_lifetime_hint)
        || (SSL_IS_TLS13(s)
            && (!PACKET_get_net_4(pkt, &age_add)
                || !PACKET_get_length_prefixed_1(pkt, &nonce)))
        || !PACKET_get_net_2(pkt, &ticklen)
        || (SSL_IS_TLS13(s) ? (ticklen == 0 || PACKET_remaining(pkt) < ticklen)
                            : PACKET_remaining(pkt) != ticklen)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    /*
     * Server is allowed to change its mind (in <=TLSv1.2) and send an empty
     * ticket. We already checked this TLSv1.3 case above, so it should never
     * be 0 here in that instance
     */
    if (ticklen == 0)
        return MSG_PROCESS_CONTINUE_READING;

    /*
     * Sessions must be immutable once they go into the session cache. Otherwise
     * we can get multi-thread problems. Therefore we don't "update" sessions,
     * we replace them with a duplicate. In TLSv1.3 we need to do this every
     * time a NewSessionTicket arrives because those messages arrive
     * post-handshake and the session may have already gone into the session
     * cache.
     */
    if (SSL_IS_TLS13(s) || s->session->session_id_length > 0) {
        SSL_SESSION *new_sess;

        /*
         * We reused an existing session, so we need to replace it with a new
         * one
         */
        if ((new_sess = ssl_session_dup(s->session, 0)) == 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                     ERR_R_MALLOC_FAILURE);
            goto err;
        }

        if ((s->session_ctx->session_cache_mode & SSL_SESS_CACHE_CLIENT) != 0
                && !SSL_IS_TLS13(s)) {
            /*
             * In TLSv1.2 and below the arrival of a new tickets signals that
             * any old ticket we were using is now out of date, so we remove the
             * old session from the cache. We carry on if this fails
             */
            SSL_CTX_remove_session(s->session_ctx, s->session);
        }

        SSL_SESSION_free(s->session);
        s->session = new_sess;
    }

    /*
     * Technically the cast to long here is not guaranteed by the C standard -
     * but we use it elsewhere, so this should be ok.
     */
    s->session->time = (long)time(NULL);

    OPENSSL_free(s->session->ext.tick);
    s->session->ext.tick = NULL;
    s->session->ext.ticklen = 0;

    s->session->ext.tick = OPENSSL_malloc(ticklen);
    if (s->session->ext.tick == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }
    if (!PACKET_copy_bytes(pkt, s->session->ext.tick, ticklen)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    s->session->ext.tick_lifetime_hint = ticket_lifetime_hint;
    s->session->ext.tick_age_add = age_add;
    s->session->ext.ticklen = ticklen;

    if (SSL_IS_TLS13(s)) {
        PACKET extpkt;

        if (!PACKET_as_length_prefixed_2(pkt, &extpkt)
                || PACKET_remaining(pkt) != 0) {
            SSLfatal(s, SSL_AD_DECODE_ERROR,
                     SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                     SSL_R_LENGTH_MISMATCH);
            goto err;
        }

        if (!tls_collect_extensions(s, &extpkt,
                                    SSL_EXT_TLS1_3_NEW_SESSION_TICKET, &exts,
                                    NULL, 1)
                || !tls_parse_all_extensions(s,
                                             SSL_EXT_TLS1_3_NEW_SESSION_TICKET,
                                             exts, NULL, 0, 1)) {
            /* SSLfatal() already called */
            goto err;
        }
    }

    /*
     * There are two ways to detect a resumed ticket session. One is to set
     * an appropriate session ID and then the server must return a match in
     * ServerHello. This allows the normal client session ID matching to work
     * and we know much earlier that the ticket has been accepted. The
     * other way is to set zero length session ID when the ticket is
     * presented and rely on the handshake to determine session resumption.
     * We choose the former approach because this fits in with assumptions
     * elsewhere in OpenSSL. The session ID is set to the SHA256 (or SHA1 is
     * SHA256 is disabled) hash of the ticket.
     */
    /*
     * TODO(size_t): we use sess_len here because EVP_Digest expects an int
     * but s->session->session_id_length is a size_t
     */
    if (!EVP_Digest(s->session->ext.tick, ticklen,
                    s->session->session_id, &sess_len,
                    EVP_sha256(), NULL)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                 ERR_R_EVP_LIB);
        goto err;
    }
    s->session->session_id_length = sess_len;
    s->session->not_resumable = 0;

    /* This is a standalone message in TLSv1.3, so there is no more to read */
    if (SSL_IS_TLS13(s)) {
        const EVP_MD *md = ssl_handshake_md(s);
        int hashleni = EVP_MD_size(md);
        size_t hashlen;
        static const unsigned char nonce_label[] = "resumption";

        /* Ensure cast to size_t is safe */
        if (!ossl_assert(hashleni >= 0)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_PROCESS_NEW_SESSION_TICKET,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }
        hashlen = (size_t)hashleni;

        if (!tls13_hkdf_expand(s, md, s->resumption_master_secret,
                               nonce_label,
                               sizeof(nonce_label) - 1,
                               PACKET_data(&nonce),
                               PACKET_remaining(&nonce),
                               s->session->master_key,
                               hashlen, 1)) {
            /* SSLfatal() already called */
            goto err;
        }
        s->session->master_key_length = hashlen;

        OPENSSL_free(exts);
        ssl_update_cache(s, SSL_SESS_CACHE_CLIENT);
        return MSG_PROCESS_FINISHED_READING;
    }

    return MSG_PROCESS_CONTINUE_READING;
 err:
    OPENSSL_free(exts);
    return MSG_PROCESS_ERROR;
}

/*
 * In TLSv1.3 this is called from the extensions code, otherwise it is used to
 * parse a separate message. Returns 1 on success or 0 on failure
 */
int tls_process_cert_status_body(SSL *s, PACKET *pkt)
{
    size_t resplen;
    unsigned int type;

    if (!PACKET_get_1(pkt, &type)
        || type != TLSEXT_STATUSTYPE_ocsp) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_STATUS_BODY,
                 SSL_R_UNSUPPORTED_STATUS_TYPE);
        return 0;
    }
    if (!PACKET_get_net_3_len(pkt, &resplen)
        || PACKET_remaining(pkt) != resplen) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_STATUS_BODY,
                 SSL_R_LENGTH_MISMATCH);
        return 0;
    }
    s->ext.ocsp.resp = OPENSSL_malloc(resplen);
    if (s->ext.ocsp.resp == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_CERT_STATUS_BODY,
                 ERR_R_MALLOC_FAILURE);
        return 0;
    }
    if (!PACKET_copy_bytes(pkt, s->ext.ocsp.resp, resplen)) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_CERT_STATUS_BODY,
                 SSL_R_LENGTH_MISMATCH);
        return 0;
    }
    s->ext.ocsp.resp_len = resplen;

    return 1;
}


MSG_PROCESS_RETURN tls_process_cert_status(SSL *s, PACKET *pkt)
{
    if (!tls_process_cert_status_body(s, pkt)) {
        /* SSLfatal() already called */
        return MSG_PROCESS_ERROR;
    }

    return MSG_PROCESS_CONTINUE_READING;
}

/*
 * Perform miscellaneous checks and processing after we have received the
 * server's initial flight. In TLS1.3 this is after the Server Finished message.
 * In <=TLS1.2 this is after the ServerDone message. Returns 1 on success or 0
 * on failure.
 */
int tls_process_initial_server_flight(SSL *s)
{
    /*
     * at this point we check that we have the required stuff from
     * the server
     */
    if (!ssl3_check_cert_and_algorithm(s)) {
        /* SSLfatal() already called */
        return 0;
    }

    /*
     * Call the ocsp status callback if needed. The |ext.ocsp.resp| and
     * |ext.ocsp.resp_len| values will be set if we actually received a status
     * message, or NULL and -1 otherwise
     */
    if (s->ext.status_type != TLSEXT_STATUSTYPE_nothing
            && s->ctx->ext.status_cb != NULL) {
        int ret = s->ctx->ext.status_cb(s, s->ctx->ext.status_arg);

        if (ret == 0) {
            SSLfatal(s, SSL_AD_BAD_CERTIFICATE_STATUS_RESPONSE,
                     SSL_F_TLS_PROCESS_INITIAL_SERVER_FLIGHT,
                     SSL_R_INVALID_STATUS_RESPONSE);
            return 0;
        }
        if (ret < 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_PROCESS_INITIAL_SERVER_FLIGHT,
                     ERR_R_MALLOC_FAILURE);
            return 0;
        }
    }
#ifndef OPENSSL_NO_CT
    if (s->ct_validation_callback != NULL) {
        /* Note we validate the SCTs whether or not we abort on error */
        if (!ssl_validate_ct(s) && (s->verify_mode & SSL_VERIFY_PEER)) {
            /* SSLfatal() already called */
            return 0;
        }
    }
#endif

    return 1;
}

MSG_PROCESS_RETURN tls_process_server_done(SSL *s, PACKET *pkt)
{
    if (PACKET_remaining(pkt) > 0) {
        /* should contain no data */
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_SERVER_DONE,
                 SSL_R_LENGTH_MISMATCH);
        return MSG_PROCESS_ERROR;
    }
#ifndef OPENSSL_NO_SRP
    if (s->s3->tmp.new_cipher->algorithm_mkey & SSL_kSRP) {
        if (SRP_Calc_A_param(s) <= 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PROCESS_SERVER_DONE,
                     SSL_R_SRP_A_CALC);
            return MSG_PROCESS_ERROR;
        }
    }
#endif

    if (!tls_process_initial_server_flight(s)) {
        /* SSLfatal() already called */
        return MSG_PROCESS_ERROR;
    }

    return MSG_PROCESS_FINISHED_READING;
}

MSG_PROCESS_RETURN GMold_tls_process_server_done(SSL *s, PACKET *pkt)
{
    if (PACKET_remaining(pkt) > 0) {
        /* should contain no data */
        ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_DECODE_ERROR);
        s->statem.state = MSG_FLOW_ERROR;
        return MSG_PROCESS_ERROR;
    }
    if (s->s3->tmp.new_cipher->algorithm_mkey & SSL_kSRP) {
        if (SRP_Calc_A_param(s) <= 0) {
            ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
            s->statem.state = MSG_FLOW_ERROR;
            return MSG_PROCESS_ERROR;
        }
    }

    /*
     * at this point we check that we have the required stuff from
     * the server
     */
    if (!ssl3_check_cert_and_algorithm(s)) {
        ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_HANDSHAKE_FAILURE);
        s->statem.state = MSG_FLOW_ERROR;
        return MSG_PROCESS_ERROR;
    }

    /*
     * Call the ocsp status callback if needed. The |tlsext_ocsp_resp| and
     * |tlsext_ocsp_resplen| values will be set if we actually received a status
     * message, or NULL and -1 otherwise
     */     //TODO xcs
    // if (s->tlsext_status_type != -1 && s->ctx->tlsext_status_cb != NULL) {
    //     int ret;
    //     ret = s->ctx->tlsext_status_cb(s, s->ctx->tlsext_status_arg);
    //     if (ret == 0) {
    //         ssl3_send_alert(s, SSL3_AL_FATAL,
    //                         SSL_AD_BAD_CERTIFICATE_STATUS_RESPONSE);
    //         return MSG_PROCESS_ERROR;
    //     }
    //     if (ret < 0) {
    //         ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
    //         return MSG_PROCESS_ERROR;
    //     }
    // }
    if (s->ct_validation_callback != NULL) {
        /* Note we validate the SCTs whether or not we abort on error */
        if (!ssl_validate_ct(s) && (s->verify_mode & SSL_VERIFY_PEER)) {
            ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_HANDSHAKE_FAILURE);
            return MSG_PROCESS_ERROR;
        }
    }

        return MSG_PROCESS_FINISHED_READING;
}

static int tls_construct_cke_psk_preamble(SSL *s, WPACKET *pkt)
{
#ifndef OPENSSL_NO_PSK
    int ret = 0;
    /*
     * The callback needs PSK_MAX_IDENTITY_LEN + 1 bytes to return a
     * \0-terminated identity. The last byte is for us for simulating
     * strnlen.
     */
    char identity[PSK_MAX_IDENTITY_LEN + 1];
    size_t identitylen = 0;
    unsigned char psk[PSK_MAX_PSK_LEN];
    unsigned char *tmppsk = NULL;
    char *tmpidentity = NULL;
    size_t psklen = 0;

    if (s->psk_client_callback == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE,
                 SSL_R_PSK_NO_CLIENT_CB);
        goto err;
    }

    memset(identity, 0, sizeof(identity));

    psklen = s->psk_client_callback(s, s->session->psk_identity_hint,
                                    identity, sizeof(identity) - 1,
                                    psk, sizeof(psk));

    if (psklen > PSK_MAX_PSK_LEN) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE, ERR_R_INTERNAL_ERROR);
        goto err;
    } else if (psklen == 0) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE,
                 SSL_R_PSK_IDENTITY_NOT_FOUND);
        goto err;
    }

    identitylen = strlen(identity);
    if (identitylen > PSK_MAX_IDENTITY_LEN) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    tmppsk = OPENSSL_memdup(psk, psklen);
    tmpidentity = OPENSSL_strdup(identity);
    if (tmppsk == NULL || tmpidentity == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    OPENSSL_free(s->s3->tmp.psk);
    s->s3->tmp.psk = tmppsk;
    s->s3->tmp.psklen = psklen;
    tmppsk = NULL;
    OPENSSL_free(s->session->psk_identity);
    s->session->psk_identity = tmpidentity;
    tmpidentity = NULL;

    if (!WPACKET_sub_memcpy_u16(pkt, identity, identitylen))  {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    ret = 1;

 err:
    OPENSSL_cleanse(psk, psklen);
    OPENSSL_cleanse(identity, sizeof(identity));
    OPENSSL_clear_free(tmppsk, psklen);
    OPENSSL_clear_free(tmpidentity, identitylen);

    return ret;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_PSK_PREAMBLE,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_construct_cke_rsa(SSL *s, WPACKET *pkt)
{
#ifndef OPENSSL_NO_RSA
    unsigned char *encdata = NULL;
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    size_t enclen;
    unsigned char *pms = NULL;
    size_t pmslen = 0;

    if (s->session->peer == NULL) {
        /*
         * We should always have a server certificate with SSL_kRSA.
         */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    pkey = X509_get0_pubkey(s->session->peer);
    if (EVP_PKEY_get0_RSA(pkey) == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    pmslen = SSL_MAX_MASTER_KEY_LENGTH;
    pms = OPENSSL_malloc(pmslen);
    if (pms == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_MALLOC_FAILURE);
        return 0;
    }

    pms[0] = s->client_version >> 8;
    pms[1] = s->client_version & 0xff;
    /* TODO(size_t): Convert this function */
    if (RAND_bytes(pms + 2, (int)(pmslen - 2)) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    /* Fix buf for TLS and beyond */
    if (s->version > SSL3_VERSION && !WPACKET_start_sub_packet_u16(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }
    pctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (pctx == NULL || EVP_PKEY_encrypt_init(pctx) <= 0
        || EVP_PKEY_encrypt(pctx, NULL, &enclen, pms, pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_EVP_LIB);
        goto err;
    }
    if (!WPACKET_allocate_bytes(pkt, enclen, &encdata)
            || EVP_PKEY_encrypt(pctx, encdata, &enclen, pms, pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 SSL_R_BAD_RSA_ENCRYPT);
        goto err;
    }
    EVP_PKEY_CTX_free(pctx);
    pctx = NULL;

    /* Fix buf for TLS and beyond */
    if (s->version > SSL3_VERSION && !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* Log the premaster secret, if logging is enabled. */
    if (!ssl_log_rsa_client_key_exchange(s, encdata, enclen, pms, pmslen)) {
        /* SSLfatal() already called */
        goto err;
    }

    s->s3->tmp.pms = pms;
    s->s3->tmp.pmslen = pmslen;

    return 1;
 err:
    OPENSSL_clear_free(pms, pmslen);
    EVP_PKEY_CTX_free(pctx);

    return 0;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_RSA,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

#ifndef OPENSSL_NO_CNSM
static int tls_construct_cke_sm2ecc(SSL *s, WPACKET *pkt)
{
    printf("ddddddddddd tls_construct_cke_sm2ecc \n");
    
    int i = 0;
    unsigned char *encdata = NULL;
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = NULL;
    size_t enclen;
    unsigned char *pms = NULL;
    size_t pmslen = 0;

    if (s->session->peer_chain == NULL) {
        /*
         * We should always have a server certificate with SSL_kECC.
         */
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
    
    /*从链表最后开始，查找第一个数据加密功能的证书,作为加密证书使用，跟排列顺序无关*/
    for(i=sk_X509_num(s->session->peer_chain)-1; i>=0; i--){
        if((X509_get_extension_flags(sk_X509_value(s->session->peer_chain, i)) & EXFLAG_KUSAGE) && (X509_get_key_usage(sk_X509_value(s->session->peer_chain, i)) & X509v3_KU_DATA_ENCIPHERMENT))
            break;
    }
        
    pkey = X509_get0_pubkey(sk_X509_value(s->session->peer_chain, i)); //get the cert_chain last one ,the enc cert in the last of cert_chain 获取证书链最后一个，证书链最后一个的enc证书
    if (EVP_PKEY_get0_EC_KEY(pkey) == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    pmslen = SSL_MAX_MASTER_KEY_LENGTH;
    pms = OPENSSL_malloc(pmslen);
    if (pms == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_MALLOC_FAILURE);
        return 0;
    }

    pms[0] = s->client_version >> 8;
    pms[1] = s->client_version & 0xff;
    /* TODO(size_t): Convert this function */
    if (RAND_bytes(pms + 2, (int)(pmslen - 2)) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

#ifdef CIPHER_DEBUG
    {
    	int gi = 0;
    	printf("client --->the pre mask key =[");
    	for(gi=0; gi<pmslen; gi++){
    		printf("%02X", pms[gi]);
    	}
    	printf("]\n");
    }
#endif
    
    /* Fix buf for TLS and beyond */
    if ((s->version > SSL3_VERSION|| s->version == SM1_1_VERSION) && !WPACKET_start_sub_packet_u16(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }
    pctx = EVP_PKEY_CTX_new_pkey_id(pkey, NID_sm2, NULL);
    if (pctx == NULL || EVP_PKEY_encrypt_init(pctx) <= 0
        || EVP_PKEY_encrypt(pctx, NULL, &enclen, pms, pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_EVP_LIB);
        goto err;
    }
    
   /*	
    if (!WPACKET_allocate_bytes(pkt, enclen, &encdata)
            || EVP_PKEY_encrypt(pctx, encdata, &enclen, pms, pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 SSL_R_BAD_RSA_ENCRYPT);
        goto err;
    }*/
    if (!WPACKET_reserve_bytes(pkt, enclen, &encdata)
            || EVP_PKEY_encrypt(pctx, encdata, &enclen, pms, pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 SSL_R_BAD_RSA_ENCRYPT);
        goto err;
    }
    pkt->written += enclen;   //签名时分配的字节数为最大的022100，所以真正签名完成时要设置真实数值，因为有的服务端不认后面带00的加密密文
    pkt->curr += enclen;
    
    EVP_PKEY_CTX_free(pctx);
    pctx = NULL;

    /* Fix buf for TLS and beyond */
    if ((s->version > SSL3_VERSION|| s->version == SM1_1_VERSION) && !WPACKET_close(pkt)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2ECC,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    /* Log the premaster secret, if logging is enabled. */
    if (!ssl_log_rsa_client_key_exchange(s, encdata, enclen, pms, pmslen)) {
        /* SSLfatal() already called */
        goto err;
    }

    s->s3->tmp.pms = pms;
    s->s3->tmp.pmslen = pmslen;

    return 1;
 err:
    OPENSSL_clear_free(pms, pmslen);
    EVP_PKEY_CTX_free(pctx);
    return 0;
}

static int tls_construct_cke_sm2dh(SSL *s, WPACKET *pkt)
{
    printf("ddddddddddd tls_construct_cke_sm2dh \n");
    
    unsigned char *encodedPoint = NULL;
    size_t encoded_pt_len = 0;
    EVP_PKEY *ckey = NULL, *skey = NULL;
    int ret = 0;
    uint16_t curve_id = 0;
    ENGINE *e_tmp = NULL;
    EVP_PKEY_CTX *pctx = NULL;

    skey = s->s3->peer_tmp;
    if (skey == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2DH,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
    /*签名私钥使用引擎时，使用引擎产生临时秘钥对*/
    if(s->cert->pkeys[SSL_PKEY_ECC].privatekey)
        e_tmp = EVP_PKEY_pmeth_engine(s->cert->pkeys[SSL_PKEY_ECC].privatekey);
    else{
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2DH,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }
    
    ckey = EVP_PKEY_new();
    pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_SM2, e_tmp);  
    
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_sm2_paramgen_curve_nid(pctx, NID_sm2);
    EVP_PKEY_CTX_set_ec_param_enc(pctx, OPENSSL_EC_NAMED_CURVE);
    
    if(!EVP_PKEY_keygen(pctx, &ckey))
    {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2DH,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }
	
    if (ssl_derive_SM2(s, ckey, skey, 0) == 0) {
        /* SSLfatal() already called */
        goto err;
    }

    /* Generate encoding of client key */
    encoded_pt_len = EVP_PKEY_get1_tls_encodedpoint(ckey, &encodedPoint);

    if (encoded_pt_len == 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2DH,
                 ERR_R_EC_LIB);
        goto err;
    }
    
    /* 国密局检测用的是00，有的厂商用的也是00，所以默认用00 */
#ifdef STD_CURVE_ID
    curve_id = tls1_nid2group_id(NID_sm2);
#else
    curve_id = 0;
#endif
    if	(!WPACKET_put_bytes_u8(pkt, NAMED_CURVE_TYPE)
                || !WPACKET_put_bytes_u8(pkt, 0)
                || !WPACKET_put_bytes_u8(pkt, curve_id)
                || !WPACKET_sub_memcpy_u8(pkt, encodedPoint, encoded_pt_len)){
	SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SM2DH,
                 ERR_R_INTERNAL_ERROR);
        goto err;

    }

    ret = 1;
 err:
    OPENSSL_free(encodedPoint);
    EVP_PKEY_free(ckey);
    return ret;
}
#endif

static int tls_construct_cke_dhe(SSL *s, WPACKET *pkt)
{
#ifndef OPENSSL_NO_DH
    DH *dh_clnt = NULL;
    const BIGNUM *pub_key;
    EVP_PKEY *ckey = NULL, *skey = NULL;
    unsigned char *keybytes = NULL;

    skey = s->s3->peer_tmp;
    if (skey == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_DHE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    ckey = ssl_generate_pkey(skey);
    if (ckey == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_DHE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    dh_clnt = EVP_PKEY_get0_DH(ckey);

    if (dh_clnt == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_DHE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (ssl_derive(s, ckey, skey, 0) == 0) {
        /* SSLfatal() already called */
        goto err;
    }

    /* send off the data */
    DH_get0_key(dh_clnt, &pub_key, NULL);
    if (!WPACKET_sub_allocate_bytes_u16(pkt, BN_num_bytes(pub_key),
                                        &keybytes)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_DHE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    BN_bn2bin(pub_key, keybytes);
    EVP_PKEY_free(ckey);

    return 1;
 err:
    EVP_PKEY_free(ckey);
    return 0;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_DHE,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_construct_cke_ecdhe(SSL *s, WPACKET *pkt)
{
#ifndef OPENSSL_NO_EC
    unsigned char *encodedPoint = NULL;
    size_t encoded_pt_len = 0;
    EVP_PKEY *ckey = NULL, *skey = NULL;
    int ret = 0;

    skey = s->s3->peer_tmp;
    if (skey == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_ECDHE,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    ckey = ssl_generate_pkey(skey);
    if (ckey == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_ECDHE,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    if (ssl_derive(s, ckey, skey, 0) == 0) {
        /* SSLfatal() already called */
        goto err;
    }

    /* Generate encoding of client key */
    encoded_pt_len = EVP_PKEY_get1_tls_encodedpoint(ckey, &encodedPoint);

    if (encoded_pt_len == 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_ECDHE,
                 ERR_R_EC_LIB);
        goto err;
    }

    if (!WPACKET_sub_memcpy_u8(pkt, encodedPoint, encoded_pt_len)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_ECDHE,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    ret = 1;
 err:
    OPENSSL_free(encodedPoint);
    EVP_PKEY_free(ckey);
    return ret;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_ECDHE,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_construct_cke_gost(SSL *s, WPACKET *pkt)
{
#ifndef OPENSSL_NO_GOST
    /* GOST key exchange message creation */
    EVP_PKEY_CTX *pkey_ctx = NULL;
    X509 *peer_cert;
    size_t msglen;
    unsigned int md_len;
    unsigned char shared_ukm[32], tmp[256];
    EVP_MD_CTX *ukm_hash = NULL;
    int dgst_nid = NID_id_GostR3411_94;
    unsigned char *pms = NULL;
    size_t pmslen = 0;

    if ((s->s3->tmp.new_cipher->algorithm_auth & SSL_aGOST12) != 0)
        dgst_nid = NID_id_GostR3411_2012_256;

    /*
     * Get server certificate PKEY and create ctx from it
     */
    peer_cert = s->session->peer;
    if (!peer_cert) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE, SSL_F_TLS_CONSTRUCT_CKE_GOST,
               SSL_R_NO_GOST_CERTIFICATE_SENT_BY_PEER);
        return 0;
    }

    pkey_ctx = EVP_PKEY_CTX_new(X509_get0_pubkey(peer_cert), NULL);
    if (pkey_ctx == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 ERR_R_MALLOC_FAILURE);
        return 0;
    }
    /*
     * If we have send a certificate, and certificate key
     * parameters match those of server certificate, use
     * certificate key for key exchange
     */

    /* Otherwise, generate ephemeral key pair */
    pmslen = 32;
    pms = OPENSSL_malloc(pmslen);
    if (pms == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 ERR_R_MALLOC_FAILURE);
        goto err;
    }

    if (EVP_PKEY_encrypt_init(pkey_ctx) <= 0
        /* Generate session key
         * TODO(size_t): Convert this function
         */
        || RAND_bytes(pms, (int)pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    };
    /*
     * Compute shared IV and store it in algorithm-specific context
     * data
     */
    ukm_hash = EVP_MD_CTX_new();
    if (ukm_hash == NULL
        || EVP_DigestInit(ukm_hash, EVP_get_digestbynid(dgst_nid)) <= 0
        || EVP_DigestUpdate(ukm_hash, s->s3->client_random,
                            SSL3_RANDOM_SIZE) <= 0
        || EVP_DigestUpdate(ukm_hash, s->s3->server_random,
                            SSL3_RANDOM_SIZE) <= 0
        || EVP_DigestFinal_ex(ukm_hash, shared_ukm, &md_len) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }
    EVP_MD_CTX_free(ukm_hash);
    ukm_hash = NULL;
    if (EVP_PKEY_CTX_ctrl(pkey_ctx, -1, EVP_PKEY_OP_ENCRYPT,
                          EVP_PKEY_CTRL_SET_IV, 8, shared_ukm) < 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 SSL_R_LIBRARY_BUG);
        goto err;
    }
    /* Make GOST keytransport blob message */
    /*
     * Encapsulate it into sequence
     */
    msglen = 255;
    if (EVP_PKEY_encrypt(pkey_ctx, tmp, &msglen, pms, pmslen) <= 0) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 SSL_R_LIBRARY_BUG);
        goto err;
    }

    if (!WPACKET_put_bytes_u8(pkt, V_ASN1_SEQUENCE | V_ASN1_CONSTRUCTED)
            || (msglen >= 0x80 && !WPACKET_put_bytes_u8(pkt, 0x81))
            || !WPACKET_sub_memcpy_u8(pkt, tmp, msglen)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
                 ERR_R_INTERNAL_ERROR);
        goto err;
    }

    EVP_PKEY_CTX_free(pkey_ctx);
    s->s3->tmp.pms = pms;
    s->s3->tmp.pmslen = pmslen;

    return 1;
 err:
    EVP_PKEY_CTX_free(pkey_ctx);
    OPENSSL_clear_free(pms, pmslen);
    EVP_MD_CTX_free(ukm_hash);
    return 0;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_GOST,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

static int tls_construct_cke_srp(SSL *s, WPACKET *pkt)
{
#ifndef OPENSSL_NO_SRP
    unsigned char *abytes = NULL;

    if (s->srp_ctx.A == NULL
            || !WPACKET_sub_allocate_bytes_u16(pkt, BN_num_bytes(s->srp_ctx.A),
                                               &abytes)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SRP,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
    BN_bn2bin(s->srp_ctx.A, abytes);

    OPENSSL_free(s->session->srp_username);
    s->session->srp_username = OPENSSL_strdup(s->srp_ctx.login);
    if (s->session->srp_username == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SRP,
                 ERR_R_MALLOC_FAILURE);
        return 0;
    }

    return 1;
#else
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_CKE_SRP,
             ERR_R_INTERNAL_ERROR);
    return 0;
#endif
}

int tls_construct_client_key_exchange(SSL *s, WPACKET *pkt)
{
    unsigned long alg_k;
    printf("ddddddddddd tls_construct_client_key_exchange \n");
    
    alg_k = s->s3->tmp.new_cipher->algorithm_mkey;

    /*
     * All of the construct functions below call SSLfatal() if necessary so
     * no need to do so here.
     */
    if ((alg_k & SSL_PSK)
        && !tls_construct_cke_psk_preamble(s, pkt))
        goto err;

    if (alg_k & (SSL_kRSA | SSL_kRSAPSK)) {
        if (!tls_construct_cke_rsa(s, pkt))
            goto err;
    } else if (alg_k & (SSL_kDHE | SSL_kDHEPSK)) {
        if (!tls_construct_cke_dhe(s, pkt))
            goto err;
    #ifndef OPENSSL_NO_CNSM
    }else if (alg_k & SSL_kECC) {
        if (!tls_construct_cke_sm2ecc(s, pkt))
            goto err;
     }else if (alg_k & SSL_kSM2DH) {
        if (!tls_construct_cke_sm2dh(s, pkt))
            goto err;
    #endif
    } else if (alg_k & (SSL_kECDHE | SSL_kECDHEPSK)) {
        if (!tls_construct_cke_ecdhe(s, pkt))
            goto err;
    } else if (alg_k & SSL_kGOST) {
        if (!tls_construct_cke_gost(s, pkt))
            goto err;
    } else if (alg_k & SSL_kSRP) {
        if (!tls_construct_cke_srp(s, pkt))
            goto err;
    } else if (!(alg_k & SSL_kPSK)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_TLS_CONSTRUCT_CLIENT_KEY_EXCHANGE, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    return 1;
 err:
    OPENSSL_clear_free(s->s3->tmp.pms, s->s3->tmp.pmslen);
    s->s3->tmp.pms = NULL;
#ifndef OPENSSL_NO_PSK
    OPENSSL_clear_free(s->s3->tmp.psk, s->s3->tmp.psklen);
    s->s3->tmp.psk = NULL;
#endif
    return 0;
}

int tls_client_key_exchange_post_work(SSL *s)
{
    printf("ddddddddddd tls_client_key_exchange_post_work \n");

    unsigned char *pms = NULL;
    size_t pmslen = 0;
#ifndef OPENSSL_NO_CNSM
    ENGINE *local_e_sm2 = NULL;
    ENGINE *local_e_sm4 = NULL;
    EVP_PKEY * local_evp_ptr = NULL;
#endif

    pms = s->s3->tmp.pms;
    pmslen = s->s3->tmp.pmslen;

#ifndef OPENSSL_NO_SRP
    /* Check for SRP */
    if (s->s3->tmp.new_cipher->algorithm_mkey & SSL_kSRP) {
        if (!srp_generate_client_master_secret(s)) {
            /* SSLfatal() already called */
            goto err;
        }
        return 1;
    }
#endif

    if (pms == NULL && !(s->s3->tmp.new_cipher->algorithm_mkey & SSL_kPSK)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_TLS_CLIENT_KEY_EXCHANGE_POST_WORK, ERR_R_MALLOC_FAILURE);
        goto err;
    }
#ifndef OPENSSL_NO_CNSM
#ifndef OPENSSL_NO_ENGINE
    //如果此ssl的私钥加载了sm4引擎，则使用引擎进行masterkey计算  
    local_evp_ptr = s->cert->pkeys[SSL_PKEY_ECC_ENC].privatekey;
    if(local_evp_ptr)
        local_e_sm2 = EVP_PKEY_pmeth_engine(local_evp_ptr);
    local_e_sm4 = ENGINE_get_cipher_engine(NID_sm4_cbc); 
    if(local_evp_ptr && local_e_sm4){
        if(s->s3 && s->s3->tmp.new_cipher && s->s3->tmp.new_cipher->id == TLS1_CK_ECDHE_WITH_SM4_SM3){      //ECDHE-SM4-SM2套件并且加载了SM2引擎，则使用密文premasterkey作为输入
            if(local_e_sm2)
                ENGINE_set_tass_flags(local_e_sm4, TASS_FLAG_PRE_MASTER_KEY_CIPHER);
        }
        if(!ENGINE_ssl_generate_master_secret(local_e_sm4, s, pms, pmslen, 1)){
            pmslen = 0;
            goto err;
        }
        ENGINE_finish(local_e_sm4);
    }
    else 
#endif
#endif
    if (!ssl_generate_master_secret(s, pms, pmslen, 1)) {
        /* SSLfatal() already called */
        /* ssl_generate_master_secret frees the pms even on error */
        pms = NULL;
        pmslen = 0;
        goto err;
    }
    pms = NULL;
    pmslen = 0;

#ifndef OPENSSL_NO_SCTP
    if (SSL_IS_DTLS(s)) {
        unsigned char sctpauthkey[64];
        char labelbuffer[sizeof(DTLS1_SCTP_AUTH_LABEL)];
        size_t labellen;

        /*
         * Add new shared key for SCTP-Auth, will be ignored if no SCTP
         * used.
         */
        memcpy(labelbuffer, DTLS1_SCTP_AUTH_LABEL,
               sizeof(DTLS1_SCTP_AUTH_LABEL));

        /* Don't include the terminating zero. */
        labellen = sizeof(labelbuffer) - 1;
        if (s->mode & SSL_MODE_DTLS_SCTP_LABEL_LENGTH_BUG)
            labellen += 1;

        if (SSL_export_keying_material(s, sctpauthkey,
                                       sizeof(sctpauthkey), labelbuffer,
                                       labellen, NULL, 0, 0) <= 0) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_CLIENT_KEY_EXCHANGE_POST_WORK,
                     ERR_R_INTERNAL_ERROR);
            goto err;
        }

        BIO_ctrl(SSL_get_wbio(s), BIO_CTRL_DGRAM_SCTP_ADD_AUTH_KEY,
                 sizeof(sctpauthkey), sctpauthkey);
    }
#endif

    return 1;
 err:
#ifndef OPENSSL_NO_CSNM
#ifndef OPENSSL_NO_ENGINE
    if(local_e_sm4)
        ENGINE_finish(local_e_sm4);
#endif
#endif
    OPENSSL_clear_free(pms, pmslen);
    s->s3->tmp.pms = NULL;
    return 0;
}

int GMold_tls_client_key_exchange_post_work(SSL *s)
{
    unsigned char *pms = NULL;
    size_t pmslen = 0;

    pms = s->s3->tmp.pms;
    pmslen = s->s3->tmp.pmslen;

    /* Check for SRP */
    if (s->s3->tmp.new_cipher->algorithm_mkey & SSL_kSRP) {
        if (!srp_generate_client_master_secret(s)) {
            goto err;
        }
        return 1;
    }

    if (pms == NULL && !(s->s3->tmp.new_cipher->algorithm_mkey & SSL_kPSK)) {
        ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
        goto err;
    }
    if (!GMold_ssl_generate_master_secret(s, pms, pmslen, 1)) {
        ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
        /* ssl_generate_master_secret frees the pms even on error */
        pms = NULL;
        pmslen = 0;
        goto err;
    }
    pms = NULL;
    pmslen = 0;
    return 1;
 err:
    CRYPTO_clear_free(pms, pmslen, OPENSSL_FILE, OPENSSL_LINE);
    s->s3->tmp.pms = NULL;
    return 0;
}
/*
 * Check a certificate can be used for client authentication. Currently check
 * cert exists, if we have a suitable digest for TLS 1.2 if static DH client
 * certificates can be used and optionally checks suitability for Suite B.
 */
static int ssl3_check_client_certificate(SSL *s)
{
    printf("ddddddddddd ssl3_check_client_certificate \n");
    /* If no suitable signature algorithm can't use certificate */
    if (!tls_choose_sigalg(s, 0) || s->s3->tmp.sigalg == NULL)
        return 0;
    /*
     * If strict mode check suitability of chain before using it. This also
     * adjusts suite B digest if necessary.
     */
    if (s->cert->cert_flags & SSL_CERT_FLAGS_CHECK_TLS_STRICT &&
        !tls1_check_chain(s, NULL, NULL, NULL, -2))
        return 0;
    return 1;
}

static int GMold_ssl3_check_client_certificate(SSL *s)
{
    if (!s->cert || !s->cert->key->x509 || !s->cert->key->privatekey)
        return 0;
    /* If no suitable signature algorithm can't use certificate */
    if (SSL_USE_SIGALGS(s))
        return 0;
    /*
     * If strict mode check suitability of chain before using it. This also
     * adjusts suite B digest if necessary.
     */
    if (s->cert->cert_flags & SSL_CERT_FLAGS_CHECK_TLS_STRICT &&
        !tls1_check_chain(s, NULL, NULL, NULL, -2))
        return 0;
    return 1;
}

WORK_STATE tls_prepare_client_certificate(SSL *s, WORK_STATE wst)
{
    printf("ddddddddddd tls_prepare_client_certificate \n");
    X509 *x509 = NULL;
    EVP_PKEY *pkey = NULL;
    int i;

    if (wst == WORK_MORE_A) {
        /* Let cert callback update client certificates if required */
        if (s->cert->cert_cb) {
            i = s->cert->cert_cb(s, s->cert->cert_cb_arg);
            if (i < 0) {
                s->rwstate = SSL_X509_LOOKUP;
                return WORK_MORE_A;
            }
            if (i == 0) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_PREPARE_CLIENT_CERTIFICATE,
                         SSL_R_CALLBACK_FAILED);
                return WORK_ERROR;
            }
            s->rwstate = SSL_NOTHING;
        }
        if (ssl3_check_client_certificate(s)) {
            if (s->post_handshake_auth == SSL_PHA_REQUESTED) {
                return WORK_FINISHED_STOP;
            }
            return WORK_FINISHED_CONTINUE;
        }

        /* Fall through to WORK_MORE_B */
        wst = WORK_MORE_B;
    }

    /* We need to get a client cert */
    if (wst == WORK_MORE_B) {
        /*
         * If we get an error, we need to ssl->rwstate=SSL_X509_LOOKUP;
         * return(-1); We then get retied later
         */
        i = ssl_do_client_cert_cb(s, &x509, &pkey);
        if (i < 0) {
            s->rwstate = SSL_X509_LOOKUP;
            return WORK_MORE_B;
        }
        s->rwstate = SSL_NOTHING;
        if ((i == 1) && (pkey != NULL) && (x509 != NULL)) {
            if (!SSL_use_certificate(s, x509) || !SSL_use_PrivateKey(s, pkey))
                i = 0;
        } else if (i == 1) {
            i = 0;
            SSLerr(SSL_F_TLS_PREPARE_CLIENT_CERTIFICATE,
                   SSL_R_BAD_DATA_RETURNED_BY_CALLBACK);
        }

        X509_free(x509);
        EVP_PKEY_free(pkey);
        if (i && !ssl3_check_client_certificate(s))
            i = 0;
        if (i == 0) {
            if (s->version == SSL3_VERSION) {
                s->s3->tmp.cert_req = 0;
                ssl3_send_alert(s, SSL3_AL_WARNING, SSL_AD_NO_CERTIFICATE);
                return WORK_FINISHED_CONTINUE;
            } else {
                s->s3->tmp.cert_req = 2;
                if (!ssl3_digest_cached_records(s, 0)) {
                    /* SSLfatal() already called */
                    return WORK_ERROR;
                }
            }
        }

        if (s->post_handshake_auth == SSL_PHA_REQUESTED)
            return WORK_FINISHED_STOP;
        return WORK_FINISHED_CONTINUE;
    }

    /* Shouldn't ever get here */
    SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_PREPARE_CLIENT_CERTIFICATE,
             ERR_R_INTERNAL_ERROR);
    return WORK_ERROR;
}


WORK_STATE GMold_tls_prepare_client_certificate(SSL *s, WORK_STATE wst)
{
    X509 *x509 = NULL;
    EVP_PKEY *pkey = NULL;
    int i;

    if (wst == WORK_MORE_A) {
        /* Let cert callback update client certificates if required */
        if (s->cert->cert_cb) {
            i = s->cert->cert_cb(s, s->cert->cert_cb_arg);
            if (i < 0) {
                s->rwstate = SSL_X509_LOOKUP;
                return WORK_MORE_A;
            }
            if (i == 0) {
                ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
                s->statem.state = MSG_FLOW_ERROR;
                return 0;
            }
            s->rwstate = SSL_NOTHING;
        }
        if (GMold_ssl3_check_client_certificate(s))
            return WORK_FINISHED_CONTINUE;

        /* Fall through to WORK_MORE_B */
        wst = WORK_MORE_B;
    }

    /* We need to get a client cert */
    if (wst == WORK_MORE_B) {
        /*
         * If we get an error, we need to ssl->rwstate=SSL_X509_LOOKUP;
         * return(-1); We then get retied later
         */
        i = ssl_do_client_cert_cb(s, &x509, &pkey);
        if (i < 0) {
            s->rwstate = SSL_X509_LOOKUP;
            return WORK_MORE_B;
        }
        s->rwstate = SSL_NOTHING;
        if ((i == 1) && (pkey != NULL) && (x509 != NULL)) {
            if (!SSL_use_certificate(s, x509) || !SSL_use_PrivateKey(s, pkey))
                i = 0;
        } else if (i == 1) {
            i = 0;
        }

        X509_free(x509);
        EVP_PKEY_free(pkey);
        if (i && !ssl3_check_client_certificate(s))
            i = 0;
        if (i == 0) {
            if (s->version == SSL3_VERSION) {
                s->s3->tmp.cert_req = 0;
                ssl3_send_alert(s, SSL3_AL_WARNING, SSL_AD_NO_CERTIFICATE);
                return WORK_FINISHED_CONTINUE;
            } else {
                s->s3->tmp.cert_req = 2;
                if (!GMold_ssl3_digest_cached_records(s, 0)) {
                    ssl3_send_alert(s, SSL3_AL_FATAL, SSL_AD_INTERNAL_ERROR);
                    s->statem.state = MSG_FLOW_ERROR;
                    return 0;
                }
            }
        }

        return WORK_FINISHED_CONTINUE;
    }

    /* Shouldn't ever get here */
    return WORK_ERROR;
}

int tls_construct_client_certificate(SSL *s, WPACKET *pkt)
{
    printf("ddddddddddd tls_construct_client_certificate \n");

    if (SSL_IS_TLS13(s)) {
        if (s->pha_context == NULL) {
            /* no context available, add 0-length context */
            if (!WPACKET_put_bytes_u8(pkt, 0)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_TLS_CONSTRUCT_CLIENT_CERTIFICATE, ERR_R_INTERNAL_ERROR);
                return 0;
            }
        } else if (!WPACKET_sub_memcpy_u8(pkt, s->pha_context, s->pha_context_len)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                     SSL_F_TLS_CONSTRUCT_CLIENT_CERTIFICATE, ERR_R_INTERNAL_ERROR);
            return 0;
        }
    }
    #ifndef OPENSSL_NO_CNSM
     if (s->s3->tmp.new_cipher->algorithm_mkey & (SSL_kECC | SSL_kSM2DH))
     {
         if (!ssl3_output_sm2_cert_chain(s, pkt, (s->s3->tmp.cert_req == 2) ? NULL : s->cert->key, &s->cert->pkeys[SSL_PKEY_ECC_ENC]))
         {
             return 0;
         }
     }
     else{
    #endif   
       
        
	    if (!ssl3_output_cert_chain(s, pkt,
	                                (s->s3->tmp.cert_req == 2) ? NULL
	                                                           : s->cert->key)) {
	        /* SSLfatal() already called */
	        return 0;
	    }
    #ifndef OPENSSL_NO_CNSM
  	}
    #endif

    if (SSL_IS_TLS13(s)
            && SSL_IS_FIRST_HANDSHAKE(s)
            && (!s->method->ssl3_enc->change_cipher_state(s,
                    SSL3_CC_HANDSHAKE | SSL3_CHANGE_CIPHER_CLIENT_WRITE))) {
        /*
         * This is a fatal error, which leaves enc_write_ctx in an inconsistent
         * state and thus ssl3_send_alert may crash.
         */
        SSLfatal(s, SSL_AD_NO_ALERT, SSL_F_TLS_CONSTRUCT_CLIENT_CERTIFICATE,
                 SSL_R_CANNOT_CHANGE_CIPHER);
        return 0;
    }

    return 1;
}

int ssl3_check_cert_and_algorithm(SSL *s)
{
    const SSL_CERT_LOOKUP *clu;
    size_t idx;
    long alg_k, alg_a;

    alg_k = s->s3->tmp.new_cipher->algorithm_mkey;
    alg_a = s->s3->tmp.new_cipher->algorithm_auth;

    /* we don't have a certificate */
    if (!(alg_a & SSL_aCERT))
        return 1;

    /* This is the passed certificate */
    clu = ssl_cert_lookup_by_pkey(X509_get0_pubkey(s->session->peer), &idx);

    /* Check certificate is recognised and suitable for cipher */
    if (clu == NULL || (alg_a & clu->amask) == 0) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_SSL3_CHECK_CERT_AND_ALGORITHM,
                 SSL_R_MISSING_SIGNING_CERT);
        return 0;
    }

#ifndef OPENSSL_NO_EC
    if (clu->amask & SSL_aECDSA) {
        if (ssl_check_srvr_ecc_cert_and_alg(s->session->peer, s))
            return 1;
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_SSL3_CHECK_CERT_AND_ALGORITHM, SSL_R_BAD_ECC_CERT);
        return 0;
    }
#endif
#ifndef OPENSSL_NO_RSA
    if (alg_k & (SSL_kRSA | SSL_kRSAPSK) && idx != SSL_PKEY_RSA) {
        SSLfatal(s, SSL_AD_HANDSHAKE_FAILURE,
                 SSL_F_SSL3_CHECK_CERT_AND_ALGORITHM,
                 SSL_R_MISSING_RSA_ENCRYPTING_CERT);
        return 0;
    }
#endif
#ifndef OPENSSL_NO_DH
    if ((alg_k & SSL_kDHE) && (s->s3->peer_tmp == NULL)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_SSL3_CHECK_CERT_AND_ALGORITHM,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }
#endif

    return 1;
}

#ifndef OPENSSL_NO_NEXTPROTONEG
int tls_construct_next_proto(SSL *s, WPACKET *pkt)
{
    printf("ddddddddddd tls_construct_next_proto \n");

    size_t len, padding_len;
    unsigned char *padding = NULL;

    len = s->ext.npn_len;
    padding_len = 32 - ((len + 2) % 32);

    if (!WPACKET_sub_memcpy_u8(pkt, s->ext.npn, len)
            || !WPACKET_sub_allocate_bytes_u8(pkt, padding_len, &padding)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_TLS_CONSTRUCT_NEXT_PROTO,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

    memset(padding, 0, padding_len);

    return 1;
}
int GMold_tls_construct_next_proto(SSL *s)
{
    unsigned int len, padding_len;
    unsigned char *d;

    len = s->ext.npn_len;
    padding_len = 32 - ((len + 2) % 32);
    d = (unsigned char *)s->init_buf->data;
    d[4] = len;
    memcpy(d + 5, s->ext.npn, len);
    d[5 + len] = padding_len;
    memset(d + 6 + len, 0, padding_len);
    *(d++) = SSL3_MT_NEXT_PROTO;
    l2n3(2 + len + padding_len, d);
    s->init_num = 4 + 2 + len + padding_len;
    s->init_off = 0;

    return 1;
}
#endif

MSG_PROCESS_RETURN tls_process_hello_req(SSL *s, PACKET *pkt)
{
    if (PACKET_remaining(pkt) > 0) {
        /* should contain no data */
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_HELLO_REQ,
                 SSL_R_LENGTH_MISMATCH);
        return MSG_PROCESS_ERROR;
    }

    if ((s->options & SSL_OP_NO_RENEGOTIATION)) {
        ssl3_send_alert(s, SSL3_AL_WARNING, SSL_AD_NO_RENEGOTIATION);
        return MSG_PROCESS_FINISHED_READING;
    }

    /*
     * This is a historical discrepancy (not in the RFC) maintained for
     * compatibility reasons. If a TLS client receives a HelloRequest it will
     * attempt an abbreviated handshake. However if a DTLS client receives a
     * HelloRequest it will do a full handshake. Either behaviour is reasonable
     * but doing one for TLS and another for DTLS is odd.
     */
    if (SSL_IS_DTLS(s))
        SSL_renegotiate(s);
    else
        SSL_renegotiate_abbreviated(s);

    return MSG_PROCESS_FINISHED_READING;
}

static MSG_PROCESS_RETURN tls_process_encrypted_extensions(SSL *s, PACKET *pkt)
{
    PACKET extensions;
    RAW_EXTENSION *rawexts = NULL;

    if (!PACKET_as_length_prefixed_2(pkt, &extensions)
            || PACKET_remaining(pkt) != 0) {
        SSLfatal(s, SSL_AD_DECODE_ERROR, SSL_F_TLS_PROCESS_ENCRYPTED_EXTENSIONS,
                 SSL_R_LENGTH_MISMATCH);
        goto err;
    }

    if (!tls_collect_extensions(s, &extensions,
                                SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS, &rawexts,
                                NULL, 1)
            || !tls_parse_all_extensions(s, SSL_EXT_TLS1_3_ENCRYPTED_EXTENSIONS,
                                         rawexts, NULL, 0, 1)) {
        /* SSLfatal() already called */
        goto err;
    }

    OPENSSL_free(rawexts);
    return MSG_PROCESS_CONTINUE_READING;

 err:
    OPENSSL_free(rawexts);
    return MSG_PROCESS_ERROR;
}

int ssl_do_client_cert_cb(SSL *s, X509 **px509, EVP_PKEY **ppkey)
{
    int i = 0;
#ifndef OPENSSL_NO_ENGINE
    if (s->ctx->client_cert_engine) {
        i = ENGINE_load_ssl_client_cert(s->ctx->client_cert_engine, s,
                                        SSL_get_client_CA_list(s),
                                        px509, ppkey, NULL, NULL, NULL);
        if (i != 0)
            return i;
    }
#endif
    if (s->ctx->client_cert_cb)
        i = s->ctx->client_cert_cb(s, px509, ppkey);
    return i;
}

static int ssl_cipher_list_to_bytes(SSL *s, STACK_OF(SSL_CIPHER) *sk, WPACKET *pkt)
{
    printf("ddddddddddd ssl_cipher_list_to_bytes \n");

    int i;
    size_t totlen = 0, len, maxlen, maxverok = 0;
    int empty_reneg_info_scsv = !s->renegotiate;

    /* Set disabled masks for this session */
    if (!ssl_set_client_disabled(s)) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_SSL_CIPHER_LIST_TO_BYTES,
                 SSL_R_NO_PROTOCOLS_AVAILABLE);
        return 0;
    }

    if (sk == NULL) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_SSL_CIPHER_LIST_TO_BYTES,
                 ERR_R_INTERNAL_ERROR);
        return 0;
    }

#ifdef OPENSSL_MAX_TLS1_2_CIPHER_LENGTH
# if OPENSSL_MAX_TLS1_2_CIPHER_LENGTH < 6
#  error Max cipher length too short
# endif
    /*
     * Some servers hang if client hello > 256 bytes as hack workaround
     * chop number of supported ciphers to keep it well below this if we
     * use TLS v1.2
     */
    if (TLS1_get_version(s) >= TLS1_2_VERSION)
        maxlen = OPENSSL_MAX_TLS1_2_CIPHER_LENGTH & ~1;
    else
#endif
        /* Maximum length that can be stored in 2 bytes. Length must be even */
        maxlen = 0xfffe;

    if (empty_reneg_info_scsv)
        maxlen -= 2;
    if (s->mode & SSL_MODE_SEND_FALLBACK_SCSV)
        maxlen -= 2;

    for (i = 0; i < sk_SSL_CIPHER_num(sk) && totlen < maxlen; i++) {
        const SSL_CIPHER *c;

        c = sk_SSL_CIPHER_value(sk, i);
        /* Skip disabled ciphers */
        if (ssl_cipher_disabled(s, c, SSL_SECOP_CIPHER_SUPPORTED, 0))
            continue;

        if (!s->method->put_cipher_by_char(c, pkt, &len)) {
            SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_SSL_CIPHER_LIST_TO_BYTES,
                     ERR_R_INTERNAL_ERROR);
            return 0;
        }

        /* Sanity check that the maximum version we offer has ciphers enabled */
        if (!maxverok) {
            if (SSL_IS_DTLS(s)) {
                if (DTLS_VERSION_GE(c->max_dtls, s->s3->tmp.max_ver)
                        && DTLS_VERSION_LE(c->min_dtls, s->s3->tmp.max_ver))
                    maxverok = 1;
            } else {
                if (c->max_tls >= s->s3->tmp.max_ver
                        && c->min_tls <= s->s3->tmp.max_ver)
                    maxverok = 1;
                #ifndef OPENSSL_NO_CNSM
                if (s->version == SM1_1_VERSION)
                    maxverok = 1;
                #endif
            }
        }

        totlen += len;
    }

    if (totlen == 0 || !maxverok) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR, SSL_F_SSL_CIPHER_LIST_TO_BYTES,
                 SSL_R_NO_CIPHERS_AVAILABLE);

        if (!maxverok)
            ERR_add_error_data(1, "No ciphers enabled for max supported "
                                  "SSL/TLS version");

        return 0;
    }

    if (totlen != 0) {
        if (empty_reneg_info_scsv) {
            static SSL_CIPHER scsv = {
                0, NULL, NULL, SSL3_CK_SCSV, 0, 0, 0, 0, 0, 0, 0, 0, 0
            };
            if (!s->method->put_cipher_by_char(&scsv, pkt, &len)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_SSL_CIPHER_LIST_TO_BYTES, ERR_R_INTERNAL_ERROR);
                return 0;
            }
        }
        if (s->mode & SSL_MODE_SEND_FALLBACK_SCSV) {
            static SSL_CIPHER scsv = {
                0, NULL, NULL, SSL3_CK_FALLBACK_SCSV, 0, 0, 0, 0, 0, 0, 0, 0, 0
            };
            if (!s->method->put_cipher_by_char(&scsv, pkt, &len)) {
                SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                         SSL_F_SSL_CIPHER_LIST_TO_BYTES, ERR_R_INTERNAL_ERROR);
                return 0;
            }
        }
    }

    return 1;
}

int GMold_ssl_cipher_list_to_bytes(SSL *s, STACK_OF(SSL_CIPHER) *sk, unsigned char *p)
{
    int i, j = 0;
    const SSL_CIPHER *c;
    unsigned char *q;
    int empty_reneg_info_scsv = !s->renegotiate;
    /* Set disabled masks for this session */
    GMold_ssl_set_client_disabled(s);

    if (sk == NULL)
    {
        printf("GMold_ssl_cipher_list_to_bytes sk == NULL\n");
        return (0);
    }
        
    q = p;
    for (i = 0; i < sk_SSL_CIPHER_num(sk); i++) {
        c = sk_SSL_CIPHER_value(sk, i);
        if (c->min_tls >= TLS1_3_VERSION)
            continue;
        /* Skip disabled ciphers */
        if (GMold_ssl_cipher_disabled(s, c, SSL_SECOP_CIPHER_SUPPORTED,1))
        {
            printf("GMold_ssl_cipher_list_to_bytes 0000000.5\n");
            continue;
        }
        j = GMold_ssl3_put_cipher_by_char(c, p);
        p += j;
    }
    /*
     * If p == q, no ciphers; caller indicates an error. Otherwise, add
     * applicable SCSVs.
     */
    if (p != q) {
        if (empty_reneg_info_scsv) {
            static SSL_CIPHER scsv = {
                0, NULL, NULL, SSL3_CK_SCSV, 0, 0, 0, 0, 0, 0, 0, 0, 0
            };
            j = GMold_ssl3_put_cipher_by_char(&scsv, p);
            p += j;
        }
        if (s->mode & SSL_MODE_SEND_FALLBACK_SCSV) {
            static SSL_CIPHER scsv = {
                0, NULL, NULL, SSL3_CK_FALLBACK_SCSV, 0, 0, 0, 0, 0, 0, 0, 0, 0
            };
            j = GMold_ssl3_put_cipher_by_char(&scsv, p);
            p += j;
        }
    }
    printf("GMold_ssl_cipher_list_to_bytes 33333333 (p - q):%d\n",(p - q));
    return (p - q);
}

int tls_construct_end_of_early_data(SSL *s, WPACKET *pkt)
{
    if (s->early_data_state != SSL_EARLY_DATA_WRITE_RETRY
            && s->early_data_state != SSL_EARLY_DATA_FINISHED_WRITING) {
        SSLfatal(s, SSL_AD_INTERNAL_ERROR,
                 SSL_F_TLS_CONSTRUCT_END_OF_EARLY_DATA,
                 ERR_R_SHOULD_NOT_HAVE_BEEN_CALLED);
        return 0;
    }

    s->early_data_state = SSL_EARLY_DATA_FINISHED_WRITING;
    return 1;
}
