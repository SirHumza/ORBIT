# FPKGi catalog compatibility

ORBIT reads the FPKGi `DATA` object: each key is a direct package URL and each value is its metadata. The bundled `catalog/catalog.json` intentionally has an empty `DATA` object until package redistribution authorization is verified. The app does not require ORBIT-hosted catalogs.

```json
{
  "DATA": {
    "https://downloads.example.org/homebrew.pkg": {
      "title_id": "ORBT00002",
      "region": "ALL",
      "name": "Owner Homebrew",
      "version": "1.00",
      "release": "2026-01-01",
      "size": "8388608",
      "min_fw": "9.00",
      "cover_url": null,
      "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    }
  }
}
```

`title_id`, `name`, `version`, and `size` are required. Size is a nonzero decimal byte count. HTTP entries require a SHA-256 digest; HTTPS entries use verified TLS and exact size plus PKG magic validation, with SHA-256 accepted when available. Unknown fields, invalid IDs, duplicate title/version identities, malformed URLs, credentials, fragments, invalid UTF-8, and unsafe redirects are rejected.

## Existing FPKGi data

At startup ORBIT checks `/user/data/FPKGi/ContentJSONs/` for `.json` files and merges their `DATA` entries. It keeps the native FPKGi cache at `/data/FPKGi/catalog.json` and writes durable queue state in the same JSON shape to `/data/FPKGi/queue.json`. Legacy catalog files are opened without following symlinks and validated before use.

The download staging directory is `/data/FPKGi/downloads`. Packages are written as `.part`, checked against catalog size and PS4 PKG magic (`7F 43 4E 54`), then submitted to BGFT/AppInstUtil. Failed downloads and installs keep the queue entry and local data for retry or inspection.

## Torrent entries

The standard FPKGi `DATA` map stays intact. ORBIT adds two optional metadata fields: `magnet_uri` for a BitTorrent v1 magnet, or `torrent_file` for a `.torrent` file copied to `/data/FPKGi/torrents/`. Use one field per entry. The package URL remains the HTTPS fallback; the torrent must match the declared size and pass PKG validation before installation.

```json
{
  "DATA": {
    "https://downloads.example.org/homebrew.pkg": {
      "title_id": "ORBT00002",
      "name": "Owner Homebrew",
      "version": "1.00",
      "size": "8388608",
      "magnet_uri": "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
    }
  }
}
```

For a qBittorrent-exported file, copy it to `/data/FPKGi/torrents/` and set its basename in `torrent_file`. ORBIT opens that file without following symlinks. The existing FPKGi package URL is used if the torrent source fails. Only import torrents and package sources you are authorized to use.
