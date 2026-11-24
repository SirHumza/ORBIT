# ORBIT delivery plan

## Product target

A controller-first FPKGi-compatible PS4 library with native BitTorrent, persistent/resumable jobs, safe package verification, automatic BGFT installation, no required hosted service, and a restrained original console UI. Target: GoldHEN 9.00 on PS4 Slim. Hardware compatibility is unverified until tested there.

## Current implementation

- [x] New Git repository with FPKGi Native GPL-2 source attribution retained.
- [x] Native OpenOrbis build and CI artifact path renamed to `ORBIT.pkg`.
- [x] FPKGi `DATA` catalog parser and verified HTTPS package flow retained.
- [x] Original dark, low-glow list UI implemented in SDL.
- [x] Durable FPKGi-shaped download queue; sequential install and retry after failure.
- [x] Reinstall/overwrite path no longer rejects an installed title before BGFT.
- [x] Default bundled catalog is empty; only authorized catalog entries may be added.
- [x] Native libtorrent 2.0.11 engine accepts v1 magnets and local `.torrent` metadata, verifies torrent pieces, selects a size-matched PKG, then passes it through the existing SHA-256/PKG checks and BGFT install flow. HTTPS remains the fallback.
- [ ] Add in-app pause/resume and fast-resume metadata. Partial torrent pieces remain in the work folder and are rechecked when retried.
- [ ] Add removable storage, installed-title view, search/filter/sort, and richer package details.
- [x] Automatic sleep and Rest Mode are OFF. ORBIT issues no power-state request when downloads finish.
- [ ] Research suspend/resume and Rest Mode behavior on GoldHEN 9.00 PS4 Slim. Do not assume userland torrent tasks survive suspend.
- [ ] Exercise clean install, reinstall, HTTPS retry, queue recovery, and package verification on the target console.

## FPKGi feedback to address

Queue loss/cancellation: [issue 39](https://github.com/ItsJokerZz/FPKGi/issues/39). Back-download retry failures: [issue 50](https://github.com/ItsJokerZz/FPKGi/issues/50). FPKGi 1.10 also shipped a queue, additional storage, reinstall support, and matched system box art; ORBIT is prioritizing durable queues and reliable retry first.

## Torrent assessment

The referenced `sergiopoverony/ps4torrent_client` GitHub URL returned 404 during research, so reusable source is not verified. Rasterbar libtorrent is BSD-3-Clause and depends on Boost. The build script pins libtorrent 2.0.11 and Boost 1.89, applies a small OpenOrbis compatibility patch, and links the static library into ORBIT. Torrent sessions have no app-imposed upload or download cap, allow up to 100 peers for the active torrent, and accelerate initial connection attempts. Peer availability, network quality, and console storage remain the practical limits; throughput is not benchmarked on PS4 hardware.
