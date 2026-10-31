"use strict";

const API_BASE_URL =
    window.location.port === "5500" || window.location.port === "5501"
        ? "http://127.0.0.1:8080"
        : "";
const POLL_INTERVAL_MS = 500;
const HISTORY_WINDOW_MS = 60_000;
const QUERY_HISTORY_KEY = "custom-db-query-history";
const replayMode = new URLSearchParams(window.location.search).get("replay") === "1";
const SEED_SCRIPT = `CREATE TABLE Users (
  id INT PRIMARY KEY,
  username VARCHAR(80) NOT NULL,
  email VARCHAR(120) NOT NULL UNIQUE
);
CREATE TABLE Products (
  id INT PRIMARY KEY,
  sku VARCHAR(32) NOT NULL UNIQUE,
  price DOUBLE NOT NULL,
  stock_qty INT NOT NULL
);
CREATE TABLE Orders (
  id INT PRIMARY KEY,
  user_id INT REFERENCES Users(id),
  total_amount DOUBLE NOT NULL,
  status VARCHAR(24) NOT NULL
);
CREATE TABLE OrderItems (
  id INT PRIMARY KEY,
  order_id INT REFERENCES Orders(id),
  product_id INT REFERENCES Products(id),
  qty INT NOT NULL
);
INSERT INTO Users VALUES
  (1, 'Ada', 'ada@example.test'),
  (2, 'Grace', 'grace@example.test');
INSERT INTO Products VALUES
  (1, 'DB-001', 12.5, 20),
  (2, 'DB-002', 8.25, 50);
INSERT INTO Orders VALUES
  (1, 1, 20.75, 'paid'),
  (2, 2, 8.25, 'shipped');
INSERT INTO OrderItems VALUES
  (1, 1, 1, 1),
  (2, 1, 2, 1),
  (3, 2, 2, 1);`;

const state = {
    inFlight: new Map(),
    controllers: new Set(),
    polling: false,
    pollInterval: null,
    failures: 0,
    nextPollAt: 0,
    previousStats: null,
    previousSampleAt: null,
    qpsHistory: [],
    frameSignature: "",
    catalog: null,
    catalogSampledAt: 0,
    queryHistory: readQueryHistory(),
    historyIndex: -1,
    historyDraft: "",
    replay: null,
    replayInterval: null,
    replayStartedAt: 0,
    replayElapsedBeforePause: 0,
    replaySampleIndex: 0,
    replayQueryIndex: 0
};

function byId(id) {
    return document.getElementById(id);
}

function setText(id, value) {
    byId(id).textContent = value;
}

function clearChildren(element) {
    while (element.firstChild) {
        element.removeChild(element.firstChild);
    }
}

function node(tag, className, text) {
    const element = document.createElement(tag);
    if (className) {
        element.className = className;
    }
    if (text !== undefined) {
        element.textContent = String(text);
    }
    return element;
}

function readQueryHistory() {
    try {
        const saved = window.localStorage.getItem(QUERY_HISTORY_KEY);
        const parsed = saved ? JSON.parse(saved) : [];
        return Array.isArray(parsed) ? parsed.filter((item) => typeof item === "string") : [];
    } catch (error) {
        console.error("Could not read saved SQL history:", error);
        return [];
    }
}

function saveQueryHistory(sql) {
    const history = state.queryHistory.filter((item) => item !== sql);
    history.push(sql);
    state.queryHistory = history.slice(-50);
    state.historyIndex = -1;
    state.historyDraft = "";
    try {
        window.localStorage.setItem(QUERY_HISTORY_KEY, JSON.stringify(state.queryHistory));
    } catch (error) {
        console.error("Could not save SQL history:", error);
        setQueryStatus("Query history could not be saved in this browser.", true);
    }
}

function setQueryStatus(message, isError = false) {
    const status = byId("queryStatus");
    status.textContent = message;
    status.classList.toggle("error", isError);
}

function setOnlineStatus(online, message) {
    const badge = byId("mainStatusBadge");
    badge.classList.toggle("online", online);
    badge.classList.toggle("offline", !online);
    setText("statusText", message);
}

function showOffline(error) {
    state.failures += 1;
    const backoffMs = Math.min(1000 * (2 ** Math.min(state.failures - 1, 5)), 30_000);
    state.nextPollAt = Date.now() + backoffMs;
    setOnlineStatus(false, "ENGINE OFFLINE");
    byId("offlineState").hidden = false;
    byId("retryButton").hidden = false;
    setText("offlineTitle", state.failures === 1
        ? "Waking up — Render free-tier cold start"
        : "Offline — engine API unreachable");
    setText("offlineMessage", state.failures === 1
        ? "Waiting for the live engine. Retrying with backoff; no metrics are being invented."
        : `The engine request failed (${error.message}). Retrying in ${Math.ceil(backoffMs / 1000)} seconds. Render free-tier cold starts can take up to a minute.`);
    setText("liveRibbon", "LIVE — engine offline; telemetry is unavailable");
    setText("pollStatus", `Retry in ${Math.ceil(backoffMs / 1000)}s`);
    clearTelemetry();
    state.previousStats = null;
    state.previousSampleAt = null;
    state.qpsHistory = [];
    clearBufferPool();
    clearStorage();
}

function showOnline() {
    state.failures = 0;
    state.nextPollAt = Date.now() + 1000;
    byId("offlineState").hidden = true;
    byId("retryButton").hidden = true;
    setOnlineStatus(true, replayMode ? "REPLAY" : "LIVE ENGINE");
    setText("pollStatus", "Live samples updating");
}

function requestJSON(key, path, options = {}) {
    if (state.inFlight.has(key)) {
        return state.inFlight.get(key);
    }
    const controller = new AbortController();
    state.controllers.add(controller);
    const promise = fetch(`${API_BASE_URL}${path}`, {
        ...options,
        signal: controller.signal,
        headers: {
            Accept: "application/json",
            ...(options.headers || {})
        }
    }).then(async (response) => {
        let data;
        try {
            data = await response.json();
        } catch (error) {
            throw new Error(`Invalid JSON from ${path}: ${error.message}`);
        }
        return { ok: response.ok, status: response.status, data };
    }).finally(() => {
        state.inFlight.delete(key);
        state.controllers.delete(controller);
    });
    state.inFlight.set(key, promise);
    return promise;
}

function abortRequests() {
    for (const controller of state.controllers) {
        controller.abort();
    }
    state.controllers.clear();
}

function ensureSuccess(result, path) {
    if (!result.ok) {
        const message = result.data && result.data.error && result.data.error.message;
        throw new Error(`${path} returned HTTP ${result.status}${message ? `: ${message}` : ""}`);
    }
    return result.data;
}

async function fetchCatalog() {
    const tablesResult = await requestJSON("catalog-names", "/api/v1/tables");
    const tables = ensureSuccess(tablesResult, "/api/v1/tables").tables;
    if (!Array.isArray(tables)) {
        throw new Error("Catalog response is missing its tables list.");
    }
    const details = await Promise.all(tables.map(async (tableName) => {
        const result = await requestJSON(`catalog-${tableName}`, `/api/v1/tables/${encodeURIComponent(tableName)}`);
        return ensureSuccess(result, `/api/v1/tables/${tableName}`);
    }));
    return details;
}

async function pollLive() {
    if (state.polling || document.hidden || Date.now() < state.nextPollAt) {
        return;
    }
    state.polling = true;
    try {
        const refreshCatalog = state.catalog === null || Date.now() - state.catalogSampledAt >= 30_000;
        const [statsResult, framesResult, storageResult, versionResult, catalog] = await Promise.all([
            requestJSON("stats", "/api/v1/stats"),
            requestJSON("buffer-pool", "/api/v1/stats/buffer_pool"),
            requestJSON("storage", "/api/v1/stats/storage"),
            requestJSON("version", "/api/v1/version"),
            refreshCatalog
                ? fetchCatalog()
                : Promise.resolve(state.catalog)
        ]);
        const sample = {
            stats: ensureSuccess(statsResult, "/api/v1/stats"),
            buffer_pool: ensureSuccess(framesResult, "/api/v1/stats/buffer_pool"),
            storage: ensureSuccess(storageResult, "/api/v1/stats/storage"),
            version: ensureSuccess(versionResult, "/api/v1/version"),
            catalog
        };
        state.catalog = catalog;
        if (refreshCatalog) {
            state.catalogSampledAt = Date.now();
        }
        renderSample(sample, Date.now());
        showOnline();
        updateLiveRibbon(sample);
    } catch (error) {
        if (error.name !== "AbortError" && !document.hidden) {
            console.error("Live engine polling failed:", error);
            showOffline(error);
        }
    } finally {
        state.polling = false;
    }
}

function startLivePolling() {
    if (state.pollInterval !== null) {
        return;
    }
    state.nextPollAt = 0;
    state.pollInterval = window.setInterval(() => {
        pollLive();
    }, POLL_INTERVAL_MS);
    pollLive();
}

function stopPolling() {
    if (state.pollInterval !== null) {
        window.clearInterval(state.pollInterval);
        state.pollInterval = null;
    }
    if (state.replayInterval !== null) {
        state.replayElapsedBeforePause += performance.now() - state.replayStartedAt;
        window.clearInterval(state.replayInterval);
        state.replayInterval = null;
    }
    abortRequests();
    state.polling = false;
}

function formatBytes(bytes) {
    if (!Number.isFinite(Number(bytes))) {
        return "—";
    }
    const value = Number(bytes);
    if (value < 1024) {
        return `${value} B`;
    }
    if (value < 1024 * 1024) {
        return `${(value / 1024).toFixed(1)} KiB`;
    }
    return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
}

function formatUptime(seconds) {
    if (!Number.isFinite(Number(seconds))) {
        return "—";
    }
    const total = Math.floor(Number(seconds));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const remainder = total % 60;
    return hours > 0
        ? `${hours}h ${String(minutes).padStart(2, "0")}m`
        : `${minutes}m ${String(remainder).padStart(2, "0")}s`;
}

function updateLiveRibbon(sample) {
    const sha = sample.version.git_sha || "unknown";
    const uptime = formatUptime(sample.stats.uptime_seconds);
    const dbSize = formatBytes(sample.storage.file_size_bytes);
    setText("liveRibbon", `LIVE — build ${sha} — uptime ${uptime} — db size ${dbSize}`);
}

function statementCount(stats) {
    const values = Object.values(stats.statements || {});
    return values.reduce((total, value) => total + Number(value || 0), 0);
}

function histogramDelta(previous, current) {
    const oldBuckets = previous && Array.isArray(previous.buckets) ? previous.buckets : [];
    const newBuckets = Array.isArray(current && current.buckets) ? current.buckets : [];
    return newBuckets.map((value, index) => Math.max(0, Number(value) - Number(oldBuckets[index] || 0)));
}

function histogramQuantile(buckets, quantile) {
    const count = buckets.reduce((total, value) => total + value, 0);
    if (count === 0) {
        return null;
    }
    const rank = Math.ceil(count * quantile);
    let seen = 0;
    for (let index = 0; index < buckets.length; index += 1) {
        seen += buckets[index];
        if (seen >= rank) {
            return 2 ** index;
        }
    }
    return 2 ** (buckets.length - 1);
}

function formatMicros(value) {
    if (value === null || !Number.isFinite(value)) {
        return "—";
    }
    return value >= 1000 ? `${(value / 1000).toFixed(2)} ms` : `${value.toFixed(0)} µs`;
}

function updateTelemetry(stats, sampledAt) {
    const previous = state.previousStats;
    const elapsedSeconds = previous && state.previousSampleAt
        ? Math.max((sampledAt - state.previousSampleAt) / 1000, 0)
        : 0;
    if (!previous || elapsedSeconds <= 0) {
        setText("cacheHitRate", "—");
        setText("queryThroughput", "—");
        setText("latencyPercentiles", "—");
        setText("diskLatency", "—");
        setText("statementActivity", `${stats.active_statements ?? "—"} / ${stats.statement_waiters ?? "—"}`);
        state.previousStats = stats;
        state.previousSampleAt = sampledAt;
        return;
    }

    const hitDelta = Math.max(
        0,
        Number(stats.buffer_pool.hits) - Number(previous.buffer_pool.hits)
    );
    const missDelta = Math.max(
        0,
        Number(stats.buffer_pool.misses) - Number(previous.buffer_pool.misses)
    );
    const lookups = hitDelta + missDelta;
    setText("cacheHitRate", lookups === 0 ? "—" : `${((hitDelta / lookups) * 100).toFixed(1)}%`);

    const qps = Math.max(0, statementCount(stats) - statementCount(previous)) / elapsedSeconds;
    setText("queryThroughput", qps.toFixed(2));
    state.qpsHistory.push({ at: sampledAt, value: qps });
    state.qpsHistory = state.qpsHistory.filter((sample) => sampledAt - sample.at <= HISTORY_WINDOW_MS);
    renderSparkline();

    const currentLatency = stats.statement_duration_us || {};
    const previousLatency = previous.statement_duration_us || {};
    const latencyBuckets = new Array(32).fill(0);
    for (const name of Object.keys(currentLatency)) {
        const delta = histogramDelta(previousLatency[name], currentLatency[name]);
        delta.forEach((value, index) => {
            latencyBuckets[index] += value;
        });
    }
    const p50 = histogramQuantile(latencyBuckets, 0.5);
    const p95 = histogramQuantile(latencyBuckets, 0.95);
    const p99 = histogramQuantile(latencyBuckets, 0.99);
    setText("latencyPercentiles", p50 === null
        ? "—"
        : `${formatMicros(p50)} / ${formatMicros(p95)} / ${formatMicros(p99)}`);

    let latencySum = 0;
    let latencyCount = 0;
    for (const name of ["read_latency_us", "write_latency_us", "sync_latency_us"]) {
        const current = stats.disk[name];
        const old = previous.disk[name];
        latencySum += Math.max(0, Number(current.sum_us) - Number(old.sum_us));
        latencyCount += Math.max(0, Number(current.count) - Number(old.count));
    }
    setText("diskLatency", latencyCount === 0 ? "—" : formatMicros(latencySum / latencyCount));
    setText("statementActivity", `${stats.active_statements ?? "—"} / ${stats.statement_waiters ?? "—"}`);
    state.previousStats = stats;
    state.previousSampleAt = sampledAt;
}

function renderSparkline() {
    const path = byId("qpsSparklinePath");
    const samples = state.qpsHistory;
    if (samples.length === 0) {
        path.setAttribute("d", "");
        return;
    }
    const max = Math.max(...samples.map((sample) => sample.value), 0);
    const points = samples.map((sample, index) => {
        const x = samples.length === 1 ? 120 : (index / (samples.length - 1)) * 240;
        const y = max === 0 ? 40 : 40 - ((sample.value / max) * 36);
        return `${index === 0 ? "M" : "L"} ${x.toFixed(1)} ${y.toFixed(1)}`;
    });
    path.setAttribute("d", points.join(" "));
}

function clearTelemetry() {
    for (const id of ["cacheHitRate", "queryThroughput", "latencyPercentiles", "diskLatency", "statementActivity"]) {
        setText(id, "—");
    }
    byId("qpsSparklinePath").setAttribute("d", "");
}

function renderFrames(bufferPool) {
    const frames = Array.isArray(bufferPool.frames) ? bufferPool.frames : [];
    const signature = JSON.stringify(frames);
    setText("frameSummary", `${frames.length} of ${bufferPool.pool_size ?? "—"} frames · ${bufferPool.pinned_frames ?? "—"} pinned`);
    if (signature === state.frameSignature) {
        return;
    }
    state.frameSignature = signature;
    const heatmap = byId("bufferPoolHeatmap");
    clearChildren(heatmap);
    for (const frame of frames) {
        const cell = node("div", "frame");
        const isFree = Number(frame.page_id) < 0;
        const pinned = Number(frame.pin_count) > 0;
        if (isFree) {
            cell.classList.add("free");
        } else {
            if (pinned) {
                cell.classList.add("pinned");
            }
            if (frame.dirty) {
                cell.classList.add("dirty");
            }
            if (!pinned && !frame.dirty) {
                cell.classList.add("clean");
            }
        }
        const status = isFree ? "free" : pinned ? "pinned" : frame.dirty ? "dirty" : "clean";
        if (Number(frame.clock_hand) === Number(frame.frame_id)) {
            cell.classList.add("clock-hand");
        }
        const title = `Frame ${frame.frame_id}: ${status}; page ${isFree ? "free" : frame.page_id}; pins ${frame.pin_count}${frame.dirty ? "; dirty" : ""}${frame.ref_bit ? "; referenced" : ""}${Number(frame.clock_hand) === Number(frame.frame_id) ? "; clock hand" : ""}`;
        cell.title = title;
        cell.setAttribute("role", "listitem");
        cell.setAttribute("aria-label", title);
        heatmap.appendChild(cell);
    }
}

function clearBufferPool() {
    state.frameSignature = "";
    clearChildren(byId("bufferPoolHeatmap"));
    setText("frameSummary", "No frame snapshot available");
}

function formatCheckpoint(unixMs) {
    if (unixMs === null || unixMs === undefined || Number(unixMs) === 0) {
        return "Not recorded";
    }
    const date = new Date(Number(unixMs));
    return Number.isNaN(date.getTime()) ? "Unknown" : date.toLocaleString();
}

function renderStorage(storage) {
    setText("databasePages", storage.page_count ?? "—");
    setText("freePages", storage.free_pages ?? "—");
    setText("walSize", formatBytes(storage.wal_size_bytes));
    setText("lastCheckpoint", formatCheckpoint(storage.last_checkpoint_unix_ms));
    const recovery = storage.recovery || {};
    setText("recoverySummary", recovery.ran
        ? `Ran · ${recovery.records ?? 0} records · ${recovery.pages_replayed ?? 0} pages replayed · ${formatMicros(Number(recovery.duration_us ?? 0))}`
        : "No recovery run recorded");
}

function clearStorage() {
    for (const id of ["databasePages", "freePages", "walSize", "lastCheckpoint", "recoverySummary"]) {
        setText(id, "—");
    }
}

function renderCatalog(catalog) {
    const atlas = byId("schemaAtlas");
    const tables = Array.isArray(catalog) ? catalog : [];
    clearChildren(atlas);
    setText("schemaSummary", `${tables.length} catalog table${tables.length === 1 ? "" : "s"}`);
    byId("seedPanel").hidden = tables.length !== 0 || replayMode;
    if (tables.length === 0) {
        atlas.appendChild(node("p", "empty-state", "No tables are present in the live catalog."));
        byId("seedScript").value = SEED_SCRIPT;
        return;
    }

    for (const table of tables) {
        const card = node("article", "schema-table");
        card.setAttribute("aria-label", `Table ${table.name}`);
        const title = node("h3", "", table.name);
        const rowCount = node("span", "panel-meta", `${table.row_estimate ?? 0} rows`);
        title.appendChild(rowCount);
        card.appendChild(title);
        const columns = node("ul", "schema-columns");
        for (const column of table.schema || []) {
            const item = node("li", "schema-column");
            const name = node("span", "", column.name);
            const type = node("span", "column-type", column.type);
            item.append(name, type);
            if (column.primary_key || column.references) {
                const constraints = [];
                if (column.primary_key) {
                    constraints.push("PRIMARY KEY");
                }
                if (column.references) {
                    constraints.push(`FK → ${column.references.table}.${column.references.column}`);
                }
                item.appendChild(node("span", "column-constraint", constraints.join(" · ")));
            }
            columns.appendChild(item);
        }
        card.appendChild(columns);
        atlas.appendChild(card);
    }
}

function renderSample(sample, sampledAt) {
    if (!sample.stats || !sample.stats.buffer_pool || !sample.stats.disk ||
        !sample.buffer_pool || !sample.storage) {
        throw new Error("Engine sample is missing required telemetry fields.");
    }
    updateTelemetry(sample.stats, sampledAt);
    renderFrames(sample.buffer_pool);
    renderStorage(sample.storage);
    if (Array.isArray(sample.catalog)) {
        renderCatalog(sample.catalog);
    }
}

function addHighlightedSql(pre, sql, lineNumber, columnNumber) {
    const lines = sql.split("\n");
    const requestedLine = Number(lineNumber) - 1;
    const requestedColumn = Number(columnNumber) - 1;
    lines.forEach((line, index) => {
        if (index > 0) {
            pre.appendChild(document.createTextNode("\n"));
        }
        pre.appendChild(document.createTextNode(`${index + 1} | `));
        if (index !== requestedLine || requestedColumn < 0) {
            pre.appendChild(document.createTextNode(line));
            return;
        }
        const safeColumn = Math.min(requestedColumn, line.length);
        pre.appendChild(document.createTextNode(line.slice(0, safeColumn)));
        const marker = node("mark", "", line.slice(safeColumn, safeColumn + 1) || " ");
        pre.appendChild(marker);
        pre.appendChild(document.createTextNode(line.slice(safeColumn + 1)));
    });
}

function renderError(sql, error) {
    const target = byId("queryResult");
    clearChildren(target);
    const code = error && error.code ? String(error.code) : "REQUEST_ERROR";
    target.appendChild(node("p", "query-error", `${code}: ${error && error.message ? error.message : "The request failed."}`));
    const line = Number(error && error.line);
    const column = Number(error && error.column);
    if (line > 0 && column > 0) {
        const location = node("pre", "error-location");
        addHighlightedSql(location, sql, line, column);
        target.appendChild(location);
        target.appendChild(node("p", "query-error", `Highlighted position: line ${line}, column ${column}.`));
    }
}

function renderTable(columns, rows) {
    const wrapper = node("div", "result-table-wrap");
    const table = node("table", "result-table");
    table.id = "resultTable";
    const head = node("thead");
    const headerRow = node("tr");
    for (const column of columns) {
        const heading = node("th", "", column.name);
        heading.scope = "col";
        if (column.type) {
            heading.title = `${column.name} (${column.type})`;
        }
        headerRow.appendChild(heading);
    }
    head.appendChild(headerRow);
    table.appendChild(head);
    const body = node("tbody");
    for (const row of rows) {
        const tr = node("tr");
        for (const value of row) {
            tr.appendChild(node("td", "", value === null ? "NULL" : String(value)));
        }
        body.appendChild(tr);
    }
    table.appendChild(body);
    wrapper.appendChild(table);
    return wrapper;
}

function parseOperatorMetrics(plan) {
    const metricPattern = /^(.+?): rows=(\d+) wall_ms=([\d.]+) pages_fetched=(\d+) pages_missed=(\d+)$/;
    const metrics = [];
    const treeLines = [];
    let actualRows = null;
    for (const line of String(plan).split("\n")) {
        const trimmed = line.trim();
        if (trimmed.startsWith("Actual rows:")) {
            actualRows = trimmed.slice("Actual rows:".length).trim();
            continue;
        }
        const match = trimmed.match(metricPattern);
        if (match) {
            metrics.push({
                name: match[1],
                rows: match[2],
                elapsed: match[3],
                pages: match[4],
                misses: match[5]
            });
        } else if (trimmed && !trimmed.startsWith("Access path:")) {
            treeLines.push(line);
        }
    }
    return { metrics, treeLines, actualRows };
}

function appendPlanTree(parent, treeLines, metrics) {
    const rootList = node("ul", "plan-tree");
    const stack = [];
    for (const line of treeLines) {
        const indent = line.match(/^\s*/)[0].length;
        const label = line.trim();
        const item = node("li");
        const name = node("span", "plan-node-name", label);
        item.appendChild(name);
        while (stack.length > 0 && stack[stack.length - 1].indent >= indent) {
            stack.pop();
        }
        if (stack.length === 0) {
            rootList.appendChild(item);
        } else {
            let children = stack[stack.length - 1].item.querySelector(":scope > ul");
            if (!children) {
                children = node("ul");
                stack[stack.length - 1].item.appendChild(children);
            }
            children.appendChild(item);
        }
        stack.push({ indent, item, label });
        const matchingMetrics = metrics.filter((metric) => label.startsWith(metric.name));
        for (const metric of matchingMetrics) {
            item.appendChild(node(
                "span",
                "plan-node-details",
                `${metric.rows} rows · ${metric.elapsed} ms · ${metric.pages} pages · ${metric.misses} misses`
            ));
        }
    }
    if (rootList.childElementCount > 0) {
        parent.appendChild(rootList);
    }
}

function updatePipeline(plan) {
    const parsed = parseOperatorMetrics(plan);
    if (parsed.metrics.length === 0) {
        clearPipeline();
        return;
    }
    const operatorText = `${parsed.treeLines.join(" ")} ${parsed.metrics.map((metric) => metric.name).join(" ")}`;
    byId("pipe-parser").classList.toggle("active", true);
    byId("pipe-plan").classList.toggle("active", true);
    byId("pipe-scan").classList.toggle("active", /Scan|Lookup/.test(operatorText));
    byId("pipe-buffer").classList.toggle(
        "active",
        parsed.metrics.some((metric) => Number(metric.pages) > 0)
    );
    byId("pipe-disk").classList.toggle(
        "active",
        parsed.metrics.some((metric) => Number(metric.misses) > 0)
    );
}

function clearPipeline() {
    for (const id of ["pipe-parser", "pipe-plan", "pipe-scan", "pipe-buffer", "pipe-disk"]) {
        byId(id).classList.remove("active");
    }
}

function renderExplain(plan) {
    const target = byId("queryResult");
    const parsed = parseOperatorMetrics(plan);
    if (parsed.actualRows !== null) {
        target.appendChild(node("p", "result-meta", `Actual rows: ${parsed.actualRows}`));
    }
    appendPlanTree(target, parsed.treeLines, parsed.metrics);
    if (parsed.metrics.length > 0) {
        const table = node("table", "operator-table");
        const head = node("thead");
        const row = node("tr");
        for (const label of ["Operator", "Rows", "Time (ms)", "Pages", "Misses"]) {
            const th = node("th", "", label);
            th.scope = "col";
            row.appendChild(th);
        }
        head.appendChild(row);
        table.appendChild(head);
        const body = node("tbody");
        for (const metric of parsed.metrics) {
            const tr = node("tr");
            for (const value of [metric.name, metric.rows, metric.elapsed, metric.pages, metric.misses]) {
                tr.appendChild(node("td", "", value));
            }
            body.appendChild(tr);
        }
        table.appendChild(body);
        target.appendChild(table);
    }
    updatePipeline(plan);
}

function renderQueryResponse(sql, result, elapsedStatus = null) {
    const target = byId("queryResult");
    clearChildren(target);
    if (!result.ok) {
        renderError(sql, result.data && result.data.error ? result.data.error : {});
        return;
    }
    const data = result.data;
    const meta = node("div", "result-meta");
    meta.append(
        node("span", "", `${data.row_count ?? data.rows?.length ?? 0} rows`),
        node("span", "", `${data.rows_affected ?? 0} rows affected`),
        node("span", "", `${data.elapsed_us ?? "—"} µs`)
    );
    target.appendChild(meta);
    if (Array.isArray(data.columns) && Array.isArray(data.rows) && data.columns.length > 0) {
        target.appendChild(renderTable(data.columns, data.rows));
    } else if (Array.isArray(data.rows) && Array.isArray(data.columns)) {
        target.appendChild(node("p", "empty-state", "Statement completed with no result columns."));
    }
    if (data.statement === "EXPLAIN") {
        renderExplain(data.plan || "");
    } else {
        clearPipeline();
    }
    if (elapsedStatus) {
        setQueryStatus(elapsedStatus);
    }
}

async function runQuery(sql) {
    const query = sql.trim();
    if (!query) {
        setQueryStatus("Enter a SQL statement first.", true);
        return;
    }
    if (replayMode) {
        setQueryStatus("Queries are read-only in replay mode.", true);
        return;
    }
    const button = byId("runQueryButton");
    button.disabled = true;
    setQueryStatus("Sending SQL to the live engine…");
    try {
        const result = await requestJSON(`query-${query}`, "/api/v1/query", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ sql: query })
        });
        saveQueryHistory(query);
        renderQueryResponse(query, result);
        if (!result.ok) {
            setQueryStatus(`Engine returned ${result.status}.`, true);
        } else {
            setQueryStatus("Query completed on the live engine.");
            try {
                state.catalog = await fetchCatalog();
                state.catalogSampledAt = Date.now();
                renderCatalog(state.catalog);
            } catch (error) {
                console.error("Catalog refresh failed after query:", error);
                setQueryStatus(`Query completed; catalog refresh failed: ${error.message}`, true);
            }
            pollLive();
        }
    } catch (error) {
        if (error.name !== "AbortError") {
            console.error("SQL request failed:", error);
            renderError(query, { code: "REQUEST_ERROR", message: error.message });
            setQueryStatus(`Query request failed: ${error.message}`, true);
        }
    } finally {
        button.disabled = replayMode;
        byId("sqlInput").focus();
    }
}

function splitStatements(script) {
    const statements = [];
    let start = 0;
    let inString = false;
    for (let index = 0; index < script.length; index += 1) {
        if (script[index] === "'") {
            if (inString && script[index + 1] === "'") {
                index += 1;
            } else {
                inString = !inString;
            }
        } else if (script[index] === ";" && !inString) {
            const statement = script.slice(start, index).trim();
            if (statement) {
                statements.push(statement);
            }
            start = index + 1;
        }
    }
    const remaining = script.slice(start).trim();
    if (remaining) {
        statements.push(remaining);
    }
    return statements;
}

async function runSeedScript() {
    const button = byId("loadDemoButton");
    button.disabled = true;
    const statements = splitStatements(byId("seedScript").value);
    if (statements.length === 0) {
        setText("seedStatus", "The seed script is empty.");
        button.disabled = false;
        return;
    }
    try {
        for (let index = 0; index < statements.length; index += 1) {
            setText("seedStatus", `Running statement ${index + 1} of ${statements.length}…`);
            const result = await requestJSON(`seed-${index}-${statements[index]}`, "/api/v1/query", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ sql: statements[index] })
            });
            if (!result.ok) {
                const error = result.data && result.data.error ? result.data.error : {};
                throw new Error(`${error.code || result.status}: ${error.message || "Seed statement failed."}`);
            }
        }
        setText("seedStatus", `Completed ${statements.length} statements through /api/v1/query.`);
        state.catalog = await fetchCatalog();
        state.catalogSampledAt = Date.now();
        renderCatalog(state.catalog);
        pollLive();
    } catch (error) {
        console.error("Demo seed script failed:", error);
        setText("seedStatus", `Seed stopped: ${error.message}`);
    } finally {
        button.disabled = false;
    }
}

function renderReplayQuery(record) {
    const result = byId("queryResult");
    clearChildren(result);
    result.appendChild(node("p", "result-meta", `REPLAY · ${record.sql}`));
    renderQueryResponse(record.sql, {
        ok: Number(record.status) >= 200 && Number(record.status) < 300,
        status: record.status,
        data: record.response
    });
    setQueryStatus("Showing a recorded query response; no live query was sent.");
}

function replayTick() {
    const replay = state.replay;
    const elapsed = state.replayElapsedBeforePause + performance.now() - state.replayStartedAt;
    let changed = false;
    while (state.replaySampleIndex < replay.samples.length &&
        Number(replay.samples[state.replaySampleIndex].elapsed_ms) <= elapsed) {
        const sample = replay.samples[state.replaySampleIndex];
        renderSample(sample, Number(sample.elapsed_ms));
        state.replaySampleIndex += 1;
        changed = true;
    }
    while (state.replayQueryIndex < replay.queries.length &&
        Number(replay.queries[state.replayQueryIndex].elapsed_ms) <= elapsed) {
        renderReplayQuery(replay.queries[state.replayQueryIndex]);
        state.replayQueryIndex += 1;
        changed = true;
    }
    if (changed) {
        setText("pollStatus", "Playing recorded engine samples");
    }
    const lastSample = replay.samples[replay.samples.length - 1];
    const lastQuery = replay.queries[replay.queries.length - 1];
    const endAt = Math.max(Number(lastSample && lastSample.elapsed_ms || 0), Number(lastQuery && lastQuery.elapsed_ms || 0));
    if (elapsed > endAt && state.replaySampleIndex >= replay.samples.length &&
        state.replayQueryIndex >= replay.queries.length) {
        window.clearInterval(state.replayInterval);
        state.replayInterval = null;
        setText("pollStatus", "Replay complete");
    }
}

async function startReplay() {
    byId("liveRibbon").hidden = true;
    byId("replayRibbon").hidden = false;
    byId("runQueryButton").disabled = true;
    byId("sqlInput").readOnly = true;
    byId("seedPanel").hidden = true;
    try {
        const response = await fetch("replay/session.json", { headers: { Accept: "application/json" } });
        if (!response.ok) {
            throw new Error(`Replay recording unavailable (HTTP ${response.status}). Run scripts/record_session.py against a live engine.`);
        }
        const replay = await response.json();
        if (replay.source !== "real-engine-run" || !Array.isArray(replay.samples) || replay.samples.length === 0) {
            throw new Error("Replay file is not a valid recording from a real engine run.");
        }
        replay.queries = Array.isArray(replay.queries) ? replay.queries : [];
        state.replay = replay;
        setText("replayRibbon", `REPLAY — recorded ${replay.recorded_at} from a real engine run`);
        byId("offlineState").hidden = true;
        setOnlineStatus(true, "REPLAY");
        state.replayStartedAt = performance.now();
        state.replayElapsedBeforePause = 0;
        replayTick();
        state.replayInterval = window.setInterval(replayTick, 100);
    } catch (error) {
        console.error("Could not start engine replay:", error);
        byId("offlineState").hidden = false;
        setText("offlineTitle", "Replay recording unavailable");
        setText("offlineMessage", error.message);
        setOnlineStatus(false, "REPLAY UNAVAILABLE");
        setText("pollStatus", "No replay data loaded");
    }
}

function onHistoryKey(event) {
    const input = byId("sqlInput");
    if (event.key === "ArrowUp" && input.selectionStart === 0 && input.selectionEnd === 0) {
        if (state.queryHistory.length === 0) {
            return;
        }
        event.preventDefault();
        if (state.historyIndex === -1) {
            state.historyDraft = input.value;
            state.historyIndex = state.queryHistory.length;
        }
        state.historyIndex = Math.max(0, state.historyIndex - 1);
        input.value = state.queryHistory[state.historyIndex];
        input.setSelectionRange(0, 0);
    } else if (event.key === "ArrowDown" && state.historyIndex !== -1 &&
        (input.selectionStart === 0 || input.selectionStart === input.value.length)) {
        event.preventDefault();
        state.historyIndex += 1;
        if (state.historyIndex >= state.queryHistory.length) {
            state.historyIndex = -1;
            input.value = state.historyDraft;
        } else {
            input.value = state.queryHistory[state.historyIndex];
        }
        input.setSelectionRange(input.value.length, input.value.length);
    }
    if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
        event.preventDefault();
        runQuery(input.value);
    }
}

function init() {
    byId("runQueryButton").addEventListener("click", () => runQuery(byId("sqlInput").value));
    byId("sqlInput").addEventListener("keydown", onHistoryKey);
    byId("retryButton").addEventListener("click", () => {
        state.failures = 0;
        state.nextPollAt = 0;
        pollLive();
    });
    byId("loadDemoButton").addEventListener("click", runSeedScript);
    document.addEventListener("visibilitychange", () => {
        if (document.hidden) {
            stopPolling();
            setText("pollStatus", "Paused while tab is hidden");
        } else if (replayMode) {
            if (state.replay && state.replayInterval === null) {
                state.replayStartedAt = performance.now();
                state.replayInterval = window.setInterval(replayTick, 100);
            }
        } else {
            startLivePolling();
        }
    });

    if (replayMode) {
        startReplay();
    } else {
        startLivePolling();
    }
}

document.addEventListener("DOMContentLoaded", init);
