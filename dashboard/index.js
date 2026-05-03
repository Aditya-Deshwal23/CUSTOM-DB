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
        cacheHitRate: 0,
        queryThroughput: 0,
        ioLatency: 0,
        activeLatches: 0
    },
    isSimulationMode: true,
    clockHand: 0,
    pinnedFrames: new Set(),
    isExecuting: false
};

let liveMetricsInterval = null;
let simulationInterval = null;

async function toggleMode() {
    state.isSimulationMode = !state.isSimulationMode;
    const btn = document.getElementById('modeToggleBtn');
    const dot = document.getElementById('statusDot');
    const text = document.getElementById('statusText');
    const badge = document.getElementById('mainStatusBadge');

    if (state.isSimulationMode) {
        // Switch to Simulation Mode
        if (liveMetricsInterval) clearInterval(liveMetricsInterval);
        
        btn.textContent = "SWITCH TO LIVE ENGINE";
        text.textContent = "SIMULATION MODE";
        dot.className = "status-dot simulation";
        badge.className = "status-badge simulation";
        
        simulationInterval = setInterval(runTelemetrySimulation, CONFIG.FETCH_INTERVAL);
        runTelemetrySimulation();
        renderMetrics();
    } else {
        // Switch to Live Engine
        if (simulationInterval) clearInterval(simulationInterval);
        
        btn.textContent = "SWITCH TO SIMULATION";
        text.textContent = "LIVE ENGINE CONNECTED";
        dot.className = "status-dot online";
        badge.className = "status-badge online";
        
        // Initial fetch to verify connection
        try {
            await updateTelemetry(); // This will handle failsafe if it throws
            liveMetricsInterval = setInterval(updateTelemetry, CONFIG.FETCH_INTERVAL);
        } catch (e) {
            // Failsafe is handled inside updateTelemetry or we can catch here
            console.error("Failed to connect to Live Engine, reverting...");
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

const EXECUTION_STEPS = [
    { delay: 0, trace: "[0ms] Parsing query using zero-copy <span class='hl-keyword'>std::string_view</span>..." },
    { delay: 300, trace: "[300ms] B+ Tree Traversal: Acquiring shared_mutex via <span class='hl-latch'>Latch Crabbing</span>..." },
    { delay: 600, trace: "[600ms] Buffer Pool: Cache Miss. <span class='hl-replacer'>Clock Replacer</span> evicting Frame ${frame}..." },
    { delay: 1000, trace: "[1000ms] Disk Manager: Executing <span class='hl-keyword'>fsync()</span> for Write-Ahead Log..." },
    { delay: 1300, trace: "[1300ms] QUERY SUCCESS: 1 row affected." }
];

async function runSQLSimulation(query) {
    if (state.isExecuting) return;
    state.isExecuting = true;
    
    const isWrite = /INSERT|UPDATE|DELETE/i.test(query);
    const isCacheMiss = Math.random() > 0.7;
    
    const sqlInput = document.getElementById('sqlInput');
    sqlInput.disabled = true;
    const tbody = document.getElementById('queryLogBody');
    const timestamp = new Date().toLocaleTimeString('en-GB', { hour12: false });
    const evictFrame = Math.floor(Math.random() * CONFIG.TOTAL_FRAMES);
    const row = document.createElement('tr');
    row.className = 'new-row';
    row.innerHTML = `<td class="timestamp-cell">${timestamp}</td><td class="command-cell">${query.toUpperCase()}</td><td class="execution-trace-cell"></td><td class="latency-cell">--</td>`;
    tbody.prepend(row);
    if (tbody.children.length > 50) tbody.lastElementChild.remove();
    const traceCell = row.querySelector('.execution-trace-cell');
    const latencyCell = row.querySelector('.latency-cell');
    const tablesInQuery = CONFIG.TABLES.filter(t => query.toUpperCase().includes(t.id.toUpperCase()));

    // If not in simulation mode, actually hit the backend
    let backendResult = null;
    if (!state.isSimulationMode) {
        fetch(API_BASE_URL + '/api/query', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ query: query })
        })
        .then(res => res.json())
        .then(data => { backendResult = data.result; })
        .catch(err => console.error("Query failed:", err));
    }

    for (let i = 0; i < EXECUTION_STEPS.length; i++) {
        if (i === 3 && !isWrite) continue;
        await new Promise(resolve => setTimeout(resolve, i === 0 ? 0 : 400));
        
        let msg = EXECUTION_STEPS[i].trace.replace('${frame}', evictFrame);
        if (i === 2 && !isCacheMiss) {
            msg = "[600ms] Buffer Pool: <span class='hl-keyword'>Cache Hit</span>. Directly accessing Page on Frame ${frame}...".replace('${frame}', evictFrame);
        }
        
        // Pipeline Synchronization
        if (i === 0) activatePipelineNode('pipe-parser');
        if (i === 1) activatePipelineNode('pipe-index');
        if (i === 2) {
            activatePipelineNode('pipe-buffer');
            simulateBufferPoolActivity();
        }
        if (i === 3) activatePipelineNode('pipe-disk');

        // At the last step, show real backend result if available
        if (i === EXECUTION_STEPS.length - 1 && backendResult) {
            msg = `[1300ms] ENGINE RESPONSE: ${backendResult}`;
        }

        const line = document.createElement('div');
        line.innerHTML = msg;
        traceCell.appendChild(line);
        
        if (i === 1 && tablesInQuery.length >= 2) {
            animatePulse(tablesInQuery[0].id, tablesInQuery[1].id, 'active');
        } else if (i === 1 && tablesInQuery.length === 1) {
            pulseNode(tablesInQuery[0].id);
        }
        
        latencyCell.textContent = `${(i * 300 + Math.random() * 50).toFixed(0)}ms`;
    }
    state.isExecuting = false;
    sqlInput.disabled = false;
    sqlInput.focus();
}

async function updateTelemetry() {
    if (state.isExecuting) return;
    
    try {
        const response = await fetch(API_BASE_URL + CONFIG.BACKEND_URL);
        if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
        
        const data = await response.json();
        
        state.metrics.cacheHitRate = data.cache_hit_rate;
        state.metrics.queryThroughput = data.query_throughput;
        state.metrics.ioLatency = data.avg_io_latency;
        state.metrics.activeLatches = data.active_latches;
        
        renderMetrics();
    } catch (error) {
        console.error("Backend connection failed: ", error);
        if (!state.isSimulationMode) {
            alert("Live Engine disconnected or unreachable. Reverting to Simulation Mode.");
            toggleMode(); // Revert to simulation
        }
        throw error; // Rethrow for toggleMode catch block
    }
}

function runTelemetrySimulation() {
    state.metrics.cacheHitRate = 88 + Math.random() * 7;
    state.metrics.queryThroughput = 45 + Math.random() * 50;
    state.metrics.ioLatency = 3 + Math.random() * 10;
    state.metrics.activeLatches = Math.floor(Math.random() * 4);
    renderMetrics();
}

function renderMetrics() {
    const ids = ['cacheHitRate', 'queryThroughput', 'ioLatency', 'activeLatches'];
    ids.forEach(id => {
        const el = document.getElementById(id);
        if (!el) return;
        if (id === 'cacheHitRate') el.textContent = `${state.metrics[id].toFixed(2)}%`;
        else if (id === 'queryThroughput') el.textContent = state.metrics[id].toFixed(1);
        else if (id === 'ioLatency') el.textContent = `${state.metrics[id].toFixed(1)}ms`;
        else el.textContent = state.metrics[id];
    });
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
    const numPins = 3 + Math.floor(Math.random() * 3);
    const affected = [];
    for (let i = 0; i < numPins; i++) {
        const idx = Math.floor(Math.random() * CONFIG.TOTAL_FRAMES);
        state.pinnedFrames.add(idx);
        affected.push(idx);
        updateFrameUI(idx);
    }
    const pinCountEl = document.getElementById('pinCount');
    if (pinCountEl) pinCountEl.textContent = `${state.pinnedFrames.size}/50 frames pinned`;
    setTimeout(() => {
        affected.forEach(idx => {
            state.pinnedFrames.delete(idx);
            updateFrameUI(idx);
        });
        if (pinCountEl) pinCountEl.textContent = `${state.pinnedFrames.size}/50 frames pinned`;
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

    // Start in Simulation Mode
    state.isSimulationMode = false; // Set to false so toggleMode() switches it to true
    toggleMode();
});
