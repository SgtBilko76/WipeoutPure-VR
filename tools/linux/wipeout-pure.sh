#!/usr/bin/env bash
# Starts Wipeout Pure from this folder. The first start asks for the disc image (UCUS-98612) and copies it into
# ~/.local/share/pure-recomp/PureNative; the HD textures go to .../textures/UCUS98612 there.
HERE="$(cd "$(dirname "$0")" && pwd)"
exec "$HERE/PureNative" "$@"
