import { sections, fieldsFor, drivesFor, collecting } from "./model.js";

const $ = id => document.getElementById(id);
const slots = new Map();
const locks = new Set();
let latest, revision = 0, timer, reading = false, again = false, toastTimer, autoCollected = false;
const invoke = (command, args) => window.__TAURI__.core.invoke(command, args);

function node(tag, className, text) {
  const el = document.createElement(tag);
  if (className) el.className = className;
  if (text !== undefined) el.textContent = text;
  return el;
}
function drawing(name, className) {
  const image = node("img", className);
  image.src = "icons/" + name + ".svg"; image.alt = ""; image.draggable = false;
  return image;
}

for (const section of sections) {
  const block = node("section", "section " + section.id);
  const head = node("div", "section-head"), heading = node("h2", "", section.title);
  heading.id = "title-" + section.id;
  block.setAttribute("aria-labelledby", heading.id);
  head.append(drawing(section.icon, "section-icon"), heading);
  block.append(head);
  const standalone = section.fields.length === 1;
  const list = node(standalone ? "div" : "dl", standalone ? "standalone" : "fields");
  for (const [key, label, icon] of section.fields) {
    const value = node(standalone ? "div" : "dd", "pending", "正在读取…");
    slots.set(key, { value, signature: null });
    if (standalone) { list.append(value); continue; }
    const row = node("div", "field"), term = node("dt");
    if (section.id === "hardware") {
      if (icon || key === "主板型号") term.append(drawing(icon || "board", "field-icon"));
      else {
        const symbol = node("span", "os-symbol");
        symbol.setAttribute("aria-hidden", "true");
        term.append(symbol);
      }
    }
    term.append(document.createTextNode(label));
    row.append(term, value); list.append(row);
  }
  block.append(list); $(section.column + "-column").append(block);
}

function notify(text) {
  clearTimeout(toastTimer);
  $("toast").textContent = text; $("toast").hidden = false;
  toastTimer = setTimeout(() => { $("toast").hidden = true; }, 3200);
}
function definitionList(className, fields) {
  const list = node("dl", className);
  for (const [label, value] of fields) {
    const row = node("div");
    row.append(node("dt", "", label), node("dd", "", value));
    list.append(row);
  }
  return list;
}

function renderDrives(el, value) {
  const drives = drivesFor(value).map(({ model, capacity, protocol, metrics, notes }) => {
    const drive = node("div", "drive"), head = node("div", "drive-head");
    head.append(node("h3", "", model));
    if (capacity) head.append(node("span", "capacity", capacity));
    drive.append(head);
    if (protocol) drive.append(node("p", "protocol", protocol));
    if (metrics.length) drive.append(definitionList("drive-metrics", metrics));
    for (const line of notes) drive.append(node("p", "protocol", line));
    return drive;
  });
  el.replaceChildren(...drives);
}

function renderPower(el, battery) {
  const summary = node("div", "power-summary");
  const text = node("div", "power-status");
  const heading = battery.present === true ? battery.state : battery.source;
  const detail = battery.present === true ? battery.source : battery.state;
  text.append(node("strong", "", heading), node("span", "", detail));
  summary.append(text);
  if (battery.present === true && Number.isInteger(battery.percent)) {
    const amount = node("span", "charge", String(battery.percent));
    amount.append(node("small", "", "%"));
    const meter = node("meter", "charge-meter");
    meter.min = 0; meter.max = 100; meter.value = battery.percent;
    meter.setAttribute("aria-label", "电池电量");
    summary.append(amount, meter);
  }
  if (battery.remaining) summary.append(node("p", "runtime", "预计续航 " + battery.remaining));
  el.replaceChildren(summary);
  for (const pack of battery.packs) {
    const group = node("div", "pack");
    group.append(node("h3", "", pack.name), definitionList("pack-fields", pack.fields.map(f => [f.label, f.value])));
    el.append(group);
  }
}

function renderField(field, battery) {
  const slot = slots.get(field.key);
  const signature = JSON.stringify([field.value, field.pending, field.key === "电池信息" ? battery : null]);
  if (slot.signature === signature) return;
  slot.signature = signature;
  const el = slot.value;
  el.classList.toggle("pending", field.pending);
  el.classList.toggle("unavailable", field.unavailable);
  if (field.key === "电池信息" && battery) renderPower(el, battery);
  else if (field.key === "SSD信息" && field.value.includes("\n")) renderDrives(el, field.value);
  else el.textContent = field.unavailable ? "未提供" : field.value;
}
function renderLog(log) {
  $("log-panel").hidden = !log;
  if (!log) return;
  const busy = collecting(log);
  const titles = {
    running: log.total ? "正在收集日志  " + log.completed + " / " + log.total : "正在准备收集日志…",
    cancelling: "正在取消收集…", complete: "日志收集完成", partial: "日志已部分收集",
    failed: "日志收集失败", cancelled: "已取消收集"
  };
  $("log-panel").dataset.status = log.state;
  $("log-title").textContent = titles[log.state] || "日志收集";
  const progress = $("log-progress");
  progress.hidden = !busy;
  if (log.total) { progress.max = log.total; progress.value = log.completed; }
  else progress.removeAttribute("value");
  const summary = busy ? log.item : ["已完整保存 " + log.saved + " 项",
    log.failed + log.partial ? (log.failed + log.partial) + " 项未完整取得" : "", log.detail].filter(Boolean).join(" · ");
  $("log-detail").textContent = summary;
  $("log-detail").hidden = !summary;
  $("log-path").textContent = log.directory;
  $("log-path").hidden = busy || !log.directory;
  $("cancel").hidden = !busy;
  $("open-report").hidden = busy || !log.hasReport;
  $("open-folder").hidden = busy || !log.directory;
}
function buttons() {
  const busy = !latest || latest.computerPending || latest.ssdPending;
  $("refresh").disabled = busy || locks.has("refresh");
  $("copy").disabled = !latest || latest.computerPending || !latest.rows.length || locks.has("copy_all");
  $("collect").disabled = !latest || collecting(latest.log) || locks.has("collect_logs");
  $("collect").lastElementChild.textContent = collecting(latest?.log) ? "收集中" : "收集日志";
  $("cancel").disabled = latest?.log?.state !== "running" || locks.has("cancel_logs");
}
function render(state) {
  latest = state; revision = state.revision;
  const fields = fieldsFor(state);
  fields.forEach(field => renderField(field, state.battery));
  const count = fields.filter(field => !field.pending && !field.unavailable).length;
  const busy = state.computerPending || state.ssdPending;
  $("state").dataset.state = busy ? "busy" : (state.diagnostic || state.error ? "warning" : "ready");
  $("status-text").textContent = state.computerPending ? "正在读取硬件信息" :
    (state.ssdPending ? "正在读取 SSD SMART · " : "") + "已读取 " + count + " / " + fields.length + " 项";
  $("time").textContent = state.updated;
  $("updated").hidden = !state.updated;
  $("machine-name").textContent = state.machine || "本机";
  const detail = [state.error, state.diagnostic].filter(Boolean).join("\n");
  $("diagnostic-text").textContent = detail;
  $("diagnostics").hidden = !detail;
  renderLog(state.log); buttons();
}
async function poll() {
  clearTimeout(timer);
  if (reading) { again = true; return; }
  reading = true;
  let failed = false;
  try {
    const next = await invoke("snapshot", { since: revision });
    if (next) {
      render(next);
      if (next.collectOnReady && !autoCollected) { autoCollected = true; $("collect").click(); }
      if (!next.computerPending && !next.ssdPending && !collecting(next.log)) {
        requestAnimationFrame(() => requestAnimationFrame(() => { invoke("frame_ready").catch(() => {}); }));
      }
    }
  } catch (error) {
    failed = true;
    $("status-text").textContent = "暂时无法读取电脑信息";
    $("state").dataset.state = "warning";
    $("diagnostic-text").textContent = String(error);
    $("diagnostics").hidden = false;
  } finally {
    reading = false;
    if (again) { again = false; timer = setTimeout(poll, 0); }
    else if (failed || !latest || latest.computerPending || latest.ssdPending || collecting(latest.log)) {
      timer = setTimeout(poll, failed ? 2000 : 170);
    }
  }
}
async function act(action) {
  if (locks.has(action)) return;
  locks.add(action); buttons();
  try {
    const ok = await invoke("perform", { action });
    if (!ok) notify(action === "copy_all" ? "剪贴板暂时不可用" : "当前操作不可用");
    else if (action === "copy_all") notify("已复制当前电脑信息");
  } catch (error) {
    notify(String(error));
  } finally {
    locks.delete(action); await poll(); buttons();
  }
}
for (const [id, action] of Object.entries({refresh:"refresh",copy:"copy_all",collect:"collect_logs",
  cancel:"cancel_logs","open-report":"open_report","open-folder":"open_folder"})) {
  $(id).addEventListener("click", () => act(action));
}
document.addEventListener("keydown", event => {
  if (event.key === "F5") { event.preventDefault(); if (!$("refresh").disabled) act("refresh"); }
});

const appWindow = window.__TAURI__.window.getCurrentWindow();
let resizeFrame;
async function syncMaximize() {
  const maximized = await appWindow.isMaximized();
  const label = maximized ? "还原" : "最大化";
  $("win-max").title = label;
  $("win-max").setAttribute("aria-label", label);
  $("win-max-glyph").setAttribute("href", maximized ? "#i-restore" : "#i-maximize");
}
function windowAction(id, action) {
  $(id).addEventListener("click", () => action().catch(error => notify(String(error))));
}
windowAction("win-min", () => appWindow.minimize());
windowAction("win-max", async () => { await appWindow.toggleMaximize(); await syncMaximize(); });
windowAction("win-close", () => appWindow.close());
window.addEventListener("resize", () => {
  cancelAnimationFrame(resizeFrame);
  resizeFrame = requestAnimationFrame(() => syncMaximize().catch(() => {}));
});
syncMaximize().catch(() => {});
poll();
