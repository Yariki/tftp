import socket
import struct
import subprocess
import sys
import time
import tempfile
from pathlib import Path

def u16(value):
    return struct.pack('!H', value)

def request(opcode, name, options=b""):
    return u16(opcode) + name.encode() + b"\0octet\0" + options

def read_file(port, name):
    result = bytearray()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(4)
        client.sendto(request(1,name), ("127.0.0.1", port))
        expected = 1
        while True:
            packet, address = client.recvfrom(2048)
            opcode, block = struct.unpack("!HH", packet[:4])
            assert opcode == 3, f"Expected DATA, got {opcode}"
            assert block == expected, f"Expected block {expected}, got {block}"
            result.extend(packet[4:])
            client.sendto(u16(4) + u16(block), address)
            if len(packet) < 512:
                return bytes(result)
            expected = (expected + 1) & 0xFFFF

def write_file(port, name, content):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(4)
        client.sendto(request(2, name,b"\0size\0" + str(len(content)).encode() + b"\0"), ("127.0.0.1", port))
        packet, address = client.recvfrom(2048)
        assert packet == u16(4) + u16(0),f"Expected ACK(0), got{packet|r}"
        block = 1
        for offset in range(0, len(content) + 1, 512):
            chunk = content[offset:offset + 512]
            client.sendto(u16(3) + u16(block) + chunk, address)
            packet, address = client.recvfrom(2048)
            assert packet == u16(4) + u16(block), f"Expected ACK({block})"
            if len(chunk) < 512:
                return;
            block = (block + 1) & 0xFFFF
    raise AssertionError("Upload did not finish")

def check_bad_name(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(4)
        client.sendto(request(1,"../secret"), ("127.0.0.1", port))
        packet, _ = client.recvfrom(2048)
        assert packet[:4] == u16(5) + u16(2), "Expected access violation"

def check_retransmission(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
        client.settimeout(4)
        client.sendto(request(1, "retry.bin"), ("127.0.0.1", port))
        first, address = client.recvfrom(2048)
        again, again_address = client.recvfrom(2048)
        assert first == again and address == again_address, "DATA was not retransmitted"
        client.sendto(u16(4) + u16(1), address)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: python3 test_tftp.py PATH_TO_TFTP_SERVER")

    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="tftp_") as temp:
        root = Path(temp)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        server = subprocess.Popen([executable, str(root), str(port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE)

        try:
            time.sleep(2.5)
            if server.poll() is not None:
                raise RuntimeError(f"Server terminated: {server.stderr.read() | r}")
            for size in (0, 1, 511, 512, 513, 1025):
                content = bytes(i % 256 for i in range(size))
                name = f"read{size}.bin"
                (root/name).write_bytes(content)
                assert read_file(port, name) == content, f"Download mismatch: {size}"
                print(f"PASS RRQ size={size}")
            (root/"retry.bin").write_bytes(b"retry")
            check_retransmission(port)
            print("PASS RRQ retramsmission")
            check_bad_name(port)
            print("PASS path traversasl blocked")

            payload = bytes(1 % 256 for i in range(1024))
            write_file(port, "uploaded.bin", payload)
            assert (root/"uploaded.bin").read_bytes() == payload
            print("PASS WRQ size=1024 (includes empty final block)")
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()

if __name__ == "__main__":
    main()
