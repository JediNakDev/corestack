## Design

BallotBox is implemented in C, which has no classes, so every class below maps to a struct, an opaque context handle, or a module (a header plus its translation units).

### Domain Class Diagram

```mermaid
classDiagram
    class Actor {
        -certName
    }
    class Admin
    class Voter
    class Observer

    class Certificate {
        -name
        -status
    }

    class Election {
        -id
        -title
        -state
        -options
        -eligibleVoters
        -openTime
        -closeTime
        -tally
    }

    class Ballot {
        -certName
        -nonce
        -encryptedPayload
        -payloadLen
    }

    class BallotHash {
        -hash
        -optionIndex
        -version
        -superseded
    }

    class Receipt {
        -hash
        -issuedAt
    }

    class VoterSession {
        -certName
        -joined
        -hasBallot
        -ballotVersion
        -myHash
        -title
        -options
        -optionCount
    }

    class PublishedResults {
        -title
        -tally
        -options
        -ballotHashes
    }

    class BallotOwner {
        -electionId
        -certName
        -currentHash
        -version
    }

    class ElectionState {
        <<enumeration>>
        DRAFT
        OPEN
        CLOSED
        PUBLISHED
    }

    class CertStatus {
        <<enumeration>>
        INVALID
        EXPIRED
        NOT_ELIGIBLE
        VALID
    }

    Admin --|> Actor
    Voter --|> Actor
    Observer --|> Actor

    Actor "1" -- "1" Certificate : identified by
    Admin "1" -- "*" Election : manages
    Voter "1" -- "1" VoterSession : has
    VoterSession "*" -- "1" Election : joins
    Observer "*" -- "*" Election : observes
    Election "1" *-- "*" Ballot : accepts
    Election "1" *-- "*" BallotHash : records
    Election "1" -- "0..1" PublishedResults : publishes
    Election "1" -- "*" BallotOwner : tracks privately
    Ballot ..> BallotHash : produces
    BallotHash ..> Receipt : returned as
    Election "1" -- "1" ElectionState : state
    Certificate "1" -- "1" CertStatus : status
```

`BallotHash` is the only per-ballot entity published in results, and it holds no voter identity.
The store keeps the voter-to-current-hash association in the separate, private `BallotOwner` mapping used for updates.

### Solution Class Diagram: voter client (ballotu)

```mermaid
classDiagram
    class VoterUI {
        -formData: VoterFormData
        -session: VoterSession
        +show(msg: String)
    }

    class VoterFormData {
        -host: String
        -port: int
        -electionId: String
        -optionIndex: int
        -secretKey: String
    }

    class VoterSession {
        -certName: String
        -joined: boolean
        -electionId: String
        -hasBallot: boolean
        -ballotVersion: int
        -myHash: String
        -title: String
        -options: List~String~
        -optionCount: int
    }

    class VoterController {
        +join(session: VoterSession, electionId: String, username: String) JoinOutcome
        +routeVote(s: VoterSession) VoteAction
        +submitVote(s: VoterSession, optionIndex: int, nonce: String) Receipt
        +deriveReceipt(secretKey: String) String
    }

    class ResultsController {
        +buildResultsRequest(id: String, username: String) BallotRequest
        +buildCheckRequest(id: String, hash: String) BallotRequest
    }

    class ClientCrypto {
        +encryptBallot(optionIndex: int, nonce: String) Ballot
        +deriveReceiptHash(secretKey: String) String
    }

    class SecureSession {
        +connect(host: String, port: int, caPath: String) ResultStatus
        +authenticate(method: String, username: String, password: String) int
        +disconnect()
        +send(req: BallotRequest) BallotResponse
    }

    class BallotRequest {
        -op: RequestOp
        -certName: String
        -electionId: String
        -ballot: Ballot
        -hash: String
        -config: ElectionConfig
    }

    class BallotResponse {
        -status: ResultStatus
        -election: Election
        -receipt: Receipt
        -hasPriorBallot: boolean
        -priorBallotVersion: int
        -tally: List~int~
        -optionCount: int
        -options: List~String~
        -hashes: List~BallotHash~
        -hashCount: int
        -found: boolean
        -foundOption: int
        -foundOptionName: String
    }

    class ResultView {
        -tally: List~int~
        -hashes: List~BallotHash~
    }

    class CheckOutcome {
        <<enumeration>>
        COUNTED
        DROPPED
        UNAVAILABLE
    }

    class VoteAction {
        <<enumeration>>
        MUST_JOIN
        CAST
        UPDATE
    }

    class JoinOutcome {
        <<enumeration>>
        TIMEOUT
        NOT_FOUND
        NOT_ELIGIBLE
        NOT_OPEN
        ADMITTED
    }

    VoterUI "1" -- "1" VoterFormData : holds
    VoterUI "1" -- "1" VoterSession : keeps

    VoterUI ..> VoterController : uses
    VoterUI ..> ResultsController : uses

    VoterController ..> ClientCrypto : uses
    VoterController ..> SecureSession : uses
    VoterController ..> VoteAction : returns
    VoterController ..> JoinOutcome : returns
    VoterController ..> CheckOutcome : returns
    ResultsController ..> SecureSession : uses
    ResultsController ..> ResultView : creates

    SecureSession ..> BallotRequest : uses
    SecureSession ..> BallotResponse : creates
```

An Observer views results through this same client, so `ResultsController` serves UC-5 for both voters and observers.
`VoterController` represents the `bu_*` functions in `libballotclient`; it is not a stored C object.
The executable keeps one `bcl_ctx` transport context and one `bu_session_t` voter session.
Voter identity comes from the username confirmed by the server's LOGIN or REGISTER exchange.

### Solution Class Diagram: admin client (ballotctl)

```mermaid
classDiagram
    class AdminUI {
        -formData: ElectionFormData
        +show(msg: String)
    }

    class ElectionFormData {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        -electionId: String
    }

    class AdminController {
        +prevalidateConfig(cfg: ElectionConfig) ResultStatus
        +buildCreate(cfg: ElectionConfig) BallotRequest
        +buildTransition(op: RequestOp, id: String) BallotRequest
        +foldEligible(names: List~String~) ResultStatus
    }

    class ResultsController {
        +viewResults(id: String) ResultView
        +checkHash(id: String, hash: String) CheckResult
    }

    class ElectionConfig {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        +isValid() ResultStatus
    }

    class AdminChannel {
        +setControlPath(path: String)
        +send(req: BallotRequest) BallotResponse
    }

    class BallotRequest {
        -op: RequestOp
        -certName: String
        -electionId: String
        -ballot: Ballot
        -hash: String
        -config: ElectionConfig
    }

    class BallotResponse {
        -status: ResultStatus
        -election: Election
        -receipt: Receipt
        -tally: List~int~
        -optionCount: int
        -options: List~String~
        -hashes: List~BallotHash~
        -hashCount: int
        -found: boolean
        -foundOption: int
        -foundOptionName: String
    }

    class ResultView {
        -tally: List~int~
        -hashes: List~BallotHash~
    }

    class CheckResult {
        -found: boolean
        -optionIndex: int
        -optionName: String
    }

    class RequestOp {
        <<enumeration>>
        JOIN
        CAST
        UPDATE
        RESULTS
        CHECK
        CREATE
        OPEN
        CLOSE
        PUBLISH
        ADMIN_RESULTS
        ADMIN_CHECK
        ADMIN_NEXT_ID
    }

    AdminUI "1" -- "1" ElectionFormData : holds

    AdminUI ..> AdminController : uses
    AdminUI ..> ResultsController : uses

    AdminController ..> ElectionConfig : creates
    AdminController ..> AdminChannel : uses
    ResultsController ..> AdminChannel : uses
    ResultsController ..> ResultView : creates

    AdminChannel ..> BallotRequest : uses
    AdminChannel ..> BallotResponse : creates
    BallotRequest ..> RequestOp : uses
```

`AdminController` holds no election of its own: it represents the `bc_*` request-building functions, not a stored C object.
`prevalidateConfig` calls the daemon library's authoritative validator, so the config rules exist in one place and the admin sees errors before a round trip.

`BallotRequest` and `BallotResponse` are shared protocol types from `libballotclient.a`.
Unlike ballotu, ballotctl sends every request through a one-shot, owner-only AF_UNIX control socket and never opens a TCP/tetriSH voter session.

### Solution Class Diagram: daemon tier

`ballotd` runs on the admin machine and is the only tier that touches the store.
ballotu reaches `BallotdService` through TCP/tetriSH after LOGIN or REGISTER.
ballotctl reaches it through ballotd's local AF_UNIX control plane.

```mermaid
classDiagram
    class BallotdService {
        +verifyCert(certName: String) CertStatus
        +checkEligibility(e: Election, certName: String) boolean
        +validateConfig(cfg: ElectionConfig) ResultStatus
        +createElection(cfg: ElectionConfig, desiredId: String) String
        +transitionState(id: String, to: ElectionState) ResultStatus
        +recordBallot(id: String, b: Ballot) Receipt
        +publishResults(id: String) ResultStatus
        +getResults(id: String, username: String) ResultView
        +getResultsAdmin(id: String) ResultView
        +lookupHash(id: String, hash: String) BallotHash
    }

    class ElectionConfig {
        -title: String
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        +isValid() ResultStatus
    }

    class Election {
        -id: String
        -title: String
        -state: ElectionState
        -options: List~String~
        -eligibleVoters: List~String~
        -openTime: String
        -closeTime: String
        -tally: List~int~
        +isOpen() boolean
        +isEligible(certName: String) boolean
        +canTransitionTo(to: ElectionState) boolean
        +getTally() List~int~
    }

    class Ballot {
        -certName: String
        -nonce: String
        -payload: byte[]
        -payloadLen: int
    }

    class BallotHash {
        -hash: String
        -optionIndex: int
        -version: int
        -superseded: boolean
        +supersede()
    }

    class Receipt {
        -hash: String
        -issuedAt: String
        +getHash() String
    }

    class ServerCrypto {
        +decryptBallot(b: Ballot) int
        +issueReceipt(b: Ballot, version: int) Receipt
    }

    class BallotStore {
        +exec(cmd: DbCommand) DbResult
    }

    class DbCommand {
        -op: DbOperation
        -electionId: String
        -newState: ElectionState
        -hashRow: BallotHash
        -hash: String
        -nonce: String
        -certName: String
        -config: ElectionConfig
    }

    class DbResult {
        -status: ResultStatus
        -election: Election
        -tally: List~int~
        -hashes: List~BallotHash~
        -found: boolean
    }

    class DbOperation {
        <<enumeration>>
        INSERT_ELECTION
        UPDATE_STATE
        APPEND_BALLOT
        MARK_SUPERSEDED
        NONCE_MARK
        GET_ELECTION
        GET_TALLY
        GET_HASHES
        FIND_HASH
        NONCE_SEEN
        GET_PRIOR_BALLOT
        SET_OWNER
    }

    class ElectionState {
        <<enumeration>>
        DRAFT
        OPEN
        CLOSED
        PUBLISHED
    }

    class CertStatus {
        <<enumeration>>
        INVALID
        EXPIRED
        NOT_ELIGIBLE
        VALID
    }

    class ResultStatus {
        <<enumeration>>
        OK
        ERR_CONFIG_TITLE
        ERR_CONFIG_OPTIONS
        ERR_CONFIG_TIME
        ERR_CONFIG_ID_TAKEN
        ERR_ILLEGAL_TRANSITION
        ERR_NOT_OPEN
        ERR_CLOSED
        ERR_NOT_PUBLISHED
        ERR_NOT_ELIGIBLE
        ERR_CERT_INVALID
        ERR_CERT_EXPIRED
        ERR_BAD_OPTION
        ERR_REPLAY
        ERR_DECRYPT
        ERR_NOT_FOUND
        ERR_NOT_JOINED
        ERR_DB
        ERR_NOT_IMPLEMENTED
        ERR_RETRY
    }

    Election "1" *-- "*" BallotHash : records
    Election "1" -- "1" ElectionState : state

    BallotdService ..> ElectionConfig : uses
    BallotdService ..> Election : uses
    BallotdService ..> Ballot : uses
    BallotdService ..> BallotHash : creates
    BallotdService ..> Receipt : creates
    BallotdService ..> ServerCrypto : uses
    BallotdService ..> BallotStore : uses
    BallotdService ..> CertStatus : returns
    BallotdService ..> ResultStatus : returns

    ServerCrypto ..> Ballot : uses
    ServerCrypto ..> Receipt : creates
    BallotStore ..> DbCommand : uses
    BallotStore ..> DbResult : creates
    DbCommand ..> DbOperation : uses
```
