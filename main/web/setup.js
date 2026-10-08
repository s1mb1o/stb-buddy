"use strict";

const form = document.getElementById("wifi-form");
const status = document.getElementById("status");
const save = document.getElementById("save");

fetch("/api/wifi", { cache: "no-store" })
  .then((response) => response.ok ? response.json() : null)
  .then((wifi) => {
    if (wifi?.ssid) document.getElementById("ap-name").textContent = wifi.ssid;
  })
  .catch(() => {});

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const ssid = document.getElementById("ssid").value;
  const password = document.getElementById("password").value;
  if (password.length > 0 && password.length < 8) {
    status.className = "setup-status error";
    status.textContent = "Password must be empty or contain at least 8 characters.";
    return;
  }

  save.disabled = true;
  status.className = "setup-status";
  status.textContent = "Saving…";
  try {
    const response = await fetch("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ssid, password }),
    });
    const body = await response.json().catch(() => null);
    if (!response.ok) throw new Error(body?.error || `HTTP ${response.status}`);
    document.getElementById("password").value = "";
    status.className = "setup-status ok";
    status.textContent = "Saved. stb-buddy is restarting and will join the selected network.";
  } catch (error) {
    status.className = "setup-status error";
    status.textContent = `Could not save Wi-Fi settings: ${error.message}`;
    save.disabled = false;
  }
});
