"""Client for the unas file daemon (Python, standard library only).

Point it at the daemon with the Base URL and bearer token from the companion
(or your UNAS_STATE files). Each method is one operation; the /v1/fs prefix,
percent-encoding, the auth header, a request timeout, the trailing-slash
listing rule, and the JSON error envelope are handled here so calling code
stays clean.

Whole-file helpers (read_file / write_file) keep the bytes in memory; the
streaming helpers (download_to / upload_from) move them a block at a time, so
a multi-gigabyte file never has to fit in RAM.
"""
from __future__ import annotations

import json
import os
import shutil
import urllib.error
import urllib.parse
import urllib.request
from http.client import HTTPResponse
from typing import Any, BinaryIO


class UnasError(Exception):
    """A request the daemon refused. ``code`` is a stable label to branch on:
    a C errno name for filesystem errors (ENOENT, EACCES, EEXIST, ENOSPC, ...),
    a protocol name for HTTP ones (EAUTH, EMETHOD, EMISMATCH, EBUSY, ...), or
    "ECONN" when the request never reached the daemon at all.
    """

    def __init__(self, code: str, message: str, http_status: int) -> None:
        super().__init__(f"{code} ({http_status}): {message}")
        self.code = code
        self.message = message
        self.http_status = http_status


class UnasClient:
    """Client for the unas file daemon."""

    def __init__(self, base_url: str, bearer_token: str, timeout: float = 30.0) -> None:
        self.base_url = base_url.rstrip("/")
        self.bearer_token = bearer_token
        self.timeout = timeout

    # -- transport -------------------------------------------------------------

    def _open(
        self,
        method: str,
        subpath: str,
        body: Any = None,
        extra_headers: dict[str, str] | None = None,
    ) -> HTTPResponse:
        """Open a response and hand it back still open, for the caller to close
        (use ``with``). A daemon error becomes a UnasError; a failure to connect
        at all becomes UnasError("ECONN").
        """
        headers = {"Authorization": f"Bearer {self.bearer_token}"}
        if extra_headers:
            headers.update(extra_headers)
        request = urllib.request.Request(
            self.base_url + subpath, data=body, method=method, headers=headers
        )
        try:
            return urllib.request.urlopen(request, timeout=self.timeout)
        except urllib.error.HTTPError as error:
            # The daemon answered, but with an error; its body is the JSON
            # envelope {"error": {"code", "http", "path", "message"}}.
            try:
                payload = error.read()
            finally:
                error.close()
            envelope: dict[str, Any] = {}
            try:
                envelope = json.loads(payload or b"{}").get("error", {})
            except ValueError:
                pass
            raise UnasError(
                envelope.get("code", "EIO"),
                envelope.get("message", "request failed"),
                error.code,
            ) from None
        except urllib.error.URLError as error:
            # Never reached the daemon: refused, DNS, or the timeout fired.
            raise UnasError("ECONN", str(error.reason), 0) from None

    def _read_bytes(
        self,
        method: str,
        subpath: str,
        body: Any = None,
        extra_headers: dict[str, str] | None = None,
    ) -> bytes:
        with self._open(method, subpath, body, extra_headers) as response:
            return response.read()

    def _read_json(self, method: str, subpath: str) -> Any:
        with self._open(method, subpath) as response:
            return json.load(response)

    @staticmethod
    def _file_location(path: str) -> str:
        # Percent-encode the segments but keep the slashes, so a space, '#',
        # '?', or '%' in a name reaches the daemon intact — it decodes on its end.
        return "/v1/fs/" + urllib.parse.quote(path.lstrip("/"), safe="/")

    @staticmethod
    def _directory_location(path: str) -> str:
        # The trailing slash is what marks a path as a directory, both for
        # listing it and for creating it.
        return UnasClient._file_location(path).rstrip("/") + "/"

    # -- liveness and introspection -------------------------------------------

    def is_alive(self) -> bool:
        """Health check — the only route that needs no token."""
        return self._read_bytes("GET", "/healthz").strip() == b"ok"

    def get_status(self) -> dict[str, Any]:
        """Version, bind address and port, served root, whether the root is
        currently mounted and writable, and uptime."""
        return self._read_json("GET", "/v1/status")

    def list_shares(self) -> list[dict[str, Any]]:
        """Capacity of the served share; pool-wide on a UNAS export."""
        return self._read_json("GET", "/v1/shares")["shares"]

    # -- reading ---------------------------------------------------------------

    def list_directory(self, directory_path: str) -> list[dict[str, Any]]:
        """List a folder. The trailing slash asks for a JSON listing rather
        than the bytes of a file by that name."""
        return self._read_json("GET", self._directory_location(directory_path))["entries"]

    def get_metadata(self, file_path: str) -> dict[str, Any]:
        """A file's etag, size, and mtime without downloading it (HTTP HEAD)."""
        with self._open("HEAD", self._file_location(file_path)) as response:
            length = response.headers.get("Content-Length")
            return {
                # The wire etag is quoted ("123-456"); return it bare to match a listing.
                "etag": (response.headers.get("ETag") or "").strip('"'),
                "size": int(length) if length is not None else None,
                "last_modified": response.headers.get("Last-Modified"),
                "content_type": response.headers.get("Content-Type"),
            }

    def read_file(self, file_path: str) -> bytes:
        """Download a whole file into memory. For large files prefer
        download_to, which streams to disk."""
        return self._read_bytes("GET", self._file_location(file_path))

    def read_range(self, file_path: str, start_byte: int, end_byte: int) -> bytes:
        """Read bytes [start_byte, end_byte] inclusive without fetching the
        whole file (the daemon answers 206)."""
        return self._read_bytes(
            "GET",
            self._file_location(file_path),
            extra_headers={"Range": f"bytes={start_byte}-{end_byte}"},
        )

    def download_to(self, file_path: str, destination: str | BinaryIO) -> None:
        """Stream a file down a block at a time, so its size never bounds memory.
        ``destination`` is a path or an already-open binary file."""
        with self._open("GET", self._file_location(file_path)) as response:
            if isinstance(destination, str):
                with open(destination, "wb") as out_file:
                    shutil.copyfileobj(response, out_file)
            else:
                shutil.copyfileobj(response, destination)

    # -- writing ---------------------------------------------------------------

    def write_file(self, file_path: str, data: bytes) -> None:
        """Create or overwrite a file from bytes in memory. Atomic on the server
        (temp file, then rename), so a reader never sees a partial write. For
        large files prefer upload_from."""
        self._read_bytes("PUT", self._file_location(file_path), body=data)

    def upload_from(self, file_path: str, source_path: str) -> None:
        """Stream a local file up a block at a time. The daemon requires a
        Content-Length (it rejects chunked uploads), so we send the file's size
        explicitly and let urllib stream the open handle."""
        size = os.path.getsize(source_path)
        with open(source_path, "rb") as handle:
            self._read_bytes(
                "PUT",
                self._file_location(file_path),
                body=handle,
                extra_headers={"Content-Length": str(size)},
            )

    def create_file(self, file_path: str, data: bytes) -> None:
        """Create only if absent. The check and the create are one atomic step,
        so of two programs racing the same name exactly one wins; the rest get
        EEXIST."""
        self._read_bytes(
            "PUT",
            self._file_location(file_path),
            body=data,
            extra_headers={"If-None-Match": "*"},
        )

    def replace_if_unchanged(self, file_path: str, data: bytes, expected_etag: str) -> None:
        """Overwrite only if the file still matches ``expected_etag`` (compare-
        and-swap); EMISMATCH if it changed underneath you. Bare or quoted etag."""
        quoted_etag = '"' + expected_etag.strip('"') + '"'
        self._read_bytes(
            "PUT",
            self._file_location(file_path),
            body=data,
            extra_headers={"If-Match": quoted_etag},
        )

    def make_directory(self, directory_path: str) -> None:
        """Create a folder and any missing parents (mkdir -p). The body is empty,
        but the daemon still wants a length, so we send zero bytes."""
        self._read_bytes("PUT", self._directory_location(directory_path), body=b"")

    # -- moving and removing ---------------------------------------------------

    def move(self, source_path: str, destination_path: str) -> None:
        """Rename within one filesystem (no copy). A cross-device move returns
        EXDEV — use copy then delete for that."""
        self._read_bytes(
            "MOVE",
            self._file_location(source_path),
            extra_headers={"Destination": self._file_location(destination_path)},
        )

    def copy(self, source_path: str, destination_path: str) -> None:
        """Copy to a new path; the original stays."""
        self._read_bytes(
            "COPY",
            self._file_location(source_path),
            extra_headers={"Destination": self._file_location(destination_path)},
        )

    def delete_file(self, file_path: str) -> None:
        """Delete a file."""
        self._read_bytes("DELETE", self._file_location(file_path))

    def delete_directory(self, directory_path: str) -> None:
        """Delete a folder and its contents. The Depth header is required — the
        daemon refuses to remove a non-empty folder without it."""
        self._read_bytes(
            "DELETE",
            self._directory_location(directory_path),
            extra_headers={"Depth": "infinity"},
        )
