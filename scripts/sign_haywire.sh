#!/bin/bash

# Self-sign haywire with debugging entitlements
# This allows task_for_pid() to work without sudo

BINARY="build/haywire"
ENTITLEMENTS="haywire.entitlements"

# Remove any existing signature
codesign --remove-signature "$BINARY" 2>/dev/null

# Sign with debugging entitlements using ad-hoc identity
codesign --entitlements "$ENTITLEMENTS" --force --sign - "$BINARY"

echo "Signed $BINARY with debugging entitlements"
echo "Verifying signature..."
codesign --display --entitlements - "$BINARY"