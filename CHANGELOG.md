# Changelog

All notable changes to this project are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/). A release is created by pushing a `vX.Y.Z` tag; the section
for that version is used as the release notes.

## [Unreleased]

## [0.1.0] - 2026-11-14

First release.

### Added
- Sysmodule that blocks the "Please turn flight mode off" popup games show while the console is in flight mode.
  It patches nifm in memory only; nothing on the system partition is changed. The patch is found by instruction
  patterns and checked against nifm from firmware 17.0.1 to 23.5.0. On a firmware that does not match, nothing is patched.
- Overlay (Ultrahand / Tesla-Menu): switch the patch on and off live, status line, version row,
  and a built-in **Run test** that sends a real nifm request and reports `works` / `FAILED` / `turn WiFi off`.
- Config file `/config/sys-antinag/config.ini` (`enabled`, `debug`, `show_debug`), status in `status.txt`, log in `log.txt`.
- Developer debug mode (off by default, hidden in the overlay unless `show_debug=1`): peek/poke/dump/probe by dropping
  files into `/config/sys-antinag/debug/`, e.g. over FTP.
- GitHub Actions build; pushing a `vX.Y.Z` tag creates a release with `sys-antinag.zip`.
- Homebrew App Store package description in `hbas/`.
- GPLv2 license, icon (`docs/icon.svg`).
