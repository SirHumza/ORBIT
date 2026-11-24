# ORBIT

ORBIT is a native OpenOrbis PS4 package browser and installer built around the FPKGi JSON catalog format. It keeps the FPKGi data directory for catalog compatibility and uses BGFT/AppInstUtil for package installation. The interface is drawn directly with SDL and uses a compact controller-first list rather than remote artwork or mandatory services.

The source is based on FPKGi Native under GPL-2.0. See [LICENSE](LICENSE), [NOTICE](NOTICE), and the preserved third-party notices. ORBIT changes are maintained in this repository; hardware support is not claimed until tested on the target console.

## Safety and catalogs

The bundled `catalog/catalog.json` is empty. Add only FPKGs you created or have permission to download and install. Catalogs use the standard FPKGi `DATA` map; a sample is in [docs/catalog.md](docs/catalog.md). Catalog URLs are optional and can be hosted by the user. ORBIT has no mandatory account, telemetry service, or download server.

HTTPS uses the packaged Mozilla root bundle, rejects downgrade redirects, validates exact package size and PS4 PKG magic, and requires SHA-256 for HTTP package links. Downloads stage under `/data/FPKGi/downloads`; the durable queue is stored as FPKGi-compatible JSON at `/data/FPKGi/queue.json`.

## Build and tests

Host contracts require Python 3.10+ and a C++11 compiler:

```sh
make HOST_CXX=g++ host-tests
python3 scripts/validate_catalog.py catalog/catalog.json
```

For a PS4 package, install OpenOrbis Toolchain v0.5.4, set `OO_PS4_TOOLCHAIN`, then run `make ORBIT.pkg`. The GitHub Actions workflow builds and validates `ORBIT.pkg` and uploads it as an artifact on pushes, pull requests, and manual runs.

## Controls

D-pad moves through catalog entries. Cross adds the selected entry to the persistent queue. Triangle refreshes the optional ORBIT catalog URL. R1 retries a saved queue after a failure. Circle exits only when no job is active. Packages install sequentially; errors leave the failed queue item and local package available for retry or inspection.

## Status

The HTTPS catalog/download/install path is inherited from the GPL-2 FPKGi Native base. The durable queue and retry path are new ORBIT work. Native BitTorrent, torrent-to-install integration, background operation after closing the app, Rest Mode completion behavior, and GoldHEN 9.00 hardware qualification remain in progress; see [PLAN.md](PLAN.md).
