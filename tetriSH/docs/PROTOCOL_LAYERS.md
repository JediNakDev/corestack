# Protocol layers — `libtetrissh` and `libhtttp`

`docs/SEQUENCES.md` draws *what the two clients say to the daemon*. This
document draws *how a sentence becomes bytes and back*: the crypto transport
underneath and the grammar on top of it.

The two libraries **never reference each other**. No compile-time dependency,
no link-time dependency, no shared header. They meet only in the application,
which pipes plaintext out of `session_recv` into `htttp_parse_*`. That is why
`libhtttp` links against libc alone and can be fuzzed with no OpenSSL present.

```mermaid
flowchart TD
    APP["application<br/>tetrisu · session.c · tetrisctl"]
    H["libhtttp — grammar<br/>METHOD PATH HTTTP/1.0, headers, body<br/>libc only, no sockets, no crypto"]
    S["libtetrissh — transport<br/>handshake, AES-128-CBC + HMAC-SHA256,<br/>sequence numbers, 4-byte BE framing"]
    C["common.c — PA2 primitives<br/>RSA-OAEP, RSA-PSS, X.509, Fernet-equivalent"]
    T["TCP socket"]

    APP -->|"htttp_serialize_request → uint8_t[]"| H
    H -->|"plaintext bytes"| APP
    APP -->|"session_send(plaintext)"| S
    S -->|"session_recv → plaintext"| APP
    S --> C
    C --> T

    APP -.->|"tetrisctl only: ctl_frame_write,<br/>same 4-byte BE prefix, NO crypto"| T
```

---

# Part A — `libtetrissh`

## A0. The API

| Function | Caller | Usage | Protocol |
|---|---|---|---|
| `session_connect(fd, ca_path)` | client | handshake, fills `session_t` or leaves it zeroed | nonce → verify cert + RSA-PSS sig → send RSA-OAEP wrapped key |
| `session_accept(fd, priv, cert_path)` | server | handshake, proves identity, learns the key | recv nonce → sign + send cert → unwrap key |
| `session_send(buf, len)` | both | one frame out, partial writes handled inside | `seq‖plaintext` → AES-128-CBC + HMAC → `len‖IV‖ct‖mac` |
| `session_recv(buf, *len)` | both | one frame in, short reads handled inside | `len‖token` → HMAC check → decrypt → seq check |
| `session_close()` | both | cleanse key, mark dead — fd stays open | none, local only |

> Every function takes `session_t *s` first. Returns `SESSION_OK` or a negative
> `session_err_t`. Frame limit 64 KiB, enforced on the wire frame in both
> directions. The library never opens or closes the fd — `connect()`,
> `accept()` and `close()` stay the application's.

---

## A1. The handshake, both sides

Every field on the wire is length-prefixed with `INT_BYTES` (`send_int` /
`read_bytes`). The client speaks first.

```mermaid
sequenceDiagram
    participant C as session_connect<br/>(client)
    participant S as session_accept<br/>(server)

    Note over C,S: both memset their session_t on entry —<br/>a failed handshake leaves nothing usable

    C->>C: RAND_bytes(nonce, 32)
    C->>S: len(32) + nonce
    S->>S: bounds check — 0 < len <= TSSH_MAX_NONCE_LEN,<br/>else SESSION_ERR_PROTO

    S->>S: sign_message_pss(priv, nonce) — RSA-PSS over SHA-256
    S->>C: len + signature
    S->>S: read cert_path off disk (fopen, fseek, fread)
    S->>C: len + X.509 certificate (PEM bytes)

    C->>C: bounds check both lengths against<br/>TSSH_MAX_SIG_LEN / TSSH_MAX_CERT_LEN
    C->>C: load_cert_bytes → verify_server_cert(cert, ca_path)
    Note over C: X509_STORE with the CA as the only trust anchor,<br/>X509_verify_cert checks signature AND validity window
    C->>C: verify_message_pss(cert, sig, nonce)
    Note over C: the nonce is the freshness proof — a recorded<br/>handshake cannot be replayed against a new nonce

    alt cert forged / self-signed / expired, or signature bad
        C->>C: SESSION_ERR_AUTH — s stays zeroed
    end

    C->>C: X509_get_pubkey(cert)
    C->>C: generate_session_key(32) = HMAC key(16) || AES key(16)
    C->>C: rsa_encrypt_block(pub, key, OAEP)
    C->>S: len + RSA-OAEP wrapped session key
    S->>S: length must equal RSA_KEY_BYTES exactly, else PROTO
    S->>S: rsa_decrypt_block(priv, OAEP) — must yield exactly 32 bytes

    C->>C: s.key = session_key, s.established = 1, seqs = 0
    S->>S: s.key = session_key, s.established = 1, seqs = 0
    Note over C,S: both sides cleanse their stack/heap copy<br/>of the key on EVERY exit path
```

**Only the server is authenticated.** The client proves nothing at this layer —
that is `libtetrisauth`'s job one layer up (`LOGIN` / `REGISTER` / `GUEST`).

---

## A2. Handshake failure taxonomy

```mermaid
flowchart TD
    A["session_connect / session_accept"] --> B{"socket read/write failed<br/>or peer closed?"}
    B -->|yes| IO["SESSION_ERR_IO"]
    B -->|no| C{"declared length 0, or over<br/>TSSH_MAX_NONCE/SIG/CERT_LEN,<br/>or symkey_len != RSA_KEY_BYTES?"}
    C -->|yes| P["SESSION_ERR_PROTO<br/>malformed handshake"]
    C -->|no| D{"verify_server_cert<br/>AND verify_message_pss?"}
    D -->|either fails| AU["SESSION_ERR_AUTH<br/>the only code that means<br/>'I do not trust you'"]
    D -->|both pass| E{"RAND_bytes / keygen /<br/>RSA wrap / RSA unwrap ok?"}
    E -->|no| CR["SESSION_ERR_CRYPTO"]
    E -->|yes| OK["SESSION_OK<br/>established = 1"]

    IO --> Z["single-exit cleanup:<br/>free every heap object (NULL-safe),<br/>OPENSSL_cleanse the key copy"]
    P --> Z
    AU --> Z
    CR --> Z
    OK --> Z
```

`client_connect` in `tetrisu` returns this code **verbatim** rather than
squashing it to `-1` — `SESSION_ERR_AUTH` is the only thing that separates a
rejected certificate from a dead route.

---

## A3. `session_send` — building one frame

```mermaid
flowchart TD
    A["session_send(s, buf, len)"] --> B{"s->established?"}
    B -->|no| P["SESSION_ERR_PROTO"]
    B -->|yes| C{"len > SESSION_MAX_FRAME - SEQ_BYTES?"}
    C -->|yes| TB["SESSION_ERR_TOOBIG<br/>nothing written"]
    C -->|no| D["malloc(len + 8)<br/>seq_put(framed, send_seq) — 8-byte BE"]
    D --> E["memcpy(framed + 8, buf, len)"]
    E --> F["session_encrypt(key, framed, len + 8)"]
    F --> G["OPENSSL_cleanse(framed) then free<br/>— for LOGIN this copy held a password"]
    G --> H{"encrypt returned NULL?"}
    H -->|yes| CR["SESSION_ERR_CRYPTO"]
    H -->|no| I{"enc_block > SESSION_MAX_FRAME?"}
    I -->|yes| TB
    I -->|no| J["send_u32(fd, enc_block) — 4-byte BE"]
    J --> K["send_all(fd, token, enc_block)<br/>loops over partial writes"]
    K --> L{"both writes ok?"}
    L -->|no| IO["SESSION_ERR_IO<br/>send_seq NOT advanced"]
    L -->|yes| M["send_seq++<br/>only a frame that went out spends a number"]
```

`session_encrypt` layout, from `common.c`:

```
IV (16, random)  ||  AES-128-CBC ciphertext (PKCS#7 padded)  ||  HMAC-SHA256 (32)
                 └──────────── HMAC covers IV || ciphertext ────────────┘
```

Encrypt-then-MAC. The MAC is over the IV as well, so an attacker cannot flip
the IV to scramble the first plaintext block.

---

## A4. The wire frame, byte by byte

```
┌───────────────┬────────────────────────────────────────────────────────────┐
│ 4 B length BE │                 encrypted token (length bytes)             │
└───────────────┴────────────────────────────────────────────────────────────┘
                 ┌──────────┬──────────────────────────────┬────────────────┐
                 │  IV 16 B │  AES-128-CBC ciphertext       │  HMAC-SHA256   │
                 │          │  (multiple of 16, PKCS#7)     │      32 B      │
                 └──────────┴──────────────────────────────┴────────────────┘
                             decrypts to ↓
                 ┌────────────────┬───────────────────────────────────────────┐
                 │ seq 8 B BE     │  plaintext = one complete HTTTP message   │
                 └────────────────┴───────────────────────────────────────────┘
                                   ┌──────────────────────────────────────────┐
                                   │ METHOD SP PATH SP HTTTP/1.0 CRLF         │
                                   │ Key: Value CRLF   (0..32 headers)        │
                                   │ CRLF                                     │
                                   │ body (Content-Length bytes, may be NUL)  │
                                   └──────────────────────────────────────────┘
```

Budget: `SESSION_MAX_FRAME` is 64 KiB and is measured on the **wire frame**,
identically on send and recv. The 8-byte sequence number, the 16-byte IV, the
32-byte HMAC and up to 16 bytes of CBC padding all come out of it, so usable
plaintext is a little under 64 KiB. `HTTTP_MAX_FRAME` is also 65536, so a
maximal HTTTP message does not quite fit a maximal session frame — the app is
responsible for sizing its serialize buffer below the limit.

---

## A5. `session_recv` — consuming one frame

```mermaid
flowchart TD
    A["session_recv(s, buf, len)"] --> B{"established AND not recv_dead?"}
    B -->|no| P1["SESSION_ERR_PROTO"]
    B -->|yes| C["read_u32(fd) → token_len"]
    C --> D{"read failed?"}
    D -->|yes| IO["SESSION_ERR_IO"]
    D -->|no| E{"token_len > SESSION_MAX_FRAME?"}
    E -->|yes| TB["recv_dead = 1<br/>SESSION_ERR_TOOBIG<br/>the body was never consumed,<br/>so the read stream is out of sync"]
    E -->|no| F["read_bytes(fd, token_len)"]
    F --> G["session_decrypt: HMAC FIRST,<br/>CRYPTO_memcmp constant time,<br/>then AES-128-CBC"]
    G --> H{"HMAC mismatch, short token,<br/>or bad padding?"}
    H -->|yes| CR["SESSION_ERR_CRYPTO"]
    H -->|no| I{"plain_len < 8?"}
    I -->|yes| P2["recv_dead = 1<br/>SESSION_ERR_PROTO<br/>not a frame we wrote"]
    I -->|no| J{"seq_get(plain) == recv_seq?"}
    J -->|no| P3["recv_dead = 1<br/>SESSION_ERR_PROTO<br/>replayed or reordered"]
    J -->|yes| K["recv_seq++ — spent either way"]
    K --> L{"body_len > caller capacity?"}
    L -->|yes| NS["SESSION_ERR_NOSPACE<br/>frame consumed, stream STILL IN SYNC,<br/>session stays usable"]
    L -->|no| M["memcpy into caller buffer<br/>*len = body_len<br/>SESSION_OK"]

    CR --> Z["OPENSSL_cleanse(plain, plain_len) then free<br/>on EVERY exit — measured 66 ns vs ~2000 ns decrypt,<br/>so it is unconditional rather than opt-in"]
    P2 --> Z
    P3 --> Z
    NS --> Z
    M --> Z
```

**Why `recv_dead` and not `established = 0`.** The desync is in what we are
being *fed*, not in what we write. One last `session_send` still works, which is
the window an application uses to answer `413 Payload Too Large` before closing
the fd.

---

## A6. Replay defence

The sequence number rides **inside** the authenticated plaintext, not beside it.

```mermaid
sequenceDiagram
    participant A as peer A
    participant M as MITM
    participant B as peer B

    A->>M: frame(seq=7) — DROP HARD
    M->>B: forwarded
    B->>B: seq_get == recv_seq(7) → accept, recv_seq = 8

    Note over M: replay the recorded frame
    M->>B: frame(seq=7) again
    B->>B: 7 != 8 → recv_dead = 1, SESSION_ERR_PROTO
    Note over B: fatal, not skippable — reading on would leave<br/>the attacker free to keep injecting

    Note over M: rewrite the counter instead
    M->>M: cannot — the counter is inside the ciphertext,<br/>and HMAC-SHA256 covers IV || ciphertext
    M->>B: tampered frame
    B->>B: CRYPTO_memcmp fails → SESSION_ERR_CRYPTO

    Note over M: reorder two live frames
    M->>B: frame(seq=9) before frame(seq=8)
    B->>B: TCP already guarantees order, so a gap means<br/>the stream was edited → PROTO
```

Exactly the next number, not merely a larger one. TCP delivers in order and
without duplicates, so any gap is evidence of tampering.

---

## A7. `session_t` lifecycle

```mermaid
stateDiagram-v2
    [*] --> Fresh: memset on entry to connect/accept
    Fresh --> Established: handshake OK — key set, seqs zeroed
    Fresh --> [*]: ERR_IO / ERR_AUTH / ERR_PROTO / ERR_CRYPTO

    Established --> Established: session_send OK — send_seq++
    Established --> Established: session_recv OK — recv_seq++
    Established --> Established: ERR_NOSPACE — frame dropped, stream in sync
    Established --> Established: ERR_CRYPTO on send — nothing written

    Established --> RecvDead: ERR_TOOBIG on recv
    Established --> RecvDead: sequence mismatch or short plaintext

    RecvDead --> RecvDead: one last session_send still works — answer 413
    RecvDead --> Closed: session_close
    Established --> Closed: session_close

    Closed --> [*]: key cleansed, established = 0 — fd NOT closed, caller owns it
```

---

# Part B — `libhtttp`

## B0. The API

| Function | Caller | Usage | Protocol |
|---|---|---|---|
| `htttp_parse_request(buf, len, req)` | both | one frame → struct, `body` points into `buf` | `METHOD SP PATH SP HTTTP/1.0 CRLF` |
| `htttp_parse_response(buf, len, res)` | client, `tetrisctl` | one frame → struct, status range-checked | `HTTTP/1.0 SP STATUS SP REASON CRLF` |
| `htttp_serialize_request(req, out, *out_len)` | both | struct → bytes, adds `Content-Length` | `METHOD SP PATH SP HTTTP/1.0 CRLF` |
| `htttp_serialize_response(res, out, *out_len)` | server only | struct → bytes, adds `Content-Length` + `Date` | `HTTTP/1.0 SP STATUS SP REASON CRLF` |
| `htttp_header_get(headers, n, key)` | both | case-insensitive lookup, `NULL` if absent | `Key: Value CRLF` |
| `htttp_header_set(headers, *n, key, value)` | both | append, `TOOLONG` at 32 | `Key: Value CRLF` |
| `htttp_reason(status)` | both | status → canonical phrase, `NULL` if unknown | the REASON field |

> Message = request-line/status-line, then headers, then `CRLF`, then
> `Content-Length` bytes of body. Buffers in, buffers out — no sockets, no
> crypto. Returns `HTTTP_OK` or a negative `htttp_err_t`, except the last two,
> which return a pointer or `NULL`.

**The Caller column is the punchline.** Requests flow *both* directions — the
client sends `MOVE`/`JOIN`, the server pushes `STATE`/`UPD_SESSION` as requests
too — so both ends call both parsers. Only `htttp_serialize_response` is
one-sided: a client never answers.

---

## B1. `htttp_parse_request`

One frame in, one message out. Framing already guarantees message boundaries,
so there is no incremental parser and no state to carry between calls.

```mermaid
flowchart TD
    A["htttp_parse_request(buf, len, req)"] --> B{"buf and req non-NULL?"}
    B -->|no| M1["HTTTP_ERR_MALFORMED"]
    B -->|yes| C["memset(req) FIRST — a rejected message<br/>must leave an unambiguously empty struct"]
    C --> D{"find_crlf from 0?"}
    D -->|none| M1
    D -->|eol| E["scan to sp1 — METHOD"]
    E --> F{"sp1 == 0 or sp1 >= eol?"}
    F -->|yes| M1
    F -->|no| G["copy_token → method[16]"]
    G --> H{"too long?"}
    H -->|yes| TL["HTTTP_ERR_TOOLONG"]
    H -->|no| I["scan from sp1+1 to sp2 — PATH<br/>starts one past sp1, so the path<br/>cannot contain a space by construction"]
    I --> J{"empty path or no version field?"}
    J -->|yes| M1
    J -->|no| K["copy_token → path[256]"]
    K --> L{"eol - sp2 - 1 == VERSION_LEN<br/>AND memcmp == HTTTP/1.0?"}
    L -->|no| M2["HTTTP_ERR_MALFORMED<br/>length checked BEFORE memcmp —<br/>also catches a third space"]
    L -->|yes| N["parse_tail(buf, len, eol + 2, ...)"]
```

## B2. `parse_tail` — the shared half

```mermaid
flowchart TD
    A["parse_tail — headers, then body"] --> B["for(;;) — two exits only"]
    B --> C{"find_crlf?"}
    C -->|none| M1["HTTTP_ERR_MALFORMED<br/>a while(found) loop would treat<br/>'ran out of buffer' as normal termination,<br/>so a truncated frame would parse as valid"]
    C -->|eol == off| D["blank line — headers done, off += 2, break"]
    C -->|eol > off| E["scan for ':' bounded by eol, never len"]
    E --> F{"colon == off, colon >= eol,<br/>or buf[colon+1] != ' '?"}
    F -->|yes| M1
    F -->|no| G{"n_headers >= 32?"}
    G -->|yes| TL["HTTTP_ERR_TOOLONG"]
    G -->|no| H["copy key [off, colon), value [colon+2, eol)<br/>empty value is legal"]
    H --> B

    D --> I["remaining = len - off<br/>look up Content-Length"]
    I --> J{"header absent?"}
    J -->|yes| K{"remaining == 0?"}
    K -->|no| L1["HTTTP_ERR_LENGTH<br/>trailing bytes rejected, not ignored"]
    K -->|yes| OK1["body = NULL, body_len = 0, HTTTP_OK"]
    J -->|no| N{"parse_uint(value) ok?"}
    N -->|no| M1
    N -->|yes| O{"declared == remaining?"}
    O -->|no| L2["HTTTP_ERR_LENGTH<br/>trusting 'Content-Length: 5000' on a<br/>10-byte message is a heap over-read"]
    O -->|yes| OK2["body = buf + off — ZERO COPY<br/>body_len = declared, HTTTP_OK"]
```

The equality check is the whole defence. Note the ownership consequence drawn
in B5.

## B3. `htttp_parse_response`

The mirror, and cheaper: the version sits at a fixed offset, so only one space
needs searching.

```mermaid
flowchart TD
    A["htttp_parse_response(buf, len, res)"] --> B["memset(res)"]
    B --> C{"find_crlf?"}
    C -->|none| M["HTTTP_ERR_MALFORMED"]
    C -->|eol| D{"eol > VERSION_LEN<br/>AND memcmp(buf, HTTTP/1.0)<br/>AND buf[VERSION_LEN] == ' '?"}
    D -->|no| M
    D -->|yes| E["scan from VERSION_LEN+1 to sp1"]
    E --> F{"empty status or no reason field?"}
    F -->|yes| M
    F -->|no| G["parse_uint IN PLACE — numbers are never copied,<br/>only string fields get copy_token"]
    G --> H{"100 <= status <= 599?"}
    H -->|no| M
    H -->|yes| I["res->status = status<br/>reason deliberately NOT stored —<br/>htttp_reason() is the single source of truth"]
    I --> J["parse_tail(buf, len, eol + 2, ...)"]
```

## B4. Serializing — where the injection guard lives

```mermaid
flowchart TD
    subgraph REQ["htttp_serialize_request"]
        A1["body == NULL but body_len > 0?<br/>→ MALFORMED (a length with no buffer<br/>would be read as body bytes)"]
        A2["empty method or path?<br/>→ MALFORMED (two adjacent spaces<br/>reparse as a different message)"]
        A3["valid_field(method, no space)<br/>valid_field(path, no space)<br/>0x21..0x7e only"]
        A4["METHOD SP PATH SP VERSION CRLF"]
        A5["emit_headers(skip_date = 0)"]
        A6["emit_body — adds Content-Length"]
        A1 --> A2 --> A3 --> A4 --> A5 --> A6
    end

    subgraph RES["htttp_serialize_response"]
        B1["htttp_reason(status) == NULL?<br/>→ MALFORMED. Checked BEFORE any byte<br/>is written, so out stays untouched.<br/>This is why reason must not return a placeholder"]
        B2["snprintf status to ASCII digits<br/>— no valid_field needed"]
        B3["gmtime_r + strftime built BEFORE the writes,<br/>so a failure cannot abort halfway through.<br/>gmtime_r not gmtime: tetrisd is threaded"]
        B4["VERSION SP STATUS SP REASON CRLF"]
        B5["Date header emitted first"]
        B6["emit_headers(skip_date = 1)<br/>— a caller's own Date is dropped"]
        B7["emit_body — adds Content-Length"]
        B1 --> B2 --> B3 --> B4 --> B5 --> B6 --> B7
    end

    G["valid_field: a space or CRLF inside a method,<br/>path or header key forges an extra request line<br/>or header. REJECTED, not escaped —<br/>this grammar has no escaping layer."]
    A3 -.-> G
    B6 -.-> G
```

Both return `*out_len = off`, never `strlen(out)`: the output is raw bytes with
no terminator, and a body may legitimately contain NULs.

## B5. Zero-copy — who owns the bytes

```mermaid
sequenceDiagram
    participant CL as Client struct
    participant NS as net_service
    participant H as libhtttp
    participant AP as the app

    Note over CL: rxbuf[65536] lives INSIDE Client,<br/>not on a stack frame
    NS->>CL: session_recv(&sh, c->rxbuf, &len)
    NS->>H: htttp_parse_request(c->rxbuf, len, &req)
    H-->>NS: req.body points INTO c->rxbuf — no malloc, no copy,<br/>no free of a payload up to 64 KiB
    NS->>AP: memcpy(&c->game, req.body, sizeof c->game)
    Note over NS,AP: the copy out happens while rxbuf is still alive.<br/>If rxbuf were a local, req.body would dangle<br/>the moment net_service returned — which is the<br/>entire reason rxbuf is a struct member
```

## B6. Error codes → HTTTP status

```mermaid
flowchart LR
    M["HTTTP_ERR_MALFORMED (-1)<br/>bad request line, bad header syntax"] --> S400["app sends 400"]
    T["HTTTP_ERR_TOOLONG (-2)<br/>method/path/header/count over bound"] --> S400
    L["HTTTP_ERR_LENGTH (-3)<br/>body size != Content-Length"] --> S400
    N["HTTTP_ERR_NOSPACE (-4)<br/>serialize output buffer too small"] --> APP["the app's own bug —<br/>never reaches the wire"]
    TB["SESSION_ERR_TOOBIG (transport)"] --> S413["app sends 413,<br/>then closes the fd"]
```

The parser never decides *policy*. Any token parses as a method — "is `MOVE`
allowed right now" is the dispatcher's question, which is exactly what lets the
same library carry tetriSH's `JOIN`/`MOVE` and BallotBox's `CAST`.

---

# Part C — the whole stack

## C1. One keypress, all the way down and back

```mermaid
sequenceDiagram
    participant K as keyboard
    participant G as game_screen
    participant NC as net.c send_cmd
    participant H as libhtttp
    participant SS as libtetrissh
    participant W as TCP
    participant SR as libtetrissh (peer)
    participant HP as libhtttp (peer)
    participant D as session.c dispatcher
    participant B as tetrisbrain

    K-->>G: getch() = KEY_RIGHT
    G->>NC: client_move(c, right)
    NC->>NC: player_path → /room/3/player/7
    NC->>NC: htttp_header_set("Player-Id", "7")
    NC->>H: htttp_serialize_request(method MOVE, body "RIGHT")
    H-->>NC: 74 bytes of plaintext
    NC->>SS: session_send(plaintext, 74)
    SS->>SS: prepend seq(8) → encrypt → IV || ct || HMAC
    SS->>W: 4-byte BE length + 128-byte token

    W->>SR: bytes
    SR->>SR: read length, read token, HMAC check, decrypt,<br/>seq check, strip 8
    SR-->>D: 74 bytes of plaintext
    D->>HP: htttp_parse_request
    HP-->>D: method MOVE, path /room/3/player/7, body RIGHT
    D->>D: ROUTES[] lookup — phase and path shape and Player-Id
    D->>B: tetrisbrain_input(MOVE_RIGHT)
    D-->>SR: 200 response, same road back
    Note over D,B: the 200 is ignored by the client —<br/>the STATE push 50 ms later is the real answer
```

Every layer boundary in that diagram is a byte buffer. Nothing above
`libtetrissh` knows a socket exists, and nothing inside `libhtttp` knows a key
exists.

## C2. Two framings, side by side

```mermaid
flowchart TD
    subgraph GAME["tetrisu ↔ tetrisd — game plane"]
        G1["4-byte BE length"] --> G2["IV 16"] --> G3["ciphertext"] --> G4["HMAC 32"]
        G3 -.->|decrypts to| G5["seq 8 || HTTTP message"]
    end

    subgraph CTL["tetrisctl ↔ tetrisd — control plane"]
        C1["4-byte BE length"] --> C2["HTTTP message, plaintext"]
    end

    NOTE["Same prefix width, same grammar, no crypto on the control plane.<br/>It is justified by the socket: AF_UNIX bound under umask(077)<br/>plus an explicit chmod 0600, so reaching it already means<br/>local access as the right user."]
    CTL --- NOTE
```

The `umask` is narrowed *around* the `bind` rather than fixed with a later
`chmod`, because a `chmod` afterwards leaves a window where the world-writable
mode is already on disk — and a world-writable control socket lets any local
user shut the server down.

## C3. Where each defence lives

```mermaid
flowchart TD
    A["hostile bytes arrive"] --> B["libtetrissh<br/>· length prefix bounded by SESSION_MAX_FRAME<br/>· HMAC before decrypt, constant-time compare<br/>· sequence number inside the ciphertext"]
    B --> C["libhtttp<br/>· every bound checked before the read, not after<br/>· Content-Length cross-checked against bytes present<br/>· no incremental state to confuse"]
    C --> D["libtetrisauth<br/>· LOGIN/REGISTER only until authenticated<br/>· 401 and 404 counted, budget never resets<br/>· GUEST touches no database"]
    D --> E["session.c dispatcher<br/>· ROUTES[] separates 501 from 409<br/>· Player-Id absent = 401, wrong = 403<br/>· path room cross-checked against session room"]
    E --> F["room.c / admin thread<br/>· owns the tables, single-threaded, no locks<br/>· ownership and phase rules"]

    X["control plane bytes"] --> Y["ctl thread<br/>· parses everything hostile<br/>· classify() validates the path shape"]
    Y --> Z["admin thread<br/>receives a 16-byte CtlReq: four ints, no pointers"]
    Z --> F
```

Each layer's failure is total for that layer and invisible to the next: a bad
HMAC never reaches the parser, a malformed request line never reaches the
dispatcher, a bad `CtlReq` never reaches the room tables.

---

# Part D — method reference

Every method the system defines, grouped by plane. `libhtttp` itself allows any
token as a method — the tables below are what the *applications* agree on.

## D0. Sample exchanges

Literal wire text, as `libhtttp` emits it. **Every line ends `CRLF`**, and a
blank `CRLF` closes the header block. On the game plane all of this is the
*plaintext* — it goes through `session_send`, so the socket carries
`len‖IV‖ciphertext‖HMAC` and none of it is readable in a capture.

Header order is not cosmetic, it is what the serializers do: requests emit
`Content-Type` (only when there is a body) then `Player-Id`, responses emit
`Date` first, and `Content-Length` is appended last by `emit_body` in both.

### Guest login

```http
GUEST /auth HTTTP/1.0

```
```http
HTTTP/1.0 200 OK
Date: Sun, 09 Aug 2026 21:38:04 GMT

```

No `Player-Id` on the auth methods — the exchange is what establishes one. The
`200` is bodyless, so no `Content-Length` is emitted at all.

### Account login

```http
LOGIN /auth HTTTP/1.0
Content-Type: application/tetris-command
Content-Length: 11

pop
hunter2
```
```http
HTTTP/1.0 200 OK
Date: Sun, 09 Aug 2026 21:38:04 GMT
Content-Length: 141

eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOjQyLCJuYW1lIjoicG9wIiwiaWF0IjoxNzg2Mzk1MDg0LCJleHAiOjE3ODY0ODE0ODR9.9pQ1n0mJmYb0k1qzZq4H8rXk3sQ1yTt7uV2wLpNcAbc
```

The body is `username\npassword`, split at the **first** LF — which is why the
password charset is unconstrained and only the username may not contain one.
The client discards the JWT: nothing in tetriSH ever verifies one.

Wrong password:

```http
HTTTP/1.0 401 Unauthorized
Date: Sun, 09 Aug 2026 21:38:04 GMT

```

### Create a room

```http
JOIN /room/0 HTTTP/1.0
Player-Id: 0
```
```http
HTTTP/1.0 201 Created
Date: Sun, 09 Aug 2026 21:38:04 GMT

```

`Player-Id: 0` is the default — before `JOIN` the server checks only that the
header is *present*, and checks its value afterwards. `201` answers only a
`JOIN` that created a room; joining an existing one gets no response at all.

Either way the real acknowledgement is the push that follows:

```http
UPD_SESSION /session/state HTTTP/1.0
Content-Type: application/tetris-state
Content-Length: 212

<212 bytes of SessionState — phase, room_id, player_id, is_owner, roster[]>
```

### One move

```http
MOVE /room/3/player/7 HTTTP/1.0
Content-Type: application/tetris-command
Player-Id: 7
Content-Length: 5

RIGHT
```
```http
HTTTP/1.0 200 OK
Date: Sun, 09 Aug 2026 21:38:04 GMT

```

The client ignores that `200`. What it watches for is the next board push,
which arrives within 50 ms whether or not the move was accepted:

```http
STATE /room/3 HTTTP/1.0
Content-Type: application/tetris-state
Content-Length: 1384

<1384 bytes of GameState — board[26][10], next[5], hold, score, standings[]>
```

Pushed as a **request**, not a response, because it is unsolicited. That is the
two-message-shape split `net_service` handles: responses are tried first,
because a status line is unambiguous.

### A refused command

```http
START /room/3 HTTTP/1.0
Player-Id: 9
```
```http
HTTTP/1.0 403 Forbidden
Date: Sun, 09 Aug 2026 21:38:04 GMT

```

Player 9 is a member but not the owner. A `MOVE` sent while the room is still
`WAITING` earns `409` instead — right method, wrong moment.

### Round over

```http
UPD_RESULT /game/result HTTTP/1.0
Content-Type: application/tetris-state
Content-Length: 4

<int32 winning player_id>
```

### Control plane

Same grammar, no crypto, `AF_UNIX` — and one connection per command:

```http
STATUS / HTTTP/1.0

```
```http
HTTTP/1.0 200 OK
Date: Sun, 09 Aug 2026 21:38:04 GMT
Content-Type: application/json
Content-Length: 39

{"uptime":3714,"sessions":12,"rooms":3}
```

```http
ROOMS / HTTTP/1.0

```
```http
HTTTP/1.0 200 OK
Date: Sun, 09 Aug 2026 21:38:04 GMT
Content-Type: application/json
Content-Length: 99

[{"id":1,"phase":"PLAYING","members":2,"owner":7},{"id":4,"phase":"WAITING","members":1,"owner":9}]
```

```http
KICK /room/1/player/7 HTTTP/1.0

```
```http
HTTTP/1.0 200 OK
Date: Sun, 09 Aug 2026 21:38:04 GMT
Content-Type: application/json
Content-Length: 15

{"kicked":true}
```

The victim gets a `403` on its own connection a moment later, then `SIGTERM`.
The operator is answered *first*: a kick that succeeded but reported failure is
the worse outcome to debug.

---

## D1. Auth — game plane, pre-authentication gate

| Method | Path | Dir | Body | Purpose |
|---|---|---|---|---|
| `LOGIN` | `/auth` | C→S | `username\npassword` | authenticate an existing account, `200` carries a JWT |
| `REGISTER` | `/auth` | C→S | `username\npassword` | create an account then authenticate |
| `GUEST` | `/auth` | C→S | — | play with no account, touches no database |

Split at the first LF, so the password charset is unconstrained by construction
and only the username may not contain one.

## D2. Lobby — after the gate

| Method | Path | Dir | Body | Purpose |
|---|---|---|---|---|
| `JOIN` | `/room/{id}` | C→S | — | join room `id`, or `id=0` to create a new one |
| `LEAVE` | `/room/{id}` | C→S | — | leave the current room |
| `START` | `/room/{id}` | C→S | — | owner starts the round for every member |

None of the three has a response of its own. `UPD_SESSION` is their
acknowledgement — see A4/A5 in `docs/SEQUENCES.md`.

## D3. In-round — valid only while `PLAYING`

| Method | Path | Dir | Body | Purpose |
|---|---|---|---|---|
| `MOVE` | `/room/{id}/player/{pid}` | C→S | `LEFT` \| `RIGHT` | shift the active piece |
| `ROTATE` | `/room/{id}/player/{pid}` | C→S | `CW` \| `CCW` | rotate the active piece |
| `DROP` | `/room/{id}/player/{pid}` | C→S | `SOFT` \| `HARD` | drop, hard drop also flushes garbage |
| `HOLD` | `/room/{id}/player/{pid}` | C→S | — | swap the active piece into hold |

Bodies are words, not digits. A body outside the listed set is `400`. The `200`
these earn is ignored by the client — the `STATE` push is the real feedback.

## D4. Server pushes — sent as requests, not responses

| Method | Path | Dir | Body | Purpose |
|---|---|---|---|---|
| `STATE` | `/room/{id}` | S→C | `GameState` bytes | the board at 20 Hz |
| `UPD_SESSION` | `/session/state` | S→C | `SessionState` bytes | room id, player id, ownership, roster, phase |
| `UPD_RESULT` | `/game/result` | S→C | `int32` winner | round over, who won |

Pushed as requests because they are unsolicited. Responses (`403`, `409`, `429`)
answer a command the client just sent. That is the two-message-shape split
`net_service` handles — see B1 in this document and A12 in `docs/SEQUENCES.md`.

## D5. Control plane — AF_UNIX, `tetrisctl` only

| Method | Path | Dir | Body | Purpose |
|---|---|---|---|---|
| `STATUS` | `/` | C→S | — | uptime, session count, room count |
| `ROOMS` | `/` | C→S | — | every room: id, phase, members, owner |
| `PLAYERS` | `/` | C→S | — | every player: room, id, pid, owner, score, lines, name |
| `KICK` | `/room/{id}/player/{pid}` | C→S | — | `403` the victim, `SIGTERM`, reap |
| `SHUTDOWN` | `/` | C→S | — | reply `200`, then tear the daemon down |
| `RELOAD` | `/` | C→S | — | reserved, answers `501` |

Replies are JSON. The daemon never pushes on this plane — the client always
speaks first, and one connection carries exactly one command.

## D6. Notes

**`HOLD` is ours**, not in the handout. Everything else in D2/D3 is fixed by the
spec.

**Content-Type** is `application/tetris-command` on a client request with a
body, `application/tetris-state` on a server push.

**The auth budget has no status code of its own.** Once the attempts are
spent the final `401` or `404` has already gone out and the daemon simply
closes the connection. `429` is fullness, not rate limiting.

**`JOIN` is the only verb whose path room is not validated against the
session** — a `JOIN` asks for a room, it does not claim to be in one. Every
other room-shaped path is cross-checked and a mismatch is `403`.

**Path shapes are exact.** `path_ids` checks `%n` against the full string
length, so `/room/1/player/2/../../etc` does not route as room 1.

## D7. Status codes

| Code | Meaning here |
|---|---|
| `200` | accepted |
| `201` | `JOIN` created a new room |
| `400` | malformed request, or a command body outside the allowed words |
| `401` | no `Player-Id` header, or bad credentials |
| `403` | forged or stale identity, not the room owner, or kicked |
| `404` | no such account, or no such player to kick |
| `409` | real method at the wrong moment, already in a room or in none, already authenticated, name taken |
| `413` | frame over `SESSION_MAX_FRAME` — sent, then the fd closes |
| `429` | room or session table full (`REJECT_FULL`) |
| `500` | ours: a build or IPC failure, or a body over `g_body` |
| `501` | no such method, or `RELOAD` |
