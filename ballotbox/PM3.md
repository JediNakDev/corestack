# BallotBox: Project Meeting 3

## Requirement Changes

No changes have been made to the requirements.
The use cases, sequence diagrams, and class diagrams were revised for greater robustness in response to feedback from the previous meeting.

## Use Case Implementation Status

Every use case is partially implemented: the logic is written and unit tested, but none runs end to end because all six wait on the same three pieces (crypto, database, integration).
The unit tests do not wait on those pieces - each one substitutes the store, crypto and transport seams and tests a single function against them - so the logic is verified now and the remaining work is the seams themselves.

| Use Case                    | Status  | Done                                             | Waiting on            |
| --------------------------- | ------- | ------------------------------------------------ | --------------------- |
| UC-1: Instantiate BallotBox | Partial | Config validation and the election state machine | Database, integration |
| UC-2: Join BallotBox        | Partial | Eligibility check and join-outcome handling      | Crypto, database      |
| UC-3: Cast a Vote           | Partial | Vote routing and the ballot recording path       | Crypto, database      |
| UC-4: Update a Vote         | Partial | Update routing, sharing UC-3's recording path    | Database              |
| UC-5: View Result           | Partial | Publish guarded by the state machine             | Database              |
| UC-6: Check Your Vote       | Partial | Receipt derivation, verified deterministic       | Database, crypto      |

### Progress Since Meeting 2

At Meeting 2 the clients ran on mock data with no real logic behind them; since then the actual application logic has been written as three libraries, with unit tests covering them.

| Item                             | Detail                                                                 |
| -------------------------------- | ---------------------------------------------------------------------- |
| `libballotbrain` (daemon logic)  | Config validation, election lifecycle, eligibility, recording, publish |
| `libballotclient` (client logic) | Voter and admin logic: vote routing, join handling, requests           |
| `libtetrissh` (connectivity)     | Secure session layer                                                   |
| Unit tests                       | Test suite running against all logic in the libraries above            |

### Demo

The demonstration video is unchanged from Meeting 2, as this cycle's work is library logic that is not yet integrated into the running clients and therefore has no visible effect on the interface.
That logic is instead demonstrated through the unit test suite, which exercises it directly.

## Feature Progress Records

| Feature                                   | Owner          | Status                                                |
| ----------------------------------------- | -------------- | ----------------------------------------------------- |
| Client (tetrisui, ballotu, and ballotctl) | Jedi           | Completed (operating on mock data)                    |
| Shell (tetrish)                           | Jedi           | Completed                                             |
| Connectivity (libtetrissh)                | Pop            | Completed                                             |
| System (ballotd)                          | Kenji          | Integration in progress (core logic largely complete) |
| Unit test                                 | Feature owners | Unit tests for implemented features are complete      |
| Integration test                          | Kenji          | Not started                                           |
| E2E test                                  | Kenji          | Not started                                           |
