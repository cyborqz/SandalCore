// app.js: the dashboard's calls and tables. The Go side is the window's
// bindings: window.go.main.App.<Method>().
const state = { players: [], selected: "", stats: [], places: [], tab: "players" };

// The bound methods: the Go package (main) and the App struct.
function backend() {
  const go = window.go || {};
  if (go.main && go.main.App) return go.main.App;
  if (go.App) return go.App;
  return go;
}

// A number the server sent. The protobuf fields carry no zero (the JSON
// leaves them out), so a missing one is a zero.
function num(value) {
  return typeof value === "number" && isFinite(value) ? value : 0;
}

// The text of a name, safe to put in the table.
function escape(text) {
  return String(text === undefined || text === null ? "" : text)
    .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function toast(message, isError) {
  const el = document.getElementById("toast");
  el.textContent = message;
  el.className = "toast" + (isError ? " error" : "");
  el.classList.add("show");
  clearTimeout(el._timer);
  el._timer = setTimeout(() => el.classList.remove("show"), 4000);
}

// call: one call to the Go side. A refusal comes back as a failure and the
// server's own reason is shown.
async function call(name, ...args) {
  try {
    const api = backend();
    const fn = api[name];
    if (typeof fn !== "function") throw new Error("ما فيه call " + name);
    return await fn.apply(api, args);
  } catch (err) {
    toast(err && err.message ? err.message : String(err), true);
    return null;
  }
}

// ── the tabs ────────────────────────────────────────────────────────────────────
document.querySelectorAll(".tab").forEach((button) => {
  button.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((b) => b.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((p) => p.classList.remove("active"));
    button.classList.add("active");
    document.getElementById(button.dataset.tab).classList.add("active");
    state.tab = button.dataset.tab;
    refresh();
  });
});

// ── the players tab ─────────────────────────────────────────────────────────────
async function loadPlayers() {
  const players = await call("Players");
  if (!players) return;
  state.players = players;
  const filter = document.getElementById("player-filter").value.toLowerCase();
  const body = document.querySelector("#players-table tbody");
  body.innerHTML = "";
  for (const p of players) {
    const name = p.name || "";
    if (filter && !name.toLowerCase().includes(filter)) continue;
    const row = document.createElement("tr");
    if (name === state.selected) row.classList.add("selected");
    const pos = p.position || {};
    row.innerHTML = `
      <td>${escape(name)}</td>
      <td class="mono">${(p.character_id || "").slice(0, 8)}</td>
      <td class="mono">${Math.round(num(pos.x))}, ${Math.round(num(pos.y))}, ${Math.round(num(pos.z))}</td>
      <td>${p.mounted ? "نعم" : "لا"}${p.dead ? " (ميت)" : ""}</td>
      <td>×${num(p.move_speed_multiplier) || 1}</td>
      <td class="actions">
        <button data-act="select">إحصائيات</button>
        <button data-act="teleport">انقل</button>
        <button data-act="speed">السرعة</button>
        <button data-act="money">فلوس</button>
        <button data-act="item">item</button>
      </td>`;
    row.querySelector('[data-act="select"]').addEventListener("click", () => {
      state.selected = name;
      document.querySelector('.tab[data-tab="stats"]').click();
    });
    row.querySelector('[data-act="teleport"]').addEventListener("click", () => teleport(p));
    row.querySelector('[data-act="speed"]').addEventListener("click", () => setSpeed(p));
    row.querySelector('[data-act="money"]').addEventListener("click", () => giveMoney(p));
    row.querySelector('[data-act="item"]').addEventListener("click", () => giveItem(p));
    body.appendChild(row);
  }
}

async function teleport(p) {
  if (!state.places.length) state.places = (await call("Places")) || [];
  const names = state.places.map((x) => x.display_name || x.name);
  const where = prompt("انقل إلى:\n" + names.join("\n"));
  if (!where) return;
  const place = state.places.find((x) => (x.display_name || x.name) === where);
  if (!place) { toast("ما فيه مكان بهذا الاسم", true); return; }
  const out = await call("Teleport", p.name, place.name, 0, 0, 0, false);
  if (out) toast("انتقل " + p.name + " إلى " + (place.display_name || place.name));
}

async function setSpeed(p) {
  const value = prompt("السرعة (0.5 - 10، و1 يرجّعها):", "3");
  if (!value) return;
  const out = await call("SetSpeed", p.name, Number(value));
  if (out !== null) toast("سرعة " + p.name + " صارت ×" + out);
}

async function giveMoney(p) {
  const value = prompt("كم نحط من copper؟", "1000");
  if (!value) return;
  const out = await call("GiveMoney", p.name, Number(value));
  if (out !== null) toast("الرصيد صار " + out);
}

async function giveItem(p) {
  const name = prompt("اسم الـ item (أو الـ Guid):");
  if (!name) return;
  const out = await call("GiveItem", p.name, name, 1);
  if (out) toast("أُعطي " + p.name + " " + out);
}

// ── the stats tab ───────────────────────────────────────────────────────────────
// The bundle's own flags (server/worldd/stats/stat_program.h): a stat whose
// kStatMulMods bit is set multiplies by its mods instead of adding them, and a
// kStatMulCompl stat multiplies by one minus the value.
const kStatMulMods = 1 << 0;
const kStatMulCompl = 1 << 6;
const kStatResource = 1 << 7;

// The unit of an override: the stat's own rule decides what the number means.
function statUnit(stat) {
  const flags = num(stat.flags);
  if ((flags & kStatMulMods) === 0) return "+";
  if ((flags & kStatMulCompl) !== 0) return "×(1-v)";
  return "×";
}

function statKind(stat) {
  const name = stat.name || "";
  if (/resist/i.test(name)) return "مقاومة";
  if (/damage/i.test(name)) return "ضرر";
  if (/^max/i.test(name)) return "حد أقصى";
  if ((num(stat.flags) & kStatResource) !== 0) return "مورد";
  return "خاصية";
}

async function loadStats() {
  if (!state.selected) return;
  const stats = await call("Stats", state.selected);
  if (!stats) return;
  state.stats = stats;
  document.getElementById("stats-player").textContent = "إحصائيات: " + state.selected;
  const filter = document.getElementById("stats-filter").value.toLowerCase();
  const body = document.querySelector("#stats-table tbody");
  body.innerHTML = "";
  for (const stat of stats) {
    const name = stat.name || "";
    if (filter && !name.toLowerCase().includes(filter)) continue;
    const unit = statUnit(stat);
    const hasOverride = stat.has_override === true;
    const row = document.createElement("tr");
    row.innerHTML = `
      <td class="mono">${escape(name || stat.guid)}</td>
      <td>${statKind(stat)}</td>
      <td><input class="current-input" data-guid="${stat.guid}" value="${num(stat.value)}"></td>
      <td>${num(stat.max) === 0 ? "بلا حد" : num(stat.max)}</td>
      <td>${hasOverride ? unit + num(stat.override) : "-"}</td>
      <td><input class="stat-input" data-guid="${stat.guid}"
                 value="${hasOverride ? num(stat.override) : ""}" placeholder="${unit}"></td>`;
    // The current value: the server refuses a value above the stat's maximum
    // (it does not cut it down), and the reason is shown.
    const current = row.querySelector(".current-input");
    current.addEventListener("change", async () => {
      const value = await call("SetCurrentStat", state.selected, Number(current.dataset.guid), Number(current.value));
      if (value !== null) {
        current.value = value;
        toast("القيمة صارت " + value);
      } else {
        current.value = num(stat.value);
      }
    });
    body.appendChild(row);
  }
}

document.getElementById("save-stats").addEventListener("click", async () => {
  if (!state.selected) { toast("اختر لاعباً أول", true); return; }
  // The server replaces every override with the ones sent, so the values
  // already on the stats go with them; a cleared field drops that override.
  const mods = [];
  document.querySelectorAll("#stats-table tbody input.stat-input").forEach((input) => {
    if (input.value.trim() === "") return;
    mods.push({ stat: Number(input.dataset.guid), value: Number(input.value) });
  });
  if (!mods.length) { toast("ما فيه تعديلات للحفظ", true); return; }
  const stats = await call("SetStatMods", state.selected, mods);
  if (stats) { toast("حُفظت التعديلات"); await loadStats(); }
});

document.getElementById("clear-stats").addEventListener("click", async () => {
  if (!state.selected) { toast("اختر لاعباً أول", true); return; }
  const stats = await call("ClearStatMods", state.selected);
  if (stats) { toast("امسحنا كل التعديلات"); await loadStats(); }
});

document.getElementById("fill-health").addEventListener("click", async () => {
  const health = state.stats.find((s) => /health/i.test(s.name || "") && !/^max/i.test(s.name || ""));
  if (!health) { toast("ما لقيت stat الدم", true); return; }
  const value = await call("SetCurrentStat", state.selected, health.guid, num(health.max) || num(health.value));
  if (value !== null) toast("الدم صار " + value);
});

document.getElementById("stats-filter").addEventListener("input", loadStats);
document.getElementById("player-filter").addEventListener("input", loadPlayers);
document.getElementById("refresh-stats").addEventListener("click", loadStats);

// ── the world tab ───────────────────────────────────────────────────────────────
// The world stream's Search: the entities whose name holds the query (at least
// two characters). The details of the one that is picked come from Inspect.
async function loadWorld() {
  const query = document.getElementById("world-filter").value.trim();
  if (query.length < 2) { toast("اكتب حرفين على الأقل", true); return; }
  const rows = await call("SearchEntities", query, 20);
  if (!rows) return;
  const body = document.querySelector("#world-table tbody");
  body.innerHTML = "";
  for (const row of rows) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escape(row.name)}</td>
      <td>${escape(row.kind)}</td>
      <td>${row.level ? row.level : "-"}</td>
      <td>${row.health_pct === 255 ? "-" : row.health_pct + "%"}</td>
      <td class="mono">${Math.round(num(row.x))}, ${Math.round(num(row.y))}</td>
      <td class="actions">
        <button data-act="inspect">تفاصيل</button>
        <button data-act="kill">اقتل</button>
        <button data-act="heal">عالج</button>
        <button data-act="reset">أعد</button>
        <button data-act="despawn">اختفِ</button>
      </td>`;
    tr.querySelector('[data-act="inspect"]').addEventListener("click", () => inspect(row.id));
    for (const action of ["kill", "heal", "reset", "despawn"]) {
      tr.querySelector('[data-act="' + action + '"]').addEventListener("click", async () => {
        const out = await call("NpcAction", action, row.id, 0, 0, 0, false);
        if (out !== null) toast(out);
        await loadWorld();
      });
    }
    body.appendChild(tr);
  }
}

async function inspect(id) {
  const detail = await call("InspectEntity", id);
  if (!detail) return;
  const el = document.getElementById("world-detail");
  el.innerHTML = `
    <div><b>${escape(detail.name)}</b> — ${escape(detail.kind)} · المستوى ${detail.level}</div>
    <div>الحياة: ${Math.round(num(detail.health))} / ${Math.round(num(detail.max_health))} · الحالة: ${escape(detail.ai_state || "-")}</div>
    <div>النوع: ${escape(detail.archetype || detail.text || "-")} · المصدر: ${escape(detail.source || "-")}</div>
    <div>المنزل: ${escape(detail.home || "-")} · بقي له للظهور: ${Math.round(num(detail.respawn_in_seconds))}ث</div>`;
}

document.getElementById("world-filter").addEventListener("input", loadWorld);
document.getElementById("refresh-world").addEventListener("click", loadWorld);

// ── the cities tab ──────────────────────────────────────────────────────────────
async function loadCities() {
  const nodes = await call("Nodes");
  if (!nodes) return;
  const body = document.querySelector("#cities-table tbody");
  body.innerHTML = "";
  for (const node of nodes) {
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escape(node.display_name || node.name)}</td>
      <td>${escape(node.level_name || node.level)}</td>
      <td>${node.xp} (${node.percent}%)</td>
      <td class="mono">${escape(node.clock)}</td>
      <td class="actions">
        <button data-act="xp">أضف خبرة</button>
        <button data-act="clock">زِد ساعة</button>
      </td>`;
    tr.querySelector('[data-act="xp"]').addEventListener("click", async () => {
      const value = prompt("كم خبرة؟", "100000");
      if (!value) return;
      const out = await call("NodeXp", node.name, Number(value));
      if (out !== null) toast(out);
      await loadCities();
    });
    tr.querySelector('[data-act="clock"]').addEventListener("click", async () => {
      const out = await call("NodeClock", 3600);
      if (out !== null) toast("ساعة المدن صارت " + out);
      await loadCities();
    });
    body.appendChild(tr);
  }
}

// ── the log tab ────────────────────────────────────────────────────────────────
async function loadLogs() {
  const names = ["worldd.log", "gateway.log", "auth.log", "character.log", "mesh.log", "citynode.log", "quests.log"];
  const parts = [];
  for (const name of names) {
    const lines = await call("LogTail", name, 60);
    if (lines && lines.length) parts.push("=== " + name + " ===\n" + lines.join("\n"));
  }
  document.getElementById("log-view").textContent = parts.join("\n\n");
}

// ── the loop ────────────────────────────────────────────────────────────────────
async function refresh() {
  const status = await call("Status");
  if (status) {
    const el = document.getElementById("status");
    el.textContent = status.connected
      ? "متصل — " + status.adminAddr + " · " + status.characterAddr + " · " + status.streamAddr +
        " · " + (status.players || 0) + " لاعب"
      : "الخادم مو شغّال" + (status.error ? ": " + status.error : "");
    el.className = "status" + (status.connected ? " ok" : " bad");
    document.getElementById("start-stack").hidden = status.connected;
  }
  // The stats tab is left alone: it holds what the game master is typing.
  if (state.tab === "players") { await loadPlayers(); }
  else if (state.tab === "world") { await loadWorld(); }
  else if (state.tab === "cities") { await loadCities(); }
  else if (state.tab === "logs") { await loadLogs(); }
}

// The stack is down: the window can start it (build\start-stack.ps1).
document.getElementById("start-stack").addEventListener("click", async () => {
  const out = await call("StartStack");
  toast(out || "شغّلنا الستاك");
  setTimeout(refresh, 3000);
});

document.getElementById("refresh-players").addEventListener("click", loadPlayers);
setInterval(refresh, 2000);
refresh();
