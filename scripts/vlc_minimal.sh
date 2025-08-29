#!/bin/sh
# Minimal setup - easy to type manually
ifconfig eth0 up
udhcpc
echo "http://dl-cdn.alpinelinux.org/alpine/v3.19/main" > /etc/apk/repositories
echo "http://dl-cdn.alpinelinux.org/alpine/v3.19/community" >> /etc/apk/repositories
apk update
apk add vlc xorg-server xterm openbox
echo "Done! Run: startx"