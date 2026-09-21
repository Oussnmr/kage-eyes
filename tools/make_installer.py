#!/usr/bin/env python3
"""make_installer.py - assemble the browser installer (ESP Web Tools).

Gathers the firmware build's bootloader / partition table / app, the
installer page and the vendored ESP Web Tools
bundle into ONE static folder that any HTTPS host can serve as-is:

    installer/dist/
        index.html          the page (version stamped in)
        manifest.json       what to flash where (ESP Web Tools format)
        firmware/*.bin      bootloader, partition table, app
        vendor/esp-web-tools/*.js   the flasher (Apache-2.0, vendored so the
                                    page has no third-party runtime deps)

Offsets come from the build's flasher_args.json, so a layout change can't
silently ship a stale offset.

    tools/make_installer.py                      # default build dir
    tools/make_installer.py --build-dir path     # another idf.py -B dir
    tools/make_installer.py --version 1.2.0      # instead of git describe

Web Serial needs a secure context: serve the folder over HTTPS (or from
http://localhost for a local check: `python3 -m http.server -d installer/dist`)."""
import argparse, datetime, json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.expanduser("~/.cache/pocket-tank/fw-build")


def git_version():
    try:
        out = subprocess.run(["git", "-C", ROOT, "describe", "--tags", "--always", "--dirty"],
                             capture_output=True, text=True, check=True).stdout.strip()
        return out
    except Exception:
        return "dev"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", default=DEFAULT_BUILD if os.path.isdir(DEFAULT_BUILD)
                    else os.path.join(ROOT, "firmware", "build"))
    ap.add_argument("--out", default=os.path.join(ROOT, "installer", "dist"))
    ap.add_argument("--version", default=None)
    ap.add_argument("--manifest-url", default="manifest.json",
                    help="what the page's install button points at: the relative default for a "
                         "self-contained folder, or an absolute URL (e.g. the GitHub Pages copy) "
                         "for a page hosted somewhere else")
    a = ap.parse_args()

    fa_path = os.path.join(a.build_dir, "flasher_args.json")
    if not os.path.isfile(fa_path):
        sys.exit(f"{fa_path}: not a firmware build dir (run idf.py build first)")
    fa = json.load(open(fa_path))
    parts = []   # (offset, source path, published name)
    # Include otadata as well as the app itself. Without it, an ESP32 that
    # previously booted ota_1 can keep starting that older slot after a USB
    # installer flash writes the new image to ota_0.
    for key, pub in (("bootloader", "bootloader.bin"), ("partition-table", "partition-table.bin"),
                     ("app", "kage_eyes.bin"), ("otadata", "ota_data_initial.bin")):
        ent = fa[key]
        parts.append((int(ent["offset"], 0), os.path.join(a.build_dir, ent["file"]), pub))
    parts.sort()
    for off, src, pub in parts:
        if not os.path.isfile(src):
            sys.exit(f"missing: {src}")

    version = a.version or git_version()
    # Keep the exact build timestamp visible on the public installer page.
    # GitHub Actions runs in UTC, so label it explicitly to avoid ambiguity.
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    out = a.out
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, "firmware"))
    total = 0
    for off, src, pub in parts:
        shutil.copyfile(src, os.path.join(out, "firmware", pub))
        if pub == "kage_eyes.bin":
            # Keep the stable OTA endpoint used by deployed Kage Eyes units.
            shutil.copyfile(src, os.path.join(out, "firmware", "app.bin"))
        total += os.path.getsize(src)
    shutil.copytree(os.path.join(ROOT, "installer", "vendor"), os.path.join(out, "vendor"))

    manifest = {
        "name": "Kage Eyes",
        "version": version,
        "built": date,                       # read by the page (ESP Web Tools ignores extra keys)
        "new_install_prompt_erase": True,   # the dialog offers "erase": a factory-fresh tank
        "builds": [{
            "chipFamily": "ESP32-S3",
            "parts": [{"path": f"firmware/{pub}", "offset": off} for off, _, pub in parts],
        }],
    }
    json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
    # Apache / LiteSpeed hosts sometimes refuse .bin or serve .json as text;
    # harmless elsewhere
    open(os.path.join(out, ".htaccess"), "w").write(
        "AddType application/octet-stream .bin\nAddType application/json .json\n"
        "AddType text/javascript .js\n")

    page = open(os.path.join(ROOT, "installer", "index.html")).read()
    page = page.replace("{{VERSION}}", version).replace("{{DATE}}", date)
    page = page.replace("{{TOTAL_MB}}", f"{total / 1e6:.1f}")
    page = page.replace("{{MANIFEST}}", a.manifest_url)
    open(os.path.join(out, "index.html"), "w").write(page)
    open(os.path.join(out, ".nojekyll"), "w").close()   # GitHub Pages: serve the folder as-is

    print(f"installer -> {out}  (version {version}, {date}; manifest {a.manifest_url})")
    for off, src, pub in parts:
        print(f"  0x{off:06x}  {os.path.getsize(src):>9,} B  {pub}")
    print(f"  {total:,} B to flash")


if __name__ == "__main__":
    main()
