# Companion protocol v1

The control-frame codec, bounded fragmented-frame assembler and fixed record
codecs are implemented and host-tested. Native handlers include authenticated
pairing, discovery, inventory, transfer, synchronization, content read/removal
and firmware flows; the feature-specific documents describe their integration
and verification limits. Native Apple and physical acceptance remain pending.
The course-context query is routed but not yet advertised, pending the removed
course switch and Apple confirmation flow. This protocol does not authorize an
unauthenticated content endpoint.

## Control frames

BLE characteristic values and decrypted HTTP control bodies carry the same frame.
A frame is exactly one header followed by its declared payload. Transport adapters
must assemble fragmented BLE values into bounded storage before decoding. They
must reject oversize input before copying it. No decoder allocation is required;
payload views borrow the input buffer and expire when it is reused.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 2 | ASCII `LC` |
| 2 | 1 | Protocol version, currently 1 |
| 3 | 1 | Command |
| 4 | 1 | Flags: bit 0 response, remaining bits zero |
| 5 | 1 | Reserved, zero |
| 6 | 4 | Request ID, unsigned little-endian |
| 10 | 2 | Payload length, unsigned little-endian, at most 1024 |
| 12 | variable | Payload |

Commands: 1 discovery, 2 inventory, 3 change exchange, 4 begin transfer,
5 transfer chunk, 6 transfer status, 7 commit, 8 abort, 9 Wi-Fi handoff,
10 firmware installation, 11 error response, 12 installation registration,
13 installation authentication, 14 journal format query, 15 content removal,
16 content read, 17 content metadata, 18 content-read handoff preparation,
19 bound-course context.
Unknown versions, commands, flags, truncation, and
trailing bytes are rejected. Request IDs correlate responses; they do not provide
durable transaction identity or replay protection.

Only discovery can be decoded before authentication. Discovery responses must
contain public capability information only. Transport code must derive the
`authenticated` argument from verified bonded BLE security or successful HTTP
AEAD verification, never a field supplied by the peer. Pairing secrets, content,
reading state, and mutable operations require authentication. The codec supplies
no encryption itself.

## Shared records

Each record starts with a one-byte version (1), then a one-byte record kind.
Integers use unsigned little-endian encoding; identities are 16 raw bytes and
SHA-256 digests are 32 raw bytes. Records have exact lengths: trailing bytes,
unknown versions, and unknown kinds are rejected. JSON fixtures under
`protocol/fixtures/` pair named fields with independently generated `binaryHex`.
No native structs are written to disk or cast from input buffers.

| Kind | Record | Fields after the header, in order | Bytes |
| --- | --- | --- | --- |
| 1 | DeviceDescriptor | device identity, storage generation, board u8, capabilities u32, battery u8, minimum protocol u8, maximum protocol u8, running build digest | 74 |
| 2 | ContentManifest | content digest, kind u8, length u64, format version u32, logical identity | 63 |
| 3 | SyncEvent | event identity, storage generation, kind u8, resource digest, body digest, study day u32, timestamp u64, clock quality u8, scheduler version u32, scheduler configuration digest, ancestor count u8, ancestor identities | 165–293 |
| 4 | SyncCheckpoint | device identity, storage generation, cursor event identity, ancestry digest | 98 |
| 5 | TransferState | transaction identity, paired installation identity, storage generation, content digest, length u64, durable offset u64, phase u8 | 99 |

An event identity is origin identity followed by epoch u64 and sequence u64.
An envelope carries up to four direct causal dependencies. Event bodies remain
separate, content-addressed records; typed reading anchors, bookmark operations,
preferences, review/undo references, and scheduler configuration bodies still
need implementation before event exchange can be enabled. The envelopes alone
provide neither journal durability nor history reconciliation.

Board codes are X4 (1), Sticky (2), X4 Pro (3), X4 Classic (4), and Paper Mono (5). Content kind codes are EPUB (1), course
(2), font (3), dictionary (4), and firmware (5). Event kinds are reading position
(1), bookmark put (2), bookmark delete (3), preference (4), review (5), undo review
(6), suspension (7), lesson completion (8), star (9), and reading completion (10).
Clock quality is unknown (0), device clock (1), or trusted clock (2). Transport
handlers must verify identity ownership and referenced bodies before acceptance.

Transfer phases are receiving (1), verified (2), installing (3), committed (4),
and aborted (5). Offsets cannot exceed the declared length. Verified, installing,
and committed states require the complete length. These checks validate the
record representation. The transfer controller enforces installation ordering,
verifies SHA-256 through its storage adapter, and acknowledges persisted offsets.
BLE handlers and startup recovery call this controller.

## Resource bounds

The session design reserves one reusable 8192-byte workspace in sync mode,
allocated with `makeUniqueNoThrow` and released on exit. The codec does not allocate
this workspace or start radios. Control frames occupy at most 1036 bytes. Inventory
pages are limited to eight entries (inventory handlers are pending). The command
queue holds at most four frames in 4144 bytes carved from the session workspace.
It copies borrowed input, refuses a fifth frame without overwriting queued data,
and requires serialized access from transport callbacks. Clear the queue on
unpairing or ownership changes before accepting a new installation. Payloads
above 1 MiB qualify for Wi-Fi assistance. Radio heap/pool usage still requires
measurement on C3 and S3.

## Verification

Run:

```sh
cmake -S test -B /tmp/lila-companion-tests
cmake --build /tmp/lila-companion-tests --target CompanionFrameTest CompanionRecordsTest CompanionCommandQueueTest
ctest --test-dir /tmp/lila-companion-tests -R Companion --output-on-failure
```

The tests cover a golden frame starting at an unaligned address, all truncated
prefixes, malformed header fields, unauthorized commands, trailing bytes, payload
limits, destination capacity, every two-fragment split, explicit reset, and
rejection of oversized declarations before body reception. Record tests verify
all five fixtures, every truncated prefix, invalid enums and versions, transfer
offset invariants, maximum causal tails, and unchanged output on rejected input.
Queue tests exercise capacity, FIFO order across wrapping, copied payload
lifetimes, authorization rejection, and insufficient workspace.
Transport reconnects, handoff,
and interrupted commits require additional tests when their handlers exist.

## Transfer command bodies

The bounded decoders in `CompanionTransferCommands.h` define these request bodies:

- BeginTransfer: a 99-byte TransferState, one unsigned path-length byte, then
  exactly that many UTF-8 path bytes. The path is absolute, nonempty, contains no
  NUL, and is shorter than 128 bytes. Initial phase must be Receiving and durable
  offset must be zero. The transfer controller additionally rejects hidden,
  empty, and dot path segments.
- TransferChunk: transaction identity (16 bytes), byte offset (u64 little-endian),
  then 1–1000 bytes. The returned span borrows the received payload.
- TransferStatus, Commit, Abort: exactly the transaction identity (16 bytes).

These decoders do not authorize writes. Handlers must resolve the owner from a
reader-side binding to the authenticated Apple installation, verify it against
TransferState.owner, and enforce destination/content policies. A client-supplied
owner field alone never grants authority. The BLE handler applies this binding and accepts EPUBs only at their canonical
hash-named destination. Other content installation policies remain pending.

## BLE connection lifetime

The HAL captures a monotonically increasing session token when it dequeues a
request. Replies require that same authenticated session. Disconnect and reader
unpair invalidate it; reconnect creates a fresh token even if the BLE connection
handle is reused. The HAL serializes notification submission with connection
callbacks so a new peer cannot receive an older session's response. This fence
is transport lifetime protection, not a durable Apple installation identity.

## Session buffer layout

`CompanionWorkspace.h` partitions the one 8192-byte allocation: command queue
0–4143, fragment assembler 4144–5179, request 5180–6215, response 6216–7251,
and transfer scratch 7252–8191. Hashing and journal encoding use only transfer
scratch while BLE runs. No buffer loan may overlap the transport-owned region.

The pinned NimBLE configuration is patched after SDK configuration headers to
allow one host connection and four bonds, matching the HAL and installation
registry. This bounds host connection bookkeeping; the prebuilt ESP controller
and the full host/task/pool heap still require measurement on hardware.

## Declared content transfer capabilities

DeviceDescriptor capability bit 0 (`0x00000001`) denotes manifest-first declared
begin support. Bit 1 (`0x00000002`) denotes validated, recoverable course pack
installation. Course transfer requires both bits. Other set bits do not imply
these features. The Apple runner and app dispatcher apply this gate. The reader advertises bit 0
and, on `LILA_TINTA` builds, bit 1. Its begin handler enforces canonical content
destinations, initial declaration state, authenticated ownership and course format 1.
`CourseTransferCapabilities.json` checks the shared descriptor representation.
Legacy protocol version 1 support by itself does not enable declared transfers.

Bit 5 (`0x00000020`) denotes validated font installation. Bitmap font transfers
require bits 0 and 5. Bit 6 (`0x00000040`) additionally permits vector fonts and
is advertised only on vector-font-enabled builds. The Apple runner checks these
capabilities before sending a font declaration. Font begin requests append a
one-byte destination length and that many UTF-8 bytes to the 162-byte declaration.

Bit 7 (`0x00000080`) denotes validated dictionary installation. Dictionary transfers
require bits 0 and 7. The Apple runner revalidates the original ZIP before sending
a format-1 declaration and derives `/dictionaries/<archive SHA-256>/dictionary`
from its content identity. Renaming the library item does not change this destination.
Firmware does not advertise bit 7 while device acceptance remains pending.

Bit 9 (`0x00000200`) denotes snapshot-bound ordinary journal merge readiness. It
requires journal export bit 3 for the ordinary upload workflow. Tinta-enabled
firmware advertises bit 9 only while its journal export owner is available.
`JRD1`/`JRR1` query formats are documented in `tinta-journal.md`; the bit denotes
query support, not a Ready result for the currently installed learner state.
Apple preparation retains export-only compatibility when bit 9 is absent and
shows an upgrade instruction before creating or resuming an ordinary upload.

## Wi-Fi message encryption foundation

`WifiMessageCipher` implements an Apple-side AES-256-GCM envelope, not a live Wi-Fi transport. Each handoff must negotiate a fresh random 32-byte key and nonzero 16-byte session identifier over authenticated BLE. Never recreate cipher instances with an existing key: directional counters restart at one, so key reuse would reuse nonces. A failed/lost ordered exchange ends the handoff; invalidate the cipher and negotiate a fresh key or resume the transaction over BLE. Retrying a command uses the durable transfer identity/offset, not a reused encrypted message.

The authenticated header is 31 bytes: `LWH` plus version 1 (4 bytes), session identifier (16), direction (1: Apple-to-reader 0, reader-to-Apple 1), counter (u64 little-endian), and plaintext length (u16 little-endian). The wire message is header, ciphertext of that length, and a 16-byte tag. Plaintext is one complete control frame, at most 1036 bytes. Empty messages are rejected. The 12-byte GCM nonce is `LWH`, direction, and the same u64 little-endian counter. Independent directional counters prevent reflection; receivers require the next exact counter and advance only after successful authentication. Counter exhaustion prevents reuse. Invalidation releases the retained key and rejects subsequent operations; it is not a guarantee about allocator-level key erasure.

`protocol/fixtures/WifiMessage.json` contains independently generated AES-GCM key, nonce, header, ciphertext, tag and complete wire bytes. Apple tests compare exact output and tamper with every byte, then verify a valid message still succeeds; they also cover replay, reflection, bounds, session mismatch and invalidation. Firmware AEAD interoperability, key exchange, HTTP transport, radio switching, timeouts and fallback integration remain unfinished. The existing companion does not advertise or initiate Wi-Fi assistance yet.

### Reader AES-GCM adapter

`HalCompanionWifiCipher` implements the same envelope with the installed ESP32-C3 SDK's mbedTLS GCM API. It owns the crypto context/header/nonce outside task-local storage, borrows disjoint input/output buffers and performs no per-message allocation. SDK key-context allocation is confined to begin/setkey, checked for failure, reused and freed at end/destruction. The caller must allocate this session owner safely outside the task stack and serialize access. Authentication failures clear the requested plaintext output and return length zero without consuming the receive counter. Encryption failure clears its output and ends the handoff; Apple sealing now also invalidates its key on crypto failure. Invalid sizes/overlaps do not consume counters.

Two HAL tests compare the production envelope code, using an OpenSSL-backed mbedTLS shim, with the independent shared fixture; they exercise byte-wise tampering, plaintext clearing, directions, replay, teardown, exact maximum payload and overlapping buffers. Both pass. The default C3 firmware build compiles the real SDK adapter successfully. This is API/codec evidence, not hardware AES-GCM execution or heap measurement. The adapter is now owned by the live activity through the combined Wi-Fi endpoint; physical C3/S3 interoperability remains pending. On-device verification must decrypt the shared fixture, encrypt its reverse-direction reply for the Apple client, exercise failed tags without exposing plaintext, and measure setup/repeated-session heap usage.

### Ephemeral handoff offer

`CompanionWifiHandoff.h` and `WifiHandoffOffer.swift` define a fixed 121-byte offer: version 1, reader identity, storage generation, paired installation identity, transfer transaction identity and fresh session identity (five 16-byte fields), fresh 32-byte key, four IPv4 address bytes, port u16 little-endian, and lifetime seconds u16 little-endian. All identities/key must be nonzero, the address must be unicast (excluding unspecified/loopback/multicast), port must be nonzero and lifetime is 1–120 seconds. The key must be generated from a secure entropy source for each offer; the codec's nonzero check does not prove entropy or freshness. Measure expiry locally with a monotonic clock, starting when received; it is an offer lifetime, not a transfer deadline.

Offer binding checks all four expected reader/storage/installation/transaction identities before use. Receive this record only through authenticated BLE, never persist or log it, and discard it on failed handoff, expiry, unpairing or connection ownership change. The codec does not authenticate arbitrary bytes by itself. `protocol/fixtures/WifiHandoffOffer.json` is shared by both implementations. The C++ round-trip/binding/malformed test and all 222 Swift tests pass. The optimized host decoder passes the 256-byte frame limit without exceptions/RTTI. Live firmware uses this header for handoff dispatch, fresh credentials and the original offer deadline. Apple negotiation and physical radio acceptance remain unfinished; no Wi-Fi capability is advertised. Device verification must reject an offer for another installation, card or transaction before starting Wi-Fi, and reject expired/reused offers after reconnect.

### Apple encrypted transfer transport

`WifiHandoffTransport` implements CompanionTransport over a bounded/cancellable WifiMessageTransport provider. Its constructor checks the offer against the expected reader, card, installation and transaction and takes the offer receipt timestamp from the same local monotonic clock used for expiry. First exchange must complete within the offer lifetime. After activation each successfully authenticated/correlated response renews a 30-second idle budget; a continuous large transfer is not limited by the original offer lifetime. The provider receives the remaining budget capped at 30 seconds and must enforce timeout, cancellation and response-body bounds itself.

Only BeginTransfer, TransferChunk, TransferStatus, Commit and Abort are permitted. Begin checks installation/storage/transaction in both legacy and declared formats; other operations require the offer's transaction. Response flag, request ID and command are checked after decryption, with Error replies permitted for protocol failures. Concurrent exchange is rejected; binding/protocol/crypto/network/cancellation/expiry failures close the cipher and wire provider. A closed handoff cannot reuse its counters. The adapter checks activity and expiry after awaited decryption as well as network receipt, so teardown cannot revive a pending exchange.

All 227 Swift tests pass, including idle renewal beyond the offer lifetime, exact expiry before network traffic, a late response, lost/tampered/wrongly correlated replies, closed-session rejection and ownership checks for both Begin formats. Tests use an encrypted in-memory provider and an injected monotonic clock. No HTTP provider or live radio handoff is wired yet; these results do not establish native network cancellation, native UI behavior or firmware handoff. Physical acceptance must include a dropped HTTP response followed by fresh-key negotiation/BLE offset resume, without reusing the ended cipher.

### Bounded Apple HTTP provider

`WifiHTTPMessageTransport` supplies the encrypted adapter with POST exchanges at the offer's literal IPv4 address/port and `/companion/v1/messages`. Its ephemeral URLSession disables cache, cookies, credential storage and configured proxies, permits one connection per host, and rejects redirect/authentication callbacks. On Apple it disables cellular use and waiting for connectivity. It accepts only HTTP 200 binary responses, rejects declared oversized responses before collection, and caps accumulated body bytes at 1083 even when length is unknown. Collected ciphertext is valid only after WifiHandoffTransport authenticates it; the HTTP provider does not establish reader identity itself.

Each request has a locked completion state and a cancellable timeout task. Parent cancellation, close, timeout and response failure resume the continuation once, cancel the URLSession task, and invalidate the failed transport. Cancellation before continuation registration remains observable, preventing a lost-cancellation hang. Timeout budgets must be nonzero and at most 30 seconds. Explicit close and object destruction invalidate the session; successful requests can reuse its connection. The provider never stores the handoff key.

All 233 Swift tests pass. URLProtocol-backed tests exercise request construction, bounded collection, HTTP/content-type failures, real URLSession task timeout/cancellation, post-failure closure and invalid requests with no HTTP start. Direct callback tests cover redirect rejection and pre-registration cancellation. These are host FoundationNetworking tests; native Apple SDK typechecking, local-network/ATS setup, real HTTP socket behavior and app lifecycle still require Apple builds and physical verification. The live app does not yet create this provider or negotiate a handoff; the firmware HTTP endpoint and radio ownership remain unfinished.

An all-zero IPv4 field in a handoff offer now denotes an address that must be discovered after joining Wi-Fi; other zero-first-octet addresses, loopback and multicast remain rejected. This accommodates saved-network DHCP without starting Wi-Fi alongside BLE. The HTTP provider requires an explicit validated four-byte resolved address for such offers and rejects address overrides for fixed-address offers. Resolution supplies routing only: the existing session-bound AES-GCM exchange authenticates the peer. The actual discovery publisher/resolver and radio transition remain unimplemented; this change alone does not enable live Wi-Fi transfers. Swift and C++ offer tests cover the zero-address sentinel and malformed alternatives, and Swift checks missing/invalid resolution and fixed-address overrides.

The discovery instance and hostname are `lila-` followed by the session identity's 32 lowercase hexadecimal digits; the Bonjour service type is `_lila-sync._tcp.`. Swift exposes these identifiers on `WifiHandoffOffer`; firmware generates the same null-terminated name into a caller-owned 38-byte buffer. The name is ephemeral routing information, not proof of peer identity. No key, installation identity, transaction, or learner content is advertised.

`HalCompanionWifiDiscovery` publishes this instance and the offered port through the C3 SDK's mDNS responder. It requires exclusive ownership of the global responder, initialized Wi-Fi/network interfaces, and a listening companion endpoint. It checks initialization, hostname registration, and service registration, frees successful initialization after later setup failures, and releases the responder on replacement, explicit end, or destruction. There are no application heap allocations: the SDK allocates its task/records, reports allocation failures through checked return values, and frees them with `mdns_free`. These SDK allocations still require device heap measurements before live enablement. Host tests compile the production HAL adapter against a fault-injected mDNS stub and exercise each setup failure, retry, replacement, and destruction. The activity now starts publication after listener readiness; the Apple resolver exists, while app negotiation and physical browsing remain pending.

Verification: the discovery HAL passes the host compiler's 256-byte frame ceiling with optimization and exceptions/RTTI disabled, and the default C3 firmware build compiles the real SDK-backed adapter successfully. The adapter is now reachable from the live activity; compilation does not establish runtime RAM cost. During a handoff, browse `_lila-sync._tcp` with macOS `dns-sd -B _lila-sync._tcp local`, check the instance against the authenticated offer, resolve the advertised port/address, and monitor internal free/largest heap before and after repeated discovery sessions. Target stack watermarks and SDK allocation recovery need physical hardware verification.

### Apple Bonjour resolution

`WifiBonjourResolver.resolve` accepts a discovery offer and the local monotonic time at which the offer arrived. Its Foundation driver browses only `_lila-sync._tcp.` in `local.`, resolves one exact session-derived instance, and requires both the service port and the Darwin IPv4 socket address port to equal the offered port. It rejects malformed, loopback, multicast, and zero-first-octet addresses, ignores IPv6 for the current IPv4 protocol, and examines at most sixteen supplied addresses. Discovery results provide routing only; the next encrypted exchange must authenticate the peer with the original offer's key/session.

The original offer lifetime bounds browsing plus resolution. Already-expired offers and backward monotonic clocks fail before browsing; late callbacks cannot extend the deadline. A timer independently ends a stalled operation. Every completion stops and unschedules the browser and resolver; cancellation before start sends no browse, and pending cancellation stops the operation. Callers can supply the returned four-byte address to `WifiHTTPMessageTransport` and retain the same receipt time for `WifiHandoffTransport`. The public native adapter is available only on Darwin; Linux tests exercise the shared lifecycle through a driver fixture and the actual matching/socket parsing functions. They do not exercise Foundation Bonjour or establish Apple SDK typechecking. The Foundation NetService APIs are deprecated by Apple in favor of Network framework; native API validation and the final networking API choice remain part of native acceptance.

The app's source plist now declares `_lila-sync._tcp`, a localized local-network purpose string, and `NSAllowsLocalNetworking` for the local HTTP endpoint. This does not grant local-network permission automatically. On iPhone/Mac, verify allow/deny behavior, exact-instance filtering with other services present, both fixed and DHCP endpoints, timer/cancellation cleanup, and authenticated rejection of a spoofed instance. The live app handoff call, BLE negotiation, hotspot joining and reader HTTP endpoint remain unfinished.

### Handoff preparation

`WifiHandoffConnector.prepare` composes the native Bonjour resolver, bounded URLSession HTTP provider, and encrypted transaction-scoped transport. It checks reader identity, storage generation, Apple installation and transaction before discovery or HTTP setup. Fixed-address offers bypass Bonjour and cannot receive a resolved-address override. Discovery receives the original receipt time, and setup checks the same monotonic deadline before resolution, after resolution, and after wire creation. The resulting transport retains that receipt time, so discovery time is deducted from the first request's available budget. If cancellation or expiry occurs after wire creation, preparation closes it before returning the error. Discovery failure or invalid routing data never creates the HTTP provider.

Five connector tests cover both endpoint modes, all four foreign bindings, expired/backward clocks, failed/malformed/late resolution, actual task cancellation during resolution and wire creation, and cleanup after expiry. A composed URLProtocol test uses the production URLSession provider and both AES-GCM directions: it verifies that the discovered IP is selected, the posted bytes decrypt to the intended transaction-bound control request, and the reply authenticates and decodes. The full CompanionKit suite passes 246 tests. This uses an in-process HTTP fixture, not a real socket or native Bonjour run; Apple SDK typechecking and device execution remain unverified.

The preparation method must be called after receiving a fresh-key authenticated BLE offer and transitioning to Wi-Fi. It does not stop/start radios, join a hotspot, negotiate offers, or restore BLE after failure. The reader now owns the radio transition and HTTP endpoint; the app does not yet negotiate handoff or invoke preparation. Do not advertise live Wi-Fi capability on the strength of these host tests. Native verification must exercise the complete transition and retry path while retaining the durable transaction and offset.

### Reader encrypted message processing

`HalCompanionWifiMessages` provides the session-side processor for the HTTP endpoint. Begin requires the validated offer to match reader, storage generation, installation and transaction, a complete existing 8 KiB workspace, and a monotonic millisecond clock. It checks offer expiry before setting the AES-GCM key and sends in the reader-to-Apple direction. Each handoff must supply a fresh random key; this processor does not generate or negotiate one. It retains the cipher and nonsecret transaction bindings rather than a copy of the raw offer key.

The processor partitions the workspace into bounded ciphertext request, plaintext request, reply payload, reply frame and ciphertext reply regions. A compile-time check keeps all message storage before `TRANSFER_OFFSET`; the existing transfer scratch region is untouched. It creates no application heap buffers. The session object contains the SDK cipher context and must be session-owned outside the small task stack when integrated, allocated with checked `makeUniqueNoThrow`. The SDK key-context allocation is checked by the cipher adapter and released at session end. BLE must relinquish the shared workspace before this processor starts; the activity must serialize calls and keep the clock/dispatch contexts alive.

After authentication/decryption, `validWifiTransferRequest` permits only begin/chunk/status/commit/abort requests for the offered transaction. Both legacy and declared begin bodies must decode completely and bind installation and storage generation as well as transaction. Chunk/status/commit/abort validate their exact transaction body shape. The existing content/destination semantics remain the transfer handler's authority. Invalid authentication, frames or bindings end the ephemeral session without dispatch. No SD operation occurs in this layer; a function-pointer dispatcher supplies the existing transfer handler and its recovery/catalog side effects, without a heap-allocating callback.

The original offer receipt time bounds the first exchange; completed exchanges renew a 30-second inactivity window. Deadline checks run before decryption, before dispatch, after dispatch and after encryption. A reply that finishes late is discarded and the session ends; durable changes already made by dispatch must be recovered through the existing transaction/offset protocol. Replay cannot dispatch twice. Session failures clear message storage and the cipher while preserving transfer scratch; reentrant processing is rejected without interrupting the outer call. Dispatch may end the session, but must immediately stop using its borrowed request/reply buffers.

Two portable binding tests and five production-HAL processor tests pass against the OpenSSL-backed host cipher shim. Coverage includes all four command bindings where applicable, malformed/truncated begins, disallowed commands, maximum-size chunks, both cipher directions, tampering, replay, initial and idle expiry, late dispatch, invalid replies, teardown and reentrant processing. Both validator and processor pass the optimized host 256-byte frame limit with exceptions/RTTI disabled. The live activity now connects this layer to the listening socket and actual transfer dispatcher. The host tests do not establish physical AES interoperability, target task-stack usage or runtime heap cost.

The default C3 build successfully compiles the new real-SDK message processor. It is now reachable from the live activity; the build's reported static RAM still does not measure an active Wi-Fi session. On hardware, verify a real maximum-size encrypted chunk, replay rejection, a lost reply followed by BLE offset recovery, idle expiry, and repeated session entry/exit while monitoring internal free/largest heap and task stack watermarks. Require the plan's 50 KiB free-heap floor with the HTTP listener, SDK mDNS and Wi-Fi allocations active.

### Bounded reader HTTP request parsing

`WifiHttpRequest` is the streaming parser for the companion POST route. It owns a fixed 256-byte line buffer and borrows the encrypted message processor's request buffer; it does not allocate a POST body, header map, or string. Its object must be session-owned outside the small task stack. Input passed to `feed` must not overlap body storage. Headers are limited to 2,048 bytes including the request line and CRLF delimiters, individual lines to 255 bytes, and the binary body to 48–1,083 bytes. The caller must impose a socket deadline; the parser alone does not perform or time network I/O.

Only exact `POST /companion/v1/messages` HTTP/1.0 or HTTP/1.1 request lines are accepted. HTTP/1.1 requires a nonempty Host. Both versions require one decimal Content-Length and one application/octet-stream Content-Type. Relevant header names and MIME values are ASCII case-insensitive; surrounding space/tab is trimmed. Duplicate length/type/Host, whitespace in header names, folded headers, malformed CRLF, control bytes, chunked/other Transfer-Encoding, Content-Encoding, Expect, and alternate routes are rejected. Binary bodies preserve NUL/high-bit bytes. Truncated input and trailing bytes fail, and the same parser can be reset for the next connection. Rejected headers do not write the borrowed body buffer; partial bodies on truncation must be cleared by the message-session owner.

Seven host tests cover every request split, bytewise input, binary content, undersized storage, exact and excessive body/header/line limits, huge decimal lengths, duplicate framing, unsupported encodings, strict route/line parsing, mandatory headers, truncation at every position, trailing bytes, reset, mixed-case headers and HTTP/1.0. The production parser passes the optimized host 256-byte frame check with exceptions/RTTI disabled. It remains unused in firmware until the HAL HTTP listener is implemented; no target build or live socket behavior is established by these tests. Listener errors must log, close the connection, and preserve the processor's authenticated counter/session rules before returning to the activity.

### HAL HTTP listener

`HalCompanionWifiHttp` connects the bounded request parser to the encrypted message processor using the C3 SDK's nonblocking lwIP socket API. It owns one listening socket and one accepted socket, with backlog one. The reader must already have initialized Wi-Fi and the authenticated message session before begin. Socket creation, nonblocking setup, bind, listen and fatal accept errors are checked and logged; failed setup closes owned sockets and ends the message session. The caller supplies the same monotonic clock and a function-pointer transfer dispatcher. The listener and processor must be activity-owned, serialized, and stopped in that order before destruction or BLE workspace reuse.

Each poll reads or writes at most 2 KiB. Reads use a 128-byte local buffer, feed the fixed-size parser, and write the ciphertext body directly into its borrowed workspace destination. Completed bodies authenticate and bind through the message processor before dispatch. Replies use a fixed 128-byte member header buffer, application/octet-stream, exact Content-Length, and Connection: close. Partial writes retain offsets; would-block/interrupted syscalls yield to the next poll. There is no C++ heap buffer or WebServer/String allocation in this adapter. lwIP still allocates socket/PCB and network buffers; checked calls report failures, and device measurements must include these resources.

A connection has separate five-second receive/send phase deadlines. Invalid/incomplete HTTP and receive timeouts close the unauthenticated connection without consuming cipher counters. A send failure or reply timeout ends the encrypted session because dispatch may already have changed durable state. `HalCompanionWifiMessages.pollDeadline` also ends an idle or never-activated session without requiring another request to arrive. False from listener poll tells the radio owner to restore BLE and retain the durable transaction for offset recovery; this layer does not perform that radio transition itself.

Six tests compile the production adapter with a shim that maps lwIP calls to real Linux TCP sockets, and the production cipher against the OpenSSL host shim. They cover encrypted replies across repeated connections, a maximum-size chunk crossing parser/socket read boundaries, malformed HTTP, receive timeout followed by a valid request with the unchanged cipher counter, tampering, idle expiry, failed bind cleanup and reply timeout after authenticated dispatch. The dispatcher in these socket tests is a fixture, so they do not establish actual SD durability or a full Tinta installation. The existing five processor tests remain green. The listener passes the optimized host 256-byte frame check and the default C3 firmware build compiles the real SDK-backed source.

The listener is now constructed through the live activity endpoint owner, with reader BLE negotiation, radio ownership and the actual transfer dispatcher. Apple invocation remains pending. On hardware, verify partial/disconnected sockets, SDK backpressure, malformed/slow clients, lost replies followed by BLE offset recovery, repeated entry/exit, and the plan's free/largest-heap and task-stack requirements with Wi-Fi, mDNS, sockets and the listener active. Successful compilation does not establish those runtime resource costs.

### Authenticated-radio handoff entropy

`HalCompanionBluetooth::generateWifiHandoffSecrets` now generates the 16-byte session identity and 32-byte AES key while holding the existing BLE mutex. It requires a running BLE controller, the exact accepted authenticated connection token, and the Authenticated transport state. The installed C3 SDK's esp_random.h states that hardware RNG output has true entropy while Bluetooth or Wi-Fi is enabled; `esp_fill_random` uses that RNG. Holding the mutex prevents the radio owner from entering its shutdown section or authentication callbacks from changing the accepted session during generation. The caller must still serialize begin/stop/object lifetime as required by the rest of the HAL, and separately authorize the Apple installation and transaction before negotiating an offer.

The portable `generateWifiHandoffSecrets` helper calls a synchronous checked source for 48 bytes, rejects missing/failed sources and all-zero session/key fields, and publishes outputs only after validation. It clears previous output secrets before generation, so failures cannot leave an older handoff key available for accidental reuse. Its 48-byte stack scratch and explicit output-clear helper use volatile byte writes; no application heap allocation is added. Temporary scratch is wiped on every exit. This provides memory clearing for these arrays, not a guarantee about compiler registers or copies subsequently retained by the cipher/provider.

Two host tests verify the source is invoked for each generation, field layout, explicit clearing, and failure/zero-output cases. The helper passes the optimized host 256-byte frame limit with exceptions/RTTI disabled. The tests inject deterministic bytes and do not verify hardware entropy, the actual NimBLE mutex/state gate, or radio timing. Live authenticated BLE offer negotiation, independent hotspot credentials, network join metadata and radio ownership remain pending; this API alone does not enable or advertise Wi-Fi capability. Physical verification must reject zero/stale/unauthenticated tokens with cleared outputs, generate distinct handoff sessions while BLE is authenticated, and discard/wipe any offer whose BLE send fails before stopping that radio. Never persist, cloud-sync or log handoff keys.

The final default C3 firmware build passes with the real authenticated-radio RNG method. The disabled LILA_COMPANION=0 HAL branch also compiles on the host without NimBLE/SDK headers; it clears the requested secret arrays, logs unavailability and returns false. These compilation checks do not replace native-radio or device entropy/lifecycle acceptance.

### Network join envelope

`WifiNetworkOffer` adds authenticated-BLE join metadata around the existing 121-byte handoff offer. Version 1 is one envelope-version byte, the complete offer, one mode byte (1 saved network, 2 reader hotspot), one SSID-byte length, one password-byte length, and the exact SSID/password bytes. Total size is 125–220 bytes. SSIDs contain 1–32 bytes without NUL. Saved-network envelopes require an empty password field: the reader's saved Wi-Fi password is not sent to Apple. Saved SSIDs retain raw bytes rather than silently normalizing or replacing invalid UTF-8; eventual UI guidance must handle names it cannot render.

Hotspot envelopes use printable ASCII SSIDs and 8–63-byte printable ASCII WPA passphrases, matching the intended generated reader hotspot. They exclude control/invalid UTF-8 strings at the OS join API boundary. Hotspot passwords must be independently generated; never derive them from the AES key or use the fixture password in production. Neither Swift envelope nor the base offer conforms to Codable. Their encoded form contains secrets and is for ephemeral authenticated BLE delivery only, not library/cloud persistence or logs.

The firmware description borrows its SSID/password views from the packet. Its radio owner must copy them into bounded null-terminated session storage before releasing BLE or reusing the shared workspace; string_view.data() is not a C API argument. Decode validates exact lengths, modes and the entire base offer before publishing the output; its temporary copy of the base key/session is explicitly wiped. Encoding validates before writing and requires source strings disjoint from the destination. No firmware heap allocation is added. Swift owns small Data values on Apple, where the reader RAM ceiling does not apply.

Two shared fixtures describe saved-network DHCP discovery and a fixed-address hotspot. Two C++ and two Swift tests pass, covering exact round trips, all truncations, malformed versions/modes/lengths, oversized/trailing packets, raw saved SSIDs, maximum fields, and rejection of saved-password disclosure, short/oversized hotspot passphrases and nonprintable hotspot values. The optimized firmware decoder passes the host 256-byte frame ceiling with exceptions/RTTI disabled. The live BLE activity now exchanges this envelope; the iOS/Mac join flow remains unwired. Verify saved-password omission, independently generated temporary hotspot credentials, SSID guidance and OS join/cancel behavior once negotiation and the radio owner are wired.

### BLE handoff control commands

WifiHandoff command 9 now has versioned prepare/activate/cancel codecs in C++ and Swift. Prepare is exactly 19 bytes: version 1, action 1, network mode, and nonzero transaction identity. Its successful reply is the complete `WifiNetworkOffer` envelope. Activate/cancel are exactly 34 bytes: version 1, action 2/3, nonzero transaction and nonzero offered session identity. Their success reply is exactly one zero byte. Existing command-Error replies carry one control-error byte. These formats separate obtaining a usable key/network offer from requesting the radio transition; preparation must leave BLE active.

The Apple response helpers validate direction, command and request ID before accepting a reply. Offer acceptance also checks requested mode, reader, storage generation, installation and transaction. Acknowledgements require a structurally valid activate/cancel request and exact success body. The firmware codecs enforce versions, actions, exact lengths and nonzero identities while preserving decoder output on failure; authorization, current-transfer ownership, offer lifetime and pending-session matching remain the radio controller's responsibility. No firmware heap allocation is added.

Shared fixtures cover both prepare modes, activate and cancel. Two C++ and three Swift tests pass, including truncation, invalid actions/identities, response correlation, all offer bindings, wrong mode, malformed acknowledgements and remote errors. Both firmware decoders pass the optimized host 256-byte frame check. The live reader activity now uses these helpers; the native app negotiation workflow remains unwired. No Wi-Fi capability is advertised.

Activation delivery remains an integration requirement. `HalCompanionBluetooth::send` uses NimBLE notifications whose successful enqueue does not prove application receipt; do not deinitialize BLE simply because that method returned true. The radio owner/client workflow must either provide completion confirmation or explicitly handle a lost activation acknowledgement and use the first authenticated encrypted HTTP exchange to confirm the switch. Preparation alone must never turn on Wi-Fi or release BLE. Failed/expired offers and cancellation must wipe ephemeral secrets and preserve the durable transfer/offset; live transition, retry and hotspot-join tests remain pending.

### Pending reader offer ownership

`WifiHandoffLease` owns the offer and bounded null-terminated SSID/password buffers while BLE remains active. Its object belongs to the activity/session outside the small task stack; it makes no heap allocation. These copies are needed because the network envelope's string views borrow the BLE workspace that Wi-Fi will reuse. Preparation requires a nonzero authenticated transport token, matching requested mode/transaction, actual reader/card/installation bindings, and a Receiving or fully Verified current transfer. Preparing another offer while one is pending is busy rather than silently replacing its key.

Activate/cancel must match the retained connection token, installation, transaction and offered session. Foreign commands preserve the legitimate pending offer. Cancel clears it; repeated matching Activate is idempotent before the radio switch. Reconciliation clears a prepared offer on connection/installation change, expiry or a backward clock. After an accepted Activate, a zero BLE token can represent the expected disconnection and preserves the pending offer only until its original deadline; a different authenticated token/owner still clears it. The radio owner must invoke reconciliation with actual authenticated state, not caller-supplied command identities, and reset explicitly on user cancellation or recovery blockage.

Only ActivationRequested state may consume setup material. The synchronous sink receives borrowed offer/strings and the original receipt time; it must copy needed values into its cipher/radio state before returning. Reentrant setup is busy. Both setup success and failure reset the lease and explicitly wipe the retained key/session/password, so the same pending material cannot be consumed twice. A retry requires a newly generated offer from the authenticated-radio RNG API, never re-preparing an old captured envelope. The lease does not maintain a history of all previously used keys; cryptographic freshness remains the generating radio owner's responsibility. The sink/radio owner must clean up partially started hardware resources when setup fails.

Six host tests cover string ownership after the input is changed, original receipt time, activation/cancellation, one-shot success/failure, secret-array clearing, reentrancy, foreign command/offer bindings, expiry, owner/connection changes, backward time and current-transfer phase checks. Prepare/encode/consume pass the optimized host 256-byte frame check with exceptions/RTTI disabled. The live activity owns this holder; the lease itself performs no radio, socket, SD or content mutation. Apple handling of a lost activation acknowledgement and physical transition tests remain pending.

### Ephemeral hotspot password generation

The authenticated BLE HAL can now generate a separate `WifiHotspotPassword`: 16 fresh random bytes encoded as 32 lowercase hexadecimal characters plus a null terminator. This is a WPA passphrase, not a 64-character raw PSK. It uses a separate entropy draw from the application encryption key and session identity; the network password must never be derived from either. The caller owns the fixed 33-byte buffer, explicitly clears it after copying into the pending offer, and clears the offer on cancellation/expiry/setup completion. Failed or unauthenticated generation clears previous output. Temporary random bytes are volatile-wiped on every exit; no heap allocation is introduced.

The BLE mutex and authenticated-session checks match handoff-key generation, keeping the Bluetooth entropy source active during the draw. Four host secret-generation tests pass, covering independent draws, encoding/termination, explicit clearing and failure clearing. The optimized host password wrapper stays below the 256-byte frame limit. The live radio controller invokes this API for each hotspot offer. Apple joining and device entropy/heap verification remain pending.

### Combined reader Wi-Fi endpoint ownership

`HalCompanionWifiSession` now owns the message cipher, bounded HTTP listener and exclusive mDNS publication as one activity-owned object. The radio owner must stop BLE, establish the Wi-Fi interface and retain the shared workspace before calling `begin`; stop this session before tearing down Wi-Fi or restoring BLE. The holder adds no allocation or second workspace. Its members exceed the small task-stack budget, so live integration must construct it outside that stack using checked activity/session ownership.

Startup validates all offer bindings and the original deadline through the message processor, opens the listener, then publishes discovery. Any failure or expiry during publication ends all resources. Cleanup removes discovery before closing sockets and wiping the cipher/workspace; transfer scratch remains intact. Polling propagates listener/authentication/deadline failure through the same cleanup. Explicit cancellation and destruction are idempotent. An active session rejects replacement, and cancellation/reentry during startup cannot resurrect it. A later retry still requires fresh session/key material; this holder is not a global key-reuse detector.

Seven host tests run the production cipher, socket, discovery and combined-owner implementations with real POSIX listener sockets, the OpenSSL crypto shim and fault-injected mDNS. They cover listener failure before publication, each discovery failure and cleanup, expiry during startup/polling, active replacement rejection, cancellation/reentry and destruction, port release, an unauthenticated HTTP message ending the whole session, and workspace wiping with transfer scratch preserved. The combined owner passes the optimized 256-byte frame check with exceptions/RTTI disabled. The live activity composes this holder with the checked radio HAL. Apple joining and physical memory/transition acceptance remain pending; the endpoint holder does not start Wi-Fi itself.

### Checked reader Wi-Fi radio ownership

`HalCompanionWifiRadio` implements exclusive saved-network or WPA2 hotspot startup behind the HAL. The caller must stop BLE and other networking owners first, and keep this holder outside the task stack because its fixed `wifi_config_t` credential union is large. No application heap buffer is added. SDK interface/driver/event-loop allocations are checked, retained as explicit ownership and released on failure or `end`; physical SDK heap cost remains unmeasured.

The SDK's `esp_netif_create_default_wifi_sta/ap` helpers explicitly abort on initialization errors, so this implementation uses checked `esp_netif_new`, attach and handler APIs instead. It refuses an already initialized Wi-Fi driver, disables Wi-Fi NVS in the initialization configuration, and requires `WIFI_STORAGE_RAM` before setting credentials. Hotspots require the validated printable passphrase, WPA2 and one connected station. Saved-network credentials remain reader-local, with full 32-byte SSID/64-byte raw-PSK support and no C-string boundary reads. The fixed credential union is volatile-wiped immediately after the SDK copies it and again on every cleanup path. No credentials are logged or written through HalStorage.

Startup creates only the chosen interface and initiates station association asynchronously. `address` returns a valid non-loopback IPv4 address only once available, preserving output while not ready. The outer handoff controller must enforce its original deadline while waiting, compare any fixed offered address to the actual address, and start the encrypted endpoint only after readiness. This HAL does not perform HTTP, discovery, BLE restoration or saved-credential selection itself.

Cleanup stops the driver, deinitializes it, detaches/destroys the interface, then deletes only a default event loop created by this owner. An existing shared loop is retained. Failed teardown retains ownership for a later retry and forbids a replacement start; the caller must not restore BLE until `end` succeeds. The SDK interface detach destroys its driver even when returning an error, so retry does not detach it a second time. The global `esp_netif_init` subsystem is shared and is not deinitialized.

Six host tests compile the production radio HAL against fault-injected SDK shims. They cover all eleven startup failure stages, four cleanup failures and retry, WPA2/single-client/RAM-storage configuration, saved-network bounds, non-ready address output, invalid credentials, foreign driver/shared-loop ownership, explicit cleanup and destruction. The optimized host implementation passes the 256-byte frame check. The compiled C3 radio object also shows a 256-byte `begin` frame and at most 32 bytes for the other emitted radio methods; this does not include SDK callee stack use or prove physical memory behavior. The live activity now invokes this HAL. Native radio operation, Apple hotspot joining, reconnect behavior and repeated C3/S3 heap/stack acceptance remain pending.

### Live reader handoff dispatch

`CompanionConnectActivity` now accepts WifiHandoff prepare/activate/cancel only after installation authentication bound to the current BLE peer. Preparation requires a current transfer above 1 MiB, uses fresh authenticated-radio session/key entropy and a separate hotspot-password draw, and leaves BLE active. Saved mode uses a mutex-protected fixed-buffer copy of the last connected credential; its password remains local and is omitted from the offer. Hotspot SSIDs use a truncated ephemeral session name, with independently generated passwords. Both modes offer address discovery, port 8080 and a 120-second initial deadline. No Wi-Fi capability bit is advertised yet because the native app negotiation/join workflow remains unwired.

The activity owns the lease, credential buffers, radio and endpoint as members of its existing checked heap allocation (`ActivityManager::goToCompanion`); there is no additional session-buffer allocation. The shared 8 KiB workspace is reused only after stopping BLE. Failed prepare delivery clears the pending offer, and consumed input/response buffers are volatile-wiped after notification enqueue. Cancellation, expiry, unpairing, authentication ownership changes, low-heap shutdown and recovery blockage clear pending key/password material. Saved credentials are copied while holding their store mutex without constructing strings, reserving vectors or writing settings.

Accepted activation is consumed on the next activity loop. The sink rechecks current transfer and all offer bindings, copies the original receipt time/key into member storage, stops BLE, then starts the checked radio. It waits asynchronously for an IPv4 address within the original deadline before starting the bound encrypted endpoint and mDNS publication. The first encrypted HTTP exchange confirms the radio switch. A notification enqueue is not proof of Apple receipt: activation acknowledgement may be lost during BLE shutdown, and the Apple workflow must tolerate that loss using the already validated fresh offer. Preparing alone never starts Wi-Fi.

The activity polls the endpoint through the same SD transfer dispatcher used by BLE. Failure, initial expiry or subsequent idle expiry ends discovery/listener/cipher, then Wi-Fi, before restarting BLE. Transfer scratch, durable transaction and offset remain available; a new BLE authentication is required. If radio cleanup fails, the holder retains ownership, retries once per second and blocks navigation/auto-sleep until cleanup succeeds. The runtime 50 KiB internal-free-heap floor is checked after radio startup, after endpoint setup and during polling. These guards do not substitute for measured C3/S3 memory acceptance.

All 55 Wi-Fi/credential host tests pass, including three new bounded credential-snapshot tests for substring/full-length sources, zero termination, malformed/missing credentials, secret clearing, short buffers and canaries. These tests exercise the protocol, radio/endpoint HALs and copy helper, not the entire renderer/activity loop. Native C3 compilation succeeds; the added activity methods have 16–192-byte frames in the inspected C3 object, with a 224-byte lease encoder. SDK callees and task high-water marks still need device measurement. Physical acceptance must cover saved/AP joining, failed/expired offers, lost activation replies, concurrent transfer recovery, repeated BLE restoration, reading resumption and free/largest heap without accumulating loss.

### Apple BLE handoff negotiation

`WifiHandoffNegotiator` provides a serialized authenticated-session prepare/activate operation. Its public entry point requires `AuthenticatedReaderSession`; raw transport injection is internal for tests. It validates the complete network offer, requested mode and reader/card/installation/transaction bindings before sending activation. Actor reentry is busy, request IDs advance without wrapping, and the original initial-budget timestamp is captured before the prepare round trip, conservatively accounting for BLE latency. Cancellation, exact expiry and backward time are checked before activation and after its reply or transport error.

A valid success acknowledgement yields `activationAcknowledged = true`. Only a native Bluetooth timeout/disconnection can return an uncertain activation candidate using the already validated offer. Malformed replies, explicit control errors, stale-session errors and other transport failures are rejected. Both results still require joining the offered network and a successful authenticated HTTP exchange to confirm the switch. An uncertain result does not prove the reader received activation, grant a fresh lifetime or permit recycling a used cipher/key. The native BLE transport separately assigns monotonic wire IDs, checks connection ownership and disconnects on timeout/cancellation, isolating late replies from later operations.

Five negotiation tests cover successful binding/activation and distinct IDs, classified lost acknowledgement, rejected/malformed failures, foreign offers without activation, actor reuse after failure, exact expiry/backward clocks and cancellation without BLE traffic. All 256 CompanionKit host tests pass. Source parsing also passes, but native Apple SDK typechecking, OS joining, UI/TransferRunner orchestration, cancellation after activation and device HTTP confirmation remain pending. The result contains ephemeral secrets and has no Codable conformance; do not store or cloud-sync it. Capability advertising remains disabled until the full app flow is wired and physical acceptance is performed.

### Apple temporary hotspot application

The iOS-only `WifiHotspotJoiner` uses NetworkExtension with the exact authenticated hotspot SSID/WPA passphrase and `joinOnce`. Its injectable MainActor operation retains the original negotiation deadline, rejects saved mode or concurrent joins, and returns an explicit cleanup lease. Timeout/cancellation/error removes the app's temporary configuration; a late completion after disposal removes it again, while duplicate callbacks during a valid lease do not interrupt that lease. `close` is idempotent and destruction schedules cleanup. Native `alreadyAssociated` is accepted only as configuration readiness, never as proof of a reader connection. Discovery and authenticated HTTP remain necessary.

Both iOS build configurations reference the new hotspot entitlement. Enable the matching capability in the signing profile as documented in `apple/App/README.md`. Six lifecycle tests pass against the injected driver; NetworkExtension execution, native SDK typechecking, signing, Mac/saved-network UI guidance and app orchestration remain unverified or unfinished. No firmware capability is enabled by these tests.

### Manual join guidance

`WifiManualJoinRequest` supports Mac and saved-network guidance without configuring or removing a user's system network. It extracts validated UTF-8 SSID and only an offered hotspot password into transient UI fields, preserves the negotiation's original deadline, and waits for one confirmation. Saved passwords are absent by protocol. Confirmation is not proof of Wi-Fi readiness or reader identity; the connector and encrypted transfer remain the authority. Timer expiry/cancellation closes the wait, overlapping waits are busy, repeated confirmation is ignored, and cancellation can still win after confirmation before the waiting task resumes. The shared Devices flow clears prompt fields on exit and supports both network modes.

Five host tests cover guidance/password omission, one-shot confirmation, early/task cancellation, timer expiry, unrenderable SSIDs/backward clocks, busy waits and the confirmation/cancellation race. Native app typechecking, physical system-control guidance, automatic reconnect and multi-job continuation remain pending. Firmware capability advertising remains disabled; explicit assistance still falls back over BLE on unsupported/unavailable preparation.

## Content removal ownership contract

`ContentRemovalRequest.json` defines a shared 115-byte request: `LRM` and version
1 (four bytes), transaction identity (16), authenticated installation owner (16),
storage generation (16), then the exact 63-byte installed content manifest.
All three identities and the content hash must be nonzero; content length must
be positive. EPUB formats 0/1, course format 1 with a nonzero family, font formats
1/2, and dictionary format 1 are accepted. Other kinds require zero logical
identity; firmware removal is rejected. Exact framing rejects trailing bytes.

The native and Apple codecs share this fixture. Native tests cover unaligned
output with boundary canaries, all truncated prefixes, invalid ownership,
unsupported kinds/formats and course-family semantics. Apple tests check the
same bytes, truncation and manifest contracts; all 361 CompanionKit tests pass.
The native record remains below 256 bytes and allocates no heap. The EPUB
endpoint is wired through authenticated BLE and encrypted Wi-Fi dispatch, with
complete-path removal plans, durable recovery, and reader-store refresh. The app
persists and resumes removal jobs and offers confirmed installed-content removal.
Firmware advertises the EPUB removal capability. Native Apple execution and
physical power-cut/resource acceptance remain unverified. Other content kinds
still require their dependency-aware removal participants; course removal must
retain isolated learner history. Device removal retains the companion library copy.

### Durable content removal phases

`CompanionContentRemovalJournal.h` defines a 168-byte LRJN v1 CRC record containing
that exact request, a nonzero SHA-256 of the verified participant plan, phase and
revision. Prepared, Quarantined, Committed and Retired use revisions 1 through 4.
Alternating `removal-a/b` slots require complete replacement, truncate/sync/close,
exact length and decoded readback before advancing live state. Recovery accepts
a valid remaining slot, rejects foreign owners/generations/manifests/plans and
nonadjacent valid phases, and leaves wholly corrupt evidence untouched. Ambiguous
writes invalidate live state until recovery. Repeating an already durable phase
performs no write.

`CompanionContentRemoval.h` verifies the plan before recording initial ownership.
Only durable Prepared permits quarantine. Verified quarantine precedes live-reference
removal, and durable Committed precedes backup retirement. Participant operations
must be idempotent and verify exact file ownership, including after-effect failures.
The coordinator rechecks its journal checkpoint between callbacks and mutations;
Retired remains available for repeated commands. Both owners retain their working
records outside the embedded task stack and borrow one 168-byte scratch span;
neither introduces heap allocation. There is no concrete content-cleanup participant or live
removal command yet, so compiled firmware is unchanged.

Nine host tests cover CRC/semantic/framing checks, all four phases and duplicate
commands, initial and phase persistence faults before/after effects, foreign
ownership/plans, torn slots, nonadjacent phases, participant failure/retry, invalid
plans, callback ownership changes and silent write corruption. Run
`ctest --test-dir /tmp/lila-companion-tests -R '^ContentRemovalJournal\.' --output-on-failure`.
These simulations do not establish SD power-loss behavior or target stack/heap limits.

### HAL removal journal storage and recovery durability

`HalContentRemovalJournalStorage` restricts access to the two exact removal slot
paths. It uses checked directory enumeration for presence, rejects directories,
reads exactly 168 bytes, and requires complete write/truncate/sync/close before
reporting success. Its member record handle and two lookup handles reuse the
checked opaque HAL wrappers. These wrappers allocate once on first use through
`makeUniqueNoThrow` in HalStorage, then remain available for subsequent writes;
this avoids allocating wrappers per slot operation. The lookup name buffer stays
in the session owner rather than the task stack. There is no per-record data-buffer
allocation; the journal borrows caller scratch. Actual ESP heap/stack measurements
remain necessary when this owner is connected to the live session.

Reading a valid record after an ambiguous sync failure does not prove it has left
this boot's buffers. `confirmRecovered()` writes, syncs and reads back an identical
replacement slot. The removal coordinator requires this confirmation for recovered
Prepared, Quarantined and Committed records before any participant mutation. Failed
confirmation leaves backups untouched and live state unavailable until recovery.
Retired duplicates only verify completed removal. Equal identical slot revisions
are permitted after confirmation; differing valid phases must remain adjacent.

Five HAL tests cover reconstructed storage, stable wrapper counts, false boolean
existence probes, exact path/size admission, directory collisions, enumeration
errors, write/truncate/sync/close failures, short reads and silent write corruption.
A tenth coordinator test forces an after-effect committed-marker failure and a
failed recovery confirmation, verifying that no participant runs until a synced
replacement succeeds. Use
`ctest --test-dir /tmp/lila-companion-tests -R '^(HalRemovalJournalTest|ContentRemovalJournal)\.' --output-on-failure`.
Content-specific quarantine plans, metadata/learner-state preservation, boot recovery,
transfer admission while removal is pending, authenticated dispatch and Apple job
persistence still need integration before enabling reader removal.

### Removal journal discovery for startup recovery

The removal journal now also accepts the current storage-generation identity without
an app-supplied request. It discovers the persisted request/participant-plan hash,
checks every valid slot against that generation, and requires agreement on owner,
transaction, full manifest and plan hash. Different requests in either slot stop
recovery, even when their phases are adjacent. A valid slot can survive another
truncated/corrupt slot; wholly corrupt evidence remains blocked and unchanged.
This mode performs no writes and does not itself authorize content mutations.
Recovery confirmation and verified content-specific participants remain required.

Known-request recovery retains a session-owned immutable seed before selecting
slots, so a borrowed current-record input cannot change its comparison contract.
That adds one bounded record and one 16-byte generation to the existing owner,
without introducing a heap allocation or task-local record. Actual session sizing
must include this owner when constructing the live removal pipeline.

Two new host tests cover startup discovery, both slot orders for mismatched owners,
transactions, plans and content, wrong/zero generations, short scratch, and corrupt
slot preservation. All 17 affected journal/HAL tests pass. `HalCompanionRecovery`
now invokes checked EPUB removal startup recovery before mutable reader stores
load. The native participant and complete-path plan are wired into that recovery;
the descriptor advertises EPUB removal, and physical acceptance remains pending.

### Durable single-file removal plans

`CompanionSingleFileRemovalPlan.h` defines LRSF v1: eight-byte magic/version/reserved
prefix, complete removal request (115 bytes), little-endian path length (two),
exact native UTF-8 path bytes, and CRC32. The path is bounded by the inventory's
511-byte limit, making the complete plan at most 640 bytes. Decoding borrows the
path from retained input; firmware C API calls must copy it into a session-owned
null-terminated path buffer before use. The journal's plan hash must cover this
complete encoded plan, and the request must agree with authoritative inventory
and verified installed bytes before publication or quarantine.

The codec accepts EPUB paths and matching font extensions beneath `/fonts` or
`/.fonts`. It excludes the private `.crosspoint` root, traversal, malformed UTF-8,
controls and invalid separators, and retains native Unicode spelling without
normalization. Courses and dictionary member sets require separate plans and
participants; they cannot use this single-file plan. The codec does not allocate
buffers or heap. Callers retain plan bytes separately from journal/hash scratch.

The removal request now accepts both legacy EPUB format 0 and native inventory
format 1; `HalInventoryBaseResolver` actually emits format 1. The Apple codec tests
both byte representations and rejects unknown format 2. All 362 Apple tests and
38 affected native record/plan/journal/HAL tests pass. Five plan tests cover exact
paths and casing, Unicode, CRC/semantic/framing rejection, private paths, wrong
content kinds/extensions, maximum lengths, short-output canaries and malformed
UTF-8. The plan is not yet persisted through HAL or connected to concrete content
cleanup or reader dispatch; compiled firmware behavior is unchanged.

### HAL publication and loading of removal plans

`HalSingleFileRemovalPlanStorage` validates the complete encoded plan and hashes
its exact bytes before constructing `removal-plan-<sha256>` beneath the private
companion directory. A digest-specific `.tmp` file is written, truncated, synced,
closed and compared in chunks before rename. Final target readback and sync precede
success. A failed rename can retry from the stage or an already published target.
Existing corrupt targets, directories, oversized stages and lookup/read failures
preserve their evidence. Only the bounded unpublished stage for the same digest
may be rewritten; this helper never modifies a content file.

Loading uses the journal's expected plan hash, validates length/SHA-256/CRC and
path grammar, then syncs and closes before assigning output fields. Existing valid
publication also requires sync before reuse. The pinned SdFat FAT and exFAT
`sync()` implementations flush the volume cache for an open file. This is a
same-boot buffered-I/O safeguard, not a physical SD power-loss acceptance result.
Plan-buffer reuse invalidates prior borrowed path views even if a later load fails.

The owner retains its SHA context, path buffers and HAL wrappers outside the task
stack. Record and lookup wrappers allocate once through checked HAL preparation
and are reused, avoiding per-chunk allocation. It borrows a comparison span (128
bytes in tests) and the caller retains up to 640 plan bytes. Together with the
168-byte removal-journal scratch these buffers total 936 bytes, below the current
940-byte transfer slice; live ownership/layout still requires integration and
canary/target heap/stack acceptance. There is no second full-plan readback buffer.

Eight HAL tests cover independent SHA calculation, reconstruction, wrapper reuse,
false existence probes, write/truncate/sync/close and silent corruption, rename
failures before/after effects, admission/overlap rejection, short reads, target and
stage collisions, directory errors, recovery sync, and a valid-CRC payload that
fails the journal's expected hash. Run
`ctest --test-dir /tmp/lila-companion-tests -R '^HalRemovalPlanTest\.' --output-on-failure`.
No compiled firmware source instantiates this storage yet. Plan cleanup after
retirement, concrete content quarantine, boot recovery and command/app integration
remain required before enabling removal.

### Checked lookup of native removal paths

`HalInventoryPathLookup` enumerates the exact parent, including `/`, through HAL
handles. Presence requires a regular matched entry and successful closes; absence
requires checked end-of-directory and successful closes. Missing parents, directory
collisions, invalid/oversized names, short-alias errors and enumeration/I/O failures
remain Error. The helper does not use the boolean existence API. Progress is checked
before each entry and after final close, so cancellation cannot publish presence or
absence after a boundary callback rejects the operation. Scans yield every 32 entries.

`HalFileName` wraps the installed SdFat UTF-8 decoder and uppercase mapping without
filesystem calls. Both FAT and exFAT compare UTF-16 units using that mapping; this
wrapper preserves that behavior, including unchanged surrogate units. It does not
normalize spelling or perform general Unicode case folding. Long names and checked
FAT short aliases are considered; exFAT supplies an empty alias. Malformed UTF-8 in
any examined name blocks lookup rather than being treated as an unrelated entry.

The parent/name buffers are two session-owned 512-byte arrays, with a 13-byte alias
buffer and two retained HAL handles. This bounded storage avoids oversized task-local
buffers and per-entry wrapper allocations. Existing HAL implementations perform
checked initial wrapper allocation and reuse them; target heap/stack acceptance
must include this owner. No new Unicode table or per-name heap buffer is allocated.

`test/companion/sdfat_path_lookup_check.py` compiles the helper against the real
installed `FsUtf.cpp` and `upcase.cpp` with strict host warnings. Assertions cover
Unicode/ASCII case, unchanged native normalization, malformed tails, non-BMP names,
root and nested files, aliases, false existence probes, wrapper reuse, invalid paths,
missing parents, directory collisions, enumeration/close/alias errors, cancellation
at presence/absence readback, and yielding across 100 entries. Checks passed against
both default and sticky dependency sources. SD handles in these checks are simulated;
physical FAT/exFAT acceptance remains pending. The build CI matrix now invokes this
check after each firmware build. No compiled firmware source constructs this lookup
until the concrete removal participant is connected.


### Removal command admission and reply

Command 15 (`RemoveContent` / `removeContent`) carries the exact 115-byte removal
request. Capability bit 8 (`0x00000100`) is reserved for EPUB removal; it does not
imply course/font/dictionary removal or depend on the transfer capability bits.
The native descriptor advertises this bit. Font removal uses the independent bit
13 (`0x00002000`) with the same command/request/reply and completed-receipt
contract. It permits vector format 1 and bitmap format 4, collects every installed
path with the exact manifest, verifies all copies before quarantine, publishes
checked fallback settings, and retires backups only after commit. The source
advertises bit 13; all five target builds/image checks pass. Physical acceptance
remains pending.
Neither bit authorizes course or dictionary removal. Older readers reject the
new command; the app must check the content kind's explicit capability before
sending it.

`CompanionContentRemovalHandler` requires installation authorization, exact
request framing, matching authenticated owner, and the measured card generation
before invoking its backend. It looks up the immutable completed request first.
Only checked receipt absence (`NotFound`) permits backend plan admission/removal;
completed requests return success without starting participant work, and corrupt,
conflicting or unreadable receipts remain errors. The backend must implement
serialized installed-manifest/plan validation, busy/dependency checks, the durable
coordinator, and store/inventory refresh before releasing the session.

Replies contain one result byte followed by the request's 16-byte transaction:

| Value | Result |
| --- | --- |
| 0 | Ok |
| 1 | Invalid |
| 2 | Unauthorized |
| 3 | WrongStorage |
| 4 | Busy |
| 5 | NotFound |
| 6 | Unsupported |
| 7 | Conflict |
| 8 | Corrupt |
| 9 | IoError |

An installation-unauthorized request is rejected before parsing and returns a
zero transaction. A malformed request also returns zero rather than publishing
partially decoded ownership. A decoded request echoes its transaction on all
outcomes. A reply buffer shorter than 17 bytes starts no backend work.
`ContentRemovalReply` rejects unknown statuses, wrong framing and a transaction
mismatch; it reports a zero-transaction authorization denial as unauthorized and
never accepts zero-transaction success. Request IDs still correlate transport
responses; durable retry identity is the request transaction.

`ContentRemovalReply.json` is shared by the C++ handler fixture check and Apple
codec. Twenty-nine focused frame/record/handler tests and all 366 Swift tests
pass, including authorization-before-work, malformed bodies, wrong owner/card,
completed duplicate suppression, receipt-error preservation and reply binding.
The handler passes the optimized host 256-byte frame check with exceptions/RTTI
disabled and adds no heap allocations. Native backend/dispatch, Apple durable
removal jobs/UI, other content participants, and physical heap/stack acceptance
remain pending. Accepting command 15 in the frame codec is not a live endpoint.

Final C3 `default` and S3 `sticky` builds pass after the frame command-range change. The release validator accepts both images with unchanged lengths/headroom (6,276,016/277,584 bytes for C3; 5,634,288/919,312 for S3). Their SHA-256 values are `52239fa66217b0fee134897de0b63824143e7a62b16b992a6650688b0e5e75bb` and `d61dbba5b5b07a5163b61e61ac2d8d56d70c0c5db0210878ecaf7461b6311051`, respectively. These builds cover the expanded frame codec; the new portable removal handler is still exercised only by host tests until native backend/dispatch integration.

### Multi-path removal plan format

`CompanionMultiPathRemovalPlan.h` defines LRMP v1 for all installed copies of one
requested EPUB/font content identity. This is a private SD recovery artifact,
not an Apple control payload. Its header is 155 bytes:

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 8 | `LRMP`, version 1, three zero reserved bytes |
| 8 | 115 | Exact removal request |
| 123 | 8 | Nonzero original inventory revision, little-endian |
| 131 | 8 | Nonzero path-record count, little-endian |
| 139 | 8 | Total bytes of path records, little-endian |
| 147 | 4 | CRC-32 of concatenated record payloads, excluding their CRC footers |
| 151 | 4 | CRC-32 of the preceding 151 header bytes |

Each record contains a two-byte little-endian UTF-8 path length, that many path
bytes, and a four-byte CRC-32 of length-plus-path. Paths borrow the established
single-file removal grammar: absolute, bounded to 511 bytes, valid UTF-8,
non-private, and matching the content kind/format/root rules. Maximum record size
is 517 bytes. Header count/length bounds reject empty plans, impossible record
counts and total-length overflow before iteration. The complete file must contain
exactly the declared records; trailing bytes are rejected. Records retain their
collected order. The eventual sink/backend must resolve physical aliases and
prevent duplicate physical paths before arming participants.

`MultiPathRemovalPlanReader` validates the entire plan against the full expected
request before exposing a ready header, then streams copied, terminated paths
through borrowed scratch of at least 517 bytes. End rechecks payload CRC, reported
file length and the complete header. Errors invalidate authority and leave path
outputs unchanged. Output/scratch overlap is rejected before IO. Header decoding
uses separate request-decoder frames to stay within the stack budget. Opening
also compares a spare decoded header before replacing the current header, so an
expected request borrowed from the current reader cannot be overwritten during
its ownership comparison.

These CRCs detect corruption/change; they do not authorize deletion. The native
owner must verify the SHA-256 of the complete file against the journal plan hash
and exclude all plan writers before participant mutations. The reader itself
allocates no heap and owns only bounded parser/header state; scratch and copied
paths belong to the enclosing off-stack owner. It never allocates an array sized
by the number of paths.

`MultiPathRemovalPlan.json` records two renamed copies and the full-file SHA-256
`dc005ea34d8791784814f4251a51d975fd6229230a979715bfe6a6798cec5e27`.
Nine format/reader tests pass: exact unaligned framing, truncation/corruption and
trailing bytes, every ownership/content field, invalid counts/lengths/kinds,
path grammar/maximum size, every open read failure, capacity/overlap, header
changes, borrowed-request aliasing, and rewritten valid record CRCs. Optimized
host and actual C3/S3 cross-compilation checks pass with exceptions/RTTI disabled
and a 256-byte function-frame limit. No production caller includes this new
header yet; these checks are not complete firmware endpoint or physical SD
acceptance. Reproduce the host tests with CMake target
`CompanionMultiPathRemovalPlanTest` and CTest prefix `MultiPathRemovalPlanTest`.
The durable HAL sink, full-file digest verification/publication, cohort participant
composition, boot-format routing, and native/Apple command flows remain pending.

### Ownership of a mutable removal-plan stage

LRSC v1 is an independent immutable claim for a streamed LRMP plan stage. Its
135 bytes contain an eight-byte `LRSC`/version-1/zero-reserved prefix, the full
115-byte removal request, the original inventory revision as little-endian u64,
and a CRC-32 of the preceding 131 bytes. The revision must be nonzero and the
request must satisfy the EPUB/font plan contract. This claim is separate from the
mutable LRMP header, so interrupted final header rewriting does not erase the
stage's ownership evidence.

`HalRemovalStageClaimStorage` hashes all 135 encoded claim bytes and derives
`/.crosspoint/companion/removal-multi-<lowercase-sha256>.owner`, its `.owner.tmp`
publication stage, and the corresponding `.stage` plan-file address. It exposes
borrowed marker/plan paths only after complete readback, sync and checked close
of the matching immutable claim. The caller must authenticate/admit the request,
check measured card identity, serialize all writers, and retain the store and
scratch while using its path pointers. This helper is not a global transaction
admission registry and does not authorize deletion of arbitrary existing stage
bytes on its own.

Publication verifies an existing marker before acknowledging a retry. A matching
verified temporary marker is renamed without rewriting its bytes. Bounded
incomplete/corrupt unpublished marker bytes under this exact claim digest can be
rebuilt; valid foreign temporary claims, published corrupt/foreign claims, and
oversized/directory collisions remain untouched. Publication performs full write,
truncate, sync, checked close, temporary readback, absent-target check, rename,
and published readback. Read/sync/close/lookup errors never expose plan-stage
authority. The store never writes/removes the actual `.stage` plan, books,
metadata, history or published claims.

`RemovalStageClaim.json` supplies an independent fixture with SHA-256
`33afdb66c9d29ff7af187b0ab6fb957cd893983895d4c15ef57716e44999bc47`.
Ten focused tests pass, covering exact/unaligned framing, truncation/corruption,
invalid claims/scratch, matching retries, write/sync/close/truncate failures,
rename after-effect failure, foreign/published-corrupt preservation, directory
and oversized collisions, bounded incomplete marker recovery, retained existing
plan-stage bytes, changed request/revision namespaces, and publication without
rewriting a sealed temporary marker. The final persist/load entry points compile
with real C3/S3 HAL and crypto headers using their PlatformIO compilation
contexts, without LTO, and with `-Wframe-larger-than=256` promoted to an error.
This is an API/function-frame check, not a linked live endpoint or physical
power-cut/heap/stack acceptance result. The helper owns bounded records, hash
context and path/handle state outside the stack; it borrows 135 bytes of scratch
and adds no direct heap allocations or per-record containers. HAL handles are
prepared and reused through existing checked APIs.

Reproduce host checks with CMake target `HalRemovalStageClaimStorageTest` and
CTest prefix `RemovalStageClaimTest`. The durable streamed plan writer must still
verify any existing plan/header against this claim, preserve foreign sealed
plans, verify intended/stored bytes and the full-file SHA-256, publish a sealed
LRMP artifact, and bind it into the parent removal journal. Participant composition, startup routing, in-memory refresh and native/Apple
removal dispatch are wired, and the reader advertises EPUB removal. Physical
alias and power-cut acceptance remain unverified.


## Reader content export

Connect & Sync prepares a retained content-reader owner before enabling BLE and
advertises content reads (bit 10), metadata (bit 11), and export handoff (bit 12)
only when that owner is available. All export commands use the bonded peer's
current installation authorization. Firmware, recovery, competing mutation,
inventory-revision, card-generation, and heap checks precede source access.

### Wire records

| Command | Request | Reply |
| --- | --- | --- |
| 16, ReadContent | `LCR`, version 1, generation 16, full manifest 63, offset u64 LE, maximum u16 LE; 93 bytes | `LCS`, version 1, result, generation 16, SHA-256 32, offset u64 LE, count u16 LE, data; header 63 bytes |
| 17, ContentMetadata | `LCM`, version 1, generation 16, full manifest 63; 83 bytes | `LCN`, version 1, result, generation 16, full manifest 63, UTF-8 basename length u8, basename; header 85 bytes |
| 18, PrepareContentHandoff | `LCW`, version 1, transaction 16, generation 16, full manifest 63, durable offset u64 LE; 107 bytes | `LCT`, version 1, result, transaction 16, generation 16, SHA-256 32, accepted offset u64 LE; 77 bytes |

Results are Ok=0, Invalid=1, Unauthorized=2, WrongStorage=3, Busy=4, NotFound=5,
Corrupt=6, and IoError=7. Malformed requests receive the normal control Error
response. Read success carries exactly the requested/end-of-file count, up to
961 bytes; failures carry no content bytes. Metadata success carries a nonempty
UTF-8 basename of at most 255 bytes, with no path separators, NUL, ASCII controls,
or DEL. Its extension must match the content kind; bitmap fonts require `.cpfont`,
while vector fonts accept `.ttf`, `.otf`, and `.ttc`. Metadata errors carry no name.
Every decoder verifies exact lengths and request bindings before accepting data.
Export manifests exclude firmware and require the supported format/logical-ID
contract for EPUBs, courses, fonts, or retained dictionary ZIP archives.

Shared request/reply JSON fixtures for ReaderContentRead, ReaderContentMetadata,
and ReaderContentHandoff verify Swift/C++ wire agreement.

### Source and lease lifetime

The reader validates the inventory/index pair and resolves the exact manifest's
path. Source attachment verifies SHA-256 and length; bounded reads check source
metadata before and after I/O. Same-size/same-timestamp rewrites are not excluded
by metadata checks, so final companion SHA verification remains mandatory.

Export admission requires more than 1 MiB remaining. A verified one-byte source
read precedes retaining the transaction, installation, manifest, resume offset,
and inventory revision. `WifiHandoffLease.prepareExport` requires that binding
and reuses authenticated preparation, activation, expiry, and consume checks.
Activation closes the source handle while preserving the export binding; the
first Wi-Fi read reopens and verifies the source. Its 109-byte body prepends the
lease transaction to the 93-byte read request. The encrypted dispatcher checks
transaction, installation, card, full manifest, revision, and the lower offset
bound. Responses retain the content-read format.

Mutation cleanup, normal session reset, stop, disconnect, and exit clear export
bindings and close readers. Close failures block subsequent mutations. The owner
retains its terminated path and checked reusable HAL handle outside the task
stack. Binding state is included in that checked allocation; all variable-size
source I/O borrows the existing 8 KiB activity workspace. No additional transfer
buffer is allocated.

### Durable companion import

Schema 40 stores reader/card/installation/manifest-bound jobs, durable offsets,
and immutable reader-provided filenames. Enqueue requires complete exact
inventory evidence and deduplicates active intent. Existing removal or deletion
state blocks import; pending imports block reader removal. First filename binding
may recover a migrated schema-39 queued, downloading, paused, or verifying job
without changing its offset. Subsequent differing names are rejected; aborted
jobs cannot bind, and completed jobs cannot create a new binding.

ReaderImportStorage uses a separate durable staging root with exact immutable
ownership records, serialized file operations, and file synchronization before
SQLite checkpoints. Recovery trims an unacknowledged tail and refuses a missing
or short acknowledged prefix. ReaderImportRunner checks fresh job state after
every read, pauses interrupted work, and does not write an in-flight chunk after
deletion. Handoff preparation checks staging and requires unchanged job state
across admission, including after reopening a retained downloading job.

ContentImporter verifies expected bytes and content kind before SQLite atomically
publishes metadata, reader selection, course identity where applicable, and job
completion. Publication checks the accepted filename again. Completed retries
verify the retained vault object and do not reselect content. Cancellation commits
aborted state before discarding owned staging; completed imports cannot be
cancelled. Terminal cleanup scans bounded pages, reports per-job failures, and
preserves pending staging, foreign bindings, and immutable vault objects.

The native Installed content section offers Import/Resume and Pause for reader-only
content and shows pending import progress and Cancel. Globally removed content is
excluded from automatic re-import. With Wi-Fi assistance enabled, more than 1 MiB
remaining and advertised support trigger admission and saved-network/hotspot
negotiation. Refused negotiation can fall back while BLE is still ready. After
radio switching, failure closes the encrypted transport and requires a fresh
connection and inventory refresh before resume. Hotspot credentials and leases
remain ephemeral. Library refresh and import completion run terminal staging
maintenance when reader operations are idle.

### Verification limits

All 502 portable Swift tests and 1,703 host CTest entries pass. Tests cover wire
fixtures, durable migration/restart, in-flight deletion, checksum rejection,
retained-owner closure/reopening, encrypted import, lost Wi-Fi reply followed by
fresh-session BLE resume, immutable filename publication, cancellation, and
bounded terminal cleanup, deselection during import, and font removal
format/path agreement. All five firmware profiles build with the corrected
font-removal codecs. Their saved images pass board/chip, checksum/SHA trailer,
and OTA-size validation. See
hardware-verification.md for image identities and compiler frame evidence.

Native source syntax and localization catalog checks pass on Linux. Native
Apple SDK compilation, UI/accessibility interaction, post-switch
reconnection on physical devices, physical BLE/Wi-Fi recovery, power-cut behavior, and measured
runtime heap/stack acceptance remain unverified or incomplete. These checks do
not establish completion of COMPANION_PLAN.md.

Dictionary removal uses independent capability bit 14 (`0x00004000`) with command
15 and the existing complete manifest/request binding. EPUB/font removal and
dictionary-transfer capabilities do not authorize this operation. Firmware dispatch
verifies all owned members across matching dictionary folders, journals quarantine,
clears a matching selection, retires only proved members/bindings, and preserves
unowned siblings and shared archives. Startup recovery resumes the retained plan.
Apple queue admission supports dictionary manifests and uses this capability in
its shared per-reader selection/removal controls and durable runner. Lost replies
retry the exact persisted request after SQLite reopen. The full Swift host suite
passes 505 tests; final firmware target/image checks and hardware acceptance are
still pending for the capability-enabled implementation.

## Bound-course context

Command 19 requires installation authentication on BLE or the current encrypted
Wi-Fi lease. Its BLE request is exactly 20 bytes: `LCQ`, binary version 1, and
nonzero card generation (16 bytes). Wi-Fi prefixes the same body with the lease's
16-byte transaction; both transaction and generation must match before dispatch.

The reply begins `LCX`, binary version 1, result u8, source u8 and generation 16.
Result values are 0 success, 1 missing, 2 wrong storage, 3 busy, 4 unsupported,
5 IO error, 6 unauthorized and 7 corrupt. Failures have source 0 and exactly
22 bytes, with no manifest. Wrong storage reports the current generation; other
replies echo the requested generation. Unauthorized requests echo the supplied
generation and expose no stored course metadata.

A successful reply has source 1 (live) or 2 (verified removed), followed by the
63-byte `ContentManifest`, for 85 bytes total. Only a positive-length course,
format 1 and nonzero identity/hash are accepted. Live replies verify the retained
binding and installed SHA. Removed replies additionally require the exact
completed receipt, sealed plan, cached SHA, state isolation and released removal
journal. Unfinished publications/journals, an unbound legacy active pack or
invalid evidence do not yield a course manifest. Querying does not mutate pack,
binding or learner state, and cached packs remain absent from installed inventory.

The query uses one checked nothrow fixed owner off stack and borrows the transfer
IO bank, disjoint from request/response storage. Other removal owners are released
before admission. Hashing and directory scans reuse retained handles/buffers and
preserve the existing 50 KiB internal-heap reserve. Source context is captured
while state writers are excluded; it is not authorization to switch a course.
The reader must independently verify the later consent and source before commit.

Shared C++/Swift fixtures define the body format. The Swift helper validates
command, response flag and request ID, and the encrypted handoff helper binds the
request to its transaction and card generation. Apple discovery/use is capability
gated and native removed-source authorization is connected; no capability is
advertised for this query yet.

Course-context discovery reserves capability bit 15 (`0x00008000`), alongside
course-transfer support. The Apple inventory collector sends command 19 only
when both bits are present, after complete inventory paging. Successful context
must agree with that card generation and snapshot: a live context matches the
single inventoried course exactly; a removed context requires no inventoried
course. A missing reply requires no inventoried course. Other statuses withhold
the snapshot, and wrong storage requires reopening the reader. Readers without
this capability receive only the existing inventory requests.

The context stays separate from installed content: it is not an exportable pack
or a removal candidate. Apple course admission compares against its bound
manifest; a different identity requires the same immutable explicit switch
confirmation used for live courses. Confirmation review includes the context,
and a changed hash cannot retarget a retained job. The native reader does not
yet advertise bit 15; complete course-flow verification remains required before
enabling discovery and course removal.

Removed-course switch source authorization uses the same read-only provider as
command 19. It checks the exact retained binding against consent, the storage
generation, completed removal receipt, sealed plan, cached pack SHA and learner
state isolation. Both initial consent and precommit validation perform these
checks independently; command-19 context alone never authorizes publication.
The portable storage default verifies only the live pack; the native HAL adds
removed-source verification while state writers remain excluded.

Aborting a switch verifies the old source before retiring consent and retains
its binding, removed proof, cache, receipt and learner files. Committing a switch
publishes the replacement binding recoverably, then retires the old removed
proof before retiring switch consent. Proof retirement reloads the exact durable
consent and matches both old identity/hash and the committed replacement's
identity/hash, generation and transaction. It verifies the replacement bytes,
completed old receipt/plan/cache and both course state directories. Recovery
accepts a checked deletion that applied before reporting failure; neither cached
packs nor learner files are deleted during this retirement. Same-course proof
retirement remains separate and cannot authorize a different identity.

Real-pack HAL tests cover removed-source consent, foreign generation/hash,
abort preservation, every publication rename boundary, metadata/journal sync
failures and before/after proof deletion. Applied consent deletion failure also
retries without touching preserved content. The installed-SdFat suite checks
explicit switched baseline verification and rejects a mismatched previous hash.
Course-context/removal capabilities remain unadvertised while the complete
course flow and hardware acceptance are being checked.

## Frozen learner review and original-pack archive upload

Command 20, `CourseBaselineReview`, is authenticated BLE control. Tinta readers
advertise review capability bit 16. Its 74-byte `TCBQ` version-1 request carries
native storage generation (offset 6), selected course (22), frozen review SHA-256
(38), little-endian offset u16 (70) and limit u16 (72). A zero digest requests
capture and is valid only at offset zero. Later pages select the same nonzero
frozen digest. Limits are 1–976 bytes; reviews are bounded to 4,416 bytes.

Success returns a `TCBP` version-1 page: total u16 at 6, offset u16 at 8, count u16
at 10, whole-review digest at 12, bytes at 44, then CRC32. Page CRC checks do not
replace full review SHA-256 and reader/card/course checks after assembly. Error
frames have one byte: 1 invalid request, 2 unauthorized, 3 busy, 4 wrong card,
5 unavailable, 6 unsupported. Wi-Fi review control is not implemented.

Command 21, `BeginCourseBaseline`, accepts the exact 155-byte `TCBI` original-pack
confirmation body defined by `CourseBaselineImportRequest-v1.fixture`. It is BLE
control bound to the authenticated Apple installation. The native handler checks
journal readiness, preserves/rechecks the reviewed files, and saves durable consent
before beginning upload to `/tinta/course-baseline.pack`. This is an archive
transaction; it does not replace the active course or apply merged learner state.
Success returns the ordinary result byte plus `TransferState`; failures use the
ordinary `TransferResult` byte. Shared framed bytes are in
`CourseBaselineBeginFrame-v1.fixture` (request ID `0x12345678`).

Chunks, status, abort and commit retain ordinary transaction IDs and durable
transfer offsets. After BLE approval, large uploads can use the existing encrypted
Wi-Fi handoff. Commit routes the archive destination through native baseline
validation/publication using the full workspace lease on either transport.
Original-pack consent is not a substitute for complete staged pack validation,
reviewed UID/history checks, or startup recovery after interrupted publication.

The Wi-Fi lease can be obtained only during authenticated dispatch with the exact
handoff session identity. Callers copy request fields out first. Permission ends
immediately on teardown/expiry, but leased bytes remain until release. Request
buffers are unavailable during processing; dispatch returning with an unreleased
loan ends the session before encoding a reply. Session owners must outlive their
serialized dispatch. This reuses the existing 8 KiB workspace without allocating
another buffer.

The Apple transfer runner has a dedicated baseline path requiring import
capability bit 17, saved original-pack consent and a retained declaration. It
queries status before initial approval or resume, and prepared Wi-Fi handoffs
cannot issue new approval. Ordinary course transfer entry points refuse baseline
jobs. The app review screen requires explicit original-pack confirmation before
queueing; prepared content work routes that saved intent to the baseline runner,
including Wi-Fi preparation, BLE fallback and commit recovery. A stale baseline
job cannot generate an ordinary installation on a replacement card.
Archive consent is separate from active-course selection: queueing and completing
an archive never changes those selections or requests installation of the
original pack. Explicit cancellation and global deletion abort an upload before
commit; an already committing job retains its declaration and finishes recovery.

Frozen isolated reviews retain `TCBV` version 1. An unbound global `/tinta`
review uses version 2 with scope byte 6 set to 1; byte 7 remains zero. Its course
identity is proposed, not an existing assignment. The same sorted file roster,
journal records, identity binding, CRC and whole-review hash apply. All four
isolation records must be absent, with zero length/hash. Reader capture requires
binding and migration records to be absent before and after capture and refuses
unexpected subdirectories or unfinished learner files. Pack and diagnostic files
in the root are recorded as evidence; recording does not authorize merging them.
The existing paged review command carries either format without changing `TCBQ`
or `TCBP`. Native import consumers require the isolated format; Swift consent
matching and queueing also refuse unbound reviews.

A fresh unbound capture preserves verified file copies under the review hash
before returning its first page. Copies use owned staging files and can resume
after interruption; sources remain unchanged. Diagnostic usage files are
preserved as evidence and are not merged. Explicit unbound backup verification
is separate from the isolated backup loan used by archive import. Retrieving a
previously sealed roster by hash alone does not attest backup completeness or
authorize migration; migration must verify the saved copies and current cohort.

Firmware advertisement of bit 17 remains pending. Initial unbound `/tinta`
migration still needs consent-driven backups and recoverable isolation before
archive installation can consume that review. Discovery must not infer archive
installation support from review capability bit 16 alone.

The separate, currently unwired migration-consent record is `TCUM`, 155 bytes:
magic at 0, version 1 at 4, explicit approval 1 at 5, reserved zeros at 6–7,
card generation at 8, Apple installation at 24, transaction at 40, original-pack
manifest at 56, frozen review SHA-256 at 119, and CRC32 at 151. Its matcher
requires an unbound review and binds the authenticated reader, generation,
installation, transaction and proposed course identity. It cannot replace
`TCBI` archive consent. The caller must hash immutable review bytes and verify
native identities; pack validation, backup verification, durable authorization
and recoverable namespace migration remain separate obligations. No command or
capability currently exposes this record as an executable migration request.
