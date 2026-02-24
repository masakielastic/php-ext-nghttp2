# php-nghttp2

A native PHP extension powered by **nghttp2** providing
low-level HTTP/2 functionality.

This extension aims to expose a minimal, explicit, and
stateful interface to HTTP/2 primitives including:

- HPACK (RFC 7541)
- HTTP/2 client (planned)
- HTTP/2 server (planned)
- Frame-level access (planned)

The design focuses on low-level experimentation,
blocking APIs, and educational use.

---

## Features (Current)

- Stateful HPACK encoder/decoder
- Dynamic table support
- Explicit error handling
- No hidden HTTP abstractions

---

## Planned Features

- Blocking HTTP/2 client
- Blocking HTTP/2 server
- Frame construction and inspection
- Explicit connection state control

---

## Requirements

- PHP 8.1+
- libnghttp2 (development headers required)
- pkg-config (recommended)

On Debian/Ubuntu:

```bash
sudo apt install libnghttp2-dev pkg-config
```

---

## Directory Structure
.
├── composer.json
├── README.md
└── ext/
    ├── config.m4
    ├── nghttp2.c
    ├── hpack.c
    ├── client.c          # planned
    ├── server.c          # planned
    ├── exception.c
    └── php_nghttp2.h

All extension sources reside in ext/.

---

## Installation (PIE)

Register repository:

```
pie repository:add vcs https://github.com/<your-account>/php-nghttp2
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

```
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
```

Error codes can be retrieved:

```php
$e->getNghttp2ErrorCode();
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
Experimental.
Intended for educational and low-level HTTP/2 experimentation.
