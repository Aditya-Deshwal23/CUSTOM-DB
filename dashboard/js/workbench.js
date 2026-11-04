import { createEditor } from "./components/editor.js";
import { renderPlan } from "./components/plan.js";
import { FUNCTIONS, KEYWORDS } from "./sql/keywords.js";
import { formatSql } from "./sql/formatter.js";
import { detectParameters, inferType, typedParameterValue } from "./sql/parameters.js";
import { splitStatements, statementAt } from "./sql/statements.js";
import { tokenize } from "./sql/lexer.js";
import { toCsv, toInsert, toJson, toMarkdown } from "./exporters.js";

const STORAGE_KEY = "custom-db-workbench-v1";
const STORAGE_LIMIT = 200 * 1024;
const HISTORY_LIMIT = 500;
const MAX_TABS = 20;
const byId = (id) => document.getElementById(id);
const make = (tag, className = "", text = "") => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== "") node.textContent = text;
    return node;
};
const tokenizeForEditing = (sql) => tokenize(sql, { tolerant: true });
const state = {
    tabs: [{ id: "q1", sql: "SELECT * FROM Users LIMIT 100", pinned: false }],
    current: "q1",
    history: [],
    parameters: [],
    results: [],
    displayedResult: null,
    displayedSql: "",
    activeRequest: null,
    catalog: [],
    catalogPromise: null,
    output: "results",
    previousPlans: new Map(),
    planEnabled: false,
    batchEnabled: false,
    autocomplete: [],
    activeSuggestion: 0,
    historyIndex: -1,
    historyDraft: ""
};

function currentTab() {
    return state.tabs.find((tab) => tab.id === state.current);
}

function renderBatchChoices(results) {
    const host = byId("resultChoices");
    host.replaceChildren();
    results.forEach((result, index) => {
        const button = make("button", "button button-secondary",
            `${index + 1}. ${result.statement || "Statement"} · ${result.rows_affected || 0} affected`);
        button.type = "button";
        button.addEventListener("click", () => {
            renderRows(result, { ok: true, data: result }, state.displayedSql);
            outputTab("results");
        });
        host.appendChild(button);
    });
}

function status(message, error = false) {
    const output = byId("queryStatus");
    output.textContent = message;
    output.classList.toggle("error", error);
    if (error) byId("messagesPanel").hidden = false;
}

function persist() {
    const record = {
        version: 1,
        current: state.current,
        tabs: state.tabs.slice(-MAX_TABS),
        history: state.history.slice(-HISTORY_LIMIT)
    };
    let serialized = JSON.stringify(record);
    while (serialized.length > STORAGE_LIMIT && record.tabs.length > 1) {
        const index = record.tabs.findIndex((tab) => tab.id !== state.current);
        if (index < 0) break;
        record.tabs.splice(index, 1);
        serialized = JSON.stringify(record);
    }
    if (serialized.length > STORAGE_LIMIT) {
        status("Query workspace exceeds the 200 KB persistence limit; shorter SQL to save it.", true);
        return;
    }
    try {
        localStorage.setItem(STORAGE_KEY, serialized);
    } catch (error) {
        status(`Could not save query workspace: ${error.message}`, true);
    }
}

function restore() {
    try {
        const raw = localStorage.getItem(STORAGE_KEY);
        if (!raw) return;
        if (raw.length > STORAGE_LIMIT) {
            localStorage.removeItem(STORAGE_KEY);
            status("Saved query workspace exceeded 200 KB and was discarded.", true);
            return;
        }
        const saved = JSON.parse(raw);
        if (saved.version !== 1 || !Array.isArray(saved.tabs)) return;
        const tabs = saved.tabs.filter((tab) => typeof tab.id === "string" &&
            typeof tab.sql === "string").slice(-MAX_TABS);
        if (tabs.length) {
            state.tabs = tabs;
            state.current = tabs.some((tab) => tab.id === saved.current)
                ? saved.current : tabs[0].id;
        }
        if (Array.isArray(saved.history)) state.history = saved.history.slice(-HISTORY_LIMIT);
    } catch (error) {
        status(`Could not restore query workspace: ${error.message}`, true);
    }
}

function queryTitle(sql) {
    return (splitStatements(sql)[0] || "New query").replace(/\s+/g, " ").slice(0, 38);
}

function renderTabs() {
    const host = byId("queryTabs");
    host.replaceChildren();
    for (const tab of state.tabs) {
        const wrapper = make("div", `query-tab${tab.id === state.current ? " active" : ""}`);
        const button = make("button", "query-tab-title", queryTitle(tab.sql));
        button.type = "button";
        button.setAttribute("role", "tab");
        button.setAttribute("aria-selected", String(tab.id === state.current));
        button.addEventListener("click", () => selectTab(tab.id));
        const pin = make("button", "query-tab-close", tab.pinned ? "●" : "×");
        pin.type = "button";
        pin.setAttribute("aria-label", tab.pinned ? `Unpin ${queryTitle(tab.sql)}` :
            `Close ${queryTitle(tab.sql)}`);
        pin.title = tab.pinned ? "Unpin tab" : "Close tab";
        pin.addEventListener("click", () => tab.pinned ? (tab.pinned = false, renderTabs(), persist()) :
            closeTab(tab.id));
        wrapper.append(button, pin);
        host.appendChild(wrapper);
    }
}

function selectTab(id, saveCurrent = true) {
    const prior = currentTab();
    if (saveCurrent && prior) prior.sql = byId("sqlInput").value;
    state.current = id;
    const tab = currentTab();
    byId("sqlInput").value = tab.sql;
    editor.refresh();
    renderTabs();
    refreshDiagnostics();
    renderParameterBar();
    persist();
}

function newTab(sql = "") {
    if (state.tabs.length >= MAX_TABS) {
        status(`At most ${MAX_TABS} query tabs may be open.`, true);
        return;
    }
    const id = `q${Date.now()}-${state.tabs.length}`;
    state.tabs.push({ id, sql, pinned: false });
    selectTab(id);
    renderTabs();
    editor.focus();
}

function closeTab(id) {
    const tab = state.tabs.find((item) => item.id === id);
    if (!tab || tab.pinned) return;
    if (state.tabs.length === 1) {
        tab.sql = "";
        selectTab(tab.id);
        return;
    }
    const index = state.tabs.indexOf(tab);
    state.tabs.splice(index, 1);
    if (state.current === id) {
        state.current = state.tabs[Math.max(0, index - 1)].id;
        selectTab(state.current, false);
    } else {
        renderTabs();
        persist();
    }
}

function outputTab(name) {
    state.output = name;
    for (const button of document.querySelectorAll(".output-tab")) {
        button.setAttribute("aria-selected", String(button.dataset.panel === name));
    }
    for (const panel of document.querySelectorAll(".output-panel")) {
        panel.hidden = panel.id !== `${name}Panel`;
    }
}

function typedParams() {
    return state.parameters.map((parameter) => typedParameterValue(
        parameter.input.value,
        parameter.type.value === "auto" ? inferType(parameter.input.value) : parameter.type.value
    ));
}

function parameterCount(sql) {
    const indexes = tokenizeForEditing(sql)
        .filter((token) => token.kind === "parameter")
        .map((token) => Number(token.text.slice(1)));
    return indexes.length ? Math.max(...indexes) : 0;
}

function typedParamsFor(sql) {
    return typedParams().slice(0, parameterCount(sql));
}

function renderParameterBar() {
    const bar = byId("parameterBar");
    bar.replaceChildren();
    let parameters;
    try {
        parameters = detectParameters(byId("sqlInput").value);
    } catch (error) {
        bar.append(make("span", "parameter-error", error.message));
        return;
    }
    state.parameters = [];
    for (const parameter of parameters) {
        const field = make("label", "parameter-field", `$${parameter.index}`);
        const input = make("input", "control");
        input.type = "text";
        input.value = "";
        input.setAttribute("aria-label", `Value for parameter $${parameter.index}`);
        const type = make("select", "control");
        type.setAttribute("aria-label", `Type for parameter $${parameter.index}`);
        for (const optionName of ["auto", "int", "float", "boolean", "null", "text"]) {
            const option = make("option", "", optionName === "auto" ? "Auto" : optionName);
            option.value = optionName;
            type.appendChild(option);
        }
        input.addEventListener("input", () => {
            if (type.value === "auto") type.title = `Detected ${inferType(input.value)}`;
        });
        field.append(input, type);
        bar.appendChild(field);
        state.parameters.push({ input, type });
    }
    bar.hidden = parameters.length === 0;
}

function classify(sql) {
    const first = tokenizeForEditing(sql).find((token) => token.kind !== "comment");
    const keyword = first?.text.toUpperCase() || "";
    if (keyword === "EXPLAIN") {
        const tokens = tokenizeForEditing(sql);
        return tokens.some((token) => token.text.toUpperCase() === "ANALYZE") ? "EXPLAIN ANALYZE" : "EXPLAIN";
    }
    return keyword;
}

function isWrite(sql) {
    const kind = classify(sql);
    if (kind === "EXPLAIN") {
        const tokens = tokenizeForEditing(sql);
        return tokens.some((token) => ["INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "CHECKPOINT"]
            .includes(token.text.toUpperCase()));
    }
    return !["SELECT", "SHOW", "DESCRIBE"].includes(kind);
}

function refreshDiagnostics() {
    const sql = byId("sqlInput").value;
    const write = isWrite(sql);
    const readOnly = byId("studioMode").textContent === "READ ONLY";
    const run = byId("runQueryButton");
    run.disabled = (readOnly && write) || Boolean(state.activeRequest);
    run.title = state.activeRequest ? "A request is already running." :
        run.disabled ? "Disabled because the engine is read-only." : "Run statement";
    const warnings = [];
    const kind = classify(sql);
    if ((kind === "UPDATE" || kind === "DELETE") &&
        !tokenizeForEditing(sql).some((token) => token.text.toUpperCase() === "WHERE")) {
        warnings.push(`${kind} without WHERE affects every matching row.`);
    }
    if (kind === "DROP") warnings.push("DROP permanently removes a database object.");
    byId("editorWarnings").replaceChildren(...warnings.map((warning) =>
        make("p", "editor-warning", warning)));
    byId("runExplanation").textContent = run.disabled
        ? "Run is disabled because this engine is read-only; the server remains authoritative."
        : "";
}

function renderResult(result, sql, requestId, started, durationMs) {
    const data = result.data || {};
    const error = data.error || null;
    const ok = result.ok;
    const historyRecord = {
        id: `${Date.now()}-${requestId || ""}`,
        timestamp: new Date().toISOString(),
        sql,
        duration_ms: durationMs,
        rows: Number(data.row_count ?? data.results?.reduce(
            (total, item) => total + Number(item.row_count || 0), 0) ?? 0),
        affected: Number(data.rows_affected ?? data.results?.reduce(
            (total, item) => total + Number(item.rows_affected || 0), 0) ?? 0),
        status: data.committed === false ? "rolled back" : result.status,
        request_id: requestId || data.request_id || "",
        kind: data.results ? "BATCH" : data.statement || classify(sql),
        pinned: false
    };
    state.history.push(historyRecord);
    while (state.history.length > HISTORY_LIMIT) {
        const oldest = state.history.findIndex((item) => !item.pinned);
        if (oldest < 0) {
            state.history.pop();
            status("History is full of pinned entries; the oldest query was not retained.", true);
            break;
        }
        state.history.splice(oldest, 1);
    }
    byId("messagesPanel").replaceChildren();
    const batchAffected = data.results?.reduce(
        (total, item) => total + Number(item.rows_affected || 0), 0);
    const message = error
        ? `${error.code || "ERROR"}: ${error.message || "Request failed"}`
        : data.results
            ? data.committed === false
                ? "Batch rolled back; no changes were committed."
                : `Batch committed; ${data.results.length} statements, ${batchAffected} rows affected.`
            : `${data.statement || classify(sql)} completed; ${data.rows_affected || 0} rows affected.`;
    byId("messagesPanel").append(make("p", error ? "message-error" : "message-success", message));
    byId("messagesPanel").append(
        make("p", "message-meta", `HTTP ${result.status} · ${durationMs.toFixed(2)} ms client · ` +
            `${data.elapsed_us ?? "—"} µs server · request ${requestId || data.request_id || "—"}`)
    );
    if (data.pages_touched) {
        byId("messagesPanel").append(make("p", "message-meta",
            `Pages hit ${data.pages_touched.hit}; miss ${data.pages_touched.miss}`));
    }
    if (data.commit) {
        const commit = make("a", "message-link",
            `Commit LSN ${data.commit.lsn} · ${data.commit.pages_written} pages · ` +
            `${data.commit.bytes} bytes · fsync ${data.commit.fsync_us} µs · open WAL`);
        commit.href = "#/system/wal";
        byId("messagesPanel").appendChild(commit);
    }
    if (error?.line && error?.column) {
        editor.setError({
            line: Number(error.line),
            column: Number(error.column),
            message: error.hint ? `${error.message} Hint: ${error.hint}` : error.message
        });
        byId("messagesPanel").append(make("p", "diagnostic-message",
            `Line ${error.line}, column ${error.column}: ${message}${error.hint ? ` Hint: ${error.hint}` : ""}`));
    } else if (error) {
        editor.setError(null);
    }
    renderRows(data, result, sql);
    if (error?.line && error?.column) {
        renderErrorLocation(byId("messagesPanel"), sql, error);
    }
    if (data.plan || data.plan_tree) {
        const key = sql.trim();
        renderPlan(byId("planPanel"), data.plan_tree, data.plan || "",
            state.previousPlans.get(key));
        if (data.plan_tree) {
            state.previousPlans.set(key, collectPlanTimes(data.plan_tree));
        }
        outputTab("plan");
    } else if (data.rows || data.rows_affected !== undefined) {
        outputTab("results");
    } else {
        outputTab("messages");
    }
    state.results.unshift({ sql, result, requestId, durationMs, started });
    state.results = state.results.slice(0, 50);
    renderResultChoices();
    renderHistory();
    persist();
    window.dispatchEvent(new CustomEvent("custom-db:workbench-result", {
        detail: { sql, result: data, ok, status: result.status, requestId }
    }));
    if (ok && isWrite(sql)) {
        window.dispatchEvent(new Event("custom-db:catalog-request"));
        loadCatalog();
    }
    return historyRecord;
}

function collectPlanTimes(node, output = {}) {
    output[node.id] = { time_us: node.time_us };
    for (const child of node.children || []) collectPlanTimes(child, output);
    return output;
}

function renderRows(data, response, sql = state.displayedSql) {
    const host = byId("queryResult");
    host.replaceChildren();
    if (Array.isArray(data.results)) {
        const committed = data.committed;
        const first = data.results[0] || {};
        data = {
            ...first,
            row_count: first.row_count,
            rows_affected: first.rows_affected,
            truncated: first.truncated,
            committed,
            commit: data.commit
        };
        renderBatchChoices(response.data.results);
    }
    state.displayedResult = data;
    state.displayedSql = sql;
    const elapsed = make("div", "result-meta");
    elapsed.append(make("span", "type-badge", data.statement || "Statement"),
        make("span", "", `${data.row_count ?? data.rows?.length ?? 0} rows`),
        make("span", "", `${data.elapsed_us ?? "—"} µs server`));
    host.appendChild(elapsed);
    if (data.truncated) {
        host.appendChild(make("p", "truncated-banner", "Results truncated at 1,000 rows."));
    }
    if (response.ok && data.committed === false) {
        host.appendChild(make("p", "empty-state", "Batch explicitly rolled back; no changes were committed."));
    }
    if (Array.isArray(data.columns) && Array.isArray(data.rows) && data.columns.length) {
        const table = make("table", "workbench-result-table");
        table.id = "resultTable";
        const head = make("thead");
        const header = make("tr");
        for (const column of data.columns) {
            const cell = make("th", "", column.name);
            cell.scope = "col";
            cell.appendChild(make("span", "type-badge", column.type));
            header.appendChild(cell);
        }
        head.appendChild(header);
        table.appendChild(head);
        const body = make("tbody");
        data.rows.forEach((row) => {
            const rowNode = make("tr");
            row.forEach((value) => rowNode.appendChild(make("td",
                value === null ? "null-value" : "", value === null ? "NULL" : String(value))));
            body.appendChild(rowNode);
        });
        table.appendChild(body);
        host.appendChild(table);
    } else if (response.ok && Array.isArray(data.columns) && data.columns.length === 0) {
        host.appendChild(make("p", "empty-state", "Statement completed without result columns."));
    }
}

function renderErrorLocation(host, sql, error) {
    const lineNumber = Number(error.line);
    const columnNumber = Number(error.column);
    if (lineNumber < 1 || columnNumber < 1) return;
    const block = make("pre", "error-location");
    const lines = sql.split("\n");
    lines.forEach((line, index) => {
        if (index > 0) block.appendChild(document.createTextNode("\n"));
        block.appendChild(document.createTextNode(`${index + 1} | `));
        if (index !== lineNumber - 1) {
            block.appendChild(document.createTextNode(line));
            return;
        }
        const offset = Math.min(columnNumber - 1, line.length);
        block.appendChild(document.createTextNode(line.slice(0, offset)));
        const marker = make("mark", "", line.slice(offset, offset + 1) || " ");
        block.appendChild(marker);
        block.appendChild(document.createTextNode(line.slice(offset + 1)));
    });
    host.appendChild(block);
}

function selectedResultExport() {
    const data = state.displayedResult;
    if (!data || !Array.isArray(data.columns) || !Array.isArray(data.rows)) {
        throw new Error("Run a query with result columns before exporting.");
    }
    const rows = data.rows.map((values) => Object.fromEntries(data.columns.map((column, index) =>
        [column.name, values[index] ?? null])));
    const format = byId("resultExportFormat").value;
    let content;
    let extension;
    let mimeType;
    if (format === "CSV") {
        content = toCsv(data.columns, rows);
        extension = "csv";
        mimeType = "text/csv;charset=utf-8";
    } else if (format === "JSON") {
        content = toJson(data.columns, rows);
        extension = "json";
        mimeType = "application/json;charset=utf-8";
    } else if (format === "Markdown") {
        content = toMarkdown(data.columns, rows);
        extension = "md";
        mimeType = "text/markdown;charset=utf-8";
    } else {
        const tokens = tokenizeForEditing(state.displayedSql);
        const from = tokens.findIndex((token) => token.text.toUpperCase() === "FROM");
        const table = from >= 0 ? tokens[from + 1]?.text : "";
        if (!table || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(table)) {
            throw new Error("SQL INSERT export needs a simple table name after FROM.");
        }
        content = toInsert(table, data.columns, rows);
        extension = "sql";
        mimeType = "application/sql;charset=utf-8";
    }
    return { content, extension, mimeType };
}

function setupResultExport() {
    byId("copyResultsButton").addEventListener("click", async () => {
        try {
            const { content } = selectedResultExport();
            await navigator.clipboard.writeText(content);
            status("Results copied to the clipboard.");
        } catch (error) {
            status(`Could not copy results: ${error.message}`, true);
        }
    });
    byId("downloadResultsButton").addEventListener("click", () => {
        try {
            const { content, extension, mimeType } = selectedResultExport();
            const url = URL.createObjectURL(new Blob([content], { type: mimeType }));
            const link = make("a");
            link.href = url;
            link.download = `custom-db-results.${extension}`;
            document.body.appendChild(link);
            link.click();
            link.remove();
            window.setTimeout(() => URL.revokeObjectURL(url), 1000);
        } catch (error) {
            status(`Could not export results: ${error.message}`, true);
        }
    });
}

function renderResultChoices() {
    const host = byId("resultChoices");
    host.replaceChildren();
    state.results.slice(0, 10).forEach((item, index) => {
        const button = make("button", "button button-secondary",
            `Result ${index + 1}: ${queryTitle(item.sql)}`);
        button.type = "button";
        button.addEventListener("click", () => {
            renderRows(item.result.data || {}, item.result, item.sql);
            outputTab("results");
        });
        host.appendChild(button);
    });
}

function executeRequest(sql, params, endpoint = "/api/v1/query", batchScript = false) {
    if (state.activeRequest) {
        status("A request is already running.", true);
        return Promise.resolve(null);
    }
    const requestId = globalThis.crypto?.randomUUID?.() || `wb-${Date.now()}-${Math.trunc(performance.now())}`;
    const controller = new AbortController();
    state.activeRequest = { requestId, controller, sql, started: performance.now() };
    byId("stopQueryButton").disabled = false;
    refreshDiagnostics();
    status("Running query…");
    return fetch(endpoint, {
        method: "POST",
        headers: {
            Accept: "application/json",
            "Content-Type": "application/json",
            "X-Request-Id": requestId
        },
        body: JSON.stringify(endpoint.endsWith("/batch")
            ? (batchScript ? { script: sql, params } : { statements: params })
            : { sql, params }),
        signal: controller.signal
    }).then(async (response) => {
        let data;
        try {
            data = await response.json();
        } catch (error) {
            throw new Error(`Invalid JSON response: ${error.message}`);
        }
        const result = { ok: response.ok, status: response.status, data };
        const active = state.activeRequest;
        if (active?.requestId === requestId) {
            const duration = performance.now() - active.started;
            state.activeRequest = null;
            byId("stopQueryButton").disabled = true;
            refreshDiagnostics();
            renderResult(result, sql, requestId, active.started, duration);
            status(response.ok ? "Query completed." : `${data.error?.code || response.status}: ${data.error?.message || "Query failed"}`,
                !response.ok);
        }
        return result;
    }).catch((error) => {
        if (error.name === "AbortError") {
            if (state.activeRequest?.requestId === requestId) {
                state.activeRequest = null;
                byId("stopQueryButton").disabled = true;
                refreshDiagnostics();
                status("Query cancelled.");
            }
        } else {
            state.activeRequest = null;
            byId("stopQueryButton").disabled = true;
            refreshDiagnostics();
            renderResult({ ok: false, status: 0, data: { error: { code: "REQUEST_ERROR", message: error.message } } },
                sql, requestId, performance.now(), 0);
            status(`Request failed: ${error.message}`, true);
        }
        return null;
    });
}

async function runOne(sql) {
    if (byId("studioMode").textContent === "READ ONLY" && isWrite(sql)) {
        status("Writes are disabled because the engine is read-only.", true);
        return null;
    }
    let params;
    try {
        params = typedParamsFor(sql);
    } catch (error) {
        status(error.message, true);
        return null;
    }
    const executable = state.planEnabled && classify(sql) !== "EXPLAIN"
        ? `EXPLAIN ANALYZE ${sql}` : sql;
    return executeRequest(executable, params);
}

async function runAll() {
    const statements = splitStatements(byId("sqlInput").value);
    for (const sql of statements) {
        const result = await runOne(sql);
        if (!result?.ok) break;
    }
}

async function runAtomicBatch() {
    const sql = byId("sqlInput").value;
    const statements = splitStatements(sql);
    if (!statements.length) return status("Enter SQL before running a batch.", true);
    if (statements.length > 50) return status("An atomic batch is limited to 50 statements.", true);
    if (byId("studioMode").textContent === "READ ONLY" && statements.some(isWrite)) {
        return status("Write batches are disabled because the engine is read-only.", true);
    }
    let params;
    try {
        params = typedParams();
    } catch (error) {
        return status(error.message, true);
    }
    const tokens = tokenizeForEditing(sql);
    if (tokens[0]?.text.toUpperCase() === "BEGIN") {
        await executeRequest(sql, params, "/api/v1/batch", true);
        return;
    }
    const body = statements.map((statement) => {
        return { sql: statement, params: params.slice(0, parameterCount(statement)) };
    });
    await executeRequest(sql, body, "/api/v1/batch");
}

function runCurrent() {
    if (byId("batchToggle").checked) return runAtomicBatch();
    const textarea = byId("sqlInput");
    const selected = textarea.value.slice(textarea.selectionStart, textarea.selectionEnd);
    const sql = selected.trim() || statementAt(textarea.value, textarea.selectionStart);
    if (!sql) return status("Place the caret in a statement or select SQL to run.", true);
    if (byId("studioMode").textContent === "READ ONLY" && isWrite(sql)) {
        return status("Writes are disabled because the engine is read-only.", true);
    }
    return runOne(sql);
}

async function requestCancellation(requestId) {
    const response = await fetch(`/api/v1/query/${encodeURIComponent(requestId)}`, {
        method: "DELETE",
        headers: { "X-Request-Id": `cancel-${requestId}` }
    });
    if (!response.ok) {
        const data = await response.json().catch(() => ({}));
        throw new Error(data.error?.message || `HTTP ${response.status}`);
    }
}

async function stopQuery() {
    const active = state.activeRequest;
    if (!active) return;
    byId("stopQueryButton").disabled = true;
    status("Requesting server-side cancellation…");
    try {
        await requestCancellation(active.requestId);
        active.controller.abort();
    } catch (error) {
        byId("stopQueryButton").disabled = false;
        status(`Cancellation request failed: ${error.message}`, true);
    }
}

function renderHistory() {
    const filter = byId("historyFilter").value.trim().toLowerCase();
    const host = byId("historyList");
    host.replaceChildren();
    for (const item of [...state.history].reverse().filter((entry) =>
        !filter || `${entry.sql} ${entry.kind} ${entry.status}`.toLowerCase().includes(filter))) {
        const row = make("div", "history-entry");
        const reopen = make("button", "history-query", item.sql);
        reopen.type = "button";
        reopen.addEventListener("click", () => {
            byId("sqlInput").value = item.sql;
            currentTab().sql = item.sql;
            editor.refresh();
            renderParameterBar();
            refreshDiagnostics();
            outputTab("results");
            editor.focus();
        });
        const meta = make("span", "history-meta",
            `${new Date(item.timestamp).toLocaleString()} · ${item.kind} · ${item.duration_ms.toFixed(1)} ms · ` +
            `${item.rows} rows · ${item.status} · ${item.request_id}`);
        const pin = make("button", "button button-secondary", item.pinned ? "Unpin" : "Pin");
        pin.type = "button";
        pin.addEventListener("click", () => {
            item.pinned = !item.pinned;
            renderHistory();
            persist();
        });
        row.append(reopen, meta, pin);
        host.appendChild(row);
    }
}

function autocompleteItems() {
    const textarea = byId("sqlInput");
    const before = textarea.value.slice(0, textarea.selectionStart);
    const match = before.match(/([A-Za-z_][A-Za-z0-9_]*)?$/);
    const prefix = match?.[0] || "";
    const dot = before.endsWith(".");
    const suggestions = new Set();
    if (dot) {
        const aliasToken = tokenizeForEditing(before.slice(0, -1)).at(-1)?.text;
        const tables = state.catalog;
        const aliases = parseAliases(before, tables);
        const tableName = aliases.get(aliasToken?.toLowerCase()) || aliasToken;
        const table = tables.find((item) => item.name.toLowerCase() === tableName?.toLowerCase());
        for (const column of table?.columns || []) suggestions.add(column.name);
    } else {
        for (const word of KEYWORDS) suggestions.add(word);
        for (const word of FUNCTIONS) suggestions.add(`${word}()`);
        for (const table of state.catalog) suggestions.add(table.name);
        if (prefix.toLowerCase() === "sel") suggestions.add(
            "SELECT * FROM ${table} WHERE ${condition} ORDER BY ${column} LIMIT 100");
        if (prefix) {
            for (const table of state.catalog) {
                for (const column of table.columns || []) suggestions.add(column.name);
            }
        }
    }
    return [...suggestions].filter((value) => value.toLowerCase().startsWith(prefix.toLowerCase()))
        .sort().slice(0, 40);
}

function parseAliases(source, tables) {
    const tokens = tokenizeForEditing(source);
    const aliases = new Map(tables.map((table) => [table.name.toLowerCase(), table.name]));
    for (let index = 0; index < tokens.length; index += 1) {
        if (!["FROM", "JOIN"].includes(tokens[index].text.toUpperCase())) continue;
        const table = tokens[index + 1]?.text;
        if (!table) continue;
        let aliasIndex = index + 2;
        if (tokens[aliasIndex]?.text.toUpperCase() === "AS") aliasIndex += 1;
        const alias = tokens[aliasIndex]?.text;
        if (alias && !KEYWORDS.has(alias.toUpperCase())) aliases.set(alias.toLowerCase(), table);
    }
    return aliases;
}

async function loadCatalog() {
    if (state.catalogPromise) return state.catalogPromise;
    state.catalogPromise = new Promise((resolve, reject) => {
        const cleanup = () => {
            clearTimeout(timeout);
            window.removeEventListener("custom-db:catalog", onCatalog);
            window.removeEventListener("custom-db:catalog-error", onError);
        };
        const onCatalog = (event) => {
            state.catalog = event.detail || [];
            cleanup();
            resolve(state.catalog);
        };
        const onError = (event) => {
            cleanup();
            reject(new Error(event.detail?.message || "Catalog refresh failed."));
        };
        const timeout = window.setTimeout(() => {
            cleanup();
            reject(new Error("Catalog refresh timed out."));
        }, 5000);
        window.addEventListener("custom-db:catalog", onCatalog);
        window.addEventListener("custom-db:catalog-error", onError);
        window.dispatchEvent(new Event("custom-db:catalog-request"));
    }).catch((error) => {
        status(`Autocomplete catalog unavailable: ${error.message}`, true);
        return state.catalog;
    }).finally(() => { state.catalogPromise = null; });
    return state.catalogPromise;
}

function showAutocomplete() {
    const input = byId("sqlInput");
    const host = byId("autocompleteList");
    const items = autocompleteItems();
    state.autocomplete = items;
    state.activeSuggestion = 0;
    host.replaceChildren();
    for (const [index, item] of items.entries()) {
        const button = make("button", "autocomplete-item", item);
        button.type = "button";
        button.setAttribute("role", "option");
        button.setAttribute("aria-selected", String(index === 0));
        button.addEventListener("mousedown", (event) => {
            event.preventDefault();
            acceptSuggestion(item);
        });
        host.appendChild(button);
    }
    host.hidden = items.length === 0;
    if (items.length) input.setAttribute("aria-controls", "autocompleteList");
}

function moveAutocomplete(direction) {
    if (!state.autocomplete.length) return;
    state.activeSuggestion = (state.activeSuggestion + direction + state.autocomplete.length) %
        state.autocomplete.length;
    for (const [index, option] of [...byId("autocompleteList").children].entries()) {
        option.setAttribute("aria-selected", String(index === state.activeSuggestion));
    }
}

function acceptSuggestion(value = state.autocomplete[state.activeSuggestion]) {
    if (!value) return;
    const input = byId("sqlInput");
    const start = input.selectionStart;
    const prefix = input.value.slice(0, start).match(/[A-Za-z_][A-Za-z0-9_]*$/)?.[0] || "";
    const replacement = prefix.toLowerCase() === "sel" && value.includes("${")
        ? "SELECT * FROM  WHERE  ORDER BY  LIMIT 100" : value;
    input.setRangeText(replacement, start - prefix.length, start, "end");
    input.dispatchEvent(new Event("input", { bubbles: true }));
    byId("autocompleteList").hidden = true;
    input.focus();
}

function setupFindReplace() {
    const bar = byId("findReplaceBar");
    byId("findButton").addEventListener("click", () => {
        bar.hidden = !bar.hidden;
        if (!bar.hidden) byId("findInput").focus();
    });
    byId("findNextButton").addEventListener("click", () => {
        const input = byId("sqlInput");
        const query = byId("findInput").value;
        if (!query) return;
        let index = input.value.indexOf(query, input.selectionEnd);
        if (index < 0) index = input.value.indexOf(query);
        if (index >= 0) input.setSelectionRange(index, index + query.length);
        input.focus();
    });
    byId("replaceButton").addEventListener("click", () => {
        const input = byId("sqlInput");
        const needle = byId("findInput").value;
        const replacement = byId("replaceInput").value;
        if (!needle) return;
        const start = input.selectionStart;
        const end = input.selectionEnd;
        if (input.value.slice(start, end) === needle) {
            input.setRangeText(replacement, start, end, "select");
        } else {
            input.value = input.value.replaceAll(needle, replacement);
        }
        input.dispatchEvent(new Event("input", { bubbles: true }));
        input.focus();
    });
    byId("closeFindButton").addEventListener("click", () => { bar.hidden = true; editor.focus(); });
}

async function refreshActivity() {
    const host = byId("activityList");
    try {
        const response = await fetch("/api/v1/activity", { headers: { Accept: "application/json" } });
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        host.replaceChildren();
        for (const item of data.activity || []) {
            const row = make("div", "activity-entry");
            row.append(make("code", "", item.request_id),
                make("span", "", `${item.state} · ${item.sql_preview}`));
            const cancel = make("button", "button button-secondary", "Cancel");
            cancel.type = "button";
            cancel.addEventListener("click", async () => {
                cancel.disabled = true;
                try {
                    await requestCancellation(item.request_id);
                    await refreshActivity();
                } catch (error) {
                    cancel.disabled = false;
                    status(`Cancellation request failed: ${error.message}`, true);
                }
            });
            row.appendChild(cancel);
            host.appendChild(row);
        }
        if (!data.activity?.length) host.append(make("p", "empty-state", "No active statements."));
    } catch (error) {
        host.replaceChildren(make("p", "empty-state",
            `Activity endpoint unavailable: ${error.message}`));
    }
}

function setupOutputTabs() {
    for (const button of document.querySelectorAll(".output-tab")) {
        button.addEventListener("click", () => {
            outputTab(button.dataset.panel);
            if (button.dataset.panel === "activity") refreshActivity();
        });
    }
    document.addEventListener("keydown", (event) => {
        if (event.key === "F2") {
            event.preventDefault();
            outputTab("results");
            byId("queryOutput").scrollIntoView({ block: "nearest" });
        }
    });
}

let editor;
let restoringHistory = false;

function navigateHistory(event) {
    const input = byId("sqlInput");
    if (event.key === "ArrowUp" && input.selectionStart === 0 && input.selectionEnd === 0) {
        if (state.history.length === 0) return;
        event.preventDefault();
        if (state.historyIndex === -1) {
            state.historyDraft = input.value;
            state.historyIndex = state.history.length;
        }
        state.historyIndex = Math.max(0, state.historyIndex - 1);
    } else if (event.key === "ArrowDown" && state.historyIndex !== -1 &&
        (input.selectionStart === 0 || input.selectionStart === input.value.length)) {
        event.preventDefault();
        state.historyIndex += 1;
        if (state.historyIndex >= state.history.length) {
            state.historyIndex = -1;
            restoringHistory = true;
            input.value = state.historyDraft;
            input.dispatchEvent(new Event("input", { bubbles: true }));
            restoringHistory = false;
            input.setSelectionRange(input.value.length, input.value.length);
            return;
        }
    } else {
        return;
    }

    restoringHistory = true;
    input.value = state.history[state.historyIndex].sql;
    input.dispatchEvent(new Event("input", { bubbles: true }));
    restoringHistory = false;
    input.setSelectionRange(event.key === "ArrowUp" ? 0 : input.value.length,
        event.key === "ArrowUp" ? 0 : input.value.length);
}

export function startSqlWorkbench() {
    restore();
    byId("sqlInput").value = currentTab().sql;
    editor = createEditor({
        textarea: byId("sqlInput"),
        highlight: byId("editorHighlight"),
        gutter: byId("lineNumbers"),
        currentLine: byId("currentLine"),
        diagnostic: byId("editorDiagnostic"),
        onChange: (sql) => {
            if (!restoringHistory) state.historyIndex = -1;
            currentTab().sql = sql;
            renderTabs();
            renderParameterBar();
            refreshDiagnostics();
            persist();
            if (byId("sqlInput").value.slice(0, byId("sqlInput").selectionStart).endsWith(".")) {
                requestAnimationFrame(() => { loadCatalog().then(showAutocomplete); });
            }
        },
        onFind: () => byId("findButton").click(),
        onAutocomplete: () => loadCatalog().then(showAutocomplete)
    });
    renderTabs();
    renderParameterBar();
    refreshDiagnostics();
    renderHistory();
    setupFindReplace();
    setupOutputTabs();
    setupResultExport();
    byId("sqlInput").addEventListener("keydown", (event) => {
        if (event.isComposing || event.keyCode === 229) return;
        const modifier = event.ctrlKey || event.metaKey;
        if (modifier && event.key === "Enter") {
            event.preventDefault();
            event.stopImmediatePropagation();
            event.shiftKey ? runAll() : runCurrent();
        } else if (modifier && event.shiftKey && event.key.toLowerCase() === "f") {
            event.preventDefault();
            event.stopImmediatePropagation();
            const input = byId("sqlInput");
            const formatted = formatSql(input.value);
            input.value = formatted;
            input.dispatchEvent(new Event("input", { bubbles: true }));
        } else if (modifier && event.key.toLowerCase() === "t") {
            event.preventDefault();
            event.stopImmediatePropagation();
            newTab();
        } else if (modifier && event.key.toLowerCase() === "w") {
            event.preventDefault();
            event.stopImmediatePropagation();
            closeTab(state.current);
        } else if (modifier && event.key === "Tab") {
            event.preventDefault();
            event.stopImmediatePropagation();
            const index = state.tabs.findIndex((tab) => tab.id === state.current);
            const next = state.tabs[(index + (event.shiftKey ? -1 : 1) + state.tabs.length) %
                state.tabs.length];
            selectTab(next.id);
        } else if (!modifier && !event.altKey && !event.isComposing && event.key === "Enter" &&
            event.target === byId("sqlInput")) {
            // Enter remains the native newline action.
        } else if ((event.key === "ArrowUp" || event.key === "ArrowDown") &&
            byId("autocompleteList").hidden) {
            navigateHistory(event);
        } else if ((event.key === "ArrowDown" || event.key === "ArrowUp") &&
            !byId("autocompleteList").hidden) {
            event.preventDefault();
            moveAutocomplete(event.key === "ArrowDown" ? 1 : -1);
        } else if (event.key === "Enter" && !byId("autocompleteList").hidden) {
            event.preventDefault();
            acceptSuggestion();
        } else if (event.key === "Escape") {
            byId("autocompleteList").hidden = true;
            stopQuery();
        }
    }, true);
    byId("runQueryButton").addEventListener("click", (event) => {
        event.preventDefault();
        event.stopImmediatePropagation();
        runCurrent();
    }, true);
    byId("stopQueryButton").addEventListener("click", stopQuery);
    byId("refreshActivity").addEventListener("click", refreshActivity);
    byId("formatSqlButton").addEventListener("click", () => {
        const input = byId("sqlInput");
        input.value = formatSql(input.value);
        input.dispatchEvent(new Event("input", { bubbles: true }));
    });
    byId("batchToggle").addEventListener("change", (event) => {
        state.batchEnabled = event.target.checked;
        byId("runQueryButton").textContent = event.target.checked ? "Run atomic batch" : "Run";
    });
    byId("runPlanToggle").addEventListener("change", (event) => {
        state.planEnabled = event.target.checked;
    });
    byId("newQueryTab").addEventListener("click", () => newTab());
    byId("historyFilter").addEventListener("input", renderHistory);
    byId("historyPinAll").addEventListener("click", () => {
        for (const item of state.history) item.pinned = true;
        renderHistory();
        persist();
    });
    byId("autocompleteList").addEventListener("keydown", (event) => {
        if (event.key === "Escape") byId("autocompleteList").hidden = true;
    });
    byId("autocompleteList").addEventListener("pointerdown", (event) => {
        event.preventDefault();
    });
    window.addEventListener("custom-db:catalog", (event) => {
        state.catalog = event.detail || [];
    });
    window.addEventListener("custom-db:refresh-catalog", () => {
        loadCatalog();
    });
    document.addEventListener("keydown", (event) => {
        if (event.key === "F5") {
            event.preventDefault();
            loadCatalog();
        }
    });
    loadCatalog();
}
