# `src/tetrisctl` — the call graph

Every function in the six files of `src/tetrisctl`, boxed by the file it lives
in, with an edge for every call — including the ones that leave the folder, and
the one that leaves the process.

**The folder is not one binary.** Five files link into `bin/tetrisctl`;
`control_plane.c` links into `bin/tetrisd` and shares no symbol with the other
five (`Makefile:139` versus `Makefile:157`). It sits here because it is the
other half of the same protocol, not because it ships in the same executable.

| | files | functions | call edges |
|---|---|---|---|
| `bin/tetrisctl` | `tetrisctl.c` `ctl_client.c` `ctl_lifecycle.c` `ctl_logtail.c` `ctl_tui.c` | 71 | 135 |
| `bin/tetrisd` | `control_plane.c` | 14 | — |

`src/tetrisd/history.c` is also linked into `bin/tetrisctl`, for the console's
score panel.

**Reading the diagrams.** A solid arrow is a direct call; a dotted arrow is a
call through a function pointer; a thick arrow crosses a process boundary. Node
fill is the file the function is defined in.

---

## 1. File level — who depends on whom

`ctl_client.c` and `ctl_lifecycle.c` are the two leaf modules; both front ends
sit on top of them, and neither front end knows about the other. The only edge
between the two binaries is the socket.

```mermaid
flowchart TB
  subgraph BC["bin/tetrisctl"]
    direction TB
    F_cli["tetrisctl.c<br/>argv front end"]
    F_tui["ctl_tui.c<br/>ncurses console"]
    F_client["ctl_client.c<br/>request + JSON decode"]
    F_life["ctl_lifecycle.c<br/>spawn, probe, signal"]
    F_log["ctl_logtail.c<br/>log tail ring"]
  end
  subgraph BD["bin/tetrisd"]
    F_cp["control_plane.c<br/>ctl thread + dispatch"]
    F_dmain["tetrisd.c<br/>admin thread"]
  end
  F_ext["src/tetrisd/history.c<br/>score reads"]

  F_cli -->|"one verb, formatted"| F_client
  F_cli -->|"start / stop"| F_life
  F_cli -->|"bare argv on a tty"| F_tui
  F_tui -->|"refresh, kick, shutdown"| F_client
  F_tui -->|"every daemon action"| F_life
  F_tui -->|"right-hand log pane"| F_log
  F_tui -->|"best + recent scores"| F_ext
  F_life -->|"probe = does STATUS answer"| F_client
  F_client ==>|"AF_UNIX, 4-byte prefix + HTTTP"| F_cp
  F_cp -->|"CtlReq over pipe"| F_dmain
  F_dmain -->|"ctl_dispatch"| F_cp

  classDef ctl fill:#dcefeb,stroke:#3d8b82,color:#0d2b28
  classDef dmn fill:#e4e0f4,stroke:#6a5cb5,color:#1e1840
  classDef ext fill:#f0ede0,stroke:#9a8d5f,color:#332e18
  class F_cli,F_tui,F_client,F_life,F_log ctl
  class F_cp,F_dmain dmn
  class F_ext ext
```

---

## 2. The argv front end — `tetrisctl.c`, outward

One graph per front end, so no edge has to cross the whole picture. Here:
everything `tetrisctl.c` defines, and the entry points it reaches for. The leaf
modules' own internals are section 4.

```mermaid
flowchart LR
  subgraph A["tetrisctl.c"]
    direction TB
    A_main["main"]
    A_usage["usage"]
    A_cs["cmd_start"]
    A_cst["cmd_stop"]
    A_step["cli_step"]
    A_fs["format_status"]
    A_fr["format_rooms"]
    A_fp["format_players"]
  end
  subgraph B["ctl_client.c — entry points"]
    direction TB
    B_req["ctl_request"]
    B_dstat["ctl_decode_status"]
    B_drooms["ctl_decode_rooms"]
    B_dplay["ctl_decode_players"]
    B_err["ctl_strerror"]
    B_up["ctl_fmt_uptime"]
  end
  subgraph C["ctl_lifecycle.c — entry points"]
    direction TB
    C_parse["ctl_daemon_parse"]
    C_name["ctl_daemon_name"]
    C_start["ctl_start"]
    C_stop["ctl_stop_logd"]
  end
  subgraph D["ctl_tui.c"]
    D_run["ctl_tui_run"]
  end

  A_main --> A_usage
  A_main --> A_cs
  A_main --> A_cst
  A_main --> A_fs
  A_main --> A_fr
  A_main --> A_fp
  A_main -->|"bare argv on a tty"| D_run
  A_main --> B_req
  A_main --> B_dstat
  A_main --> B_drooms
  A_main --> B_dplay
  A_main --> B_err
  A_fs --> B_up
  A_cs --> C_parse
  A_cs --> C_name
  A_cs --> C_start
  A_cst --> C_parse
  A_cst --> C_stop
  A_cst --> B_req
  A_cst --> B_err
  C_start -.->|"CtlStepFn"| A_step
  C_stop -.->|"CtlStepFn"| A_step

  classDef fa fill:#d9edea,stroke:#3f8a80,color:#0c2b27
  classDef fb fill:#f2e9da,stroke:#9c8149,color:#332816
  classDef fc fill:#e9e0f0,stroke:#84669f,color:#281b31
  classDef fd fill:#e0e8f4,stroke:#54759f,color:#122031
  class A_main,A_usage,A_cs,A_cst,A_step,A_fs,A_fr,A_fp fa
  class B_req,B_dstat,B_drooms,B_dplay,B_err,B_up fb
  class C_parse,C_name,C_start,C_stop fc
  class D_run fd
```

`main` is the only caller of everything else in the file. `cli_step` is the one
thing called from outside it, and only through a function pointer that
`ctl_lifecycle.c` was handed.

---

## 3. The console — `ctl_tui.c`, outward

All 25 functions, split into the three things the file actually does — draw a
frame, run an action, gather data — plus the entry points each reaches for.

```mermaid
flowchart LR
  T_run["ctl_tui_run<br/><i>event loop</i>"]

  subgraph DR["ctl_tui.c — draw a frame"]
    direction TB
    T_draw["draw"]
    T_right["draw_right"]
    T_dash["build_dashboard"]
    T_recent["build_recent"]
    T_emit["emit"]
    T_best["get_individual_best_score"]
    T_dis["disabled_reason"]
    T_wb["windows_build"]
    T_wf["windows_free"]
  end

  subgraph AC["ctl_tui.c — run an action"]
    direction TB
    T_start["do_start"]
    T_startall["do_start_all"]
    T_stopd["do_stop_tetrisd"]
    T_slnow["stop_logd_now"]
    T_stoplogd["do_stop_logd"]
    T_stopall["do_stop_all"]
    T_db["do_db"]
    T_kick["do_kick"]
    T_say["say"]
    T_after["after_page"]
    T_step["tui_step"]
  end

  subgraph GA["ctl_tui.c — gather data"]
    direction TB
    T_refall["refresh_all"]
    T_refdb["refresh_db"]
    T_move["move_sel"]
    T_now["now_ms"]
  end

  subgraph LM["leaf modules — entry points"]
    direction TB
    L_req["ctl_request"]
    L_refresh["ctl_refresh"]
    L_err["ctl_strerror"]
    L_up["ctl_fmt_uptime"]
    L_start["ctl_start"]
    L_stop["ctl_stop_logd"]
    L_probe["ctl_probe"]
    L_pdb["ctl_probe_db"]
    L_dbcmd["ctl_db_command"]
    L_ltinit["logtail_init"]
    L_ltpoll["logtail_poll"]
    L_ltclose["logtail_close"]
    L_ltlines["logtail_lines"]
    L_ltmiss["is_logtail_missing"]
    L_hist["history_db_read_*<br/>src/tetrisd/history.c"]
  end

  T_run --> T_wb
  T_run --> T_wf
  T_run --> T_draw
  T_run --> T_refall
  T_run --> T_move
  T_run --> T_now
  T_run --> T_dis
  T_run --> T_after
  T_run --> T_start
  T_run --> T_startall
  T_run --> T_stopd
  T_run --> T_stoplogd
  T_run --> T_stopall
  T_run --> T_db
  T_run --> T_kick
  T_run --> L_ltinit
  T_run --> L_ltpoll
  T_run --> L_ltclose

  T_wb --> T_wf
  T_draw --> T_dash
  T_draw --> T_right
  T_draw --> T_dis
  T_dash --> T_emit
  T_dash --> T_best
  T_dash --> L_up
  T_dash --> L_err
  T_right --> T_recent
  T_right --> L_ltlines
  T_right --> L_ltmiss
  T_recent --> T_emit
  T_move --> T_dis

  T_say --> T_after
  T_start --> L_start
  T_start --> T_say
  T_start --> T_after
  T_stopd --> L_req
  T_stopd --> L_err
  T_stopd --> T_say
  T_slnow --> L_stop
  T_slnow --> T_say
  T_slnow --> T_after
  T_stoplogd --> T_slnow
  T_stoplogd --> T_after
  T_db --> L_dbcmd
  T_db --> T_say
  T_db --> T_after
  T_startall --> T_start
  T_startall --> T_db
  T_startall --> L_probe
  T_startall --> L_pdb
  T_stopall --> T_slnow
  T_stopall --> T_db
  T_stopall --> L_req
  T_stopall --> L_probe
  T_stopall --> L_pdb
  T_stopall --> T_after
  T_kick --> L_req
  T_kick --> L_err
  T_kick --> T_say
  T_kick --> T_after

  T_refall --> T_refdb
  T_refall --> L_refresh
  T_refall --> L_probe
  T_refall --> L_pdb
  T_refdb --> L_hist

  L_start -.->|"CtlStepFn"| T_step
  L_stop -.->|"CtlStepFn"| T_step

  classDef tui fill:#e0e8f4,stroke:#54759f,color:#122031
  classDef leaf fill:#f2e9da,stroke:#9c8149,color:#332816
  classDef root fill:#cfe3de,stroke:#2f7a70,color:#0b2723
  class T_draw,T_right,T_dash,T_recent,T_emit,T_best,T_dis,T_wb,T_wf,T_start,T_startall,T_stopd,T_slnow,T_stoplogd,T_stopall,T_db,T_kick,T_say,T_after,T_step,T_refall,T_refdb,T_move,T_now tui
  class L_req,L_refresh,L_err,L_up,L_start,L_stop,L_probe,L_pdb,L_dbcmd,L_ltinit,L_ltpoll,L_ltclose,L_ltlines,L_ltmiss,L_hist leaf
  class T_run root
```

`after_page` (8 callers) and `say` (5) are the file's real shared plumbing;
every `do_*` action ends in one of them. `do_stop_all` is the only function that
reaches all four subsystems in one call.

---

## 4. Inside the leaf modules

The three modules nothing calls upward from. Each is a short tree, which is why
they read cleanly on their own — and the single edge that escapes is
`ctl_probe → ctl_request`.

```mermaid
flowchart LR
  subgraph CL["ctl_client.c"]
    direction TB
    K_refresh["ctl_refresh"]
    K_req["ctl_request"]
    K_conn["ctl_connect"]
    K_dstat["ctl_decode_status"]
    K_drooms["ctl_decode_rooms"]
    K_dplay["ctl_decode_players"]
    K_each["json_each"]
    K_ritem["room_item"]
    K_pitem["player_item"]
    K_int["json_int"]
    K_str["json_str"]
    K_bool["json_bool"]
    K_key["find_key"]
    K_err["ctl_strerror"]
    K_up["ctl_fmt_uptime"]
  end

  subgraph LF["ctl_lifecycle.c"]
    direction TB
    F_start["ctl_start"]
    F_stop["ctl_stop_logd"]
    F_probe["ctl_probe"]
    F_pdb["ctl_probe_db"]
    F_dbcmd["ctl_db_command"]
    F_spawn["spawn_dspawn2"]
    F_readpid["read_pidfile"]
    F_pidpath["pidfile_path"]
    F_alive["logd_socket_alive"]
    F_rootp["ctl_root_path"]
    F_root["ctl_root"]
    F_name["ctl_daemon_name"]
    F_parse["ctl_daemon_parse"]
    F_nap["nap_ms"]
  end

  subgraph LT["ctl_logtail.c"]
    direction TB
    G_init["logtail_init"]
    G_initat["logtail_init_at"]
    G_poll["logtail_poll"]
    G_try["try_open_log_file"]
    G_open["open_at"]
    G_push["ring_push"]
    G_lines["logtail_lines"]
    G_miss["is_logtail_missing"]
    G_close["logtail_close"]
  end

  K_refresh --> K_req
  K_refresh --> K_dstat
  K_refresh --> K_drooms
  K_refresh --> K_dplay
  K_req --> K_conn
  K_dstat --> K_int
  K_dstat --> K_key
  K_drooms --> K_each
  K_dplay --> K_each
  K_each -.->|"item fn"| K_ritem
  K_each -.->|"item fn"| K_pitem
  K_ritem --> K_int
  K_ritem --> K_str
  K_pitem --> K_int
  K_pitem --> K_str
  K_pitem --> K_bool
  K_int --> K_key
  K_str --> K_key
  K_bool --> K_key

  F_start --> F_probe
  F_start --> F_spawn
  F_start --> F_name
  F_start --> F_nap
  F_stop --> F_probe
  F_stop --> F_readpid
  F_stop --> F_pidpath
  F_stop --> F_rootp
  F_stop --> F_nap
  F_probe --> F_readpid
  F_probe --> F_alive
  F_probe --> F_name
  F_probe ==>|"the one cross-module edge"| K_req
  F_spawn --> F_name
  F_spawn --> F_root
  F_readpid --> F_pidpath
  F_pidpath --> F_name
  F_pidpath --> F_rootp
  F_alive --> F_rootp
  F_pdb --> F_rootp
  F_dbcmd --> F_root
  F_rootp --> F_root

  G_init --> G_initat
  G_initat --> G_poll
  G_poll --> G_try
  G_poll --> G_open
  G_poll --> G_push
  G_try --> G_open

  classDef fb fill:#f2e9da,stroke:#9c8149,color:#332816
  classDef fc fill:#e9e0f0,stroke:#84669f,color:#281b31
  classDef fg fill:#e2ecda,stroke:#688a4f,color:#1f2b16
  class K_refresh,K_req,K_conn,K_dstat,K_drooms,K_dplay,K_each,K_ritem,K_pitem,K_int,K_str,K_bool,K_key,K_err,K_up fb
  class F_start,F_stop,F_probe,F_pdb,F_dbcmd,F_spawn,F_readpid,F_pidpath,F_alive,F_rootp,F_root,F_name,F_parse,F_nap fc
  class G_init,G_initat,G_poll,G_try,G_open,G_push,G_lines,G_miss,G_close fg
```

`ctl_strerror`, `ctl_fmt_uptime`, `ctl_daemon_parse`, `logtail_lines`,
`is_logtail_missing` and `logtail_close` have no outgoing edge — pure leaves,
called only from the front ends above.

**What the edges say.**

- `ctl_lifecycle.c → ctl_client.c` is a single edge: `ctl_probe` asks
  `ctl_request` whether STATUS answers. That is the whole reason liveness is by
  observation and not by pidfile.
- `ctl_start` and `ctl_stop_logd` call back *up* into their caller's file
  through `CtlStepFn` — `cli_step` prints a line, `tui_step` advances a progress
  bar, and neither is known to the lifecycle module.
- `json_each` is the same trick one level down: `ctl_decode_rooms` and
  `ctl_decode_players` hand it `room_item` / `player_item`, so the array walk
  exists once.
- `ctl_tui.c` has no edge into `tetrisctl.c`. The console adds no capability the
  argv CLI lacks; the dependency runs one way only.

---

## 5. The load-bearing half — single-caller functions removed

Same binary, with every function that has exactly one caller deleted and its
edges spliced through to that caller. Those 35 could be pasted into their single
call site without changing a symbol anyone else can see — they exist for
readability, not for sharing. What survives is the 36 functions that are
actually shared, plus `main`, which has no caller at all.

**One exception is kept.** `cli_step`, `tui_step`, `room_item` and `player_item`
each have one static call site, but that site invokes them through a function
pointer — `CtlStepFn` and `json_each`'s item callback. Inlining them would
delete the indirection they exist to provide.

```mermaid
flowchart LR
  subgraph P["tetrisctl.c"]
    direction TB
    P_main["main<br/><i>0 callers</i>"]
    P_step["cli_step<br/><i>2 · fn ptr</i>"]
  end

  subgraph Q["ctl_tui.c"]
    direction TB
    Q_after["after_page<br/><i>8</i>"]
    Q_say["say<br/><i>5</i>"]
    Q_dis["disabled_reason<br/><i>3</i>"]
    Q_db["do_db<br/><i>3</i>"]
    Q_emit["emit<br/><i>2</i>"]
    Q_wf["windows_free<br/><i>2</i>"]
    Q_start["do_start<br/><i>2</i>"]
    Q_slnow["stop_logd_now<br/><i>2</i>"]
    Q_step["tui_step<br/><i>2 · fn ptr</i>"]
  end

  subgraph R["ctl_client.c"]
    direction TB
    R_req["ctl_request<br/><i>7</i>"]
    R_err["ctl_strerror<br/><i>5</i>"]
    R_key["find_key<br/><i>4</i>"]
    R_int["json_int<br/><i>3</i>"]
    R_str["json_str<br/><i>2</i>"]
    R_each["json_each<br/><i>2</i>"]
    R_ritem["room_item<br/><i>1 · fn ptr</i>"]
    R_pitem["player_item<br/><i>1 · fn ptr</i>"]
    R_dstat["ctl_decode_status<br/><i>2</i>"]
    R_drooms["ctl_decode_rooms<br/><i>2</i>"]
    R_dplay["ctl_decode_players<br/><i>2</i>"]
    R_up["ctl_fmt_uptime<br/><i>2</i>"]
  end

  subgraph S["ctl_lifecycle.c"]
    direction TB
    S_name["ctl_daemon_name<br/><i>5</i>"]
    S_probe["ctl_probe<br/><i>5</i>"]
    S_rootp["ctl_root_path<br/><i>4</i>"]
    S_root["ctl_root<br/><i>3</i>"]
    S_pdb["ctl_probe_db<br/><i>3</i>"]
    S_start["ctl_start<br/><i>2</i>"]
    S_stop["ctl_stop_logd<br/><i>2</i>"]
    S_readpid["read_pidfile<br/><i>2</i>"]
    S_pidpath["pidfile_path<br/><i>2</i>"]
    S_parse["ctl_daemon_parse<br/><i>2</i>"]
    S_nap["nap_ms<br/><i>2</i>"]
  end

  subgraph U["ctl_logtail.c"]
    direction TB
    U_poll["logtail_poll<br/><i>2</i>"]
    U_open["open_at<br/><i>2</i>"]
  end

  P_main --> R_req
  P_main --> R_dstat
  P_main --> R_drooms
  P_main --> R_dplay
  P_main --> R_err
  P_main --> R_up
  P_main --> S_parse
  P_main --> S_name
  P_main --> S_start
  P_main --> S_stop
  P_main --> Q_after
  P_main --> Q_say
  P_main --> Q_dis
  P_main --> Q_db
  P_main --> Q_emit
  P_main --> Q_wf
  P_main --> Q_start
  P_main --> Q_slnow
  P_main --> S_probe
  P_main --> S_pdb
  P_main --> U_poll

  Q_start --> S_start
  Q_start --> Q_say
  Q_start --> Q_after
  Q_slnow --> S_stop
  Q_slnow --> Q_say
  Q_slnow --> Q_after
  Q_db --> Q_say
  Q_db --> Q_after
  Q_db --> S_root
  Q_say --> Q_after

  R_dstat --> R_int
  R_dstat --> R_key
  R_drooms --> R_each
  R_dplay --> R_each
  R_each -.-> R_ritem
  R_each -.-> R_pitem
  R_ritem --> R_int
  R_ritem --> R_str
  R_pitem --> R_int
  R_pitem --> R_str
  R_pitem --> R_key
  R_int --> R_key
  R_str --> R_key

  S_start --> S_probe
  S_start --> S_name
  S_start --> S_root
  S_start --> S_nap
  S_start -.-> P_step
  S_start -.-> Q_step
  S_stop --> S_probe
  S_stop --> S_readpid
  S_stop --> S_pidpath
  S_stop --> S_rootp
  S_stop --> S_nap
  S_stop -.-> P_step
  S_stop -.-> Q_step
  S_probe --> S_readpid
  S_probe --> S_name
  S_probe --> S_rootp
  S_probe ==> R_req
  S_readpid --> S_pidpath
  S_pidpath --> S_name
  S_pidpath --> S_rootp
  S_pdb --> S_rootp
  S_rootp --> S_root
  U_poll --> U_open

  classDef fa fill:#d9edea,stroke:#3f8a80,color:#0c2b27
  classDef ftui fill:#e0e8f4,stroke:#54759f,color:#122031
  classDef fb fill:#f2e9da,stroke:#9c8149,color:#332816
  classDef fc fill:#e9e0f0,stroke:#84669f,color:#281b31
  classDef fg fill:#e2ecda,stroke:#688a4f,color:#1f2b16
  class P_main,P_step fa
  class Q_after,Q_say,Q_dis,Q_db,Q_emit,Q_wf,Q_start,Q_slnow,Q_step ftui
  class R_req,R_err,R_key,R_int,R_str,R_each,R_ritem,R_pitem,R_dstat,R_drooms,R_dplay,R_up fb
  class S_name,S_probe,S_rootp,S_root,S_pdb,S_start,S_stop,S_readpid,S_pidpath,S_parse,S_nap fc
  class U_poll,U_open fg
```

`main` absorbs the whole `tetrisctl.c` command layer and the entire console
event loop, because every one of those was a single-caller function — which is
why it ends up with 20 outgoing edges here and 8 in the real code.

**What the reduction says.**

- `ctl_logtail.c` collapses from 9 functions to 2. Seven of the nine have
  exactly one caller — the module is a private state machine with a thin public
  face, which is what a log tail should be.
- `ctl_tui.c` collapses from 25 to 9, and all the survivors are plumbing
  (`after_page`, `say`, `emit`) or actions reused by the two bulk operations
  (`do_start`, `stop_logd_now`, `do_db`). Every drawing function and every
  top-level handler is single-caller.
- `ctl_lifecycle.c` barely shrinks — 14 to 11. Almost everything in it is
  genuinely shared, which is the signature of a real module rather than one
  function cut into pieces.
- The five most-called functions are `after_page` (8 callers), `ctl_request`
  (7), and `ctl_daemon_name`, `ctl_probe` and `ctl_strerror` (5 each). Those are
  the signatures that cost the most to change.

| File | Functions | Single caller | Kept |
|---|---:|---:|---:|
| `tetrisctl.c` | 8 | 6 | 2 |
| `ctl_tui.c` | 25 | 16 | 9 |
| `ctl_client.c` | 15 | 3 | 12 |
| `ctl_lifecycle.c` | 14 | 3 | 11 |
| `ctl_logtail.c` | 9 | 7 | 2 |
| **total** | **71** | **35** | **36** |

---

## 6. `bin/tetrisd` — `control_plane.c`, and the seam

`control_plane.c` is compiled into the daemon, so its callers live in
`src/tetrisd/tetrisd.c` and its callees in `src/tetrisd/room.c`. The split of
work between the two threads is the design: the control thread touches every
byte that came from outside the process, the admin thread touches the room
tables and nothing else.

```mermaid
flowchart LR
  subgraph SX["src/tetrisctl (client half)"]
    X_req["ctl_request<br/>ctl_client.c"]
  end

  subgraph SCP["control_plane.c"]
    direction TB
    CP_open["ctl_open"]
    CP_thread["ctl_thread"]
    CP_handle["handle_conn"]
    CP_class["classify"]
    CP_kickp["parse_kick_path"]
    CP_reply["ctl_reply"]
    CP_to["set_timeout"]
    CP_disp["ctl_dispatch"]
    CP_bstat["build_status"]
    CP_brooms["build_rooms"]
    CP_bplay["build_players"]
    CP_up["uptime_seconds"]
    CP_phase["phase_name"]
    CP_esc["json_escape"]
  end

  subgraph SD["src/tetrisd/tetrisd.c"]
    direction TB
    D_main["main"]
    D_admin["admin_thread"]
    D_hctl["handle_ctl"]
  end

  subgraph SR["src/tetrisd/room.c"]
    direction TB
    R_cc["client_count"]
    R_rc["room_count"]
    R_rs["room_snapshot"]
    R_ps["player_snapshot"]
    R_fd["client_fd_by_player"]
  end

  X_req ==>|"AF_UNIX frame"| CP_handle
  CP_reply ==>|"reply frame"| X_req

  D_main --> CP_open
  D_main -->|"pthread_create"| CP_thread
  CP_thread --> CP_handle
  CP_handle --> CP_to
  CP_handle --> CP_class
  CP_handle --> CP_reply
  CP_class --> CP_kickp
  CP_handle ==>|"CtlReq + fd over g_ctl_notify pipe"| D_admin
  D_admin --> D_hctl
  D_hctl --> CP_disp
  CP_disp --> CP_bstat
  CP_disp --> CP_brooms
  CP_disp --> CP_bplay
  CP_disp --> CP_reply
  CP_disp --> CP_to
  CP_disp --> R_fd
  CP_bstat --> CP_up
  CP_bstat --> R_cc
  CP_bstat --> R_rc
  CP_brooms --> CP_phase
  CP_brooms --> R_rs
  CP_bplay --> CP_esc
  CP_bplay --> R_ps
  CP_disp -.->|"CtlAfter"| D_hctl

  classDef out fill:#d9edea,stroke:#3f8a80,color:#0c2b27
  classDef room fill:#f0ede0,stroke:#9a8d5f,color:#332e18
  class X_req out
  class R_cc,R_rc,R_rs,R_ps,R_fd room
```

One socket hop from the CLI, one pipe hop from the control thread to the admin
thread. `ctl_dispatch` returns a `CtlAfter` rather than acting, because shutdown
and fd-drop belong to `tetrisd.c`, not here.

---

## 7. Calls that leave the folder

Every symbol `src/tetrisctl` reaches for outside its own six files, and which
function reaches for it. This is the surface that has to hold still for the
folder to keep compiling.

```mermaid
flowchart LR
  subgraph SRC["src/tetrisctl"]
    direction TB
    E_req["ctl_request"]
    E_err["ctl_strerror"]
    E_tui["ctl_tui.c<br/>draw / say / do_*"]
    E_refdb["refresh_db"]
    E_probedb["ctl_probe_db"]
    E_ltinit["logtail_init"]
    E_spawn["spawn_dspawn2"]
    E_dbcmd["ctl_db_command"]
    E_reply["ctl_reply / handle_conn"]
    E_build["build_status / rooms / players"]
    E_many["most functions"]
  end

  H_htttp["libhtttp/htttp.h<br/>serialize_request, parse_response,<br/>parse_request, serialize_response,<br/>header_set, reason"]
  H_ui["libtetrisui/tetrisui.h<br/>init, shutdown, form, confirm,<br/>message, progress_*, status bar"]
  H_log["libtetrisutil/logmsg.h<br/>log_send"]
  H_rc["libtetrisutil/rc.h<br/>rc_get"]
  H_hist["tetrisd/history.h<br/>read_recent, read_best_scores"]
  H_room["tetrisd/room.h<br/>counts, snapshots, fd lookup"]
  P_spawn["bin/dspawn2<br/>fork + execvp"]
  P_db["bin/tetrisdb<br/>account database"]

  E_req --> H_htttp
  E_err --> H_htttp
  E_reply --> H_htttp
  E_tui --> H_ui
  E_refdb --> H_hist
  E_build --> H_room
  E_many --> H_log
  E_probedb --> H_rc
  E_ltinit --> H_rc
  E_spawn ==>|"spawns"| P_spawn
  E_dbcmd ==>|"spawns"| P_db
  E_probedb ==>|"connects"| P_db

  classDef lib fill:#f0ede0,stroke:#9a8d5f,color:#332e18
  classDef proc fill:#f4e6d5,stroke:#ac7c40,color:#3a2612
  class H_htttp,H_ui,H_log,H_rc,H_hist,H_room lib
  class P_spawn,P_db proc
```

`tetrisd/room.h` is reached only from `control_plane.c` and `tetrisd/history.h`
only from `ctl_tui.c` — the two edges that make this folder not self-contained,
in opposite directions.

---

## 8. Function index

Line numbers as of the working tree at the time of writing. **In** is the number
of distinct functions that call it; **1** means it was dropped from section 5.

### `tetrisctl.c` — 465 lines

| Function | Line | In | Calls |
|---|---:|---:|---|
| `main` | 213 | 0 | usage, cmd_start, cmd_stop, format_*, ctl_request, ctl_decode_*, ctl_strerror, ctl_tui_run |
| `cli_step` | 116 | 2 | fn-ptr target of ctl_start / ctl_stop_logd |
| `usage` | 45 | 1 | — |
| `format_status` | 63 | 1 | ctl_fmt_uptime |
| `format_rooms` | 70 | 1 | — |
| `format_players` | 90 | 1 | — |
| `cmd_start` | 123 | 1 | ctl_daemon_parse, ctl_daemon_name, ctl_start |
| `cmd_stop` | 163 | 1 | ctl_daemon_parse, ctl_request, ctl_stop_logd, ctl_strerror |

### `ctl_client.c` — 385 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `ctl_request` | 144 | 7 | ctl_connect | htttp_serialize_request, htttp_parse_response, log_send |
| `ctl_strerror` | 224 | 5 | — | htttp_reason |
| `find_key` | 26 | 4 | — | — |
| `json_int` | 34 | 3 | find_key | — |
| `json_str` | 41 | 2 | find_key | — |
| `json_each` | 80 | 2 | fn ptr: room_item / player_item | — |
| `ctl_decode_status` | 255 | 2 | find_key, json_int | — |
| `ctl_decode_rooms` | 284 | 2 | json_each | — |
| `ctl_decode_players` | 316 | 2 | json_each | — |
| `ctl_fmt_uptime` | 243 | 2 | — | — |
| `room_item` | 271 | 1\* | json_int, json_str | — |
| `player_item` | 300 | 1\* | json_int, json_str, json_bool | — |
| `ctl_connect` | 120 | 1 | — | socket, connect |
| `json_bool` | 64 | 1 | find_key | — |
| `ctl_refresh` | 328 | 1 | ctl_request, ctl_decode_* | log_send |

\* one static call site, but reached through a function pointer.

### `ctl_lifecycle.c` — 487 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `ctl_daemon_name` | 44 | 5 | — | — |
| `ctl_probe` | 133 | 5 | ctl_daemon_name, read_pidfile, logd_socket_alive, **ctl_request** | log_send |
| `ctl_root_path` | 70 | 4 | ctl_root | — |
| `ctl_root` | 62 | 3 | — | getenv, getcwd |
| `ctl_probe_db` | 183 | 3 | ctl_root_path | rc_get, connect → bin/tetrisdb |
| `ctl_start` | 332 | 2 | ctl_probe, spawn_dspawn2, ctl_daemon_name, nap_ms, *step cb* | log_send |
| `ctl_stop_logd` | 411 | 2 | ctl_probe, read_pidfile, pidfile_path, ctl_root_path, nap_ms, *step cb* | kill, log_send |
| `read_pidfile` | 88 | 2 | pidfile_path | — |
| `pidfile_path` | 81 | 2 | ctl_daemon_name, ctl_root_path | — |
| `ctl_daemon_parse` | 49 | 2 | — | — |
| `nap_ms` | 266 | 2 | — | nanosleep |
| `logd_socket_alive` | 113 | 1 | ctl_root_path | socket, connect |
| `ctl_db_command` | 204 | 1 | ctl_root | fork/exec → bin/tetrisdb |
| `spawn_dspawn2` | 281 | 1 | ctl_daemon_name, ctl_root | fork/exec → bin/dspawn2 |

### `ctl_logtail.c` — 224 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `logtail_poll` | 106 | 2 | try_open_log_file, open_at, ring_push | read, stat |
| `open_at` | 52 | 2 | — | open, lseek |
| `ring_push` | 44 | 1 | — | — |
| `try_open_log_file` | 74 | 1 | open_at | — |
| `logtail_init_at` | 87 | 1 | logtail_poll | — |
| `logtail_init` | 99 | 1 | logtail_init_at | rc_get |
| `is_logtail_missing` | 196 | 1 | — | — |
| `logtail_lines` | 201 | 1 | — | — |
| `logtail_close` | 217 | 1 | — | — |

### `ctl_tui.c` — 869 lines

| Function | Line | In | Calls | External |
|---|---:|---:|---|---|
| `after_page` | 455 | 8 | — | ncurses |
| `say` | 467 | 5 | after_page | tetrisui_message |
| `disabled_reason` | 103 | 3 | — | — |
| `do_db` | 541 | 3 | ctl_db_command, say, after_page | — |
| `emit` | 142 | 2 | — | — |
| `windows_free` | 259 | 2 | — | delwin |
| `do_start` | 483 | 2 | ctl_start, say, after_page | tetrisui_progress_begin / _end |
| `stop_logd_now` | 516 | 2 | ctl_stop_logd, say, after_page | tetrisui_progress_begin / _end |
| `tui_step` | 477 | 2 | fn-ptr target of ctl_start / ctl_stop_logd | tetrisui_progress_step |
| `ctl_tui_run` | 732 | 1 | windows_build/_free, draw, refresh_all, move_sel, now_ms, after_page, disabled_reason, do_*, logtail_* | tetrisui_init, tetrisui_shutdown, log_send |
| `draw` | 368 | 1 | build_dashboard, draw_right, disabled_reason | tetrisui_set_status, tetrisui_draw_status_bar |
| `draw_right` | 328 | 1 | build_recent, logtail_lines, is_logtail_missing | ncurses |
| `build_dashboard` | 170 | 1 | emit, get_individual_best_score, ctl_fmt_uptime, ctl_strerror | — |
| `build_recent` | 303 | 1 | emit | — |
| `get_individual_best_score` | 156 | 1 | — | — |
| `windows_build` | 273 | 1 | windows_free | newwin |
| `do_stop_tetrisd` | 500 | 1 | ctl_request, ctl_strerror, say, after_page | tetrisui_confirm |
| `do_stop_logd` | 529 | 1 | stop_logd_now, after_page | — |
| `do_start_all` | 551 | 1 | do_start, do_db, ctl_probe, ctl_probe_db | — |
| `do_stop_all` | 564 | 1 | stop_logd_now, do_db, ctl_request, ctl_probe, ctl_probe_db, after_page | — |
| `do_kick` | 582 | 1 | ctl_request, ctl_strerror, say, after_page | tetrisui_form |
| `refresh_all` | 670 | 1 | refresh_db, ctl_refresh, ctl_probe, ctl_probe_db | log_send |
| `refresh_db` | 636 | 1 | — | history_db_read_recent, history_db_read_best_scores |
| `move_sel` | 713 | 1 | disabled_reason | — |
| `now_ms` | 629 | 1 | — | clock_gettime |

### `control_plane.c` — 752 lines, links into `bin/tetrisd`

| Function | Line | Calls | External |
|---|---:|---|---|
| `ctl_open` | 217 | — | bind, listen, log_send · **called by tetrisd.c main** |
| `ctl_thread` | 446 | handle_conn | poll, accept · **pthread entry from tetrisd.c main** |
| `handle_conn` | 384 | set_timeout, classify, ctl_reply | htttp_parse_request, write → g_ctl_notify |
| `classify` | 354 | parse_kick_path | — |
| `parse_kick_path` | 345 | — | — |
| `ctl_dispatch` | 605 | build_status, build_rooms, build_players, ctl_reply, set_timeout | client_fd_by_player, log_send · **called by tetrisd.c handle_ctl** |
| `build_status` | 555 | uptime_seconds | client_count, room_count |
| `build_rooms` | 563 | phase_name | room_snapshot |
| `build_players` | 581 | json_escape | player_snapshot |
| `ctl_reply` | 151 | — | htttp_header_set, htttp_serialize_response, log_send |
| `json_escape` | 81 | — | — |
| `phase_name` | 54 | — | — |
| `uptime_seconds` | 41 | — | time |
| `set_timeout` | 48 | — | setsockopt |
