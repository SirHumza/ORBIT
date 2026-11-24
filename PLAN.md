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
- [ ] Native BitTorrent engine: implement magnets, `.torrent`, piece/hash verification, pause/resume, and source fallback. A local libtorrent 2.0.11 static cross-build now compiles against the OpenOrbis target with a small PS4 portability patch; it is not yet linked into ORBIT or verified on hardware.
- [ ] Connect torrent completion to the existing PKG verification and BGFT installer.
- [ ] Add removable storage, installed-title view, search/filter/sort, and richer package details.
- [ ] Add configurable completion behavior and Rest Mode request after queue completion.
- [ ] Verify suspend/resume and Rest Mode behavior on GoldHEN 9.00 PS4 Slim; do not assume userland torrent tasks survive suspend.
- [ ] Exercise clean install, reinstall, HTTPS retry, queue recovery, and package verification on the target console.

## FPKGi feedback to address

Queue loss/cancellation: [issue 39](https://github.com/ItsJokerZz/FPKGi/issues/39). Back-download retry failures: [issue 50](https://github.com/ItsJokerZz/FPKGi/issues/50). FPKGi 1.10 also shipped a queue, additional storage, reinstall support, and matched system box art; ORBIT is prioritizing durable queues and reliable retry first.

## Torrent assessment

The referenced `sergiopoverony/ps4torrent_client` GitHub URL returned 404 during research, so reusable source is not yet verified. Rasterbar libtorrent is BSD-3-Clause and depends on Boost. A local experiment built its 2.0.11 static archive for the OpenOrbis FreeBSD target using Boost 1.89 headers, disabled route-table enumeration, FreeBSD socket ioctl constants, and a signal-header compatibility fix. This is only a compile result: the archive has not been linked into ORBIT, exercised through its runtime APIs, packaged, or tested on a PS4. Do not claim native torrent support until those steps succeed.
