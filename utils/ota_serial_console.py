import argparse
import socket
import sys
import threading


def recv_loop(sock: socket.socket) -> None:
    try:
        while True:
            data = sock.recv(1024)
            if not data:
                break
            sys.stdout.write(data.decode("utf-8", errors="replace"))
            sys.stdout.flush()
    except OSError:
        pass


def send_loop(sock: socket.socket) -> None:
    try:
        for line in sys.stdin:
            sock.sendall(line.encode("utf-8"))
    except OSError:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Connect to the EZQ wireless debug console.")
    parser.add_argument("--host", default="ezq-ctlr-b-dev.local", help="Device hostname or IP")
    parser.add_argument("--port", type=int, default=2323, help="Console TCP port")
    args = parser.parse_args()

    with socket.create_connection((args.host, args.port), timeout=5) as sock:
        print(f"Connected to {args.host}:{args.port}")
        receiver = threading.Thread(target=recv_loop, args=(sock,), daemon=True)
        receiver.start()
        send_loop(sock)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
