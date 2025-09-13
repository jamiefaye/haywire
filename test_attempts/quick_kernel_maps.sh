#!/bin/bash

echo "Testing if kernel threads expose memory mappings..."

# Use the existing guest agent connection
QGA=/tmp/qga.sock

send_cmd() {
    echo "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"/bin/sh\",\"arg\":[\"-c\",\"$1\"],\"capture-output\":true}}" | nc -U $QGA
}

echo "1. Checking kernel threads:"
send_cmd "ps aux | grep '\[' | head -3"

echo -e "\n2. Checking /proc/0/maps:"
send_cmd "cat /proc/0/maps 2>&1 | head -3"

echo -e "\n3. Checking /proc/2/maps (kthreadd):"
send_cmd "cat /proc/2/maps 2>&1 | head -3"

echo -e "\n4. Checking /proc/kcore:"
send_cmd "ls -la /proc/kcore"