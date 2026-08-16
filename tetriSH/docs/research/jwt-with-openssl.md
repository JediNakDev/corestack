# Minting and verifying a JWT with only OpenSSL

Research note for [issue #45](https://github.com/JediNakDev/tetriSH/issues/45).
This is a facts-gathering note.
It does **not** choose a scheme - see [Open questions](#open-questions-what-this-note-deliberately-does-not-decide) at the end.

## 0. Scope: which OpenSSL this project actually links

The Makefile resolves OpenSSL through Homebrew and adds it to both the include and library search paths (`Makefile:5-7`).
Every OpenSSL-linked binary gets `-lssl -lcrypto` from the global `LDLIBS` (`Makefile:4`).

On this machine that prefix is `/opt/homebrew/opt/openssl@3`, and the headers there declare:

```
# define OPENSSL_VERSION_STR "3.6.3"
# define OPENSSL_FULL_VERSION_STR "3.6.3"
```

`openssl version` on this machine reports `OpenSSL 3.6.3 9 Jun 2026`, `platform: darwin64-arm64-cc`.
`README.md:99` states the build "expects OpenSSL from Homebrew at `/opt/homebrew/opt/openssl@3`", and `README.md:61` documents `brew install openssl@3`.

**Every API claim below is taken from the OpenSSL 3.6.3 man pages installed with that exact library** (`MANPATH=/opt/homebrew/opt/openssl@3/share/man man 3 <page>`), which are the same pages published at [docs.openssl.org](https://docs.openssl.org/) for the 3.x series.
Each man page footer confirms the version, e.g. `3.6.3   2026-06-09   EVP_ENCODEINIT(3ssl)`.

Consequence: this is a 3.x-only answer.
Nothing here needs to work around 1.1.1, and the 3.0 deprecations below are live concerns rather than future ones.

## 1. base64url (unpadded)

### 1.1 What base64url is

RFC 7515 defines the encoding JWT/JWS uses, in [§2 Terminology](https://datatracker.ietf.org/doc/html/rfc7515#section-2):

> "Base64 encoding using the URL- and filename-safe character set defined in Section 5 of RFC 4648, with all trailing '=' characters omitted (as permitted by Section 3.2) and without the inclusion of any line breaks, whitespace, or other additional characters."

So there are exactly three deltas from standard base64.

**Alphabet.**
[RFC 4648 §5](https://datatracker.ietf.org/doc/html/rfc4648#section-5) says the URL-safe alphabet is "technically identical to the previous one, except for the 62:nd and 63:rd alphabet character": `+` becomes `-` and `/` becomes `_`.
This is a two-character substitution over the output, in both directions.

**Padding.**
[RFC 4648 §3.2](https://datatracker.ietf.org/doc/html/rfc4648#section-3.2) makes padding the default: "Implementations MUST include appropriate pad characters at the end of encoded data unless the specification referring to this document explicitly states otherwise."
RFC 7515 §2 is exactly such a specification, and states otherwise: all trailing `=` are omitted.
A JWT segment is therefore 4n, 4n+2 or 4n+3 characters long - never 4n+1.

**No whitespace.**
No line breaks anywhere, per RFC 7515 §2 above.
[RFC 7519 §7.2](https://datatracker.ietf.org/doc/html/rfc7519#section-7.2) step 3 repeats it for the verifier: base64url decode the header "following the restriction that no line breaks, whitespace, or other additional characters have been used."

**Strictness on decode.**
[RFC 4648 §3.3](https://datatracker.ietf.org/doc/html/rfc4648#section-3.3): "Implementations MUST reject the encoded data if it contains characters outside the base alphabet ... unless the specification referring to this document explicitly states otherwise."
RFC 7515 does not relax this, so a JWT decoder should reject `+`, `/`, `=`, CR, LF and space rather than tolerate them.

### 1.2 Is there an OpenSSL base64url primitive?

**No.**
OpenSSL 3.6.3 ships only standard base64.
`EVP_EncodeInit(3ssl)` describes the encoding as using "the characters A-Z, a-z, 0-9, '+' and '/'", and there is no flag, BIO or `OSSL_PARAM` anywhere in the EVP encode routines or in `BIO_f_base64(3ssl)` that selects the URL-safe alphabet or suppresses padding.

Confirmed absent by grep: nothing in `src/`, `include/` or `tests/` mentions `base64`, `EVP_Encode*`, `EVP_Decode*` or `BIO_f_base64`.

So base64url is hand-rolled either way.
The only choice is what to hand-roll it *over*.

### 1.3 Gotchas of each OpenSSL base64 API

**`EVP_EncodeBlock(unsigned char *t, const unsigned char *f, int n)`** - the usable one.

From `EVP_EncodeInit(3ssl)`:

> "encodes a full block of input data in f and of length n and stores it in t. For every 3 bytes of input provided 4 bytes of output data will be produced. If n is not divisible by 3 then the block is encoded as a final block of data and the output is padded such that it is always divisible by 4. Additionally a NUL terminator character will be added. For example if 16 bytes of input data is provided then 24 bytes of encoded data is created plus 1 byte for a NUL terminator (i.e. 25 bytes in total). The length of the data generated *without* the NUL terminator is returned from the function."

Practical consequences:
- It emits **no newlines**. This is the property that makes it, and not `EVP_EncodeUpdate`, the right primitive here.
- Output buffer must be `4 * ((n + 2) / 3) + 1` bytes (the `+1` for the NUL it always writes).
- It pads. Post-processing is: translate `+`->`-`, `/`->`_`, then truncate at the first `=`.
- `n` is an `int`, so inputs are bounded by `INT_MAX`. A JWT segment never approaches this, but the cast from `size_t` should be checked rather than assumed.

**`EVP_EncodeUpdate` / `EVP_EncodeFinal`** - wrong tool, avoid.

Same man page: "Encoding of binary data is performed in blocks of 48 input bytes ... For each 48 byte input block encoded 64 bytes of base64 data is output plus an additional newline character (i.e. 65 bytes in total)."
And of the final block: "Similarly a newline character will also be output."
Newlines are unconditional; there is no flag to turn them off in this API.
You would have to strip them afterwards, which is strictly more work than using `EVP_EncodeBlock`.

**`EVP_DecodeBlock(unsigned char *t, const unsigned char *f, int n)`** - usable, but the padding behaviour is the trap.

From `EVP_EncodeInit(3ssl)`:

> "Any leading whitespace will be trimmed as will any trailing whitespace, newlines, carriage returns or EOF characters. Internal whitespace MUST NOT be present. After trimming the data in f MUST consist entirely of valid base64 characters or padding (only at the tail of the input) and its length MUST be divisible by 4. For every 4 input bytes exactly 3 output bytes will be produced. **Padding bytes (=) (even if internal) are decoded to 6 zero bits, the caller is responsible for taking trailing padding into account, by ignoring as many bytes at the tail of the returned output.** EVP_DecodeBlock() will return the length of the data decoded or -1 on error."

Three separate gotchas fall out of this:

1. **Input length MUST be divisible by 4.** JWT segments are unpadded, so they usually are not. The caller must re-pad with `=` to the next multiple of 4 before calling. A remainder of 1 is impossible in valid base64 and should be rejected outright.
2. **The return value is always a multiple of 3 and includes the padding-derived zero bytes.** `EVP_DecodeBlock` does not tell you the true plaintext length. The caller must subtract the number of `=` characters it added. Trusting the return value directly is the classic way to get one or two spurious trailing NUL bytes appended to a decoded JWT payload - which then breaks JSON parsing or, worse, silently succeeds.
3. **It tolerates leading/trailing whitespace** (trimmed) that RFC 7515 §2 forbids. If you want RFC-conformant strictness you must validate the character set yourself before calling, which - since you already have to walk the string to translate `-`/`_` back to `+`/`/` - is close to free.

**`EVP_DecodeUpdate` / `EVP_DecodeFinal`** - also wrong tool, and version-sensitive.

Same man page: "Any whitespace, newline or carriage return characters are ignored", and "For compatibility with PEM, the - (hyphen) character is treated as a soft end-of-input".
That hyphen rule is disqualifying on its own: `-` is a *valid base64url data character*, and this API would treat it as end-of-input.

Note also the HISTORY section: "The EVP_DecodeUpdate() function was fixed in OpenSSL 3.5, so now it produces the number of bytes specified in outl* and does not decode padding bytes (=) to 6 zero bits."
We link 3.6.3 so we would get the fixed behaviour, but the hyphen problem is independent of that and remains.

**`BIO_f_base64`** - wrong tool, newline handling is the reason.

From `BIO_f_base64(3ssl)`: "For writing, by default output is divided to lines of length 64 characters and there is a newline at the end of output. This behavior can be changed with BIO_FLAGS_BASE64_NO_NL flag."
And: "The flag BIO_FLAGS_BASE64_NO_NL can be set with BIO_set_flags(). For writing, it causes all data to be written on one line without newline at the end. For reading, it removes all expectations on newlines in the input data."

So it is *fixable* with `BIO_FLAGS_BASE64_NO_NL`, but it costs a BIO chain, a `BIO_push`, a `BIO_flush` (the man page: "BIO_flush() on a base64 BIO that is being written through is used to signal that no more data is to be encoded"), and a `BIO_free_all`, plus the same alphabet and padding fixups as `EVP_EncodeBlock`.
It also inherits the soft end-of-input `-` behaviour on read: "Decoding stops when base64 padding is encountered, a soft end-of-input character (-, see EVP_DecodeUpdate(3)) occurs..." - the same disqualifier as above.
And the man page's own NOTES: "Because of the format of base64 encoding the end of the encoded block cannot always be reliably determined."

### 1.4 Summary of the base64url shape

Encode: `EVP_EncodeBlock` -> translate two chars -> strip `=`.
Decode: validate alphabet -> translate two chars -> re-pad to a multiple of 4 -> `EVP_DecodeBlock` -> subtract the added padding count from the returned length.

Rough size: two functions, on the order of 40-60 lines including the length arithmetic and error paths.
Alternatively a self-contained table-driven base64url with no OpenSSL involvement is a comparable amount of code and avoids gotchas 1 and 2 entirely.
This note does not pick between those.

## 2. HS256 vs RS256 in OpenSSL 3.6.3

### 2.1 What the specs require of each

[RFC 7518 §3.1](https://datatracker.ietf.org/doc/html/rfc7518#section-3) lists `HS256` (HMAC using SHA-256) as **Required** and `RS256` (RSASSA-PKCS1-v1_5 using SHA-256) as **Recommended**.

Key size floors are hard requirements, not advice:
- HS256, [RFC 7518 §3.2](https://datatracker.ietf.org/doc/html/rfc7518#section-3): "A key of the same size as the hash output (for instance, 256 bits for 'HS256') or larger MUST be used with this algorithm."
- RS256, [RFC 7518 §3.3](https://datatracker.ietf.org/doc/html/rfc7518#section-3): "A key of size 2048 bits or larger MUST be used with these algorithms."

**This matters for the repo.** The existing RSA private key at `auth/private_key.pem` is a **1024-bit** key (`openssl rsa -in auth/private_key.pem -noout -text` reports `Private-Key: (1024 bit, 2 primes)`).
Reusing it for RS256 as-is would violate RFC 7518 §3.3.
RS256 here therefore implies either regenerating the server key at 2048 bits or minting JWTs with a second, separate key.
`auth/generate_keys.sh` is a **zero-byte file**, so there is no script in the repo that documents how the current key was produced.

RFC 2104 backs the HS256 floor independently.
[RFC 2104 §3](https://datatracker.ietf.org/doc/html/rfc2104#section-3) sets the minimum recommended key length at L bytes (the hash output length, 32 for SHA-256) and notes keys longer than the block size B "are first hashed using H".
The construction itself, [§2](https://datatracker.ietf.org/doc/html/rfc2104#section-2), is `H(K XOR opad, H(K XOR ipad, text))`.

### 2.2 The thing being signed

[RFC 7515 §2](https://datatracker.ietf.org/doc/html/rfc7515#section-2) defines the JWS Signing Input:

> "ASCII(BASE64URL(UTF8(JWS Protected Header)) || '.' || BASE64URL(JWS Payload))"

Identical for both algorithms.
Only the signature function over those bytes differs.
This is worth stating explicitly: the base64url layer, the JSON layer, the claim checking and the token assembly are shared, so the HS256-vs-RS256 decision affects perhaps 30 lines out of the whole thing.

### 2.3 HS256 with `HMAC()`

```c
unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len,
                    const unsigned char *data, size_t data_len,
                    unsigned char *md, unsigned int *md_len);
```

**`HMAC()` is not deprecated.**
This is worth stating plainly because the surrounding API is.
The HISTORY section of `HMAC(3ssl)` reads: "All functions except for HMAC() were deprecated in OpenSSL 3.0."
The deprecated set - listed under "The following functions have been deprecated since OpenSSL 3.0" in the SYNOPSIS - is `HMAC_CTX_new`, `HMAC_CTX_reset`, `HMAC_Init_ex`, `HMAC_Update`, `HMAC_Final`, `HMAC_CTX_free`, `HMAC_CTX_copy`, `HMAC_CTX_set_flags`, `HMAC_CTX_get_md`, `HMAC_size` (and `HMAC_Init`, deprecated since 1.1.0).
The DESCRIPTION says of those: "All of the functions described below are deprecated. Applications should instead use EVP_MAC_CTX_new(3) ... or the 'quick' single-shot MAC function EVP_Q_mac(3)."
That sentence is about the incremental `HMAC_CTX_*` family, not about the one-shot `HMAC()`.

The one caveat the man page does attach to `HMAC()`: "HMAC() uses the default OSSL_LIB_CTX. Use EVP_Q_mac(3) instead if a library context is required."
This project uses the default library context everywhere, so that caveat does not bind.

Two further notes from `HMAC(3ssl)`: `md` "must have space for the output of the hash function, which is no more than EVP_MAX_MD_SIZE bytes", and "passing a NULL value for md to use the static array is not thread safe" - always pass a caller-owned 32-byte buffer.
RETURN VALUES: "HMAC() returns a pointer to the message authentication code or NULL if an error occurred."

**Cost.** One call to sign, one call plus one `CRYPTO_memcmp` to verify.
No context object, no allocation, no cleanup path, no error branch beyond a NULL check.
This is the cheapest correct option in the library, and it is already in use in this repo (see §4).

### 2.4 HS256 with `EVP_MAC` / `EVP_Q_mac`

The 3.x provider API. `EVP_MAC(3ssl)` HISTORY: "These functions were added in OpenSSL 3.0."

The single-shot form is `EVP_Q_mac`:

```c
unsigned char *EVP_Q_mac(OSSL_LIB_CTX *libctx, const char *name, const char *propq,
                         const char *subalg, const OSSL_PARAM *params,
                         const void *key, size_t keylen,
                         const unsigned char *data, size_t datalen,
                         unsigned char *out, size_t outsize, size_t *outlen);
```

Per the man page it "computes the message authentication code of data with length datalen using the MAC algorithm name and the key key with length keylen ... If out is not NULL, it places the result in the memory pointed at by out, but only if outsize is sufficient (otherwise no computation is made). If out is NULL, it allocates and uses a buffer of suitable length, which will be returned on success and must be freed by the caller."

The incremental form is `EVP_MAC_fetch` -> `EVP_MAC_CTX_new` -> `EVP_MAC_init` -> `EVP_MAC_update` -> `EVP_MAC_final`, with `EVP_MAC_CTX_free` and `EVP_MAC_free` on every exit path.
`EVP_MAC_fetch` returns a value that "must eventually be freed with EVP_MAC_free(3)".

**Cost.** `EVP_Q_mac` is roughly on par with `HMAC()` for a one-shot (one extra `OSSL_PARAM` array for the `digest` subalg, and a `size_t` outlen instead of `unsigned int`).
The full `EVP_MAC_*` path is materially more code: a fetch, two objects to own, and five error paths.
It buys nothing for a fixed-algorithm, single-buffer, default-libctx use like a JWT.

**Deprecation status: neither is deprecated.**
`HMAC()` survives; `EVP_MAC`/`EVP_Q_mac` are the newer 3.0 API.
There is no forced migration here.

### 2.5 RS256 with `EVP_DigestSign` / `EVP_DigestVerify`

Sign, from `EVP_DigestSignInit(3ssl)`:

```c
int EVP_DigestSignInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pctx,
                       const EVP_MD *type, ENGINE *e, EVP_PKEY *pkey);
int EVP_DigestSign(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen,
                   const unsigned char *tbs, size_t tbslen);
```

The man page's algorithm table confirms RSA works with SHA-256 ("All other RSA padding types: Support SHA1, SHA224, SHA256, ..."), and confirms the length-probe idiom: "Unless sig is NULL EVP_DigestSignFinal() signs the data in ctx and places the signature in sig. Otherwise the maximum necessary size of the output buffer is written to the siglen parameter."
So RS256 signing needs two `EVP_DigestSignFinal` calls (or `EVP_DigestSign` with a caller-sized buffer), plus a malloc between them.

Verify, from `EVP_DigestVerifyInit(3ssl)`:

```c
int EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen,
                     const unsigned char *tbs, size_t tbslen);
```

RETURN VALUES: "EVP_DigestVerifyFinal() and EVP_DigestVerify() return 1 for success; any other value indicates failure. A return value of zero indicates that the signature did not verify successfully (that is, tbs did not match the original data or the signature had an invalid form), while other values indicate a more serious error."

Note the trap in that return convention: `if (EVP_DigestVerify(...))` is a **bug**, because a negative error return is truthy in C.
The check must be `== 1`.
The repo already gets this right - see §4.

**Padding.** RS256 is PKCS#1 v1.5, which is `EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING)`.
This is *not* what the repo's existing `sign_message_pss` does - that sets `RSA_PKCS1_PSS_PADDING` (`src/libtetrissh/common.c:251`), which is the `PS256` JWA algorithm, listed as Optional in [RFC 7518 §3.1](https://datatracker.ietf.org/doc/html/rfc7518#section-3).
The existing helpers are therefore *structurally* reusable but not directly reusable for RS256 without changing the padding mode.
PKCS#1 v1.5 is the default padding for RSA in OpenSSL, so an RS256 path can simply omit the padding call - but that means it cannot share code with the PSS helpers as they stand.

**Cost.** An `EVP_MD_CTX` to allocate and free, an `EVP_PKEY` to load and free, a two-pass length probe and a malloc on the sign side, and a `== 1` comparison on the verify side.
Call it 3-4x the line count of the HS256 path, with real cleanup obligations on every early return.
Verification also produces a much larger signature: 256 bytes for a 2048-bit key vs 32 bytes for HS256, which is ~342 vs ~43 base64url characters in the token.

### 2.6 Side-by-side

| | HS256 | RS256 |
|---|---|---|
| Primary API | `HMAC()` (`openssl/hmac.h`) | `EVP_DigestSign`/`EVP_DigestVerify` (`openssl/evp.h`) |
| Deprecated in 3.x? | No (`HMAC()` is the sole survivor of that header) | No |
| 3.x alternative | `EVP_Q_mac` / `EVP_MAC_*` | none needed |
| Objects to own/free | none | `EVP_MD_CTX`, `EVP_PKEY` |
| Sign call shape | 1 call | init + (probe, malloc, final) |
| Verify call shape | 1 call + `CRYPTO_memcmp` | init + `== 1` check |
| Key material | 32+ byte shared secret ([RFC 7518 §3.2](https://datatracker.ietf.org/doc/html/rfc7518#section-3)) | 2048+ bit RSA ([RFC 7518 §3.3](https://datatracker.ietf.org/doc/html/rfc7518#section-3)) |
| Repo key usable today | no key exists; `RAND_bytes` can mint one | **no** - `auth/private_key.pem` is 1024-bit |
| Signature size | 32 bytes | 256 bytes (2048-bit key) |
| Verifier needs the signing secret? | yes | no (public key only) |

The last row is the only real architectural difference, and it only matters if some party other than the issuer needs to verify.

## 3. Pitfalls a correct verifier must handle

### 3.1 `alg: none`

[RFC 8725 §2.1](https://datatracker.ietf.org/doc/html/rfc8725#section-2.1) describes it directly:

> "The algorithm can be changed to 'none' by an attacker, and some libraries would trust this value and 'validate' the JWT without checking any signature."

**What a correct verifier does:** never dispatch on the token's own `alg`.
[RFC 8725 §3.1](https://datatracker.ietf.org/doc/html/rfc8725#section-3.1) is the normative rule: "Libraries MUST enable the caller to specify a supported set of algorithms and MUST NOT use any other algorithms when performing cryptographic operations."

For a single-algorithm application the cheapest correct implementation is stricter still: the verifier hard-codes one algorithm, and rejects the token unless the header's `alg` string equals that exact value.
There is no code path that reads `alg` and selects a function from it, so `none` has nothing to select.

`alg` cannot simply be ignored either - [RFC 7515 §4.1.1](https://datatracker.ietf.org/doc/html/rfc7515#section-4.1.1): "This Header Parameter MUST be present and MUST be understood and processed by implementations."
So: require it, compare it, do not dispatch on it.

Note the comparison is **case-sensitive** - RFC 7515 §4.1.1 calls the value "a case-sensitive ASCII string". `hs256` must be rejected.

### 3.2 HMAC/RSA algorithm confusion

[RFC 8725 §2.1](https://datatracker.ietf.org/doc/html/rfc8725#section-2.1), the second attack in that section: an attacker changes `RS256` to `HS256`, and "libraries would try to validate the signature using HMAC-SHA256 and using the RSA public key as the HMAC shared secret".
Since the RSA public key is public, the attacker can then mint arbitrary valid-looking tokens.

**What a correct verifier does:** the same fix as §3.1 - a hard-coded algorithm and no dispatch - closes this too.
[RFC 8725 §3.1](https://datatracker.ietf.org/doc/html/rfc8725#section-3.1) adds the general principle: "each key MUST be used with exactly one algorithm, and this MUST be checked when the cryptographic operation is performed."

Concretely, that means the HMAC secret must not be any key that is used for anything else.
Deriving the JWT HMAC key from, say, a session key or a PEM file's bytes would violate this.

### 3.3 Constant-time signature comparison

This applies to **HS256 only**.
[RFC 7518 §3.2](https://datatracker.ietf.org/doc/html/rfc7518#section-3) requires the computed MAC be compared to the received one "in a constant-time manner to thwart timing attacks".
RFC 8725 does not restate this; RFC 7518 §3.2 is the section that owns the requirement.

OpenSSL's primitive is `CRYPTO_memcmp` (`openssl/crypto.h`).
From `CRYPTO_memcmp(3ssl)`: it "compares the len bytes pointed to by a and b for equality. It takes an amount of time dependent on len, but independent of the contents of the memory regions pointed to by a and b."
RETURN VALUES: "returns 0 if the memory regions are equal and nonzero otherwise."
NOTES: "Unlike memcmp(2), this function cannot be used to order the two memory regions as the return value when they differ is undefined, other than being nonzero."

Two implementation notes:
- Compare the **raw 32 bytes**, after base64url-decoding the received signature - not the base64url strings. Comparing strings works but leaks through the length check and is not what the RFC describes.
- Check the decoded signature length is exactly 32 **before** comparing, and reject otherwise. `CRYPTO_memcmp` takes a single `len` and will happily read past a short buffer.

For RS256, `EVP_DigestVerify` does the comparison internally and no `CRYPTO_memcmp` is needed - but see §2.5 on the `== 1` return check.

### 3.4 Expiry and clock handling

[RFC 7519 §2](https://datatracker.ietf.org/doc/html/rfc7519#section-2) defines NumericDate as seconds since 1970-01-01T00:00:00Z UTC ignoring leap seconds, and explicitly permits non-integer values.
A parser that assumes an integer will mis-handle a conformant `1.5`-style value from another issuer; for a self-issued token this is a non-issue, but a verifier that only ever accepts its own tokens should say so rather than assume.

- **`exp`**, [RFC 7519 §4.1.4](https://datatracker.ietf.org/doc/html/rfc7519#section-4.1.4): "identifies the expiration time on or after which the JWT MUST NOT be accepted for processing." Note "on or after" - the comparison is `now >= exp` rejects, not `now > exp`.
- **`nbf`**, [RFC 7519 §4.1.5](https://datatracker.ietf.org/doc/html/rfc7519#section-4.1.5): "identifies the time before which the JWT MUST NOT be accepted for processing."
- **`iat`**, [RFC 7519 §4.1.6](https://datatracker.ietf.org/doc/html/rfc7519#section-4.1.6): "identifies the time at which the JWT was issued. This claim can be used to determine the age of the JWT." `iat` is informational; it is not by itself a rejection condition.

All three claims are OPTIONAL per RFC 7519 §4.1, which means **a verifier that requires `exp` must enforce that requirement itself** - a token with no `exp` is a valid JWT, and silently accepting it as non-expiring is the failure mode.

**Leeway.** Both §4.1.4 and §4.1.5 use identical language: implementers MAY provide "some small leeway, usually no more than a few minutes, to account for clock skew."
The RFC gives no specific number, only that ceiling.

**Clock source.** `exp`/`nbf` are wall-clock UTC seconds, so they must be compared against `time(NULL)`, not `CLOCK_MONOTONIC`.
This is a live distinction in this codebase: `src/tetrisd/session.c:56-60` defines `now_ms()` on `CLOCK_MONOTONIC` and comments that it is "immune to wall-clock changes", and `get_time()` at `src/libtetrissh/common.c:600` is another existing clock helper.
Neither is the right clock for JWT claim checking.

### 3.5 Ordering

[RFC 7519 §7.2](https://datatracker.ietf.org/doc/html/rfc7519#section-7.2) fixes the validation order: split on `.` (step 1: "Verify that the JWT contains at least one period ('.') character"), base64url-decode the header (step 3), verify it is valid UTF-8 JSON (step 4), verify the header contains only understood parameters (step 5), then run JWS validation, then decode and parse the payload (steps 9-10).

The load-bearing consequence: **signature verification comes before the claims are trusted**.
Parsing the payload to read `exp` before checking the signature means parsing attacker-controlled JSON, and acting on any of it is a vulnerability.
[RFC 7515 §5.2](https://datatracker.ietf.org/doc/html/rfc7515#section-5.2) step 8 puts the signature check over the raw signing input: "Validate the JWS Signature against the JWS Signing Input ASCII(BASE64URL(UTF8(JWS Protected Header)) || '.' || BASE64URL(JWS Payload)) in the manner defined for the algorithm being used, which MUST be accurately represented by the value of the 'alg' (algorithm) Header Parameter, which MUST be present."

Note also that the signing input is the **original received bytes** of the two segments, not a re-encoding of the decoded values.
Re-encoding after a decode/encode round trip can differ (whitespace in the JSON, key ordering) and will fail to verify.

### 3.6 Weak HS256 secrets

[RFC 8725 §2.2](https://datatracker.ietf.org/doc/html/rfc8725#section-2.2) notes that HS256 with a weak symmetric key is open to "offline brute-force or dictionary attacks once an attacker gets hold of such a token".
A JWT is presented to the client, so the attacker always has one.
The secret must be random bytes from `RAND_bytes`, not a password or a string literal in the source.

## 4. What already exists in the repo

Grepped `src/`, `include/` and `tests/` for `base64`, `EVP_`, `HMAC`, `SHA256`, `RAND`, `CRYPTO_memcmp`, `jwt`, `token`.

### 4.1 Directly reusable

**`HMAC()` with SHA-256 - already used, twice.**
- `src/libtetrissh/common.c:505-507` (in `session_encrypt`)
- `src/libtetrissh/common.c:544-546` (in `session_decrypt`)

Both call `HMAC(EVP_sha256(), key, len, data, len, out, &outlen)` in exactly the one-shot form HS256 needs.
`HMAC_LEN` is already defined as 32 at `src/libtetrissh/common.h:65`, and `openssl/hmac.h` is already included at `src/libtetrissh/common.h:39`.
An HS256 signer is this same call over the signing input.

**`CRYPTO_memcmp` - already used correctly.**
`src/libtetrissh/common.c:551`:

```c
if (CRYPTO_memcmp(hmac_computed, hmac_received, HMAC_LEN) != 0)
```

Compares raw 32-byte MACs with an explicit length, checked against 0 - this is the correct pattern, and the same one §3.3 needs.
Note the surrounding code also does a length floor check first (`common.c:530`) before slicing the buffer, which is the analogous defence to the "reject signatures that are not exactly 32 bytes" rule.

**`RAND_bytes` - already used, with the return value checked.**
- `src/libtetrissh/common.c:439` (`generate_session_key`)
- `src/libtetrissh/common.c:462` (IV generation in `session_encrypt`)

Both check `!= 1`, which matches `RAND_bytes(3ssl)` RETURN VALUES: "return 1 on success, -1 if not supported by the current RAND method, or 0 on other failure ... it is important to always check the error return value."
An HS256 secret can be minted the same way. `openssl/rand.h` is already included at `src/libtetrissh/common.h:38`.

**`print_ssl_error`** - existing OpenSSL error-reporting helper, declared at `src/libtetrissh/common.h:238`, defined at `src/libtetrissh/common.c:589`.

### 4.2 Structurally similar but not directly reusable

**`sign_message_pss` / `verify_message_pss`** - `src/libtetrissh/common.c:233` and `src/libtetrissh/common.c:281`, declared at `src/libtetrissh/common.h:164` and `:172`.

These are the full `EVP_DigestSignInit` -> `Update` -> `Final` (two-pass length probe at `common.c:260` and `:267`) and `EVP_DigestVerifyInit` -> `Update` -> `Final` shape that RS256 would need.
Two reasons they cannot be used as-is:

1. **Padding is PSS, not PKCS#1 v1.5.** `common.c:251` sets `RSA_PKCS1_PSS_PADDING` and `common.c:253` sets `RSA_PSS_SALTLEN_MAX`. That is JWA `PS256` ([RFC 7518 §3.1](https://datatracker.ietf.org/doc/html/rfc7518#section-3), Optional), not `RS256`.
2. **`verify_message_pss` takes an `X509 *`, not an `EVP_PKEY *`** (`common.h:172`), so it is coupled to the certificate-based flow rather than to a bare public key.

Worth noting for correctness precedent: `common.c:311` checks `EVP_DigestVerifyFinal(...) == 1`, which is the correct comparison per §2.5 above.

**Key loading.** `load_private_key` at `src/libtetrissh/common.c:101` (declared `common.h:126`), and a second independent `load_server_key` at `src/tetrisd/session.c:42`.
The comment at `src/tetrisd/session.c:36-41` explains the duplication: `common.h` is deliberately not in `include/`, "so the OpenSSL helper surface stays private to that library", and `session_accept()` takes an `EVP_PKEY*` so the application owns key loading.
Any RS256 work must respect that boundary - a JWT module in `src/libtetrissh/` can use `common.h`, one outside it cannot.

### 4.3 Not present at all

- **No base64 of any kind.** No `base64`, `EVP_Encode*`, `EVP_Decode*` or `BIO_f_base64` anywhere in `src/`, `include/` or `tests/`. This is net-new code regardless of the algorithm chosen.
- **No JSON.** No JSON parser or serializer in the tree, and no JSON dependency in the Makefile. This is the part of the cost estimate most likely to be underestimated: a JWT needs a JSON *writer* for the header and claims (easy, since the issuer controls the shape) and a JSON *reader* for the claims on verify (harder, since it parses attacker-supplied bytes even though they are signed). The header `{"alg":"HS256","typ":"JWT"}` and a fixed claim set can both be handled without a general parser, but that is a design decision, not a given.
- **No existing JWT, token-minting or auth-token code.** The `token` grep hits in `src/tetrish/shell.c`, `src/tetrish/main.c`, `src/tetrish/system_programs/ld.c` and `src/tetrish/lib/rc_parser.c` are all `strtok` string tokenizing, unrelated. The `token` references in `common.h:224-230` and `common.c:518-539` mean the AES+HMAC session blob, not a JWT.
- **No 2048-bit RSA key.** `auth/private_key.pem` is 1024-bit (see §2.1). `auth/generate_keys.sh` is empty (0 bytes), so nothing in the repo records how to regenerate it.

## 5. What a minimal correct implementation consists of

Stated as a fact-check on the issue's premise ("a few dozen lines"), not as a recommendation.

Shared, regardless of algorithm:
1. base64url encode + decode (§1.4) - ~40-60 lines.
2. JSON emit for header and claims - small if the shape is fixed.
3. JSON read for claims on verify - the open cost, see §4.3.
4. Split on `.`, reject anything that is not exactly three segments (§3.5).
5. Header check: `alg` equals the one hard-coded value, case-sensitively (§3.1, §3.2).
6. Signature check over the **original** header and payload segment bytes (§3.5).
7. `exp` present and `now < exp`; `nbf` if present and `now >= nbf`; leeway of at most a few minutes; wall clock, not monotonic (§3.4).

Plus, for HS256: one `HMAC()` call, one length check, one `CRYPTO_memcmp` - roughly 15 lines, all of it patterned on code that already exists at `src/libtetrissh/common.c:505` and `:551`.

Plus, for RS256: `EVP_MD_CTX` lifecycle, PKCS#1 v1.5 padding, two-pass sign, `== 1` verify, plus a new 2048-bit key - roughly 60 lines and a key-management change.

The issue's premise holds for the crypto.
It is the base64url and JSON layers, not the signature, that dominate the line count.

## Open questions: what this note deliberately does not decide

- **HS256 vs RS256.** Both are implementable here. The deciding question is architectural and not answered by any source cited above: does anything other than the issuer need to verify these tokens? If issuer and verifier are the same process (or share a filesystem), HS256's shared secret is not a drawback and it is strictly less code. If not, RS256's public-key verification is the point.
- **Whether to reuse OpenSSL for base64 at all.** §1.3 documents `EVP_EncodeBlock`/`EVP_DecodeBlock` as workable-with-caveats. A standalone table-driven base64url is comparable in size and sidesteps the padding-arithmetic gotchas. Not evaluated by measurement here.
- **The JSON strategy.** Hand-rolled fixed-shape emit and a minimal reader, versus a vendored parser, versus a claim format that avoids general JSON. §4.3 flags this as the largest uncosted piece; this note does not size it.
- **Whether to regenerate `auth/private_key.pem` at 2048 bits.** §2.1 establishes that the current key does not meet RFC 7518 §3.3. Whether that key is regenerated, or JWTs get a separate key, or RS256 is dropped, is a decision with blast radius beyond JWTs - the same key backs the existing certificate flow.
- **Leeway value.** RFC 7519 §4.1.4/§4.1.5 give a ceiling ("no more than a few minutes") but no number. Picking one requires knowing the deployment's clock-sync assumptions, which are not documented in the repo.
- **Token lifetime, claim set, and where the secret lives.** Entirely out of scope of the sources consulted.

### Not verified

- The OpenSSL man pages cited are the **locally installed 3.6.3 pages** from `/opt/homebrew/opt/openssl@3/share/man`, read directly. Attempts to fetch the corresponding pages from `docs.openssl.org` returned the site's navigation index rather than page bodies, so the docs.openssl.org URLs are given as references but the quoted text is from the installed 3.6.3 man pages - which are the authoritative pages for the exact library this project links.
- No claim here has been validated by compiling and running code. Every statement is from a specification, a man page, or the repo's own source.
- CI and other developers' machines were not checked; the version scoping in §0 is based on this machine plus `Makefile:5` and `README.md:99`.
