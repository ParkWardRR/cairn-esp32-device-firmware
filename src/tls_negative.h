/*
 * A CA the server's certificate is NOT signed by, for proving that TLS
 * verification actually happens.
 *
 * A passing happy path is not evidence of verification. mbedTLS will complete a
 * handshake against any certificate if the configuration asks it to, and the
 * same mistake is documented against this modem family's own AT stack: a CA
 * uploaded without setting authmode is never checked, and the connection
 * succeeds anyway. That failure is invisible -- every log line reads as success
 * while the device will talk to anyone -- so it has to be tested from the
 * failing side.
 *
 * env:cairn-tlsneg pins this instead of the real CA and expects the handshake
 * to be REJECTED. If it connects, verification is off and the uplink's
 * confidentiality is worth nothing.
 *
 * This is a public certificate for a throwaway key, kept only so the test is
 * repeatable. It authorises nothing: the server refuses a client certificate
 * signed by it, which is itself one of the checked cases.
 */

#ifndef CAIRN_TLS_NEGATIVE_H
#define CAIRN_TLS_NEGATIVE_H

#define CAIRN_WRONG_CA_PEM \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIBezCCASGgAwIBAgIUb6VgxNBtFINvSfEAsMMFT9Nu75QwCgYIKoZIzj0EAwIw\n" \
    "EzERMA8GA1UEAwwIUm9ndWUgQ0EwHhcNMjYxMDA4MDUzNDI1WhcNMjcxMDA4MDUz\n" \
    "NDI1WjATMREwDwYDVQQDDAhSb2d1ZSBDQTBZMBMGByqGSM49AgEGCCqGSM49AwEH\n" \
    "A0IABDcw+q57g4m1yvujCRXhE8eMcliTBWc2NJo7u6mun/LuQhdkRcpwzsBx/I7u\n" \
    "fgPZWGTOiU8mdz6Gx/mIYgF72FWjUzBRMB0GA1UdDgQWBBQI+YIxCvNnFZzRmHpM\n" \
    "lfmGnkIjmDAfBgNVHSMEGDAWgBQI+YIxCvNnFZzRmHpMlfmGnkIjmDAPBgNVHRMB\n" \
    "Af8EBTADAQH/MAoGCCqGSM49BAMCA0gAMEUCIGSHaqp2BVDIBHmTDrhc75G+FbDU\n" \
    "v2S1HSSiscmJ5IKQAiEAyxU4puhjSgS2F+f7uidGo6yWlPXjUFN/yG3y7eFFN4A=\n" \
    "-----END CERTIFICATE-----\n"

#endif /* CAIRN_TLS_NEGATIVE_H */
