/**
 * Client for the unas file daemon (Node, Bun, or Deno — not the browser).
 *
 * It uses Node's built-in http/https rather than fetch on purpose: the daemon
 * answers one request per connection and then closes it, and that close-
 * delimited style trips an intermittent assertion in fetch's connection-pooling
 * stack. node:http handles it cleanly and streams bodies naturally. (Not the
 * browser regardless: the daemon sends no CORS headers, so a page on another
 * origin is blocked before the request leaves.)
 *
 * Point it at the daemon with the Base URL and bearer token from the companion.
 * Whole-file helpers (readFile / writeFile) keep the bytes in memory; the
 * streaming helpers (downloadTo / uploadFrom) move them a block at a time, so a
 * multi-gigabyte file never has to fit in RAM.
 */
import { createReadStream, createWriteStream } from "node:fs";
import { stat } from "node:fs/promises";
import { request as httpRequest, type IncomingMessage } from "node:http";
import { request as httpsRequest } from "node:https";
import { Readable } from "node:stream";
import { pipeline } from "node:stream/promises";

export interface DirectoryEntry {
  name: string;
  type: "file" | "dir" | "other";
  size?: number;
  mtime: string;
  etag: string;
}

export interface Status {
  version: string;
  addr: string;
  port: number;
  root: string;
  mounted: boolean;
  writable: boolean;
  uptime_s: number;
}

export interface Share {
  name: string;
  path: string;
  total_bytes: number | null;
  free_bytes?: number;
  avail_bytes?: number;
  scope: string;
}

export interface Metadata {
  etag: string;
  size: number | null;
  lastModified: string | null;
  contentType: string | null;
}

/**
 * A request the daemon refused. `code` is a stable label to branch on: a C
 * errno name for filesystem errors (ENOENT, EACCES, EEXIST, ENOSPC, ...), a
 * protocol name for HTTP ones (EAUTH, EMETHOD, EMISMATCH, EBUSY, ...), or
 * "ECONN" when the request never reached the daemon at all.
 */
export class UnasError extends Error {
  readonly code: string;
  readonly httpStatus: number;

  constructor(code: string, message: string, httpStatus: number) {
    super(`${code} (${httpStatus}): ${message}`);
    this.name = "UnasError";
    this.code = code;
    this.httpStatus = httpStatus;
  }
}

type RequestBody = Buffer | Uint8Array | string | Readable;

export class UnasClient {
  private readonly baseUrl: string;
  private readonly bearerToken: string;
  private readonly timeoutMs: number;

  constructor(baseUrl: string, bearerToken: string, timeoutMs = 30_000) {
    this.baseUrl = baseUrl.replace(/\/+$/, "");
    this.bearerToken = bearerToken;
    this.timeoutMs = timeoutMs;
  }

  // -- transport --------------------------------------------------------------

  /** Open a request and resolve once the response head arrives, handing back
   *  the live response stream. A failure to connect (refused, DNS, timeout)
   *  becomes UnasError("ECONN"). */
  private open(
    method: string,
    subpath: string,
    body?: RequestBody,
    extraHeaders?: Record<string, string>,
  ): Promise<IncomingMessage> {
    return new Promise((resolve, reject) => {
      const url = new URL(this.baseUrl + subpath);
      const sendRequest = url.protocol === "https:" ? httpsRequest : httpRequest;
      const headers: Record<string, string> = {
        Authorization: `Bearer ${this.bearerToken}`,
        ...extraHeaders,
      };
      const request = sendRequest(
        url,
        { method, headers, signal: AbortSignal.timeout(this.timeoutMs) },
        resolve,
      );
      request.on("error", (cause) => reject(new UnasError("ECONN", cause.message, 0)));

      if (body === undefined) {
        request.end();
      } else if (body instanceof Readable) {
        // Streaming upload: the caller sets Content-Length, so node frames the
        // body by length instead of chunking it (which the daemon rejects).
        body.on("error", (cause) => request.destroy(cause));
        body.pipe(request);
      } else {
        // Buffer / Uint8Array / string: node sets Content-Length from its size.
        request.end(body);
      }
    });
  }

  /** Read a response body fully into a Buffer. */
  private static collect(response: IncomingMessage): Promise<Buffer> {
    return new Promise((resolve, reject) => {
      const chunks: Buffer[] = [];
      response.on("data", (chunk: Buffer) => chunks.push(chunk));
      response.on("end", () => resolve(Buffer.concat(chunks)));
      response.on("error", reject);
    });
  }

  private static header(response: IncomingMessage, name: string): string | null {
    const value = response.headers[name];
    if (value === undefined) return null;
    return Array.isArray(value) ? (value[0] ?? null) : value;
  }

  /** Send a request and check the status. A 4xx/5xx becomes a UnasError built
   *  from the daemon's JSON error envelope; otherwise the live response is
   *  returned for the caller to read, stream, or drain. */
  private async send(
    method: string,
    subpath: string,
    body?: RequestBody,
    extraHeaders?: Record<string, string>,
  ): Promise<IncomingMessage> {
    const response = await this.open(method, subpath, body, extraHeaders);
    const status = response.statusCode ?? 0;
    if (status >= 400) {
      const payload = await UnasClient.collect(response);
      let code = "EIO";
      let message = "request failed";
      try {
        const envelope = (JSON.parse(payload.toString() || "{}") as {
          error?: { code?: string; message?: string };
        }).error;
        if (envelope) {
          code = envelope.code ?? code;
          message = envelope.message ?? message;
        }
      } catch {
        // non-JSON or empty error body — keep the defaults
      }
      throw new UnasError(code, message, status);
    }
    return response;
  }

  /** Send a request whose body we don't read, draining it so the connection is
   *  released. */
  private async complete(
    method: string,
    subpath: string,
    body?: RequestBody,
    extraHeaders?: Record<string, string>,
  ): Promise<void> {
    const response = await this.send(method, subpath, body, extraHeaders);
    response.resume();
  }

  private fileLocation(path: string): string {
    // Encode each segment but keep the slashes, so a space, '#', '?', or '%' in
    // a name reaches the daemon intact — it percent-decodes on its end.
    return "/v1/fs/" + path.replace(/^\/+/, "").split("/").map(encodeURIComponent).join("/");
  }

  private directoryLocation(path: string): string {
    // The trailing slash marks a path as a directory, for listing and mkdir.
    return this.fileLocation(path).replace(/\/+$/, "") + "/";
  }

  // -- liveness and introspection --------------------------------------------

  /** Health check — the only route that needs no token. */
  async isAlive(): Promise<boolean> {
    const response = await this.send("GET", "/healthz");
    return (await UnasClient.collect(response)).toString().trim() === "ok";
  }

  /** Version, bind address and port, served root, whether the root is currently
   *  mounted and writable, and uptime. */
  async getStatus(): Promise<Status> {
    const response = await this.send("GET", "/v1/status");
    return JSON.parse((await UnasClient.collect(response)).toString()) as Status;
  }

  /** Capacity of the served share; pool-wide on a UNAS export. */
  async listShares(): Promise<Share[]> {
    const response = await this.send("GET", "/v1/shares");
    const data = JSON.parse((await UnasClient.collect(response)).toString()) as { shares: Share[] };
    return data.shares;
  }

  // -- reading ----------------------------------------------------------------

  /** List a folder. The trailing slash asks for a JSON listing rather than the
   *  bytes of a file by that name. */
  async listDirectory(path: string): Promise<DirectoryEntry[]> {
    const response = await this.send("GET", this.directoryLocation(path));
    const data = JSON.parse((await UnasClient.collect(response)).toString()) as {
      entries: DirectoryEntry[];
    };
    return data.entries;
  }

  /** A file's etag, size, and mtime without downloading it (HTTP HEAD). */
  async getMetadata(path: string): Promise<Metadata> {
    const response = await this.send("HEAD", this.fileLocation(path));
    response.resume(); // HEAD carries no body
    const length = UnasClient.header(response, "content-length");
    return {
      // The wire etag is quoted ("123-456"); return it bare to match a listing.
      etag: (UnasClient.header(response, "etag") ?? "").replace(/^"|"$/g, ""),
      size: length !== null ? Number(length) : null,
      lastModified: UnasClient.header(response, "last-modified"),
      contentType: UnasClient.header(response, "content-type"),
    };
  }

  /** Download a whole file into memory. For large files prefer downloadTo. */
  async readFile(path: string): Promise<Uint8Array> {
    const response = await this.send("GET", this.fileLocation(path));
    return new Uint8Array(await UnasClient.collect(response));
  }

  /** Read bytes [startByte, endByte] inclusive without fetching the whole file
   *  (the daemon answers 206). */
  async readRange(path: string, startByte: number, endByte: number): Promise<Uint8Array> {
    const response = await this.send("GET", this.fileLocation(path), undefined, {
      Range: `bytes=${startByte}-${endByte}`,
    });
    return new Uint8Array(await UnasClient.collect(response));
  }

  /** Stream a file down to disk a block at a time, so its size never bounds
   *  memory. */
  async downloadTo(path: string, destinationPath: string): Promise<void> {
    const response = await this.send("GET", this.fileLocation(path));
    await pipeline(response, createWriteStream(destinationPath));
  }

  // -- writing ----------------------------------------------------------------

  /** Create or overwrite a file from bytes in memory. Atomic on the server
   *  (temp file, then rename), so a reader never sees a partial write. For
   *  large files prefer uploadFrom. */
  async writeFile(path: string, data: Uint8Array): Promise<void> {
    await this.complete("PUT", this.fileLocation(path), Buffer.from(data));
  }

  /** Stream a local file up a block at a time. The daemon requires a Content-
   *  Length (it rejects chunked uploads), so we send the file's size and let
   *  node stream the handle straight from disk. */
  async uploadFrom(path: string, sourcePath: string): Promise<void> {
    const { size } = await stat(sourcePath);
    await this.complete("PUT", this.fileLocation(path), createReadStream(sourcePath), {
      "Content-Length": String(size),
    });
  }

  /** Create only if absent. The check and the create are one atomic step, so of
   *  two programs racing the same name exactly one wins; the rest get EEXIST. */
  async createFile(path: string, data: Uint8Array): Promise<void> {
    await this.complete("PUT", this.fileLocation(path), Buffer.from(data), {
      "If-None-Match": "*",
    });
  }

  /** Overwrite only if the file still matches expectedEtag (compare-and-swap);
   *  EMISMATCH if it changed underneath you. Bare or quoted etag. */
  async replaceIfUnchanged(path: string, data: Uint8Array, expectedEtag: string): Promise<void> {
    const quoted = `"${expectedEtag.replace(/^"|"$/g, "")}"`;
    await this.complete("PUT", this.fileLocation(path), Buffer.from(data), { "If-Match": quoted });
  }

  /** Create a folder and any missing parents (mkdir -p). The body is empty, but
   *  the daemon still wants a length, so we send zero bytes. */
  async makeDirectory(path: string): Promise<void> {
    await this.complete("PUT", this.directoryLocation(path), Buffer.alloc(0));
  }

  // -- moving and removing ----------------------------------------------------

  /** Rename within one filesystem (no copy). A cross-device move returns EXDEV
   *  — use copy then delete for that. */
  async move(sourcePath: string, destinationPath: string): Promise<void> {
    await this.complete("MOVE", this.fileLocation(sourcePath), undefined, {
      Destination: this.fileLocation(destinationPath),
    });
  }

  /** Copy to a new path; the original stays. */
  async copy(sourcePath: string, destinationPath: string): Promise<void> {
    await this.complete("COPY", this.fileLocation(sourcePath), undefined, {
      Destination: this.fileLocation(destinationPath),
    });
  }

  /** Delete a file. */
  async deleteFile(path: string): Promise<void> {
    await this.complete("DELETE", this.fileLocation(path));
  }

  /** Delete a folder and its contents. The Depth header is required — the
   *  daemon refuses to remove a non-empty folder without it. */
  async deleteDirectory(path: string): Promise<void> {
    await this.complete("DELETE", this.directoryLocation(path), undefined, { Depth: "infinity" });
  }
}
