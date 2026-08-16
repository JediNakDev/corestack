# Prompt: Generate Unity Unit Tests

Generate Unity unit tests for the module described below. Output one C file that compiles and runs against the Unity framework, linked with the named module's source and `tests/unity/unity.c`.

## Rules

1. Output a single C file only. No prose, no markdown fences, no explanation around the code.
2. The file must follow this structure:
   - `#include "unity.h"`
   - `#include "<module>.h"` (or `#include "common.h"` if you are testing helpers in common.c)
   - Any other standard library and POSIX headers needed (e.g. `<string.h>`, `<sys/socket.h>`, `<unistd.h>`)
   - `void setUp(void)` and `void tearDown(void)` (Unity requires these, even if empty)
   - Each test as a separate `void test_xxx(void)` function
   - A `main()` that calls `UNITY_BEGIN()`, then `RUN_TEST(...)` for each test, then `return UNITY_END();`
3. Cover normal cases, edge cases, and invalid input.
4. Tests must be self-contained processes. Do NOT spawn a server, do NOT call `system()` to launch the client, do NOT read from stdin. End-to-end behaviour belongs in `tests/integration/`, which the student maintains separately.
5. Each test function name must start with `test_`.
6. Add a comment at the top of the file marking it as an AI-generated draft, with a note that the student is responsible for reviewing and modifying it.
7. Keep tests small and readable. One assertion concept per test where possible.
8. If a function under test allocates memory and the contract says the caller must `free()` it, the test must `free()` the result.

## What you may use

- Unity assertions: `TEST_ASSERT_EQUAL_INT`, `TEST_ASSERT_EQUAL_UINT64`, `TEST_ASSERT_EQUAL_STRING`, `TEST_ASSERT_EQUAL_MEMORY`, `TEST_ASSERT_NULL`, `TEST_ASSERT_NOT_NULL`, `TEST_ASSERT_TRUE`, `TEST_ASSERT_FALSE`. See `tests/unity/unity.h` if unsure.
- `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)` to test functions that take a socket fd, without involving TCP or the loopback interface. Remember to `signal(SIGPIPE, SIG_IGN)` if you plan to write to a closed peer, otherwise the test process will be killed.
- OpenSSL APIs from `<openssl/...>`. Tests that exercise the crypto helpers (RSA sign/verify, AES-CBC + HMAC, X.509 verification) should generate keys and certs in-memory or load them from `source/auth/` if the helper accepts a file path.

## What you must not do

- Do not test private (static) functions. Test only what is declared in the header.
- Do not invent behaviour. If the header does not specify behaviour for some input, ask the student rather than guess.
- Do not include or link against any file that has its own `main()` (`ClientWithoutSecurity.c`, `ServerWithoutSecurity.c`, the AP/CP1/CP2 variants). Linking two `main()` symbols breaks the test binary.
- Do not write tests that bind to a fixed TCP port. Multiple integration tests already share port 14321, and a unit test fighting for it will cause flaky failures.
- Do not modify any file outside `tests/unit/`.

## Edge cases worth considering

For byte / integer conversion (`int_to_bytes`, `bytes_to_int`):

- 0, 1, `UINT64_MAX`
- Values with only the MSB set, only the LSB set (catches endianness bugs)
- Round trip: `bytes_to_int(int_to_bytes(x)) == x` for many `x`

For reliable I/O over a socket (`read_bytes`, `send_all`, `send_int`):

- Exact length match in a single send
- Length spread across multiple sends (forces the read loop to iterate)
- Peer closes before the requested number of bytes arrive (must return NULL / non-zero rc, not block)
- Zero-length request
- Send-side failure when the peer has closed (`SIGPIPE` ignored, error returned)

For OpenSSL key and certificate helpers (`load_private_key`, `load_cert_file`, `load_cert_bytes`, `verify_server_cert`):

- A valid PEM file produces a non-NULL handle that the caller must free
- A non-existent path returns NULL without crashing
- A corrupt PEM (e.g. truncated, header missing) returns NULL
- A self-signed cert and a CA-issued cert are distinguishable by `verify_server_cert`

For RSA sign / verify (`sign_message_pss`, `verify_message_pss`):

- A signature produced with key K verifies with the matching cert for K
- A signature does not verify against a different public key
- A flipped-bit ciphertext does not verify
- An empty message both signs and verifies

For symmetric session encryption (`session_encrypt`, `session_decrypt`, `generate_session_key`):

- Round trip: `session_decrypt(session_encrypt(p)) == p` for empty, single-block, multi-block plaintexts
- A flipped bit in the HMAC tail causes `session_decrypt` to return NULL
- A flipped bit in the IV or ciphertext causes `session_decrypt` to return NULL
- Two calls to `generate_session_key` produce different keys with overwhelming probability

For RSA block encryption (`rsa_encrypt_block`, `rsa_decrypt_block`):

- Round trip with both OAEP and PKCS1v15 modes
- Plaintext at the documented chunk size limit (`RSA_OAEP_CHUNK`, `RSA_PKCS1_CHUNK`)
- Plaintext over the limit returns NULL rather than truncating
