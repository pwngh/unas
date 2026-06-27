# Client examples

Small, dependency-light clients for the unas HTTP API — one in Python, one in
TypeScript. Each is a single file you can copy into a project; both cover the
whole API (list, read, byte ranges, write, atomic create, compare-and-swap,
mkdir, move, copy, delete) with streaming for large files, a request timeout,
percent-encoded paths, and a typed error for every failure.

Run them on a server runtime (Python, Node, Bun, Deno) — **not the browser**:
the daemon sends no CORS headers, so a page on another origin is blocked before
the request even leaves.

Point a client at the daemon with the **Base URL** and **bearer token** from the
companion's copy buttons (or your `UNAS_STATE` files):

```sh
export UNAS_BASE="http://127.0.0.1:8088"
export UNAS_TOKEN="…"
```

## Python — `python/client.py`

Standard library only; nothing to install.

```python
import os
from client import UnasClient            # examples/python/client.py

client = UnasClient(os.environ["UNAS_BASE"], os.environ["UNAS_TOKEN"])
client.make_directory("Photos/2026")
client.upload_from("Photos/2026/cat.jpg", "cat.jpg")     # streams from disk
print([entry["name"] for entry in client.list_directory("Photos/2026")])
client.download_to("Photos/2026/cat.jpg", "copy.jpg")    # streams to disk
```

## TypeScript — `typescript/client.ts`

Uses Node's built-in `http` (see the note atop the file for why, not fetch). No
runtime dependencies; the dev dependencies are only for type-checking.

```sh
cd typescript
npm install            # typescript + @types/node, for `npm run typecheck`
npm run typecheck
```

```ts
import { UnasClient } from "./client.ts";

const client = new UnasClient(process.env.UNAS_BASE!, process.env.UNAS_TOKEN!);
await client.makeDirectory("Photos/2026");
await client.uploadFrom("Photos/2026/cat.jpg", "cat.jpg");    // streams from disk
console.log((await client.listDirectory("Photos/2026")).map((entry) => entry.name));
await client.downloadTo("Photos/2026/cat.jpg", "copy.jpg");   // streams to disk
```

Run a `.ts` file directly with Node 22.6+ (`node --experimental-strip-types app.ts`),
Bun (`bun app.ts`), Deno (`deno run -A app.ts`), or `npx tsx app.ts`.
