#!/usr/bin/env bash
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${RED}=== TuxFanControl Uninstaller ===${NC}"

# 1. Check if run as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: Please run this script with sudo or as root.${NC}"
    exit 1
fi

# 2. Stop and disable Systemd Service
if systemctl is-active --quiet tuxfan.service &>/dev/null; then
    echo -e "Stopping background service..."
    systemctl stop tuxfan.service || true
fi

if systemctl is-enabled --quiet tuxfan.service &>/dev/null; then
    echo -e "Disabling systemd service..."
    systemctl disable tuxfan.service || true
fi

if [ -f "/etc/systemd/system/tuxfan.service" ]; then
    echo -e "Removing systemd service file..."
    rm /etc/systemd/system/tuxfan.service
    systemctl daemon-reload
fi

# 3. Remove binary
if [ -f "/usr/local/bin/tuxfan" ]; then
    echo -e "Removing system binary..."
    rm /usr/local/bin/tuxfan
fi

# 4. Optional configuration cleanup
if [ -f "/etc/tuxfan.conf" ]; then
    echo -e "\nConfiguration file found at /etc/tuxfan.conf"
    read -p "Do you want to delete your fan profiles config as well? (y/N): " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm /etc/tuxfan.conf
        echo -e "Configuration deleted."
    else
        echo -e "Configuration file kept at /etc/tuxfan.conf."
    fi
fi

echo -e "${GREEN}TuxFanControl has been successfully uninstalled.${NC}"
