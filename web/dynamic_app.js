const API_ROOT = "/sprinkler/api";
const DAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const TIMEZONES = [
  "Europe/Budapest", "UTC", "Europe/London", "Europe/Berlin", "Europe/Paris", "Europe/Madrid", "Europe/Rome",
  "America/New_York", "America/Chicago", "America/Denver", "America/Los_Angeles",
];
const WEATHER = {
  wind: ["Wind", "km/h"],
  gust: ["Gust", "km/h"],
  rain_24h: ["Rain 24h", "mm"],
  rain_rate: ["Rain rate", "mm/h"],
  temperature: ["Daily temp", "C"],
};
const WEATHER_PROTECTIONS = {
  wind: "Wind protection",
  gust: "Gust protection",
  rain_rate: "Active rain protection",
  rain_24h: "24-hour rain adjustment",
  temperature: "Temperature adjustment",
};

const appState = {
  schedules: [],
  selectedId: 0,
  editor: null,
  manualRows: [],
  status: null,
  statusReceivedAt: 0,
  history: [],
  historyReceivedAt: 0,
  historyNextOffset: null,
  historyTotal: 0,
  historyLoading: false,
  runHistory: [],
  runHistoryNextOffset: null,
  runHistoryTotal: 0,
  system: null,
  zoneNames: [],
  zoneIds: [],
  weatherCapabilities: {},
  solarAvailable: false,
  tab: "dashboard",
  statusLoading: false,
  actionInProgress: false,
  statusGeneration: 0,
  statusFailures: 0,
  statusTimer: null,
  displayRunKey: "",
  displayRowKey: "",
  displayRemaining: null,
  displayElapsed: null,
};

function apiUrl(path, params = {}) {
  const url = new URL(`${API_ROOT}/${path}`, window.location.origin);
  for (const [key, value] of Object.entries(params)) url.searchParams.set(key, value);
  return url;
}

async function api(path, { method = "GET", params = {}, data } = {}) {
  const options = { method, credentials: "same-origin", cache: "no-store" };
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), method === "POST" ? 15000 : 5000);
  options.signal = controller.signal;
  if (data !== undefined) {
    const body = new URLSearchParams();
    body.set("data", JSON.stringify(data));
    options.body = body;
    options.headers = { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" };
  } else if (method === "POST") {
    const body = new URLSearchParams(params);
    options.body = body;
    options.headers = { "Content-Type": "application/x-www-form-urlencoded;charset=UTF-8" };
    params = {};
  }
  if (method === "POST") options.headers = { ...options.headers, "X-Sprinkler-Request": "1" };
  try {
    const response = await fetch(apiUrl(path, params), options);
    let payload;
    try {
      payload = await response.json();
    } catch {
      if (response.ok) throw new Error("Controller returned an invalid JSON response");
      payload = {};
    }
    if (!response.ok) throw new Error(payload.error || `Request failed (${response.status})`);
    if (payload === null || typeof payload !== "object") throw new Error("Controller returned an invalid response");
    return payload;
  } finally {
    clearTimeout(timeout);
  }
}

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, (char) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
  })[char]);
}

function defaultRow() {
  return {
    zones: [1],
    duration_seconds: 60,
    delay_after_seconds: 0,
    max_wind: 0,
    max_gust: 0,
    rain_target: 0,
  };
}

function defaultSchedule() {
  return {
    id: 0,
    revision: 0,
    name: "New schedule",
    enabled: false,
    start_triggers: [{ type: "clock", minute: 360 }],
    rounds: 1,
    days_mask: 0x7F,
    max_wait_minutes: 240,
    base_temp: 22,
    temp_adjust: 0,
    rows: [defaultRow()],
  };
}

function formatClock(seconds) {
  const value = Math.max(0, Math.floor(Number(seconds) || 0));
  const hours = Math.floor(value / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  const remainder = value % 60;
  return hours > 0
    ? `${hours}:${String(minutes).padStart(2, "0")}:${String(remainder).padStart(2, "0")}`
    : `${minutes}:${String(remainder).padStart(2, "0")}`;
}

function formatRemainingClock(seconds) {
  return formatClock(Math.ceil(Math.max(0, Number(seconds) || 0)));
}

function wifiSignalLabel(rssi) {
  if (rssi === null || rssi === undefined) return "";
  const value = Number(rssi);
  if (!Number.isFinite(value)) return "";
  if (value >= -50) return "Excellent";
  if (value >= -65) return "Good";
  if (value >= -75) return "Fair";
  return "Weak";
}

function formatStartTime(minutes) {
  const value = Math.max(0, Math.min(1439, Number(minutes) || 0));
  return `${String(Math.floor(value / 60)).padStart(2, "0")}:${String(value % 60).padStart(2, "0")}`;
}

function parseStartTime(value) {
  const [hour, minute] = String(value).split(":").map(Number);
  return hour * 60 + minute;
}

function normalizeStartTriggers(schedule) {
  if (!Array.isArray(schedule.start_triggers) || !schedule.start_triggers.length) {
    if (Array.isArray(schedule.start_times) && schedule.start_times.length) {
      schedule.start_triggers = schedule.start_times.map((minute) => ({ type: "clock", minute }));
    } else {
      schedule.start_triggers = [{ type: "clock", minute: 360 }];
    }
  }
  schedule.rounds = Number(schedule.rounds) || 1;
}

function formatCountdown(seconds) {
  const value = Math.max(0, Math.floor(Number(seconds) || 0));
  const days = Math.floor(value / 86400);
  const hours = Math.floor((value % 86400) / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  if (days) return `${days}d ${hours}h`;
  if (hours) return `${hours}h ${minutes}m`;
  return `${minutes}m`;
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.round(bytes / 1024)} KB`;
}

function formatAge(seconds) {
  if (seconds === null || seconds === undefined) return "no update";
  if (seconds < 60) return `${Math.floor(seconds)}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  return `${Math.floor(seconds / 3600)}h ago`;
}

function toast(message, error = false) {
  const element = document.getElementById("toast");
  if (!element) return;
  element.textContent = message;
  element.classList.toggle("error", error);
  element.classList.add("show");
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => element.classList.remove("show"), 2600);
}

function setText(element, value) {
  if (element && element.textContent !== value) element.textContent = value;
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function monotonicNow() {
  return globalThis.performance?.now?.() ?? Date.now();
}

function statusHasSettled(status) {
  return status.state !== "starting";
}

function skipStatusHasSettled(status, previous) {
  if (!statusHasSettled(status) || !previous?.active) return statusHasSettled(status);
  const sameRow = status.active && status.round === previous.round && status.row === previous.row;
  return !(status.state === "running" && sameRow && Number(status.remaining_ms) < 1000);
}

function renderShell() {
  if (!document.querySelector('meta[name="viewport"]')) {
    const viewport = document.createElement("meta");
    viewport.name = "viewport";
    viewport.content = "width=device-width, initial-scale=1, viewport-fit=cover";
    document.head.appendChild(viewport);
  }
  const mount = document.querySelector("esp-app") || document.body;
  mount.innerHTML = `
    <main class="app dynamic-app">
      <header class="topbar">
        <div class="title"><h1>Sprinkler Controller</h1><p id="connection-text">Connecting</p></div>
        <div class="topbar-right">
          <div id="weather-strip" class="weather-strip"></div>
          <div id="run-summary" class="status-pill">Idle</div>
        </div>
      </header>
      <nav class="tabs" role="tablist" aria-label="Controller sections">
        <button id="tab-dashboard" class="tab active" role="tab" aria-selected="true" aria-controls="view-dashboard" data-tab="dashboard">Dashboard</button>
        <button id="tab-schedules" class="tab" role="tab" aria-selected="false" aria-controls="view-schedules" data-tab="schedules">Schedules</button>
        <button id="tab-manual" class="tab" role="tab" aria-selected="false" aria-controls="view-manual" data-tab="manual">Manual</button>
        <button id="tab-history" class="tab" role="tab" aria-selected="false" aria-controls="view-history" data-tab="history">History</button>
        <button id="tab-settings" class="tab" role="tab" aria-selected="false" aria-controls="view-settings" data-tab="settings">Settings</button>
      </nav>
      <section id="view-dashboard" class="view active" role="tabpanel" aria-labelledby="tab-dashboard"></section>
      <section id="view-schedules" class="view" role="tabpanel" aria-labelledby="tab-schedules"></section>
      <section id="view-manual" class="view" role="tabpanel" aria-labelledby="tab-manual"></section>
      <section id="view-history" class="view" role="tabpanel" aria-labelledby="tab-history"></section>
      <section id="view-settings" class="view" role="tabpanel" aria-labelledby="tab-settings"></section>
    </main>
    <div id="toast" class="toast" role="status" aria-live="polite"></div>`;

  document.querySelectorAll("[data-tab]").forEach((button) => {
    button.addEventListener("click", () => setTab(button.dataset.tab));
  });
}

function renderWeatherStrip() {
  const strip = document.getElementById("weather-strip");
  if (!strip) return;
  strip.innerHTML = Object.entries(WEATHER)
    .filter(([key]) => appState.weatherCapabilities[key])
    .map(([key, [label]]) => `<div class="weather-item" data-weather="${key}"><div class="weather-label">${label}</div><div class="weather-value">-</div><div class="weather-age">no update</div></div>`)
    .join("");
  strip.hidden = !strip.children.length;
}

function applyWeatherCapabilities(capabilities) {
  if (!capabilities || typeof capabilities !== "object") return;
  const normalized = Object.fromEntries(Object.keys(WEATHER).map((key) => [key, Boolean(capabilities[key])]));
  if (JSON.stringify(normalized) === JSON.stringify(appState.weatherCapabilities)) return;
  appState.weatherCapabilities = normalized;
  renderWeatherStrip();
  renderSettings();
  if (appState.editor) renderEditor();
}

function applySolarAvailability(available) {
  const next = Boolean(available);
  if (next === appState.solarAvailable) return;
  appState.solarAvailable = next;
  if (appState.editor) renderEditor();
}

function setTab(tab) {
  appState.tab = tab;
  document.querySelectorAll("[data-tab]").forEach((button) => {
    const selected = button.dataset.tab === tab;
    button.classList.toggle("active", selected);
    button.setAttribute("aria-selected", String(selected));
  });
  document.querySelectorAll(".view").forEach((view) => view.classList.toggle("active", view.id === `view-${tab}`));
  if (tab === "history") loadHistory();
  if (tab === "settings") loadSystem();
}

function renderDashboard() {
  document.getElementById("view-dashboard").innerHTML = `
    <div class="grid three runtime-grid">
      <article id="current-run-panel" class="panel">
        <div class="panel-header"><h2>Current Run</h2><span id="runtime-state" class="status-pill">Idle</span></div>
        <div class="panel-body">
          <div class="grid four">
            <div class="metric"><div class="label">Program</div><div id="runtime-name" class="value">-</div></div>
            <div class="metric"><div class="label">Row</div><div id="runtime-row" class="value">-</div></div>
            <div class="metric"><div class="label">Row remaining</div><div id="runtime-remaining" class="value timer-value">0:00</div></div>
            <div class="metric"><div class="label">Run elapsed</div><div id="runtime-elapsed" class="value timer-value">0:00</div></div>
          </div>
          <div id="runtime-zones" class="active-zone-list"></div>
          <div class="runtime-actions">
            <button id="pause-run" class="btn">Pause</button>
            <button id="resume-run" class="btn primary">Resume</button>
            <button id="skip-row" class="btn">Skip row</button>
            <button id="stop-run" class="btn danger">Emergency stop</button>
          </div>
        </div>
      </article>
      <article id="next-panel" class="panel">
        <div class="panel-header"><h2>Next Schedule</h2></div>
        <div class="panel-body">
          <div class="metric"><div class="label">Program</div><div id="next-name" class="value">-</div></div>
          <div class="next-grid">
            <div><span>Starts</span><strong id="next-start">-</strong></div>
            <div><span>In</span><strong id="next-countdown">-</strong></div>
          </div>
        </div>
      </article>
      <article id="capacity-panel" class="panel">
        <div class="panel-header"><h2>Capacity</h2></div>
        <div class="panel-body capacity-body">
          <div class="metric"><div class="label">Saved schedules</div><div id="schedule-count" class="value">0 / 32</div></div>
          <p class="notice">Only schedules and rows you add appear in the editor.</p>
        </div>
      </article>
    </div>`;
  document.getElementById("pause-run").onclick = () => control("pause");
  document.getElementById("resume-run").onclick = () => control("resume");
  document.getElementById("skip-row").onclick = () => control("skip");
  document.getElementById("stop-run").onclick = () => control("stop");
}

async function control(action) {
  if (appState.actionInProgress) return;
  const previous = appState.status;
  appState.actionInProgress = true;
  appState.statusGeneration += 1;
  try {
    await api("control", { method: "POST", params: { action } });
    await loadSettledStatus((status) => {
      return action === "skip" ? skipStatusHasSettled(status, previous) : statusHasSettled(status);
    });
  } catch (error) {
    toast(error.message, true);
  } finally {
    appState.actionInProgress = false;
    scheduleStatusPoll();
  }
}

function updateRuntime() {
  const status = appState.status || { state: "idle", active: false, zones: [] };
  const since = Math.max(0, (monotonicNow() - appState.statusReceivedAt) / 1000);
  const ticking = status.state === "running" || status.state === "delay";
  const sampledRemaining = Number.isFinite(Number(status.remaining_ms))
    ? Number(status.remaining_ms) / 1000
    : Number(status.remaining_seconds) || 0;
  const sampledElapsed = Number.isFinite(Number(status.elapsed_ms))
    ? Number(status.elapsed_ms) / 1000
    : Number(status.elapsed_seconds) || 0;
  const rawRemaining = Math.max(0, sampledRemaining - (ticking ? since : 0));
  const rawElapsed = sampledElapsed + (status.active && status.state !== "paused" ? since : 0);
  const runKey = status.active
    ? `${status.run_sequence ?? "legacy"}:${status.manual}:${status.schedule_id}:${status.name}`
    : "";
  const rowKey = status.active ? `${runKey}:${status.round}:${status.row}:${status.state}` : "";
  if (runKey !== appState.displayRunKey) {
    appState.displayRunKey = runKey;
    appState.displayElapsed = null;
    appState.displayRowKey = "";
  }
  if (rowKey !== appState.displayRowKey) {
    appState.displayRowKey = rowKey;
    appState.displayRemaining = null;
  }
  appState.displayRemaining = appState.displayRemaining === null
    ? rawRemaining
    : Math.min(appState.displayRemaining, rawRemaining);
  appState.displayElapsed = appState.displayElapsed === null
    ? rawElapsed
    : Math.max(appState.displayElapsed, rawElapsed);
  const remaining = appState.displayRemaining;
  const elapsed = appState.displayElapsed;
  const stateLabel = status.state.replaceAll("_", " ");
  const values = {
    "runtime-state": stateLabel,
    "run-summary": status.active ? `${status.name || "Program"} - ${stateLabel}` : "Idle",
    "runtime-name": status.active ? status.name : "-",
    "runtime-row": status.active ? `Round ${status.round} / ${status.round_count}, row ${status.row} / ${status.row_count}` : "-",
    "runtime-remaining": status.state === "starting" ? "Starting..." : formatRemainingClock(remaining),
    "runtime-elapsed": formatClock(elapsed),
    "next-name": status.next?.name || "No enabled schedules",
    "next-start": status.next?.start || "-",
    "next-countdown": status.next?.name ? formatCountdown(Math.max(0, (status.next.seconds || 0) - since)) : "-",
  };
  for (const [id, value] of Object.entries(values)) {
    setText(document.getElementById(id), value);
  }
  const zones = document.getElementById("runtime-zones");
  if (zones) {
    const zoneLabels = (status.zones || []).map((zone) => zoneName(zone));
    const renderKey = JSON.stringify(zoneLabels);
    if (zones.dataset.renderKey !== renderKey) {
      zones.innerHTML = zoneLabels.map((name) => `<span>${escapeHtml(name)}</span>`).join("");
      zones.dataset.renderKey = renderKey;
    }
  }
  const pause = document.getElementById("pause-run");
  const resume = document.getElementById("resume-run");
  const skip = document.getElementById("skip-row");
  const stop = document.getElementById("stop-run");
  if (pause) pause.disabled = appState.actionInProgress || status.state !== "running";
  if (resume) resume.disabled = appState.actionInProgress || status.state !== "paused";
  if (skip) skip.disabled = appState.actionInProgress || !status.active;
  if (stop) stop.disabled = appState.actionInProgress || !status.active;
  document.getElementById("current-run-panel")?.classList.toggle("is-idle", !status.active);
  const summary = document.getElementById("run-summary");
  if (summary) summary.hidden = !status.active;
  for (const [key, [, unit]] of Object.entries(WEATHER)) {
    const item = document.querySelector(`[data-weather="${key}"]`);
    const reading = status.weather?.[key];
    if (!item) continue;
    const value = reading?.value === null || reading?.value === undefined
      ? "-" : `${Number(reading.value).toFixed(1)} ${unit}`;
    setText(item.querySelector(".weather-value"), value);
    setText(item.querySelector(".weather-age"), formatAge(reading?.age_seconds));
  }
}

function renderScheduleView() {
  document.getElementById("view-schedules").innerHTML = `
    <div class="schedule-layout">
      <aside class="schedule-sidebar">
        <div class="sidebar-header"><h2>Schedules</h2><button id="add-schedule" class="btn primary">Add</button></div>
        <div id="schedule-list" class="schedule-list"></div>
      </aside>
      <div id="schedule-editor" class="schedule-editor empty-editor">
        <p>Select a schedule or add one.</p>
      </div>
    </div>`;
  document.getElementById("add-schedule").onclick = addSchedule;
  renderScheduleList();
}

function renderScheduleList() {
  const list = document.getElementById("schedule-list");
  if (!list) return;
  list.innerHTML = appState.schedules.length
    ? appState.schedules.map((schedule) => `
        <button class="schedule-list-item${schedule.id === appState.selectedId ? " active" : ""}" data-id="${schedule.id}">
          <strong>${escapeHtml(schedule.name)}</strong><span>${escapeHtml(schedule.start)} - ${schedule.row_count} rows</span>
        </button>`).join("")
    : `<p class="empty-message">No schedules yet.</p>`;
  list.querySelectorAll("[data-id]").forEach((button) => {
    button.onclick = () => selectSchedule(Number(button.dataset.id));
  });
  const count = document.getElementById("schedule-count");
  if (count) count.textContent = `${appState.schedules.length} / 32`;
}

function addSchedule() {
  if (appState.schedules.length >= 32) return toast("Schedule capacity reached", true);
  appState.selectedId = 0;
  appState.editor = defaultSchedule();
  renderScheduleList();
  renderEditor();
}

async function selectSchedule(id) {
  try {
    appState.selectedId = id;
    appState.editor = await api("schedule", { params: { id } });
    normalizeStartTriggers(appState.editor);
    renderScheduleList();
    renderEditor();
  } catch (error) {
    toast(error.message, true);
  }
}

function scheduleFieldsFromDom() {
  const editor = appState.editor;
  editor.name = document.getElementById("schedule-name").value.trim() || "Schedule";
  editor.enabled = document.getElementById("schedule-enabled").checked;
  editor.start_triggers = [...document.querySelectorAll(".start-trigger")].map((card, index) => {
    const type = card.querySelector("[data-trigger-type]").value;
    const clock = card.querySelector("[data-trigger-clock]");
    const offset = card.querySelector("[data-trigger-offset]");
    return type === "clock"
      ? { type, minute: clock ? parseStartTime(clock.value) : 360 }
      : { type, offset: offset ? Number(offset.value) : 0, resolved: editor.start_triggers[index]?.resolved || null };
  });
  editor.rounds = Number(document.getElementById("schedule-rounds").value);
  const maxWait = document.getElementById("max-wait");
  const baseTemp = document.getElementById("base-temp");
  const tempAdjust = document.getElementById("temp-adjust");
  if (maxWait) editor.max_wait_minutes = Number(maxWait.value);
  if (baseTemp) editor.base_temp = Number(baseTemp.value);
  if (tempAdjust) editor.temp_adjust = Number(tempAdjust.value);
  editor.days_mask = DAYS.reduce((mask, _, index) => mask | (document.getElementById(`day-${index}`).checked ? 1 << index : 0), 0);
  editor.rows = readRows("schedule-rows");
  return editor;
}

function renderEditor() {
  const container = document.getElementById("schedule-editor");
  const schedule = appState.editor;
  if (!container || !schedule) return;
  const weather = appState.weatherCapabilities;
  const hasWaitProtection = weather.wind || weather.gust || weather.rain_rate;
  normalizeStartTriggers(schedule);
  container.className = "schedule-editor";
  container.innerHTML = `
    <article class="panel editor-panel">
      <div class="panel-header editor-title">
        <input id="schedule-name" class="title-input" aria-label="Schedule name" maxlength="31" value="${escapeHtml(schedule.name)}">
        <label class="check-control"><input id="schedule-enabled" type="checkbox"${schedule.enabled ? " checked" : ""}> Enabled</label>
      </div>
      <div class="panel-body">
        ${hasWaitProtection || weather.temperature ? `<div class="schedule-settings">
          ${hasWaitProtection ? `<label class="field">Max weather wait, min<input id="max-wait" type="number" min="0" max="600" value="${schedule.max_wait_minutes}"></label>` : ""}
          ${weather.temperature ? `<label class="field">Base temperature, C<input id="base-temp" type="number" min="-20" max="60" step="0.5" value="${schedule.base_temp}"></label>
          <label class="field">Adjustment, % / C<input id="temp-adjust" type="number" min="-20" max="20" step="0.5" value="${schedule.temp_adjust}"></label>` : ""}
          <label class="field">Rounds<input id="schedule-rounds" type="number" min="1" max="10" step="1" value="${schedule.rounds}"></label>
        </div>` : ""}
        ${!(hasWaitProtection || weather.temperature) ? `<div class="schedule-settings"><label class="field">Rounds<input id="schedule-rounds" type="number" min="1" max="10" step="1" value="${schedule.rounds}"></label></div>` : ""}
        <div class="times-heading"><h3>Start triggers <span>${schedule.start_triggers.length} / 8</span></h3><button id="add-start-time" class="btn"${schedule.start_triggers.length >= 8 ? " disabled" : ""}>Add trigger</button></div>
        <div class="start-times">${schedule.start_triggers.map((trigger, index) => `
          <div class="start-trigger" data-trigger-index="${index}">
            <label class="field">Mode<select data-trigger-type>
              <option value="clock"${trigger.type === "clock" ? " selected" : ""}>Clock</option>
              ${appState.solarAvailable ? `<option value="sunrise"${trigger.type === "sunrise" ? " selected" : ""}>Sunrise</option><option value="sunset"${trigger.type === "sunset" ? " selected" : ""}>Sunset</option>` : ""}
            </select></label>
            ${trigger.type === "clock" ? `<label class="field">Time<input data-trigger-clock type="time" value="${formatStartTime(trigger.minute)}" required></label>` : `<label class="field">Offset, min<input data-trigger-offset type="number" min="-360" max="360" step="5" value="${trigger.offset || 0}"></label><div class="solar-preview" data-solar-preview="${index}">Today: ${escapeHtml(trigger.resolved || "calculating...")}</div>`}
            ${index ? `<button data-remove-time="${index}" class="remove-zone trigger-remove" title="Remove trigger" aria-label="Remove trigger ${index + 1}">x</button>` : ""}
          </div>`).join("")}
        </div>
        <div class="day-picker">${DAYS.map((day, index) => `<label><input id="day-${index}" type="checkbox"${schedule.days_mask & (1 << index) ? " checked" : ""}>${day}</label>`).join("")}</div>
        <div class="rows-heading"><h3>Rows <span>${schedule.rows.length} / 32</span></h3><button id="add-schedule-row" class="btn">Add row</button></div>
        <div id="schedule-rows" class="dynamic-rows"></div>
        <div class="editor-actions">
          <button id="save-schedule" class="btn primary">Save schedule</button>
          <button id="run-schedule" class="btn">Run now</button>
          ${schedule.id ? `<button id="delete-schedule" class="btn danger">Delete</button>` : ""}
        </div>
      </div>
    </article>`;
  renderRows("schedule-rows", schedule.rows);
  document.getElementById("add-start-time").onclick = () => mutateStartTimes();
  document.querySelectorAll("[data-remove-time]").forEach((button) => {
    button.onclick = () => mutateStartTimes(Number(button.dataset.removeTime));
  });
  document.querySelectorAll("[data-trigger-type]").forEach((select) => {
    select.onchange = () => changeTriggerType(Number(select.closest(".start-trigger").dataset.triggerIndex), select.value);
  });
  document.querySelectorAll("[data-trigger-offset]").forEach((input) => {
    let previewTimer;
    input.oninput = () => {
      const index = Number(input.closest(".start-trigger").dataset.triggerIndex);
      appState.editor.start_triggers[index].offset = Number(input.value);
      appState.editor.start_triggers[index].resolved = null;
      const preview = document.querySelector(`[data-solar-preview="${index}"]`);
      if (preview) preview.textContent = "Today: calculating...";
      clearTimeout(previewTimer);
      previewTimer = setTimeout(() => updateSolarPreview(index), 250);
    };
  });
  appState.editor.start_triggers.forEach((trigger, index) => {
    if (trigger.type !== "clock" && !trigger.resolved) updateSolarPreview(index);
  });
  document.getElementById("add-schedule-row").onclick = () => addRow("schedule");
  document.getElementById("save-schedule").onclick = saveSchedule;
  document.getElementById("run-schedule").onclick = runSchedule;
  document.getElementById("delete-schedule")?.addEventListener("click", deleteSchedule);
}

function mutateStartTimes(removeIndex = null) {
  scheduleFieldsFromDom();
  if (removeIndex === null) {
    if (appState.editor.start_triggers.length >= 8) return toast("Start trigger capacity reached", true);
    const clockTriggers = appState.editor.start_triggers.filter((trigger) => trigger.type === "clock");
    const last = clockTriggers.at(-1)?.minute ?? 360;
    const used = new Set(clockTriggers.map((trigger) => trigger.minute));
    let candidate = (last + 60) % 1440;
    while (used.has(candidate)) candidate = (candidate + 60) % 1440;
    appState.editor.start_triggers.push({ type: "clock", minute: candidate });
  } else if (appState.editor.start_triggers.length > 1) {
    appState.editor.start_triggers.splice(removeIndex, 1);
  }
  renderEditor();
}

function changeTriggerType(index, type) {
  scheduleFieldsFromDom();
  appState.editor.start_triggers[index] = type === "clock"
    ? { type, minute: 360 }
    : { type, offset: 0, resolved: null };
  renderEditor();
}

async function updateSolarPreview(index) {
  const trigger = appState.editor?.start_triggers?.[index];
  if (!trigger || trigger.type === "clock") return;
  const requestedType = trigger.type;
  const requestedOffset = Number(trigger.offset) || 0;
  const preview = document.querySelector(`[data-solar-preview="${index}"]`);
  if (preview) preview.textContent = "Today: calculating...";
  try {
    const payload = await api("solar", {
      params: { type: requestedType, offset: requestedOffset },
    });
    const current = appState.editor?.start_triggers?.[index];
    if (!current || current.type !== requestedType || (Number(current.offset) || 0) !== requestedOffset) return;
    trigger.resolved = payload.resolved || null;
    if (preview) preview.textContent = `Today: ${trigger.resolved || "unavailable"}`;
  } catch (error) {
    const current = appState.editor?.start_triggers?.[index];
    if (!current || current.type !== requestedType || (Number(current.offset) || 0) !== requestedOffset) return;
    trigger.resolved = null;
    if (preview) preview.textContent = `Today: ${error.message}`;
  }
}

function zoneOptions(selected) {
  return appState.zoneNames.map((name, index) => {
    const id = appState.zoneIds[index];
    return `<option value="${id}"${selected === id ? " selected" : ""}>${id} - ${escapeHtml(name)}</option>`;
  }).join("");
}

function zoneName(zoneId) {
  const index = appState.zoneIds.indexOf(Number(zoneId));
  return index >= 0 ? appState.zoneNames[index] : `Zone ${zoneId}`;
}

function applyZoneNames(names, configuredZones) {
  let zoneIds = appState.zoneIds;
  if (Array.isArray(configuredZones) && configuredZones.length) {
    names = configuredZones.map((zone) => zone.name);
    zoneIds = configuredZones.map((zone) => Number(zone.id));
  }
  if (!Array.isArray(names) || names.length < 1 || names.length > 32) return;
  if (zoneIds.length !== names.length) zoneIds = names.map((_, index) => index + 1);
  const zonesChanged = JSON.stringify(names) !== JSON.stringify(appState.zoneNames) ||
    JSON.stringify(zoneIds) !== JSON.stringify(appState.zoneIds);
  if (!zonesChanged) return;
  appState.zoneIds = zoneIds;
  appState.zoneNames = names;
  renderZoneNameFields();
  document.querySelectorAll("[data-quick-zone]").forEach((button) => {
    button.textContent = zoneName(button.dataset.quickZone);
  });
  document.querySelectorAll("select[data-zone] option").forEach((option) => {
    const zone = Number(option.value);
    option.textContent = `${zone} - ${zoneName(zone)}`;
  });
  document.querySelectorAll("[data-zone-name]").forEach((input) => {
    if (document.activeElement !== input) input.value = zoneName(input.dataset.zoneName);
  });
}

function renderRows(containerId, rows) {
  const container = document.getElementById(containerId);
  const manual = containerId === "manual-rows";
  const weather = appState.weatherCapabilities;
  container.innerHTML = rows.map((row, index) => `
    <div class="dynamic-row" data-row="${index}">
      <div class="row-topline"><strong>Row ${index + 1}</strong><div class="row-tools">
        <button class="icon-command" data-move="up" title="Move up"${index === 0 ? " disabled" : ""}>Up</button>
        <button class="icon-command" data-move="down" title="Move down"${index === rows.length - 1 ? " disabled" : ""}>Down</button>
        <button class="icon-command danger-text" data-remove-row title="Remove row"${rows.length === 1 ? " disabled" : ""}>Remove</button>
      </div></div>
      <div class="zone-editor">
        <div class="zone-selects">${row.zones.map((zone, zoneIndex) => `<label class="field">Zone ${zoneIndex + 1}<span class="inline-control"><select data-zone>${zoneOptions(zone)}</select>${zoneIndex ? `<button data-remove-zone="${zoneIndex}" class="remove-zone" title="Remove zone" aria-label="Remove zone ${zoneIndex + 1} from row ${index + 1}">x</button>` : ""}</span></label>`).join("")}</div>
        ${row.zones.length < 3 ? `<button class="btn add-zone" data-add-zone>Add zone</button>` : ""}
      </div>
      <div class="row-fields">
        <label class="field">Duration, min<input data-field="duration" type="number" min="0.02" max="60" step="0.5" value="${row.duration_seconds / 60}"></label>
        <label class="field">Delay after, min<input data-field="delay" type="number" min="0" max="120" step="0.5" value="${row.delay_after_seconds / 60}"></label>
        ${manual ? "" : `
          ${weather.wind ? `<label class="field">Max wind, km/h<input data-field="wind" type="number" min="0" max="150" step="0.5" value="${row.max_wind}"></label>` : ""}
          ${weather.gust ? `<label class="field">Max gust, km/h<input data-field="gust" type="number" min="0" max="150" step="0.5" value="${row.max_gust}"></label>` : ""}
          ${weather.rain_24h ? `<label class="field">Rain target, mm<input data-field="rain" type="number" min="0" max="100" step="0.5" value="${row.rain_target}"></label>` : ""}`}
      </div>
    </div>`).join("");

  container.querySelectorAll(".dynamic-row").forEach((card) => {
    const index = Number(card.dataset.row);
    card.querySelector("[data-add-zone]")?.addEventListener("click", () => mutateRows(containerId, (items) => {
      const used = new Set(items[index].zones);
      const next = appState.zoneIds.find((zone) => !used.has(zone));
      if (next) items[index].zones.push(next);
    }));
    card.querySelectorAll("[data-remove-zone]").forEach((button) => button.addEventListener("click", () => mutateRows(containerId, (items) => items[index].zones.splice(Number(button.dataset.removeZone), 1))));
    card.querySelector("[data-remove-row]").onclick = () => mutateRows(containerId, (items) => {
      if (items.length > 1) items.splice(index, 1);
    });
    card.querySelectorAll("[data-move]").forEach((button) => button.addEventListener("click", () => mutateRows(containerId, (items) => {
      const target = index + (button.dataset.move === "up" ? -1 : 1);
      [items[index], items[target]] = [items[target], items[index]];
    })));
  });
}

function readRows(containerId) {
  const manual = containerId === "manual-rows";
  const existingRows = manual ? appState.manualRows : appState.editor?.rows || [];
  return [...document.querySelectorAll(`#${containerId} .dynamic-row`)].map((card, index) => {
    const existing = existingRows[index] || defaultRow();
    return {
      zones: [...card.querySelectorAll("[data-zone]")].map((select) => Number(select.value)),
      duration_seconds: Math.round(Number(card.querySelector('[data-field="duration"]').value) * 60),
      delay_after_seconds: Math.round(Number(card.querySelector('[data-field="delay"]').value) * 60),
      max_wind: Number(card.querySelector('[data-field="wind"]')?.value ?? (manual ? 0 : existing.max_wind)),
      max_gust: Number(card.querySelector('[data-field="gust"]')?.value ?? (manual ? 0 : existing.max_gust)),
      rain_target: Number(card.querySelector('[data-field="rain"]')?.value ?? (manual ? 0 : existing.rain_target)),
    };
  });
}

function mutateRows(containerId, mutator) {
  const manual = containerId === "manual-rows";
  const rows = readRows(containerId);
  mutator(rows);
  if (manual) appState.manualRows = rows;
  else appState.editor.rows = rows;
  renderRows(containerId, rows);
  updateRowCount();
}

function addRow(kind) {
  if (kind === "schedule") scheduleFieldsFromDom();
  else appState.manualRows = readRows("manual-rows");
  const rows = kind === "manual" ? appState.manualRows : appState.editor.rows;
  if (rows.length >= 32) return toast("Row capacity reached", true);
  rows.push(defaultRow());
  renderRows(kind === "manual" ? "manual-rows" : "schedule-rows", rows);
  updateRowCount();
}

function validateRows(rows) {
  if (!rows.length) throw new Error("At least one row is required");
  for (const [index, row] of rows.entries()) {
    if (new Set(row.zones).size !== row.zones.length) {
      throw new Error(`Row ${index + 1} contains the same zone more than once`);
    }
  }
}

function validateStartTriggers(triggers) {
  if (!triggers.length) throw new Error("At least one start trigger is required");
  if (triggers.length > 8) throw new Error("A schedule can have at most eight start triggers");
  for (const trigger of triggers) {
    if (trigger.type === "clock" && (!Number.isInteger(trigger.minute) || trigger.minute < 0 || trigger.minute > 1439)) {
      throw new Error("Every clock time must be valid");
    }
    if (trigger.type !== "clock" && (!Number.isInteger(trigger.offset) || trigger.offset < -360 || trigger.offset > 360)) {
      throw new Error("Solar offsets must be whole minutes between -360 and 360");
    }
  }
  const keys = triggers.map((trigger) => trigger.type === "clock" ? `clock:${trigger.minute}` : `${trigger.type}:${trigger.offset}`);
  if (new Set(keys).size !== keys.length) throw new Error("Schedule start triggers must be unique");
}

function updateRowCount() {
  const heading = document.querySelector(".rows-heading h3 span");
  if (heading && appState.editor) heading.textContent = `${appState.editor.rows.length} / 32`;
}

async function saveSchedule() {
  try {
    const schedule = scheduleFieldsFromDom();
    const nameBytes = new TextEncoder().encode(schedule.name).length;
    if (nameBytes < 1 || nameBytes > 31) throw new Error("Schedule name must be 1-31 UTF-8 bytes");
    if (!schedule.days_mask) throw new Error("Select at least one weekday");
    validateStartTriggers(schedule.start_triggers);
    if (!Number.isInteger(schedule.rounds) || schedule.rounds < 1 || schedule.rounds > 10) throw new Error("Rounds must be between 1 and 10");
    if (schedule.rows.some((row) => row.duration_seconds < schedule.rounds)) throw new Error("Each row needs at least one second per round");
    validateRows(schedule.rows);
    const saved = await api("save", { method: "POST", data: schedule });
    appState.editor = saved;
    appState.selectedId = saved.id;
    await loadSchedules();
    renderEditor();
    toast("Schedule saved");
  } catch (error) {
    toast(error.message, true);
  }
}

async function runSchedule() {
  if (!appState.editor.id) return toast("Save the schedule before running it", true);
  if (appState.actionInProgress) return;
  appState.actionInProgress = true;
  appState.statusGeneration += 1;
  try {
    await api("run", { method: "POST", params: { id: appState.editor.id } });
    await loadSettledStatus();
    toast("Schedule started");
    setTab("dashboard");
  } catch (error) {
    toast(error.message, true);
  } finally {
    appState.actionInProgress = false;
    scheduleStatusPoll();
  }
}

async function deleteSchedule() {
  if (!confirm(`Delete ${appState.editor.name}?`)) return;
  try {
    await api("delete", { method: "POST", params: { id: appState.editor.id } });
    appState.editor = null;
    appState.selectedId = 0;
    await loadSchedules();
    renderScheduleView();
    toast("Schedule deleted");
  } catch (error) {
    toast(error.message, true);
  }
}

function renderManual() {
  if (!appState.manualRows.length) appState.manualRows = [defaultRow()];
  document.getElementById("view-manual").innerHTML = `
    <div class="grid two manual-layout">
    <article class="panel">
      <div class="panel-header"><h2>Quick Manual Run</h2></div>
      <div class="panel-body">
        <label class="field quick-duration">Duration, min<input id="quick-manual-duration" type="number" min="1" max="60" step="1" value="2"></label>
        <div class="quick-zone-grid">${appState.zoneNames.map((name, index) => `<button class="btn" data-quick-zone="${appState.zoneIds[index]}">${escapeHtml(name)}</button>`).join("")}</div>
      </div>
    </article>
    <article class="panel manual-program-panel">
      <div class="panel-header"><h2>One-off Manual Program</h2><button id="add-manual-row" class="btn">Add row</button></div>
      <div class="panel-body">
        <p class="notice">Rows run once in order and are not saved.</p>
        <div id="manual-rows" class="dynamic-rows"></div>
        <div class="editor-actions"><button id="start-manual" class="btn primary">Start manual program</button></div>
      </div>
    </article></div>`;
  renderRows("manual-rows", appState.manualRows);
  document.getElementById("add-manual-row").onclick = () => addRow("manual");
  document.getElementById("start-manual").onclick = startManual;
  document.querySelectorAll("[data-quick-zone]").forEach((button) => {
    button.onclick = () => startQuickManual(Number(button.dataset.quickZone));
  });
}

async function startQuickManual(zone) {
  if (appState.actionInProgress) return;
  const duration = Math.max(1, Math.min(60, Number(document.getElementById("quick-manual-duration").value) || 2));
  appState.actionInProgress = true;
  appState.statusGeneration += 1;
  try {
    await api("manual", { method: "POST", data: {
      name: "Quick manual run",
      rows: [{ ...defaultRow(), zones: [zone], duration_seconds: Math.round(duration * 60) }],
    } });
    await loadSettledStatus();
    toast("Manual run started");
    setTab("dashboard");
  } catch (error) {
    toast(error.message, true);
  } finally {
    appState.actionInProgress = false;
    scheduleStatusPoll();
  }
}

async function startManual() {
  if (appState.actionInProgress) return;
  appState.manualRows = readRows("manual-rows");
  appState.actionInProgress = true;
  appState.statusGeneration += 1;
  try {
    validateRows(appState.manualRows);
    await api("manual", { method: "POST", data: { name: "One-off manual program", rows: appState.manualRows } });
    await loadSettledStatus();
    toast("Manual program started");
    setTab("dashboard");
  } catch (error) {
    toast(error.message, true);
  } finally {
    appState.actionInProgress = false;
    scheduleStatusPoll();
  }
}

function renderHistory() {
  const container = document.getElementById("view-history");
  const rows = appState.history.map((entry) => {
    const started = entry.started_at ? new Date(entry.started_at * 1000).toLocaleString() : "-";
    const position = `Round ${entry.round}, row ${entry.row}`;
    const masks = `${entry.commanded_on || "-"} → ${entry.commanded_off || "-"}`;
    return `<tr>
      <td><strong>${escapeHtml(entry.name)}</strong><br><span class="table-detail">${entry.manual ? "Manual" : `Schedule ${entry.schedule_id}`}</span></td>
      <td>${escapeHtml(entry.zone_name)} <span class="table-detail">(#${entry.zone_id})</span></td>
      <td>${escapeHtml(position)}</td>
      <td>${escapeHtml(started)}</td>
      <td data-activity-duration data-active="${entry.active ? "1" : "0"}" data-watered-ms="${Number(entry.watered_ms) || 0}">${entry.active ? "Currently running" : formatClock((Number(entry.watered_ms) || 0) / 1000)}</td>
      <td>${escapeHtml(entry.result)}</td>
      <td><code>${escapeHtml(masks)}</code></td>
    </tr>`;
  }).join("");
  const runRows = appState.runHistory.map((entry) => `
    <tr>
      <td>${escapeHtml(entry.name)}</td>
      <td>${entry.started_at ? new Date(entry.started_at * 1000).toLocaleString() : "-"}</td>
      <td>${formatClock(entry.watered_seconds)}</td>
      <td>${escapeHtml(entry.result)}</td>
    </tr>`).join("");
  container.innerHTML = `
    <article class="panel">
      <div class="panel-header"><h2>Zone Activity</h2><button id="refresh-history" class="btn">Refresh</button></div>
      <div class="history-table">
        ${appState.history.length ? `<table><thead><tr><th>Program</th><th>Zone</th><th>Position</th><th>Started</th><th>Watered</th><th>Result</th><th>Commanded zones</th></tr></thead><tbody>${rows}</tbody></table>` : `<p class="empty-message">No zone activity recorded yet.</p>`}
      </div>
      <div class="panel-footer history-footer">
        <span>${appState.history.length} of ${appState.historyTotal} zone intervals</span>
        <button id="load-older-history" class="btn" ${appState.historyNextOffset === null ? "disabled" : ""}>Load older</button>
      </div>
      <p class="notice">Commanded-zone masks use zone IDs as bits and help distinguish scheduler commands from relay hardware behavior.</p>
    </article>
    <article class="panel">
      <div class="panel-header"><h2>Recent Runs</h2></div>
      <div class="history-table">
        ${appState.runHistory.length ? `<table><thead><tr><th>Program</th><th>Started</th><th>Watered</th><th>Result</th></tr></thead><tbody>${runRows}</tbody></table>` : `<p class="empty-message">No completed runs yet.</p>`}
      </div>
      <div class="panel-footer history-footer">
        <span>${appState.runHistory.length} of ${appState.runHistoryTotal} runs</span>
        <button id="load-older-runs" class="btn" ${appState.runHistoryNextOffset === null ? "disabled" : ""}>Load older</button>
      </div>
    </article>`;
  document.getElementById("refresh-history").onclick = () => loadHistory(true);
  document.getElementById("load-older-history").onclick = () => loadHistory(false);
  document.getElementById("load-older-runs").onclick = loadOlderRuns;
  updateHistoryRuntime();
}

function updateHistoryRuntime() {
  const elapsed = Math.max(0, monotonicNow() - appState.historyReceivedAt);
  document.querySelectorAll("[data-activity-duration]").forEach((cell) => {
    if (cell.dataset.active !== "1") return;
    const wateredMs = (Number(cell.dataset.wateredMs) || 0) + elapsed;
    setText(cell, `Currently running · ${formatClock(wateredMs / 1000)}`);
  });
}

function renderSettings() {
  const availableProtections = Object.entries(WEATHER_PROTECTIONS)
    .filter(([key]) => appState.weatherCapabilities[key]);
  document.getElementById("view-settings").innerHTML = `
    <div class="grid two settings-layout">
      <article class="panel">
        <div class="panel-header"><h2>System Health</h2><button id="refresh-system" class="btn">Refresh</button></div>
        <div class="panel-body"><div class="grid two">
          <div class="metric"><div class="label">System load</div><div id="system-load" class="value">-</div></div>
          <div class="metric"><div class="label">Max loop time</div><div id="system-loop" class="value">-</div></div>
          <div class="metric"><div class="label">Free heap</div><div id="system-free-heap" class="value">-</div></div>
          <div class="metric"><div class="label">Largest heap block</div><div id="system-largest-block" class="value">-</div></div>
          <div class="metric"><div class="label">Uptime</div><div id="system-uptime" class="value">-</div></div>
          <div class="metric"><div class="label">Last refreshed</div><div id="system-refreshed" class="value">-</div></div>
        </div></div>
      </article>
      <article class="panel">
        <div class="panel-header"><h2>Controller Settings</h2></div>
        <div class="panel-body">
          <div class="settings-grid">
            <label class="field">Timezone<select id="timezone-setting">${TIMEZONES.map((timezone) => `<option>${timezone}</option>`).join("")}</select></label>
            <div class="metric compact"><div class="label">Device time</div><div id="system-device-time" class="value">-</div></div>
          </div>
          <p class="notice">Timezone is stored on the controller.</p>
        </div>
      </article>
      ${availableProtections.length ? `<article class="panel">
        <div class="panel-header"><h2>Weather Protections</h2>${availableProtections.length ? `<button id="disable-weather-protections" class="btn">Disable all</button>` : ""}</div>
        <div class="panel-body weather-protection-grid">
          ${availableProtections.map(([key, label]) => `<button class="toggle-setting" data-weather-protection="${key}" aria-pressed="false">${label}: Off</button>`).join("")}
        </div>
      </article>` : ""}
      <article class="panel">
        <div class="panel-header"><h2>Backup</h2></div>
        <div class="panel-body">
          <div class="editor-actions backup-actions">
            <button id="export-controller" class="btn">Export</button>
            <button id="import-controller" class="btn">Import</button>
            <input id="import-controller-file" type="file" accept="application/json,.json" hidden>
          </div>
        </div>
      </article>
      <article class="panel zone-settings-panel">
        <div class="panel-header"><h2>Zone Names</h2></div>
        <div id="zone-name-grid" class="panel-body zone-name-grid"></div>
      </article>
    </div>`;
  document.getElementById("refresh-system").onclick = loadSystem;
  document.getElementById("timezone-setting").onchange = (event) => saveSetting("timezone", event.target.value);
  document.querySelectorAll("[data-weather-protection]").forEach((button) => {
    button.onclick = () => saveSetting("weather_protection", button.getAttribute("aria-pressed") === "true" ? "off" : "on", { key: button.dataset.weatherProtection });
  });
  document.getElementById("disable-weather-protections")?.addEventListener("click", () => saveSetting("disable_weather_protections", "off"));
  document.getElementById("export-controller").onclick = exportController;
  document.getElementById("import-controller").onclick = () => document.getElementById("import-controller-file").click();
  document.getElementById("import-controller-file").onchange = importController;
  renderZoneNameFields();
}

function renderZoneNameFields() {
  const container = document.getElementById("zone-name-grid");
  if (!container) return;
  container.innerHTML = appState.zoneNames.length
    ? appState.zoneNames.map((name, index) => `<label class="field">Zone ID ${appState.zoneIds[index]}<input data-zone-name="${appState.zoneIds[index]}" maxlength="31" value="${escapeHtml(name)}"></label>`).join("")
    : `<p class="empty-message">Loading configured zones...</p>`;
  container.querySelectorAll("[data-zone-name]").forEach((input) => {
    input.onchange = () => saveSetting("zone_name", input.value.trim(), { index: input.dataset.zoneName });
  });
}

function updateSystem() {
  const system = appState.system;
  if (!system) return;
  applyZoneNames(system.zone_names, system.configured_zones);
  applyWeatherCapabilities(system.weather?.capabilities);
  applySolarAvailability(system.solar_available);
  const loop = Number(system.max_loop_ms) || 0;
  const load = loop <= 30 ? "OK" : loop <= 100 ? "Busy" : loop <= 500 ? "High" : "Critical";
  const values = {
    "system-load": load,
    "system-loop": `${loop} ms`,
    "system-free-heap": formatBytes(system.free_heap),
    "system-largest-block": formatBytes(system.largest_heap_block),
    "system-uptime": formatCountdown(system.uptime_seconds),
    "system-device-time": system.device_time || "-",
    "system-refreshed": new Date().toLocaleTimeString(),
  };
  for (const [id, value] of Object.entries(values)) {
    const element = document.getElementById(id);
    if (element) element.textContent = value;
  }
  const timezone = document.getElementById("timezone-setting");
  if (timezone && document.activeElement !== timezone && TIMEZONES.includes(system.timezone)) timezone.value = system.timezone;
  document.querySelectorAll("[data-weather-protection]").forEach((button) => {
    const key = button.dataset.weatherProtection;
    const enabled = Boolean(system.weather?.enabled?.[key]);
    button.setAttribute("aria-pressed", String(enabled));
    button.classList.toggle("active", enabled);
    button.textContent = `${WEATHER_PROTECTIONS[key]}: ${enabled ? "On" : "Off"}`;
  });
}

async function loadSystem() {
  const button = document.getElementById("refresh-system");
  try {
    if (button) {
      button.disabled = true;
      button.textContent = "Refreshing...";
    }
    appState.system = await api("system");
    updateSystem();
  } catch (error) {
    toast(error.message, true);
  } finally {
    const currentButton = document.getElementById("refresh-system");
    if (currentButton) {
      currentButton.disabled = false;
      currentButton.textContent = "Refresh";
    }
  }
}

async function saveSetting(setting, value, extra = {}) {
  try {
    appState.system = await api("settings", { method: "POST", params: { setting, value, ...extra } });
    updateSystem();
    toast("Setting saved");
  } catch (error) {
    toast(error.message, true);
  }
}

async function exportController() {
  try {
    toast("Preparing backup");
    const [system, list] = await Promise.all([api("system"), api("schedules")]);
    const schedules = [];
    for (const item of list.schedules || []) schedules.push(await api("schedule", { params: { id: item.id } }));
    const backup = {
      format: "dynamic-sprinkler-backup",
      version: 1,
      created_at: new Date().toISOString(),
      settings: {
        timezone: system.timezone,
        manual_duration_minutes: system.manual_duration_minutes,
        zone_names: system.zone_names,
        configured_zones: system.configured_zones,
        weather_enabled: system.weather?.enabled || {},
      },
      schedules,
    };
    const blob = new Blob([JSON.stringify(backup, null, 2)], { type: "application/json" });
    const link = document.createElement("a");
    link.href = URL.createObjectURL(blob);
    link.download = `sprinkler-backup-${new Date().toISOString().slice(0, 10)}.json`;
    document.body.appendChild(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(link.href);
    toast("Backup exported");
  } catch (error) {
    toast(error.message, true);
  }
}

async function importController(event) {
  const input = event.target;
  let locked = false;
  try {
    const file = input.files?.[0];
    if (!file) return;
    const backup = JSON.parse(await file.text());
    if (backup.format !== "dynamic-sprinkler-backup" || backup.version !== 1 || !Array.isArray(backup.schedules)) {
      throw new Error("Unsupported backup file");
    }
    if (backup.schedules.length > 32) throw new Error("Backup exceeds the 32-schedule limit");
    const settings = backup.settings || {};
    if (settings.timezone && !TIMEZONES.includes(settings.timezone)) throw new Error("Backup contains an unsupported timezone");
    if (settings.zone_names && !Array.isArray(settings.zone_names)) throw new Error("Backup zone names are invalid");
    for (const name of settings.zone_names || []) {
      const bytes = new TextEncoder().encode(String(name)).length;
      if (bytes < 1 || bytes > 31) throw new Error("Backup contains a zone name outside the 1-31 byte limit");
    }
    let importedZones;
    if (settings.configured_zones !== undefined) {
      if (!Array.isArray(settings.configured_zones)) throw new Error("Backup configured zones are invalid");
      importedZones = settings.configured_zones.map((zone) => {
        if (!zone || typeof zone !== "object") throw new Error("Backup configured zones are invalid");
        const id = Number(zone.id);
        const name = String(zone.name ?? "");
        const bytes = new TextEncoder().encode(name).length;
        if (!Number.isInteger(id) || !appState.zoneIds.includes(id) || bytes < 1 || bytes > 31) {
          throw new Error("Backup contains an invalid configured zone");
        }
        return { id, name };
      });
    } else {
      importedZones = (settings.zone_names || []).map((name, index) => ({ id: appState.zoneIds[index], name }));
    }
    const duration = Number(settings.manual_duration_minutes ?? 2);
    if (!Number.isFinite(duration) || duration < 1 || duration > 60) throw new Error("Backup manual duration is invalid");
    for (const schedule of backup.schedules) {
      normalizeStartTriggers(schedule);
      const candidate = { ...schedule, id: 0, revision: 0 };
      await api("validate", { method: "POST", data: candidate });
    }
    if (!confirm(`Replace all controller settings and ${appState.schedules.length} saved schedules with this backup?`)) return;
    if (appState.actionInProgress) throw new Error("Another controller operation is still running");
    appState.actionInProgress = true;
    appState.statusGeneration += 1;
    locked = true;

    const liveStatus = await api("status");
    if (liveStatus.active) throw new Error("Stop the active run before importing a backup");

    const current = await api("schedules");
    const mutationStatus = await api("status");
    if (mutationStatus.active) throw new Error("A run started before import; stop it and retry");
    for (const schedule of current.schedules || []) await api("delete", { method: "POST", params: { id: schedule.id } });

    if (TIMEZONES.includes(settings.timezone)) await api("settings", { method: "POST", params: { setting: "timezone", value: settings.timezone } });
    if (duration >= 1 && duration <= 60) await api("settings", { method: "POST", params: { setting: "manual_duration", value: duration } });
    for (const zone of importedZones) {
      if (zone.name && appState.zoneIds.includes(Number(zone.id))) await api("settings", { method: "POST", params: { setting: "zone_name", index: zone.id, value: zone.name } });
    }
    for (const key of Object.keys(WEATHER_PROTECTIONS)) {
      if (!appState.weatherCapabilities[key]) continue;
      await api("settings", { method: "POST", params: {
        setting: "weather_protection", key, value: settings.weather_enabled?.[key] ? "on" : "off",
      } });
    }
    for (const schedule of backup.schedules) {
      await api("save", { method: "POST", data: { ...schedule, id: 0, revision: 0 } });
    }
    appState.editor = null;
    appState.selectedId = 0;
    await Promise.all([loadSchedules(), loadSettledStatus(), loadSystem()]);
    renderScheduleView();
    toast("Backup imported");
  } catch (error) {
    toast(error.message, true);
  } finally {
    if (locked) {
      appState.actionInProgress = false;
      scheduleStatusPoll();
    }
    input.value = "";
  }
}

async function loadSchedules() {
  const payload = await api("schedules");
  appState.schedules = payload.schedules || [];
  renderScheduleList();
}

async function fetchStatusSnapshot() {
  const requestedAt = monotonicNow();
  const status = await api("status");
  const receivedAt = monotonicNow();
  return {
    status,
    // The device samples its clocks while the request is in flight. Using the
    // request midpoint avoids re-anchoring timers to variable response latency.
    sampledAt: requestedAt + ((receivedAt - requestedAt) / 2),
  };
}

function applyStatusSnapshot({ status, sampledAt }) {
  const wasActive = Boolean(appState.status?.active);
  appState.status = status;
  appState.statusReceivedAt = sampledAt;
  applyWeatherCapabilities(status.weather?.capabilities);
  applySolarAvailability(status.solar_available);
  applyZoneNames(status.zone_names, status.configured_zones);
  const rssi = status.wifi_rssi === null || status.wifi_rssi === undefined ? NaN : Number(status.wifi_rssi);
  const signal = wifiSignalLabel(status.wifi_rssi);
  setText(document.getElementById("connection-text"), Number.isFinite(rssi)
    ? `Connected · Wi-Fi ${rssi} dBm (${signal})`
    : "Connected · Wi-Fi unavailable");
  appState.statusFailures = 0;
  updateRuntime();
  if (appState.tab === "history" && wasActive && !status.active) void loadHistory(true);
}

async function loadSettledStatus(predicate = statusHasSettled, timeoutMs = 4000) {
  const deadline = monotonicNow() + timeoutMs;
  let snapshot;
  do {
    snapshot = await fetchStatusSnapshot();
    if (predicate(snapshot.status)) {
      applyStatusSnapshot(snapshot);
      return snapshot.status;
    }
    if (monotonicNow() >= deadline) return appState.status;
    await delay(200);
  } while (true);
}

async function loadStatus() {
  if (appState.statusLoading || appState.actionInProgress || document.hidden) return true;
  appState.statusLoading = true;
  const generation = appState.statusGeneration;
  try {
    let snapshot = await fetchStatusSnapshot();
    const transitionDeadline = monotonicNow() + 4000;
    while (snapshot.status.state === "starting" && monotonicNow() < transitionDeadline) {
      await delay(200);
      snapshot = await fetchStatusSnapshot();
    }
    if (snapshot.status.state === "starting") return true;
    if (generation !== appState.statusGeneration || appState.actionInProgress) return true;
    applyStatusSnapshot(snapshot);
    return true;
  } catch (error) {
    appState.statusFailures += 1;
    const reason = error?.name === "AbortError" ? "response timeout" : "network/API error";
    document.getElementById("connection-text").textContent = `Update delayed · ${reason}`;
    return false;
  } finally {
    appState.statusLoading = false;
  }
}

function scheduleStatusPoll() {
  clearTimeout(appState.statusTimer);
  const baseDelay = appState.status?.active ? 3000 : 10000;
  const backoff = appState.statusFailures
    ? Math.min(60000, baseDelay * (2 ** Math.min(appState.statusFailures, 3)))
    : baseDelay;
  appState.statusTimer = setTimeout(async () => {
    await loadStatus();
    scheduleStatusPoll();
  }, backoff);
}

async function loadHistory(reset = true) {
  if (appState.historyLoading) return;
  appState.historyLoading = true;
  try {
    const offset = reset ? 0 : appState.historyNextOffset;
    if (!reset && offset === null) return;
    const [payload, runs] = await Promise.all([
      api("activity", { params: { offset, limit: 10 } }),
      reset ? api("history", { params: { offset: 0, limit: 10 } }) : Promise.resolve(null),
    ]);
    const activity = Array.isArray(payload.activity) ? payload.activity : [];
    appState.history = reset ? activity : [...appState.history, ...activity];
    appState.historyReceivedAt = monotonicNow();
    appState.historyNextOffset = payload.next_offset === null || payload.next_offset === undefined
      ? null : Number(payload.next_offset);
    appState.historyTotal = Number(payload.total) || 0;
    if (runs) {
      appState.runHistory = Array.isArray(runs.history) ? runs.history : [];
      appState.runHistoryNextOffset = runs.next_offset === null || runs.next_offset === undefined
        ? null : Number(runs.next_offset);
      appState.runHistoryTotal = Number(runs.total) || 0;
    }
    renderHistory();
  } catch (error) {
    toast(error.message, true);
  } finally {
    appState.historyLoading = false;
  }
}

async function loadOlderRuns() {
  if (appState.historyLoading || appState.runHistoryNextOffset === null) return;
  appState.historyLoading = true;
  try {
    const payload = await api("history", { params: { offset: appState.runHistoryNextOffset, limit: 10 } });
    const runs = Array.isArray(payload.history) ? payload.history : [];
    appState.runHistory = [...appState.runHistory, ...runs];
    appState.runHistoryNextOffset = payload.next_offset === null || payload.next_offset === undefined
      ? null : Number(payload.next_offset);
    appState.runHistoryTotal = Number(payload.total) || 0;
    renderHistory();
  } catch (error) {
    toast(error.message, true);
  } finally {
    appState.historyLoading = false;
  }
}

async function init() {
  renderShell();
  renderDashboard();
  renderScheduleView();
  renderManual();
  renderHistory();
  renderSettings();
  try {
    await Promise.all([loadSchedules(), loadStatus()]);
    renderManual();
  } catch (error) {
    toast(error.message, true);
  }
  scheduleStatusPoll();
  setInterval(() => {
    if (appState.status?.active) updateRuntime();
    if (appState.tab === "history") updateHistoryRuntime();
  }, 250);
  setInterval(() => {
    if (appState.tab === "settings") loadSystem();
  }, 10000);
  setInterval(() => {
    if (appState.tab === "history" && appState.status?.active) loadHistory(true);
  }, 3000);
}

if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
else init();
