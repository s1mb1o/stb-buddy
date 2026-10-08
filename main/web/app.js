"use strict";

const THEMES = {
  light: {
    background: "#fbfbfa", foreground: "#1f2328", cursor: "#1f2328",
    selectionBackground: "#b6d6fd", black: "#24292f", red: "#cf222e",
    green: "#116329", yellow: "#4d2d00", blue: "#0969da",
    magenta: "#8250df", cyan: "#1b7c83", white: "#6e7781",
    brightBlack: "#57606a", brightRed: "#a40e26", brightGreen: "#1a7f37",
    brightYellow: "#633c01", brightBlue: "#218bff",
    brightMagenta: "#a475f9", brightCyan: "#3192aa", brightWhite: "#8c959f",
  },
  dark: {
    background: "#0f1115", foreground: "#d7dae0", cursor: "#d7dae0",
    selectionBackground: "#264f78", black: "#484f58", red: "#ff7b72",
    green: "#3fb950", yellow: "#d29922", blue: "#58a6ff",
    magenta: "#bc8cff", cyan: "#39c5cf", white: "#b1bac4",
    brightBlack: "#6e7681", brightRed: "#ffa198", brightGreen: "#56d364",
    brightYellow: "#e3b341", brightBlue: "#79c0ff",
    brightMagenta: "#d2a8ff", brightCyan: "#56d4dd", brightWhite: "#f0f6fc",
  },
};

const $ = (id) => document.getElementById(id);
const darkQuery = window.matchMedia("(prefers-color-scheme: dark)");
const theme = () => darkQuery.matches ? THEMES.dark : THEMES.light;
const terminal = new Terminal({
  scrollback: 100000,
  cursorBlink: true,
  fontFamily: 'ui-monospace, "SF Mono", Menlo, Consolas, monospace',
  fontSize: 13,
  theme: theme(),
});
const fit = new FitAddon.FitAddon();
terminal.loadAddon(fit);
terminal.open($("terminal"));
fit.fit();
terminal.focus();
new ResizeObserver(() => fit.fit()).observe($("terminal"));
darkQuery.addEventListener("change", () => { terminal.options.theme = theme(); });

let next = null;
let messageTimer = null;
let txTail = Promise.resolve();
let logRequest = null;
let logGeneration = 0;
let irNext = 0;
let uartNoteNext = 0;
let uartNoteGeneration = 0;
let latestUart = {
  port: 1, baud: 115200, data_bits: 8, parity: "none", stop_bits: 1,
};
const encoder = new TextEncoder();

function showMessage(text, timeoutMs = 5000) {
  $("message").textContent = text;
  clearTimeout(messageTimer);
  if (timeoutMs > 0) {
    messageTimer = setTimeout(() => { $("message").textContent = ""; }, timeoutMs);
  }
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
}

function uartFrame(config) {
  const parity = { none: "N", even: "E", odd: "O" }[config.parity] || "?";
  return `${config.data_bits}${parity}${config.stop_bits}`;
}

function updateUartSummary(config) {
  $("uart").textContent = `UART${config.port} · ${config.baud}`;
  $("uart").title = `Configure UART properties (${uartFrame(config)})`;
}

function queueWrite(bytes) {
  for (let offset = 0; offset < bytes.length; offset += 4096) {
    const chunk = bytes.slice(offset, offset + 4096);
    txTail = txTail.then(async () => {
      const response = await fetch("/api/uart/write", {
        method: "POST",
        headers: {
          "Content-Type": "application/octet-stream",
          "X-STB-Buddy-Interactive": "1",
        },
        body: chunk,
      });
      if (!response.ok) throw new Error(`UART write failed: HTTP ${response.status}`);
    }).catch((error) => showMessage(error.message));
  }
}

terminal.onData((data) => queueWrite(encoder.encode(data)));
terminal.onBinary((data) => queueWrite(Uint8Array.from(data, (c) => c.charCodeAt(0))));

async function pollLog() {
  const generation = logGeneration;
  const controller = new AbortController();
  logRequest = controller;
  try {
    const initial = next === null;
    const query = initial ? "max_bytes=262144" : `since=${next}&max_bytes=32768`;
    const response = await fetch(`/api/read?${query}`, {
      cache: "no-store",
      signal: controller.signal,
    });
    if (!response.ok) throw new Error(`log read failed: HTTP ${response.status}`);
    if (generation !== logGeneration) return;
    const oldest = Number(response.headers.get("X-Log-Oldest"));
    const start = Number(response.headers.get("X-Log-Start"));
    if (!initial && oldest > next) terminal.reset();
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (generation !== logGeneration) return;
    if (bytes.length) terminal.write(bytes);
    next = Number(response.headers.get("X-Log-Next"));
    if (!Number.isFinite(next)) next = start + bytes.length;
  } catch (error) {
    if (error.name !== "AbortError") showMessage(error.message);
  } finally {
    if (logRequest === controller) logRequest = null;
    setTimeout(pollLog, 150);
  }
}

async function pollStatus() {
  try {
    const response = await fetch("/health", { cache: "no-store" });
    if (!response.ok) throw new Error();
    const status = await response.json();
    const online = status.wifi.connected;
    $("link").textContent = online ? "connected" : "disconnected";
    $("link").className = `pill ${online ? "on" : "off"}`;
    $("version").textContent = `v${status.version}`;
    latestUart = status.uart;
    updateUartSummary(latestUart);
    $("wifi").textContent = `${status.wifi.ssid} · ${status.wifi.ip || "no IP"}`;
  } catch {
    $("link").textContent = "buddy offline";
    $("link").className = "pill off";
  } finally {
    setTimeout(pollStatus, 2000);
  }
}

const uartPopover = $("uart-form");

function populateUartForm() {
  $("uart-baud").value = latestUart.baud;
  $("uart-data-bits").value = latestUart.data_bits;
  $("uart-parity").value = latestUart.parity;
  $("uart-stop-bits").value = latestUart.stop_bits;
  $("uart-config-status").className = "uart-config-status";
  $("uart-config-status").textContent = "Changes last until reboot.";
}

function setUartPopoverOpen(open) {
  uartPopover.hidden = !open;
  $("uart").setAttribute("aria-expanded", String(open));
  if (open) {
    populateUartForm();
    $("uart-baud").focus();
    $("uart-baud").select();
  }
}

function setUartFormBusy(busy) {
  for (const id of [
    "uart-baud", "uart-data-bits", "uart-parity", "uart-stop-bits",
    "uart-cancel", "uart-apply",
  ]) $(id).disabled = busy;
}

$("uart").addEventListener("click", () => {
  if (!$("ir-panel").hidden) setIrPanelOpen(false);
  if (!$("help-panel").hidden) setHelpPanelOpen(false);
  setUartPopoverOpen(uartPopover.hidden);
});
$("uart-cancel").addEventListener("click", () => {
  setUartPopoverOpen(false);
  $("uart").focus();
});
$("uart-control").addEventListener("pointerdown", (event) => {
  event.stopPropagation();
});
document.addEventListener("pointerdown", () => {
  if (!uartPopover.hidden) setUartPopoverOpen(false);
});
document.addEventListener("keydown", (event) => {
  if (!uartPopover.hidden && event.key === "Escape") {
    event.preventDefault();
    setUartPopoverOpen(false);
    $("uart").focus();
  }
});

$("uart-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  const configuration = {
    baud: Number($("uart-baud").value),
    data_bits: Number($("uart-data-bits").value),
    parity: $("uart-parity").value,
    stop_bits: Number($("uart-stop-bits").value),
  };
  setUartFormBusy(true);
  $("uart-config-status").className = "uart-config-status";
  $("uart-config-status").textContent = "Applying…";
  try {
    const response = await fetch("/api/uart/configure", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(configuration),
    });
    const body = await response.json().catch(() => null);
    if (!response.ok || !body) {
      throw new Error(body?.error || `HTTP ${response.status}`);
    }
    latestUart = body;
    updateUartSummary(latestUart);
    setUartPopoverOpen(false);
    showMessage(`UART configured: ${body.baud} ${uartFrame(body)}`);
    terminal.focus();
  } catch (error) {
    $("uart-config-status").className = "uart-config-status error";
    $("uart-config-status").textContent = `Failed: ${error.message}`;
  } finally {
    setUartFormBusy(false);
  }
});

function writeAnnotation(source, message) {
  const safeSource = String(source).replace(/[\x00-\x1f\x7f-\x9f]/g, "?");
  const safeMessage = String(message).replace(/[\x00-\x1f\x7f-\x9f]/g, "?");
  const color = darkQuery.matches ? "210;168;255" : "130;80;223";
  terminal.write(
    `\r\n\x1b[1;38;2;${color}m[${safeSource}] ${safeMessage}\x1b[0m\r\n`
  );
}

async function pollIrEvents() {
  try {
    const response = await fetch(`/api/ir/events?since=${irNext}`, {
      cache: "no-store",
    });
    if (!response.ok) throw new Error(`IR event read failed: HTTP ${response.status}`);
    const body = await response.json();
    if (body.next < irNext) {
      irNext = 0;
      return;
    }
    if (irNext !== 0 && body.oldest > irNext + 1) {
      writeAnnotation(
        "stb-buddy IR",
        `${body.oldest - irNext - 1} event(s) were overwritten`
      );
    }
    for (const event of body.events) {
      writeAnnotation("stb-buddy IR", event.message);
    }
    if (Number.isSafeInteger(body.next) && body.next >= irNext) irNext = body.next;
  } catch (error) {
    showMessage(error.message);
  } finally {
    setTimeout(pollIrEvents, 500);
  }
}

async function pollUartNotes() {
  const generation = uartNoteGeneration;
  try {
    const response = await fetch(`/api/uart/events?since=${uartNoteNext}`, {
      cache: "no-store",
    });
    if (!response.ok) {
      throw new Error(`UART annotation read failed: HTTP ${response.status}`);
    }
    const body = await response.json();
    if (generation !== uartNoteGeneration) return;
    if (body.next < uartNoteNext) {
      uartNoteNext = 0;
      return;
    }
    if (uartNoteNext !== 0 && body.oldest > uartNoteNext + 1) {
      writeAnnotation(
        "stb-buddy UART",
        `${body.oldest - uartNoteNext - 1} annotation(s) were overwritten`
      );
    }
    for (const event of body.events) {
      writeAnnotation(event.source, event.message);
    }
    if (Number.isSafeInteger(body.next) && body.next >= uartNoteNext) {
      uartNoteNext = body.next;
    }
  } catch (error) {
    showMessage(error.message);
  } finally {
    setTimeout(pollUartNotes, 500);
  }
}

const irPanel = $("ir-panel");
let irRemotes = [];

function setIrStatus(kind, text) {
  $("ir-status").className = `ir-status ${kind}`;
  $("ir-status").textContent = text;
}

function setIrBusy(busy) {
  for (const id of ["ir-remote", "ir-upload", "ir-download"]) {
    $(id).disabled = busy;
  }
  for (const button of $("ir-keys").querySelectorAll("button")) {
    button.disabled = busy;
  }
}

function renderIrKeys() {
  const keys = $("ir-keys");
  keys.replaceChildren();
  const remoteIndex = Number($("ir-remote").value);
  const remote = irRemotes.find((candidate) => candidate.index === remoteIndex);
  if (!remote || remote.keys.length === 0) {
    const empty = document.createElement("p");
    empty.className = "ir-empty";
    empty.textContent = remote ? "This remote has no keys." : "No remote configuration loaded.";
    keys.append(empty);
    return;
  }
  for (const key of remote.keys) {
    const row = document.createElement("div");
    row.className = "ir-key";
    const name = document.createElement("code");
    name.textContent = key;
    name.title = key;
    const send = document.createElement("button");
    send.type = "button";
    send.textContent = "Send";
    send.addEventListener("click", async () => {
      send.disabled = true;
      setIrStatus("", `Sending ${remote.name}/${key}…`);
      try {
        const response = await fetch("/api/ir/send", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ remote: remote.index, key, repeats: 0 }),
        });
        const body = await response.json().catch(() => null);
        if (!response.ok) throw new Error(body?.error || `HTTP ${response.status}`);
        setIrStatus("ok", `Sent ${remote.name}/${key}`);
        send.textContent = "Sent";
        setTimeout(() => {
          if (send.isConnected) send.textContent = "Send";
        }, 1200);
      } catch (error) {
        setIrStatus("error", `Send failed: ${error.message}`);
      } finally {
        send.disabled = false;
      }
    });
    row.append(name, send);
    keys.append(row);
  }
}

async function refreshIrRemotes(preferredIndex = null) {
  setIrBusy(true);
  setIrStatus("", "Loading remotes…");
  try {
    const response = await fetch("/api/remotes", { cache: "no-store" });
    const body = await response.json().catch(() => null);
    if (!response.ok || !body) throw new Error(body?.error || `HTTP ${response.status}`);
    irRemotes = Array.isArray(body.remotes) ? body.remotes : [];
    const select = $("ir-remote");
    select.replaceChildren();
    for (const remote of irRemotes) {
      const option = document.createElement("option");
      option.value = remote.index;
      option.textContent = `${remote.name} · ${remote.protocol} · ${remote.keys.length} keys`;
      select.append(option);
    }
    const selected = irRemotes.some((remote) => remote.index === preferredIndex)
      ? preferredIndex
      : irRemotes[0]?.index;
    if (selected !== undefined) select.value = selected;
    renderIrKeys();
    setIrStatus("", irRemotes.length
      ? `${body.remote_count} remotes · ${body.key_count} keys`
      : "Upload a lircd.conf to add remote controls.");
  } catch (error) {
    irRemotes = [];
    $("ir-remote").replaceChildren();
    renderIrKeys();
    setIrStatus("error", `Could not load remotes: ${error.message}`);
  } finally {
    setIrBusy(false);
  }
}

function setIrPanelOpen(open) {
  irPanel.hidden = !open;
  $("ir").setAttribute("aria-expanded", String(open));
  if (open) {
    setUartPopoverOpen(false);
    setHelpPanelOpen(false);
    refreshIrRemotes(Number($("ir-remote").value));
  }
}

$("ir").addEventListener("click", () => setIrPanelOpen(irPanel.hidden));
$("ir-close").addEventListener("click", () => {
  setIrPanelOpen(false);
  $("ir").focus();
});
$("ir-remote").addEventListener("change", renderIrKeys);
$("ir-control").addEventListener("pointerdown", (event) => event.stopPropagation());
document.addEventListener("pointerdown", () => {
  if (!irPanel.hidden) setIrPanelOpen(false);
});
document.addEventListener("keydown", (event) => {
  if (!irPanel.hidden && event.key === "Escape") {
    event.preventDefault();
    setIrPanelOpen(false);
    $("ir").focus();
  }
});

$("ir-upload").addEventListener("click", () => {
  $("ir-config-file").value = "";
  $("ir-config-file").click();
});

$("ir-config-file").addEventListener("change", async () => {
  const file = $("ir-config-file").files[0];
  if (!file) return;
  if (file.size === 0 || file.size > 8192) {
    setIrStatus("error", "lircd.conf must be 1..8192 bytes");
    return;
  }
  setIrBusy(true);
  setIrStatus("", `Uploading ${file.name}…`);
  try {
    const response = await fetch("/api/remotes", {
      method: "PUT",
      headers: { "Content-Type": "text/plain" },
      body: file,
    });
    const body = await response.json().catch(() => null);
    if (!response.ok || !body) {
      const detail = body?.line ? `${body.error} at line ${body.line}` : body?.error;
      throw new Error(detail || `HTTP ${response.status}`);
    }
    await refreshIrRemotes();
    setIrStatus("ok", `Loaded ${body.remote_count} remotes · ${body.key_count} keys`);
  } catch (error) {
    setIrStatus("error", `Upload failed: ${error.message}`);
  } finally {
    setIrBusy(false);
  }
});

$("ir-download").addEventListener("click", async () => {
  setIrBusy(true);
  setIrStatus("", "Preparing lircd.conf…");
  try {
    const response = await fetch("/api/remotes/config", { cache: "no-store" });
    if (!response.ok) {
      const body = await response.json().catch(() => null);
      throw new Error(body?.error || `HTTP ${response.status}`);
    }
    const url = URL.createObjectURL(await response.blob());
    const link = document.createElement("a");
    link.href = url;
    link.download = "lircd.conf";
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    setIrStatus("ok", "Downloaded lircd.conf");
  } catch (error) {
    setIrStatus("error", `Download failed: ${error.message}`);
  } finally {
    setIrBusy(false);
  }
});

const helpPanel = $("help-panel");

function setHelpPanelOpen(open) {
  helpPanel.hidden = !open;
  $("help").setAttribute("aria-expanded", String(open));
  if (open) {
    setUartPopoverOpen(false);
    setIrPanelOpen(false);
  }
}

$("help").addEventListener("click", () => setHelpPanelOpen(helpPanel.hidden));
$("help-close").addEventListener("click", () => {
  setHelpPanelOpen(false);
  $("help").focus();
});
$("help-control").addEventListener("pointerdown", (event) => event.stopPropagation());
document.addEventListener("pointerdown", () => {
  if (!helpPanel.hidden) setHelpPanelOpen(false);
});
document.addEventListener("keydown", (event) => {
  if (!helpPanel.hidden && event.key === "Escape") {
    event.preventDefault();
    setHelpPanelOpen(false);
    $("help").focus();
  }
});

$("clear-history").addEventListener("click", async () => {
  if (!window.confirm("Clear all retained UART history? This cannot be undone.")) {
    terminal.focus();
    return;
  }
  $("clear-history").disabled = true;
  logGeneration += 1;
  logRequest?.abort();
  try {
    const response = await fetch("/api/clear_history", { method: "POST" });
    const body = await response.json().catch(() => null);
    if (!response.ok) throw new Error(body?.error || `HTTP ${response.status}`);
    uartNoteGeneration += 1;
    uartNoteNext = 0;
    terminal.reset();
    next = body.next;
  } catch (error) {
    showMessage(`Clear History failed: ${error.message}`);
  } finally {
    $("clear-history").disabled = false;
    terminal.focus();
  }
});

const downloadDialog = $("dl-dialog");
let downloading = false;
let downloadFinished = false;

function setDownloadStatus(kind, ...children) {
  $("dl-status").className = `dl-status ${kind}`;
  $("dl-status").replaceChildren(...children);
}

function setDownloadBusy(on) {
  downloading = on;
  for (const id of ["dl-path", "dl-start", "dl-cancel"]) $(id).disabled = on;
}

function setDownloadFinished(on) {
  downloadFinished = on;
  $("dl-start").textContent = on ? "Close" : "Download";
  $("dl-cancel").hidden = on;
}

function downloadResultList(result) {
  const list = document.createElement("dl");
  list.className = "dl-result";
  const ips = result.board_ips === null
    ? "not checked (console only)"
    : result.board_ips.join(", ") || "none found";
  const rows = [
    ["File", result.name],
    ["Size", `${result.size.toLocaleString()} bytes`],
    ["Transport", `${result.transport} · ${result.seconds} s · ${result.retries} retries`],
    ["STB IP", ips],
    ["SHA-256 STB", result.sha256_source, "mono"],
    ["SHA-256 copy", result.sha256_local, "mono"],
  ];
  for (const [label, value, cls] of rows) {
    const dt = document.createElement("dt");
    const dd = document.createElement("dd");
    dt.textContent = label;
    dd.textContent = value;
    if (cls) dd.className = cls;
    list.append(dt, dd);
  }
  return list;
}

function saveDownloadedFile(result) {
  const link = document.createElement("a");
  link.href = result.download_url;
  link.download = result.name;
  document.body.append(link);
  link.click();
  link.remove();
}

async function waitForDownload(generation, started) {
  while (true) {
    const response = await fetch("/api/download", { cache: "no-store" });
    const status = await response.json().catch(() => null);
    if (!response.ok || !status) {
      throw new Error(status?.error || `HTTP ${response.status}`);
    }
    if (status.generation !== generation) {
      throw new Error("download was replaced by another request");
    }
    if (status.state === "failed") throw new Error(status.error || "download failed");
    if (status.state === "complete") return status;
    const elapsed = Math.round((Date.now() - started) / 1000);
    const progress = status.size > 0
      ? ` ${status.received.toLocaleString()} / ${status.size.toLocaleString()} bytes.`
      : "";
    setDownloadStatus("busy", `Working… ${elapsed} s.${progress} The terminal shows each command.`);
    await new Promise((resolve) => setTimeout(resolve, 500));
  }
}

$("download-file").addEventListener("click", () => {
  if (!downloading) {
    setDownloadStatus("");
    setDownloadFinished(false);
  }
  downloadDialog.showModal();
  $("dl-path").select();
});
$("dl-cancel").addEventListener("click", () => downloadDialog.close());
$("dl-path").addEventListener("input", () => {
  if (downloadFinished) setDownloadFinished(false);
});
document.addEventListener("keydown", (event) => {
  if (downloading && event.key === "Escape") event.preventDefault();
}, true);
downloadDialog.addEventListener("cancel", (event) => {
  if (downloading) event.preventDefault();
});
downloadDialog.addEventListener("close", () => terminal.focus());

$("dl-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (downloadFinished) {
    downloadDialog.close();
    return;
  }
  const path = $("dl-path").value.trim();
  if (!path || downloading) return;
  setDownloadBusy(true);
  const started = Date.now();
  setDownloadStatus("busy", "Working… 0 s. The terminal shows each command.");
  try {
    const response = await fetch("/api/download", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path }),
    });
    const body = await response.json().catch(() => null);
    if (!response.ok) throw new Error(body?.error || `HTTP ${response.status}`);
    const result = await waitForDownload(body.generation, started);
    const match = document.createElement("p");
    match.className = "ok";
    match.textContent = "✓ SHA-256 of the STB file and the copy match. The browser saves the file.";
    setDownloadStatus("done", match, downloadResultList(result));
    saveDownloadedFile(result);
    setDownloadBusy(false);
    setDownloadFinished(true);
    $("dl-start").focus();
  } catch (error) {
    setDownloadStatus("error", `Download failed: ${error.message}`);
    setDownloadBusy(false);
    $("dl-path").focus();
  }
});

async function refreshOtaStatus() {
  try {
    const response = await fetch("/api/ota", { cache: "no-store" });
    if (!response.ok) throw new Error(`OTA status failed: HTTP ${response.status}`);
    const status = await response.json();
    if (status.running?.version) $("version").textContent = `v${status.running.version}`;
    $("ota-restart").hidden = !status.reboot_required;
  } catch (error) {
    showMessage(error.message);
  }
}

function uploadOta(file) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open("POST", "/api/ota");
    request.setRequestHeader("Content-Type", "application/octet-stream");
    request.timeout = 300000;
    request.upload.addEventListener("progress", (event) => {
      if (!event.lengthComputable) return;
      const percent = Math.min(100, Math.round((event.loaded * 100) / event.total));
      showMessage(`Uploading ${file.name}: ${percent}%`, 0);
    });
    request.addEventListener("load", () => {
      let body = null;
      try { body = JSON.parse(request.responseText); } catch {}
      if (request.status >= 200 && request.status < 300) resolve(body);
      else reject(new Error(body?.error || `HTTP ${request.status}`));
    });
    request.addEventListener("error", () => reject(new Error("network error")));
    request.addEventListener("timeout", () => reject(new Error("upload timed out")));
    request.send(file);
  });
}

$("ota-select").addEventListener("click", () => {
  $("ota-file").value = "";
  $("ota-file").click();
});

$("ota-file").addEventListener("change", async () => {
  const file = $("ota-file").files[0];
  if (!file) return;
  if (!file.name.toLowerCase().endsWith(".bin")) {
    showMessage("Select an ESP-IDF application .bin file");
    return;
  }
  if (!window.confirm(
    `Upload ${file.name} (${formatBytes(file.size)})? The device will not restart automatically.`
  )) return;

  $("ota-select").disabled = true;
  showMessage(`Uploading ${file.name}: 0%`, 0);
  try {
    const result = await uploadOta(file);
    $("ota-restart").hidden = false;
    showMessage(
      `OTA ${result.version} ready in ${result.partition}. Restart when ready.`,
      0
    );
  } catch (error) {
    showMessage(`OTA upload failed: ${error.message}`, 10000);
  } finally {
    $("ota-select").disabled = false;
  }
});

$("ota-restart").addEventListener("click", async () => {
  if (!window.confirm("Restart stb-buddy into the uploaded firmware now?")) return;
  $("ota-select").disabled = true;
  $("ota-restart").disabled = true;
  showMessage("Restarting stb-buddy…", 0);
  try {
    const response = await fetch("/api/ota/reboot", { method: "POST" });
    if (!response.ok) {
      const body = await response.json().catch(() => null);
      throw new Error(body?.error || `HTTP ${response.status}`);
    }
  } catch (error) {
    showMessage(`Restart failed: ${error.message}`, 10000);
    $("ota-select").disabled = false;
    $("ota-restart").disabled = false;
  }
});

pollLog();
pollStatus();
pollIrEvents();
pollUartNotes();
refreshOtaStatus();
