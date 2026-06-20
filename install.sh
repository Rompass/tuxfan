#!/usr/bin/env bash
set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== TuxFanControl Installer ===${NC}"

# 1. Check if run as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: Please run this script with sudo or as root.${NC}"
    exit 1
fi

# 2. Check build dependencies
echo -e "Checking build tools..."
for cmd in cmake make gcc; do
    if ! command -v "$cmd" &> /dev/null; then
        echo -e "${RED}Error: Required tool '$cmd' is not installed.${NC}"
        echo -e "Please install it using your package manager (e.g., 'apt install build-essential cmake' or 'pacman -S base-devel cmake')."
        exit 1
    fi
done

# 3. Compile the program
echo -e "Compiling TuxFanControl..."
mkdir -p build
cd build
cmake ..
make -j$(nproc)
cd ..

# 4. Install binary to system path
echo -e "Installing binary to /usr/local/bin/tuxfan..."
cp build/tuxfan /usr/local/bin/tuxfan
chmod 755 /usr/local/bin/tuxfan

# 5. Create Systemd Service
echo -e "Setting up systemd service..."
cat <<EOF > /etc/systemd/system/tuxfan.service
[Unit]
Description=TuxFanControl - Server Fan Speed Controller
After=syslog.target network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/tuxfan --daemon
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload

# 6. Handle initial configuration check
if [ -f "/etc/tuxfan.conf" ]; then
    echo -e "Found existing configuration at /etc/tuxfan.conf."
    echo -e "Enabling and starting systemd service..."
    systemctl enable tuxfan.service
    systemctl restart tuxfan.service
    echo -e "${GREEN}Installation successful! The background service is running.${NC}"
else
    echo -e "${YELLOW}Warning: Configuration file /etc/tuxfan.conf not found.${NC}"
    echo -e "Systemd service was installed but is not started yet."
    echo -e "\n${GREEN}To complete setup:${NC}"
    echo -e "1. Run the configuration tool to map your fans and sensors:"
    echo -e "   ${YELLOW}sudo tuxfan${NC}"
    echo -e "2. Save with [F10] and exit."
    echo -e "3. Start the background system service:"
    echo -e "   ${YELLOW}sudo systemctl enable --now tuxfan.service${NC}"
fi

echo -e "\nCheck service logs anytime with: ${YELLOW}journalctl -u tuxfan -f${NC}"
