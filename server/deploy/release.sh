#!/usr/bin/env bash
#
# Publishes a signed firmware image for over-the-air update, and chooses which units
# are offered it. Run from Git Bash in the repository. See docs/ota-phase2-design.md, 6a.
#
#   server/deploy/release.sh status
#       What is on offer now, to whom, and each unit's last report.
#
#   server/deploy/release.sh publish <image.bin> <unit-id>... [-y]
#   server/deploy/release.sh publish <image.bin> --all [-y]
#       Checks the image (ESP32 app, version, size, signed with our key), shows what
#       will change, asks to confirm (-y skips that), then puts the image and the
#       rollout list in place together. --all offers it to every unit.
#
#   server/deploy/release.sh units <unit-id>... [-y]
#   server/deploy/release.sh units --all [-y]
#       Changes who is offered the current image, without changing the image.
#
#   server/deploy/release.sh withdraw [-y]
#       Removes the image; every unit is then told there is nothing new.
#
# Unit ids are the 6 MAC hex digits in a unit's clip names, e.g. f30f1c.

set -euo pipefail

HOST=mike@10.147.20.210
FW_DIR=firmware                                   # relative to the server account's home
KEY=C:/Users/gardn/keys/bird_watch_signing.pem
PYTHON=C:/Espressif/python_env/idf5.3_py3.11_env/Scripts/python.exe
MAX_IMAGE=$((3 * 1024 * 1024))                    # one app partition

die()  { echo "release: $*" >&2; exit 1; }
note() { echo "== $*"; }

# ssh that leaves standard input alone, so it cannot swallow the answer to a prompt
remote() { ssh -n "$HOST" "$@"; }

confirm() {
    [[ "$ASSUME_YES" == 1 ]] && return 0
    local answer=""
    read -r -p "$1 [y/N] " answer || true
    [[ "$answer" == y || "$answer" == Y ]] || die "cancelled; nothing changed"
}

# Prints the version from an image's app description (offset 32, magic 0xABCD5432)
image_version() {
    "$PYTHON" -I -c '
import struct, sys
data = open(sys.argv[1], "rb").read(32 + 48 + 32)
if len(data) < 80 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
    sys.exit("not an ESP32 app image")
print(data[48:80].split(b"\0")[0].decode())
' "$1"
}

check_ids() {
    for id in "$@"; do
        [[ "$id" =~ ^[0-9a-f]{1,8}$ ]] || die "'$id' is not a unit id (lowercase hex, e.g. f30f1c)"
    done
}

# Builds the new devices.txt contents for the given ids, or "--all"
devices_text() {
    if [[ "$1" == --all ]]; then
        echo ""
    else
        echo "# Units offered ~/firmware/bird_watch.bin, one device id per line."
        echo "# Written by server/deploy/release.sh on $(date -u '+%Y-%m-%d %H:%M UTC')."
        printf '%s\n' "$@"
    fi
}

show_status() {
    ssh "$HOST" bash -s <<'EOF'
cd ~/firmware 2>/dev/null || { echo "No ~/firmware folder on the server"; exit 0; }
if [ -f bird_watch.bin ]; then
    ver=$(dd if=bird_watch.bin bs=1 skip=48 count=32 2>/dev/null | tr -d '\0')
    echo "On offer:   $ver  ($(stat -c %s bird_watch.bin) bytes, $(stat -c %y bird_watch.bin | cut -c1-16))"
    echo "SHA-256:    $(sha256sum bird_watch.bin | cut -c1-64)"
else
    echo "On offer:   nothing (no bird_watch.bin)"
fi
if [ -f devices.txt ]; then
    echo "Offered to: $(grep -v '^[[:space:]]*#' devices.txt | sed 's/#.*//' | tr -s ' \n' ' ')"
else
    echo "Offered to: every unit (no devices.txt)"
fi
[ -f test_cutoff ] && echo "WARNING:    test_cutoff is present ($(cat test_cutoff) bytes); no download can complete"
echo
echo "Unit reports (latest HELLO from each):"
for f in status/*; do
    [ -f "$f" ] || continue
    awk -F= -v id="$(basename "$f")" '
        { v[$1] = $2 }
        END { printf "  %-8s %-8s %s  battery %s mV  trial %s  last reset %s%s\n",
                     id, v["version"], v["time"], v["battery_mv"], v["on_trial"],
                     (v["last_reset"] == "" ? "?" : v["last_reset"]),
                     (v["rolled_back"] == "" ? "" : "  rolled back " v["rolled_back"]) }' "$f"
done
EOF
}

# Writes devices.txt (or removes it, for --all) on the server, via a temporary file
install_devices() {
    local text
    text=$(devices_text "$@")
    if [[ "$1" == --all ]]; then
        remote "rm -f ~/$FW_DIR/devices.txt"
    else
        printf '%s\n' "$text" | ssh "$HOST" "cat > ~/$FW_DIR/devices.new && mv ~/$FW_DIR/devices.new ~/$FW_DIR/devices.txt"
    fi
}

# ---------------------------------------------------------------------------------

ASSUME_YES=0
ARGS=()
for a in "$@"; do
    if [[ "$a" == -y ]]; then ASSUME_YES=1; else ARGS+=("$a"); fi
done
set -- "${ARGS[@]+"${ARGS[@]}"}"

command=${1:-status}
shift || true

case "$command" in
status)
    show_status
    ;;

publish)
    [[ $# -ge 2 ]] || die "usage: release.sh publish <image.bin> <unit-id>... | --all [-y]"
    image=$1; shift
    [[ -f "$image" ]] || die "no such file: $image"
    [[ "$1" == --all && $# -eq 1 ]] || check_ids "$@"

    size=$(stat -c %s "$image")
    (( size > 0 && size <= MAX_IMAGE )) || die "$image is $size bytes; an app partition holds $MAX_IMAGE"
    version=$(image_version "$image") || die "$image is not an ESP32 app image"

    note "Checking the signature against $KEY"
    "$PYTHON" -m espsecure verify_signature --version 2 --keyfile "$KEY" "$image" > /dev/null 2>&1 \
        || die "$image is not signed with our key; units would refuse it"

    sha=$(sha256sum "$image" | cut -c1-64)
    current=$(remote "cd ~/$FW_DIR 2>/dev/null && [ -f bird_watch.bin ] && dd if=bird_watch.bin bs=1 skip=48 count=32 2>/dev/null | tr -d '\0'" || true)
    current_sha=$(remote "cd ~/$FW_DIR 2>/dev/null && [ -f bird_watch.bin ] && sha256sum bird_watch.bin | cut -c1-64" || true)
    if [[ -n "$current" && "$current" == "$version" && "$current_sha" != "$sha" ]]; then
        die "version $version is already on offer with different contents; give this build a new version number"
    fi

    echo
    echo "Image:      $image"
    echo "Version:    $version  ($size bytes, signed, SHA-256 ${sha:0:16}...)"
    echo "Replaces:   ${current:-nothing}"
    if [[ "$1" == --all ]]; then echo "Offer to:   every unit"; else echo "Offer to:   $*"; fi
    echo
    confirm "Publish $version?"

    note "Uploading"
    remote "mkdir -p ~/$FW_DIR/status"
    scp -q "$image" "$HOST:$FW_DIR/bird_watch.new"
    remote_sha=$(remote "sha256sum ~/$FW_DIR/bird_watch.new | cut -c1-64")
    [[ "$remote_sha" == "$sha" ]] || { remote "rm -f ~/$FW_DIR/bird_watch.new"; die "upload damaged (SHA-256 mismatch); nothing changed"; }

    # Rollout list first, then the image, so the new list never applies to the old image
    # for longer than the moment between the two renames
    install_devices "$@"
    remote "mv ~/$FW_DIR/bird_watch.new ~/$FW_DIR/bird_watch.bin"
    note "Published"
    show_status
    ;;

units)
    [[ $# -ge 1 ]] || die "usage: release.sh units <unit-id>... | --all [-y]"
    [[ "$1" == --all && $# -eq 1 ]] || check_ids "$@"
    if [[ "$1" == --all ]]; then echo "Offer the current image to every unit."; else echo "Offer the current image to: $*"; fi
    confirm "Change the rollout list?"
    install_devices "$@"
    show_status
    ;;

withdraw)
    confirm "Withdraw the image on offer? Every unit will be told there is nothing new."
    remote "rm -f ~/$FW_DIR/bird_watch.bin ~/$FW_DIR/bird_watch.new"
    note "Withdrawn"
    show_status
    ;;

*)
    die "unknown command '$command' (status, publish, units, withdraw)"
    ;;
esac
