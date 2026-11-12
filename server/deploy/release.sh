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
#   server/deploy/release.sh config <file.cfg> [-y]
#       Publishes unit settings (see server/config.cfg.example). Every setting is
#       checked against the names and ranges in main/bw_settings.c first. Units on
#       1.5.0+ fetch it at their next connection. To put every unit back to its
#       firmware defaults, publish a file with no settings in it (comments only).
#
# Unit ids are the 6 MAC hex digits in a unit's clip names, e.g. f30f1c.

set -euo pipefail

HOST=mike@10.147.20.210
FW_DIR=firmware                                   # relative to the server account's home
KEY=C:/Users/gardn/keys/bird_watch_signing.pem
PYTHON=C:/Espressif/python_env/idf5.3_py3.11_env/Scripts/python.exe
MAX_IMAGE=$((3 * 1024 * 1024))                    # one app partition
MAX_CONFIG=1024                                   # BW_SETTINGS_MAX_TEXT in main/bw_settings.h
SETTINGS_C="$(cd "$(dirname "$0")/../.." && pwd)/main/bw_settings.c"

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
if [ -f config.cfg ]; then
    echo "Settings:   config.cfg, CRC $(gzip -c config.cfg | tail -c8 | od -An -tx4 -N4 | tr -d ' '), $(stat -c %s config.cfg) bytes, $(stat -c %y config.cfg | cut -c1-16)"
else
    echo "Settings:   no config.cfg (units keep the settings they last received)"
fi
echo
echo "Unit reports (latest HELLO from each):"
for f in status/*; do
    [ -f "$f" ] || continue
    awk -F= -v id="$(basename "$f")" '
        { v[$1] = $2 }
        END { printf "  %-8s %-8s %s  battery %s mV  trial %s  last reset %s%s\n",
                     id, v["version"], v["time"], v["battery_mv"], v["on_trial"],
                     (v["last_reset"] == "" ? "?" : v["last_reset"]),
                     (v["rolled_back"] == "" ? "" : "  rolled back " v["rolled_back"])
              if ("config_current" in v)
                  printf "           settings %s, current: %s%s\n", v["config"], v["config_current"],
                         (v["config_refused"] + 0 > 0 ? ", " v["config_refused"] " value(s) REFUSED" : "") }' "$f"
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

config)
    [[ $# -eq 1 ]] || die "usage: release.sh config <file.cfg> [-y]"
    cfg=$1
    [[ -f "$cfg" ]] || die "no such file: $cfg"
    clean=$(mktemp)
    trap 'rm -f "$clean"' EXIT

    # Check every setting against the firmware's own table of names and ranges, and
    # write just the settings (no comments) to $clean: that is what units receive
    note "Checking $cfg against $SETTINGS_C"
    "$PYTHON" -I - "$cfg" "$SETTINGS_C" "$clean" <<'PY' || die "fix the settings above; nothing changed"
import re, sys, math
cfg_path, src_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
table = {}
for m in re.finditer(r'\{"(\w+)",\s*SET_(FLOAT|U32),\s*F\(\w+\),\s*[^,]+,\s*([-\d.]+),\s*([-\d.]+)\}', open(src_path).read()):
    table[m.group(1)] = (m.group(2), float(m.group(3)), float(m.group(4)))
if not table:
    sys.exit("could not read the settings table from " + src_path)
problems, values = [], {}
for n, raw in enumerate(open(cfg_path, encoding="ascii", errors="replace"), 1):
    line = raw.split("#", 1)[0].strip()
    if not line:
        continue
    if "=" not in line:
        problems.append(f"line {n}: no '=' in '{line}'"); continue
    key, val = (p.strip() for p in line.split("=", 1))
    if key not in table:
        problems.append(f"line {n}: unknown setting '{key}' (known: {', '.join(table)})"); continue
    kind, lo, hi = table[key]
    try:
        v = float(val)
    except ValueError:
        problems.append(f"line {n}: {key}={val} is not a number"); continue
    if not math.isfinite(v) or v < lo or v > hi or (kind == "U32" and v != int(v)):
        problems.append(f"line {n}: {key}={val} is outside {lo:g} to {hi:g}" + (" (whole numbers)" if kind == "U32" else "")); continue
    values[key] = (val, v)
s, e = values.get("active_start_hour", ("", 0))[1], values.get("active_end_hour", ("", 24))[1]
if s == e:
    problems.append(f"active_start_hour and active_end_hour are both {s:g}: that window is empty")
for p in problems:
    print("  " + p)
if problems:
    sys.exit(1)
# A file with no settings must still reach the units (an empty file means "no
# settings on the server", and units would keep their old ones), so it becomes a
# comment, which units apply as "everything at its default"
text = "".join(f"{k}={val}\n" for k, (val, _) in values.items()) or "# all firmware defaults\n"
open(out_path, "w", newline="\n").write(text)
print("  " + (", ".join(f"{k}={val}" for k, (val, _) in values.items()) or "no settings: units go back to their firmware defaults"))
PY
    size=$(stat -c %s "$clean")
    (( size <= MAX_CONFIG )) || die "the settings come to $size bytes; units accept at most $MAX_CONFIG"
    crc=$("$PYTHON" -I -c 'import sys, zlib; print("%08x" % zlib.crc32(open(sys.argv[1], "rb").read()))' "$clean")
    echo
    echo "Settings:   from $cfg ($size bytes as sent, CRC $crc)"
    echo "Applies to: every unit on firmware 1.5.0 or later, at its next connection"
    echo
    confirm "Publish these settings?"
    remote "mkdir -p ~/$FW_DIR"
    scp -q "$clean" "$HOST:$FW_DIR/config.new"
    remote "mv ~/$FW_DIR/config.new ~/$FW_DIR/config.cfg"
    note "Published"
    show_status
    ;;

*)
    die "unknown command '$command' (status, publish, units, withdraw, config)"
    ;;
esac
