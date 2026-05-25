import argparse
import http.client
import os
import socket
import sys
from typing import cast


def resolve_host(host: str, port: int) -> str:
    infos = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    if not infos:
        raise OSError(f"Could not resolve {host}")
    sockaddr = infos[0][4]
    return cast(str, sockaddr[0])


def infer_endpoint(image_path: str, filesystem_partition: str) -> str:
    image_name = os.path.basename(image_path).lower()
    if image_name == "firmware.bin":
        return "/api/dev/ota"
    if image_name.endswith(".bin"):
        return f"/api/dev/fs/{filesystem_partition}"
    return "/api/dev/ota"


def upload_image(host: str, port: int, image_path: str, filesystem_partition: str) -> int:
    image_size = os.path.getsize(image_path)
    resolved = resolve_host(host, port)
    endpoint = infer_endpoint(image_path, filesystem_partition)
    print(f"Resolved {host} -> {resolved}")
    print(f"Uploading {image_path} ({image_size} bytes) to http://{host}:{port}{endpoint}")

    connection = http.client.HTTPConnection(host, port=port, timeout=15)
    connection.connect()
    connection.putrequest("POST", endpoint)
    connection.putheader("Content-Type", "application/octet-stream")
    connection.putheader("Content-Length", str(image_size))
    connection.endheaders()

    sent = 0
    with open(image_path, "rb") as firmware:
        while True:
            chunk = firmware.read(4096)
            if not chunk:
                break
            connection.send(chunk)
            sent += len(chunk)
            progress = (sent / image_size) * 100 if image_size else 100.0
            print(f"\rSent {sent}/{image_size} bytes ({progress:5.1f}%)", end="", flush=True)

    print()
    response = connection.getresponse()
    body = response.read().decode("utf-8", errors="replace")
    print(f"HTTP {response.status} {response.reason}")
    print(body)
    connection.close()
    return 0 if 200 <= response.status < 300 else 1


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload EZQ firmware over HTTP OTA.")
    parser.add_argument("--host", default="ezq-ctlr-b-dev.local", help="Device hostname or IP")
    parser.add_argument("--port", type=int, default=8032, help="OTA HTTP port")
    parser.add_argument(
        "--filesystem-partition",
        default="settings",
        help="Partition label to target for filesystem uploads",
    )
    parser.add_argument("--image", required=True, help="Path to firmware binary")
    args = parser.parse_args()

    try:
        return upload_image(args.host, args.port, args.image, args.filesystem_partition)
    except OSError as exc:
        print(f"Upload failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
