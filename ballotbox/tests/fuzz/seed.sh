#!/bin/sh
#
# seed.sh - write the seed corpus for every fuzz target.
#
# A coverage-guided fuzzer starting from an empty corpus spends its first
# minutes rediscovering "\r\n" and "key=value". Seeding is not about handing it
# the answers: it is about starting inside the grammar so the mutations that
# follow probe the parser's decisions instead of its front door. Each seed
# below is a well-formed message or a documented boundary case - the ones the
# unit tests assert on, plus the edges the headers call out.
#
# Idempotent: run it as often as you like, it overwrites its own files and
# leaves fuzzer-discovered corpus entries alone.
#
# Usage: tests/fuzz/seed.sh [corpus-root]        (default tests/fuzz/corpus)

set -eu

root="${1:-tests/fuzz/corpus}"

# printf, not echo: we need real CRLFs and \x escapes, portably.
seed() {
    dir="$root/$1"
    name="$2"
    mkdir -p "$dir"
    # shellcheck disable=SC2059  # the format string is the point
    printf "$3" > "$dir/$name"
}

CRLF='\r\n'

# --- htttp_request ---------------------------------------------------------

seed htttp_request minimal      "JOIN /room/1 HTTTP/1.0${CRLF}${CRLF}"
seed htttp_request one_header   "MOVE /game/move HTTTP/1.0${CRLF}Player-Id: 7${CRLF}${CRLF}"
seed htttp_request with_body    "CAST /election/e1/ballot HTTTP/1.0${CRLF}Cert-Name: alice${CRLF}Content-Length: 26${CRLF}${CRLF}nonce=abc\npayload=deadbeef\n"
seed htttp_request many_headers "CREATE /election HTTTP/1.0${CRLF}A: 1${CRLF}B: 2${CRLF}C: 3${CRLF}D: 4${CRLF}E: 5${CRLF}${CRLF}"
seed htttp_request empty_body   "OPEN /election/e1 HTTTP/1.0${CRLF}Content-Length: 0${CRLF}${CRLF}"
# Boundaries the parser is written around: a bare LF is not a line break (D1),
# a Content-Length that disagrees with the body is HTTTP_ERR_LENGTH, and a
# path at exactly HTTTP_MAX_PATH-1 is the last accepted one.
seed htttp_request bare_lf      "JOIN /room/1 HTTTP/1.0\n\n"
seed htttp_request length_lie   "CAST /e HTTTP/1.0${CRLF}Content-Length: 100${CRLF}${CRLF}short"
seed htttp_request no_blank     "JOIN /room/1 HTTTP/1.0${CRLF}Player-Id: 7${CRLF}"

# --- htttp_response --------------------------------------------------------

seed htttp_response ok          "HTTTP/1.0 200 OK${CRLF}${CRLF}"
seed htttp_response with_body   "HTTTP/1.0 200 OK${CRLF}Content-Length: 13${CRLF}${CRLF}status=BB_OK\n"
seed htttp_response err_400     "HTTTP/1.0 400 Bad Request${CRLF}${CRLF}"
seed htttp_response err_409     "HTTTP/1.0 409 Conflict${CRLF}Content-Length: 28${CRLF}${CRLF}status=BB_ERR_NOT_OPEN\n"
seed htttp_response no_reason   "HTTTP/1.0 200${CRLF}${CRLF}"

# --- codec_request ---------------------------------------------------------
# One per operation: the decoder switches on the method, so a corpus missing
# an op leaves that whole branch uncovered no matter how long it runs.

seed codec_request join    "JOIN /election/e1 HTTTP/1.0${CRLF}Cert-Name: alice${CRLF}${CRLF}"
seed codec_request cast    "CAST /election/e1/ballot HTTTP/1.0${CRLF}Cert-Name: alice${CRLF}Content-Length: 26${CRLF}${CRLF}nonce=n1\npayload=deadbeef\n"
seed codec_request update  "UPDATE /election/e1/ballot HTTTP/1.0${CRLF}Cert-Name: alice${CRLF}Content-Length: 26${CRLF}${CRLF}nonce=n2\npayload=cafebabe\n"
seed codec_request results "RESULTS /election/e1/results HTTTP/1.0${CRLF}${CRLF}"
seed codec_request check   "CHECK /election/e1/check HTTTP/1.0${CRLF}Cert-Name: alice${CRLF}Content-Length: 10${CRLF}${CRLF}hash=abcd\n"
seed codec_request create  "CREATE /election HTTTP/1.0${CRLF}Cert-Name: admin${CRLF}Content-Length: 118${CRLF}${CRLF}election_id=e1\ntitle=Pick one\nopen_time=2026-01-01T00:00:00Z\nclose_time=2026-01-02T00:00:00Z\noption=A\noption=B\neligible=alice\neligible=bob\n"
seed codec_request open    "OPEN /election/e1 HTTTP/1.0${CRLF}Cert-Name: admin${CRLF}${CRLF}"
seed codec_request close   "CLOSE /election/e1 HTTTP/1.0${CRLF}Cert-Name: admin${CRLF}${CRLF}"
seed codec_request publish "PUBLISH /election/e1 HTTTP/1.0${CRLF}Cert-Name: admin${CRLF}${CRLF}"
seed codec_request next_id "ADMIN_NEXT_ID /election/next-id HTTTP/1.0${CRLF}${CRLF}"
# Ambiguity bait: an id containing the path separator, and a body whose
# repeated keys overrun the array bounds (BB_MAX_OPTIONS 16, BB_MAX_VOTERS 64).
seed codec_request slash_id "JOIN /election/a/b HTTTP/1.0${CRLF}${CRLF}"
seed codec_request odd_hex  "CAST /election/e1/ballot HTTTP/1.0${CRLF}Content-Length: 20${CRLF}${CRLF}nonce=n\npayload=abc\n"

# --- codec_response --------------------------------------------------------
# First byte selects the op for the re-encode leg, the rest is the frame.

seed codec_response join    "\x00HTTTP/1.0 200 OK${CRLF}Content-Length: 60${CRLF}${CRLF}status=BB_OK\nelection_id=e1\ntitle=Pick one\nstate=BB_STATE_OPEN\n"
seed codec_response cast    "\x01HTTTP/1.0 200 OK${CRLF}Content-Length: 42${CRLF}${CRLF}status=BB_OK\nhash=abcd\nissued_at=2026-01-01\n"
seed codec_response results "\x03HTTTP/1.0 200 OK${CRLF}Content-Length: 78${CRLF}${CRLF}status=BB_OK\ntally_count=2\ntally=3,4\nhash_count=1\nrow=abcd,0,1,0\n"
seed codec_response check   "\x04HTTTP/1.0 200 OK${CRLF}Content-Length: 48${CRLF}${CRLF}status=BB_OK\nfound=1\nfound_option=0\nfound_option_name=A\n"
seed codec_response error   "\x00HTTTP/1.0 403 Forbidden${CRLF}Content-Length: 27${CRLF}${CRLF}status=BB_ERR_NOT_ELIGIBLE\n"
# Counts that lie about the rows that follow - the array-bound case.
seed codec_response overcount "\x03HTTTP/1.0 200 OK${CRLF}Content-Length: 46${CRLF}${CRLF}status=BB_OK\ntally_count=999\nhash_count=999\n"

# --- rows ------------------------------------------------------------------
# The reply shape rows.c documents: narration, header, rule, rows, count.

seed rows one_row   "Started a new transaction tid = 3\nAdded scan of table user\nid\tsalt\tdigest\titers\t\n----------------------------\n7\tb3\t1f\t600000\t\n\n 1 rows.\n"
seed rows zero_rows "id\tsalt\tdigest\titers\t\n----------------------------\n\n 0 rows.\n"
seed rows two_rows  "id\tname\t\n------------\n1\talice\t\n2\tbob\t\n\n 2 rows.\n"
seed rows error     "query parse error: near 'SELCT'\n"
seed rows truncated "id\tsalt\t\n------------\n7\tb3\t"
seed rows no_rule   "id\tsalt\t\n7\tb3\t\n\n 1 rows.\n"

# --- rc_line ---------------------------------------------------------------

seed rc_line comment    "# a comment\n"
seed rc_line blank      "\n"
seed rc_line path       "PATH=/usr/bin:/bin\n"
seed rc_line path_space "PATH = /usr/bin\n"
seed rc_line pathetic   "PATHETIC arg\n"
seed rc_line command    "   ls -la\n"
seed rc_line directive  "log_level = debug\n"

# --- rc_bind ---------------------------------------------------------------
# Whole files, since rc_bind reads a path. First byte parity picks the
# owned_prefix mode, so seeds come in both flavours naturally.

seed rc_bind all_keys "fuzz_level = 3\nfuzz_attempts = 5\nfuzz_queue = 1024\nfuzz_enabled = on\nfuzz_path = /var/log/x\nfuzz_host = localhost\nfuzz_facility = daemon\nfuzz_checked = 50\n"
seed rc_bind mixed    "# comment\n\nPATH=/bin\nfuzz_level=7\nsome_other_key=whatever\nls -la\n"
seed rc_bind bad_int  "fuzz_level = 900\n"
seed rc_bind bad_bool "fuzz_enabled = maybe\n"
seed rc_bind long_str "fuzz_host = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
seed rc_bind unknown  "fuzz_nonesuch = 1\n"
seed rc_bind empty    ""

# --- playername ------------------------------------------------------------

seed playername simple   "alice"
seed playername mixed    "JediNakDev"
seed playername symbols  "a_b-c9"
seed playername max      "aaaaaaaaaaaaaaa"
seed playername illegal  "o'brien"
seed playername tabbed   "a\tb"
seed playername newline  "a\nb"
seed playername empty    ""

# --- ctl_frame -------------------------------------------------------------
# 4-byte big-endian length prefix, then the payload.

seed ctl_frame small      "\x00\x00\x00\x05hello"
seed ctl_frame zero_len   "\x00\x00\x00\x00"
seed ctl_frame huge_len   "\xff\xff\xff\xffhello"
seed ctl_frame over_cap   "\x00\x00\x20\x01payload"
seed ctl_frame torn       "\x00\x00\x00\x10short"
seed ctl_frame prefix_only "\x00\x00\x00"
seed ctl_frame htttp_frame "\x00\x00\x00\x1cOPEN /election/e1 HTTTP/1.0${CRLF}${CRLF}"

echo "seeded $(find "$root" -type f ! -name .gitkeep | wc -l | tr -d ' ') corpus files under $root"
echo "note: tests/fuzz/corpus/jwt_verify is written by bin/seedgen_jwt (make fuzz-seed)"
