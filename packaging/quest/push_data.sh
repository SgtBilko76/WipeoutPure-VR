#!/usr/bin/env bash
# Copies the game data to the headset over adb: your disc image of Wipeout Pure (US Greatest Hits, UCUS-98612
# v2.00, CRC32 4502C588) and, optionally, an HD texture pack in PPSSPP's format (a folder holding textures.ini,
# such as NR74W's WipEout Pure HD texture pack).
#
#   packaging/quest/push_data.sh <disc image .iso or .zip> [<texture pack folder>]
#
# The app plays from /sdcard/Android/data/online.portroyale.wipeoutpurevr/files/game/ (disc.iso and EBOOT.ELF,
# the disc's plain executable) and reads texture packs from .../files/textures/UCUS98612/. Folders made there
# over adb are not the app's own, so the data goes in as tar archives into .../files/import/, which the app
# unpacks the next time it starts (a minute or two for the texture pack).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PACKAGE=online.portroyale.wipeoutpurevr
FILES="/sdcard/Android/data/$PACKAGE/files"
ADB="${ADB:-adb}"
[[ $# -ge 1 ]] || { sed -n '2,12p' "$0"; exit 2; }
image="$1"
pack="${2:-}"
work="$(mktemp -d "${TMPDIR:-/tmp}/pure-push.XXXXXX")"
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/game/game"
if [[ "$image" == *.zip ]]; then
    echo "unpacking $image"
    python3 - "$image" "$work/game/game/disc.iso" <<'EOF'
import shutil, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    name = next(n for n in z.namelist() if n.lower().endswith(".iso"))
    with z.open(name) as src, open(sys.argv[2], "wb") as dst:
        shutil.copyfileobj(src, dst, 1 << 20)
EOF
else
    cp "$image" "$work/game/game/disc.iso"
fi
python3 "$ROOT/tools/iso_extract.py" "$work/game/game/disc.iso" verify
python3 "$ROOT/tools/iso_extract.py" "$work/game/game/disc.iso" cat /PSP_GAME/SYSDIR/BOOT.BIN > "$work/game/game/EBOOT.ELF"
tar --format=ustar -cf "$work/game.tar" -C "$work/game" game

if [[ -n "$pack" ]]; then
    [[ -f "$pack/textures.ini" ]] || { echo "$pack holds no textures.ini" >&2; exit 1; }
    mkdir -p "$work/tex/textures"
    ln -s "$(cd "$pack" && pwd)" "$work/tex/textures/UCUS98612"
    tar --format=ustar --dereference --exclude=.git -cf "$work/textures.tar" -C "$work/tex" textures
fi

# The app makes its folders on its first start.
if ! "$ADB" shell "[ -d $FILES/import ]"; then
    echo "starting the app once so that it makes its folders"
    "$ADB" shell monkey -p "$PACKAGE" -c android.intent.category.LAUNCHER 1 > /dev/null
    for _ in $(seq 1 30); do
        "$ADB" shell "[ -d $FILES/import ]" && break
        sleep 1
    done
    "$ADB" shell am force-stop "$PACKAGE"
fi
"$ADB" shell "[ -d $FILES/import ]" || { echo "the app did not make $FILES/import; is it installed?" >&2; exit 1; }
"$ADB" push "$work/game.tar" "$FILES/import/game.tar"
[[ -f "$work/textures.tar" ]] && "$ADB" push "$work/textures.tar" "$FILES/import/textures.tar"
echo "pushed; the app unpacks it the next time it starts"
