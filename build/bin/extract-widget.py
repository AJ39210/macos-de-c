#!/usr/bin/env python3
import os
import shutil
import stat
import sys
import zipfile


def fail(message):
    print(message, file=sys.stderr)
    raise SystemExit(1)


def main():
    if len(sys.argv) != 3:
        fail("usage: extract-widget.py ARCHIVE DESTINATION")
    archive_path, destination = sys.argv[1:]
    destination = os.path.realpath(destination)
    os.makedirs(destination, mode=0o700, exist_ok=True)
    total_size = 0

    try:
        with zipfile.ZipFile(archive_path) as archive:
            entries = archive.infolist()
            if not entries or len(entries) > 512:
                fail("Widget archive must contain between 1 and 512 entries.")
            for entry in entries:
                name = entry.filename.replace("\\", "/")
                parts = name.split("/")
                if name.startswith("/") or any(part in ("", ".", "..") for part in parts[:-1]):
                    fail("Widget archive contains an unsafe path.")
                if parts[-1] in ("", ".", ".."):
                    if not entry.is_dir():
                        fail("Widget archive contains an invalid path.")
                mode = entry.external_attr >> 16
                file_type = stat.S_IFMT(mode)
                if file_type not in (0, stat.S_IFREG, stat.S_IFDIR):
                    fail("Widget archives may contain only regular files and directories.")
                if entry.is_dir() != (file_type == stat.S_IFDIR):
                    if file_type != 0:
                        fail("Widget archive entry type does not match its path.")
                total_size += entry.file_size
                if total_size > 256 * 1024 * 1024:
                    fail("Expanded widget archive exceeds the 256 MiB limit.")
                output_path = os.path.realpath(
                    os.path.join(destination, *parts))
                if os.path.commonpath((destination, output_path)) != destination:
                    fail("Widget archive contains a path outside the bundle.")

            for entry in entries:
                name = entry.filename.replace("\\", "/")
                target = os.path.join(destination, *name.split("/"))
                if entry.is_dir():
                    os.makedirs(target, mode=0o700, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), mode=0o700, exist_ok=True)
                with archive.open(entry) as source, open(target, "xb") as output:
                    shutil.copyfileobj(source, output)
                mode = entry.external_attr >> 16
                permissions = mode & 0o777
                os.chmod(target, permissions if permissions else 0o600)
    except (OSError, zipfile.BadZipFile, RuntimeError, ValueError) as error:
        fail("Could not safely extract widget archive: {}".format(error))


if __name__ == "__main__":
    main()
