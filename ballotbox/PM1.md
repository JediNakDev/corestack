# BallotBox: Project Meeting 1

## Clarification of Requirements

How can an individual observe the ballot outcome via the client interface?

- Through UC-6: the published result view lists every ballot's hash, so a voter runs check with their receipt hash to confirm their vote was counted.

Can an individual update their vote within the time limit?

- Yes, see UC-4: voters may re-cast freely while the election is _Open_, and the latest version wins; once closed, updates are refused.

How is persistent storage handled?

- Ballots and the authoritative tally are saved server-side on the admin's ballotd server (durable, append-only store), not on the client.

## Development Process, Constraints & Risks

Process. We prototype the riskiest part first (secure session \+ secret/verifiable result view) to validate the foundation, then deliver in vertical slices week by week, coordinated in short iterations with CI running the security and concurrency test cases.

Constraints: custom C backend (manual memory safety); fixed CoreStack libraries; mandatory confidentiality \+ integrity.

| \#  | Risk                                                                                                | Mitigation                                                                                                                                                                       |
| --- | --------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R1  | Race condition — concurrent ballots cause lost or double-counted votes.                             | Funnel all writes through a single message queue so ballotd processes them serially; atomic append \+ durable commit before returning a receipt; stress/concurrency test cases.  |
| R2  | Verifiability vs. secrecy — a voter must verify their own vote while others cannot link it to them. | Issue each voter a receipt hash (commitment); publish only hashes in the result view, never identities or choices, so only the holder of a receipt can confirm their own ballot. |

## Timeline & Workload

Aligned to the 50.003 course schedule (17 May – 23 Aug 2026), by week (Wk 7 is Recess and is skipped; Wk 13 is the Final Presentation).

Team & roles:

- Jedi: command & application logic.
- Kenji: shell daemon.
- Pop: networking — libhtttp / libtetrissh.

| Week  | Date | Focus / Deliverables                                                                                                                         | Lead                                               |
| ----- | ---- | -------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| Wk 5  | 14/6 | Finalise Meeting 1 deliverables; set up repo \+ CI; spike CoreStack integration. Record & submit Meeting 1 video by 21/6.                    | All                                                |
| Wk 6  | 21/6 | Project Meeting 1\. Stand up ballotd skeleton \+ secure session prototype (libtetrissh handshake).                                           | Pop (session), Kenji (daemon)                      |
| Wk 8  | 5/7  | Implement instantiate \+ join \+ cast path (UC-1/2/3): encrypted ballot to ballotd, persistence, receipt hash. Prep Meeting 2 video by 12/7. | Jedi (logic), Kenji (daemon), Pop (transport)      |
| Wk 9  | 12/7 | Project Meeting 2\. Unit \+ integration tests for the cast path; confidentiality \+ auth checks.                                             | All                                                |
| Wk 10 | 19/7 | Lifecycle (open/close) \+ update-a-vote (UC-4); concurrency via message queue \+ atomic writes (R1). Prep Meeting 3 video by 26/7.           | Jedi (lifecycle/versioning), Kenji (queue/daemon)  |
| Wk 11 | 26/7 | Project Meeting 3\. Anti-replay / double-vote refusal; fuzz the request parser.                                                              | Pop (anti-replay), Jedi (parser/logic)             |
| Wk 12 | 2/8  | Result view \+ check-your-vote (UC-5/6); secrecy↔verifiability separation (R2); memory-safety pass. Project consultation.                    | Jedi (verification), Kenji (hardening), Pop (logs) |
| Wk 13 | 9/8  | Final Presentation. End-to-end demo (instantiate → join → cast → update → close → view → check), report, peer review.                        | All                                                |
