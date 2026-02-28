# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog, and this project generally follows Semantic Versioning.

## [Unreleased]

### Added

- Added `Nghttp2\Session` as a transport-agnostic low-level HTTP/2 session API for PHP userland event loops.
- Added `Nghttp2\Exception\SessionException`.
- Added session event queue support for `headers`, `data`, `stream_close`, `settings`, `ping`, `goaway`, and `rst_stream`.
- Added control frame submission APIs: `submitSettings()`, `submitPing()`, `submitRstStream()`, and `submitGoaway()`.
- Added request body support with queued `submitData()` calls for multi-chunk outbound stream data.
- Added PHPT coverage for session class loading, event queue behavior, control frames, and request/response body streaming.
- Added a session class design note in [docs/session-class-design.md](/home/masakielastic/php-ext-nghttp2/docs/session-class-design.md).

### Changed

- Repositioned the README around API layers: `Hpack`, `Session`, `Client`, and `Server`.
- Documented the `Session` basic flow, event model, and client/server usage examples in [README.md](/home/masakielastic/php-ext-nghttp2/README.md).

## [0.1.0] - 2026-02-28

### Added

- Initial public package metadata and release tagging.
- HPACK support via `Nghttp2\Hpack`.
- Blocking HTTP/2 client and server APIs backed by nghttp2 and OpenSSL.

