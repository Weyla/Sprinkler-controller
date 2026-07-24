const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

class Element {
  constructor() {
    this.dataset = {};
    this.children = new Map();
    this.classList = { toggle() {} };
    this.hidden = false;
    this.disabled = false;
    this.textWrites = 0;
    this.htmlWrites = 0;
    this.value = "";
    this.html = "";
  }

  get textContent() { return this.value; }
  set textContent(value) { this.value = value; this.textWrites += 1; }
  get innerHTML() { return this.html; }
  set innerHTML(value) { this.html = value; this.htmlWrites += 1; }
  querySelector(selector) { return this.children.get(selector) || null; }
}

function loadApp(fetchImpl = async () => {
  throw new Error("Unexpected fetch");
}) {
  const elements = new Map();
  const weather = new Map();
  const document = {
    hidden: false,
    readyState: "loading",
    addEventListener() {},
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, new Element());
      return elements.get(id);
    },
    querySelector(selector) {
      const match = selector.match(/^\[data-weather="([^"]+)"\]$/);
      if (match) return weather.get(match[1]) || null;
      return null;
    },
    querySelectorAll() { return []; },
  };
  const context = vm.createContext({
    console,
    document,
    window: { location: { origin: "http://controller" } },
    URL,
    URLSearchParams,
    AbortController,
    Blob,
    TextEncoder,
    fetch: fetchImpl,
    setTimeout,
    clearTimeout,
    setInterval() {},
    confirm() { return true; },
  });
  const source = fs.readFileSync(path.join(__dirname, "../web/dynamic_app.js"), "utf8");
  vm.runInContext(`${source}\n;globalThis.__test = { appState, updateRuntime, statusHasSettled, skipStatusHasSettled, loadHistory, loadOlderRuns, renderHistory };`, context);
  return { ...context.__test, document, elements, weather };
}

test("transient start and expired pre-skip frames are not settled", () => {
  const { statusHasSettled, skipStatusHasSettled } = loadApp();
  const previous = { active: true, round: 1, row: 1 };
  assert.equal(statusHasSettled({ state: "starting" }), false);
  assert.equal(statusHasSettled({ state: "running" }), true);
  assert.equal(skipStatusHasSettled({ state: "running", active: true, round: 1, row: 1, remaining_ms: 0 }, previous), false);
  assert.equal(skipStatusHasSettled({ state: "starting", active: true, round: 1, row: 2, remaining_ms: 0 }, previous), false);
  assert.equal(skipStatusHasSettled({ state: "running", active: true, round: 1, row: 2, remaining_ms: 5000 }, previous), true);
});

test("unchanged runtime zones and weather do not rewrite DOM nodes", () => {
  const { appState, updateRuntime, document, elements, weather } = loadApp();
  const weatherItem = new Element();
  weatherItem.children.set(".weather-value", new Element());
  weatherItem.children.set(".weather-age", new Element());
  weather.set("wind", weatherItem);
  appState.weatherCapabilities = { wind: true };
  appState.zoneIds = [1];
  appState.zoneNames = ["Front lawn"];
  appState.statusReceivedAt = Date.now();
  appState.status = {
    state: "paused",
    active: true,
    manual: true,
    schedule_id: 0,
    name: "Test",
    round: 1,
    round_count: 1,
    row: 1,
    row_count: 1,
    remaining_ms: 5000,
    elapsed_ms: 2000,
    zones: [1],
    next: {},
    weather: { wind: { value: 4.2, age_seconds: 3 } },
  };

  updateRuntime();
  updateRuntime();

  assert.equal(document.getElementById("runtime-zones").htmlWrites, 1);
  assert.equal(weatherItem.querySelector(".weather-value").textWrites, 1);
  assert.equal(weatherItem.querySelector(".weather-age").textWrites, 1);
  assert.equal(elements.get("runtime-remaining").textContent, "0:05");
});

test("a new run sequence resets monotonic timer anchors", () => {
  const { appState, updateRuntime } = loadApp();
  const base = {
    state: "paused",
    active: true,
    manual: false,
    schedule_id: 7,
    name: "Repeated",
    round: 1,
    round_count: 1,
    row: 1,
    row_count: 1,
    zones: [],
    next: {},
    weather: {},
  };
  appState.statusReceivedAt = Date.now();
  appState.status = { ...base, run_sequence: 4, remaining_ms: 500, elapsed_ms: 9000 };
  updateRuntime();
  assert.equal(appState.displayElapsed, 9);

  appState.status = { ...base, run_sequence: 5, remaining_ms: 8000, elapsed_ms: 250 };
  updateRuntime();
  assert.equal(appState.displayElapsed, 0.25);
  assert.equal(appState.displayRemaining, 8);
});

test("activity history loads bounded pages and appends older intervals", async () => {
  const requests = [];
  const pages = new Map([
    [0, {
      activity: [{
        run_id: 8,
        schedule_id: 2,
        name: "Morning",
        manual: false,
        round: 1,
        row: 1,
        zone_id: 1,
        zone_name: "Front",
        started_at: 100,
        watered_ms: 5000,
        active: false,
        result: "Completed",
        commanded_on: "0x00000001",
        commanded_off: "0x00000000",
      }],
      total: 2,
      next_offset: 1,
    }],
    [1, {
      activity: [{
        run_id: 7,
        schedule_id: 0,
        name: "Manual run",
        manual: true,
        round: 1,
        row: 1,
        zone_id: 2,
        zone_name: "Back",
        started_at: 50,
        watered_ms: 3000,
        active: false,
        result: "Stopped",
        commanded_on: "0x00000002",
        commanded_off: "0x00000000",
      }],
      total: 2,
      next_offset: null,
    }],
  ]);
  const fetchImpl = async (url) => {
    const path = url.pathname.split("/").pop();
    const offset = Number(url.searchParams.get("offset"));
    requests.push({ path, offset, limit: Number(url.searchParams.get("limit")) });
    if (path === "history") {
      return {
        ok: true,
        status: 200,
        json: async () => ({
          history: [{ name: "Morning", started_at: 100, watered_seconds: 5, result: "Completed" }],
          total: 1,
          next_offset: null,
        }),
      };
    }
    return { ok: true, status: 200, json: async () => pages.get(offset) };
  };
  const { appState, loadHistory, document } = loadApp(fetchImpl);

  await loadHistory(true);
  assert.equal(appState.history.length, 1);
  assert.equal(appState.historyNextOffset, 1);
  assert.equal(appState.runHistory.length, 1);
  assert.match(document.getElementById("view-history").innerHTML, /Morning/);

  await loadHistory(false);
  assert.equal(appState.history.length, 2);
  assert.equal(appState.historyNextOffset, null);
  assert.deepEqual(requests, [
    { path: "activity", offset: 0, limit: 10 },
    { path: "history", offset: 0, limit: 10 },
    { path: "activity", offset: 1, limit: 10 },
  ]);
  assert.match(document.getElementById("view-history").innerHTML, /Manual run/);
});
