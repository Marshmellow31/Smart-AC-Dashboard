/**
 * AC Control — frontend for the ESP32 AC controller.
 *
 * Plain ES2020, no dependencies; served gzipped from LittleFS. Talks to the
 * REST API in WebServerManager.cpp. The device is the single source of truth:
 * every mutation POSTs and re-renders from the confirmed response, with an
 * optimistic paint in between for snappy controls.
 *
 * Layout of this file:
 *   1. constants & tiny helpers          5. automation: timers/schedules/programs
 *   2. API client (with timeout)         6. 24h timeline projection
 *   3. dial widget                       7. stats (tiles + daily chart)
 *   4. control tab                       8. settings, log, tabs, boot/polling
 */
(() => {
  "use strict";

  // ------------------------------------------------------------------
  // 1. Constants & helpers

  const MIN_TEMP = 16;
  const MAX_TEMP = 30;
  const MODES = ["cool", "dry", "fan", "auto", "heat"];
  const FAN_ORDER = ["auto", "low", "medium", "high"];
  const FAN_LABELS = ["Auto", "Low", "Medium", "High"];
  const DAY_NAMES = ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"];
  const TABS = ["control", "automation", "stats", "settings"];
  const MODE_META = {
    cool: { label: "Cool", accent: "#4fb0ff" },
    dry: { label: "Dry", accent: "#4fd6c2" },
    fan: { label: "Fan", accent: "#9aa7bd" },
    auto: { label: "Auto", accent: "#b48cff" },
    heat: { label: "Heat", accent: "#ff9d4f" },
  };
  const OFF_ACCENT = "#3a4356";
  const DAY_SECONDS = 24 * 3600;

  const $ = (id) => document.getElementById(id);
  const el = {};
  [
    "connDot", "toast", "autoStatus", "themeToggle",
    "syncGate", "syncSpinner", "syncIcon", "syncTitle", "syncMsg", "syncRetry",
    "stateDot", "stateText", "stateSub", "stateNext",
    "powerToggle", "powerState", "tempDial", "tempValue", "tempModeLabel",
    "tempDown", "tempUp", "modeGrid", "fanGrid",
    "presetButtons",
    "tlSummary", "tlWrap", "tlMarkers", "tlTrack", "tlTicks", "tlNotes", "tlTip",
    "timerDial", "timerMin", "timerOffSeg", "timerOnSeg", "timerOnOpts",
    "timerTempDown", "timerTempValue", "timerTempUp", "timerAdd", "timerList",
    "scheduleAdd", "scheduleList", "scheduleSave", "scheduleSaveBar",
    "programAdd", "programList", "programSave", "programSaveBar",
    "statCards", "usageChart", "chartMeta", "chartTip",
    "filterText", "filterFill", "filterReset", "logBox",
    "setAuto", "setRestore", "setHold", "setWatts", "setTariff",
    "setFilter", "setSafety", "settingsSave",
  ].forEach((id) => { el[id] = $(id); });

  const esc = (s) => String(s == null ? "" : s).replace(/[&<>"']/g,
      (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

  const fmtClock = (epoch) =>
      new Date(epoch * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  const fmtDayTime = (epoch) => epoch
      ? new Date(epoch * 1000).toLocaleString([], { weekday: "short", hour: "2-digit", minute: "2-digit" })
      : "";
  const fmtDur = (sec) => {
    if (sec >= 3600) {
      const h = Math.floor(sec / 3600), m = Math.round((sec % 3600) / 60);
      return m > 0 ? `${h}h ${m}m` : `${h}h`;
    }
    return `${Math.max(0, Math.round(sec / 60))}m`;
  };
  const fmtHours = (sec) => (sec / 3600).toFixed(1) + " h";
  const money = (v) => "₹" + ((v || 0) >= 100 ? Math.round(v) : (v || 0).toFixed(1));

  let toastTimer = null;
  function showToast(message, ok) {
    el.toast.textContent = message;
    el.toast.classList.toggle("ok", !!ok);
    el.toast.classList.add("visible");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.toast.classList.remove("visible"), 3200);
  }

  function setConnected(online) {
    el.connDot.classList.toggle("online", online);
    el.connDot.classList.toggle("offline", !online);
  }

  // Theme: dark by default, honours the OS preference on first run, then
  // remembers the user's manual choice. The <meta name="theme-color"> is kept
  // in sync so the mobile status bar matches the app background.
  const THEME_BG = { dark: "#0b0d12", light: "#f4f5f8" };
  const themeMeta = document.querySelector('meta[name="theme-color"]');

  function applyTheme(theme) {
    document.documentElement.setAttribute("data-theme", theme);
    if (themeMeta) themeMeta.setAttribute("content", THEME_BG[theme]);
    el.themeToggle.textContent = theme === "dark" ? "☾" : "☀";
    el.themeToggle.setAttribute("aria-label", theme === "dark" ? "Switch to light mode" : "Switch to dark mode");
  }

  function initTheme() {
    let theme = null;
    try { theme = localStorage.getItem("ac-theme"); } catch (_) { /* private mode */ }
    if (theme !== "dark" && theme !== "light") {
      theme = window.matchMedia && window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
    }
    applyTheme(theme);
  }

  el.themeToggle.addEventListener("click", () => {
    const next = document.documentElement.getAttribute("data-theme") === "dark" ? "light" : "dark";
    applyTheme(next);
    try { localStorage.setItem("ac-theme", next); } catch (_) { /* ignore */ }
  });
  initTheme();

  function setPending(elements, pending) {
    elements.forEach((e) => e && e.classList.toggle("is-pending", pending));
  }

  // ------------------------------------------------------------------
  // 2. API client

  const REQUEST_TIMEOUT_MS = 8000;

  async function request(path, options = {}) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), REQUEST_TIMEOUT_MS);
    try {
      const res = await fetch(path, { ...options, signal: ctl.signal });
      if (!res.ok) {
        let message = `Request failed (${res.status})`;
        try { message = (await res.json()).error || message; } catch (_) { /* not JSON */ }
        throw new Error(message);
      }
      return await res.json();
    } catch (err) {
      if (err.name === "AbortError") throw new Error("Device not responding");
      throw err;
    } finally {
      clearTimeout(timer);
    }
  }

  const apiGet = (path) => request(path);
  const apiPost = (path, body) => request(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body === undefined ? {} : body),
  });

  // ------------------------------------------------------------------
  // App state — mirrors of the device's data, refreshed by polling.

  const app = {
    state: { power: false, mode: "cool", temp: 24, fan: "auto" },
    status: {},        // /api/status
    settings: { acWatts: 1560, tariffPerKwh: 8 },
    schedules: [],     // saved server state (drives the timeline)
    programs: [],
    timers: [],
    stats: null,
  };

  // ------------------------------------------------------------------
  // 3. Dial widget: 270° sweep with a gap at the bottom. Pointer + keyboard.

  function makeDial(node, { min, max, step, get, set, commit }) {
    const frac = (v) => (v - min) / (max - min);
    function paint(v) {
      const deg = 270 * Math.max(0, Math.min(1, frac(v)));
      node.style.setProperty("--deg", deg + "deg");
      node.style.setProperty("--a", (-135 + deg) + "deg");
      node.setAttribute("aria-valuenow", v);
    }
    function fromEvent(e) {
      const r = node.getBoundingClientRect();
      let a = Math.atan2(e.clientY - r.top - r.height / 2, e.clientX - r.left - r.width / 2) * 180 / Math.PI;
      a = (a - 135 + 360) % 360;
      if (a > 270) a = a > 315 ? 0 : 270;
      return Math.round((min + (a / 270) * (max - min)) / step) * step;
    }
    node.addEventListener("pointerdown", (e) => {
      node.setPointerCapture(e.pointerId);
      const move = (ev) => { const v = fromEvent(ev); if (v !== get()) { set(v); paint(v); } };
      move(e);
      node.onpointermove = move;
      node.onpointerup = node.onpointercancel = () => { node.onpointermove = null; commit(get()); };
    });
    node.addEventListener("keydown", (e) => {
      let v = get();
      if (e.key === "ArrowRight" || e.key === "ArrowUp") v = Math.min(max, v + step);
      else if (e.key === "ArrowLeft" || e.key === "ArrowDown") v = Math.max(min, v - step);
      else return;
      e.preventDefault();
      set(v); paint(v); commit(v);
    });
    paint(get());
    return { paint };
  }

  // ------------------------------------------------------------------
  // 4. Control tab

  let tempDraft = app.state.temp;

  // Tick ring around the temperature dial: one dot per degree, lit up to the
  // current setting. Built once, positioned on the same 270° sweep as the thumb.
  const tempTicks = (() => {
    const frag = document.createDocumentFragment();
    const nodes = [];
    for (let i = 0; i <= MAX_TEMP - MIN_TEMP; i++) {
      const deg = -135 + 270 * (i / (MAX_TEMP - MIN_TEMP));
      const tick = document.createElement("span");
      tick.className = "dial-tick";
      tick.style.transform = `rotate(${deg}deg) translateY(-92px)`;
      frag.appendChild(tick);
      nodes.push(tick);
    }
    el.tempDial.appendChild(frag);
    return nodes;
  })();
  function paintTempTicks(v) {
    const lit = v - MIN_TEMP;
    tempTicks.forEach((t, i) => t.classList.toggle("on", i <= lit));
  }

  const tempDialCtl = makeDial(el.tempDial, {
    min: MIN_TEMP, max: MAX_TEMP, step: 1,
    get: () => tempDraft,
    set: (v) => { tempDraft = v; el.tempValue.textContent = v; paintTempTicks(v); },
    commit: (v) => {
      tempDraft = v;
      if (v === app.state.temp) return;
      runOptimistic({ ...app.state, temp: v }, [el.tempDial, el.tempDown, el.tempUp], "/api/temp", { value: v });
    },
  });

  function applyAccent(mode, power) {
    const accent = power ? (MODE_META[mode] || MODE_META.cool).accent : OFF_ACCENT;
    const root = document.documentElement.style;
    root.setProperty("--accent", accent);
    root.setProperty("--accent-dim", accent + "22");
  }

  function renderControls(s) {
    app.state = { power: s.power, mode: s.mode, temp: s.temp, fan: s.fan };
    tempDraft = s.temp;

    applyAccent(s.mode, s.power);

    el.powerToggle.setAttribute("aria-checked", String(s.power));
    el.powerState.textContent = s.power ? "On" : "Off";

    el.tempValue.textContent = s.temp;
    el.tempModeLabel.textContent = s.power ? (MODE_META[s.mode] ? MODE_META[s.mode].label : s.mode) : "Standby";
    tempDialCtl.paint(s.temp);
    paintTempTicks(s.temp);
    el.tempDown.disabled = s.temp <= MIN_TEMP;
    el.tempUp.disabled = s.temp >= MAX_TEMP;

    el.modeGrid.querySelectorAll(".mode-btn").forEach((btn) => {
      btn.setAttribute("aria-pressed", String(btn.dataset.mode === s.mode));
    });
    el.fanGrid.querySelectorAll(".fan-seg").forEach((btn) => {
      btn.setAttribute("aria-pressed", String(btn.dataset.fan === s.fan));
    });

    // Persistent state summary bar.
    const modeLabel = MODE_META[s.mode] ? MODE_META[s.mode].label : s.mode;
    const fanLabel = FAN_LABELS[Math.max(0, FAN_ORDER.indexOf(s.fan))] || "Auto";
    el.stateText.textContent = s.power ? `${modeLabel} · ${s.temp}°` : "Off";
    el.stateSub.textContent = s.power ? `Fan ${fanLabel.toLowerCase()}` : "";
    el.stateDot.classList.toggle("on", !!s.power);
  }

  async function runOptimistic(optimisticState, pendingEls, path, body) {
    const previous = app.state;
    renderControls(optimisticState);
    setPending(pendingEls, true);
    try {
      const confirmed = await apiPost(path, body);
      renderStatus(confirmed);
      setConnected(true);
    } catch (err) {
      renderControls(previous);
      setConnected(false);
      showToast(err.message || "ESP32 unreachable");
    } finally {
      setPending(pendingEls, false);
    }
  }

  el.powerToggle.addEventListener("click", () => {
    const next = { ...app.state, power: !app.state.power };
    runOptimistic(next, [el.powerToggle], "/api/power", { on: next.power });
  });
  el.tempDown.addEventListener("click", () => {
    if (app.state.temp <= MIN_TEMP) return;
    const next = { ...app.state, temp: app.state.temp - 1 };
    runOptimistic(next, [el.tempDial, el.tempDown, el.tempUp], "/api/temp", { value: next.temp });
  });
  el.tempUp.addEventListener("click", () => {
    if (app.state.temp >= MAX_TEMP) return;
    const next = { ...app.state, temp: app.state.temp + 1 };
    runOptimistic(next, [el.tempDial, el.tempDown, el.tempUp], "/api/temp", { value: next.temp });
  });
  el.modeGrid.addEventListener("click", (evt) => {
    const btn = evt.target.closest(".mode-btn");
    if (!btn || btn.dataset.mode === app.state.mode) return;
    runOptimistic({ ...app.state, mode: btn.dataset.mode },
        [...el.modeGrid.querySelectorAll(".mode-btn")], "/api/mode", { mode: btn.dataset.mode });
  });
  el.fanGrid.addEventListener("click", (evt) => {
    const btn = evt.target.closest(".fan-seg");
    if (!btn || btn.dataset.fan === app.state.fan) return;
    runOptimistic({ ...app.state, fan: btn.dataset.fan },
        [...el.fanGrid.querySelectorAll(".fan-seg")], "/api/fan", { speed: btn.dataset.fan });
  });

  // Presets

  async function loadPresets() {
    try {
      const data = await apiGet("/api/presets");
      el.presetButtons.innerHTML = (data.presets || []).map((p) =>
          `<button type="button" class="preset-btn" data-preset="${esc(p.name)}"><span>${esc(p.name)}</span>` +
          `<span>${p.action.power === false ? "turn off" : esc((p.action.temp || "") + "° " + (p.action.mode || ""))}</span></button>`).join("");
    } catch (_) { /* non-critical */ }
  }

  el.presetButtons.addEventListener("click", async (evt) => {
    const btn = evt.target.closest("[data-preset]");
    if (!btn) return;
    try {
      renderStatus(await apiPost("/api/presets/apply", { name: btn.dataset.preset }));
      showToast(`Preset "${btn.dataset.preset}" applied`, true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Status strip + shared status rendering

  function renderStatus(st) {
    const prevProgId = app.status.program && app.status.program.active ? app.status.program.id : null;
    const prevTimerCount = app.status.timerCount;
    app.status = st;
    renderControls(st);

    const rows = [];
    let noteworthy = false;
    if (!st.timeValid) {
      rows.push(`<div class="warn">Device time not synced yet — automations on hold</div>`);
      noteworthy = true;
    }
    if (st.override && st.override.active) {
      rows.push(`<div>Manual hold active until <b>${fmtDayTime(st.override.until)}</b> (schedules paused) <button class="btn-ghost" id="clearHold">Resume automations</button></div>`);
      noteworthy = true;
    }
    if (st.program && st.program.active) {
      rows.push(`<div>Program running: <b>${esc(st.program.name || st.program.id)}</b>, step ${st.program.step}${st.program.endsAt ? " · ends " + fmtDayTime(st.program.endsAt) : ""}</div>`);
      noteworthy = true;
    }
    if (!st.automationEnabled) {
      rows.push(`<div class="warn">Weekly schedules are disabled in Settings.</div>`);
      noteworthy = true;
    }
    el.autoStatus.innerHTML = rows.join("");
    el.autoStatus.classList.toggle("visible", noteworthy);

    const clearBtn = $("clearHold");
    if (clearBtn) clearBtn.onclick = async () => {
      try { renderStatus(await apiPost("/api/override/clear")); } catch (e) { showToast(e.message); }
    };

    // A fired/cancelled timer or a program transition changes what the
    // timeline shows — refresh the mirrors that changed.
    const curProgId = st.program && st.program.active ? st.program.id : null;
    if (curProgId !== prevProgId) refreshProgramRunStates();
    if (prevTimerCount !== undefined && st.timerCount !== prevTimerCount) loadTimers();

    renderTimeline();
  }

  async function refreshStatus() {
    try {
      renderStatus(await apiGet("/api/status"));
      setConnected(true);
    } catch (_) {
      setConnected(false);
    }
  }

  // ------------------------------------------------------------------
  // 5a. Countdown timers

  let timerMinDraft = 60;
  let timerOn = false;
  let timerTempDraft = 24;

  makeDial(el.timerDial, {
    min: 5, max: 240, step: 5,
    get: () => timerMinDraft,
    set: (v) => { timerMinDraft = v; el.timerMin.textContent = v; },
    commit: (v) => { timerMinDraft = v; },
  });

  function paintTimerSegment() {
    el.timerOffSeg.setAttribute("aria-pressed", String(!timerOn));
    el.timerOnSeg.setAttribute("aria-pressed", String(timerOn));
    el.timerOnOpts.style.display = timerOn ? "" : "none";
  }
  el.timerOffSeg.addEventListener("click", () => { timerOn = false; paintTimerSegment(); });
  el.timerOnSeg.addEventListener("click", () => { timerOn = true; paintTimerSegment(); });

  function paintTimerTemp() { el.timerTempValue.textContent = timerTempDraft + "°"; }
  el.timerTempDown.addEventListener("click", () => { timerTempDraft = Math.max(MIN_TEMP, timerTempDraft - 1); paintTimerTemp(); });
  el.timerTempUp.addEventListener("click", () => { timerTempDraft = Math.min(MAX_TEMP, timerTempDraft + 1); paintTimerTemp(); });

  function renderTimers(data) {
    app.timers = data.timers || [];
    el.timerList.innerHTML = app.timers.map((t) => {
      const what = t.action.power === false ? "Turn OFF" : `Turn ON ${t.action.temp || ""}°`;
      return `<div class="timer-row"><span>${what} in <b>${fmtDur(t.remainingSec)}</b> · ${fmtDayTime(t.fireAt)}</span>` +
             `<button type="button" class="btn-text-danger" data-cancel="${t.id}">Cancel</button></div>`;
    }).join("") || `<div class="faint-12">No timers set.</div>`;
    renderTimeline();
  }

  el.timerList.addEventListener("click", async (evt) => {
    const btn = evt.target.closest("[data-cancel]");
    if (!btn) return;
    try { renderTimers(await apiPost("/api/timers/cancel", { id: Number(btn.dataset.cancel) })); }
    catch (e) { showToast(e.message); }
  });

  el.timerAdd.addEventListener("click", async () => {
    const action = timerOn ? { power: true, temp: timerTempDraft, mode: "cool" } : { power: false };
    try {
      renderTimers(await apiPost("/api/timers", { minutes: timerMinDraft, action }));
      showToast("Timer set", true);
    } catch (e) { showToast(e.message); }
  });

  async function loadTimers() {
    try { renderTimers(await apiGet("/api/timers")); } catch (_) { /* retried by poll */ }
  }

  // ------------------------------------------------------------------
  // 5b. Weekly schedules editor

  const modeOptions = (sel) => MODES.map((m) => `<option ${m === sel ? "selected" : ""}>${m}</option>`).join("");
  const fanOptions = (sel) => FAN_ORDER.map((f) => `<option ${f === sel ? "selected" : ""}>${f}</option>`).join("");

  function daysDesc(days) {
    const sorted = [...days].sort();
    const key = sorted.join(",");
    if (sorted.length === 7) return "Every day";
    if (key === "1,2,3,4,5") return "Weekdays";
    if (key === "0,6") return "Weekends";
    if (!sorted.length) return "No days";
    return sorted.map((d) => DAY_NAMES[d]).join(",");
  }
  function actionDesc(action) {
    if (action.power === false) return "turn off";
    return `${action.temp}° ${action.mode} · fan ${action.fan}`;
  }
  function dayChips(days) {
    return DAY_NAMES.map((d, di) =>
        `<label class="day-chip"><input type="checkbox" data-day="${di}" ${days.includes(di) ? "checked" : ""}><span>${d}</span></label>`).join("") +
        `<button type="button" class="day-quick" data-days="0,1,2,3,4,5,6">All</button>` +
        `<button type="button" class="day-quick" data-days="1,2,3,4,5">Weekdays</button>` +
        `<button type="button" class="day-quick" data-days="0,6">Weekend</button>`;
  }

  function markScheduleDirty() { el.scheduleSaveBar.hidden = false; }
  function markProgramDirty() { el.programSaveBar.hidden = false; }

  function renderSchedules() {
    el.scheduleList.innerHTML = app.schedules.map((s, i) => {
      const off = s.action.power === false;
      return `<div class="row-card" data-idx="${i}">
        <div class="row-summary">
          <button type="button" class="row-main" data-toggle>
            <span class="row-name">${esc(s.name)}</span>
            <span class="row-desc">${esc(daysDesc(s.days))} · ${esc(actionDesc(s.action))}</span>
          </button>
          <span class="row-time">${esc(s.time)}</span>
          <button type="button" class="switch small s-enabled-switch" role="switch" aria-checked="${s.enabled ? "true" : "false"}" aria-label="Enable schedule"><span class="switch-knob"></span></button>
        </div>
        <div class="row-editor" hidden>
          <div class="field-row">
            <input type="text" class="s-name in-txt" value="${esc(s.name)}" placeholder="name" maxlength="23">
            <input type="time" class="s-time in-sel" value="${esc(s.time)}" aria-label="Time">
            <button type="button" class="btn-text-danger s-del">Delete</button>
          </div>
          <div class="day-chips">${dayChips(s.days)}</div>
          <div class="field-row">
            <select class="s-power in-sel"><option value="on" ${off ? "" : "selected"}>Turn ON</option><option value="off" ${off ? "selected" : ""}>Turn OFF</option></select>
            <span class="s-onopts field-row" ${off ? 'style="display:none"' : ""}>
              <input type="number" class="s-temp in-num" min="16" max="30" value="${s.action.temp || 24}" aria-label="Temperature">°
              <select class="s-mode in-sel">${modeOptions(s.action.mode || "cool")}</select>
              <select class="s-fan in-sel">${fanOptions(s.action.fan || "auto")}</select>
            </span>
          </div>
        </div>
      </div>`;
    }).join("") || `<div class="faint-12">No schedules yet. Add one, e.g. ON at 23:00 every day at 24°.</div>`;
  }

  function readSchedules() {
    return [...el.scheduleList.querySelectorAll(".row-card")].map((row) => {
      const off = row.querySelector(".s-power").value === "off";
      const action = off ? { power: false } : {
        power: true,
        temp: Number(row.querySelector(".s-temp").value),
        mode: row.querySelector(".s-mode").value,
        fan: row.querySelector(".s-fan").value,
      };
      return {
        name: row.querySelector(".s-name").value || "schedule",
        enabled: row.querySelector(".s-enabled-switch").getAttribute("aria-checked") === "true",
        time: row.querySelector(".s-time").value || "00:00",
        days: [...row.querySelectorAll("[data-day]")].filter((c) => c.checked).map((c) => Number(c.dataset.day)),
        action,
      };
    });
  }

  el.scheduleList.addEventListener("click", (evt) => {
    const quick = evt.target.closest(".day-quick");
    if (quick) {
      const wanted = quick.dataset.days.split(",").map(Number);
      quick.closest(".day-chips").querySelectorAll("[data-day]").forEach((c) => {
        c.checked = wanted.includes(Number(c.dataset.day));
      });
      markScheduleDirty();
      return;
    }
    const toggleBtn = evt.target.closest("[data-toggle]");
    if (toggleBtn) {
      const editor = toggleBtn.closest(".row-card").querySelector(".row-editor");
      editor.hidden = !editor.hidden;
      return;
    }
    const enableBtn = evt.target.closest(".s-enabled-switch");
    if (enableBtn) {
      const checked = enableBtn.getAttribute("aria-checked") === "true";
      enableBtn.setAttribute("aria-checked", String(!checked));
      markScheduleDirty();
      return;
    }
    if (evt.target.closest(".s-del")) {
      app.schedules = readSchedules();
      app.schedules.splice(Number(evt.target.closest(".row-card").dataset.idx), 1);
      renderSchedules();
      markScheduleDirty();
    }
  });
  el.scheduleList.addEventListener("change", (evt) => {
    if (evt.target.classList.contains("s-power")) {
      const opts = evt.target.closest(".row-editor").querySelector(".s-onopts");
      opts.style.display = evt.target.value === "off" ? "none" : "";
    }
    markScheduleDirty();
  });
  el.scheduleList.addEventListener("input", markScheduleDirty);

  el.scheduleAdd.addEventListener("click", () => {
    app.schedules = readSchedules();
    app.schedules.push({ name: "Schedule", enabled: true, time: "22:00",
        days: [0, 1, 2, 3, 4, 5, 6], action: { power: true, temp: 24, mode: "cool", fan: "auto" } });
    renderSchedules();
    markScheduleDirty();
    const editor = el.scheduleList.querySelector(`.row-card[data-idx="${app.schedules.length - 1}"] .row-editor`);
    if (editor) editor.hidden = false;
  });

  el.scheduleSave.addEventListener("click", async () => {
    const slots = readSchedules();
    const bad = slots.find((s) => s.days.length === 0);
    if (bad) { showToast(`Schedule "${bad.name}" needs at least one day`); return; }
    try {
      const data = await apiPost("/api/schedules", { slots });
      app.schedules = data.slots || [];
      renderSchedules();
      el.scheduleSaveBar.hidden = true;
      showToast("Schedules saved", true);
      refreshStatus();
    } catch (e) { showToast(e.message); }
  });

  async function loadSchedules() {
    try {
      app.schedules = (await apiGet("/api/schedules")).slots || [];
      renderSchedules();
      renderTimeline();
    } catch (_) { /* retried on next visit */ }
  }

  // ------------------------------------------------------------------
  // 5c. Programs editor + start/stop

  const programOnSeconds = (p) =>
      p.steps.reduce((acc, s) => acc + (s.on ? s.minutes * 60 : 0), 0);
  const programTotalSeconds = (p) =>
      p.steps.reduce((acc, s) => acc + s.minutes * 60, 0);

  function programRunCost(p) {
    const kwh = programOnSeconds(p) / 3600 * (app.settings.acWatts || 0) / 1000;
    return kwh * (app.settings.tariffPerKwh || 0);
  }

  // Cool steps get a temperature tint (colder = deeper blue) so a sleep curve
  // 24°→26° is visible at a glance; other modes use their flat accent.
  function stepColor(step) {
    if (!step.on) return null;
    const mode = step.mode || "cool";
    if (mode !== "cool" && mode !== "auto") return MODE_META[mode].accent;
    const t = Math.max(0, Math.min(1, ((step.temp || 24) - MIN_TEMP) / (MAX_TEMP - MIN_TEMP)));
    const from = [126, 197, 255], to = [37, 84, 128];  // 16° bright → 30° muted
    const c = from.map((f, i) => Math.round(f + (to[i] - f) * t));
    return `rgb(${c[0]},${c[1]},${c[2]})`;
  }

  function stepbarHtml(steps) {
    const total = steps.reduce((acc, s) => acc + s.minutes, 0);
    if (!total) return "";
    return `<div class="stepbar">` + steps.map((s) => {
      const w = (s.minutes / total * 100).toFixed(2);
      const color = stepColor(s);
      const style = color ? `background:${color}` : "";
      const label = s.on ? `ON ${s.temp}° for ${fmtDur(s.minutes * 60)}` : `OFF for ${fmtDur(s.minutes * 60)}`;
      return `<div class="stepbar-seg${s.on ? "" : " off"}" style="width:${w}%;${style}" title="${esc(label)}"></div>`;
    }).join("") + `</div>`;
  }

  function programDesc(p) {
    if (!p.steps.length) return "No steps";
    const parts = [`${p.steps.length} step${p.steps.length > 1 ? "s" : ""}`, fmtDur(programTotalSeconds(p))];
    const cost = programRunCost(p);
    if (cost > 0) parts.push(`≈${money(cost)}${p.repeat ? "/cycle" : "/run"}`);
    if (p.repeat) parts.push("repeats" + (p.endTime ? " until " + p.endTime : ""));
    else if (p.endTime) parts.push("cut off at " + p.endTime);
    return parts.join(" · ");
  }

  function programActionHtml(p) {
    const running = app.status.program && app.status.program.active && app.status.program.id === p.id;
    if (running) return `<button type="button" class="btn-danger-outline" data-stop="${esc(p.id)}">Stop</button>`;
    return `<input type="time" class="p-until in-sel" aria-label="Run until" title="Optional end time" value="${esc(p.endTime || "")}">` +
           `<button type="button" class="btn-primary" data-start="${esc(p.id)}">Start</button>`;
  }

  function refreshProgramRunStates() {
    el.programList.querySelectorAll(".row-action").forEach((span) => {
      const p = app.programs.find((x) => x.id === span.dataset.pid);
      if (p) span.innerHTML = programActionHtml(p);
    });
  }

  function stepEditorHtml(s, si, count) {
    return `
      <div class="field-row step-row" data-step="${si}">
        <span class="step-order">
          <button type="button" class="p-stepup" ${si === 0 ? "disabled" : ""} aria-label="Move step up">▲</button>
          <button type="button" class="p-stepdown" ${si === count - 1 ? "disabled" : ""} aria-label="Move step down">▼</button>
        </span>
        <select class="p-on in-sel"><option value="on" ${s.on ? "selected" : ""}>ON</option><option value="off" ${s.on ? "" : "selected"}>OFF</option></select>
        <input type="number" class="p-min in-num" min="1" max="1440" value="${s.minutes}" aria-label="Minutes"> min
        <span class="p-onopts field-row" ${s.on ? "" : 'style="display:none"'}>
          <input type="number" class="p-temp in-num" min="16" max="30" value="${s.temp || 24}" aria-label="Temperature">°
          <select class="p-mode in-sel">${modeOptions(s.mode || "cool")}</select>
          <select class="p-fan in-sel">${fanOptions(s.fan || "auto")}</select>
        </span>
        <button type="button" class="btn-text-danger p-stepdel" aria-label="Delete step">✕</button>
      </div>`;
  }

  function renderPrograms() {
    el.programList.innerHTML = app.programs.map((p, i) => `
      <div class="row-card" data-idx="${i}">
        <div class="row-summary">
          <button type="button" class="row-main" data-toggle>
            <span class="row-name">${esc(p.name)}</span>
            <span class="row-desc">${esc(programDesc(p))}</span>
            ${stepbarHtml(p.steps)}
          </button>
          <span class="row-action" data-pid="${esc(p.id)}">${programActionHtml(p)}</span>
        </div>
        <div class="row-editor" hidden>
          <div class="field-row">
            <input type="text" class="p-name in-txt" value="${esc(p.name)}" placeholder="name" maxlength="39">
            <input type="hidden" class="p-id" value="${esc(p.id)}">
            <label class="dim-13" style="display:flex;align-items:center;gap:6px"><input type="checkbox" class="p-repeat" ${p.repeat ? "checked" : ""}> repeat</label>
            <span class="field-label">default end</span>
            <input type="time" class="p-end in-sel" value="${esc(p.endTime || "")}">
            <button type="button" class="btn-text-danger p-del">Delete</button>
          </div>
          ${p.steps.map((s, si) => stepEditorHtml(s, si, p.steps.length)).join("")}
          <div class="field-row">
            <button type="button" class="btn-ghost p-stepadd">+ step</button>
            <span class="field-label p-total">total ${fmtDur(programTotalSeconds(p))}</span>
          </div>
        </div>
      </div>`).join("") || `<div class="faint-12">No programs yet. Programs chain timed steps — e.g. a sleep curve that raises the temperature overnight.</div>`;
  }

  function readPrograms() {
    return [...el.programList.querySelectorAll(".row-card")].map((row) => ({
      id: row.querySelector(".p-id").value || "p" + Math.random().toString(36).slice(2, 8),
      name: row.querySelector(".p-name").value || "program",
      repeat: row.querySelector(".p-repeat").checked,
      endTime: row.querySelector(".p-end").value || "",
      steps: [...row.querySelectorAll(".step-row")].map((sr) => {
        const on = sr.querySelector(".p-on").value === "on";
        const step = { on, minutes: Number(sr.querySelector(".p-min").value) };
        if (on) {
          step.temp = Number(sr.querySelector(".p-temp").value);
          step.mode = sr.querySelector(".p-mode").value;
          step.fan = sr.querySelector(".p-fan").value;
        }
        return step;
      }),
    }));
  }

  // Re-read the DOM, mutate one program, re-render with the editor kept open.
  function mutateProgram(idx, fn) {
    app.programs = readPrograms();
    fn(app.programs[idx]);
    renderPrograms();
    markProgramDirty();
    const editor = el.programList.querySelector(`.row-card[data-idx="${idx}"] .row-editor`);
    if (editor) editor.hidden = false;
  }

  el.programList.addEventListener("click", async (evt) => {
    const startBtn = evt.target.closest("[data-start]");
    if (startBtn) {
      const until = startBtn.closest(".row-action").querySelector(".p-until").value || "";
      try {
        renderStatus(await apiPost("/api/program/start", { id: startBtn.dataset.start, endTime: until }));
        showToast("Program started", true);
      } catch (e) { showToast(e.message); }
      return;
    }
    const stopBtn = evt.target.closest("[data-stop]");
    if (stopBtn) {
      try { renderStatus(await apiPost("/api/program/stop")); showToast("Program stopped", true); }
      catch (e) { showToast(e.message); }
      return;
    }
    const toggleBtn = evt.target.closest("[data-toggle]");
    if (toggleBtn) {
      const editor = toggleBtn.closest(".row-card").querySelector(".row-editor");
      editor.hidden = !editor.hidden;
      return;
    }
    const row = evt.target.closest(".row-card");
    if (!row) return;
    const idx = Number(row.dataset.idx);
    const stepRow = evt.target.closest(".step-row");
    const stepIdx = stepRow ? Number(stepRow.dataset.step) : -1;

    if (evt.target.closest(".p-del")) {
      app.programs = readPrograms();
      app.programs.splice(idx, 1);
      renderPrograms();
      markProgramDirty();
    } else if (evt.target.closest(".p-stepadd")) {
      mutateProgram(idx, (p) => p.steps.push({ on: true, minutes: 30, temp: 24, mode: "cool", fan: "auto" }));
    } else if (evt.target.closest(".p-stepdel")) {
      mutateProgram(idx, (p) => p.steps.splice(stepIdx, 1));
    } else if (evt.target.closest(".p-stepup") && stepIdx > 0) {
      mutateProgram(idx, (p) => p.steps.splice(stepIdx - 1, 0, p.steps.splice(stepIdx, 1)[0]));
    } else if (evt.target.closest(".p-stepdown") && stepIdx >= 0) {
      mutateProgram(idx, (p) => p.steps.splice(stepIdx + 1, 0, p.steps.splice(stepIdx, 1)[0]));
    }
  });
  el.programList.addEventListener("change", (evt) => {
    if (evt.target.classList.contains("p-until")) return;
    if (evt.target.classList.contains("p-on")) {
      const opts = evt.target.closest(".step-row").querySelector(".p-onopts");
      opts.style.display = evt.target.value === "off" ? "none" : "";
    }
    markProgramDirty();
  });
  el.programList.addEventListener("input", (evt) => {
    if (evt.target.classList.contains("p-until")) return;
    markProgramDirty();
  });

  el.programAdd.addEventListener("click", () => {
    app.programs = readPrograms();
    app.programs.push({ id: "", name: "New program", repeat: false, endTime: "",
        steps: [{ on: true, minutes: 60, temp: 24, mode: "cool", fan: "auto" }] });
    renderPrograms();
    markProgramDirty();
    const editor = el.programList.querySelector(`.row-card[data-idx="${app.programs.length - 1}"] .row-editor`);
    if (editor) editor.hidden = false;
  });

  el.programSave.addEventListener("click", async () => {
    const programs = readPrograms();
    const bad = programs.find((p) => p.steps.length === 0);
    if (bad) { showToast(`Program "${bad.name}" needs at least one step`); return; }
    try {
      const data = await apiPost("/api/programs", { programs });
      app.programs = data.programs || [];
      renderPrograms();
      el.programSaveBar.hidden = true;
      showToast("Programs saved", true);
      renderTimeline();
    } catch (e) { showToast(e.message); }
  });

  async function loadPrograms() {
    try {
      app.programs = (await apiGet("/api/programs")).programs || [];
      renderPrograms();
      renderTimeline();
    } catch (_) { /* retried on next visit */ }
  }

  // ------------------------------------------------------------------
  // 6. 24h timeline: simulate the device's own automation rules forward.
  //
  // Mirrors AutomationEngine/AcController behaviour:
  //  - schedules are skipped while a program runs, while a manual hold is
  //    active, or when automation is disabled;
  //  - countdown timers always fire, and cancel a running program;
  //  - a repeat program cycles until its end time (then the AC turns off);
  //  - a one-shot program keeps the last step's state when it completes.

  function cmdApply(state, cmd) {
    const n = { ...state };
    if (cmd.power !== undefined) n.power = cmd.power;
    if (cmd.temp !== undefined) n.temp = cmd.temp;
    if (cmd.mode !== undefined) n.mode = cmd.mode;
    if (cmd.fan !== undefined) n.fan = cmd.fan;
    return n;
  }

  function scheduleOccurrences(slot, t0, t1) {
    const [hh, mm] = (slot.time || "0:0").split(":").map(Number);
    const out = [];
    for (let offset = 0; offset <= 1; offset++) {
      const d = new Date(t0 * 1000);
      d.setDate(d.getDate() + offset);
      d.setHours(hh, mm, 0, 0);
      const t = Math.floor(d.getTime() / 1000);
      if (t > t0 && t <= t1 && slot.days.includes(d.getDay())) out.push(t);
    }
    return out;
  }

  function stepCmd(step) {
    return step.on
        ? { power: true, temp: step.temp, mode: step.mode, fan: step.fan }
        : { power: false };
  }

  function expandProgramEvents(prog, startedAt, endsAt, t0, t1) {
    const events = [];
    const total = programTotalSeconds(prog);
    if (!total || !prog.steps.length) return events;
    const hardEnd = endsAt > 0 ? Math.min(endsAt, t1) : t1;

    if (prog.repeat) {
      let cycle = Math.floor(Math.max(0, t0 - startedAt) / total);
      for (; cycle < 400; cycle++) {
        let t = startedAt + cycle * total;
        if (t >= hardEnd) break;
        for (const step of prog.steps) {
          if (t > t0 && t < hardEnd) {
            events.push({ t, kind: "step", cmd: stepCmd(step), label: `${prog.name}: ${step.on ? step.temp + "°" : "off"}` });
          }
          t += step.minutes * 60;
          if (t >= hardEnd) break;
        }
      }
      if (endsAt > 0 && endsAt > t0 && endsAt <= t1) {
        events.push({ t: endsAt, kind: "progEnd", label: `${prog.name} ends — AC off` });
      }
    } else {
      let t = startedAt;
      for (const step of prog.steps) {
        if (t > t0 && t < hardEnd) {
          events.push({ t, kind: "step", cmd: stepCmd(step), label: `${prog.name}: ${step.on ? step.temp + "°" : "off"}` });
        }
        t += step.minutes * 60;
      }
      const done = startedAt + total;
      if (endsAt > 0 && endsAt < done && endsAt > t0 && endsAt <= t1) {
        events.push({ t: endsAt, kind: "progEnd", label: `${prog.name} ends — AC off` });
      } else if (done > t0 && done <= t1) {
        events.push({ t: done, kind: "progDone", label: `${prog.name} completes` });
      }
    }
    return events;
  }

  function simulateNext24h() {
    const st = app.status;
    const t0 = st.epoch;
    const t1 = t0 + DAY_SECONDS;
    const holdUntil = st.override && st.override.active ? st.override.until : 0;

    const events = [];

    for (const t of app.timers) {
      if (t.fireAt > t0 && t.fireAt <= t1) {
        const what = t.action.power === false ? "AC off" : `AC on ${t.action.temp || ""}°`;
        events.push({ t: t.fireAt, kind: "timer", cmd: t.action, label: `Timer: ${what}` });
      }
    }

    for (const slot of app.schedules) {
      if (!slot.enabled) continue;
      for (const t of scheduleOccurrences(slot, t0, t1)) {
        const what = slot.action.power === false ? "off" : `on ${slot.action.temp}°`;
        events.push({ t, kind: "schedule", cmd: slot.action, label: `${slot.name}: ${what}` });
      }
    }

    let progActive = !!(st.program && st.program.active);
    if (progActive) {
      const prog = app.programs.find((p) => p.id === st.program.id);
      if (prog) {
        events.push(...expandProgramEvents(prog, st.program.startedAt, st.program.endsAt || 0, t0, t1));
      } else {
        progActive = false;  // definition not loaded yet — skip projection
      }
    }

    const KIND_ORDER = { progEnd: 0, progDone: 1, step: 2, timer: 3, schedule: 4 };
    events.sort((a, b) => a.t - b.t || KIND_ORDER[a.kind] - KIND_ORDER[b.kind]);

    let cur = { ...app.state };
    let segStart = t0;
    const segments = [];
    const markers = [];
    const pushSeg = (end) => {
      if (end > segStart) segments.push({ start: segStart, end, ...cur });
    };

    for (const e of events) {
      let applied = false;
      let reason = "";
      let next = cur;

      if (e.kind === "schedule") {
        if (!st.automationEnabled) reason = "schedules disabled";
        else if (progActive) reason = "skipped: program running";
        else if (holdUntil && e.t < holdUntil) reason = "skipped: manual hold";
        else { next = cmdApply(cur, e.cmd); applied = true; }
      } else if (e.kind === "timer") {
        next = cmdApply(cur, e.cmd);
        applied = true;
        if (progActive) { progActive = false; reason = "cancels the program"; }
      } else if (e.kind === "step") {
        if (!progActive) continue;
        next = cmdApply(cur, e.cmd);
        applied = true;
      } else if (e.kind === "progEnd") {
        if (!progActive) continue;
        progActive = false;
        next = cmdApply(cur, { power: false });
        applied = true;
      } else if (e.kind === "progDone") {
        if (!progActive) continue;
        progActive = false;
      }

      if (e.kind !== "step") markers.push({ t: e.t, kind: e.kind, label: e.label, applied, reason });
      if (applied) {
        const changed = next.power !== cur.power || next.temp !== cur.temp ||
                        next.mode !== cur.mode || next.fan !== cur.fan;
        if (changed) { pushSeg(e.t); cur = next; segStart = e.t; }
      }
    }
    pushSeg(t1);

    if (holdUntil > t0 && holdUntil <= t1) {
      markers.push({ t: holdUntil, kind: "hold", label: "Manual hold ends — schedules resume", applied: true, reason: "" });
    }

    const onSeconds = segments.reduce((acc, s) => acc + (s.power ? s.end - s.start : 0), 0);
    const kwh = onSeconds / 3600 * (app.settings.acWatts || 0) / 1000;
    return { t0, t1, segments, markers, onSeconds, kwh, cost: kwh * (app.settings.tariffPerKwh || 0) };
  }

  const MARKER_GLYPHS = { schedule: "⏰", timer: "⏳", progEnd: "⏹", progDone: "✔", hold: "✋" };

  function renderTimeline() {
    if (!el.tlTrack) return;
    const st = app.status;
    if (!st.timeValid || !st.epoch) {
      el.tlTrack.innerHTML = "";
      el.tlMarkers.innerHTML = "";
      el.tlTicks.innerHTML = "";
      el.tlSummary.textContent = "";
      el.tlNotes.innerHTML = `<div class="warn">Waiting for the device clock to sync…</div>`;
      if (el.stateNext) el.stateNext.textContent = "—";
      return;
    }

    const sim = simulateNext24h();
    const pct = (t) => ((t - sim.t0) / DAY_SECONDS * 100);

    // Feed the persistent state bar: the next projected change of state.
    if (el.stateNext) {
      const nextSeg = sim.segments.find((seg) => seg.start > sim.t0);
      el.stateNext.textContent = nextSeg
          ? `${nextSeg.power ? "ON " + nextSeg.temp + "°" : "OFF"} at ${fmtClock(nextSeg.start)}`
          : "—";
    }

    // Track: gridlines + ON segments + "now" cursor.
    let html = "";
    for (let h = 4; h < 24; h += 4) html += `<div class="tl-gridline" style="left:${(h / 24 * 100).toFixed(2)}%"></div>`;
    for (const seg of sim.segments) {
      if (!seg.power) continue;
      const left = pct(seg.start);
      const width = pct(seg.end) - left;
      const color = stepColor({ on: true, temp: seg.temp, mode: seg.mode });
      const tempT = (seg.temp - MIN_TEMP) / (MAX_TEMP - MIN_TEMP);
      const label = width > 4 ? `${seg.temp}°` : "";
      const tip = `${fmtClock(seg.start)}–${fmtClock(seg.end)} · ${seg.temp}° ${seg.mode}|${fmtDur(seg.end - seg.start)} on`;
      html += `<div class="tl-seg" data-tip="${esc(tip)}" style="left:${left.toFixed(2)}%;width:calc(${width.toFixed(2)}% - 2px);background:${color};color:${tempT > 0.55 ? "#e8f1fb" : "#08121d"}">${label}</div>`;
    }
    html += `<div class="tl-now" style="left:0"></div>`;
    el.tlTrack.innerHTML = html;

    // Event markers above the track.
    el.tlMarkers.innerHTML = sim.markers.map((m) => {
      const tip = `${fmtClock(m.t)} · ${m.label}${m.reason ? "|" + m.reason : ""}`;
      return `<span class="tl-marker${m.applied ? "" : " skipped"}" data-tip="${esc(tip)}" style="left:${pct(m.t).toFixed(2)}%">${MARKER_GLYPHS[m.kind] || "•"}</span>`;
    }).join("");

    // Ticks: "Now" then the clock time every 4 hours.
    let ticks = `<span class="tl-tick first" style="left:0">Now</span>`;
    for (let h = 4; h <= 24; h += 4) {
      ticks += `<span class="tl-tick${h === 24 ? " last" : ""}" style="left:${(h / 24 * 100).toFixed(2)}%">${fmtClock(sim.t0 + h * 3600)}</span>`;
    }
    el.tlTicks.innerHTML = ticks;

    el.tlSummary.innerHTML = sim.onSeconds > 0
        ? `AC on <b>${fmtHours(sim.onSeconds)}</b> · ${sim.kwh.toFixed(1)} kWh · ≈<b>${money(sim.cost)}</b>`
        : `AC stays off`;

    const notes = [];
    if (st.override && st.override.active) {
      notes.push(`<div class="warn">Manual hold until ${fmtClock(st.override.until)} — schedules before then are skipped.</div>`);
    }
    if (!st.automationEnabled && app.schedules.some((s) => s.enabled)) {
      notes.push(`<div class="warn">Weekly schedules are disabled in Settings, so none of them will fire.</div>`);
    }
    if (!sim.markers.length && !(st.program && st.program.active)) {
      notes.push(`<div>Nothing scheduled — the AC keeps its current state. Add a schedule, timer or program below.</div>`);
    }
    notes.push(`<div class="faint-12">Projection assumes no manual changes. Energy at ${app.settings.acWatts || "?"} W, ₹${app.settings.tariffPerKwh || "?"}/kWh.</div>`);
    el.tlNotes.innerHTML = notes.join("");
  }

  // Shared tooltip for timeline segments/markers (hover + tap).
  function bindTooltip(container, tipEl) {
    const positionTip = (target) => {
      const [head, sub] = (target.dataset.tip || "").split("|");
      tipEl.innerHTML = `<div>${esc(head)}</div>${sub ? `<div class="tip-sub">${esc(sub)}</div>` : ""}`;
      tipEl.hidden = false;
      const card = tipEl.parentElement;
      const cardRect = card.getBoundingClientRect();
      const targetRect = target.getBoundingClientRect();
      let x = targetRect.left + targetRect.width / 2 - cardRect.left - tipEl.offsetWidth / 2;
      x = Math.max(6, Math.min(x, cardRect.width - tipEl.offsetWidth - 6));
      tipEl.style.left = x + "px";
      tipEl.style.top = (targetRect.top - cardRect.top - tipEl.offsetHeight - 8) + "px";
    };
    container.addEventListener("pointerover", (e) => {
      const t = e.target.closest("[data-tip]");
      if (t) positionTip(t);
    });
    container.addEventListener("pointerout", (e) => {
      if (!e.relatedTarget || !e.relatedTarget.closest || !e.relatedTarget.closest("[data-tip]")) tipEl.hidden = true;
    });
    container.addEventListener("click", (e) => {
      const t = e.target.closest("[data-tip]");
      if (t) positionTip(t); else tipEl.hidden = true;
    });
  }
  bindTooltip(el.tlWrap, el.tlTip);

  // ------------------------------------------------------------------
  // 7. Stats: cost tiles + daily on-time chart

  function isoDate(d) {
    return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
  }

  function renderStats() {
    const s = app.stats;
    if (!s) return;
    const tariff = app.settings.tariffPerKwh || 0;
    const watts = app.settings.acWatts || 0;
    const ratePerHour = watts / 1000 * tariff;
    const week = s.week || { onMinutes: 0, kwh: 0, cost: 0 };

    const running = s.continuousOnMinutes > 0;
    const tiles = [
      { label: "Today", value: money(s.today.cost), sub: `${fmtDur(s.today.onMinutes * 60)} on · ${s.today.kwh.toFixed(1)} kWh` },
      { label: "Last 7 days", value: money(week.cost), sub: `${fmtDur(week.onMinutes * 60)} on · ${week.kwh.toFixed(1)} kWh` },
      { label: "This month", value: money(s.month.cost), sub: `${fmtDur(s.month.onMinutes * 60)} on · ${s.month.kwh.toFixed(1)} kWh` },
      { label: "Running now", value: running ? fmtDur(s.continuousOnMinutes * 60) : "–",
        sub: running ? `costing ≈${money(ratePerHour)}/h` : "AC is off" },
    ];
    el.statCards.innerHTML = tiles.map((t) =>
        `<div class="stat-tile"><div class="stat-label">${t.label}</div><div class="stat-value">${t.value}</div><div class="stat-sub">${t.sub}</div></div>`).join("");

    renderUsageChart(s);

    el.filterText.textContent = `${s.filter.hours}h / ${s.filter.limitHours}h`;
    const pctFill = Math.max(0, Math.min(100, (s.filter.hours / Math.max(1, s.filter.limitHours)) * 100));
    el.filterFill.style.width = pctFill + "%";
    el.filterFill.classList.toggle("needs-cleaning", !!s.filter.needsCleaning);
  }

  function renderUsageChart(s) {
    const DAYS = 14;
    const byDate = new Map((s.days || []).map((d) => [d.date, d]));
    const anchor = app.status.epoch ? new Date(app.status.epoch * 1000) : new Date();

    const series = [];
    for (let i = DAYS - 1; i >= 0; i--) {
      const d = new Date(anchor);
      d.setDate(d.getDate() - i);
      const key = isoDate(d);
      const rec = byDate.get(key);
      series.push({
        date: key,
        dayNum: d.getDate(),
        hours: rec ? rec.onMinutes / 60 : 0,
        kwh: rec ? rec.kwh : 0,
        cost: rec ? (rec.cost !== undefined ? rec.cost : rec.kwh * (app.settings.tariffPerKwh || 0)) : 0,
        today: i === 0,
      });
    }

    const maxHours = Math.max(...series.map((d) => d.hours));
    if (maxHours <= 0) {
      el.usageChart.innerHTML = `<div class="chart-empty">No usage recorded yet — data appears once the AC runs.</div>`;
      el.chartMeta.textContent = "";
      return;
    }
    const niceMax = [2, 4, 6, 8, 12, 18, 24].find((v) => v >= maxHours) || 24;
    const maxIdx = series.reduce((best, d, i) => (d.hours > series[best].hours ? i : best), 0);

    // Plot geometry must match .chart-bars insets (top 14px, bottom 22px).
    // When the stats tab is hidden clientHeight is 0 — fall back to the
    // stylesheet height (170px) minus the insets.
    const PLOT_TOP = 14, PLOT_BOTTOM = 22;
    const h = el.usageChart.clientHeight;
    const plotH = h > PLOT_TOP + PLOT_BOTTOM ? h - PLOT_TOP - PLOT_BOTTOM : 134;

    let html = "";
    for (const level of [niceMax, niceMax / 2]) {
      const bottom = PLOT_BOTTOM + (level / niceMax) * plotH;
      html += `<div class="chart-grid" style="bottom:${bottom.toFixed(0)}px"><span class="glabel">${level}h</span><span class="gline"></span></div>`;
    }
    html += `<div class="chart-bars">` + series.map((d, i) => {
      const hPct = (d.hours / niceMax * 100).toFixed(1);
      const showVal = (i === maxIdx || d.today) && d.hours > 0;
      const tip = `${d.date}|${d.hours.toFixed(1)} h on · ${d.kwh.toFixed(1)} kWh · ${money(d.cost)}`;
      return `<div class="bar-col${d.today ? " today" : ""}" data-tip="${esc(tip)}">
        ${showVal ? `<span class="bar-val" style="bottom:calc(${hPct}% + 3px)">${d.hours.toFixed(1)}h</span>` : ""}
        <div class="bar" style="height:${hPct}%${d.hours > 0 ? ";min-height:2px" : ""}"></div>
        <span class="bar-day">${d.today ? "today" : d.dayNum}</span>
      </div>`;
    }).join("") + `</div>`;
    el.usageChart.innerHTML = html;

    const total = series.reduce((acc, d) => acc + d.cost, 0);
    el.chartMeta.textContent = `14-day total ≈ ${money(total)}`;
  }

  bindTooltip(el.usageChart, el.chartTip);

  async function loadStats() {
    try {
      app.stats = await apiGet("/api/stats");
      renderStats();
    } catch (_) { /* refreshed periodically */ }
  }

  el.filterReset.addEventListener("click", async () => {
    try { await apiPost("/api/filter/reset"); showToast("Filter counter reset", true); loadStats(); }
    catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // 8. Settings

  el.setAuto.addEventListener("click", () => {
    el.setAuto.setAttribute("aria-checked", String(el.setAuto.getAttribute("aria-checked") !== "true"));
  });
  el.setRestore.addEventListener("click", () => {
    el.setRestore.setAttribute("aria-checked", String(el.setRestore.getAttribute("aria-checked") !== "true"));
  });

  async function loadSettings() {
    try {
      const s = await apiGet("/api/settings");
      app.settings = s;
      el.setAuto.setAttribute("aria-checked", String(!!s.automationEnabled));
      el.setRestore.setAttribute("aria-checked", String(!!s.restoreOnBoot));
      el.setHold.value = s.holdMinutes;
      el.setWatts.value = s.acWatts;
      el.setTariff.value = s.tariffPerKwh;
      el.setFilter.value = s.filterLimitHours;
      el.setSafety.value = s.maxContinuousHours;
    } catch (_) { /* defaults remain */ }
  }

  el.settingsSave.addEventListener("click", async () => {
    try {
      const s = await apiPost("/api/settings", {
        holdMinutes: Number(el.setHold.value),
        automationEnabled: el.setAuto.getAttribute("aria-checked") === "true",
        restoreOnBoot: el.setRestore.getAttribute("aria-checked") === "true",
        acWatts: Number(el.setWatts.value),
        tariffPerKwh: Number(el.setTariff.value),
        filterLimitHours: Number(el.setFilter.value),
        maxContinuousHours: Number(el.setSafety.value),
      });
      app.settings = s;
      showToast("Settings saved", true);
      refreshStatus();
      loadStats();
    } catch (e) { showToast(e.message); }
  });

  // Event log

  async function loadLog() {
    try {
      const data = await apiGet("/api/log");
      el.logBox.innerHTML = (data.events || []).map((e) =>
          `<div class="log-row"><span class="log-time">${fmtDayTime(e.time)}</span><span class="log-src">[${esc(e.source)}]</span><span class="log-msg">${esc(e.msg)}</span></div>`).join("") ||
          `<div class="faint-12">Nothing yet.</div>`;
    } catch (_) { /* non-critical */ }
  }

  // ------------------------------------------------------------------
  // Tabs

  let activeTab = "control";

  function setTab(tab, updateHash) {
    if (!TABS.includes(tab)) tab = "control";
    activeTab = tab;
    document.querySelectorAll(".tab").forEach((btn) => btn.setAttribute("aria-selected", String(btn.dataset.tab === tab)));
    document.querySelectorAll(".tab-panel").forEach((panel) => { panel.hidden = panel.dataset.panel !== tab; });
    if (updateHash !== false) history.replaceState(null, "", "#" + tab);
    if (tab === "stats") { loadStats(); loadLog(); }
    if (tab === "automation") { loadTimers(); renderTimeline(); }
  }
  document.querySelectorAll(".tab").forEach((btn) => btn.addEventListener("click", () => setTab(btn.dataset.tab)));
  window.addEventListener("hashchange", () => setTab(location.hash.slice(1)));

  // ------------------------------------------------------------------
  // Sync gate: block the UI until we've confirmed the device's live state.
  // Because the shell is cached (PWA), opening the app can paint stale
  // controls before any request completes — the gate makes sure nothing is
  // interactive until the first /api/status succeeds, and shows an offline
  // screen (with Retry) if the device can't be reached.

  let synced = false;

  function showGate(state) {
    el.syncGate.classList.add("visible");
    const offline = state === "offline";
    el.syncSpinner.hidden = offline;
    el.syncIcon.hidden = !offline;
    el.syncRetry.hidden = !offline;
    el.syncTitle.textContent = offline ? "Can’t reach the AC" : "Syncing with your AC…";
    el.syncMsg.textContent = offline
        ? "You appear to be offline or on a different Wi-Fi network. Check the connection and try again."
        : "Fetching the current state.";
  }

  function hideGate() { el.syncGate.classList.remove("visible"); }

  // Cold-start (and Retry): show the syncing gate, block until a first status
  // read succeeds, then load the rest of the app data.
  async function startup() {
    showGate("syncing");
    let status;
    try {
      status = await apiGet("/api/status");
    } catch (_) {
      setConnected(false);
      showGate("offline");
      return;
    }
    setConnected(true);
    synced = true;
    await loadSettings();     // watts/tariff feed every cost estimate
    renderStatus(status);     // re-render with real settings loaded
    hideGate();
    loadPresets();
    loadTimers();
    loadSchedules();
    loadPrograms();
    loadStats();
    loadLog();
  }

  // Returning to the app (e.g. reopening the PWA): re-verify the connection
  // silently. Only surface the gate if the device is unreachable.
  async function resyncOnResume() {
    if (!synced) { startup(); return; }
    try {
      renderStatus(await apiGet("/api/status"));
      setConnected(true);
      hideGate();
    } catch (_) {
      setConnected(false);
      showGate("offline");
    }
  }

  el.syncRetry.addEventListener("click", startup);

  // ------------------------------------------------------------------
  // Boot + polling

  const POLL_STATUS_MS = 5000;
  const POLL_STATS_MS = 60000;

  async function boot() {
    setTab(location.hash.slice(1) || "control", false);
    paintTimerSegment();
    paintTimerTemp();
    await startup();
  }

  setInterval(() => { if (!document.hidden && synced) refreshStatus(); }, POLL_STATUS_MS);
  setInterval(() => {
    if (document.hidden || !synced) return;
    if (activeTab === "stats") { loadStats(); loadLog(); }
    if (activeTab === "automation") loadTimers();
  }, POLL_STATS_MS);
  document.addEventListener("visibilitychange", () => { if (!document.hidden) resyncOnResume(); });

  boot();
})();
