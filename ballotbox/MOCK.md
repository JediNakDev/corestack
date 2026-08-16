# Mock Data Reference

`ballotu` and `ballotctl` are UI demos with in-memory mock state.
Nothing is shared between the two apps or persisted; every run starts from the seeds below.

## Accounts (cert names)

### ballotu (voter login)

| Cert name      | Result                                                          |
| -------------- | --------------------------------------------------------------- |
| `alice`, `bob` | Valid and eligible                                              |
| `mallory`      | Valid cert, but refused at join: not on the eligible-voter list |
| `expired`      | Rejected at login: cert expired                                 |
| anything else  | Rejected at login: unrecognized cert                            |

### ballotctl (admin login)

| Cert name     | Result                |
| ------------- | --------------------- |
| `admin`       | Valid admin cert      |
| anything else | Rejected and "logged" |

## Seeded elections (both apps)

| ID      | Title               | State     | Options                                   |
| ------- | ------------------- | --------- | ----------------------------------------- |
| `E-100` | Board Motion 2026   | Open      | Approve / Reject / Abstain (ballotu only) |
| `E-042` | Budget Ratification | Published | Yes 14 / No 6                             |

Join in ballotu is by typed ID: `E-100` joins, `E-042` is refused (Published, not Open), anything else is "not found".
Use `E-100` for join/cast/update (UC-2/3/4); use `E-042` for results/check (UC-5/6).
Elections created in ballotctl get IDs `E-101`, `E-102`, ...

## Check your vote (UC-6, against E-042)

You type your secret ballot key; the client derives your receipt hash from it and looks it up in the published tally.

| Secret key          | Result                                                                                              |
| ------------------- | --------------------------------------------------------------------------------------------------- |
| `alice23489ut49499` | Derives `fa15b8bb...28a63299`: "Your vote is 'No' and is included in the tally."                    |
| anything else       | Derives a hash that is not in the tally: Verification FAILED, dropped ballot                        |

Hashes issued during your own session (cast/update in `E-100`) only verify after that election is Published, which the voter app cannot do - use the key above for the UC-6 demo.
The results view (UC-5) shows the tally, then the counted hashes grouped in one column per option, so a voter can locate their own hash under their choice.

## Other demo triggers

- ballotctl create form: empty title/options, or close time <= open time, shows the "invalid config, stays in Draft" error (alt flow 4a).
- Lifecycle guards: Open only from Draft, Close only from Open, Publish only from Closed; wrong state shows an error naming the current state.
- Casting twice in ballotu routes to Update (and updating with no ballot routes to Cast).
