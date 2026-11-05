# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Releases: every tag publishes `fastdl-linux32-authum.zip` and
  `fastdl-win32-authum.zip` (the module, its config and licences; Authum is
  installed on its own), and CI builds Windows next to Linux.
- A logo and a README in the style of the other Authum modules.

### Changed

- `lib/authum` moves to Authum `4360d64` (lh 0.5.0): lh's execution part is
  `lh/rail` now; the HTTP test driver follows.

### Fixed

- The sanitizer test build links Authum's runtime, which fills lh's runtime
  slots; without it the HTTP driver died on its first allocation. The HTTP
  test also reports the driver's exit code and stderr when it exits early.

## [0.5.0] - 2026-11-04

### Changed

- Built on Authum 4.0.0 (module ABI 3.0): Authum is the game library, no
  Metamod. Placeholders (`{game}`, `{module}`, `{data}`, ...) come from Authum's
  placeholder service; the HTTP server, clock and disk logging are Authum's
  shared services.
- No libc: the module reads the world through the Authum facade, so the same
  sources build as a 32-bit Linux `.so` and a 32-bit Windows `.dll`.
- HTTP hardening: stricter request-target checks (encoded `/` and NUL, control
  characters, reserved names, 8.3 short names) and bounded stop on unload.
