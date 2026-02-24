# php-nghttp2

A native PHP extension powered by **nghttp2** providing
low-level HTTP/2 functionality.

This extension aims to expose a minimal, explicit, and
stateful interface to HTTP/2 primitives including:

- HPACK (RFC 7541)
- Blocking HTTP/2 client
- Blocking HTTP/2 server
- Frame-level access (planned)

The design focuses on low-level experimentation,
blocking APIs, and educational use.

---

## Features (Current)

- Stateful HPACK encoder/decoder
- Blocking HTTP/2 client over TLS (ALPN `h2`)
- Blocking HTTP/2 server over TLS (ALPN `h2`)
- Configurable server response (`setResponse`)
- Explicit error handling with nghttp2 error codes

---

## Requirements

- PHP 8.1+
- libnghttp2 (development headers required)
- OpenSSL (development headers required)
- pkg-config (recommended)

On Debian/Ubuntu:

```bash
sudo apt install libnghttp2-dev libssl-dev pkg-config
```

---

## Directory Structure

```
.
├── composer.json
├── README.md
└── ext/
    ├── config.m4
    ├── nghttp2.c
    ├── hpack.c
    ├── client.c
    ├── server.c
    ├── exception.c
    └── php_nghttp2.h
```

All extension sources reside in ext/.

---

## Installation (PIE)

Register repository:

```
pie repository:add vcs https://github.com/masakielastic/php-nghttp2
```

Install:

```
pie install php-nghttp2
```

Verify:

```
php -m | grep nghttp2
```

---

## Namespace

```php
namespace Nghttp2;
```

---

## HPACK API

Class: Nghttp2\Hpack

```
final class Hpack
{
    public function __construct(int $maxDynamicTableSize = 4096);

    public function setMaxDynamicTableSize(int $size): void;
    public function getMaxDynamicTableSize(): int;

    public function getDynamicTableSize(): int;
    public function clearDynamicTable(): void;

    public function encode(array $headers): string;
    public function decode(string $headerBlock): array;
}
```

---

## HTTP/2 Client API

Class: `Nghttp2\Client`

```php
final class Client
{
    public function __construct(string $host, int $port = 443);

    /**
     * @return array{
     *   status:int|null,
     *   headers:array<int, array{name:string, value:string}>,
     *   body:string
     * }
     */
    public function request(string $path, array $headers = []): array;

    public function close(): void;
}
```

---

## HTTP/2 Server API

Class: `Nghttp2\Server`

```php
final class Server
{
    public function __construct(
        string $certFile,
        string $keyFile,
        string $ip = "127.0.0.1",
        int $port = 8443
    );

    public function setResponse(int $status, array $headers, string $body): void;
    public function serveOnce(): void;
    public function serve(): void;
    public function close(): void;
}
```

`setResponse()` の `headers` は次のどちらでも指定できます。

```php
[
    "content-type" => "application/json",
    ["name" => "x-example", "value" => "1"],
]
```

`content-length` は内部で再計算されます。

---

## Header Format

Encoding requires list format:

```php
[
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':path', 'value' => '/'],
    ['name' => 'host', 'value' => 'example.com'],
]
```

Decoding returns the same list format.

Headers are treated as raw binary strings.

---

## Example

```php
use Nghttp2\Hpack;

$hpack = new Hpack();

$headers = [
    ['name' => ':method', 'value' => 'GET'],
    ['name' => ':path', 'value' => '/'],
    ['name' => 'host', 'value' => 'example.com'],
];

$block = $hpack->encode($headers);

$decoded = $hpack->decode($block);

var_dump($decoded);
```

---

## Error Handling

All nghttp2-related failures throw:

```php
Nghttp2\Exception\HpackException
Nghttp2\Exception\ClientException
Nghttp2\Exception\ServerException
```

Error codes can be retrieved:

```php
$e->getNghttp2ErrorCode();
```

---

## Client Example

```php
$client = new Nghttp2\Client('httpbin.org', 443);

try {
    $res = $client->request('/get?foo=bar', [
        'accept' => 'application/json',
    ]);

    var_dump($res['status']);
    echo $res['body'], PHP_EOL;
} finally {
    $client->close();
}
```

---

## Server Example

```php
$server = new Nghttp2\Server(__DIR__ . '/server.crt', __DIR__ . '/server.key');
$server->setResponse(
    200,
    ['content-type' => 'application/json'],
    '{"ok":true}'
);

$server->serve();
```

self-signed certificate:

```bash
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout server.key -out server.crt -days 365 \
  -subj "/CN=127.0.0.1"
```

test:

```bash
curl -k --http2 https://127.0.0.1:8443/
```

---

## Design Principles

 * Minimal abstraction
 * Explicit state control
 * No implicit I/O
 * Suitable for RFC experimentation
 * Designed for low-level HTTP/2 work

---

## Non-Goals
 * High-level HTTP client abstraction
 * PSR-7 integration
 * Asynchronous event loop integration
 * HTTP/3 (QPACK)

---

## License

MIT (or your chosen license)

---

## Status

Experimental.
Intended for educational and low-level HTTP/2 experimentation.
