#!/bin/bash

# Setup script to configure wangemu-terminal-server to start on boot
# This adds the startup script to rc.local for automatic startup

set -e

WANGEMU_DIR="$(pwd)"
STARTUP_SCRIPT="$WANGEMU_DIR/start-wangemu.sh"

echo "Setting up Wang 2200 Terminal Server to start on boot..."

# Make startup script executable
chmod +x "$STARTUP_SCRIPT"

# Backup rc.local if it exists
if [ -f /etc/rc.local ]; then
    echo "Backing up existing /etc/rc.local to /etc/rc.local.backup"
    sudo cp /etc/rc.local /etc/rc.local.backup
fi

# Create or modify rc.local to include our startup script
echo "Adding startup command to /etc/rc.local"

# Remove our entry if it already exists
sudo sed -i '/start-wangemu\.sh/d' /etc/rc.local 2>/dev/null || true

# Add our startup command before the final 'exit 0' line
sudo sed -i "/^exit 0/i\\
# Start Wang 2200 Terminal Server\\
$STARTUP_SCRIPT" /etc/rc.local

# If rc.local doesn't exist or doesn't have exit 0, create it
if ! grep -q "exit 0" /etc/rc.local 2>/dev/null; then
    echo "Creating new /etc/rc.local"
    sudo tee /etc/rc.local > /dev/null << EOF
#!/bin/sh -e
#
# rc.local
#
# This script is executed at the end of each multiuser runlevel.

# Start Wang 2200 Terminal Server
$STARTUP_SCRIPT

exit 0
EOF
fi

# Make rc.local executable
sudo chmod +x /etc/rc.local

echo ""
echo "Setup complete!"
echo "Wang 2200 Terminal Server will now start automatically on boot"
echo ""
echo "To test now: $STARTUP_SCRIPT"
echo "To remove from startup: sudo sed -i '/start-wangemu.sh/d' /etc/rc.local"