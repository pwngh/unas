//! unas companion — tray/menu-bar app: first-run setup + live status.
//!
//! Two jobs:
//!   1. Onboard. With nothing configured it walks the user through mounting
//!      a share (handed to the OS), pointing unas at it, and starting the
//!      daemon — then remembers the choice.
//!   2. Watch. Once configured it supervises `unasd` (starts it, restarts it
//!      if it dies, stops it on quit) and reports live status.
//!
//! The daemon owns all real state. It generates the bearer token and writes
//! unas.token (0600); the companion reads it only to display and copy it,
//! never persists it, and drops the daemon's token-bearing stdout so the
//! secret can't reach unasd.log. It never modifies the share — it only
//! launches the daemon and reads its HTTP API. HTTP runs here in Rust, not
//! the webview, to avoid CORS.

use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tauri::{
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Manager, PhysicalPosition, RunEvent, State,
};
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind};

/// What we remember between launches: which folder to serve and where the
/// daemon program lives. The port and token are deliberately NOT saved here —
/// the daemon makes fresh ones each run and writes them to its own files, so the
/// secret never sits in our config.
#[derive(Serialize, Deserialize, Clone)]
struct Config {
    root: String,
    unasd_path: String,
    #[serde(default)]
    smb_url: Option<String>,
}

/// A handle to the running unasd process, kept behind a lock so the app can
/// check whether it's still alive and stop it on quit. Empty until we start one.
#[derive(Default)]
struct Daemon(Arc<Mutex<Option<Child>>>);

struct Endpoint {
    base: String,
    token: String,
}

// ---- paths & config ----

/// The companion's own folder (on macOS, ~/Library/Application Support/…),
/// created if it isn't there yet. We keep config.json here and point the daemon
/// at it too, so its port/token files land somewhere we already know to read.
fn app_dir(app: &AppHandle) -> Result<PathBuf, String> {
    let dir = app.path().app_config_dir().map_err(|e| e.to_string())?;
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    Ok(dir)
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    Ok(app_dir(app)?.join("config.json"))
}

fn load_config(app: &AppHandle) -> Option<Config> {
    let p = config_path(app).ok()?;
    let s = std::fs::read_to_string(p).ok()?;
    serde_json::from_str(&s).ok()
}

fn save_config(app: &AppHandle, cfg: &Config) -> Result<(), String> {
    let p = config_path(app)?;
    let s = serde_json::to_string_pretty(cfg).map_err(|e| e.to_string())?;
    std::fs::write(p, s).map_err(|e| e.to_string())
}

fn read_trim(p: &Path) -> Option<String> {
    std::fs::read_to_string(p)
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
}

/// Work out the daemon's address and the token needed to talk to it. First
/// honor UNAS_BASE/UNAS_TOKEN if someone set them by hand (this is how a Windows
/// box points at a remote daemon); otherwise read the port and token the daemon
/// itself dropped in its state folder.
fn resolve_endpoint(app: &AppHandle) -> Option<Endpoint> {
    if let (Ok(base), Ok(token)) = (std::env::var("UNAS_BASE"), std::env::var("UNAS_TOKEN")) {
        if !base.is_empty() && !token.is_empty() {
            return Some(Endpoint { base, token });
        }
    }
    let dir = std::env::var_os("UNAS_STATE")
        .map(PathBuf::from)
        .or_else(|| app_dir(app).ok())?;
    let port = read_trim(&dir.join("unas.port"))?;
    let token = read_trim(&dir.join("unas.token"))?;
    Some(Endpoint {
        base: format!("http://127.0.0.1:{port}"),
        token,
    })
}

fn get_json(url: &str, token: &str) -> Result<Value, String> {
    ureq::get(url)
        .set("Authorization", &format!("Bearer {token}"))
        .timeout(Duration::from_millis(1500))
        .call()
        .map_err(|e| e.to_string())?
        .into_json::<Value>()
        .map_err(|e| e.to_string())
}

// ---- daemon supervision ----

/// Spawn the daemon if it isn't already alive. Port 0 lets the kernel pick a
/// free port (written to unas.port); no --token means the daemon generates
/// one. Both land in the app's state dir, which is also where we read them.
fn ensure_daemon(app: &AppHandle, cfg: &Config, daemon: &Arc<Mutex<Option<Child>>>) -> Result<(), String> {
    let mut guard = daemon.lock().map_err(|_| "lock".to_string())?;
    if let Some(child) = guard.as_mut() {
        if matches!(child.try_wait(), Ok(None)) {
            return Ok(()); // still running
        }
    }
    // Prefer the bundled daemon: it survives app upgrades even if the saved
    // path goes stale, and keeps the daemon version matched to the companion.
    let bin = bundled_unasd().unwrap_or_else(|| cfg.unasd_path.clone());
    if !Path::new(&bin).is_file() {
        return Err(format!("unasd not found at {bin}"));
    }
    if !Path::new(&cfg.root).is_dir() {
        return Err(format!("share folder not found: {}", cfg.root));
    }
    let dir = app_dir(app)?;
    // Drop stdout: the daemon prints PORT=/TOKEN= there and the token is a 0600
    // secret — capturing it would copy it into a world-readable (0644 under the
    // usual umask) log. We read port/token from the 0600 state files instead.
    // Keep stderr: it's the human-readable serving line, no secret.
    let err = std::fs::File::create(dir.join("unasd.log")).map_err(|e| e.to_string())?;
    let child = Command::new(&bin)
        .arg("--port")
        .arg("0")
        .arg(&cfg.root)
        .env("UNAS_STATE", &dir)
        .stdout(Stdio::null())
        .stderr(Stdio::from(err))
        .spawn()
        .map_err(|e| format!("could not start unasd: {e}"))?;
    *guard = Some(child);
    Ok(())
}

fn kill_daemon(daemon: &Arc<Mutex<Option<Child>>>) {
    if let Ok(mut guard) = daemon.lock() {
        if let Some(mut child) = guard.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

// ---- commands ----

/// Decides which screen to show outside the wizard: setup, connecting/error,
/// or running. The wizard is client-side (main.js pauses polling while open).
#[tauri::command]
fn app_state(app: AppHandle, daemon: State<Daemon>) -> Value {
    let cfg = load_config(&app);
    let env_set = std::env::var("UNAS_BASE").is_ok() || std::env::var("UNAS_STATE").is_ok();

    if cfg.is_none() && !env_set {
        return json!({ "mode": "setup" });
    }

    // Already reachable? Report running.
    if let Some(ep) = resolve_endpoint(&app) {
        if let Ok(status) = get_json(&format!("{}/v1/status", ep.base), &ep.token) {
            let shares = get_json(&format!("{}/v1/shares", ep.base), &ep.token).ok();
            return json!({
                "mode": "running", "base": ep.base, "token": ep.token,
                "status": status, "shares": shares,
                "smb": cfg.as_ref().and_then(|c| c.smb_url.clone()),
            });
        }
    }

    // We own a config but it's not up — (re)start and report "connecting".
    if let Some(c) = &cfg {
        match ensure_daemon(&app, c, &daemon.0) {
            Ok(()) => json!({ "mode": "connecting", "root": c.root }),
            Err(e) => json!({ "mode": "error", "error": e, "root": c.root }),
        }
    } else {
        json!({ "mode": "connecting" })
    }
}

/// Hand the share URL to the OS so its native dialog mounts it.
#[tauri::command]
fn open_smb(url: String) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    let (prog, arg) = ("open", url);
    #[cfg(target_os = "windows")]
    let (prog, arg) = ("explorer", url.replace("smb://", "\\\\").replace('/', "\\"));
    #[cfg(all(unix, not(target_os = "macos")))]
    let (prog, arg) = ("xdg-open", url);

    Command::new(prog)
        .arg(arg)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// Reveal a local folder (the served share) in the file manager.
#[tauri::command]
fn open_path(path: String) -> Result<(), String> {
    #[cfg(target_os = "macos")]
    let prog = "open";
    #[cfg(target_os = "windows")]
    let prog = "explorer";
    #[cfg(all(unix, not(target_os = "macos")))]
    let prog = "xdg-open";

    Command::new(prog)
        .arg(path)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

/// Native folder picker (for choosing the mounted share to serve).
#[tauri::command]
async fn pick_folder(app: AppHandle) -> Option<String> {
    let a = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        a.dialog()
            .file()
            .blocking_pick_folder()
            .and_then(|p| p.into_path().ok())
            .map(|p| p.to_string_lossy().into_owned())
    })
    .await
    .ok()
    .flatten()
}

/// Native file picker (for locating the unasd binary when we can't find it).
#[tauri::command]
async fn pick_file(app: AppHandle) -> Option<String> {
    let a = app.clone();
    tauri::async_runtime::spawn_blocking(move || {
        a.dialog()
            .file()
            .blocking_pick_file()
            .and_then(|p| p.into_path().ok())
            .map(|p| p.to_string_lossy().into_owned())
    })
    .await
    .ok()
    .flatten()
}

/// The daemon shipped inside the app bundle — a Tauri sidecar, which Tauri
/// places next to the companion executable. Present in a packaged build,
/// absent under `cargo` / `tauri dev` (then we fall back to a system unasd).
fn bundled_unasd() -> Option<String> {
    let exe = std::env::current_exe().ok()?;
    let name = if cfg!(windows) { "unasd.exe" } else { "unasd" };
    let cand = exe.parent()?.join(name);
    cand.is_file().then(|| cand.to_string_lossy().into_owned())
}

/// Find a daemon to run: the bundled sidecar first (self-contained and version-
/// matched), then a saved path, then PATH, then common install dirs.
#[tauri::command]
fn locate_unasd(app: AppHandle) -> Option<String> {
    if let Some(b) = bundled_unasd() {
        return Some(b);
    }
    if let Some(c) = load_config(&app) {
        if Path::new(&c.unasd_path).is_file() {
            return Some(c.unasd_path);
        }
    }
    #[cfg(unix)]
    if let Ok(out) = Command::new("sh").arg("-c").arg("command -v unasd").output() {
        if out.status.success() {
            let p = String::from_utf8_lossy(&out.stdout).trim().to_string();
            if !p.is_empty() {
                return Some(p);
            }
        }
    }
    for p in ["/usr/local/bin/unasd", "/opt/homebrew/bin/unasd"] {
        if Path::new(p).is_file() {
            return Some(p.to_string());
        }
    }
    None
}

/// Save the config and kick off the daemon. Returns immediately; the UI keeps
/// polling app_state() and flips to "running" once /v1/status answers.
#[tauri::command]
fn start_serving(
    app: AppHandle,
    daemon: State<Daemon>,
    root: String,
    unasd_path: String,
    smb_url: Option<String>,
) -> Result<(), String> {
    if !Path::new(&root).is_dir() {
        return Err(format!("not a folder: {root}"));
    }
    if !Path::new(&unasd_path).is_file() {
        return Err(format!("unasd not found: {unasd_path}"));
    }
    let cfg = Config { root, unasd_path, smb_url };
    save_config(&app, &cfg)?;
    kill_daemon(&daemon.0);
    ensure_daemon(&app, &cfg, &daemon.0)
}

/// Stop the daemon and forget the setup (back to the wizard).
#[tauri::command]
fn disconnect(app: AppHandle, daemon: State<Daemon>) -> Result<(), String> {
    kill_daemon(&daemon.0);
    if let Ok(p) = config_path(&app) {
        let _ = std::fs::remove_file(p);
    }
    Ok(())
}

/// macOS/Linux can host the POSIX daemon (fork, POSIX sockets); Windows can't,
/// so there it's a remote client only and the wizard offers a connect path.
#[tauri::command]
fn host_can_serve() -> bool {
    cfg!(any(target_os = "macos", target_os = "linux"))
}

// ---- window / tray ----

/// "Reconfigure" is a big lever: it stops the file server and throws away the
/// share you set up. Since it sits one stray click away in a menu, we ask first
/// — the way emptying the Trash asks first — and tear nothing down unless you
/// say yes. The question is asked without freezing the menu, so the real work
/// waits for your answer in the callback (do_reconfigure).
fn reconfigure(app: &AppHandle) {
    let app = app.clone();
    app.dialog()
        .message("This stops the unas server and forgets the share you set up. You'll go back to the setup screen.")
        .title("Reconfigure unas?")
        .kind(MessageDialogKind::Warning)
        .buttons(MessageDialogButtons::OkCancelCustom(
            "Reconfigure".to_string(),
            "Cancel".to_string(),
        ))
        .show(move |confirmed| {
            if confirmed {
                do_reconfigure(&app);
            }
        });
}

/// The teardown, reached only after you confirm. Deleting the saved config is
/// the trick that makes the next status check fall back to the setup wizard; we
/// also pop the window open so the reset is something you see, not a silent one.
fn do_reconfigure(app: &AppHandle) {
    if let Some(daemon) = app.try_state::<Daemon>() {
        kill_daemon(&daemon.0);
    }
    if let Ok(p) = config_path(app) {
        let _ = std::fs::remove_file(p);
    }
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.show();
        let _ = w.set_focus();
    }
}

fn toggle_window(app: &AppHandle, anchor: Option<PhysicalPosition<f64>>) {
    let Some(win) = app.get_webview_window("main") else {
        return;
    };
    if win.is_visible().unwrap_or(false) {
        let _ = win.hide();
        return;
    }
    if let Some(pos) = anchor {
        if let Ok(size) = win.outer_size() {
            let x = (pos.x - size.width as f64 / 2.0).max(8.0);
            let _ = win.set_position(PhysicalPosition::new(x, pos.y + 8.0));
        }
    }
    let _ = win.show();
    let _ = win.set_focus();
}

pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(Daemon::default())
        .invoke_handler(tauri::generate_handler![
            app_state,
            open_smb,
            open_path,
            pick_folder,
            pick_file,
            locate_unasd,
            start_serving,
            disconnect,
            host_can_serve
        ])
        .setup(|app| {
            // Menu-bar / tray utility: keep it out of the Dock and app
            // switcher (the equivalent of LSUIElement / agent app).
            #[cfg(target_os = "macos")]
            app.set_activation_policy(tauri::ActivationPolicy::Accessory);

            let show = MenuItem::with_id(app, "toggle", "Show / hide unas", true, None::<&str>)?;
            let reconfig_item = MenuItem::with_id(app, "reconfigure", "Reconfigure…", true, None::<&str>)?;
            let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&show, &reconfig_item, &quit])?;

            TrayIconBuilder::with_id("unas-tray")
                .icon(app.default_window_icon().cloned().unwrap())
                .tooltip("unas")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "toggle" => toggle_window(app, None),
                    "reconfigure" => reconfigure(app),
                    "quit" => app.exit(0),
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        position,
                        ..
                    } = event
                    {
                        toggle_window(tray.app_handle(), Some(position));
                    }
                })
                .build(app)?;
            Ok(())
        })
        .build(tauri::generate_context!())
        .expect("error while building unas companion")
        .run(|app, event| {
            if let RunEvent::Exit = event {
                if let Some(daemon) = app.try_state::<Daemon>() {
                    kill_daemon(&daemon.0);
                }
            }
        });
}
