# Nghttp2 Session Class Design

## Summary

This document proposes a low-level `Nghttp2\Session` API intended for
PHP userland event loops.

The current extension already wraps `nghttp2_session`, but the state
machine is embedded inside the blocking `Nghttp2\Client` and
`Nghttp2\Server` implementations. A dedicated `Session` class would
expose the HTTP/2 session state machine directly while keeping socket,
TLS, and event loop integration in PHP userland.

## Current State

### Client

`Nghttp2\Client` currently owns all of the following responsibilities:

- TCP connect
- TLS context creation and handshake
- ALPN verification
- `nghttp2_session` lifecycle
- request submission
- blocking send and receive loop
- response buffering for one in-flight stream

This is simple, but it hardcodes a synchronous I/O model and prevents
reuse from userland event loops.

### Server

`Nghttp2\Server` currently owns:

- listen socket creation
- TLS server setup
- accept loop
- per-connection `nghttp2_session` setup
- blocking `SSL_read()` driven receive loop
- fixed response submission on request completion

This is useful for a demo server, but it does not expose the session
state machine needed for per-connection event loop integration.

### Design Tension

The README already describes the extension as low-level and explicitly
mentions planned frame-level access. The current public API is still
mostly blocking and transport-bound.

## Goals

- Expose a stateful HTTP/2 session primitive to PHP userland
- Allow integration with existing event loops
- Keep socket and TLS handling outside the `Session` class
- Preserve explicit state transitions
- Support both client and server session modes
- Make it possible to refactor the current `Client` and `Server`
  implementations into thin wrappers over `Session`

## Non-Goals

- Replacing the blocking `Client` and `Server` APIs immediately
- Hiding HTTP/2 details behind a high-level PSR-style abstraction
- Embedding a reactor or event loop in the extension
- Requiring PHP callbacks from nghttp2 callbacks in the first version
- Introducing a separate `Stream` class in the first version

## Proposed API

```php
namespace Nghttp2;

final class Session
{
    public static function client(array $options = []): self;

    public static function server(array $options = []): self;

    public function receive(string $bytes): int;

    public function popOutbound(): string;

    /**
     * @return list<array<string, mixed>>
     */
    public function popEvents(): array;

    public function wantsRead(): bool;

    public function wantsWrite(): bool;

    public function submitRequest(array $headers, bool $endStream = true): int;

    public function submitResponse(
        int $streamId,
        array $headers,
        bool $endStream = false
    ): void;

    public function submitHeaders(
        int $streamId,
        array $headers,
        bool $endStream = false
    ): void;

    public function submitData(
        int $streamId,
        string $data,
        bool $endStream = false
    ): void;

    public function submitSettings(array $settings): void;

    public function submitPing(string $opaqueData = ""): void;

    public function submitRstStream(int $streamId, int $errorCode): void;

    public function submitGoaway(
        int $errorCode = 0,
        string $debugData = ""
    ): void;

    public function close(): void;
}
```

## Core Model

`Session` should be transport-agnostic.

It should not own:

- socket file descriptors
- TLS state
- DNS resolution
- connect or accept loops

It should own:

- `nghttp2_session *`
- outbound frame buffer
- inbound event queue
- per-stream send buffers needed by `submitData()`
- mode-specific session callbacks
- session closed state

## Recommended Userland Flow

### Client-side event loop integration

```php
$session = Nghttp2\Session::client();

$streamId = $session->submitRequest([
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':scheme', 'value' => 'https'],
    ['name' => ':authority', 'value' => 'example.com'],
    ['name' => ':path', 'value' => '/'],
], true);

$socket->write($session->popOutbound());

while (true) {
    $session->receive($socket->read());

    foreach ($session->popEvents() as $event) {
        // handle headers, data, stream_close, goaway, ...
    }

    $outbound = $session->popOutbound();
    if ($outbound !== '') {
        $socket->write($outbound);
    }
}
```

### Server-side event loop integration

```php
$session = Nghttp2\Session::server();

$session->receive($tlsBytesFromPeer);

foreach ($session->popEvents() as $event) {
    if ($event['type'] === 'headers' && $event['endStream']) {
        $session->submitResponse(
            $event['streamId'],
            [
                ['name' => ':status', 'value' => '200'],
                ['name' => 'content-type', 'value' => 'text/plain'],
            ],
            false
        );

        $session->submitData($event['streamId'], "hello\n", true);
    }
}

$wireBytes = $session->popOutbound();
```

## Event Model

The first version should avoid invoking PHP callbacks directly from
nghttp2 callbacks. Instead, C callbacks should append normalized events
to an internal queue, and PHP userland should pull them via
`popEvents()`.

This keeps the core deterministic and easier to test.

### Suggested event shapes

Headers:

```php
[
    'type' => 'headers',
    'streamId' => 1,
    'headers' => [
        ['name' => ':status', 'value' => '200'],
        ['name' => 'content-type', 'value' => 'text/plain'],
    ],
    'category' => 'response',
    'endStream' => false,
]
```

Data:

```php
[
    'type' => 'data',
    'streamId' => 1,
    'data' => "chunk",
    'endStream' => false,
]
```

Stream close:

```php
[
    'type' => 'stream_close',
    'streamId' => 1,
    'errorCode' => 0,
]
```

Goaway:

```php
[
    'type' => 'goaway',
    'lastStreamId' => 3,
    'errorCode' => 0,
    'debugData' => '',
]
```

Additional event types can be added later for:

- `settings_ack`
- `rst_stream`
- `ping`
- `window_update`
- priority-related events

## Why Queue-Based Events First

Using a pull-based event queue in the first version has several
advantages:

- avoids re-entrant PHP execution from C callbacks
- keeps the API explicit and predictable
- works naturally with any PHP event loop
- simplifies tests by decoupling callbacks from transport
- allows future callback-based sugar to be built in userland

## Internal C Design

## Session object shape

The new internal object should look roughly like this:

```c
typedef struct _nghttp2_session_object {
    nghttp2_session *session;
    zend_bool is_server;
    zend_bool closed;

    smart_str outbound;
    zval events;

    HashTable *stream_send_buffers;

    zend_object std;
} nghttp2_session_object;
```

The final layout may differ, but the key point is that transport state
must not be embedded in the object.

### Send callback

The current client and server send callbacks write directly into TLS.

For `Session`, the send callback should append frame bytes into
`smart_str outbound` and return the number of bytes accepted.

That turns `nghttp2_session_send()` into a frame serialization step
instead of a network write.

### Receive path

`receive(string $bytes)` should:

1. validate that the session is still open
2. feed bytes into `nghttp2_session_mem_recv()`
3. run any resulting nghttp2 callbacks
4. serialize any pending outbound frames into the outbound buffer
5. return the number of bytes consumed

### Data submission

`submitData()` will need per-stream body state because nghttp2 reads
body bytes through a callback-driven data provider.

The first implementation can keep this simple:

- each `submitData()` call copies the provided string into stream-owned
  storage
- the data read callback drains from that storage
- once the stream is closed, stream-owned data is released

This is acceptable for an initial low-level API. Zero-copy optimization
can come later if needed.

## Header Format

The extension should continue to use the existing normalized header
format:

```php
[
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':path', 'value' => '/'],
]
```

This format already matches the current HPACK API and is suitable for a
low-level session interface.

## Error Handling

Add a dedicated `Nghttp2\Exception\SessionException`.

Reasons:

- `Session` is a distinct abstraction from `Client` and `Server`
- transport errors will usually happen outside of `Session`
- nghttp2 protocol and state errors should remain explicit

Exception behavior should mirror the current exception classes:

- exception code carries the nghttp2 code where available
- `getNghttp2ErrorCode()` remains available

## Compatibility Strategy

The current `Client` and `Server` classes should remain public and
unchanged during the first phase.

The intended migration path is:

1. introduce `Session`
2. stabilize and test it
3. internally refactor `Client` and `Server` to use it
4. keep their current blocking APIs as convenience wrappers

This avoids a breaking change and lets the extension support both
educational blocking usage and event-loop-driven low-level usage.

## Implementation Plan

### Phase 1: Introduce the class skeleton

- add `ext/session.c`
- register `Nghttp2\Session`
- register `Nghttp2\Exception\SessionException`
- update module init and header declarations

At this phase, only minimal construction and lifecycle management are
needed.

### Phase 2: Core session loop

- implement client and server session creation
- implement generic send callback backed by an outbound buffer
- implement `receive()`
- implement `popOutbound()`
- implement `wantsRead()` and `wantsWrite()`

This is the minimum useful event-loop integration surface.

### Phase 3: Event queue

- collect header events
- collect data events
- collect stream close events
- collect goaway events
- expose them through `popEvents()`

This phase makes userland stream state management possible.

### Phase 4: Submission APIs

- implement `submitRequest()`
- implement `submitHeaders()`
- implement `submitResponse()`
- implement `submitData()`
- implement `submitSettings()`
- implement `submitPing()`
- implement `submitRstStream()`
- implement `submitGoaway()`

This phase completes the low-level usable surface.

### Phase 5: Tests

Testing should use in-memory client/server session pairs instead of real
network I/O wherever possible.

Recommended tests:

- class loading and method existence
- client preface and settings exchange
- request submission from client to server
- response headers and data from server to client
- stream close event generation
- goaway submission and receipt
- invalid state handling after close

This should be done with PHPT so the behavior stays close to the
extension boundary.

### Phase 6: Internal refactor of existing classes

- reimplement `Client` on top of `Session` plus existing TCP/TLS code
- reimplement `Server` connection handling on top of `Session`
- keep public signatures stable

This reduces duplication and keeps one HTTP/2 state machine in the
extension.

## Risks and Tradeoffs

### Event queue memory growth

If userland does not drain events or outbound data, memory use can grow.

This is acceptable for a low-level API, but the documentation should
make the contract explicit.

### Data copying

The first version will likely copy submitted body data and received
event payloads. That is simpler and safer, though not optimal.

### No callback API initially

Some users may prefer callback registration instead of polling
`popEvents()`. That can be added later once the queue-based API is
stable.

### No stream object initially

Using `streamId` directly is lower-level and less ergonomic, but it
keeps the first version aligned with nghttp2 and avoids premature object
model complexity.

## Recommendation

Proceed with a queue-based, transport-agnostic `Nghttp2\Session`
implementation and keep the current `Client` and `Server` APIs intact.

This gives the extension a real low-level core suitable for PHP
userland event loops without forcing a high-level architecture too
early.
