#!/bin/sh

MAJOR=$(awk "\$2==\"lbprofile\" {print \$1}" /proc/devices)
echo "$MAJOR"
rm -f /dev/lbprofile

mknod /dev/lbprofile c "$MAJOR" 0

