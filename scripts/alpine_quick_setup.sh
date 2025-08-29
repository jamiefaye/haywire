#!/bin/sh
# Quick Alpine setup script - run this inside the Alpine VM

echo "Setting up Alpine with VLC..."

# Setup network
echo "Configuring network..."
ifconfig eth0 up
udhcpc -i eth0

# Setup repositories with a direct mirror (no interaction needed)
echo "Setting up package repositories..."
echo "http://dl-cdn.alpinelinux.org/alpine/v3.19/main" > /etc/apk/repositories
echo "http://dl-cdn.alpinelinux.org/alpine/v3.19/community" >> /etc/apk/repositories

# Update package index
echo "Updating package index..."
apk update

# Install VLC and dependencies
echo "Installing VLC and X11..."
apk add --no-cache \
    xorg-server \
    xf86-video-vesa \
    xf86-input-evdev \
    xf86-input-mouse \
    xf86-input-keyboard \
    vlc \
    mesa-dri-gallium \
    ttf-dejavu \
    openbox \
    xterm

echo "Setup complete!"
echo ""
echo "To start GUI with VLC:"
echo "  startx"
echo "  Then in xterm: vlc"