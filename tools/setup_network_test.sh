#!/bin/bash

# Setup networking for VMM testing

echo "Setting up TAP interface for VMM network testing..."

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo "Please run as root (use sudo)"
    exit 1
fi

# Create TAP interface if it doesn't exist
if ! ip link show tap0 &> /dev/null; then
    echo "Creating tap0 interface..."
    ip tuntap add dev tap0 mode tap
else
    echo "tap0 already exists"
fi

# Configure the TAP interface
echo "Configuring tap0 interface..."
ip link set tap0 up
ip addr flush dev tap0
ip addr add 192.168.100.1/24 dev tap0

# Enable IP forwarding (optional, for internet access)
echo 1 > /proc/sys/net/ipv4/ip_forward

# Show configuration
echo ""
echo "TAP interface configured:"
ip addr show tap0

echo ""
echo "Setup complete!"
echo "Host IP: 192.168.100.1"
echo "Suggested guest IP: 192.168.100.2"
echo ""
echo "Run your VMM with: sudo ./cos run vmm_ping_test enable-nic"
echo "Then in the guest VM, configure networking with:"
echo "  ip addr add 192.168.100.2/24 dev eth0"
echo "  ip link set eth0 up"
echo "  ping 192.168.100.1"
