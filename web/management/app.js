"use strict";
const $ = (id) => document.getElementById(id);
const state = {
  keys: [],
  listCursors: [null],
  listNext: null,
  listSequence: 0,
  listController: null,
  detail: null,
  detailSequence: 0,
  detailController: null,
  requestCursors: [null],
  requestNext: null,
  offset: 480,
  createBusy: false,
  disableBusy: false,
  disableKey: null,
};
const count = (value) =>
  value === null || value === undefined
    ? "未上报"
    : BigInt(value).toLocaleString("zh-CN");
const time = (value) =>
  new Date(value).toLocaleString("zh-CN", { hour12: false });
const day = (ago = 0) =>
  new Date(Date.now() + state.offset * 60000 - ago * 86400000)
    .toISOString()
    .slice(0, 10);
function node(tag, text, className) {
  const el = document.createElement(tag);
  if (text !== undefined) el.textContent = text;
  if (className) el.className = className;
  return el;
}
function message(id, text = "") {
  $(id).textContent = text;
  $(id).hidden = !text;
}
function toast(text) {
  message("toast", text);
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => message("toast"), 4000);
}
function button(text, action, className = "text-button") {
  const el = node("button", text, className);
  el.type = "button";
  el.addEventListener("click", action);
  return el;
}
function blankRow(parent, columns, text) {
  const tr = node("tr");
  const td = node("td", text, "muted");
  td.colSpan = columns;
  tr.append(td);
  $(parent).replaceChildren(tr);
}
async function api(path, { method = "GET", body, signal } = {}) {
  const options = {
    method,
    signal,
    cache: "no-store",
    credentials: "omit",
    headers: { Accept: "application/json" },
  };
  if (body !== undefined) {
    options.headers["Content-Type"] = "application/json";
    options.headers["X-Gateway-Management"] = "1";
    options.body = JSON.stringify(body);
  }
  const response = await fetch(`/api/manage${path}`, options);
  const data = await response.json();
  if (!response.ok)
    throw new Error(data.error?.message || `请求失败（${response.status}）`);
  return data;
}
function queryString(values) {
  return new URLSearchParams(
    Object.entries(values).filter(
      ([, value]) => value !== null && value !== undefined,
    ),
  ).toString();
}
function renderKeys(items) {
  const rows = document.createDocumentFragment();
  for (const key of items) {
    const tr = node("tr");
    const name = node("td");
    const title = node("span", key.name, "key-name");
    title.title = key.name;
    name.append(title, node("span", time(key.created_at), "cell-subtitle"));
    tr.append(name);
    const masked = node("td");
    masked.append(node("code", key.masked_key, "mono masked"));
    tr.append(masked);
    const status = node("td");
    status.append(
      node(
        "span",
        key.status === "enabled" ? "启用" : "已禁用",
        `badge ${key.status === "disabled" ? "disabled" : ""}`,
      ),
    );
    tr.append(status);
    for (const [value, unknown] of [
      [key.today_tokens, key.today_unreported_requests],
      [key.total_tokens, key.total_unreported_requests],
    ]) {
      const td = node("td", count(value), "number");
      if (BigInt(unknown) > 0n)
        td.append(node("span", "含未完整上报", "unknown-mark"));
      tr.append(td);
    }
    const actions = node("td", undefined, "actions");
    const view = button("查看", () => openDetail(key));
    view.setAttribute("aria-label", `查看 ${key.name} 的用量`);
    actions.append(view);
    if (key.status === "enabled") {
      const disable = button(
        "禁用",
        () => openDisable(key),
        "text-button destructive",
      );
      disable.setAttribute("aria-label", `禁用 ${key.name}`);
      actions.append(disable);
    }
    tr.append(actions);
    rows.append(tr);
  }
  $("key-rows").replaceChildren(rows);
  $("key-count").textContent = `${items.length} / 本页`;
  $("list-empty").hidden = items.length > 0;
}
async function loadKeys() {
  const seq = ++state.listSequence;
  state.listController?.abort();
  state.listController = new AbortController();
  $("refresh").disabled = true;
  $("list-loading").hidden = state.keys.length > 0;
  try {
    const result = await api(
      `/keys?${queryString({ limit: 50, cursor: state.listCursors.at(-1) })}`,
      { signal: state.listController.signal },
    );
    if (seq !== state.listSequence) return;
    state.keys = result.items;
    state.listNext = result.next_cursor;
    state.offset = result.billing_timezone.utc_offset_minutes;
    renderKeys(result.items);
    message("list-error");
    $("updated").textContent = `更新于 ${time(result.as_of)}`;
    const sign = state.offset >= 0 ? "+" : "−";
    const hours = String(Math.floor(Math.abs(state.offset) / 60)).padStart(
      2,
      "0",
    );
    $("timezone").textContent =
      `UTC${sign}${hours}:${String(Math.abs(state.offset) % 60).padStart(2, "0")}`;
  } catch (error) {
    if (seq === state.listSequence && error.name !== "AbortError") {
      message(
        "list-error",
        `${error.message}。${state.keys.length ? "当前保留上次数据，可点击刷新重试。" : "请确认网关已启用管理功能且数据库可用，再点击刷新。"}`,
      );
      $("list-empty").hidden = true;
    }
  } finally {
    if (seq === state.listSequence) {
      $("refresh").disabled = false;
      $("list-loading").hidden = true;
      $("list-prev").disabled = state.listCursors.length === 1;
      $("list-next").disabled = !state.listNext;
      $("list-page").textContent = String(state.listCursors.length);
    }
  }
}
function openCreate() {
  $("create-form").reset();
  message("create-error");
  $("create-dialog").showModal();
  $("key-name").focus();
}
function closeCreate() {
  if (!state.createBusy) $("create-dialog").close();
}
$("create-open").addEventListener("click", openCreate);
$("empty-create").addEventListener("click", openCreate);
$("create-close").addEventListener("click", closeCreate);
$("create-cancel").addEventListener("click", closeCreate);
$("create-dialog").addEventListener("cancel", (event) => {
  if (state.createBusy) event.preventDefault();
});
$("create-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (state.createBusy) return;
  const name = $("key-name").value.trim();
  if (
    Array.from(name).length < 1 ||
    Array.from(name).length > 64 ||
    /[\x00-\x1f\x7f-\x9f]/u.test(name)
  ) {
    message("create-error", "请输入 1–64 个字符的名称，不含控制字符。");
    return;
  }
  state.createBusy = true;
  $("create-submit").disabled = true;
  $("create-submit").textContent = "正在创建…";
  message("create-error");
  try {
    const result = await api("/keys", { method: "POST", body: { name } });
    $("create-dialog").close();
    $("key-secret").value = result.secret;
    result.secret = null;
    $("copy-status").textContent = "";
    $("copy-secret").textContent = "复制密钥";
    $("secret-dialog").showModal();
    state.listCursors = [null];
    await loadKeys();
  } catch (error) {
    message(
      "create-error",
      `${error.message}。如未收到创建结果，请关闭此窗口并刷新列表确认；新条目若未保存密钥，可禁用后重新创建。`,
    );
  } finally {
    state.createBusy = false;
    $("create-submit").disabled = false;
    $("create-submit").textContent = "创建密钥";
  }
});
$("copy-secret").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText($("key-secret").value);
    $("copy-status").textContent = "";
    $("copy-secret").textContent = "✓ 已复制";
  } catch {
    $("key-secret").focus();
    $("key-secret").select();
    $("copy-status").textContent = "自动复制不可用。已选中密钥，请手动复制。";
  }
});
$("secret-close").addEventListener("click", () => {
  $("key-secret").value = "";
  $("secret-dialog").close();
});
$("secret-dialog").addEventListener("cancel", (event) => {
  if (!confirm("完整密钥关闭后无法再次查看。已妥善保存？"))
    event.preventDefault();
  else $("key-secret").value = "";
});
$("secret-dialog").addEventListener("close", () => {
  $("key-secret").value = "";
});
function openDisable(key) {
  state.disableKey = key;
  $("disable-name").textContent = `${key.name} · ${key.masked_key}`;
  message("disable-error");
  $("disable-dialog").showModal();
}
$("disable-cancel").addEventListener("click", () => {
  if (!state.disableBusy) $("disable-dialog").close();
});
$("disable-dialog").addEventListener("cancel", (event) => {
  if (state.disableBusy) event.preventDefault();
});
$("disable-confirm").addEventListener("click", async () => {
  if (state.disableBusy || !state.disableKey) return;
  state.disableBusy = true;
  $("disable-confirm").disabled = true;
  message("disable-error");
  try {
    await api(`/keys/${state.disableKey.id}/disable`, {
      method: "POST",
      body: {},
    });
    $("disable-dialog").close();
    toast("密钥已禁用，历史用量已保留。");
    await loadKeys();
  } catch (error) {
    message(
      "disable-error",
      `${error.message}。可重试或刷新列表确认当前状态。`,
    );
  } finally {
    state.disableBusy = false;
    $("disable-confirm").disabled = false;
  }
});
function resetDetails() {
  for (const field of ["billable", "input", "output", "cached"])
    $(`metric-${field}`).textContent = "—";
  $("day-rows").replaceChildren();
  $("request-rows").replaceChildren();
  $("detail-updated").textContent = "";
  $("incomplete").hidden = true;
  state.requestNext = null;
  $("request-prev").disabled = true;
  $("request-next").disabled = true;
}
function openDetail(key) {
  state.detail = key;
  state.requestCursors = [null];
  resetDetails();
  $("detail-title").textContent = key.name;
  $("detail-key").textContent =
    `${key.masked_key} · ${key.status === "enabled" ? "启用" : "已禁用"}`;
  $("date-start").value = day();
  $("date-end").value = day();
  selectPreset(1);
  $("detail-dialog").showModal();
  loadDetail();
}
function selectPreset(days) {
  document
    .querySelectorAll("[data-days]")
    .forEach((el) =>
      el.classList.toggle("selected", Number(el.dataset.days) === days),
    );
}
$("detail-close").addEventListener("click", () => $("detail-dialog").close());
$("detail-dialog").addEventListener("close", () => {
  state.detail = null;
  ++state.detailSequence;
  state.detailController?.abort();
  loadKeys();
});
document.querySelectorAll("[data-days]").forEach((el) =>
  el.addEventListener("click", () => {
    const days = Number(el.dataset.days);
    $("date-start").value = day(days - 1);
    $("date-end").value = day();
    selectPreset(days);
    changeRange();
  }),
);
function changeRange() {
  state.requestCursors = [null];
  resetDetails();
  loadDetail();
}
$("date-form").addEventListener("submit", (event) => {
  event.preventDefault();
  selectPreset(0);
  changeRange();
});
function renderUsage(result) {
  for (const field of ["billable", "input", "output", "cached"])
    $(`metric-${field}`).textContent = count(result.total[`${field}_tokens`]);
  const unknown = BigInt(result.total.unreported_requests);
  message(
    "incomplete",
    unknown > 0n
      ? `${count(unknown)} 个请求未完整上报，实际消耗可能不完整。`
      : "",
  );
  const rows = document.createDocumentFragment();
  for (const d of result.days) {
    const tr = node("tr");
    tr.append(
      node("td", d.day),
      node("td", count(d.requests), "number"),
      node("td", count(d.billable_tokens), "number"),
      node("td", BigInt(d.unreported_requests) > 0n ? "含未完整上报" : "完整"),
    );
    rows.append(tr);
  }
  $("day-rows").replaceChildren(rows);
  if (!result.days.length) blankRow("day-rows", 4, "所选日期暂无消耗");
  $("detail-updated").textContent = `更新于 ${time(result.as_of)}`;
}
function renderRequests(result) {
  const rows = document.createDocumentFragment();
  for (const request of result.requests) {
    const tr = node("tr");
    const when = node("td", time(request.finished_at));
    when.append(node("span", request.model, "cell-subtitle"));
    tr.append(when);
    const outcome = node("td", `${request.http_status}`);
    outcome.append(node("span", request.outcome, "cell-subtitle"));
    tr.append(outcome);
    for (const field of ["input_tokens", "output_tokens", "billable_tokens"])
      tr.append(node("td", count(request[field]), "number"));
    tr.append(node("td", request.usage_complete ? "完整" : "未完整上报"));
    rows.append(tr);
  }
  $("request-rows").replaceChildren(rows);
  if (!result.requests.length) blankRow("request-rows", 6, "所选日期暂无请求");
  state.requestNext = result.next_cursor;
}
async function loadDetail() {
  if (!state.detail) return;
  const seq = ++state.detailSequence;
  state.detailController?.abort();
  state.detailController = new AbortController();
  const start = $("date-start").value,
    end = $("date-end").value;
  const duration = (Date.parse(end) - Date.parse(start)) / 86400000;
  if (
    !start ||
    !end ||
    !Number.isFinite(duration) ||
    duration < 0 ||
    duration > 365
  ) {
    message("detail-error", "请选择有效的起止日期，范围为 1–366 天。");
    $("detail-loading").hidden = true;
    return;
  }
  $("detail-loading").hidden = false;
  try {
    const id = state.detail.id;
    const options = { signal: state.detailController.signal };
    const [usage, requests] = await Promise.all([
      api(`/keys/${id}/usage?${queryString({ start, end })}`, options),
      api(
        `/keys/${id}/requests?${queryString({ start, end, limit: 50, cursor: state.requestCursors.at(-1) })}`,
        options,
      ),
    ]);
    if (seq !== state.detailSequence || !state.detail) return;
    renderUsage(usage);
    renderRequests(requests);
    message("detail-error");
  } catch (error) {
    if (seq === state.detailSequence && error.name !== "AbortError")
      message("detail-error", `${error.message}。数据未更新，可点击查询重试。`);
  } finally {
    if (seq === state.detailSequence) {
      $("detail-loading").hidden = true;
      $("request-prev").disabled = state.requestCursors.length === 1;
      $("request-next").disabled = !state.requestNext;
    }
  }
}
$("refresh").addEventListener("click", loadKeys);
$("list-prev").addEventListener("click", () => {
  if (state.listCursors.length > 1) {
    state.listCursors.pop();
    loadKeys();
  }
});
$("list-next").addEventListener("click", () => {
  if (state.listNext) {
    state.listCursors.push(state.listNext);
    state.listNext = null;
    loadKeys();
  }
});
$("request-prev").addEventListener("click", () => {
  if (state.requestCursors.length > 1) {
    state.requestCursors.pop();
    loadDetail();
  }
});
$("request-next").addEventListener("click", () => {
  if (state.requestNext) {
    state.requestCursors.push(state.requestNext);
    state.requestNext = null;
    loadDetail();
  }
});
$("endpoint").textContent = `${location.origin}/v1`;
loadKeys();
setInterval(() => {
  if (
    document.hidden ||
    state.createBusy ||
    state.disableBusy ||
    $("create-dialog").open ||
    $("secret-dialog").open ||
    $("disable-dialog").open
  )
    return;
  if (state.detail) {
    if ($("detail-loading").hidden) loadDetail();
  } else if (!$("refresh").disabled) loadKeys();
}, 10000);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    state.listController?.abort();
    state.detailController?.abort();
  }
});
