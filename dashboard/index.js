/**
 * Database Observability Console - Immersive Engine Simulation & Topology
 */

const API_BASE_URL = window.location.port === '5500' || window.location.port === '5501' ? 'http://127.0.0.1:8080' : '';

const CONFIG = {
    FETCH_INTERVAL: 1000,
    BACKEND_URL: '/api/metrics',
    TOTAL_FRAMES: 50,
    TABLES: [
        { id: 'Users', x: 100, y: 100, cols: ['id (PK)', 'username', 'email', 'created_at'] },
        { id: 'Orders', x: 400, y: 100, cols: ['id (PK)', 'user_id (FK)', 'total_amount', 'status'] },
        { id: 'Products', x: 700, y: 100, cols: ['id (PK)', 'sku', 'price', 'stock_qty'] },
        { id: 'OrderItems', x: 400, y: 300, cols: ['id (PK)', 'order_id (FK)', 'product_id (FK)', 'qty'] }
    ],
    RELATIONS: [
        { from: 'Users', to: 'Orders' },
        { from: 'Orders', to: 'OrderItems' },
        { from: 'Products', to: 'OrderItems' }
    ]
};

const state = {
    metrics: {
        cacheHitRate: null,
        queryThroughput: null,
        avgLatency: null,
        errorCount: null
    },
    isSimulationMode: true,
    clockHand: 0,
    pinnedFrames: new Set(),
    nextSimulationFrame: 0,
    isExecuting: false
};

let liveMetricsInterval = null;
let simulationInterval = null;

function renderMode() {
    const btn = document.getElementById('modeToggleBtn');
    const dot = document.getElementById('statusDot');
    const text = document.getElementById('statusText');
    const badge = document.getElementById('mainStatusBadge');
    const ribbon = document.getElementById('simulationRibbon');

    ribbon.hidden = !state.isSimulationMode;
    if (state.isSimulationMode) {
        btn.textContent = "SWITCH TO LIVE ENGINE";
        text.textContent = "SIMULATION MODE";
        dot.className = "status-dot simulation";
        badge.className = "status-badge simulation";
    } else {
        btn.textContent = "SWITCH TO SIMULATION";
        text.textContent = "LIVE ENGINE CONNECTED";
        dot.className = "status-dot online";
        badge.className = "status-badge online";
    }
}

async function toggleMode() {
    state.isSimulationMode = !state.isSimulationMode;
    if (state.isSimulationMode) {
        if (liveMetricsInterval) clearInterval(liveMetricsInterval);
        liveMetricsInterval = null;
        renderMode();
        runTelemetrySimulation();
        simulationInterval = setInterval(runTelemetrySimulation, CONFIG.FETCH_INTERVAL);
    } else {
        if (simulationInterval) clearInterval(simulationInterval);
        simulationInterval = null;
        renderMode();
        try {
            await updateTelemetry();
            liveMetricsInterval = setInterval(() => {
                updateTelemetry().catch((error) => {
                    console.error("Live metrics request failed:", error);
                    clearInterval(liveMetricsInterval);
                    liveMetricsInterval = null;
                    state.isSimulationMode = true;
                    renderMode();
                    runTelemetrySimulation();
                });
            }, CONFIG.FETCH_INTERVAL);
        } catch (error) {
            console.error("Failed to connect to the live engine:", error);
            state.isSimulationMode = true;
            renderMode();
            runTelemetrySimulation();
            simulationInterval = setInterval(runTelemetrySimulation, CONFIG.FETCH_INTERVAL);
        }
    }
}

function initPanZoom() {
    // Completely disable all scroll-to-zoom and drag-to-pan as per Step 1
    const container = document.getElementById('topologyContainer');
    if (!container) return;

    container.style.cursor = 'default';
    container.addEventListener('wheel', (e) => {
        // Allow default scroll behavior (global scrolling)
    }, { passive: true });

    container.addEventListener('mousedown', (e) => {
        // Prevent pan behavior
    });
}

function updateViewport() {
    const viewport = document.getElementById('viewport');
    if (viewport) viewport.setAttribute('transform', `translate(60, 40) scale(0.85)`);
}

function activatePipelineNode(id) {
    const el = document.getElementById(id);
    if (el) {
        el.classList.add('active');
        setTimeout(() => el.classList.remove('active'), 1200);
    }
}

function initTopology() {
    const viewport = document.getElementById('viewport');
    if (!viewport) return;
    viewport.innerHTML = '';

    const COLORS = {
        Users: '#3b82f6',
        Orders: '#10b981',
        Products: '#f59e0b',
        OrderItems: '#ef4444'
    };

    // Draw Organic Bezier Links
    CONFIG.RELATIONS.forEach((rel) => {
        const from = CONFIG.TABLES.find(t => t.id === rel.from);
        const to = CONFIG.TABLES.find(t => t.id === rel.to);
        const path = document.createElementNS("http://www.w3.org/2000/svg", "path");

        // Calculate curve points for smooth organic flow (Bezier)
        const startX = from.x + 80;
        const startY = from.y;
        const endX = to.x - 80;
        const endY = to.y;

        // Midpoint-based control points for elegant curves
        const midX = (startX + endX) / 2;
        const d = `M ${startX} ${startY} C ${midX} ${startY}, ${midX} ${endY}, ${endX} ${endY}`;

        path.setAttribute("d", d);
        path.setAttribute("class", "topology-link");
        path.setAttribute("id", `link-${from.id}-${to.id}`);
        viewport.appendChild(path);
    });

    // Draw Professional ERD Table Nodes
    CONFIG.TABLES.forEach(table => {
        const g = document.createElementNS("http://www.w3.org/2000/svg", "g");
        g.setAttribute("class", "table-node-group");
        g.setAttribute("id", `node-${table.id}`);
        g.setAttribute("transform", `translate(${table.x}, ${table.y})`);

        const w = 170;
        const headerH = 34;
        const rowH = 22;
        const h = headerH + (table.cols.length * rowH) + 12;

        // Main Table Body (Professional ERD look)
        const rect = document.createElementNS("http://www.w3.org/2000/svg", "rect");
        rect.setAttribute("x", -w/2); rect.setAttribute("y", -h/2);
        rect.setAttribute("width", w); rect.setAttribute("height", h);
        rect.setAttribute("class", "table-node");
        rect.setAttribute("rx", 6);

        // Distinct Colored Header Bar
        const header = document.createElementNS("http://www.w3.org/2000/svg", "rect");
        header.setAttribute("x", -w/2); header.setAttribute("y", -h/2);
        header.setAttribute("width", w); header.setAttribute("height", headerH);
        header.setAttribute("class", "table-header");
        header.style.fill = COLORS[table.id] || 'var(--primary)';
        header.style.stroke = COLORS[table.id] || 'var(--primary)';
        header.setAttribute("rx", 6);

        // Table Title (e.g., USERS)
        const title = document.createElementNS("http://www.w3.org/2000/svg", "text");
        title.setAttribute("y", -h/2 + 22);
        title.setAttribute("text-anchor", "middle");
        title.setAttribute("class", "table-text");
        title.textContent = table.id.toUpperCase();

        g.appendChild(rect);
        g.appendChild(header);
        g.appendChild(title);

        // Column List with PK/FK Indicators
        table.cols.forEach((col, idx) => {
            const t = document.createElementNS("http://www.w3.org/2000/svg", "text");
            t.setAttribute("x", -w/2 + 16);
            t.setAttribute("y", -h/2 + headerH + 20 + (idx * rowH));
            t.setAttribute("class", "column-text");

            if (col.includes('(PK)')) {
                t.style.fill = 'var(--primary)';
                t.style.fontWeight = 'bold';
            } else if (col.includes('(FK)')) {
                t.style.fill = 'var(--warning)';
            }

            t.textContent = `> ${col}`;
            g.appendChild(t);
        });

        viewport.appendChild(g);
    });
    updateViewport();
}


function animatePulse(sourceId, targetId, mode = 'active') {
    const originalPath = document.getElementById(`link-${sourceId}-${targetId}`) || document.getElementById(`link-${targetId}-${sourceId}`);
    if (!originalPath) return;
    const viewport = document.getElementById('viewport');
    if (!viewport) return;
    const pulse = document.createElementNS("http://www.w3.org/2000/svg", "path");
    pulse.setAttribute("d", originalPath.getAttribute("d"));
    pulse.setAttribute("class", `pulse-path pulse-${mode}`);
    const length = originalPath.getTotalLength();
    pulse.style.strokeDasharray = mode === 'active' ? `50, ${length}` : `0, ${length}`;
    viewport.appendChild(pulse);

    setTimeout(() => {
        pulse.remove();
        if (mode === 'active') {
            pulseNode(targetId);
            pulseNode(sourceId);
        }
    }, 800);
}

function pulseNode(tableId) {
    const node = document.querySelector(`#node-${tableId} .table-node`);
    if (node) {
        node.classList.add('table-hit');
        setTimeout(() => node.classList.remove('table-hit'), 600);
    }
}

const ILLUSTRATIVE_TRACE = [
    "Illustrative trace: no engine request was sent.",
    "The parser, index, buffer, and schema visuals are examples only.",
    "Switch to Live Engine to submit this command to /api/query."
];

async function runSQLSimulation(query) {
    if (state.isExecuting) return;
    state.isExecuting = true;
    const sqlInput = document.getElementById('sqlInput');
    sqlInput.disabled = true;
    const tbody = document.getElementById('queryLogBody');
    const timestamp = new Date().toLocaleTimeString('en-GB', { hour12: false });
    const row = document.createElement('tr');
    row.className = 'new-row';
    const timestampCell = document.createElement('td');
    timestampCell.className = 'timestamp-cell';
    timestampCell.textContent = timestamp;
    const commandCell = document.createElement('td');
    commandCell.className = 'command-cell';
    commandCell.textContent = query.toUpperCase();
    const traceCell = document.createElement('td');
    traceCell.className = 'execution-trace-cell';
    const latencyCell = document.createElement('td');
    latencyCell.className = 'latency-cell';
    latencyCell.textContent = '—';
    row.append(timestampCell, commandCell, traceCell, latencyCell);
    tbody.prepend(row);
    if (tbody.children.length > 50) tbody.lastElementChild.remove();

    try {
        if (state.isSimulationMode) {
            for (const message of ILLUSTRATIVE_TRACE) {
                const line = document.createElement('div');
                line.textContent = message;
                traceCell.appendChild(line);
            }
            simulateBufferPoolActivity();
        } else {
            const response = await fetch(API_BASE_URL + '/api/query', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ query })
            });
            const data = await response.json();
            if (!response.ok) {
                throw new Error(`HTTP error! status: ${response.status}`);
            }
            traceCell.textContent = data.result;
        }
    } catch (error) {
        console.error("Query request failed:", error);
        traceCell.textContent = `Request failed: ${error.message}`;
    } finally {
        state.isExecuting = false;
        sqlInput.disabled = false;
        sqlInput.focus();
    }
}

async function updateTelemetry() {
    const response = await fetch(API_BASE_URL + CONFIG.BACKEND_URL);
    if (!response.ok) {
        throw new Error(`HTTP error! status: ${response.status}`);
    }
    const data = await response.json();
    if (data.source === 'uninstrumented') {
        runTelemetrySimulation();
        return;
    }
    if (data.source !== 'engine') {
        throw new Error('Metrics response has an unknown source.');
    }

    state.metrics.cacheHitRate = data.cache_hit_rate;
    state.metrics.queryThroughput = data.query_throughput;
    state.metrics.avgLatency = data.avg_latency;
    state.metrics.errorCount = data.error_count;
    renderMetrics();
}

function runTelemetrySimulation() {
    state.metrics.cacheHitRate = null;
    state.metrics.queryThroughput = null;
    state.metrics.avgLatency = null;
    state.metrics.errorCount = null;
    renderMetrics();
}

function renderMetrics() {
    const { cacheHitRate, queryThroughput, avgLatency, errorCount } = state.metrics;
    document.getElementById('cacheHitRate').textContent =
        cacheHitRate === null ? '—' : `${cacheHitRate.toFixed(2)}%`;
    document.getElementById('queryThroughput').textContent =
        queryThroughput === null ? '—' : queryThroughput.toFixed(1);
    document.getElementById('avgLatency').textContent =
        avgLatency === null ? '—' : `${avgLatency.toFixed(1)}ms`;
    document.getElementById('errorCount').textContent =
        errorCount === null ? '—' : errorCount;
}

function initBufferPool() {
    const heatmap = document.getElementById('bufferPoolHeatmap');
    if (!heatmap) return;
    heatmap.innerHTML = '';
    for (let i = 0; i < CONFIG.TOTAL_FRAMES; i++) {
        const frame = document.createElement('div');
        frame.className = 'frame';
        frame.id = `frame-${i}`;
        heatmap.appendChild(frame);
    }
}

function updateFrameUI(idx) {
    const el = document.getElementById(`frame-${idx}`);
    if (!el) return;
    el.className = 'frame';
    if (state.pinnedFrames.has(idx)) el.classList.add('pinned');
    if (state.clockHand === idx) el.classList.add('clock-hand');
}

function simulateBufferPoolActivity() {
    const affected = [];
    for (let i = 0; i < 4; i++) {
        const idx = (state.nextSimulationFrame + i) % CONFIG.TOTAL_FRAMES;
        state.pinnedFrames.add(idx);
        affected.push(idx);
        updateFrameUI(idx);
    }
    state.nextSimulationFrame = (state.nextSimulationFrame + affected.length) % CONFIG.TOTAL_FRAMES;
    setTimeout(() => {
        affected.forEach(idx => {
            state.pinnedFrames.delete(idx);
            updateFrameUI(idx);
        });
    }, 1500);
}

document.addEventListener('DOMContentLoaded', () => {
    initBufferPool();
    initTopology();
    initPanZoom();

    const sqlInput = document.getElementById('sqlInput');
    if (sqlInput) {
        sqlInput.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' && sqlInput.value.trim() && !state.isExecuting) {
                runSQLSimulation(sqlInput.value.trim());
                sqlInput.value = '';
            }
        });
    }

    const modeToggleBtn = document.getElementById('modeToggleBtn');
    if (modeToggleBtn) {
        modeToggleBtn.addEventListener('click', toggleMode);
    }

    renderMode();
    runTelemetrySimulation();
});
