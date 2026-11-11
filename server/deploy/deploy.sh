#!/usr/bin/env bash
#
# Builds a server on the Debian machine and swaps it in, keeping the old binary.
# Run from Git Bash:   server/deploy/deploy.sh socksrvr
#
# If the new binary does not start and listen on its port, the previous one is
# put back and restarted, and the script exits with an error.

set -euo pipefail

HOST=mike@10.147.20.210
NAME=${1:?usage: deploy.sh <server name, e.g. socksrvr>}

case "$NAME" in
    socksrvr) BIN=socksrvr.3590; PORT=3590 ;;
    *) echo "Unknown server: $NAME" >&2; exit 1 ;;
esac

SERVER_DIR=$(cd "$(dirname "$0")/.." && pwd)

echo "== Building $NAME on the server"
ssh "$HOST" 'mkdir -p ~/bw_server_src'
scp -q -r "$SERVER_DIR/Makefile" "$SERVER_DIR/$NAME" "$HOST:bw_server_src/"
ssh "$HOST" "cd ~/bw_server_src && make CXX=g++ LDFLAGS= build/$NAME"

echo "== Swapping in the new binary"
ssh "$HOST" bash -s <<EOF
set -e
cd ~/bin
cp -p $BIN $BIN.prev
# A rename replaces the file safely even while the old binary is running
install -m 755 ~/bw_server_src/build/$NAME $BIN.new
mv $BIN.new $BIN
systemctl --user restart $NAME
sleep 2
if systemctl --user is-active --quiet $NAME && ss -ltn | grep -q ":$PORT "; then
    echo "Deployed: $NAME is running on port $PORT (previous binary kept as $BIN.prev)"
else
    echo "The new $NAME did not start; restoring the previous binary" >&2
    cp -p $BIN.prev $BIN.new
    mv $BIN.new $BIN
    systemctl --user restart $NAME
    exit 1
fi
EOF
