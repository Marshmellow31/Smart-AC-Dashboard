import { initializeApp } from "https://www.gstatic.com/firebasejs/10.9.0/firebase-app.js";
import { getAuth, signInWithPopup, GoogleAuthProvider, onAuthStateChanged, signOut } from "https://www.gstatic.com/firebasejs/10.9.0/firebase-auth.js";
import { getDatabase, ref, onValue, update, push } from "https://www.gstatic.com/firebasejs/10.9.0/firebase-database.js";

const firebaseConfig = {
  apiKey: "AIzaSyCI4QK9dSxx1UD6399cQ-lRwDTZGMmzP9M",
  authDomain: "ac-controller-430f7.firebaseapp.com",
  databaseURL: "https://ac-controller-430f7-default-rtdb.firebaseio.com",
  projectId: "ac-controller-430f7",
  storageBucket: "ac-controller-430f7.firebasestorage.app",
  messagingSenderId: "512769029119",
  appId: "1:512769029119:web:4fa13e93a9177ddb2cedca",
  measurementId: "G-5RKXCD1JE1"
};

const app = initializeApp(firebaseConfig);
const auth = getAuth(app);
const db = getDatabase(app);

// ---------------------------------------------------------------------------
// Cloud transport. The ESP32 mirrors every REST payload to /state/* and
// listens on /ctl (acState writes + a /ctl/cmd RPC queue), so this UI is a
// full realtime peer of the LAN one: every tab re-renders the moment the
// device (or another client) changes something.

(() => {
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
  const HEARTBEAT_STALE_SEC = 150; // device heartbeats every 60s

  const $ = (id) => document.getElementById(id);
  const el = {
    connDot: $("connDot"), toast: $("toast"),
    powerToggle: $("powerToggle"), powerState: $("powerState"),
    tempDial: $("tempDial"), tempValue: $("tempValue"), tempModeLabel: $("tempModeLabel"),
    tempDown: $("tempDown"), tempUp: $("tempUp"),
    modeGrid: $("modeGrid"),
    fanDial: $("fanDial"), fanDialLabel: $("fanDialLabel"), fanGrid: $("fanGrid"),
    presetButtons: $("presetButtons"),
    autoStatus: $("autoStatus"),
    timerDial: $("timerDial"), timerMin: $("timerMin"),
    timerOffSeg: $("timerOffSeg"), timerOnSeg: $("timerOnSeg"), timerOnOpts: $("timerOnOpts"),
    timerTempDown: $("timerTempDown"), timerTempValue: $("timerTempValue"), timerTempUp: $("timerTempUp"),
    timerAdd: $("timerAdd"), timerList: $("timerList"),
    scheduleAdd: $("scheduleAdd"), scheduleList: $("scheduleList"), scheduleSave: $("scheduleSave"),
    programAdd: $("programAdd"), programList: $("programList"), programSave: $("programSave"),
    statCards: $("statCards"), filterText: $("filterText"), filterFill: $("filterFill"), filterReset: $("filterReset"),
    logBox: $("logBox"),
    setAuto: $("setAuto"), setRestore: $("setRestore"),
    setHold: $("setHold"), setWatts: $("setWatts"), setTariff: $("setTariff"),
    setFilter: $("setFilter"), setSafety: $("setSafety"), settingsSave: $("settingsSave"),
  };

  const loginOverlay = $("loginOverlay");
  const appShell = $("appShell");
  const googleSignInBtn = $("googleSignInBtn");
  const signOutBtn = $("signOutBtn");

  let state = { power: false, mode: "cool", temp: 24, fan: "auto" };
  let status = {};
  let statusCache = {};
  let settingsData = { acWatts: 1350, tariffPerKwh: 7.5 };
  let schedulesData = [];
  let programsData = [];
  let timersData = [];
  let statsData = null;
  let scheduleDirty = false, programDirty = false;
  let toastTimer = null;
  let tempDraft = state.temp;
  let fanDraft = 0;
  let timerMinDraft = 60;
  let timerOn = false;
  let timerTempDraft = 24;

  let rtdbConnected = false;
  let lastHeartbeat = 0;
  let unsubs = [];

  const esc = (s) => String(s == null ? "" : s).replace(/[&<>"']/g,
      (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const fmtEpoch = (e) => e ? new Date(e * 1000).toLocaleString([], { weekday: "short", hour: "2-digit", minute: "2-digit" }) : "";
  const fmtDur = (sec) => sec >= 3600 ? `${Math.floor(sec / 3600)}h ${Math.floor((sec % 3600) / 60)}m` : `${Math.ceil(sec / 60)}m`;
  const money = (v) => "₹" + (v || 0).toFixed(1);
  const nowSec = () => Math.floor(Date.now() / 1000);

  function showToast(message, ok) {
    el.toast.textContent = message;
    el.toast.classList.toggle("ok", !!ok);
    el.toast.classList.add("visible");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => el.toast.classList.remove("visible"), 3200);
  }

  const deviceOnline = () => rtdbConnected && (nowSec() - lastHeartbeat) < HEARTBEAT_STALE_SEC;

  function updateConn() {
    const online = deviceOnline();
    el.connDot.classList.toggle("online", online);
    el.connDot.classList.toggle("offline", !online);
    el.connDot.title = online ? "Device online" : "Device offline";
  }

  function setPending(elements, pending) {
    elements.forEach((e) => e && e.classList.toggle("is-pending", pending));
  }

  const mergedStatus = () => ({ timeValid: false, ...statusCache, ...state });

  // Quick controls write the canonical state node; everything else goes
  // through the /ctl/cmd RPC queue the firmware executes and deletes.
  // Commands sitting in the queue when the device reconnects are dropped by
  // design (stale commands must not re-blast the AC), hence the offline warn.
  function warnIfDeviceOffline() {
    if (!deviceOnline()) showToast("Device offline — the command may not apply", false);
  }

  async function setAcState(partial) {
    warnIfDeviceOffline();
    state = { ...state, ...partial };
    await update(ref(db, "ctl/acState"), partial);
    return mergedStatus();
  }

  async function sendCmd(cmd) {
    warnIfDeviceOffline();
    await push(ref(db, "ctl/cmd"), { ts: nowSec(), ...cmd });
  }

  async function apiPost(path, body) {
    if (path === "/api/power") return setAcState({ power: body.on });
    if (path === "/api/temp") return setAcState({ temp: body.value });
    if (path === "/api/mode") return setAcState({ mode: body.mode, power: true });
    if (path === "/api/fan") return setAcState({ fan: body.speed });
    throw new Error("Unknown control path " + path);
  }

  function applyAccent(mode, power) {
    const accent = power ? (MODE_META[mode] || MODE_META.cool).accent : OFF_ACCENT;
    const root = document.documentElement.style;
    root.setProperty("--accent", accent);
    root.setProperty("--accent-dim", accent + "22");
  }

  // ------------------------------------------------------------------
  // Dial component: 270deg sweep, gap at bottom. Pointer + keyboard.

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

  const tempDialCtl = makeDial(el.tempDial, {
    min: MIN_TEMP, max: MAX_TEMP, step: 1,
    get: () => tempDraft,
    set: (v) => { tempDraft = v; el.tempValue.textContent = v; },
    commit: (v) => {
      tempDraft = v;
      if (v === state.temp) return;
      runOptimistic({ ...state, temp: v }, [el.tempDial, el.tempDown, el.tempUp], "/api/temp", { value: v });
    },
  });

  const fanDialCtl = makeDial(el.fanDial, {
    min: 0, max: 3, step: 1,
    get: () => fanDraft,
    set: (v) => { fanDraft = v; el.fanDialLabel.textContent = FAN_LABELS[v]; },
    commit: (v) => {
      fanDraft = v;
      const speed = FAN_ORDER[v];
      if (speed === state.fan) return;
      runOptimistic({ ...state, fan: speed }, [el.fanDial, ...el.fanGrid.querySelectorAll(".chip")], "/api/fan", { speed });
    },
  });

  const timerDialCtl = makeDial(el.timerDial, {
    min: 5, max: 240, step: 5,
    get: () => timerMinDraft,
    set: (v) => { timerMinDraft = v; el.timerMin.textContent = v; },
    commit: (v) => { timerMinDraft = v; },
  });

  // ------------------------------------------------------------------
  // Main controls

  function render(s) {
    state = { power: s.power, mode: s.mode, temp: s.temp, fan: s.fan };
    tempDraft = s.temp;
    fanDraft = Math.max(0, FAN_ORDER.indexOf(s.fan));

    applyAccent(s.mode, s.power);

    el.powerToggle.setAttribute("aria-checked", String(s.power));
    el.powerState.textContent = s.power ? "On" : "Off";

    el.tempValue.textContent = s.temp;
    el.tempModeLabel.textContent = s.power ? (MODE_META[s.mode] ? MODE_META[s.mode].label : s.mode) : "Standby";
    tempDialCtl.paint(s.temp);
    el.tempDown.disabled = s.temp <= MIN_TEMP;
    el.tempUp.disabled = s.temp >= MAX_TEMP;

    el.modeGrid.querySelectorAll(".mode-btn").forEach((btn) => {
      btn.setAttribute("aria-pressed", String(btn.dataset.mode === s.mode));
    });
    el.fanGrid.querySelectorAll(".chip").forEach((btn) => {
      btn.setAttribute("aria-pressed", String(btn.dataset.fan === s.fan));
    });
    el.fanDialLabel.textContent = FAN_LABELS[fanDraft] || "Auto";
    fanDialCtl.paint(fanDraft);
  }

  async function runOptimistic(optimisticState, pendingEls, path, body) {
    const previous = state;
    render(optimisticState);
    setPending(pendingEls, true);
    try {
      const confirmed = await apiPost(path, body);
      renderStatus(confirmed);
    } catch (err) {
      render(previous);
      showToast(err.message || "Update failed");
    } finally {
      setPending(pendingEls, false);
    }
  }

  el.powerToggle.addEventListener("click", () => {
    const next = { ...state, power: !state.power };
    runOptimistic(next, [el.powerToggle], "/api/power", { on: next.power });
  });
  el.tempDown.addEventListener("click", () => {
    if (state.temp <= MIN_TEMP) return;
    const next = { ...state, temp: state.temp - 1 };
    runOptimistic(next, [el.tempDial, el.tempDown, el.tempUp], "/api/temp", { value: next.temp });
  });
  el.tempUp.addEventListener("click", () => {
    if (state.temp >= MAX_TEMP) return;
    const next = { ...state, temp: state.temp + 1 };
    runOptimistic(next, [el.tempDial, el.tempDown, el.tempUp], "/api/temp", { value: next.temp });
  });
  el.modeGrid.addEventListener("click", (evt) => {
    const btn = evt.target.closest(".mode-btn");
    if (!btn || btn.dataset.mode === state.mode) return;
    runOptimistic({ ...state, mode: btn.dataset.mode },
        [...el.modeGrid.querySelectorAll(".mode-btn")], "/api/mode", { mode: btn.dataset.mode });
  });
  el.fanGrid.addEventListener("click", (evt) => {
    const btn = evt.target.closest(".chip");
    if (!btn || btn.dataset.fan === state.fan) return;
    runOptimistic({ ...state, fan: btn.dataset.fan },
        [el.fanDial, ...el.fanGrid.querySelectorAll(".chip")], "/api/fan", { speed: btn.dataset.fan });
  });

  // ------------------------------------------------------------------
  // Automation status strip

  function renderStatus(st) {
    const prevProgId = status.program && status.program.active ? status.program.id : null;
    status = st;
    render(st);

    const rows = [];
    let noteworthy = false;
    if (!st.timeValid) {
      rows.push(`<div class="warn">Device time not synced yet — automations on hold</div>`);
      noteworthy = true;
    } else {
      rows.push(`<div>Device time: <b>${esc(st.time)}</b></div>`);
    }
    if (st.override && st.override.active) {
      rows.push(`<div>Manual hold active until <b>${fmtEpoch(st.override.until)}</b> (schedules paused) <button class="btn-ghost" id="clearHold">Resume automations</button></div>`);
      noteworthy = true;
    }
    if (st.program && st.program.active) {
      rows.push(`<div>Program running: <b>${esc(st.program.name || st.program.id)}</b>, step ${st.program.step}${st.program.endsAt ? " · ends " + fmtEpoch(st.program.endsAt) : ""}</div>`);
      noteworthy = true;
    }
    if (st.nextSchedule) {
      rows.push(`<div>Next schedule: <b>${esc(st.nextSchedule.name)}</b> at ${fmtEpoch(st.nextSchedule.at)}</div>`);
    }
    if (!st.automationEnabled) {
      rows.push(`<div class="warn">Weekly schedules are disabled in Settings.</div>`);
    }
    el.autoStatus.innerHTML = rows.join("");
    el.autoStatus.classList.toggle("visible", noteworthy);

    const clearBtn = $("clearHold");
    if (clearBtn) clearBtn.onclick = async () => {
      try {
        await sendCmd({ type: "clearOverride" });
        showToast("Automations resumed", true);
      } catch (e) { showToast(e.message); }
    };

    const curProgId = st.program && st.program.active ? st.program.id : null;
    if (curProgId !== prevProgId) refreshProgramRunStates();
  }

  // ------------------------------------------------------------------
  // Presets

  function renderPresets(presets) {
    el.presetButtons.innerHTML = presets.map((p) =>
        `<button type="button" class="preset-btn" data-preset="${esc(p.name)}"><span>${esc(p.name)}</span>` +
        `<span>${p.action.power === false ? "turn off" : (p.action.temp || "") + "° " + (p.action.mode || "")}</span></button>`).join("");
  }

  el.presetButtons.addEventListener("click", async (evt) => {
    const btn = evt.target.closest("[data-preset]");
    if (!btn) return;
    try {
      await sendCmd({ type: "applyPreset", name: btn.dataset.preset });
      showToast(`Preset "${btn.dataset.preset}" applied`, true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Countdown timers

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

  function renderTimers() {
    // remainingSec is derived here — the mirror only carries fireAt so it
    // doesn't have to be rewritten every second.
    el.timerList.innerHTML = timersData.map((t) => {
      const remaining = Math.max(0, (t.fireAt || 0) - nowSec());
      const what = t.action.power === false ? "Turn OFF" : `Turn ON ${t.action.temp || ""}°`;
      return `<div class="timer-row"><span>${what} in <b>${fmtDur(remaining)}</b> · ${fmtEpoch(t.fireAt)}</span>` +
             `<button type="button" class="btn-text-danger" data-cancel="${t.id}">Cancel</button></div>`;
    }).join("") || `<div class="faint-12">No timers set.</div>`;
  }

  el.timerList.addEventListener("click", async (evt) => {
    const btn = evt.target.closest("[data-cancel]");
    if (!btn) return;
    try {
      await sendCmd({ type: "cancelTimer", id: Number(btn.dataset.cancel) });
      showToast("Timer cancelled", true);
    } catch (e) { showToast(e.message); }
  });

  el.timerAdd.addEventListener("click", async () => {
    const minutes = timerMinDraft;
    const action = timerOn ? { power: true, temp: timerTempDraft, mode: "cool" } : { power: false };
    try {
      await sendCmd({ type: "addTimer", minutes, action });
      showToast("Timer set", true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Weekly schedules editor

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
        `<label class="day-chip"><input type="checkbox" data-day="${di}" ${days.includes(di) ? "checked" : ""}><span>${d}</span></label>`).join("");
  }

  function markScheduleDirty() { scheduleDirty = true; el.scheduleSave.style.display = ""; }
  function markProgramDirty() { programDirty = true; el.programSave.style.display = ""; }

  function renderSchedules() {
    el.scheduleList.innerHTML = schedulesData.map((s, i) => {
      const off = s.action.power === false;
      const days = s.days || [];
      return `<div class="row-card" data-idx="${i}">
        <div class="row-summary">
          <button type="button" class="row-main" data-toggle>
            <span class="row-name">${esc(s.name)}</span>
            <span class="row-desc">${esc(daysDesc(days))} · ${esc(actionDesc(s.action))}</span>
          </button>
          <span class="row-time">${esc(s.time)}</span>
          <button type="button" class="switch small s-enabled-switch" role="switch" aria-checked="${s.enabled ? "true" : "false"}"><span class="switch-knob"></span></button>
        </div>
        <div class="row-editor" hidden>
          <div class="field-row">
            <input type="text" class="s-name in-txt" value="${esc(s.name)}" placeholder="name" maxlength="23">
            <input type="time" class="s-time in-sel" value="${esc(s.time)}">
            <button type="button" class="btn-text-danger s-del">Delete</button>
          </div>
          <div class="day-chips">${dayChips(days)}</div>
          <div class="field-row">
            <select class="s-power in-sel"><option value="on" ${off ? "" : "selected"}>Turn ON</option><option value="off" ${off ? "selected" : ""}>Turn OFF</option></select>
            <span class="s-onopts field-row" ${off ? 'style="display:none"' : ""}>
              <input type="number" class="s-temp in-num" min="16" max="30" value="${s.action.temp || 24}">°
              <select class="s-mode in-sel">${modeOptions(s.action.mode || "cool")}</select>
              <select class="s-fan in-sel">${fanOptions(s.action.fan || "auto")}</select>
            </span>
          </div>
        </div>
      </div>`;
    }).join("") || `<div class="faint-12">No schedules yet. Add a slot, e.g. ON at 23:00 every day at 24°.</div>`;
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
      schedulesData = readSchedules();
      schedulesData.splice(Number(evt.target.closest(".row-card").dataset.idx), 1);
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
    schedulesData = readSchedules();
    schedulesData.push({ name: "Schedule", enabled: true, time: "22:00",
        days: [0, 1, 2, 3, 4, 5, 6], action: { power: true, temp: 24, mode: "cool", fan: "auto" } });
    renderSchedules();
    markScheduleDirty();
    const editor = el.scheduleList.querySelector(`.row-card[data-idx="${schedulesData.length - 1}"] .row-editor`);
    if (editor) editor.hidden = false;
  });

  el.scheduleSave.addEventListener("click", async () => {
    const slots = readSchedules();
    const bad = slots.find((s) => s.days.length === 0);
    if (bad) { showToast(`Slot "${bad.name}" needs at least one day`); return; }
    try {
      await sendCmd({ type: "schedules", slots });
      schedulesData = slots;
      renderSchedules();
      scheduleDirty = false;
      el.scheduleSave.style.display = "none";
      showToast("Schedules saved", true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Programs editor + start/stop

  function stepsSummary(p) {
    if (!p.steps.length) return "No steps";
    return `${p.steps.length} step${p.steps.length > 1 ? "s" : ""}${p.repeat ? " · repeat" : ""}${p.endTime ? " · until " + p.endTime : ""}`;
  }

  function programActionHtml(p) {
    const running = status.program && status.program.active && status.program.id === p.id;
    if (running) return `<button type="button" class="btn-danger-outline" data-stop="${esc(p.id)}">Stop</button>`;
    return `<input type="time" class="p-until in-sel" aria-label="Run until" value="${esc(p.endTime || "")}">` +
           `<button type="button" class="btn-primary" data-start="${esc(p.id)}">Start</button>`;
  }

  function refreshProgramRunStates() {
    el.programList.querySelectorAll(".row-action").forEach((span) => {
      const p = programsData.find((x) => x.id === span.dataset.pid);
      if (p) span.innerHTML = programActionHtml(p);
    });
  }

  function renderPrograms() {
    el.programList.innerHTML = programsData.map((p, i) => {
      const steps = (p.steps || []).map((s, si) => `
        <div class="field-row step-row" data-step="${si}">
          <select class="p-on in-sel"><option value="on" ${s.on ? "selected" : ""}>ON</option><option value="off" ${s.on ? "" : "selected"}>OFF</option></select>
          <input type="number" class="p-min in-num" min="1" max="1440" value="${s.minutes}" title="minutes"> min
          <span class="p-onopts field-row" ${s.on ? "" : 'style="display:none"'}>
            <input type="number" class="p-temp in-num" min="16" max="30" value="${s.temp || 24}">°
            <select class="p-mode in-sel">${modeOptions(s.mode || "cool")}</select>
            <select class="p-fan in-sel">${fanOptions(s.fan || "auto")}</select>
          </span>
          <button type="button" class="btn-text-danger p-stepdel">✕</button>
        </div>`).join("");
      return `<div class="row-card" data-idx="${i}">
        <div class="row-summary">
          <button type="button" class="row-main" data-toggle>
            <span class="row-name">${esc(p.name)}</span>
            <span class="row-desc">${esc(stepsSummary({ ...p, steps: p.steps || [] }))}</span>
          </button>
          <span class="row-action" data-pid="${esc(p.id)}">${programActionHtml(p)}</span>
        </div>
        <div class="row-editor" hidden>
          <div class="field-row">
            <input type="text" class="p-name in-txt" value="${esc(p.name)}" placeholder="name" maxlength="39">
            <input type="hidden" class="p-id" value="${esc(p.id)}">
            <label class="dim-13" style="display:flex;align-items:center;gap:6px"><input type="checkbox" class="p-repeat" ${p.repeat ? "checked" : ""}> repeat</label>
            <span class="dim-13">default end</span>
            <input type="time" class="p-end in-sel" value="${esc(p.endTime || "")}">
            <button type="button" class="btn-text-danger p-del">Delete</button>
          </div>
          ${steps}
          <div class="field-row"><button type="button" class="btn-ghost p-stepadd">+ step</button></div>
        </div>
      </div>`;
    }).join("") || `<div class="faint-12">No programs yet.</div>`;
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

  el.programList.addEventListener("click", async (evt) => {
    const startBtn = evt.target.closest("[data-start]");
    if (startBtn) {
      const until = startBtn.closest(".row-summary").querySelector(".p-until").value || "";
      try {
        await sendCmd({ type: "startProgram", id: startBtn.dataset.start, endTime: until });
        showToast("Program started", true);
      } catch (e) { showToast(e.message); }
      return;
    }
    const stopBtn = evt.target.closest("[data-stop]");
    if (stopBtn) {
      try {
        await sendCmd({ type: "stopProgram" });
        showToast("Program stopped", true);
      } catch (e) { showToast(e.message); }
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
    if (evt.target.closest(".p-del")) {
      programsData = readPrograms();
      programsData.splice(idx, 1);
      renderPrograms();
      markProgramDirty();
    } else if (evt.target.closest(".p-stepadd")) {
      programsData = readPrograms();
      programsData[idx].steps.push({ on: true, minutes: 30, temp: 24, mode: "cool", fan: "auto" });
      renderPrograms();
      markProgramDirty();
    } else if (evt.target.closest(".p-stepdel")) {
      programsData = readPrograms();
      const stepRow = evt.target.closest(".step-row");
      programsData[idx].steps.splice(Number(stepRow.dataset.step), 1);
      renderPrograms();
      markProgramDirty();
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
    programsData = readPrograms();
    programsData.push({ id: "", name: "New program", repeat: false, endTime: "",
        steps: [{ on: true, minutes: 60, temp: 24, mode: "cool", fan: "auto" }] });
    renderPrograms();
    markProgramDirty();
    const editor = el.programList.querySelector(`.row-card[data-idx="${programsData.length - 1}"] .row-editor`);
    if (editor) editor.hidden = false;
  });

  el.programSave.addEventListener("click", async () => {
    const programs = readPrograms();
    const bad = programs.find((p) => p.steps.length === 0);
    if (bad) { showToast(`Program "${bad.name}" needs at least one step`); return; }
    try {
      await sendCmd({ type: "programs", programs });
      programsData = programs;
      renderPrograms();
      programDirty = false;
      el.programSave.style.display = "none";
      showToast("Programs saved", true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Stats

  function renderStats() {
    const s = statsData;
    if (!s || !s.today) return;
    const estPerDay = (settingsData.acWatts || 0) / 1000 * 24 * (settingsData.tariffPerKwh || 0);
    const tiles = [
      { label: "Today on-time", value: fmtDur(s.today.onMinutes * 60), sub: `${s.today.kwh.toFixed(2)} kWh · ${money(s.today.cost)}` },
      { label: "This month", value: fmtDur(s.month.onMinutes * 60), sub: `${s.month.kwh.toFixed(1)} kWh · ${money(s.month.cost)}` },
      { label: "Running now", value: s.continuousOnMinutes > 0 ? fmtDur(s.continuousOnMinutes * 60) : "–", sub: s.continuousOnMinutes > 0 ? "continuous" : "AC is off" },
      { label: "Est. cost/day", value: money(estPerDay), sub: `at ₹${(settingsData.tariffPerKwh || 0).toFixed(1)}/kWh` },
    ];
    el.statCards.innerHTML = tiles.map((t) =>
        `<div class="stat-tile"><div class="stat-label">${t.label}</div><div class="stat-value">${t.value}</div><div class="stat-sub">${t.sub}</div></div>`).join("");

    el.filterText.textContent = `${s.filter.hours}h / ${s.filter.limitHours}h`;
    const pct = Math.max(0, Math.min(100, (s.filter.hours / Math.max(1, s.filter.limitHours)) * 100));
    el.filterFill.style.width = pct + "%";
    el.filterFill.classList.toggle("needs-cleaning", !!s.filter.needsCleaning);
  }

  el.filterReset.addEventListener("click", async () => {
    try {
      await sendCmd({ type: "filterReset" });
      showToast("Filter counter reset", true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Settings

  el.setAuto.addEventListener("click", () => {
    el.setAuto.setAttribute("aria-checked", String(el.setAuto.getAttribute("aria-checked") !== "true"));
  });
  el.setRestore.addEventListener("click", () => {
    el.setRestore.setAttribute("aria-checked", String(el.setRestore.getAttribute("aria-checked") !== "true"));
  });

  function renderSettings() {
    const s = settingsData;
    el.setAuto.setAttribute("aria-checked", String(!!s.automationEnabled));
    el.setRestore.setAttribute("aria-checked", String(!!s.restoreOnBoot));
    el.setHold.value = s.holdMinutes;
    el.setWatts.value = s.acWatts;
    el.setTariff.value = s.tariffPerKwh;
    el.setFilter.value = s.filterLimitHours;
    el.setSafety.value = s.maxContinuousHours;
  }

  el.settingsSave.addEventListener("click", async () => {
    try {
      await sendCmd({ type: "settings", settings: {
        holdMinutes: Number(el.setHold.value),
        automationEnabled: el.setAuto.getAttribute("aria-checked") === "true",
        restoreOnBoot: el.setRestore.getAttribute("aria-checked") === "true",
        acWatts: Number(el.setWatts.value),
        tariffPerKwh: Number(el.setTariff.value),
        filterLimitHours: Number(el.setFilter.value),
        maxContinuousHours: Number(el.setSafety.value),
      } });
      showToast("Settings saved", true);
    } catch (e) { showToast(e.message); }
  });

  // ------------------------------------------------------------------
  // Event log

  function renderLog(events) {
    el.logBox.innerHTML = events.map((e) =>
        `<div class="log-row"><span class="log-time">${fmtEpoch(e.time)}</span><span class="log-src">[${esc(e.source)}]</span><span class="log-msg">${esc(e.msg)}</span></div>`).join("") ||
        `<div class="faint-12">Nothing yet.</div>`;
  }

  // ------------------------------------------------------------------
  // Tabs

  function setTab(tab, updateHash) {
    if (!TABS.includes(tab)) tab = "control";
    document.querySelectorAll(".tab").forEach((btn) => btn.setAttribute("aria-selected", String(btn.dataset.tab === tab)));
    document.querySelectorAll(".tab-panel").forEach((panel) => { panel.hidden = panel.dataset.panel !== tab; });
    if (updateHash !== false) history.replaceState(null, "", "#" + tab);
  }
  document.querySelectorAll(".tab").forEach((btn) => btn.addEventListener("click", () => setTab(btn.dataset.tab)));
  window.addEventListener("hashchange", () => setTab(location.hash.slice(1)));
  setTab(location.hash.slice(1) || "control", false);

  // ------------------------------------------------------------------
  // Realtime subscriptions — the ESP32 mirrors everything to /state/*, so
  // there is no polling: each node re-renders its tab the moment it changes.

  function startSync() {
    const sub = (path, fn) => unsubs.push(onValue(ref(db, path), (snap) => fn(snap.val())));

    sub("ctl/acState", (v) => {
      if (!v) return;
      state = { ...state, ...v };
      renderStatus(mergedStatus());
    });
    sub("state/status", (v) => {
      if (!v) return;
      statusCache = v;
      renderStatus(mergedStatus());
    });
    sub("state/settings", (v) => {
      if (!v) return;
      settingsData = v;
      renderSettings();
      renderStats();
    });
    sub("state/schedules", (v) => {
      // Never clobber unsaved local edits with a remote refresh.
      if (scheduleDirty) return;
      schedulesData = (v && v.slots) || [];
      renderSchedules();
    });
    sub("state/programs", (v) => {
      if (programDirty) return;
      programsData = (v && v.programs) || [];
      renderPrograms();
    });
    sub("state/presets", (v) => renderPresets((v && v.presets) || []));
    sub("state/timers", (v) => {
      timersData = (v && v.timers) || [];
      renderTimers();
    });
    sub("state/stats", (v) => { statsData = v; renderStats(); });
    sub("state/log", (v) => renderLog((v && v.events) || []));
    sub("state/heartbeat", (v) => { lastHeartbeat = v || 0; updateConn(); });
    sub(".info/connected", (v) => { rtdbConnected = !!v; updateConn(); });
  }

  function stopSync() {
    unsubs.forEach((u) => u());
    unsubs = [];
  }

  // Countdown labels and the online dot decay without server events.
  setInterval(() => { renderTimers(); updateConn(); }, 30000);

  // ------------------------------------------------------------------
  // Auth gate

  const provider = new GoogleAuthProvider();
  googleSignInBtn.addEventListener("click", () => {
    signInWithPopup(auth, provider).catch(error => alert("Login failed: " + error.message));
  });
  signOutBtn.addEventListener("click", () => signOut(auth));

  onAuthStateChanged(auth, (user) => {
    if (user) {
      if (user.email !== "1080patelharshil@gmail.com") {
        signOut(auth);
        alert("Unauthorized account. Only 1080patelharshil@gmail.com is allowed.");
        return;
      }
      loginOverlay.style.display = "none";
      appShell.style.display = "flex";
      startSync();
    } else {
      stopSync();
      loginOverlay.style.display = "flex";
      appShell.style.display = "none";
      rtdbConnected = false;
      updateConn();
    }
  });

  paintTimerSegment();
  paintTimerTemp();
})();
