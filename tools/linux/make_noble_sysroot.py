#!/usr/bin/env python3
"""Build an Ubuntu 24.04 (noble, glibc 2.39) x86-64 sysroot from the archive's .deb files, without root:
the C library, kernel headers and GCC 13's libstdc++/libgcc, extracted with dpkg-deb -x."""
import gzip, pathlib, subprocess, sys, urllib.request

OUT = pathlib.Path(sys.argv[1]).resolve()
CACHE = OUT.parent / "noble-debs"
MIRROR = "http://archive.ubuntu.com/ubuntu"
WANTED = ["libc6", "libc6-dev", "linux-libc-dev", "libcrypt-dev", "libcrypt1", "libgcc-13-dev",
          "libstdc++-13-dev", "libgcc-s1", "libstdc++6", "gcc-13-base", "libasan8", "libubsan1"]

def index(suite):
    url = f"{MIRROR}/dists/{suite}/main/binary-amd64/Packages.gz"
    data = gzip.decompress(urllib.request.urlopen(url, timeout=60).read()).decode("utf-8", "replace")
    found = {}
    for block in data.split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if ":" in line and not line.startswith(" "):
                k, v = line.split(":", 1)
                fields[k] = v.strip()
        if fields.get("Package") in WANTED:
            found[fields["Package"]] = (fields["Version"], fields["Filename"])
    return found

packages = index("noble")
packages.update(index("noble-updates"))  # newer point releases win
missing = [p for p in WANTED if p not in packages]
if missing:
    sys.exit(f"not found: {missing}")
CACHE.mkdir(parents=True, exist_ok=True)
OUT.mkdir(parents=True, exist_ok=True)
for name in WANTED:
    version, filename = packages[name]
    deb = CACHE / pathlib.Path(filename).name
    if not deb.exists():
        urllib.request.urlretrieve(f"{MIRROR}/{filename}", deb)
    subprocess.run(["dpkg-deb", "-x", str(deb), str(OUT)], check=True)
    print(f"{name} {version}")
# Absolute symlinks inside the sysroot (e.g. lib64/ld-linux-x86-64.so.2) must point into it.
for link in OUT.rglob("*"):
    if link.is_symlink():
        target = link.readlink()
        if target.is_absolute():
            link.unlink()
            link.symlink_to(OUT / target.relative_to("/"))
print("sysroot:", OUT)
