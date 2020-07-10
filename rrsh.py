#!/usr/bin/env python3

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass, field
import errno
import fcntl
import os
import pty
import selectors
import signal
import socket
import subprocess
import sys
import termios
import tty
from typing import Any, Iterator, Set, Union, Protocol


DEFAULT_PORT = 32254  # rsh uses port 514, this is 2**15 - 514 :)


class HasFileno(Protocol):
    def fileno(self) -> int:
        ...


FileDescriptorLike = Union[int, HasFileno]


@contextmanager
def raw_stdio() -> Iterator[None]:
    try:
        old_stdin_attr = termios.tcgetattr(sys.stdin)
        stdin_isatty = True
    except termios.error:
        stdin_isatty = False
    old_stdin_flags = fcntl.fcntl(sys.stdin, fcntl.F_GETFD)
    old_stdout_flags = fcntl.fcntl(sys.stdout, fcntl.F_GETFD)
    try:
        if stdin_isatty:
            tty.setraw(sys.stdin)
        fcntl.fcntl(sys.stdin, fcntl.F_SETFD, old_stdin_flags | os.O_NONBLOCK)
        fcntl.fcntl(sys.stdout, fcntl.F_SETFD, old_stdout_flags | os.O_NONBLOCK)
        yield
    finally:
        fcntl.fcntl(sys.stdout, fcntl.F_SETFD, old_stdout_flags)
        fcntl.fcntl(sys.stdin, fcntl.F_SETFD, old_stdin_flags)
        if stdin_isatty:
            termios.tcsetattr(sys.stdin, termios.TCSAFLUSH, old_stdin_attr)


class Multiplexer:
    @dataclass(eq=False, frozen=True)
    class File:
        fileobj: FileDescriptorLike
        sinks: Set[Multiplexer.File] = field(default_factory=set, init=False)
        buf: bytearray = field(default_factory=bytearray, init=False)

    def __init__(self, master: FileDescriptorLike) -> None:
        # EpollSelector doesn't allow regular files.
        self._sel = selectors.PollSelector()
        self._master = Multiplexer.File(master)
        self._files = {master: self._master}
        self._pendingbufs: Set[Multiplexer.File] = set()

    def close(self) -> None:
        self._sel.close()

    def __enter__(self) -> Multiplexer:
        return self

    def __exit__(self, exc_type: Any, exc_value: Any, traceback: Any) -> None:
        self.close()

    def _modify(self, file: Multiplexer.File) -> None:
        events = 0
        if file.sinks:
            events |= selectors.EVENT_READ
        if file.buf:
            events |= selectors.EVENT_WRITE
        if events:
            try:
                old_events = self._sel.get_key(file.fileobj)
            except KeyError:
                self._sel.register(file.fileobj, events, file)
            else:
                self._sel.modify(file.fileobj, events, file)
        else:
            try:
                self._sel.unregister(file.fileobj)
            except KeyError:
                pass

    def splice(
        self, infileobj: FileDescriptorLike, outfileobj: FileDescriptorLike
    ) -> None:
        try:
            infile = self._files[infileobj]
        except KeyError:
            infile = Multiplexer.File(infileobj)
            self._files[infileobj] = infile
        try:
            outfile = self._files[outfileobj]
        except KeyError:
            outfile = Multiplexer.File(outfileobj)
            self._files[outfileobj] = outfile
        infile = self._files[infileobj]
        infile.sinks.add(outfile)
        self._modify(infile)

    def run(self) -> None:
        modify = set()
        while self._master.sinks or self._pendingbufs:
            for key, mask in self._sel.select():
                file: Multiplexer.File = key.data
                if mask & selectors.EVENT_READ:
                    try:
                        buf = os.read(key.fd, 4096)
                    except OSError as e:
                        if e.errno == errno.EIO:
                            buf = b""
                        else:
                            raise
                    if buf:
                        for sink in file.sinks:
                            sink.buf.extend(buf)
                            self._pendingbufs.add(sink)
                            modify.add(sink)
                    else:
                        file.sinks.clear()
                        modify.add(file)
                if mask & selectors.EVENT_WRITE:
                    written = os.write(key.fd, file.buf)
                    del file.buf[:written]
                    if not file.buf:
                        self._pendingbufs.remove(file)
                        modify.add(file)
            for file in modify:
                self._modify(file)
            modify.clear()


def server(args: argparse.Namespace) -> None:
    tokens = args.address.rsplit(":", 1)
    host = tokens[0] if len(tokens) > 0 else ""
    port = tokens[1] if len(tokens) > 1 else ""
    family, _, _, _, address = socket.getaddrinfo(
        host or "::", port or DEFAULT_PORT, proto=socket.IPPROTO_TCP
    )[0]
    with socket.create_server(
        address,
        family=family,
        reuse_port=True,
        dualstack_ipv6=(family == socket.AF_INET6),
    ) as ssock:
        if args.verbose:
            print(f"Listening on {ssock.getsockname()}", file=sys.stderr)
        while True:
            sock, peername = ssock.accept()
            with sock:
                sock.setblocking(False)
                if args.verbose:
                    print(f"Connection from {peername}", file=sys.stderr)
                with raw_stdio(), Multiplexer(sock) as multiplexer:
                    multiplexer.splice(sock, sys.stdout)
                    multiplexer.splice(sys.stdin, sock)
                    multiplexer.run()
            if args.verbose:
                print(f"Disconnected from {peername}", file=sys.stderr)
            if not args.keep_open:
                break


def client(args: argparse.Namespace) -> None:
    tokens = args.address.rsplit(":", 1)
    host = tokens[0] if len(tokens) > 0 else ""
    port = tokens[1] if len(tokens) > 1 else ""
    if not args.command:
        args.command = ["sh", "-i"]
    with socket.create_connection((host or None, port or DEFAULT_PORT)) as sock:
        peername = sock.getpeername()
        if args.verbose:
            print(f"Connected to {peername}", file=sys.stderr)
        pid, master = pty.fork()
        if pid == 0:
            os.execvp(args.command[0], args.command)
        try:
            flags = fcntl.fcntl(master, fcntl.F_GETFD)
            fcntl.fcntl(master, fcntl.F_SETFD, flags | os.O_NONBLOCK)
            sock.setblocking(False)
            with raw_stdio(), Multiplexer(master) as multiplexer:
                multiplexer.splice(master, sock)
                multiplexer.splice(sock, master)
                multiplexer.splice(master, sys.stdout)
                multiplexer.splice(sys.stdin, master)
                multiplexer.run()
        finally:
            os.close(master)
            wstatus = os.waitpid(pid, 0)[1]
    if args.verbose:
        print(f"Disconnected from {peername}", file=sys.stderr)
        if os.WIFEXITED(wstatus):
            print(f"Command exited with status {os.WEXITSTATUS(wstatus)}")
        else:
            try:
                signame = signals.Signals(os.WTERMSIG(wstatus)).name
            except ValueError:
                signame = str(os.WTERMSIG(wstatus))
            print(f"Command was terminated by signal {signame}")
    if os.WIFEXITED(wstatus):
        sys.exit(os.WEXITSTATUS(wstatus))
    else:
        sys.exit(128 + os.WTERMSIG(wstatus))


def main() -> None:
    parser = argparse.ArgumentParser(description="Reverse remote shell")
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="log extra information to standard error",
    )

    subparsers = parser.add_subparsers(
        title="mode", dest="mode", description="mode to run in", required=True
    )

    parser_server = subparsers.add_parser("server", help="listen for client connection")
    parser_server.add_argument(
        "address",
        metavar="[address][:[port]]",
        default="",
        nargs="?",
        help=f"address (default: any) and port (default: {DEFAULT_PORT}) to listen on",
    )
    parser_server.add_argument(
        "-k",
        "--keep-open",
        action="store_true",
        help="keep listening after a client disconnects",
    )
    parser_server.set_defaults(func=server)

    parser_client = subparsers.add_parser(
        "client", help="run command and connect to server"
    )
    parser_client.add_argument(
        "address",
        metavar="[address][:[port]]",
        default="",
        help=f"address and port (default: {DEFAULT_PORT}) to connect to",
    )
    parser_client.add_argument(
        "command", nargs=argparse.REMAINDER, help="command to run (default: sh -i)"
    )
    parser_client.set_defaults(func=client)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
