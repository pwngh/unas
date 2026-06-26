// unas companion — the screen you see.
//
// This file stays small on purpose: it asks the Rust side "what's going on?"
// (the app_state command), shows the one screen that matches the answer — the
// setup wizard, a "starting…" spinner, an error, or the live status card — and
// repeats on a loop so what you see stays current.
//
// The one rule worth knowing: while the setup wizard is open the user is
// driving, so we stop the loop from changing the screen out from under them
// (see tick). It picks back up the moment they start the daemon.

const core = window.__TAURI__ && window.__TAURI__.core;
const inTauri = !!core;

// Browser preview (no Tauri host): fake the commands so you can click through.
// Append ?setup to the URL to preview the wizard instead of the status card.
let mockRunning = !location.search.includes("setup");
const MOCK_STATUS = {
  version: "unas/1.0", addr: "127.0.0.1", port: 8088,
  root: "/Volumes/unas/Photos", mounted: true, writable: true, uptime_s: 11520,
};
const MOCK_SHARES = { shares: [{ name: "unas", path: "/Volumes/unas/Photos",
  total_bytes: 994662584320, free_bytes: 762697428992, avail_bytes: 762697428992, scope: "pool" }] };
async function mock(cmd) {
  switch (cmd) {
    case "app_state": return mockRunning
      ? { mode: "running", base: "http://127.0.0.1:8088", token: "9f2c7b1e4a0d8c63f5e21477b9a0a3f1", status: MOCK_STATUS, shares: MOCK_SHARES }
      : { mode: "setup" };
    case "pick_folder": return "/Volumes/unas/Photos";
    case "pick_file": return "/usr/local/bin/unasd";
    case "locate_unasd": return "/usr/local/bin/unasd";
    case "start_serving": mockRunning = true; return null;
    case "host_can_serve": return true;
    default: return null;
  }
}
const call = (cmd, args) => (inTauri ? core.invoke(cmd, args) : mock(cmd, args));

let phase = "init";           // which screen we're on: init|wizard|busy|running|error|remote
let canServe = true;          // false on Windows: daemon is POSIX-only
let draft = { smb: null, root: null, unasd: null };
let current = null;           // last running payload
let revealed = false;

const $ = (id) => document.getElementById(id);

// ---- helpers ----
function gb(bytes) {
  if (bytes == null) return "—";
  const u = ["B", "KB", "MB", "GB", "TB", "PB"];
  let n = bytes, i = 0;
  while (n >= 1000 && i < u.length - 1) { n /= 1000; i++; }
  return `${n >= 100 || i === 0 ? Math.round(n) : n.toFixed(1)} ${u[i]}`;
}
function uptime(s) {
  if (s == null) return "";
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
  if (d) return `up ${d}d ${h}h`;
  if (h) return `up ${h}h ${m}m`;
  if (m) return `up ${m}m`;
  return "up <1m";
}
const masked = (t) => (t ? "•".repeat(12) + " " + t.slice(-4) : "—");

let toastTimer = null;
function toast(msg) {
  const t = $("toast");
  t.textContent = msg; t.classList.add("show");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.remove("show"), 1200);
}
async function copy(text, what) {
  try { await navigator.clipboard.writeText(text); toast(`Copied ${what}`); }
  catch { toast("Copy failed"); }
}

function showView(name) {
  for (const v of ["status", "setup", "busy", "remote"]) $("view-" + v).hidden = v !== name;
}

// ---- routing ----
// Given the Rust side's answer, show the matching screen: "running" → the status
// card, "connecting" → a spinner, "error" → the message; and if nothing's set up
// yet, the wizard — or, on Windows where the daemon can't run, the remote panel.
function route(s) {
  if (s.mode === "running") { phase = "running"; showStatus(s); }
  else if (s.mode === "connecting") { phase = "busy"; showBusy("Starting unas…", s.root || "", false); }
  else if (s.mode === "error") { phase = "error"; showBusy("Couldn’t start unas", s.error || "", true); }
  else if (canServe) { enterWizard(); }
  else { showRemote(); }
}
function showRemote() {
  showView("remote");
  phase = "remote";
  $("subtitle").textContent = "Windows — remote client";
}
// The heartbeat: every 1.5s, ask Rust what's happening and route to the right
// screen.
async function tick() {
  if (phase === "wizard") return;          // don't disturb an open wizard
  let s;
  try { s = await call("app_state"); } catch (e) { s = { mode: "error", error: String(e) }; }
  route(s);
}

// ---- status view ----
function showStatus(s) {
  current = s;
  showView("status");
  $("subtitle").textContent = (s.status && s.status.root) || "—";
  const st = s.status || {};
  $("state").textContent = "Serving";
  $("meta").textContent = [st.mounted ? "mounted" : "not mounted",
    st.writable ? "writable" : "read-only", uptime(st.uptime_s)].filter(Boolean).join(" · ");
  $("port").textContent = st.port ? ":" + st.port : "";
  $("baseurl").textContent = (s.base || "") + "/v1/fs/";
  $("ver").textContent = `${st.version || "unas"} · ${st.addr || ""}`.trim();
  $("token").textContent = revealed ? s.token : masked(s.token);
  const sh = s.shares && s.shares.shares && s.shares.shares[0];
  if (sh && sh.total_bytes) {
    const free = sh.avail_bytes != null ? sh.avail_bytes : sh.free_bytes;
    $("capnum").textContent = `${gb(free)} free · ${gb(sh.total_bytes)} total`;
    $("capbar").style.width = Math.max(0, Math.min(100, Math.round((1 - free / sh.total_bytes) * 100))) + "%";
    $("capwrap").hidden = false;
  } else $("capwrap").hidden = true;
}

// ---- busy / error view ----
function showBusy(title, msg, isError) {
  showView("busy");
  $("subtitle").textContent = "";
  $("busytitle").textContent = title;
  $("busymsg").textContent = msg;
  $("spin").hidden = isError;
  $("busyx").hidden = !isError;
  $("busyback").hidden = !isError;
}

// ---- wizard ----
function enterWizard() {
  if (phase === "wizard") return;
  phase = "wizard";
  showView("setup");
  $("subtitle").textContent = "No share connected yet";
  goStep(draft.root ? 2 : 1);
}
function goStep(n) {
  $("wiz-1").hidden = n !== 1;
  $("wiz-2").hidden = n !== 2;
  $("s2").classList.toggle("on", n === 2);
  if (n === 2) {
    $("chosen").textContent = draft.root || "—";
    locateUnasd();
  }
}
async function locateUnasd() {
  $("unasd").textContent = "looking…";
  $("unasd").className = "ell grow";
  const u = await call("locate_unasd");
  draft.unasd = u || null;
  $("unasd").textContent = u || "not found — locate it";
  $("unasd").classList.add(u ? "ok" : "err");
  $("locate").hidden = false;
  $("start").disabled = !(draft.root && draft.unasd);
}

// ---- wiring ----
$("mount").addEventListener("click", async () => {
  const url = $("smburl").value.trim();
  if (!/^(smb|nfs):\/\//.test(url)) return toast("Enter an smb:// or nfs:// address");
  draft.smb = url;
  // Await so a spawn failure surfaces instead of a misleading success toast.
  try { await call("open_smb", { url }); toast("Opening your file manager…"); }
  catch (e) { toast(String(e)); }
});
$("choose1").addEventListener("click", chooseFolder);
$("choose2").addEventListener("click", chooseFolder);
async function chooseFolder() {
  const p = await call("pick_folder");
  if (p) { draft.root = p; goStep(2); }
}
$("locate").addEventListener("click", async () => {
  const f = await call("pick_file");
  if (f) {
    draft.unasd = f;
    $("unasd").textContent = f;
    $("unasd").className = "ell grow ok";
    $("start").disabled = !(draft.root && draft.unasd);
  }
});
$("back2").addEventListener("click", () => goStep(1));
$("start").addEventListener("click", async () => {
  phase = "busy";
  showBusy("Starting unas…", "", false);
  try {
    await call("start_serving", { root: draft.root, unasdPath: draft.unasd, smbUrl: draft.smb });
  } catch (e) {
    phase = "error";
    showBusy("Couldn’t start unas", String(e), true);
  }
});
$("busyback").addEventListener("click", async () => {
  try { await call("disconnect"); } catch {}
  draft = { smb: null, root: null, unasd: null };
  phase = "init";
  tick();
});

// status actions
document.querySelectorAll("[data-copy]").forEach((b) => b.addEventListener("click", () => {
  if (!current) return;
  if (b.dataset.copy === "baseurl") copy((current.base || "") + "/v1/fs/", "base URL");
  else if (current.token) copy(current.token, "token");
}));
$("reveal").addEventListener("click", () => {
  revealed = !revealed;
  if (current && current.token) $("token").textContent = revealed ? current.token : masked(current.token);
});
$("curl").addEventListener("click", () => {
  if (current && current.token)
    copy(`curl -H "Authorization: Bearer ${current.token}" ${current.base}/v1/fs/`, "curl");
});
$("open").addEventListener("click", async () => {
  const root = current && current.status && current.status.root;
  if (!root) return;
  if (inTauri) { try { await core.invoke("open_path", { path: root }); } catch (e) { toast(String(e)); } }
  else toast("Open share (Tauri only)");
});

(async () => {
  try { canServe = await call("host_can_serve"); } catch {}
  tick();
})();
setInterval(tick, 1500);
