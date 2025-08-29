#!/bin/bash

# Simple file server to share files with VM
echo "Starting file server on http://10.0.2.2:8000"
echo "In the VM, download files with:"
echo "  wget http://10.0.2.2:8000/yourfile.mp4"
echo ""
echo "Press Ctrl+C to stop"

cd /Users/jamie/haywire
python3 -m http.server 8000