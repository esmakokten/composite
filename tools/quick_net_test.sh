#!/bin/bash

# Quick network test launcher
# Usage: sudo ./quick_net_test.sh [http|tcp|udp|icmp] [interface_name]

TEST_TYPE=${1:-http}
INTERFACE_NAME=${2:-tap0}

echo "╔════════════════════════════════════════════════╗"
echo "║   VMM Network Testing - Quick Start            ║"
echo "╚════════════════════════════════════════════════╝"
echo ""

# Check if root
if [ "$EUID" -ne 0 ]; then 
    echo "❌ Please run as root (use sudo)"
    exit 1
fi

# Setup network interface
echo "📡 Setting up network interface..."

# Check if interface exists
if ip link show $INTERFACE_NAME &>/dev/null; then
    echo "   $INTERFACE_NAME already exists (real or existing virtual interface)"
else
    # Try to create as TAP interface
    if ip tuntap add dev $INTERFACE_NAME mode tap 2>/dev/null; then
        echo "   Created TAP interface: $INTERFACE_NAME"
    else
        echo "❌ Interface $INTERFACE_NAME does not exist and could not be created"
        exit 1
    fi
fi

ip link set $INTERFACE_NAME up
ip addr flush dev $INTERFACE_NAME
ip addr add 15.15.15.15/24 dev $INTERFACE_NAME
echo "✓ Interface ready: $INTERFACE_NAME (15.15.15.15/24)"
echo ""

# Start appropriate server
case $TEST_TYPE in
    http)
        echo "🌐 Starting HTTP server on port 8000..."
        cd /tmp
        cat > vmm_test.html << 'EOF'
<!DOCTYPE html>
<html><head><title>VMM Network Test</title></head>
<body style="font-family: Arial; padding: 40px; background: #f0f0f0;">
    <h1 style="color: #2ecc71;">✓ SUCCESS!</h1>
    <p style="font-size: 18px;">Network connectivity is working!</p>
    <p>Guest VM → VMM → NIC Manager → Host</p>
    <hr>
    <p><small>Test server running on Composite OS host</small></p>
</body></html>
EOF
        pkill -f "python.*SimpleHTTP" 2>/dev/null
        python3 -m http.server 8000 --bind 15.15.15.15 &
        SERVER_PID=$!
        echo "✓ HTTP Server: http://15.15.15.15:8000"
        ;;
    tcp)
        echo "💬 Starting TCP echo server on port 9999..."
        python3 - <<'EOF' &
import socket, threading
def handle(c, a):
    print(f"[+] {a}")
    try:
        while True:
            d = c.recv(1024)
            if not d: break
            print(f"[{a}] {d.decode('utf-8', errors='ignore').strip()}")
            c.sendall(b"Echo: " + d)
    except: pass
    finally: c.close()
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('15.15.15.15', 9999))
s.listen(5)
print("[+] Listening on 15.15.15.15:9999")
while True:
    c, a = s.accept()
    threading.Thread(target=handle, args=(c,a), daemon=True).start()
EOF
        SERVER_PID=$!
        sleep 1
        echo "✓ TCP Server: nc 15.15.15.15 9999"
        ;;
    udp)
        echo "📨 Starting UDP echo server on port 8888..."
        python3 - <<'EOF' &
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('15.15.15.15', 8888))
print("[+] Listening on 15.15.15.15:8888")
n = 0
while True:
    d, a = s.recvfrom(4096)
    n += 1
    received = d.decode('utf-8', errors='ignore').strip()
    print(f"[{n}] REQUEST from {a}: {received}")
    reply = f"Echo #{n}: ".encode() + d
    print(f"[{n}] REPLY to {a}: {reply.decode('utf-8', errors='ignore').strip()}")
    s.sendto(reply, a)
EOF
        SERVER_PID=$!
        sleep 1
        echo "✓ UDP Server: echo 'hello' | nc -u 15.15.15.15 8888"
        ;;
    icmp|ping)
        echo "🏓 Starting ICMP Echo Reply server..."
        python3 - <<'EOF' &
import socket
import struct
import time

# Create raw ICMP socket
s = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_ICMP)
s.bind(('15.15.15.15', 0))
print("[+] ICMP Echo server listening on 15.15.15.15")
print("[+] Ready to reply to ping requests")
print("")

def checksum(data):
    s = 0
    for i in range(0, len(data), 2):
        if i + 1 < len(data):
            s += (data[i] << 8) + data[i+1]
        else:
            s += data[i]
    s = (s >> 16) + (s & 0xffff)
    s = ~s & 0xffff
    return s

n = 0
while True:
    packet, addr = s.recvfrom(1024)
    
    # Parse ICMP header (type, code, checksum, id, sequence)
    icmp_type, code, chk, p_id, seq = struct.unpack('!BBHHH', packet[20:28])
    
    if icmp_type == 8:  # Echo Request
        n += 1
        print(f"[{n}] REQUEST from {addr[0]}: ICMP Echo Request (id={p_id}, seq={seq})")
        
        # Create Echo Reply (type 0)
        reply_header = struct.pack('!BBHHH', 0, 0, 0, p_id, seq)
        reply_data = packet[28:]  # Copy original data
        reply_chk = checksum(reply_header + reply_data)
        reply_header = struct.pack('!BBHHH', 0, 0, socket.htons(reply_chk), p_id, seq)
        reply_packet = reply_header + reply_data
        
        # Send reply
        s.sendto(reply_packet, (addr[0], 0))
        print(f"[{n}] REPLY to {addr[0]}: ICMP Echo Reply (id={p_id}, seq={seq})")
        print("")
EOF
        SERVER_PID=$!
        sleep 1
        echo "✓ ICMP Server: ping 15.15.15.15"
        ;;
esac

echo ""
echo "╔════════════════════════════════════════════════╗"
echo "║   Server is running!                           ║"
echo "╚════════════════════════════════════════════════╝"
echo ""
echo "📋 Next steps:"
echo ""
echo "1️⃣  In another terminal, run:"
echo "   sudo ./cos run vmm_ping_test enable-nic"
echo ""
echo "2️⃣  Inside the guest VM:"
echo "   ip addr add 15.15.15.15/24 dev eth0"
echo "   ip link set eth0 up"
echo "   ping 15.15.15.15 -c 4"
echo ""
echo "3️⃣  Test the server:"

case $TEST_TYPE in
    http)
        echo "   wget http://15.15.15.15:8000/vmm_test.html"
        echo "   curl http://15.15.15.15:8000/vmm_test.html"
        ;;
    tcp)
        echo "   nc 15.15.15.15 9999"
        echo "   (type messages and press enter)"
        ;;
    udp)
        echo "   echo 'Hello VMM!' | nc -u 15.15.15.15 8888"
        ;;
    icmp|ping)
        echo "   ping 15.15.15.15 -c 4"
        echo "   (or from guest: ping 15.15.15.15)"
        ;;
esac

echo ""
echo "Press Ctrl+C to stop..."
echo ""

# Cleanup on exit
trap "echo ''; echo '🛑 Stopping server...'; kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null; echo '✓ Server stopped'; exit 0" INT TERM

# Wait
wait $SERVER_PID 2>/dev/null
