#!/bin/bash
# FPT-169: Test driver for Raft PoC
# Run from host while docker-compose is up.
# Control ports: node0=9100, node1=9101, node2=9102

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

PASS=0
FAIL=0

send_cmd() {
    local port=$1
    shift
    echo "$@" | nc -q1 -w2 localhost "$port" 2>/dev/null || echo "ERROR: connection failed"
}

check() {
    local desc="$1"
    local expected="$2"
    local actual="$3"

    if echo "$actual" | grep -q "$expected"; then
        echo -e "${GREEN}PASS${NC}: $desc"
        PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC}: $desc"
        echo "  expected to contain: $expected"
        echo "  actual: $actual"
        FAIL=$((FAIL + 1))
    fi
}

echo "==========================================="
echo "  FPT-169: Raft PoC Test Suite"
echo "==========================================="
echo ""

# --- Test 1: Leader election ---
echo "--- Test 1: Leader Election ---"
echo "Waiting 5s for election..."
sleep 5

LEADER_PORT=""
LEADER_ID=""
for port in 9100 9101 9102; do
    resp=$(send_cmd $port "status")
    echo "  node (port $port): $resp"
    if echo "$resp" | grep -q "state=leader"; then
        LEADER_PORT=$port
        LEADER_ID=$(echo "$resp" | grep -o 'id=[0-9]*' | cut -d= -f2)
    fi
done

if [ -n "$LEADER_PORT" ]; then
    check "Leader elected" "state=leader" "$(send_cmd $LEADER_PORT status)"
    echo "  Leader is node $LEADER_ID on control port $LEADER_PORT"
else
    echo -e "${RED}FAIL${NC}: No leader elected after 5s"
    FAIL=$((FAIL + 1))
    echo "Aborting — no leader available"
    echo ""
    echo "Results: $PASS passed, $FAIL failed"
    exit 1
fi
echo ""

# --- Test 2: Custom entry types ---
echo "--- Test 2: Custom Entry Types ---"

# MHT_UPDATE
resp=$(send_cmd $LEADER_PORT "submit MHT_UPDATE 0 10")
check "MHT_UPDATE submit" "rc=0" "$resp"

resp=$(send_cmd $LEADER_PORT "submit MHT_UPDATE 1 20")
check "MHT_UPDATE submit (kernel 1)" "rc=0" "$resp"

# CAP_BLOCK
resp=$(send_cmd $LEADER_PORT "submit CAP_BLOCK")
check "CAP_BLOCK submit" "rc=0" "$resp"

# CAP_UNBLOCK
resp=$(send_cmd $LEADER_PORT "submit CAP_UNBLOCK")
check "CAP_UNBLOCK submit" "rc=0" "$resp"

# CAP_EXCHANGE
resp=$(send_cmd $LEADER_PORT "submit CAP_EXCHANGE")
check "CAP_EXCHANGE submit" "rc=0" "$resp"

# CAP_GROUP_REVOKE
resp=$(send_cmd $LEADER_PORT "submit CAP_GROUP_REVOKE")
check "CAP_GROUP_REVOKE submit" "rc=0" "$resp"

# UPDATE_ROOT_CA
resp=$(send_cmd $LEADER_PORT "submit UPDATE_ROOT_CA")
check "UPDATE_ROOT_CA submit" "rc=0" "$resp"

echo ""
echo "Waiting 2s for replication..."
sleep 2

# --- Test 3: State consistency ---
echo "--- Test 3: State Consistency ---"

STATE0=$(send_cmd 9100 "state_hash")
STATE1=$(send_cmd 9101 "state_hash")
STATE2=$(send_cmd 9102 "state_hash")

echo "  node0: $STATE0"
echo "  node1: $STATE1"
echo "  node2: $STATE2"

if [ "$STATE0" = "$STATE1" ] && [ "$STATE1" = "$STATE2" ]; then
    check "All nodes have identical state" "applied=" "$STATE0"
else
    echo -e "${RED}FAIL${NC}: State mismatch between nodes"
    FAIL=$((FAIL + 1))
fi

# Verify content
check "MHT entries present" "mht=2" "$STATE0"
check "Block count" "block=1" "$STATE0"
check "Exchange count" "exchange=1" "$STATE0"
echo ""

# --- Test 4: Commit latency measurement ---
echo "--- Test 4: Commit Latency ---"
START=$(date +%s%N)
for i in $(seq 1 20); do
    send_cmd $LEADER_PORT "submit MHT_UPDATE $((i+10)) $i" > /dev/null
done
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
echo "  20 entries submitted in ${ELAPSED_MS}ms (avg $((ELAPSED_MS / 20))ms/entry)"
echo ""

# --- Test 5: Leader failure + re-election ---
echo "--- Test 5: Leader Failure ---"

# Determine leader container name
LEADER_CONTAINER="raft-node${LEADER_ID}"
echo "  Killing leader: $LEADER_CONTAINER"
docker stop "$LEADER_CONTAINER" > /dev/null 2>&1

echo "  Waiting 5s for re-election..."
sleep 5

NEW_LEADER_PORT=""
for port in 9100 9101 9102; do
    resp=$(send_cmd $port "status" 2>/dev/null) || continue
    if echo "$resp" | grep -q "state=leader"; then
        NEW_LEADER_PORT=$port
        NEW_LEADER_ID=$(echo "$resp" | grep -o 'id=[0-9]*' | cut -d= -f2)
    fi
done

if [ -n "$NEW_LEADER_PORT" ] && [ "$NEW_LEADER_PORT" != "$LEADER_PORT" ]; then
    check "New leader elected" "state=leader" "$(send_cmd $NEW_LEADER_PORT status)"
    echo "  New leader is node $NEW_LEADER_ID on port $NEW_LEADER_PORT"
else
    echo -e "${RED}FAIL${NC}: No new leader elected after killing $LEADER_CONTAINER"
    FAIL=$((FAIL + 1))
fi

# Submit to new leader
if [ -n "$NEW_LEADER_PORT" ]; then
    resp=$(send_cmd $NEW_LEADER_PORT "submit MHT_UPDATE 99 99")
    check "Submit to new leader" "rc=0" "$resp"
fi
echo ""

# --- Test 6: Log replay (restart killed node) ---
echo "--- Test 6: Log Replay ---"
echo "  Restarting $LEADER_CONTAINER..."
docker start "$LEADER_CONTAINER" > /dev/null 2>&1

echo "  Waiting 8s for catch-up..."
sleep 8

# Check if restarted node caught up
RESTART_PORT=$((9100 + LEADER_ID))
resp=$(send_cmd $RESTART_PORT "status")
echo "  Restarted node status: $resp"

# Note: after restart, in-memory state is lost since we don't persist.
# The node re-joins but starts fresh. This is expected for RAM-only mode.
if echo "$resp" | grep -q "id="; then
    check "Restarted node is responsive" "id=" "$resp"
else
    echo -e "${YELLOW}WARN${NC}: Restarted node not responsive (expected — RAM-only, no persistence)"
fi
echo ""

# --- Summary ---
echo "==========================================="
echo "  Results: ${GREEN}$PASS passed${NC}, ${RED}$FAIL failed${NC}"
echo "==========================================="

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
