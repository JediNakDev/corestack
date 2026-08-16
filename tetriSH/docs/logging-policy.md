# Function-level logging policy

Routine function entry is not logged.
A major operation emits one completion record at `LOG_INFO`, including its outcome and useful safe context.
A minor operation usually emits no success record and may use `LOG_DEBUG` when the record is diagnostically valuable.
Helpers emit no routine entry or exit records.
Game actions emit one record at the HTTTP boundary rather than records from internal game mechanics.
A failed operation replaces its success record at the boundary that owns the outcome.

| Type | Recommended logging |
|---|---|
| Major | Log completion once at `INFO`, including outcome and useful context |
| Minor | Usually no success log; use `DEBUG` only when diagnostically valuable |
| Helper | No routine entry/exit logging |
| Game action | One HTTTP boundary record |
| 4xx outcome | One `WARN` at the owning boundary |
| 5xx outcome | One `ERROR` at the owning boundary |
| Lifecycle event | One `INFO` for meaningful state change |

The boundary that owns the final outcome logs its warning or error.
Lower layers preserve and return structured failure information without repeating the same failure record.
A lower layer logs separately only for an independent invariant violation, a locally handled failure, or diagnostic context that cannot be propagated.

Milestones inside a major function use `LOG_DEBUG` when they explain implementation progress, retries, or hand-offs.
A milestone uses `LOG_INFO` only when it is itself an operator-visible lifecycle or domain transition that should remain in the normal operational timeline.

Completion records preserve scalar return values as their raw status.
Pointer-returning functions use status `0` for a non-null result and `-1` for `NULL` without logging the pointer value.
Value-object returns use status `0` and do not serialize object contents.
A lower-layer `-1` remains at that function's classified INFO or DEBUG level; the owning boundary emits the single WARN or ERROR for the final outcome.

Logging is best effort and nonblocking.
Code ignores `log_send()` failures and must not change its primary result because logging is unavailable or saturated.

Safe context includes operation, method, status, room ID, player ID, session PID, phase, score, lines, elapsed time, and stable reason codes.
Logs must exclude passwords, credential bodies, JWTs, signing secrets, private keys, symmetric keys, decrypted frames, digests, and salts.
If no existing textual representation exists, log the truthful raw enum or integer rather than adding a translation API.

Normal deployments should configure `tetrislogd` above `LOG_DEBUG`.
Debug logging remains available for diagnostic runs without filling normal logs with 20 Hz gameplay mechanics.
