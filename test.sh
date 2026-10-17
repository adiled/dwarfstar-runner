#!/bin/sh
# End-to-end trial for the front door.
#
#   ./test.sh
#
# Stop everything, start the front door, then make two turns through wme that
# should push the arbiter into swapping the model underneath the client. Every
# step is asserted and the script exits non-zero if any check fails.
#
# A cold turn loads tens of gigabytes, so a full run takes minutes.
#
# Override with env: STATION_A, STATION_B, PROMPT, EXPECT, WME, FRONT.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT"

STATION_A=${STATION_A:-deepseek-v4-flash}
STATION_B=${STATION_B:-glm-5.2}
PROMPT=${PROMPT:-Reply with exactly: PONG}
EXPECT=${EXPECT:-PONG}
WME=${WME:-wme}
FRONT=${FRONT:-http://127.0.0.1:8899}

pass=0
fail=0
ok()    { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()   { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (want \"$3\", got \"$2\")"; fi; }
note()  { printf '\n%s\n' "$1"; }

command -v "$WME" >/dev/null 2>&1 || { echo "test.sh: $WME is not in PATH" >&2; exit 2; }
command -v jq >/dev/null 2>&1   || { echo "test.sh: jq is not in PATH" >&2; exit 2; }

# Ports declared by the grove specs, one per model.
grove_ports() {
    grep -h -o -E -- '--port [0-9]+' servers/*.orch 2>/dev/null | awk '{print $2}' | sort -n
}

# Which of those ports is listening right now.
live_groves() {
    for p in $(grove_ports); do
        lsof -nP -iTCP:"$p" -sTCP:LISTEN >/dev/null 2>&1 && echo "$p"
    done
}

front_code() { curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$FRONT/v1/models" || true; }

# launchd needs a moment to install or remove the unit, so poll rather than
# asserting on the instant.
wait_up() {
    i=0
    while [ "$i" -lt 50 ]; do
        [ "$(front_code)" = "200" ] && return 0
        i=$((i + 1)); sleep 0.2
    done
    return 1
}

wait_down() {
    i=0
    while [ "$i" -lt 25 ]; do
        [ "$(front_code)" = "000" ] && return 0
        i=$((i + 1)); sleep 0.2
    done
    return 1
}

note "1. everything down"
./stop
if wait_down; then ok "front door is gone after ./stop"; else bad "front door still answers after ./stop"; fi
check "no grove is listening" "$(live_groves | wc -l | tr -d ' ')" "0"

note "2. front door up"
./run >/dev/null
if wait_up; then ok "front door answers /v1/models"; else bad "front door did not come up"; fi
ids=$(curl -s --max-time 5 "$FRONT/v1/models" | jq -r '.data | length' 2>/dev/null || echo 0)
if [ "${ids:-0}" -ge 1 ]; then ok "advertises $ids model ids"; else bad "advertises no model ids"; fi
check "no grove is loaded before the first request" "$(live_groves | wc -l | tr -d ' ')" "0"

note "3. wme -p on $STATION_A, which should raise the first grove"
a_out=$("$WME" --station "$STATION_A" -p "$PROMPT" 2>&1 | tail -1)
check "$STATION_A answered" "$a_out" "$EXPECT"
a_ports=$(live_groves)
check "exactly one grove is listening" "$(printf '%s\n' "$a_ports" | grep -c .)" "1"

note "4. wme -p on $STATION_B, which should swap the holder"
b_out=$("$WME" --station "$STATION_B" -p "$PROMPT" 2>&1 | tail -1)
check "$STATION_B answered" "$b_out" "$EXPECT"
b_ports=$(live_groves)
check "exactly one grove is listening" "$(printf '%s\n' "$b_ports" | grep -c .)" "1"
if [ "$a_ports" != "$b_ports" ]; then
    ok "the holder moved from :$a_ports to :$b_ports"
else
    bad "the holder did not move, still :$a_ports"
fi
if printf '%s\n' "$b_ports" | grep -qx "$a_ports"; then
    bad "the previous grove is still listening on :$a_ports"
else
    ok "the previous grove was stopped"
fi

note "result"
printf '  %d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
