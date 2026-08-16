# `src/tetrisu` — the call graph

Every function in the six files of `src/tetrisu`, boxed by the file it lives in,
with an edge for every call — plus the ones that leave the folder for the shared
libraries. All six link into a single binary, `bin/tetrisu` (`Makefile:167`), so
unlike `src/tetrisctl` there is no linker seam running through the folder.

| | |
|---|---|
| functions | **58** across six files, 2 260 lines |
| call edges | **88** internal, no cycles |
| single caller | **39** could be pasted into their one call site |
| entry points | **2** — `main`, and `handle_sigint` from the kernel |

**Reading the diagrams.** A solid arrow is a direct call; a thick arrow crosses
a boundary (socket, signal). Node fill is the file the function is defined in.

---

## 1. File level — who depends on whom

`net.c` is the one leaf: everything calls into it and it calls nothing in the
folder back. The three screen files are siblings that never call each other
except through `tetrisu.c`'s loop — with one exception, `screen_main_menu`,
which invokes two other screens directly.

```mermaid
flowchart TB
  subgraph BU["bin/tetrisu"]
    direction TB
    F_main["tetrisu.c<br/>argv, screen loop, signals"]
    F_conn["screens.c<br/>connect, menu, join, history, wait"]
    F_auth["screen_auth.c<br/>guest / login / register"]
    F_game["game_screen.c<br/>the live round"]
    F_rend["render.c<br/>board + preview to ncurses"]
    F_net["net.c<br/>Client, framing, events"]
  end
  F_srv["tetrisd<br/><i>separate process</i>"]

  F_main -->|"drives every screen"| F_conn
  F_main -->|"the auth gate"| F_auth
  F_main -->|"the round"| F_game
  F_main -->|"connect / disconnect"| F_net
  F_conn -->|"verbs + poll loop"| F_net
  F_auth -->|"verbs + poll loop"| F_net
  F_game -->|"verbs + poll loop"| F_net
  F_game -->|"one frame"| F_rend
  F_net ==>|"libtetrissh session, HTTTP over TCP"| F_srv

  classDef u fill:#dcefeb,stroke:#3d8b82,color:#0d2b28
  classDef netc fill:#f0e7d8,stroke:#9c8149,color:#332816
  classDef srv fill:#e4e0f4,stroke:#6a5cb5,color:#1e1840
  class F_main,F_conn,F_auth,F_game,F_rend u
  class F_net netc
  class F_srv srv
```

One binary, one direction. `render.c` is reached only from `game_screen.c`, and
knows nothing about the network — it is handed a `GameState` and draws it.

---

## 2. The screen loop — `tetrisu.c`, the spine

Every screen is a function that returns a `ScreenResult` and `main` decides
where to go next. That is the whole control structure of the client — there is
no screen stack and no dispatch table, so this graph *is* the state machine.

```mermaid
flowchart LR
  SIG(["SIGINT"])

  subgraph A["tetrisu.c"]
    direction TB
    A_main["main<br/><i>the screen loop</i>"]
    A_usage["usage"]
    A_recon["offer_reconnect"]
    A_sig["handle_sigint"]
  end

  subgraph B["screens.c"]
    direction TB
    B_conn["screen_connect"]
    B_menu["screen_main_menu"]
    B_join["screen_join_room"]
    B_hist["screen_history"]
    B_wait["screen_wait_start"]
  end

  subgraph C["screen_auth.c"]
    C_auth["screen_auth"]
  end

  subgraph D["game_screen.c"]
    D_game["screen_game"]
  end

  subgraph E["net.c — lifecycle"]
    direction TB
    E_dis["client_disconnect"]
  end

  SIG ==>|"signal(SIGINT, ...)"| A_sig
  A_main --> A_usage
  A_main --> A_recon
  A_main --> E_dis
  A_main -->|"1. host + port"| B_conn
  A_main -->|"2. auth gate"| C_auth
  A_main -->|"3. lobby"| B_menu
  A_main -->|"4. room, pre-round"| B_wait
  A_main -->|"5. the round"| D_game
  B_menu -->|"browse"| B_hist
  B_menu -->|"pick a room"| B_join

  classDef fa fill:#d9edea,stroke:#3f8a80,color:#0c2b27
  classDef fb fill:#e0e8f4,stroke:#54759f,color:#122031
  classDef fc fill:#e9e0f0,stroke:#84669f,color:#281b31
  classDef fd fill:#e2ecda,stroke:#688a4f,color:#1f2b16
  classDef fe fill:#f0e7d8,stroke:#9c8149,color:#332816
  classDef ev fill:#f4e6d5,stroke:#ac7c40,color:#3a2612
  class A_main,A_usage,A_recon,A_sig fa
  class B_conn,B_menu,B_join,B_hist,B_wait fb
  class C_auth fc
  class D_game fd
  class E_dis fe
  class SIG ev
```

`main` calls five screens; only `screen_main_menu` calls screens of its own, and
both of those return to it rather than to `main`. `handle_sigint` is the second
entry point — nothing in the program calls it.

---

## 3. Inside the screens — `screens.c` and `screen_auth.c`

The two screen files and the `net.c` entry points they reach for. Each keeps a
private `say` helper — same name, same signature, two separate `static`
functions with no sharing between them.

```mermaid
flowchart LR
  subgraph B["screens.c"]
    direction TB
    B_conn["screen_connect"]
    B_menu["screen_main_menu"]
    B_join["screen_join_room"]
    B_hist["screen_history"]
    B_wait["screen_wait_start"]
    B_status["status"]
    B_say["say<br/><i>static, screens.c</i>"]
    B_waithist["wait_history_reply"]
    B_build["build_history_lines"]
    B_drawwait["draw_wait"]
  end

  subgraph C["screen_auth.c"]
    direction TB
    C_auth["screen_auth"]
    C_guest["guest_flow"]
    C_cred["credential_flow"]
    C_send["send_guest_and_wait"]
    C_waitauth["wait_auth_reply"]
    C_say["say<br/><i>static, screen_auth.c</i>"]
    C_warn["warn_line"]
    C_vu["valid_username"]
    C_vp["valid_password"]
  end

  subgraph N["net.c — entry points"]
    direction TB
    N_conn["client_connect"]
    N_join["client_join"]
    N_leave["client_leave"]
    N_start["client_start"]
    N_hist["client_history"]
    N_login["client_login"]
    N_reg["client_register"]
    N_guest["client_guest"]
    N_fd["client_fd"]
    N_svc["client_service"]
    N_phase["client_phase_str"]
  end

  B_conn --> N_conn
  B_conn --> B_say
  B_conn --> B_status
  B_status --> N_phase
  B_menu --> N_join
  B_menu --> B_say
  B_menu --> B_hist
  B_menu --> B_join
  B_join --> N_join
  B_join --> B_say
  B_hist --> N_hist
  B_hist --> B_build
  B_hist --> B_say
  B_hist --> B_waithist
  B_waithist --> N_fd
  B_waithist --> N_svc
  B_wait --> N_fd
  B_wait --> N_svc
  B_wait --> N_start
  B_wait --> N_leave
  B_wait --> B_drawwait
  B_wait --> B_say

  C_auth --> C_guest
  C_auth --> C_cred
  C_guest --> C_send
  C_guest --> C_say
  C_cred --> N_login
  C_cred --> N_reg
  C_cred --> C_send
  C_cred --> C_waitauth
  C_cred --> C_say
  C_cred --> C_warn
  C_cred --> C_vu
  C_cred --> C_vp
  C_send --> N_guest
  C_send --> C_waitauth
  C_waitauth --> N_fd
  C_waitauth --> N_svc

  classDef fb fill:#e0e8f4,stroke:#54759f,color:#122031
  classDef fc fill:#e9e0f0,stroke:#84669f,color:#281b31
  classDef fe fill:#f0e7d8,stroke:#9c8149,color:#332816
  class B_conn,B_menu,B_join,B_hist,B_wait,B_status,B_say,B_waithist,B_build,B_drawwait fb
  class C_auth,C_guest,C_cred,C_send,C_waitauth,C_say,C_warn,C_vu,C_vp fc
  class N_conn,N_join,N_leave,N_start,N_hist,N_login,N_reg,N_guest,N_fd,N_svc,N_phase fe
```

`client_fd` and `client_service` always appear together — that pair is the poll
loop, written out three times here and once more in `screen_game`.
`credential_flow` falls through to `send_guest_and_wait`, which is how a failed
login can still end up in a guest session.

---

## 4. `net.c`, `render.c`, `game_screen.c` — the leaves

The three files nothing in the folder calls upward from. `net.c` is a funnel:
twenty-five functions, every outbound one ending at `send_cmd`, and every
inbound byte arriving through `net_service`.

```mermaid
flowchart LR
  subgraph N["net.c"]
    direction TB
    N_conn["client_connect"]
    N_mks["make_connect_socket"]
    N_dis["client_disconnect"]
    N_clear["clear_auth"]
    N_fd["client_fd"]
    N_cmd["send_cmd<br/><i>8 callers</i>"]
    N_room["room_path"]
    N_player["player_path"]
    N_word["send_word"]
    N_join["client_join"]
    N_leave["client_leave"]
    N_start["client_start"]
    N_move["client_move"]
    N_rot["client_rotate"]
    N_drop["client_drop"]
    N_hold["client_hold"]
    N_hist["client_history"]
    N_auth["send_auth"]
    N_login["client_login"]
    N_reg["client_register"]
    N_guest["client_guest"]
    N_fold["fold_auth_reply"]
    N_svc["client_service"]
    N_nsvc["net_service"]
    N_phase["client_phase_str"]
  end

  subgraph G["game_screen.c"]
    direction TB
    G_game["screen_game"]
    G_key["handle_key"]
    G_foot["draw_footer"]
  end

  subgraph R["render.c"]
    direction TB
    R_render["render_game"]
    R_init["init_colors"]
    R_cell["cell_at"]
    R_dcell["draw_cell"]
    R_pale["draw_pale_cell"]
    R_prev["draw_preview"]
    R_color["piece_color"]
  end

  N_conn --> N_mks
  N_dis --> N_clear
  N_word --> N_player
  N_word --> N_cmd
  N_join --> N_room
  N_join --> N_cmd
  N_leave --> N_room
  N_leave --> N_cmd
  N_start --> N_room
  N_start --> N_cmd
  N_move --> N_word
  N_rot --> N_word
  N_drop --> N_word
  N_hold --> N_player
  N_hold --> N_cmd
  N_hist --> N_cmd
  N_auth --> N_cmd
  N_login --> N_auth
  N_reg --> N_auth
  N_guest --> N_cmd
  N_svc --> N_nsvc
  N_svc --> N_fold
  N_svc --> N_clear

  G_game --> G_key
  G_game --> G_foot
  G_game --> N_fd
  G_game --> N_svc
  G_game --> N_leave
  G_game --> R_render
  G_key --> N_move
  G_key --> N_rot
  G_key --> N_drop
  G_key --> N_hold

  R_render --> R_init
  R_render --> R_cell
  R_render --> R_dcell
  R_render --> R_pale
  R_render --> R_prev
  R_prev --> R_dcell
  R_prev --> R_pale
  R_dcell --> R_color
  R_pale --> R_color

  classDef fe fill:#f0e7d8,stroke:#9c8149,color:#332816
  classDef fg fill:#e2ecda,stroke:#688a4f,color:#1f2b16
  classDef fr fill:#f2e0e2,stroke:#a86b70,color:#331a1c
  class N_conn,N_mks,N_dis,N_clear,N_fd,N_cmd,N_room,N_player,N_word,N_join,N_leave,N_start,N_move,N_rot,N_drop,N_hold,N_hist,N_auth,N_login,N_reg,N_guest,N_fold,N_svc,N_nsvc,N_phase fe
  class G_game,G_key,G_foot fg
  class R_render,R_init,R_cell,R_dcell,R_pale,R_prev,R_color fr
```

`send_cmd` has 8 callers and is the only place a request is serialized;
`client_service` wraps `net_service` and is the only place an event is
interpreted. Those two are the entire read and write surface of the client.

---

## 5. The load-bearing half — single-caller functions removed

Same binary, with every function that has exactly one caller deleted and its
edges spliced through to that caller. Those 39 could be pasted into their single
call site without changing a symbol anyone else can see. What survives is the 17
functions that are genuinely shared, plus the two entry points nothing calls.

**No exceptions this time.** Unlike `src/tetrisctl`, nothing in `src/tetrisu` is
reached through a function pointer — there is no callback type and no dispatch
table in the folder, so a single static caller really does mean inlineable.

```mermaid
flowchart LR
  subgraph A["tetrisu.c"]
    direction TB
    A_main["main<br/><i>0 callers</i>"]
    A_sig["handle_sigint<br/><i>0 — kernel</i>"]
  end

  subgraph B["screens.c"]
    B_say["say<br/><i>5</i>"]
  end

  subgraph C["screen_auth.c"]
    direction TB
    C_say["say<br/><i>2</i>"]
    C_wait["wait_auth_reply<br/><i>2</i>"]
    C_send["send_guest_and_wait<br/><i>2</i>"]
  end

  subgraph N["net.c"]
    direction TB
    N_cmd["send_cmd<br/><i>8</i>"]
    N_fd["client_fd<br/><i>4</i>"]
    N_svc["client_service<br/><i>4</i>"]
    N_room["room_path<br/><i>3</i>"]
    N_word["send_word<br/><i>3</i>"]
    N_clear["clear_auth<br/><i>2</i>"]
    N_player["player_path<br/><i>2</i>"]
    N_join["client_join<br/><i>2</i>"]
    N_leave["client_leave<br/><i>2</i>"]
    N_auth["send_auth<br/><i>2</i>"]
  end

  subgraph R["render.c"]
    direction TB
    R_dcell["draw_cell<br/><i>2</i>"]
    R_pale["draw_pale_cell<br/><i>2</i>"]
    R_color["piece_color<br/><i>2</i>"]
  end

  A_main --> B_say
  A_main --> C_say
  A_main --> C_send
  A_main --> C_wait
  A_main --> N_cmd
  A_main --> N_fd
  A_main --> N_svc
  A_main --> N_room
  A_main --> N_word
  A_main --> N_clear
  A_main --> N_player
  A_main --> N_join
  A_main --> N_leave
  A_main --> N_auth
  A_main --> R_dcell
  A_main --> R_pale

  C_send --> N_cmd
  C_send --> C_wait
  C_wait --> N_fd
  C_wait --> N_svc

  N_word --> N_player
  N_word --> N_cmd
  N_join --> N_room
  N_join --> N_cmd
  N_leave --> N_room
  N_leave --> N_cmd
  N_auth --> N_cmd
  N_svc --> N_clear

  R_dcell --> R_color
  R_pale --> R_color

  classDef fa fill:#d9edea,stroke:#3f8a80,color:#0c2b27
  classDef fb fill:#e0e8f4,stroke:#54759f,color:#122031
  classDef fc fill:#e9e0f0,stroke:#84669f,color:#281b31
  classDef fe fill:#f0e7d8,stroke:#9c8149,color:#332816
  classDef fr fill:#f2e0e2,stroke:#a86b70,color:#331a1c
  class A_main,A_sig fa
  class B_say fb
  class C_say,C_wait,C_send fc
  class N_cmd,N_fd,N_svc,N_room,N_word,N_clear,N_player,N_join,N_leave,N_auth fe
  class R_dcell,R_pale,R_color fr
```

`game_screen.c` disappears completely — all three of its functions have exactly
one caller. `screens.c` is reduced to its private `say`: every screen in the
file is called from exactly one place.

**What the reduction says.**

- `net.c` keeps 10 of 25 and every survivor is internal plumbing — `send_cmd`,
  `send_word`, `room_path`, `player_path`, `send_auth`. The public `client_*`
  verbs are almost all single-caller facades over it, which is the shape of a
  protocol wrapper doing its job.
- `game_screen.c` vanishes and `screens.c` keeps only `say`. The screen layer is
  a straight-line sequence, not a graph — which is exactly what section 2 shows.
- The two functions with the same name are both survivors. `screens.c`'s `say`
  has 5 callers, `screen_auth.c`'s has 2 — duplicated deliberately, one per
  file, rather than promoted to a shared header.
- `send_cmd` at 8 callers is the single busiest function in the folder;
  `client_fd` and `client_service` tie at 4 because the poll loop is written out
  four separate times.

| File | Lines | Functions | Single caller | Kept |
|---|---:|---:|---:|---:|
| `net.c` | 524 | 25 | 15 | 10 |
| `screens.c` | 570 | 10 | 9 | 1 |
| `screen_auth.c` | 309 | 9 | 6 | 3 |
| `render.c` | 385 | 7 | 4 | 3 |
| `tetrisu.c` | 245 | 4 | 2 | 2 |
| `game_screen.c` | 227 | 3 | 3 | 0 |
| **total** | **2 260** | **58** | **39** | **19** |

---

## 6. Calls that leave the folder

`libtetrisui` is the widest dependency — it is touched by every file that draws.

```mermaid
flowchart LR
  subgraph SRC["src/tetrisu"]
    direction TB
    E_cmd["send_cmd"]
    E_nsvc["net_service"]
    E_conn["client_connect"]
    E_dis["client_disconnect"]
    E_draw["screens.c / screen_auth.c<br/>every drawing function"]
    E_render["render.c<br/>cell_at, draw_preview"]
    E_build["build_history_lines"]
    E_vu["valid_username"]
    E_cred["credential_flow"]
    E_main["main"]
    E_sig["handle_sigint"]
  end

  H_sh["libtetrissh/tetrissh.h<br/>session_connect, session_send,<br/>session_recv, session_close"]
  H_htttp["libhtttp/htttp.h<br/>header_set, serialize_request,<br/>parse_request, parse_response"]
  H_ui["libtetrisui/tetrisui.h<br/>menu, form, list_view, message,<br/>set_status, draw_status_bar,<br/>progress_step / _end, shutdown"]
  H_brain["libtetrisbrain<br/>piece_size, piece_filled,<br/>tetrisbrain_ghost"]
  H_hview["tetrisd/historyview.h<br/>history_lines"]
  H_name["libtetrisutil/name.h<br/>user_name_ok"]
  H_rc["libtetrisutil/rc.h<br/>rc_get, rc_get_int"]
  H_auth["libtetrisauth/auth.h<br/>auth_retry_handler"]
  H_admin["tetrisd/adminmsg.h<br/>REJECT_* status codes"]

  E_conn --> H_sh
  E_dis --> H_sh
  E_cmd --> H_sh
  E_nsvc --> H_sh
  E_cmd --> H_htttp
  E_nsvc --> H_htttp
  E_draw --> H_ui
  E_main --> H_ui
  E_sig --> H_ui
  E_render --> H_brain
  E_build --> H_hview
  E_build --> H_admin
  E_vu --> H_name
  E_cred --> H_auth
  E_main --> H_rc

  classDef lib fill:#f0ede0,stroke:#9a8d5f,color:#332e18
  class H_sh,H_htttp,H_ui,H_brain,H_hview,H_name,H_rc,H_auth,H_admin lib
```

`net.c` is the only file that touches `libtetrissh` or `libhtttp`, and no screen
file touches either — the protocol is sealed behind the `Client` API in
`include/tetrisu/client.h`.

---

## 7. Function index

Line numbers as of the working tree at the time of writing. **In** is the number
of distinct functions that call it; **1** means it was dropped from section 5.

### `tetrisu.c` — 245 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `main` | 67 | 0 | usage, offer_reconnect, client_disconnect, screen_connect, screen_auth, screen_main_menu, screen_wait_start, screen_game | rc_get, rc_get_int, tetrisui_message, tetrisui_shutdown |
| `handle_sigint` | 60 | 0 | — *registered with `signal(SIGINT)`* | tetrisui_shutdown |
| `usage` | 41 | 1 | — | — |
| `offer_reconnect` | 52 | 1 | — | — |

### `net.c` — 524 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `send_cmd` | 121 | 8 | — | htttp_header_set, htttp_serialize_request, session_send |
| `client_fd` | 113 | 4 | — | — |
| `client_service` | 489 | 4 | net_service, fold_auth_reply, clear_auth | — |
| `room_path` | 166 | 3 | — | — |
| `send_word` | 178 | 3 | player_path, send_cmd | — |
| `clear_auth` | 93 | 2 | — | — |
| `player_path` | 171 | 2 | — | — |
| `client_join` | 186 | 2 | room_path, send_cmd | — |
| `client_leave` | 198 | 2 | room_path, send_cmd | — |
| `send_auth` | 264 | 2 | send_cmd | — |
| `make_connect_socket` | 36 | 1 | — | socket, connect |
| `client_connect` | 59 | 1 | make_connect_socket | session_connect |
| `client_disconnect` | 101 | 1 | clear_auth | session_close |
| `client_start` | 224 | 1 | room_path, send_cmd | — |
| `client_move` | 231 | 1 | send_word | — |
| `client_rotate` | 235 | 1 | send_word | — |
| `client_drop` | 239 | 1 | send_word | — |
| `client_hold` | 243 | 1 | player_path, send_cmd | — |
| `client_history` | 250 | 1 | send_cmd | — |
| `client_login` | 282 | 1 | send_auth | — |
| `client_register` | 287 | 1 | send_auth | — |
| `client_guest` | 292 | 1 | send_cmd | — |
| `fold_auth_reply` | 308 | 1 | — | — |
| `net_service` | 327 | 1 | — | session_recv, htttp_parse_request, htttp_parse_response |
| `client_phase_str` | 508 | 1 | — | — |

### `screens.c` — 570 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `say` | 41 | 5 | — | tetrisui_message |
| `status` | 35 | 1 | client_phase_str | tetrisui_set_status |
| `screen_connect` | 49 | 1 | client_connect, say, status | tetrisui_form, tetrisui_progress_step / _end |
| `screen_main_menu` | 123 | 1 | client_join, say, screen_history, screen_join_room | tetrisui_menu |
| `wait_history_reply` | 160 | 1 | client_fd, client_service | poll, tetrisui_progress_step / _end |
| `build_history_lines` | 201 | 1 | — | history_lines, REJECT_* codes |
| `screen_history` | 263 | 1 | client_history, build_history_lines, say, wait_history_reply | tetrisui_list_view |
| `screen_join_room` | 291 | 1 | client_join, say | — |
| `draw_wait` | 325 | 1 | — | tetrisui_draw_status_bar |
| `screen_wait_start` | 425 | 1 | client_fd, client_service, client_start, client_leave, draw_wait, say | poll |

### `screen_auth.c` — 309 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `say` | 31 | 2 | — | tetrisui_message |
| `wait_auth_reply` | 88 | 2 | client_fd, client_service | poll, tetrisui_progress_step / _end |
| `send_guest_and_wait` | 129 | 2 | client_guest, wait_auth_reply | — |
| `valid_username` | 41 | 1 | — | user_name_ok |
| `valid_password` | 50 | 1 | — | — |
| `warn_line` | 67 | 1 | — | — |
| `guest_flow` | 138 | 1 | say, send_guest_and_wait | tetrisui_set_status |
| `credential_flow` | 162 | 1 | client_login, client_register, say, send_guest_and_wait, wait_auth_reply, warn_line, valid_username, valid_password | auth_retry_handler, tetrisui_set_status |
| `screen_auth` | 292 | 1 | guest_flow, credential_flow | tetrisui_menu |

### `render.c` — 385 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `piece_color` | 118 | 2 | — | — |
| `draw_pale_cell` | 149 | 2 | piece_color | ncurses |
| `draw_cell` | 163 | 2 | piece_color | ncurses |
| `init_colors` | 78 | 1 | — | ncurses |
| `cell_at` | 226 | 1 | — | piece_filled |
| `draw_preview` | 247 | 1 | draw_cell, draw_pale_cell | piece_size |
| `render_game` | 268 | 1 | init_colors, cell_at, draw_cell, draw_pale_cell, draw_preview | tetrisbrain_ghost |

### `game_screen.c` — 227 lines · every function single-caller

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `screen_game` | 89 | 1 | handle_key, draw_footer, client_fd, client_service, client_leave, render_game | poll |
| `handle_key` | 27 | 1 | client_move, client_rotate, client_drop, client_hold | — |
| `draw_footer` | 76 | 1 | — | ncurses |
