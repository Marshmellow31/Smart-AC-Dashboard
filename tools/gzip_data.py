"""Pre-build step: gzip the text assets in data/ so WebServerManager can serve
them with Content-Encoding: gzip (see data/*.gz, .setTryGzipFirst(true))."""
Import("env")

import gzip
import os

DATA_DIR = os.path.join(env["PROJECT_DIR"], "data")
FILES = ["index.html", "style.css", "script.js"]


def gzip_data_files(*_args, **_kwargs):
    for name in FILES:
        src = os.path.join(DATA_DIR, name)
        dst = src + ".gz"
        if not os.path.isfile(src):
            continue
        if os.path.isfile(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            continue
        with open(src, "rb") as f_in:
            data = f_in.read()
        with gzip.GzipFile(dst, "wb", compresslevel=9, mtime=0) as f_out:
            f_out.write(data)
        print("gzip_data: wrote %s (%d -> %d bytes)" % (dst, len(data), os.path.getsize(dst)))


gzip_data_files()

# Best-effort: also regenerate immediately before the filesystem image is
# packaged, in case data/ changed after this script first ran. Not fatal if
# these target names shift between PlatformIO/SCons versions, since the
# eager call above already covers the common case.
for target in ("buildfs", "$BUILD_DIR/littlefs.bin"):
    try:
        env.AddPreAction(target, gzip_data_files)
    except Exception as exc:  # noqa: BLE001
        print("gzip_data: could not hook target %s (%s)" % (target, exc))
