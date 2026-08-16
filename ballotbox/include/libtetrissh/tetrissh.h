#ifndef TETRISSH_H
#define TETRISSH_H

#include <stdint.h>
#include <openssl/x509.h>
#include <openssl/evp.h>

/* Frame limit: applies to the encrypted wire frame (4-byte BE length prefix
 * + ciphertext token), enforced identically on send and recv. Max usable
 * plaintext per frame is slightly smaller (IV 16 + HMAC 32 + CBC padding). */
#define SESSION_MAX_FRAME (64 * 1024)

/* Return codes — all functions return SESSION_OK (0) or a negative error */
typedef enum
{
    SESSION_OK = 0,
    SESSION_ERR_IO = -1,      /* socket read/write failed or peer closed   */
    SESSION_ERR_AUTH = -2,    /* cert verification or signature check fail */
    SESSION_ERR_CRYPTO = -3,  /* encrypt/decrypt failure (common.c layer)  */
    SESSION_ERR_TOOBIG = -4,  /* frame > SESSION_MAX_FRAME. On recv the
                                 stream is out of sync: the session is
                                 marked dead and the caller must close the
                                 fd (app sends 413 first if it can). On
                                 send nothing was written.                 */
    SESSION_ERR_PROTO = -5,   /* malformed handshake message, or a traffic
                                 call before a successful handshake.
                                 The session is dead: close the fd.         */
    SESSION_ERR_NOSPACE = -6, /* recv: caller buffer too small; that frame
                                 is discarded, session stays usable        */
} session_err_t;

typedef struct
{
    int fd;          /* connected TCP socket (owned by caller) */
    uint8_t key[32]; /* session key: HMAC-SHA256 key (16) || AES-128 key
                        (16) — layout fixed by common.c; size checked by
                        _Static_assert in tetrissh.c */
    int established; /* 1 after successful handshake */
    /* NO replay defence lives here. Frames carry no counter, so a recorded
       frame replays cleanly: it decrypts, its HMAC verifies, and recv hands
       it to the caller as new. Any replay guarantee must come from a layer
       above this one. */
    int recv_dead;     /* 1 once the RECEIVE stream is out of sync. Send
                          still works: the desync is in what we are being
                          fed, not in what we write. That is the window an
                          app uses to answer 413 before closing. */
} session_t;

/* The library is silent by default; compile with -DTSSH_DEBUG for handshake
 * progress + error messages on stderr. It never opens or closes the fd —
 * connect()/accept()/close() are the application's job. */

/* --- Handshake ----------------------------------------------------------- */

/* Client side. fd = already-connected socket. ca_path = path to the trusted
 * CA certificate (PEM) used to verify the server's X.509 cert. Runs: send
 * nonce → verify RSA-PSS sig + cert chain → generate session key → send it
 * RSA-OAEP-wrapped. SESSION_ERR_AUTH on forged/self-signed cert or bad
 * signature. *s is zeroed on entry and filled only on success. */
int session_connect(session_t *s, int fd, const char *ca_path);

/* Server side. priv = server RSA private key, cert_path = path to the
 * server's X.509 certificate (PEM). Runs: receive nonce → RSA-PSS-sign it,
 * send sig + cert → receive and unwrap the client's session key.
 * *s is zeroed on entry and filled only on success. */
int session_accept(session_t *s, int fd, EVP_PKEY *priv, const char *cert_path);

/* --- Traffic (only valid after handshake) -------------------------------- */

/* Encrypt buf with the session key, prepend 4-byte BE length, write all.
 * The whole frame budget is the caller's — nothing is prepended to the
 * plaintext. Resulting wire frame > SESSION_MAX_FRAME → SESSION_ERR_TOOBIG
 * (nothing written). Handles partial writes internally. */
int session_send(session_t *s, const uint8_t *buf, uint32_t len);

/* Read one frame: 4-byte BE length, then exactly that many ciphertext
 * bytes; decrypt into buf. On entry *len = capacity of buf; on success
 * *len = plaintext length. Advertised length > SESSION_MAX_FRAME →
 * SESSION_ERR_TOOBIG and the RECEIVE side is marked dead: no further recv
 * will succeed, but one last session_send still will, so the app can answer
 * 413 before closing the fd. Plaintext larger than the caller's buffer →
 * SESSION_ERR_NOSPACE (frame discarded, session usable). Handles short
 * reads internally. */
int session_recv(session_t *s, uint8_t *buf, uint32_t *len);

/* --- Teardown ------------------------------------------------------------ */

/* Zero key material, mark session dead. Does NOT close fd (caller owns it). */
void session_close(session_t *s);

#endif /* TETRISSH_H */
