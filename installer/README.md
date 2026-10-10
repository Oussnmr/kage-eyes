# Browser installer

The hosted installer also includes a reference rollback build at
`versions/8983f4d/manifest.json`. The Pages workflow rebuilds that pinned
commit and publishes its manifest and firmware beside the current installer,
so the reference button remains available after GitHub Actions artifacts
expire. Use it to compare connectivity against the earlier firmware; it does
not include the behavior updates introduced later. Leave "Erase device"
unchecked when installing it to preserve saved Wi-Fi and calibration settings.

Plug the board in, open a page, click Install: the same "flash it from the
browser" flow ESPHome and Home Assistant use ([ESP Web
Tools](https://esphome.github.io/esp-web-tools/), Apache-2.0, vendored under
`vendor/` so the page has no runtime dependency on a CDN). Chrome or Edge on
a desktop; it uses Web Serial, which Safari and Firefox don't have.

The hosted one: **https://stratobuilds.com/pocket-tank-installer/**

## Build the upload folder

```
cd firmware && idf.py build                        # or idf.py -B <dir> build
tools/make_installer.py                            # -> installer/dist/
python3 -m http.server -d installer/dist 8765      # local check at http://localhost:8765
```

`installer/dist/` is the whole thing: `index.html`, `manifest.json`,
`firmware/*.bin` (bootloader, partition table, app, model), `vendor/`. The
offsets in the manifest come from the build's `flasher_args.json` and from
`firmware/partitions.csv`, and the page carries the git version. It is
gitignored: rebuild it for every release. `--build-dir` points at another
`idf.py -B` build directory; the default is `firmware/build`.

## It updates itself (GitHub Pages)

`.github/workflows/installer.yml` builds the firmware with ESP-IDF v5.4.1 on
every push to `main` that touches `firmware/`, `common/`, `model/out/`,
`installer/` or the assembler, runs `tools/make_installer.py`, and deploys
the folder to **https://mediacutlet.github.io/pocket-tank/**. A fork gets
the same for free: enable Pages (Settings -> Pages -> Source: GitHub
Actions) and push. Pages serves HTTPS with `Access-Control-Allow-Origin:
*`, so a page on any other site can point its button at that manifest:

```
tools/make_installer.py --manifest-url https://<you>.github.io/pocket-tank/manifest.json --out /tmp/site
```

The page fetches the manifest on load and shows the version and build date
of what it will actually flash.

## Host your own

Web Serial needs a secure context, so the page must be on **HTTPS** (or
`localhost`). Any static host works; the manifest and the `.bin` files must be
fetchable from the page's origin (or send CORS headers).

- **A plain folder on a web server.** Upload `installer/dist/` as-is. The
  included `.htaccess` adds the `.bin` and `.json` types for Apache /
  LiteSpeed hosts that refuse them; on nginx add
  `types { application/octet-stream bin; }`. If a CDN sits in front, purge
  the folder after uploading a new build (the manifest carries the version,
  so a stale cache shows an old version string on the page).
- **GitHub Pages.** Push `dist/` to a `gh-pages` branch or a `docs/` folder.
  Pages serves HTTPS and `Access-Control-Allow-Origin: *`, so the button can
  even live on another site (a `<script type="module">` tag plus
  `<esp-web-install-button manifest="https://...manifest.json">`) while the
  binaries stay on GitHub.

Two things to check once on a new host: the button must say *Install Pocket
Tank* rather than the unsupported notice (a script optimizer rewriting the
module tag breaks it), and `manifest.json` and `firmware/*.bin` must return
200 in the browser's network tab.

## What the user sees

Click → the browser's port picker (`USB JTAG/serial debug unit`) → a dialog
that asks whether to erase first (yes = a brand-new tank; no = keep an
existing tank's fish and history) → a progress bar over the four parts →
*Installation complete*, and the board resets into the tank. The page's
"If something's off" section covers the usual snags: a drowsing tank hides
its USB port (press BOOT), holding BOOT while plugging in forces download
mode, Linux group permissions, charge-only cables.

## Verified

The artifact set in `dist/firmware/` at the manifest's offsets was written to
the real board with `esptool.py write_flash` (the exact operation ESP Web
Tools performs, from the same files), and the tank booted; the page, manifest
and vendor bundle were checked from a local server, and the hosted copy was
click-tested end to end on the board.
