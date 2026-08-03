const state = {
  activeTab: "dashboard",
  history: [],
  lastMetrics: {},
  lastStatus: null
};

const views = {
  dashboard: {
    title: "Dashboard",
    subtitle: "Live health, storage files, and request metrics."
  },
  keys: {
    title: "Keys",
    subtitle: "Current in-memory keyspace with value sizes and TTL metadata."
  },
  replication: {
    title: "Replication",
    subtitle: "Sequence position, journal size, and change export checks."
  },
  operations: {
    title: "Operations",
    subtitle: "Read, write, delete, compact, and verify values through the HTTP API."
  }
};

function $(id) {
  return document.getElementById(id);
}

function baseUrl() {
  return $("nodeUrl").value.replace(/\/+$/, "");
}

function formatBytes(value) {
  if (!Number.isFinite(value) || value <= 0) {
    return "0 B";
  }
  const units = ["B", "KB", "MB", "GB"];
  let size = value;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
}

function log(message, level = "info") {
  const timestamp = new Date();
  state.history.unshift({ timestamp, message, level });
  state.history = state.history.slice(0, 40);
  renderLog();
}

function renderLog() {
  $("logList").innerHTML = state.history
    .map((entry) => {
      const time = entry.timestamp.toLocaleTimeString();
      return `<div class="log-entry"><strong>${time}</strong><span>${escapeHtml(entry.message)}</span></div>`;
    })
    .join("");
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

async function request(path, options = {}) {
  const response = await fetch(`${baseUrl()}${path}`, options);
  if (!response.ok) {
    const detail = await response.text();
    throw new Error(detail.trim() || `HTTP ${response.status}`);
  }
  return response;
}

async function fetchJson(path) {
  const response = await request(path);
  return response.json();
}

async function refresh() {
  const healthPromise = fetchJson("/health");
  const statusPromise = fetchJson("/storage/status");
  const keysPromise = fetchJson("/kv");
  const metricsPromise = request("/metrics").then((response) => response.text());

  const [health, status, keys, metrics] = await Promise.all([
    healthPromise,
    statusPromise,
    keysPromise,
    metricsPromise
  ]);

  state.lastStatus = status;
  state.lastMetrics = parseMetrics(metrics);
  renderConnection(true);
  renderDashboard(health, status, metrics);
  renderKeys(keys.keys || []);
  renderReplication(status);
  $("lastUpdated").textContent = new Date().toLocaleTimeString();
}

function renderConnection(online) {
  const element = $("connectionState");
  element.textContent = online ? "online" : "offline";
  element.classList.toggle("ok", online);
  element.classList.toggle("danger", !online);
}

function renderDashboard(health, status, metrics) {
  $("healthValue").textContent = health.status || "ok";
  $("keysValue").textContent = status.keys ?? 0;
  $("sequenceValue").textContent = status.last_sequence ?? 0;
  $("compactionsValue").textContent = status.compactions_total ?? 0;
  $("metricsText").textContent = metrics.trim() || "No metrics returned.";

  const wal = status.wal_bytes || 0;
  const sstable = status.sstable_bytes || 0;
  const replication = status.replication_bytes || 0;
  const total = wal + sstable + replication;
  $("totalBytes").textContent = formatBytes(total);
  $("walBytes").textContent = formatBytes(wal);
  $("sstableBytes").textContent = formatBytes(sstable);
  $("replicationBytes").textContent = formatBytes(replication);

  const max = Math.max(wal, sstable, replication, 1);
  $("walBar").style.width = `${Math.max(4, (wal / max) * 100)}%`;
  $("sstableBar").style.width = `${Math.max(4, (sstable / max) * 100)}%`;
  $("replicationBar").style.width = `${Math.max(4, (replication / max) * 100)}%`;
}

function renderKeys(keys) {
  $("keyCountLabel").textContent = `${keys.length} ${keys.length === 1 ? "key" : "keys"}`;
  if (keys.length === 0) {
    $("keyList").innerHTML = `<p class="muted">No live keys.</p>`;
    return;
  }
  const now = Date.now();
  $("keyList").innerHTML = keys
    .map((item) => {
      const ttl = item.expires_at_ms > 0
        ? `${Math.max(0, Math.ceil((item.expires_at_ms - now) / 1000))}s TTL`
        : "persistent";
      return `<div class="key-item">
        <span class="key-name" title="${escapeHtml(item.key)}">${escapeHtml(item.key)}</span>
        <code>${formatBytes(item.value_size)}</code>
        <span class="muted">${ttl}</span>
      </div>`;
    })
    .join("");
}

function renderReplication(status) {
  $("replicationState").textContent = "ready";
  $("replicationSequence").textContent = status.last_sequence ?? 0;
  $("replicationBytesDetail").textContent = formatBytes(status.replication_bytes || 0);
}

function parseMetrics(metrics) {
  const parsed = {};
  for (const line of metrics.split("\n")) {
    const [key, value] = line.trim().split(/\s+/);
    if (key && value) {
      parsed[key] = Number(value);
    }
  }
  return parsed;
}

function selectTab(tab) {
  state.activeTab = tab;
  document.querySelectorAll(".tab").forEach((button) => {
    button.classList.toggle("active", button.dataset.tab === tab);
  });
  document.querySelectorAll(".view").forEach((view) => {
    view.classList.toggle("active", view.id === tab);
  });
  $("viewTitle").textContent = views[tab].title;
  $("viewSubtitle").textContent = views[tab].subtitle;
}

async function guarded(action, successMessage) {
  try {
    await action();
    if (successMessage) {
      log(successMessage);
    }
    await refresh();
  } catch (error) {
    renderConnection(false);
    log(error.message, "error");
  }
}

function encodedKey(value) {
  const key = value.trim();
  if (!key) {
    throw new Error("Key is required.");
  }
  if (key.includes("/")) {
    throw new Error("Keys cannot contain slash characters.");
  }
  return encodeURIComponent(key);
}

function bindEvents() {
  document.querySelectorAll(".tab").forEach((button) => {
    button.addEventListener("click", () => selectTab(button.dataset.tab));
  });

  $("refreshButton").addEventListener("click", () => guarded(refresh, "Refreshed node state."));
  $("clearLogButton").addEventListener("click", () => {
    state.history = [];
    renderLog();
  });

  $("compactButton").addEventListener("click", () => guarded(async () => {
    await request("/compact", { method: "POST" });
  }, "Compaction requested."));

  $("putButton").addEventListener("click", () => guarded(async () => {
    const key = encodedKey($("putKey").value);
    const ttl = $("putTtl").value.trim();
    const suffix = ttl ? `?ttl=${encodeURIComponent(ttl)}` : "";
    await request(`/kv/${key}${suffix}`, {
      method: "PUT",
      headers: { "Content-Type": "text/plain; charset=utf-8" },
      body: $("putValue").value
    });
  }, "Value stored."));

  $("getButton").addEventListener("click", () => guarded(async () => {
    const key = encodedKey($("lookupKey").value);
    const response = await request(`/kv/${key}`);
    $("lookupResult").textContent = await response.text();
  }, "Value loaded."));

  $("deleteButton").addEventListener("click", () => guarded(async () => {
    const key = encodedKey($("lookupKey").value);
    await request(`/kv/${key}`, { method: "DELETE" });
    $("lookupResult").textContent = "Deleted.";
  }, "Value deleted."));

  $("checkChangesButton").addEventListener("click", () => guarded(async () => {
    const after = $("afterSequence").value.trim() || "0";
    const response = await request(`/replication/changes?after=${encodeURIComponent(after)}`);
    const bytes = Number(response.headers.get("Content-Length") || 0);
    $("changesResult").textContent = `Change batch available: ${formatBytes(bytes)}`;
  }, "Checked replication changes."));
}

bindEvents();
selectTab("dashboard");
guarded(refresh, "Connected to StrataDB.");
