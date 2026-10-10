#!/usr/bin/env bash
# Copies the package (build/AstroQuest-SteamFrame, made by build.sh) to the Steam Frame over SSH,
# with the files' executable bits, which a copy through Windows loses.
#
#   steam-frame/deploy.sh steamos@<headset address> [folder on the headset]
#
# The folder defaults to Applications/AstroQuest in the home folder there. Nothing there is
# deleted, and a settings.txt of your own is kept (the package's goes next to it as
# settings.default.txt, to see what keys there are). A game put into the package's games folder
# goes along; with rsync (on both sides) only what changed is sent again, without it (tar through
# ssh) games are left out - copy them once yourself. STEAM_FRAME_SSH_KEY=<file> names the key
# to log in with (the Devkit Client's, for instance); ssh's own settings apply otherwise.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/.." && pwd)
src="$root/build/AstroQuest-SteamFrame"

host=${1:-}
dest=${2:-Applications/AstroQuest}
if [[ -z "$host" ]]; then
    echo "usage: $0 steamos@<headset address> [folder on the headset]" >&2
    exit 2
fi
[[ -x "$src/bin/shadps4" ]] || { echo "No package in $src: run steam-frame/build.sh first" >&2; exit 1; }

ssh_args=(-o ConnectTimeout=10)
[[ -n "${STEAM_FRAME_SSH_KEY:-}" ]] && ssh_args+=(-i "$STEAM_FRAME_SSH_KEY")
remote() { ssh "${ssh_args[@]}" "$host" "$@"; }

echo "[deploy] to $host:$dest"
remote "mkdir -p '$dest'"
if command -v rsync >/dev/null && remote "command -v rsync" >/dev/null 2>&1; then
    rsync -a --info=progress2 -e "ssh ${ssh_args[*]}" \
        --exclude settings.txt "$src/" "$host:$dest/"
else
    tar -C "$src" --exclude ./settings.txt --exclude './games/CUSA*' -cf - . |
        remote "tar -C '$dest' -xf -"
fi
# The package's settings: as settings.txt where there is none yet, beside it otherwise.
remote "cat > '$dest/settings.default.txt'" < "$src/settings.txt"
remote "[ -f '$dest/settings.txt' ] || cp '$dest/settings.default.txt' '$dest/settings.txt'"
remote "chmod +x '$dest/astroquest.sh' '$dest/bin/shadps4'"
echo "[deploy] done: start $dest/astroquest.sh (as a Non-Steam game, or from a shell there)"
