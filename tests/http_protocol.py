#!/usr/bin/env python3

import socket
import sys


class RawHTTP:
    def __init__(self, port: int):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.buffer = bytearray()

    def send(self, request: bytes) -> None:
        self.socket.sendall(request)

    def response(self) -> tuple[int, dict[str, str], bytes]:
        marker = b"\r\n\r\n"
        while marker not in self.buffer:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise AssertionError("connection closed before response header")
            self.buffer.extend(chunk)
        raw_header, remainder = self.buffer.split(marker, 1)
        self.buffer = bytearray(remainder)
        lines = raw_header.decode("ascii").split("\r\n")
        status = int(lines[0].split(" ", 2)[1])
        headers: dict[str, str] = {}
        for line in lines[1:]:
            name, value = line.split(":", 1)
            headers[name.lower()] = value.strip()
        length = int(headers["content-length"])
        while len(self.buffer) < length:
            chunk = self.socket.recv(65536)
            if not chunk:
                raise AssertionError("connection closed before response body")
            self.buffer.extend(chunk)
        body = bytes(self.buffer[:length])
        del self.buffer[:length]
        return status, headers, body

    def expect_eof(self) -> None:
        if self.buffer:
            raise AssertionError(f"unexpected buffered bytes: {self.buffer!r}")
        self.socket.settimeout(1)
        if self.socket.recv(1) != b"":
            raise AssertionError("server did not close the connection")

    def close(self) -> None:
        self.socket.close()


def request(target: str, *, version: str = "HTTP/1.1", connection: str = "") -> bytes:
    headers = f"GET {target} {version}\r\nHost: localhost\r\n"
    if connection:
        headers += f"Connection: {connection}\r\n"
    return (headers + "\r\n").encode("ascii")


def assert_status(response: tuple[int, dict[str, str], bytes], expected: int) -> None:
    if response[0] != expected:
        raise AssertionError(f"expected HTTP {expected}, got {response[0]}")


def main() -> None:
    port = int(sys.argv[1])

    connection = RawHTTP(port)
    connection.send(
        request("/health")
        + request("/rpc?v=5&type=info&arg[]=alpha-one")
    )
    health = connection.response()
    info = connection.response()
    assert_status(health, 200)
    assert_status(info, 200)
    assert b"packages: 4" in health[2]
    assert b'"Version":"2.0-1"' in info[2]
    assert health[1].get("connection", "").lower() != "close"
    assert info[1].get("connection", "").lower() != "close"

    connection.send(request("/rpc?v=5&type=search&arg=tools"))
    search = connection.response()
    assert_status(search, 200)
    assert b'"Name":"beta-tools"' in search[2]

    connection.send(request("/health", connection="close"))
    closing = connection.response()
    assert_status(closing, 200)
    assert closing[1].get("connection", "").lower() == "close"
    connection.expect_eof()
    connection.close()

    http10 = RawHTTP(port)
    http10.send(request("/health", version="HTTP/1.0"))
    response10 = http10.response()
    assert_status(response10, 200)
    assert response10[1].get("connection", "").lower() == "close"
    http10.expect_eof()
    http10.close()

    http10_keep_alive = RawHTTP(port)
    http10_keep_alive.send(
        request("/health", version="HTTP/1.0", connection="Keep-Alive")
    )
    response10_keep_alive = http10_keep_alive.response()
    assert_status(response10_keep_alive, 200)
    assert (
        response10_keep_alive[1].get("connection", "").lower() == "keep-alive"
    )
    http10_keep_alive.send(
        request("/health", version="HTTP/1.0", connection="close")
    )
    assert_status(http10_keep_alive.response(), 200)
    http10_keep_alive.expect_eof()
    http10_keep_alive.close()

    body = RawHTTP(port)
    body.send(
        b"GET /health HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Content-Length: 1\r\n\r\nx"
    )
    rejected = body.response()
    assert_status(rejected, 400)
    assert rejected[1].get("connection", "").lower() == "close"
    body.expect_eof()
    body.close()


if __name__ == "__main__":
    main()
