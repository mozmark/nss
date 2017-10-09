/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is PRIVATE to SSL.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "pk11func.h"
#include "ssl.h"
#include "sslt.h"
#include "sslimpl.h"
#include "selfencrypt.h"
#include "tls13con.h"
#include "tls13err.h"
#include "tls13hashstate.h"

/*
 * The cookie is structured as a self-encrypted structure with the
 * inner value being.
 *
 * struct {
 *     uint8 indicator = 0xff;            // To disambiguate from tickets.
 *     uint16 cipherSuite;                // Selected cipher suite.
 *     uint16 keyShare;                   // Requested key share group (0=none)
 *     opaque applicationToken<0..65535>; // Application token
 *     opaque ch_hash[rest_of_buffer];    // H(ClientHello)
 * } CookieInner;
 */
SECStatus
tls13_MakeHrrCookie(sslSocket *ss, const sslNamedGroupDef *selectedGroup,
                    const PRUint8 *appToken, unsigned int appTokenLen,
                    PRUint8 *buf, unsigned int *len, unsigned int maxlen)
{
    SECStatus rv;
    SSL3Hashes hashes;
    PRUint8 cookie[1024];
    sslBuffer cookieBuf = SSL_BUFFER(cookie);
    static const PRUint8 indicator = 0xff;

    /* Encode header. */
    rv = sslBuffer_Append(&cookieBuf, &indicator, 1);
    if (rv != SECSuccess) {
        return SECFailure;
    }
    rv = sslBuffer_AppendNumber(&cookieBuf, ss->ssl3.hs.cipher_suite, 2);
    if (rv != SECSuccess) {
        return SECFailure;
    }
    rv = sslBuffer_AppendNumber(&cookieBuf,
                                selectedGroup ? selectedGroup->name : 0, 2);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    /* Application token. */
    rv = sslBuffer_AppendVariable(&cookieBuf, appToken, appTokenLen, 2);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    /* Compute and encode hashes. */
    rv = tls13_ComputeHandshakeHashes(ss, &hashes);
    if (rv != SECSuccess) {
        return SECFailure;
    }
    rv = sslBuffer_Append(&cookieBuf, hashes.u.raw, hashes.len);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    /* Encrypt right into the buffer. */
    rv = ssl_SelfEncryptProtect(ss, cookieBuf.buf, cookieBuf.len,
                                buf, len, maxlen);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    return SECSuccess;
}

/* Recover the hash state from the cookie. */
SECStatus
tls13_RecoverHashState(sslSocket *ss,
                       unsigned char *cookie, unsigned int cookieLen,
                       ssl3CipherSuite *previousCipherSuite,
                       const sslNamedGroupDef **previousGroup)
{
    SECStatus rv;
    unsigned char plaintext[1024];
    SECItem ptItem = { siBuffer, plaintext, 0 };
    sslBuffer messageBuf = SSL_BUFFER_EMPTY;
    PRUint32 sentinel;
    PRUint32 cipherSuite;
    PRUint32 group;
    const sslNamedGroupDef *selectedGroup;
    PRUint32 appTokenLen;
    PRUint8 *appToken;

    rv = ssl_SelfEncryptUnprotect(ss, cookie, cookieLen,
                                  ptItem.data, &ptItem.len, sizeof(plaintext));
    if (rv != SECSuccess) {
        return SECFailure;
    }

    /* Should start with 0xff. */
    rv = ssl3_ConsumeNumberFromItem(&ptItem, &sentinel, 1);
    if ((rv != SECSuccess) || (sentinel != 0xff)) {
        FATAL_ERROR(ss, SSL_ERROR_RX_MALFORMED_CLIENT_HELLO, illegal_parameter);
        return SECFailure;
    }
    /* The cipher suite should be the same or there are some shenanigans. */
    rv = ssl3_ConsumeNumberFromItem(&ptItem, &cipherSuite, 2);
    if (rv != SECSuccess) {
        FATAL_ERROR(ss, SSL_ERROR_RX_MALFORMED_CLIENT_HELLO, illegal_parameter);
        return SECFailure;
    }

    /* The named group, if any. */
    rv = ssl3_ConsumeNumberFromItem(&ptItem, &group, 2);
    if (rv != SECSuccess) {
        FATAL_ERROR(ss, SSL_ERROR_RX_MALFORMED_CLIENT_HELLO, illegal_parameter);
        return SECFailure;
    }
    selectedGroup = ssl_LookupNamedGroup(group);

    /* Application token. */
    PORT_Assert(ss->xtnData.applicationToken.len == 0);
    rv = ssl3_ConsumeNumberFromItem(&ptItem, &appTokenLen, 2);
    if (rv != SECSuccess) {
        FATAL_ERROR(ss, SSL_ERROR_RX_MALFORMED_CLIENT_HELLO, illegal_parameter);
        return SECFailure;
    }
    if (SECITEM_AllocItem(NULL, &ss->xtnData.applicationToken,
                          appTokenLen) == NULL) {
        FATAL_ERROR(ss, PORT_GetError(), internal_error);
        return SECFailure;
    }
    ss->xtnData.applicationToken.len = appTokenLen;
    rv = ssl3_ConsumeFromItem(&ptItem, &appToken, appTokenLen);
    if (rv != SECSuccess) {
        FATAL_ERROR(ss, SSL_ERROR_RX_MALFORMED_CLIENT_HELLO, illegal_parameter);
        return SECFailure;
    }
    PORT_Memcpy(ss->xtnData.applicationToken.data, appToken, appTokenLen);

    /* The remainder is the hash. */
    if (ptItem.len != tls13_GetHashSize(ss)) {
        FATAL_ERROR(ss, SSL_ERROR_RX_MALFORMED_CLIENT_HELLO, illegal_parameter);
        return SECFailure;
    }

    /* Now reinject the message. */
    SSL_ASSERT_HASHES_EMPTY(ss);
    rv = ssl_HashHandshakeMessageInt(ss, ssl_hs_message_hash, 0,
                                     ptItem.data, ptItem.len);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    /* And finally reinject the HRR. */
    rv = tls13_ConstructHelloRetryRequest(ss, cipherSuite,
                                          selectedGroup,
                                          cookie, cookieLen,
                                          &messageBuf);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    rv = ssl_HashHandshakeMessageInt(ss, ssl_hs_hello_retry_request, 0,
                                     SSL_BUFFER_BASE(&messageBuf),
                                     SSL_BUFFER_LEN(&messageBuf));
    sslBuffer_Clear(&messageBuf);
    if (rv != SECSuccess) {
        return SECFailure;
    }

    *previousCipherSuite = cipherSuite;
    *previousGroup = selectedGroup;
    return SECSuccess;
}
