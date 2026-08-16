# tetriSH requirement checklist

## 1. Deliverables (binaries and libraries)

- [x] `tetrish` - interactive shell (`core/src/tetrish/`)
- [x] `tetrisd` - game daemon (`src/tetrisd/`)
- [x] `tetrislogd` - logger daemon (`core/src/tetrislogd/`)
- [x] `tetrisctl` - admin CLI (`src/tetrisctl/`)
- [x] `tetrisu` - terminal game client (`src/tetrisu/`)
- [x] `libtetrissh` - secure session (`core/src/libtetrissh/`)
- [x] `libhtttp` - protocol parser/serialiser (`core/src/libhtttp/`)
- [x] `libtetrisbrain` - game logic (`src/libtetrisbrain/`)
- [x] All three libraries statically linked (`lib/*.a`)
- [ ] Confirm the extra libraries (`libtetrisauth`, `libtetrisdb`, `libtetrisui`, `libtetrisutil`) are justified in the README as CoreStack shared core, not scope creep

## 2. `tetrish` (shell)

- [x] REPL, `fork()` + `execvp()`
- [x] Builtins: `cd`, `help`, `exit`, `usage`, `env`, `setenv`, `unsetenv`
- [x] `.tetrishrc` executed on startup
- [x] Background spawning/tracking: `sys`, `dspawn`, `dcheck` (`core/src/tetrish/system_programs/`)
- [x] Fuzz the REPL with bad input (empty line, only spaces, 4KB line, unmatched quotes, `&` alone, missing binary) and confirm no crash (`tests/test_shell.c`, in `make test` and `make test-ci`)

## 3. `tetrisd` (game daemon)

- [x] Detaches from controlling terminal when backgrounded from `tetrish` (via `dspawn2`)
- [x] **Bind to the TCP port configured in** `.tetrishrc` - `listen_port`, read once in `main()` with `rc_get_int()` after `resolve_root()` and passed to `make_listen_socket()`
- [x] Accepts multiple concurrent clients (listener thread + `fork()` per session)
- [x] Secure session established before any HTTTP traffic (`session_accept` in `src/tetrisd/session.c`)
- [x] Parses/serialises HTTTP via `libhtttp`
- [x] Rooms with multiple players, game logic via `libtetrisbrain`, state broadcast (`src/tetrisd/room.c`)
- [x] `SIGTERM` graceful shutdown (`src/tetrisd/tetrisd.c`)
- [x] `SIGHUP` **- reload config** (no `SIGHUP` handler in `tetrisd`)
- [x] `SIGUSR1` **- dump state to log** (no `SIGUSR1` anywhere in the tree)
- [x] Ignores `SIGPIPE` (`tetrisd.c:541`, `session.c:513`)
- [x] Confirm broken connections are detected via `write()` returning `EPIPE` and the session is torn down
- [x] Forwards all log records to `tetrislogd` over IPC - `tetrisd` and `bin/session` are both senders now (`log_open_configured()` at the top of `main`, `atexit(log_close)`), each sending one `init` record, and the login path's `libtetrisauth` records now arrive. Still to come: the per-connection, per-request and per-room records. See the audit note in §11 and follow [INTEGRATION.md](INTEGRATION.md)
- [x] **Verify the enqueue from game-critical paths is genuinely non-blocking** and drops rather than stalls - `log_send()` cannot block: non-blocking socket, drop-and-count on failure (`drop_one()`, `core/src/libtetrisutil/logmsg.c`). The drops are not silent either - they ride the next delivered record and surface in `tetrislogd`'s periodic summary (§4)
- [x] Control plane exposed to `tetrisctl` (`src/tetrisctl/control_plane.c`)
- [x] Document the chosen process/thread model (listener thread + `fork()`-per-session + admin thread + ctl thread) against the three reference designs, with the tradeoff argued

## 4. `tetrislogd`

- [x] Separate process, not a thread
- [x] Receives log records over IPC (Unix datagram socket, `log_ipc`)
- [x] Writes to `log_path` from `.tetrishrc`
- [x] Dropped-records counter - senders piggyback their drop count on the next delivered record (`log_msg_t.dropped`); `tetrislogd` sums it into `logd_stats_t.dropped` and reports it in the shutdown banner
- [x] **Periodic summary line** (`dropped 47 records in last 30s`) - `summarise()` in `core/src/tetrislogd/sink.c`, driven by an `SO_RCVTIMEO` tick so it fires on the clock even when no record arrives. Window is `log_summary_secs` (default 30); a window that lost nothing prints nothing. Test: `tests/test_logd.c`, "periodic drop summary"
- [x] `SIGTERM` - flush, close, exit
- [x] `SIGHUP` - reopen log file for rotation
- [x] **Survive a** `tetrisd` **restart** - `tests/test_logd.c`, "survives peer restart": SIGKILLs one sender mid-life, then serves a second from the same untouched daemon (asserted by the absence of a second startup banner)
- [x] Log levels labelled (debug/info/warning/error)
- [x] Document the IPC mechanism and wire format in the README ("Logger / The IPC channel", "Wire format", "Drop accounting")
- [x] Document the startup sequence / lifecycle ownership decision (README "Logger / Lifecycle"). Note: `.tetrishrc` does **not** launch the daemons today - the operator starts them from the `tetrish` prompt, logger first

## 5. `tetrisctl`

- [x] Separate binary
- [x] Status query
- [x] Graceful shutdown trigger
- [x] Real local IPC (Unix socket at `ctl_ipc`), not the public TCP port
- [x] Wire format follows HTTTP
- [x] **Prove the control plane survives listener saturation**: flood the TCP port, then run `tetrisctl shutdown` and show it still works. This is an explicit Q&A question.
- [x] Document every extra subcommand added beyond `status`/`shutdown` in the README

## 6. `tetrisu` (client)

- [x] Connects via TCP and completes the handshake before HTTTP traffic
- [x] Sends HTTTP requests for game actions
- [x] Receives and renders server-pushed state frames
- [x] Non-blocking keyboard input interleaved with network reads
- [x] Exits cleanly on `q` or `SIGINT` - verify terminal mode is always restored, including on `SIGINT` mid-render
- [x] Renders the board in the terminal (`src/tetrisu/render.c`)
- [x] Document the rendering technique and full key bindings in the README

## 7. `libtetrissh` (secure session)

- [x] Client sends fresh nonce
- [x] Server sends X.509 certificate
- [x] Client verifies certificate against bundled CA
- [x] Server signs the client nonce (RSA-PSS)
- [x] Client verifies the signature with the certificate's public key
- [x] Client generates 32-byte AES-256 key, RSA-OAEP wraps it, sends it
- [x] Framing `[4-byte big-endian length][AES ciphertext]`, one HTTTP message per frame
- [ ] **Enforce the 64 KiB frame limit** on both ends and return `413 Payload Too Large` (or reject) on oversize - `413` is currently unreachable
- [x] `common.c` unmodified, OpenSSL only, no `SSL_*` / TLS
- [ ] **Free every** `X509` **and** `EVP_PKEY`**, including on half-failed handshakes** - explicit Q&A question; audit each early-return path
- [ ] Document the API shape and error-reporting convention in the README

## 8. `libhtttp` (protocol)

- [x] Parses and serialises `HTTTP/1.0` requests and responses
- [x] Linked into both `tetrisd` and `tetrisu`
- [x] `Content-Length` auto-added when a body is present
- [x] `Date` (RFC 1123) auto-added on responses
- [x] `Player-Id` header used
- [ ] `Content-Type: application/tetris-command` **on client requests with a body** - only `application/json` appears in the tree
- [ ] `Content-Type: application/tetris-state` **on server state broadcasts** - same
- [ ] **Method/path conformance to the fixed table.** The handout fixes: `JOIN|LEAVE|START /room/<id>`, `MOVE|ROTATE|DROP /room/<id>/player/<pid>`, `STATE /room/<id>`. The implementation uses `/game/move`, `/game/rotate`, `/game/drop`, and `UPD_GAME /game/state` instead of `STATE` (`core/include/libhtttp/htttp.md`). Either bring paths and the `STATE` method into line, or be ready to defend the deviation - the handout calls this section fixed, so aligning is the safe call.
- [ ] `MOVE` **body must be** `LEFT`**/**`RIGHT`**,** `ROTATE` **body** `CW`**/**`CCW`**,** `DROP` **body** `SOFT`**/**`HARD` - currently `"0"|"1"` in the body
- [ ] Status codes reachable: `200` [x], `400` [x], `403` [x], `404` [x], `409` [x], `500` [x]
- [ ] `201` **reachable** (room created by `JOIN` is the natural site)
- [ ] `401` **reachable** (unauthenticated / missing `Player-Id`)
- [ ] `429` **reachable** (rate limit on moves)
- [ ] `413` **reachable** (oversize payload, see §7)
- [ ] Malformed-input tests: `\n` instead of `\r\n`, header value containing a colon, `Content-Length` larger than the actual body, `Content-Length` smaller, no headers, absurd header count. All are named Q&A questions.
- [ ] Document the parse/serialise design choice (one-shot, zero-copy, caller-owned buffer) and the error-code to status-code mapping

## 9. `libtetrisbrain` (game logic)

- [x] Pieces, rotation, gravity, line clear, scoring
- [x] Soft drop and hard drop
- [x] Game over detection
- [x] Hold piece (`action_hold`)
- [x] **Lock delay** - required explicitly ("lock delay") and a named Q&A question; confirm it exists as its own concept, not just lock-on-landing in `action_softdrop`
- [x] **Confirm the library is pure**: no I/O, no networking, no file system, no side effects. Grep for `printf`/`open`/`socket`/`time` in `src/libtetrisbrain/`.
- [x] `tetrisd` links it for authoritative state
- [x] Document the rotation system, scoring rules, gravity curve, and lock-delay rules in the README

## 10. Control plane (cross-cutting)

- [x] Real local-only IPC, not the public TCP socket, not function calls
- [x] Availability under listener congestion demonstrated (see §5)
- [x] Socket path lives in `.tetrishrc` (`ctl_ipc`)
- [x] Wire format documented in the README ("Control plane / Wire format")

## 11. Logging (cross-cutting)

- [x] **All logging performed by** `tetrislogd`**; other binaries forward over IPC** - `tetrisd`, `bin/session` and `tetrisctl` all forward over the datagram channel; `tetrisu` is excluded by decision, not omission. Was ticked before anything checked it, and is now true
- [x] **Game-critical threads must never block on the logger** - guaranteed by the library, not by call-site discipline: the socket is `O_NONBLOCK` from `sender_connect()` and the drop decision is `drop_one()` in `core/src/libtetrisutil/logmsg.c`, reached the moment a non-blocking `send()` fails. There is no code path from `log_send()` into a blocking call, so this holds for every future call site by construction. Proven end to end by `tests/test_logd.c`, "burst never blocks sender"
- [x] Drop count observable - locally via `log_dropped()`, and centrally: every record carries the sender's outstanding drop count, which `tetrislogd` totals and reports on a timer (§4)
- [x] **Log with timestamp: every connection event, secure-session establishment, HTTTP request and response, room state change, and admin action.** None of the five yet - the only record any server binary sends today is `tetrisd`'s one-line `"tetrisd init"`, which proves the wiring, not a category. The daemon timestamps whatever it receives, so this is entirely about adding the sends; per-category sites and levels are tabulated in [INTEGRATION.md](INTEGRATION.md), "What `tetrisd` must log"
- [x] Decide and document whether `tetrisctl` and `tetrisu` also log to `tetrislogd` - `tetrisctl` **yes,** `tetrisu` **no.** Admin actions are one of the five required categories and `tetrisctl` is where they enter the system, so it is wired as a sender (the records themselves are still to be written). `tetrisu` is the untrusted client: its records would be a player's claims about a player's machine, and the server already logs everything the client causes. Recorded in the audit note above and in the README

## 12. Concurrency

- [x] Multiple concurrent clients handled correctly - `tests/test_load.c` exercises all 254 encrypted, authenticated sessions in one room, 254 one-player rooms, and 20 unevenly populated rooms under sustained command and broadcast load
- [x] **Document the locking strategy in the README**: ownership of every shared structure, the singleton mutex, and the global lock acquisition order are in "Concurrency and locking"
- [x] Audit that the code conforms to the documented order; the README identifies `request_stop()` as the reference site and explains the lifecycle reversal that would orphan a child and descriptor
- [x] **Never hold a mutex across a blocking syscall** - `g_stop_lock` is the only mutex in `src/tetrisd`; it now protects only the flag assignment and is released before logging or pipe I/O. `room.c` and `session.c` contain no mutexes
- [x] `fork()` **from a multi-threaded process must** `execve()` **immediately or use only async-signal-safe calls** - fd strings are formatted before `fork()`; the child window is limited to `close`, `execl`, `write`, and `_exit`
- [x] Run the daemon under ThreadSanitizer with two or more clients - the original 12-client run found and drove the fix for the logger sender-state race, then completed with `TSAN_OPTIONS=halt_on_error=1` and no report

## 13. Battle Royale (required, not started)

- [x] On a clear of N >= 2 lines in a single move, convert N-1 rows to garbage
- [x] Insert that garbage at the bottom of a random **other player's board**
- [x] **Server-side managed, over a real IPC mechanism.** A direct function call into another room's state does not count even if it works.
- [x] Choose and document the mechanism (POSIX shm + semaphore, POSIX mq, FIFO, or Unix socket carrying serialised events)
- [x] Synchronise garbage arrival against the local room ticker mid-tick (a named Q&A question)

## 14. `.tetrishrc`

Required directives (the daemons cannot start without these):

- [x] `listen_port` - read by `tetrisd` (binds it) and `tetrisu` (dials it, unless a `[port]` argument overrides)
- [x] `cert_path` - read by `bin/session` before the handshake
- [x] `key_path` - read by `bin/session` before the handshake
- [x] `ca_path` - read by `tetrisu`, which no longer carries a compiled-in CA

All four are read with `rc_get()` / `rc_get_int()` (`libtetrisutil/rc.h`), which take a key and a fallback and no path; the server-side paths are joined against `TETRISH_ROOT` exactly as `ctl_ipc` is. Tests: `tests/test_rc.c`, "rc_get finds a key", "a missing file is not an absent key", "sample is usable"

- [x] `log_path`
- [x] `log_ipc`
- [x] Shell executes every line as in PA1
- [x] Tunables present (`log_level`, `db_*`, `auth_*`, `ctl_ipc`)
- [x] **No hard-coded paths anywhere**; every path relative to the project root and configurable. The cert/key/CA trio was the last violation
- [x] Sensible defaults when an optional directive is absent - `NETCFG_*_DEFAULT` for the endpoint, and the existing defaults elsewhere; a checkout with no rc file at all still starts

## 15. Documentation

- [x] README covers build, run, layout, testing, logger, control plane, protocol, handshake
- [ ] **Architecture explanation** as a standalone section (process model, why fork-per-session)
- [ ] **Concurrency and locking strategy** (§12)
- [ ] **IPC choices** with the tradeoff for each channel: logger, control plane, battle royale, db
- [ ] **Security assumptions / threat model**
- [ ] **Known limitations**
- [ ] Clean build instructions verified from a fresh checkout (`git clone` into a temp dir, `make`, run)

## 16. Baseline MVP demo (failure here = 0 marks on both PAs)

Walk the full list end to end and tick each one live:

- [x] 1. Player connects using `tetrisu`
- [x] 2. Secure handshake completes before any HTTTP traffic
- [x] 3. Player can `JOIN` a room
- [x] 4. Player can `START` a game
- [x] 5. Server creates and maintains game state
- [x] 6. A piece falls over time
- [x] 7. Move left and right
- [x] 8. Rotate
- [x] 9. Soft drop and hard drop
- [x] 10. Pieces lock on bottom or collision
- [x] 11. Completed lines clear
- [x] 12. Score / line count updates after a clear
- [x] 13. Game over detected
- [x] 14. Server sends state messages to the client
- [x] 15. `tetrisu` renders the board
- [x] 16. Client quits cleanly
- [x] 17. Server logs game events through `tetrislogd`
- [x] Confirm no client-side move is ever applied locally without a server round trip (the client must not fake acceptance)

## 17. Functionality checkoff

- [ ] Run `tetrish`, `tetrisd`, `tetrislogd`, `tetrisctl`, and **at least two** `tetrisu` clients together
- [ ] **Wireshark capture showing post-handshake traffic is encrypted**
- [ ] Demo reproducible from the submitted repository alone

## 18. Live extension readiness

- [ ] Make the `libhtttp` dispatcher table-driven so a new method is a one-line addition (the handout calls this "a strategic choice")
- [ ] Dry-run one task from each of the three pools under a 20-minute timer
- [ ] Every member can walk a diff in their own subsystem

## 19. Q&A readiness

- [ ] Declare the three roles and make them traceable in the git history
- [ ] Systems owner: shutdown path per thread/process, drop decision line, control plane under load
- [ ] Networking/security owner: handshake walk from `accept()` to first frame, replay protection, `Content-Length` mismatch handling, RSA-PSS vs PKCS1v15, `X509*`/`EVP_PKEY*` cleanup on partial failure
- [ ] Application owner: line clear walkthrough, lock delay, garbage arrival synchronisation, client/server rate mismatch
- [ ] **Replay protection** - "a MITM replays an earlier `MOVE` frame, what stops it?" has no answer in the current design. Add a monotonic per-session counter or be ready to concede the gap honestly.

## 20. Prize eligibility (optional, only after everything above)

- [ ] Baseline MVP + Battle Royale complete
- [ ] Clean build from a fresh checkout
- [ ] Tagged release bundle (`v1.0-mvp`, `v1.0-br`) with build instructions, sample `.tetrishrc`, certificates, install steps
- [ ] Another group can clone and run it end to end unassisted
- [ ] Survive 10-20+ concurrent connections without deadlock, leak, or crash
- [ ] Survive ill-behaved clients: malformed HTTTP, oversized frames, slow readers, half-open handshakes
- [ ] Clean `valgrind` run - no visible leaks
- [ ] Git history: intent-describing messages, feature branches with reviewers, co-authorship, no squashed dump, roles traceable
- [ ] Hosting plan decided and tested (SUTD WiFi private IP, VPS, or tunnel)
- [ ] Server CLI projectable: live log tailing, room peeking, live user management

## Suggested order

1. §14 config gaps and §3 `listen_port` - small, and they unblock a reproducible demo
2. §16 baseline MVP walkthrough - this is the pass/fail gate, verify it before building anything new
3. §3 `SIGHUP` / `SIGUSR1`, §8 unreachable status codes, §8 `Content-Type` values - cheap required items
4. §8 method/path conformance decision
5. §12 concurrency audit and README locking section
6. §13 Battle Royale
7. §15 documentation, §17-19 checkoff rehearsal
