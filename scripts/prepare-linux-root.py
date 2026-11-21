#!/usr/bin/env python3
"""Download unchanged Alpine Linux x86-64 applications into a guest-only tar.

No host packages are installed. All downloads and generated files stay in build/.
"""
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import tarfile
import urllib.request

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build" / "linux"
BASE = "https://dl-cdn.alpinelinux.org/alpine/v3.22"
BUSYBOX = "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"


def download(url, name):
    path = BUILD / name
    if not path.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            data = response.read()
        path.write_bytes(data)
    return path.read_bytes()


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    packages = {}
    providers = {}
    for repository in ("main", "community"):
        data = download(f"{BASE}/{repository}/x86_64/APKINDEX.tar.gz",
                        f"{repository}-index.tar.gz")
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
            index = archive.extractfile("APKINDEX").read().decode()
        for entry in index.split("\n\n"):
            fields = dict(line.split(":", 1) for line in entry.splitlines()
                          if ":" in line)
            if "P" not in fields:
                continue
            fields["repository"] = repository
            packages[fields["P"]] = fields
            for provider in fields.get("p", "").split():
                providers[provider.split("=", 1)[0]] = fields["P"]

    pending = ["xeyes", "musl"]
    selected = {}
    while pending:
        name = pending.pop()
        if name in selected:
            continue
        package = packages[name]
        selected[name] = package
        for dependency in package.get("D", "").split():
            if dependency.startswith("!"):
                continue
            key = dependency.split("=", 1)[0].split(">", 1)[0].split("<", 1)[0]
            pending.append(providers[key] if key.startswith("so:") else key)

    files = {}
    manifest = []
    for name, package in sorted(selected.items()):
        filename = f"{name}-{package['V']}.apk"
        url = f"{BASE}/{package['repository']}/x86_64/{filename}"
        data = download(url, filename)
        manifest.append({"package": name, "version": package["V"], "url": url,
                         "license": package.get("L", ""),
                         "sha256": hashlib.sha256(data).hexdigest()})
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz",
                          ignore_zeros=True) as archive:
            for member in archive:
                path = PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(f"Unsafe package path: {member.name}")
                if member.name.startswith(".") or not (member.isfile() or member.issym()):
                    continue
                member.uid = member.gid = member.mtime = 0
                member.uname = member.gname = ""
                files[str(path)] = (member, archive.extractfile(member).read()
                                    if member.isfile() else None)

    data = download(BUSYBOX, "busybox-linux-x86_64")
    member = tarfile.TarInfo("bin/busybox")
    member.mode = 0o755
    member.size = len(data)
    files[member.name] = (member, data)
    manifest.append({"package": "busybox", "version": "1.35.0", "url": BUSYBOX,
                     "license": "GPL-2.0-only",
                     "sha256": hashlib.sha256(data).hexdigest()})
    output = REPO / "build" / "linux-root.tar"
    with tarfile.open(output, "w", format=tarfile.USTAR_FORMAT) as archive:
        for name, (member, data) in sorted(files.items()):
            archive.addfile(member, io.BytesIO(data) if data is not None else None)
    (BUILD / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"{output}: {len(files)} guest files, {len(manifest)} source packages")


if __name__ == "__main__":
    main()
