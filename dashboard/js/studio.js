import { createVirtualGrid } from "./grid.js";
import { filtersToJson, rowsUrl } from "./filter.js";
import { layoutRelations } from "./er-layout.js";
import { toCsv, toInsert, toJson, toMarkdown, toTsv } from "./exporters.js";

const state = {
    tables: [],
    table: null,
    subtab: "data",
    readOnly: null,
    rows: [],
    columns: [],
    nextCursor: null,
    pageIndex: 0,
    pageCursors: [null],
    pageSize: 100,
    filters: [],
    order: "",
    descending: false,
    showRid: true,
    visibleColumns: new Set(),
    grid: null,
    edits: new Map(),
    busy: false,
    erZoom: 1,
    erOffset: { x: 0, y: 0 },
    contextTable: null,
    rowError: null
};

const byId = (id) => document.getElementById(id);
const make = (tag, className = "", text = "") => {
    const element = document.createElement(tag);
    if (className) element.className = className;
    if (text !== "") element.textContent = text;
    return element;
};
const clear = (element) => element.replaceChildren();

async function getJson(url, options = {}) {
    const response = await fetch(url, {
        headers: { Accept: "application/json", ...(options.headers || {}) },
        ...options
    });
    if (response.status === 304) return { status: 304, data: null, headers: response.headers };
    let data;
    try {
        data = await response.json();
    } catch (error) {
        throw new Error(`Invalid JSON from ${url}: ${error.message}`);
    }
    if (!response.ok) {
        const detail = data?.error?.message || data?.message || `HTTP ${response.status}`;
        const failure = new Error(detail);
        failure.status = response.status;
        failure.data = data;
        throw failure;
    }
    return { status: response.status, data, headers: response.headers };
}

function setStatus(message, isError = false) {
    const status = byId("studioStatus");
    status.textContent = message;
    status.classList.toggle("error", isError);
}

function displayError(error, operation) {
    const status = error.status ? ` (${error.status})` : "";
    setStatus(`${operation} failed${status}: ${error.message}`, true);
}

function routeTo(hash) {
    window.location.hash = hash;
}

function tableByName(name) {
    return state.tables.find((table) => table.name === name) || null;
}

function safeIdentifier(value) {
    return /^[A-Za-z_][A-Za-z0-9_]*$/.test(value);
}

function tableRoute(name, subtab = "data") {
    routeTo(`#/table/${encodeURIComponent(name)}/${subtab}`);
}

function filteredTables() {
    const query = byId("explorerFilter").value.trim().toLocaleLowerCase();
    if (!query) return state.tables;
    return state.tables.filter((table) => {
        const contents = [
            table.name,
            ...(table.columns || []).map((column) =>
                `${column.name} ${column.type} ${column.references?.table || ""}`)
        ].join(" ").toLocaleLowerCase();
        return contents.includes(query);
    });
}

function appendHighlighted(target, value, query) {
    const text = String(value);
    if (!query) {
        target.textContent = text;
        return;
    }
    const lower = text.toLocaleLowerCase();
    let offset = 0;
    let match = lower.indexOf(query.toLocaleLowerCase());
    while (match !== -1) {
        target.append(document.createTextNode(text.slice(offset, match)));
        const mark = make("mark", "", text.slice(match, match + query.length));
        target.append(mark);
        offset = match + query.length;
        match = lower.indexOf(query.toLocaleLowerCase(), offset);
    }
    target.append(document.createTextNode(text.slice(offset)));
}

function menuButton(label, action, disabled = false, reason = "") {
    const button = make("button", "context-item", label);
    button.type = "button";
    button.setAttribute("role", "menuitem");
    button.disabled = disabled;
    if (reason) button.title = reason;
    button.addEventListener("click", () => {
        hideContextMenu();
        action();
    });
    return button;
}

function hideContextMenu() {
    const menu = byId("studioContextMenu");
    menu.hidden = true;
    menu.replaceChildren();
}

async function copyText(value, label) {
    try {
        await navigator.clipboard.writeText(value);
        setStatus(`${label} copied.`);
    } catch (error) {
        displayError(error, "Copy");
    }
}

function insertTemplate(table) {
    const names = (table.columns || []).map((column) => column.name);
    if (!safeIdentifier(table.name) || names.some((name) => !safeIdentifier(name))) {
        return `-- Template unavailable: table or column name requires quoting.\n`;
    }
    return `INSERT INTO ${table.name} (${names.join(", ")}) VALUES (${names.map(() => "?").join(", ")});`;
}

function showContextMenu(event, table) {
    event.preventDefault();
    state.contextTable = table;
    const menu = byId("studioContextMenu");
    clear(menu);
    menu.append(
        menuButton("Open data", () => tableRoute(table.name, "data")),
        menuButton("Structure", () => tableRoute(table.name, "structure")),
        menuButton("DDL", () => tableRoute(table.name, "ddl")),
        menuButton("Select top 100", () => openTopQuery(table)),
        menuButton("Copy name", () => copyText(table.name, "Table name")),
        menuButton("Copy CREATE TABLE", () => copyText(table.ddl, "CREATE TABLE")),
        menuButton("Generate INSERT template", () =>
            copyText(insertTemplate(table), "INSERT template")),
        menuButton("Refresh", () => window.dispatchEvent(new Event("custom-db:refresh-catalog"))),
        menuButton("Drop table…", () => confirmDrop(table),
            state.readOnly !== false || !safeIdentifier(table.name),
            state.readOnly === true ? "Disabled because the engine is read-only." :
                state.readOnly === null ? "Waiting for read-only mode to be confirmed." :
                    "This table name cannot be safely used by the SQL API.")
    );
    menu.hidden = false;
    menu.style.left = `${Math.max(8, Math.min(event.clientX, window.innerWidth - 240))}px`;
    menu.style.top = `${Math.max(8, Math.min(event.clientY, window.innerHeight - 360))}px`;
    menu.querySelector("button:not(:disabled)")?.focus();
}

function renderTree() {
    const tree = byId("explorerTree");
    clear(tree);
    const visible = filteredTables();
    const query = byId("explorerFilter").value.trim();
    const root = make("div", "tree-root");
    root.setAttribute("role", "navigation");
    root.setAttribute("aria-label", "Database objects");
    const database = make("div", "tree-section-label", "custom-db");
    root.appendChild(database);
    const tablesBranch = make("details", "tree-branch");
    tablesBranch.open = true;
    const summary = make("summary", "tree-summary", `Tables (${state.tables.length})`);
    tablesBranch.appendChild(summary);
    const list = make("div", "tree-table-list");
    list.tabIndex = 0;
    const rowHeight = 30;
    const maxVisible = 28;
    const renderRows = () => {
        const start = visible.length > maxVisible
            ? Math.max(0, Math.floor(list.scrollTop / rowHeight) - 4)
            : 0;
        const end = Math.min(visible.length, start + maxVisible + 8);
        const fragment = document.createDocumentFragment();
        if (start > 0) {
            const spacer = make("div", "tree-spacer");
            spacer.style.height = `${start * rowHeight}px`;
            fragment.appendChild(spacer);
        }
        for (let index = start; index < end; index += 1) {
            const table = visible[index];
            const tableDetails = make("details", "tree-table");
            const tableSummary = make("summary", "tree-table-summary");
            tableSummary.tabIndex = -1;
            tableSummary.dataset.table = table.name;
            const name = make("span", "tree-object-name");
            appendHighlighted(name, table.name, query);
            const estimate = make("span", "tree-estimate",
                `~${Number(table.row_estimate || 0).toLocaleString()}`);
            estimate.title = "Estimated rows";
            tableSummary.append(name, estimate);
            tableSummary.addEventListener("click", (event) => {
                if (event.detail === 2) tableRoute(table.name, "data");
            });
            tableSummary.addEventListener("contextmenu", (event) =>
                showContextMenu(event, table));
            tableDetails.appendChild(tableSummary);
            const children = make("div", "tree-object-children");
            const columnsHeading = make("div", "tree-group-label", "Columns");
            children.appendChild(columnsHeading);
            for (const column of table.columns || []) {
                const item = make("button", "tree-column");
                item.type = "button";
                const icon = column.pk ? "🔑" : column.references ? "↗" : "•";
                const title = make("span", "tree-column-name");
                appendHighlighted(title, `${icon} ${column.name}`, query);
                item.append(title, make("span", "tree-type", column.type));
                item.addEventListener("click", () => tableRoute(table.name, "structure"));
                children.appendChild(item);
            }
            if (table.indexes?.length) {
                children.appendChild(make("div", "tree-group-label", "Indexes"));
                for (const index of table.indexes) {
                    children.appendChild(make("div", "tree-index",
                        `${index.unique ? "◆" : "◇"} ${index.name} · ${index.columns.join(", ")}`));
                }
            }
            const constraints = [];
            for (const column of table.columns || []) {
                if (column.pk) constraints.push(`PK ${column.name}`);
                if (column.references) {
                    constraints.push(`FK ${column.name} → ${column.references.table}.${column.references.column}`);
                }
                if (!column.nullable) constraints.push(`NOT NULL ${column.name}`);
            }
            if (constraints.length) {
                children.appendChild(make("div", "tree-group-label", "Constraints"));
                for (const constraint of constraints) {
                    children.appendChild(make("div", "tree-constraint", constraint));
                }
            }
            tableDetails.appendChild(children);
            fragment.appendChild(tableDetails);
        }
        if (end < visible.length) {
            const spacer = make("div", "tree-spacer");
            spacer.style.height = `${(visible.length - end) * rowHeight}px`;
            fragment.appendChild(spacer);
        }
        list.replaceChildren(fragment);
    };
    list.addEventListener("scroll", renderRows, { passive: true });
    list.addEventListener("keydown", (event) => {
        const selected = event.target.closest("[data-table]");
        if (event.key === "Enter" || event.key === " ") {
            if (selected) {
                event.preventDefault();
                tableRoute(selected.dataset.table, "data");
            }
        } else if (event.key === "ArrowRight" && selected) {
            event.preventDefault();
            selected.parentElement.open = true;
        } else if (event.key === "ArrowLeft" && selected) {
            event.preventDefault();
            selected.parentElement.open = false;
        } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
            const items = [...list.querySelectorAll("[data-table]")];
            const current = items.indexOf(selected);
            const delta = event.key === "ArrowDown" ? 1 : -1;
            const next = items[Math.max(0, Math.min(items.length - 1, current + delta))];
            if (next) {
                event.preventDefault();
                next.focus();
            } else {
                list.focus();
            }
        }
    });
    tablesBranch.appendChild(list);
    root.appendChild(tablesBranch);
    const system = make("details", "tree-branch");
    const systemSummary = make("summary", "tree-summary", "System");
    system.appendChild(systemSummary);
    const systemLinks = make("div", "tree-object-children");
    for (const [label, target] of [
        ["Catalog", "catalog"], ["Free list", "freelist"], ["WAL", "wal"]
    ]) {
        const link = make("button", "tree-link", label);
        link.type = "button";
        link.addEventListener("click", () => routeTo(`#/system/${target}`));
        systemLinks.appendChild(link);
    }
    system.appendChild(systemLinks);
    root.appendChild(system);
    tree.appendChild(root);
    renderRows();
    list.scrollTop = 0;
    tree.setAttribute("aria-busy", "false");
}

function currentRoute() {
    const parts = window.location.hash.replace(/^#\/?/, "").split("/");
    try {
        if (parts[0] === "table") {
            return { kind: "table", name: decodeURIComponent(parts[1] || ""),
                subtab: parts[2] || "data" };
        }
    } catch (error) {
        return { kind: "error", message: `Invalid table route: ${error.message}` };
    }
    if (parts[0] === "er") return { kind: "er" };
    if (parts[0] === "system") {
        return { kind: "system", view: parts[1] || "catalog", page: parts[2], slot: parts[3] };
    }
    return { kind: "welcome" };
}

function renderTabHeader(table, subtab) {
    const view = byId("studioView");
    clear(view);
    const header = make("div", "studio-view-header");
    header.append(
        make("div", "studio-breadcrumb", `custom-db / Tables / ${table.name}`),
        make("h2", "", table.name),
        make("span", "row-estimate-label", `~${Number(table.row_estimate || 0).toLocaleString()} rows`)
    );
    const tabs = make("nav", "table-subtabs");
    tabs.setAttribute("aria-label", `${table.name} views`);
    for (const tab of ["data", "structure", "indexes", "constraints", "ddl", "storage", "relations"]) {
        const label = tab === "ddl" ? "DDL" : tab[0].toUpperCase() + tab.slice(1);
        const button = make("button", "subtab", label);
        button.type = "button";
        button.setAttribute("aria-current", tab === subtab ? "page" : "false");
        button.addEventListener("click", () => tableRoute(table.name, tab));
        tabs.appendChild(button);
    }
    view.append(header, tabs);
    return view;
}

function renderGridToolbar(view) {
    const toolbar = make("div", "grid-toolbar");
    const pageSize = make("select", "control");
    pageSize.setAttribute("aria-label", "Rows per page");
    for (const size of [50, 100, 500]) {
        const option = make("option", "", String(size));
        option.value = String(size);
        option.selected = state.pageSize === size;
        pageSize.appendChild(option);
    }
    pageSize.addEventListener("change", () => {
        state.pageSize = Number(pageSize.value);
        resetAndLoadRows();
    });
    const order = make("select", "control");
    order.setAttribute("aria-label", "Sort rows by column");
    const noSort = make("option", "", "Default order");
    noSort.value = "";
    order.appendChild(noSort);
    for (const column of state.columns) {
        const option = make("option", "", column.name);
        option.value = column.name;
        option.selected = state.order === column.name;
        order.appendChild(option);
    }
    order.addEventListener("change", () => {
        state.order = order.value;
        resetAndLoadRows();
    });
    const direction = make("button", "button button-secondary",
        state.descending ? "Descending" : "Ascending");
    direction.type = "button";
    direction.setAttribute("aria-pressed", String(state.descending));
    direction.addEventListener("click", () => {
        state.descending = !state.descending;
        renderTableView();
    });
    const ridLabel = make("label", "inline-control");
    const rid = make("input");
    rid.type = "checkbox";
    rid.checked = state.showRid;
    rid.addEventListener("change", () => {
        state.showRid = rid.checked;
        renderRowsGrid();
    });
    ridLabel.append(rid, document.createTextNode("RID"));

    const visibility = make("details", "column-visibility");
    const visibilitySummary = make("summary", "", "Columns");
    visibility.appendChild(visibilitySummary);
    const visibilityList = make("div", "column-visibility-list");
    for (const column of state.columns) {
        const label = make("label", "inline-control");
        const input = make("input");
        input.type = "checkbox";
        input.checked = state.visibleColumns.has(column.name);
        input.addEventListener("change", () => {
            if (input.checked) state.visibleColumns.add(column.name);
            else state.visibleColumns.delete(column.name);
            renderRowsGrid();
        });
        label.append(input, document.createTextNode(column.name));
        visibilityList.appendChild(label);
    }
    visibility.appendChild(visibilityList);

    toolbar.append(make("span", "toolbar-label", "Page size"), pageSize,
        make("span", "toolbar-label", "Order"), order, direction, ridLabel, visibility);
    view.appendChild(toolbar);
    return toolbar;
}

function renderFilterBuilder(view) {
    const container = make("section", "filter-builder");
    const heading = make("div", "filter-heading");
    heading.append(make("strong", "", "Filters"),
        make("span", "muted", "Structured predicates; values are sent as JSON, not SQL."));
    const rows = make("div", "filter-rows");
    const addRow = (existing = {}) => {
        const row = make("div", "filter-row");
        const column = make("select", "control");
        column.setAttribute("aria-label", "Filter column");
        for (const item of state.columns) {
            const option = make("option", "", item.name);
            option.value = item.name;
            option.selected = item.name === existing.column;
            column.appendChild(option);
        }
        const operator = make("select", "control");
        operator.setAttribute("aria-label", "Filter operator");
        for (const value of ["=", "<>", "<", "<=", ">", ">=", "LIKE", "ISNULL"]) {
            const option = make("option", "", value);
            option.value = value;
            option.selected = value === existing.op;
            operator.appendChild(option);
        }
        const input = make("input", "control filter-value");
        input.setAttribute("aria-label", "Filter value");
        input.value = existing.value ?? "";
        input.hidden = operator.value === "ISNULL";
        operator.addEventListener("change", () => {
            input.hidden = operator.value === "ISNULL";
        });
        const remove = make("button", "icon-button", "Remove");
        remove.type = "button";
        remove.addEventListener("click", () => row.remove());
        row.append(column, operator, input, remove);
        rows.appendChild(row);
    };
    for (const filter of state.filters) addRow(filter);
    const controls = make("div", "filter-actions");
    const add = make("button", "button button-secondary", "Add filter");
    add.type = "button";
    add.addEventListener("click", () => addRow());
    const apply = make("button", "button button-primary", "Apply filters");
    apply.type = "button";
    apply.addEventListener("click", () => {
        try {
            state.filters = [...rows.querySelectorAll(".filter-row")].map((row) => ({
                column: row.children[0].value,
                op: row.children[1].value,
                value: row.children[2].value
            })).filter((filter) => filter.op === "ISNULL" || filter.value !== "");
            state.filters = filtersToJson(state.filters, state.columns);
            resetAndLoadRows();
        } catch (error) {
            setStatus(`Filter is invalid: ${error.message}`, true);
        }
    });
    controls.append(add, apply);
    container.append(heading, rows, controls);
    view.appendChild(container);
}

function currentEdits() {
    return [...state.edits.values()];
}

function sqlForEdit(edit) {
    const table = state.table;
    const primaryKey = table?.columns?.find((column) => column.pk);
    if (!primaryKey || !safeIdentifier(table.name) ||
        !safeIdentifier(edit.column.name) || !safeIdentifier(primaryKey.name)) {
        throw new TypeError("This table does not have identifiers supported by the SQL editor.");
    }
    return {
        sql: `UPDATE ${table.name} SET ${edit.column.name} = ? WHERE ${primaryKey.name} = ?`,
        params: [edit.value, edit.row.values[primaryKey.name]]
    };
}

function renderChangeset(view) {
    const edits = currentEdits();
    if (!edits.length) return;
    const bar = make("div", "changeset-bar");
    bar.append(make("strong", "", `${edits.length} change${edits.length === 1 ? "" : "s"}`));
    const review = make("button", "button button-secondary", "Review SQL");
    review.type = "button";
    review.addEventListener("click", () => showReviewDialog(edits));
    const apply = make("button", "button button-primary", state.busy ? "Applying…" : "Apply");
    apply.type = "button";
    apply.disabled = state.busy;
    apply.addEventListener("click", applyChanges);
    const discard = make("button", "button button-secondary", "Discard");
    discard.type = "button";
    discard.disabled = state.busy;
    discard.addEventListener("click", () => {
        state.edits.clear();
        state.rowError = null;
        renderTableView();
    });
    bar.append(review, apply, discard);
    view.appendChild(bar);
}

function showReviewDialog(edits = currentEdits()) {
    const dialog = byId("studioDialog");
    clear(dialog);
    const heading = make("h2", "", "Review parameterized SQL");
    const list = make("ol", "review-list");
    for (const edit of edits) {
        const item = make("li");
        const statement = sqlForEdit(edit);
        const code = make("code", "", statement.sql);
        const params = make("pre", "review-params", JSON.stringify(statement.params));
        item.append(code, params);
        list.appendChild(item);
    }
    const close = make("button", "button button-secondary", "Close");
    close.type = "button";
    close.addEventListener("click", () => dialog.close());
    dialog.append(heading, list, close);
    dialog.showModal();
}

async function applyChanges() {
    const edits = currentEdits();
    if (!edits.length || state.busy) return;
    let statements;
    try {
        statements = edits.map(sqlForEdit);
    } catch (error) {
        setStatus(error.message, true);
        return;
    }
    state.busy = true;
    renderTableView();
    try {
        await getJson("/api/v1/batch", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ statements })
        });
        state.edits.clear();
        state.rowError = null;
        state.busy = false;
        setStatus(`${statements.length} row update${statements.length === 1 ? "" : "s"} committed atomically.`);
        await loadRows();
        window.dispatchEvent(new Event("custom-db:refresh-catalog"));
    } catch (error) {
        state.busy = false;
        const index = Number(error.data?.error?.statement_index);
        state.rowError = Number.isInteger(index) ? edits[index] || null : edits[0] || null;
        setStatus(`Changeset rolled back; ${error.message}`, true);
        renderTableView();
    } finally {
        state.busy = false;
    }
}

function editCell({ row, column, cell }) {
    if (state.readOnly !== false || !state.table?.columns?.some((item) => item.pk)) return;
    const schemaColumn = state.table.columns.find((item) => item.name === column.name);
    const currentValue = row.values[column.name];
    const editor = schemaColumn.type === "BOOLEAN" ? make("select", "cell-editor") :
        make("input", "cell-editor");
    if (schemaColumn.type === "BOOLEAN") {
        if (schemaColumn.nullable) {
            const option = make("option", "", "NULL");
            option.value = "null";
            option.selected = currentValue === null;
            editor.appendChild(option);
        }
        for (const value of ["true", "false"]) {
            const option = make("option", "", value);
            option.value = value;
            option.selected = String(currentValue) === value;
            editor.appendChild(option);
        }
    } else {
        editor.type = schemaColumn.type === "VARCHAR" ? "text" : "number";
        editor.value = currentValue ?? "";
        if (schemaColumn.type === "INT") editor.step = "1";
        if (schemaColumn.type === "DOUBLE") editor.step = "any";
        if (schemaColumn.type === "VARCHAR") {
            editor.maxLength = schemaColumn.length;
            editor.setAttribute("aria-label", `${column.name}, maximum ${schemaColumn.length} characters`);
        }
    }
    let finished = false;
    const finish = (save) => {
        if (finished) return;
        if (!save) {
            finished = true;
            renderRowsGrid();
            return;
        }
        let value = editor.value;
        if (schemaColumn.type === "INT") {
            if (value === "" && schemaColumn.nullable) {
                value = null;
            } else {
                value = Number(value);
            }
            if (value !== null && !Number.isSafeInteger(value)) {
                setStatus("INT values must be safe whole numbers.", true);
                editor.focus();
                return;
            }
        } else if (schemaColumn.type === "DOUBLE") {
            if (value === "" && schemaColumn.nullable) {
                value = null;
            } else {
                value = Number(value);
            }
            if (value !== null && !Number.isFinite(value)) {
                setStatus("DOUBLE values must be finite numbers.", true);
                editor.focus();
                return;
            }
        } else if (schemaColumn.type === "BOOLEAN") {
            value = value === "null" ? null : value === "true";
        }
        if (value === currentValue) {
            finished = true;
            renderRowsGrid();
            return;
        }
        finished = true;
        const key = `${row.rid}:${column.name}`;
        state.edits.set(key, {
            rid: row.rid, row, column: schemaColumn, value, oldValue: currentValue
        });
        renderTableView();
    };
    editor.addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
            event.preventDefault();
            finish(true);
        } else if (event.key === "Escape") {
            event.preventDefault();
            finish(false);
        }
    });
    editor.addEventListener("blur", () => finish(true), { once: true });
    cell.replaceChildren(editor);
    editor.focus();
    editor.select?.();
}

function renderRowsGrid() {
    const container = byId("tableGrid");
    if (!container) return;
    const columns = state.columns.filter((column) => state.visibleColumns.has(column.name));
    state.grid = createVirtualGrid(container, columns, state.rows, {
        label: `${state.table?.name || "Table"} rows`,
        showRid: state.showRid,
        onCellDoubleClick: editCell,
        onRidClick: (rid) => {
            const [page, slot] = rid.split(":");
            routeTo(`#/system/page/${page}/${slot}`);
        },
        getCellClass: (row, column) => {
            const classes = [];
            if (state.edits.has(`${row.rid}:${column.name}`)) classes.push("edited-cell");
            if (state.rowError?.rid === row.rid &&
                state.rowError?.column?.name === column.name) classes.push("error-cell");
            return classes.join(" ");
        },
        getDisplayValue: (row, column, original) => {
            const edit = state.edits.get(`${row.rid}:${column.name}`);
            return edit ? edit.value : original;
        },
        onSort: (column) => {
            state.order = column.name;
            state.descending = !state.descending;
            resetAndLoadRows();
        }
    });
}

function renderExportActions(view) {
    const actions = make("div", "export-actions");
    const copy = make("button", "button button-secondary", "Copy row");
    copy.type = "button";
    copy.addEventListener("click", async () => {
        const row = state.grid?.activeRow();
        if (!row) return setStatus("No rows on this page to copy.", true);
        await copyText(toTsv(state.columns, [row.values]), "Row");
    });
    const copySelection = make("button", "button button-secondary", "Copy selection");
    copySelection.type = "button";
    copySelection.addEventListener("click", async () => {
        const selection = state.grid?.selectedRows();
        if (!selection?.rows.length || !selection.columns.length) {
            return setStatus("Select cells in the grid first.", true);
        }
        await copyText(toTsv(selection.columns, selection.rows), "Selection");
    });
    const format = make("select", "control");
    format.setAttribute("aria-label", "Copy format");
    for (const value of ["CSV", "JSON", "SQL INSERT", "Markdown"]) {
        const option = make("option", "", value);
        option.value = value;
        format.appendChild(option);
    }
    const copyAs = make("button", "button button-secondary", "Copy as");
    copyAs.type = "button";
    copyAs.addEventListener("click", async () => {
        const rows = state.rows.map((row) => row.values);
        const exporters = {
            CSV: () => toCsv(state.columns, rows),
            JSON: () => toJson(state.columns, rows),
            "SQL INSERT": () => toInsert(state.table.name, state.columns, rows),
            Markdown: () => toMarkdown(state.columns, rows)
        };
        try {
            await copyText(exporters[format.value](), format.value);
        } catch (error) {
            displayError(error, "Export");
        }
    });
    const exportCurrent = make("button", "button button-secondary", "Export page");
    exportCurrent.type = "button";
    exportCurrent.addEventListener("click", () => exportPage(format.value));
    const exportAll = make("button", "button button-secondary", "Export all (max 5,000)");
    exportAll.type = "button";
    exportAll.addEventListener("click", () => exportAllRows(format.value));
    actions.append(copy, copySelection, format, copyAs, exportCurrent, exportAll);
    view.appendChild(actions);
}

async function exportPage(format) {
    const rows = state.rows.map((row) => row.values);
    try {
        const output = format === "CSV" ? toCsv(state.columns, rows) :
            format === "JSON" ? toJson(state.columns, rows) :
                format === "SQL INSERT" ? toInsert(state.table.name, state.columns, rows) :
                    toMarkdown(state.columns, rows);
        downloadText(`${state.table.name}.${format.toLowerCase().replaceAll(" ", "-")}.txt`, output);
    } catch (error) {
        displayError(error, "Export");
    }
}

function downloadText(filename, content) {
    const url = URL.createObjectURL(new Blob([content], { type: "text/plain;charset=utf-8" }));
    const link = make("a");
    link.href = url;
    link.download = filename;
    link.click();
    URL.revokeObjectURL(url);
}

async function exportAllRows(format) {
    const dialog = byId("studioDialog");
    clear(dialog);
    const title = make("h2", "", "Export table");
    const progress = make("p", "", "Preparing export…");
    const cancel = make("button", "button button-secondary", "Cancel");
    cancel.type = "button";
    const controller = new AbortController();
    cancel.addEventListener("click", () => {
        controller.abort();
        dialog.close();
    });
    dialog.addEventListener("cancel", () => controller.abort(), { once: true });
    dialog.append(title, progress, cancel);
    dialog.showModal();
    const allRows = [];
    let cursor = null;
    try {
        do {
            const url = rowsUrl(state.table.name, {
                limit: 500, after: cursor, order: state.order || undefined,
                descending: state.descending, filters: state.filters
            });
            const response = await getJson(url, { signal: controller.signal });
            allRows.push(...response.data.rows.map((row) => row.values));
            cursor = response.data.next_cursor;
            progress.textContent = `Fetched ${allRows.length} rows (maximum 5,000).`;
            if (allRows.length >= 5000 && cursor) {
                throw new Error("Export limit of 5,000 rows reached; refine the filter.");
            }
        } while (cursor);
        const exporters = {
            CSV: () => toCsv(state.columns, allRows),
            JSON: () => toJson(state.columns, allRows),
            "SQL INSERT": () => toInsert(state.table.name, state.columns, allRows),
            Markdown: () => toMarkdown(state.columns, allRows)
        };
        const output = exporters[format]();
        const extension = format.toLowerCase().replaceAll(" ", "-");
        downloadText(`${state.table.name}.${extension}`, output);
        setStatus(`Exported ${allRows.length} rows as ${format}.`);
        dialog.close();
    } catch (error) {
        if (error.name !== "AbortError") {
            progress.textContent = `Export failed: ${error.message}`;
            setStatus(`Export failed: ${error.message}`, true);
        }
    }
}

function renderPager(view) {
    const pager = make("div", "table-pager");
    const previous = make("button", "button button-secondary", "Previous");
    previous.type = "button";
    previous.disabled = state.pageIndex === 0;
    previous.addEventListener("click", () => {
        if (state.pageIndex > 0) {
            state.pageIndex -= 1;
            loadRows();
        }
    });
    const next = make("button", "button button-secondary", "Next");
    next.type = "button";
    next.disabled = !state.nextCursor;
    next.addEventListener("click", () => {
        if (!state.nextCursor) return;
        state.pageCursors[state.pageIndex + 1] = state.nextCursor;
        state.pageIndex += 1;
        loadRows();
    });
    const first = state.pageIndex * state.pageSize + (state.rows.length ? 1 : 0);
    const last = state.pageIndex * state.pageSize + state.rows.length;
    const estimate = Number(state.table?.row_estimate || 0);
    pager.append(previous,
        make("span", "pager-label", `Rows ${first}–${last} of ~${estimate.toLocaleString()} (estimate)`),
        next);
    view.appendChild(pager);
}

async function loadRows() {
    if (!state.table || state.busy) return;
    const container = byId("tableGrid");
    if (container) {
        container.setAttribute("aria-busy", "true");
        container.replaceChildren(make("div", "skeleton-grid", "Loading rows…"));
    }
    try {
        const filters = state.filters.length ? state.filters :
            filtersToJson([], state.columns);
        const cursor = state.pageCursors[state.pageIndex] || null;
        const url = rowsUrl(state.table.name, {
            limit: state.pageSize,
            after: cursor,
            order: state.order || undefined,
            descending: state.descending,
            filters
        });
        const response = await getJson(url);
        state.rows = response.data.rows;
        state.nextCursor = response.data.next_cursor;
        state.columns = response.data.columns.map((column) => ({
            ...column,
            length: state.table.columns.find((item) => item.name === column.name)?.length
        }));
        if (!state.visibleColumns.size) {
            state.visibleColumns = new Set(state.columns.map((column) => column.name));
        }
        renderTableView();
        setStatus(`Loaded ${state.rows.length} rows from ${state.table.name}.`);
    } catch (error) {
        if (container) {
            container.replaceChildren(make("p", "plain-error",
                `Request failed (${error.status || "network"}); ${error.message}`));
        }
        displayError(error, "Load rows");
    }
}

function resetAndLoadRows() {
    state.pageIndex = 0;
    state.pageCursors = [null];
    state.nextCursor = null;
    loadRows();
}

function renderRowsPage(view) {
    renderGridToolbar(view);
    renderFilterBuilder(view);
    const gridHost = make("div", "table-grid-host");
    gridHost.id = "tableGrid";
    gridHost.setAttribute("aria-busy", "false");
    view.appendChild(gridHost);
    renderRowsGrid();
    renderExportActions(view);
    renderPager(view);
    if (state.readOnly === true) {
        const note = make("p", "read-only-note",
            "Read-only mode: cell editing is unavailable.");
        note.title = "The engine reports READ_ONLY=1.";
        view.appendChild(note);
    } else if (!state.table.columns.some((column) => column.pk)) {
        view.appendChild(make("p", "read-only-note",
            "Editing is unavailable because this table has no primary key."));
    } else if (state.readOnly === null) {
        view.appendChild(make("p", "read-only-note",
            "Editing is unavailable until engine mode is confirmed."));
    }
    renderChangeset(view);
}

function renderStructure(view) {
    const table = make("table", "metadata-table");
    table.innerHTML = "";
    const head = make("thead");
    const header = make("tr");
    for (const label of ["#", "Name", "Type", "Nullable", "Key", "References", "Indexes"]) {
        header.appendChild(make("th", "", label));
    }
    head.appendChild(header);
    const body = make("tbody");
    state.table.columns.forEach((column, index) => {
        const row = make("tr");
        row.append(make("td", "", String(index + 1)),
            make("td", "mono", column.name),
            make("td", "", column.type === "VARCHAR" ? `${column.type}(${column.length})` : column.type),
            make("td", "", column.nullable ? "Yes" : "No"),
            make("td", "", column.pk ? "PK" : ""));
        const reference = make("td");
        if (column.references) {
            const link = make("button", "text-link",
                `${column.references.table}.${column.references.column}`);
            link.type = "button";
            link.addEventListener("click", () =>
                tableRoute(column.references.table, "data"));
            reference.appendChild(link);
        } else {
            reference.textContent = "—";
        }
        const indexes = (state.table.indexes || [])
            .filter((item) => item.columns.includes(column.name))
            .map((item) => item.name).join(", ");
        row.append(reference, make("td", "", indexes || "—"));
        body.appendChild(row);
    });
    table.append(head, body);
    view.appendChild(table);
}

function renderIndexes(view) {
    const table = make("table", "metadata-table");
    const head = make("thead");
    const row = make("tr");
    for (const label of ["Name", "Columns", "Unique", "Kind", "Height", "Entries", "Leaf pages", "Fill %", "Tree"]) {
        row.appendChild(make("th", "", label));
    }
    head.appendChild(row);
    const body = make("tbody");
    for (const index of state.table.indexes || []) {
        const tr = make("tr");
        tr.append(make("td", "mono", index.name),
            make("td", "", index.columns.join(", ")),
            make("td", "", index.unique ? "Yes" : "No"),
            make("td", "", index.kind),
            make("td", "numeric-cell", String(index.height)),
            make("td", "numeric-cell", String(index.entries)),
            make("td", "numeric-cell", String(index.leaf_pages)),
            make("td", "numeric-cell", `${Number(index.fill_factor || 0).toFixed(1)}%`));
        const open = make("button", "text-link", "Open B+ tree");
        open.type = "button";
        open.addEventListener("click", () => renderIndexTree(index.name));
        const treeCell = make("td");
        treeCell.appendChild(open);
        tr.appendChild(treeCell);
        body.appendChild(tr);
    }
    table.append(head, body);
    view.appendChild(table);
    if (!(state.table.indexes || []).length) {
        view.appendChild(make("p", "empty-state", "No indexes are defined for this table."));
    }
}

function renderConstraints(view) {
    const list = make("ul", "constraint-list");
    for (const column of state.table.columns) {
        if (column.pk) list.appendChild(make("li", "", `PRIMARY KEY (${column.name})`));
        if (column.references) {
            const item = make("li");
            item.append(document.createTextNode(`FOREIGN KEY (${column.name}) REFERENCES `));
            const link = make("button", "text-link",
                `${column.references.table}(${column.references.column})`);
            link.type = "button";
            link.addEventListener("click", () => tableRoute(column.references.table, "data"));
            item.append(link, document.createTextNode(" ON DELETE RESTRICT"));
            list.appendChild(item);
        }
        if (!column.nullable) list.appendChild(make("li", "", `NOT NULL (${column.name})`));
    }
    for (const index of state.table.indexes || []) {
        if (index.unique && !state.table.columns.some((column) =>
            column.pk && index.columns.includes(column.name))) {
            list.appendChild(make("li", "", `UNIQUE (${index.columns.join(", ")})`));
        }
    }
    view.appendChild(list);
}

function renderDdl(view) {
    const actions = make("div", "export-actions");
    const copy = make("button", "button button-secondary", "Copy DDL");
    copy.type = "button";
    copy.addEventListener("click", () => copyText(state.table.ddl, "DDL"));
    const editor = make("button", "button button-primary", "Open in editor");
    editor.type = "button";
    editor.addEventListener("click", () => openQuery(state.table.ddl));
    actions.append(copy, editor);
    const pre = make("pre", "ddl-block");
    pre.textContent = state.table.ddl;
    view.append(actions, pre);
}

function renderStorage(view) {
    const heap = state.table.heap || {};
    const info = make("dl", "storage-details");
    for (const [label, value] of [
        ["Heap pages", heap.pages],
        ["Bytes used", heap.bytes_used],
        ["Bytes free", heap.bytes_free],
        ["Tuple estimate", heap.tuples_estimate],
        ["Average tuple storage bytes · estimate", heap.tuples_estimate
            ? Math.round(Number(heap.bytes_used || 0) / Number(heap.tuples_estimate))
            : null]
    ]) {
        const entry = make("div");
        entry.append(make("dt", "", label),
            make("dd", "mono", value === null || value === undefined
                ? "—" : Number(value).toLocaleString()));
        info.appendChild(entry);
    }
    view.appendChild(info);
    view.appendChild(make("h3", "", "Heap free space by page"));
    const histogram = heap.free_space_histogram || [];
    const totalPages = histogram.reduce((total, bucket) => total + Number(bucket.pages || 0), 0);
    if (!totalPages) {
        view.appendChild(make("p", "empty-state", "No data pages in this heap."));
    } else {
        for (const bucket of histogram) {
            const count = Number(bucket.pages || 0);
            const line = make("div", "storage-bar-row");
            line.appendChild(make("span", "", bucket.range));
            const track = make("div", "storage-bar-track");
            const fill = make("div", "storage-bar-fill");
            fill.style.width = `${(count / totalPages) * 100}%`;
            track.appendChild(fill);
            line.append(track, make("span", "mono", `${count} pages`));
            view.appendChild(line);
        }
    }
    const title = make("h3", "", "Index sizes");
    view.appendChild(title);
    for (const index of state.table.indexes || []) {
        const bar = make("div", "storage-bar-row");
        const pages = Math.max(0, Number(index.pages || 0));
        const scale = Math.max(1, ...state.table.indexes.map((item) => Number(item.pages || 0)));
        bar.append(make("span", "", index.name));
        const track = make("div", "storage-bar-track");
        const fill = make("div", "storage-bar-fill");
        fill.style.width = `${(pages / scale) * 100}%`;
        track.appendChild(fill);
        bar.append(track, make("span", "mono", `${pages} pages`));
        view.appendChild(bar);
    }
}

function renderRelations(view) {
    const related = new Set([state.table.name]);
    for (const column of state.table.columns) {
        if (column.references) related.add(column.references.table);
    }
    for (const table of state.tables) {
        if ((table.columns || []).some((column) =>
            column.references?.table === state.table.name)) related.add(table.name);
    }
    renderEr(view, state.tables.filter((table) => related.has(table.name)), state.table.name);
}

async function renderIndexTree(name) {
    const panel = byId("indexTreePanel");
    if (!panel) return;
    clear(panel);
    panel.appendChild(make("h3", "", `B+ tree · ${name}`));
    try {
        const response = await getJson(`/api/v1/inspect/indexes/${encodeURIComponent(name)}/tree?max_nodes=200`);
        const pre = make("pre", "json-view");
        pre.textContent = JSON.stringify(response.data, null, 2);
        panel.appendChild(pre);
    } catch (error) {
        panel.appendChild(make("p", "plain-error", error.message));
    }
}

function renderTableView() {
    const table = state.table;
    if (!table) {
        renderWelcome();
        return;
    }
    const view = renderTabHeader(table, state.subtab);
    if (state.subtab === "data") {
        renderRowsPage(view);
    } else if (state.subtab === "structure") {
        renderStructure(view);
    } else if (state.subtab === "indexes") {
        renderIndexes(view);
        const panel = make("section", "index-tree-panel");
        panel.id = "indexTreePanel";
        view.appendChild(panel);
    } else if (state.subtab === "constraints") {
        renderConstraints(view);
    } else if (state.subtab === "ddl") {
        renderDdl(view);
    } else if (state.subtab === "storage") {
        renderStorage(view);
    } else if (state.subtab === "relations") {
        renderRelations(view);
    } else {
        view.appendChild(make("p", "plain-error", `Unknown table view: ${state.subtab}`));
    }
}

function svgElement(name, attrs = {}) {
    const element = document.createElementNS("http://www.w3.org/2000/svg", name);
    for (const [key, value] of Object.entries(attrs)) {
        element.setAttribute(key, String(value));
    }
    return element;
}

function renderEr(container, tables, focused = "") {
    const { nodes, edges } = layoutRelations(tables);
    const maxX = Math.max(760, ...nodes.map((node) => node.x + node.width + 30));
    const maxY = Math.max(460, ...nodes.map((node) => node.y + node.height + 30));
    const wrapper = make("div", "er-canvas");
    const svg = svgElement("svg", {
        viewBox: `0 0 ${maxX} ${maxY}`,
        role: "group",
        "aria-label": "Entity relationship diagram"
    });
    const defs = svgElement("defs");
    const marker = svgElement("marker", {
        id: "er-many-marker", markerWidth: 10, markerHeight: 10,
        refX: 9, refY: 5, orient: "auto", markerUnits: "strokeWidth"
    });
    marker.appendChild(svgElement("path", { d: "M 0 0 L 9 5 L 0 10", class: "er-crow" }));
    defs.appendChild(marker);
    const oneMarker = svgElement("marker", {
        id: "er-one-marker", markerWidth: 8, markerHeight: 10,
        refX: 4, refY: 5, orient: "auto", markerUnits: "strokeWidth"
    });
    oneMarker.appendChild(svgElement("path", { d: "M 4 0 V 10", class: "er-crow" }));
    defs.appendChild(oneMarker);
    svg.appendChild(defs);
    const viewport = svgElement("g", { class: "er-viewport" });
    viewport.setAttribute("transform",
        `translate(${state.erOffset.x} ${state.erOffset.y}) scale(${state.erZoom})`);
    for (const edge of edges) {
        const path = svgElement("path", {
            d: edge.path, class: "er-edge", "data-source": edge.source,
            "data-target": edge.target, "marker-start": "url(#er-many-marker)",
            "marker-end": "url(#er-one-marker)"
        });
        path.addEventListener("mouseenter", () => path.classList.add("highlight"));
        path.addEventListener("mouseleave", () => path.classList.remove("highlight"));
        viewport.appendChild(path);
    }
    for (const node of nodes) {
        const accessibleName = node.name + node.columns.map((column) =>
            `${column.pk ? "◆ " : column.references ? "↗ " : ""}${column.name}  ${column.type}`
        ).join("");
        const group = svgElement("g", {
            class: `er-table${node.name === focused ? " focused" : ""}`,
            transform: `translate(${node.x} ${node.y})`,
            tabindex: 0,
            role: "link",
            "aria-label": accessibleName,
            "data-table": node.name
        });
        group.appendChild(svgElement("rect", {
            x: 0, y: 0, width: node.width, height: node.height, rx: 4, class: "er-box"
        }));
        const title = svgElement("text", { x: 12, y: 23, class: "er-title" });
        title.textContent = node.name;
        group.appendChild(title);
        node.columns.forEach((column, index) => {
            const text = svgElement("text", {
                x: 12, y: 54 + index * 22, class: "er-column"
            });
            const markerText = column.pk ? "◆ " : column.references ? "↗ " : "";
            text.textContent = `${markerText}${column.name}  ${column.type}`;
            group.appendChild(text);
        });
        group.addEventListener("click", () => tableRoute(node.name, "data"));
        group.addEventListener("keydown", (event) => {
            if (event.key === "Enter" || event.key === " ") {
                event.preventDefault();
                tableRoute(node.name, "data");
            }
        });
        group.addEventListener("mouseenter", () => {
            svg.querySelectorAll(".er-edge").forEach((line) => {
                if (line.dataset.source === node.name || line.dataset.target === node.name) {
                    line.classList.add("highlight");
                }
            });
        });
        group.addEventListener("mouseleave", () => {
            svg.querySelectorAll(".er-edge").forEach((line) => line.classList.remove("highlight"));
        });
        viewport.appendChild(group);
    }
    svg.appendChild(viewport);
    const pan = { active: false, x: 0, y: 0, startX: 0, startY: 0 };
    const pointers = new Map();
    let pinch = null;
    const pointerDistance = () => {
        const [first, second] = [...pointers.values()];
        return Math.hypot(first.x - second.x, first.y - second.y);
    };
    svg.addEventListener("pointerdown", (event) => {
        if (event.target.closest(".er-table")) return;
        pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
        svg.setPointerCapture(event.pointerId);
        if (pointers.size === 2) {
            pinch = { distance: pointerDistance(), zoom: state.erZoom };
            pan.active = false;
        } else {
            pan.active = true;
            pan.x = state.erOffset.x;
            pan.y = state.erOffset.y;
            pan.startX = event.clientX;
            pan.startY = event.clientY;
        }
    });
    svg.addEventListener("pointermove", (event) => {
        if (!pointers.has(event.pointerId)) return;
        pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
        if (pinch && pointers.size >= 2 && pinch.distance > 0) {
            state.erZoom = Math.min(2.5, Math.max(0.35,
                pinch.zoom * pointerDistance() / pinch.distance));
        } else if (pan.active) {
            state.erOffset = { x: pan.x + event.clientX - pan.startX,
                y: pan.y + event.clientY - pan.startY };
        }
        viewport.setAttribute("transform",
            `translate(${state.erOffset.x} ${state.erOffset.y}) scale(${state.erZoom})`);
    });
    const endPointer = (event) => {
        pointers.delete(event.pointerId);
        if (pointers.size < 2) pinch = null;
        if (pointers.size === 1) {
            const [pointer] = pointers.values();
            pan.active = true;
            pan.x = state.erOffset.x;
            pan.y = state.erOffset.y;
            pan.startX = pointer.x;
            pan.startY = pointer.y;
        } else if (pointers.size === 0) {
            pan.active = false;
        }
    };
    svg.addEventListener("pointerup", endPointer);
    svg.addEventListener("pointercancel", endPointer);
    svg.addEventListener("wheel", (event) => {
        event.preventDefault();
        state.erZoom = Math.min(2.5, Math.max(0.35,
            state.erZoom * (event.deltaY < 0 ? 1.1 : 0.9)));
        viewport.setAttribute("transform",
            `translate(${state.erOffset.x} ${state.erOffset.y}) scale(${state.erZoom})`);
    }, { passive: false });
    wrapper.appendChild(svg);
    container.appendChild(wrapper);
    const controls = make("div", "er-controls");
    for (const [label, action] of [
        ["Zoom in", () => { state.erZoom = Math.min(2.5, state.erZoom * 1.2); }],
        ["Zoom out", () => { state.erZoom = Math.max(0.35, state.erZoom / 1.2); }],
        ["Fit", () => { state.erZoom = 1; state.erOffset = { x: 0, y: 0 }; }]
    ]) {
        const button = make("button", "button button-secondary", label);
        button.type = "button";
        button.addEventListener("click", () => {
            action();
            viewport.setAttribute("transform",
                `translate(${state.erOffset.x} ${state.erOffset.y}) scale(${state.erZoom})`);
        });
        controls.appendChild(button);
    }
    const exportSvg = make("button", "button button-secondary", "Export SVG");
    exportSvg.type = "button";
    exportSvg.addEventListener("click", () => {
        const clone = svg.cloneNode(true);
        clone.setAttribute("xmlns", "http://www.w3.org/2000/svg");
        downloadText("custom-db-relations.svg", new XMLSerializer().serializeToString(clone));
    });
    const exportPng = make("button", "button button-secondary", "Export PNG");
    exportPng.type = "button";
    exportPng.addEventListener("click", () => exportDiagramPng(svg, maxX, maxY));
    controls.append(exportSvg, exportPng);
    container.appendChild(controls);
}

function exportDiagramPng(svg, width, height) {
    const source = new XMLSerializer().serializeToString(svg);
    const image = new Image();
    const url = URL.createObjectURL(new Blob([source], { type: "image/svg+xml;charset=utf-8" }));
    image.onload = () => {
        const canvas = document.createElement("canvas");
        canvas.width = width;
        canvas.height = height;
        const context = canvas.getContext("2d");
        if (!context) {
            URL.revokeObjectURL(url);
            setStatus("PNG export is unavailable in this browser.", true);
            return;
        }
        context.drawImage(image, 0, 0);
        URL.revokeObjectURL(url);
        canvas.toBlob((blob) => {
            if (blob) {
                const output = URL.createObjectURL(blob);
                const link = make("a");
                link.href = output;
                link.download = "custom-db-relations.png";
                link.click();
                URL.revokeObjectURL(output);
            } else {
                setStatus("Could not encode the ER diagram as PNG.", true);
            }
        });
    };
    image.onerror = () => {
        URL.revokeObjectURL(url);
        setStatus("Could not render the ER diagram for PNG export.", true);
    };
    image.src = url;
}

function renderErPage() {
    const view = byId("studioView");
    clear(view);
    view.append(make("div", "studio-view-header", "Entity relationship diagram"));
    const filter = make("input", "control er-filter");
    filter.placeholder = "Related to table…";
    filter.setAttribute("aria-label", "Filter ER diagram to a table and its relations");
    const canvas = make("div", "er-container");
    const draw = () => {
        clear(canvas);
        const search = filter.value.trim().toLocaleLowerCase();
        let tables = state.tables;
        if (search) {
            const selected = state.tables.find((table) =>
                table.name.toLocaleLowerCase() === search);
            if (selected) {
                const names = new Set([selected.name]);
                selected.columns.forEach((column) => {
                    if (column.references) names.add(column.references.table);
                });
                state.tables.forEach((table) => {
                    if (table.columns.some((column) => column.references?.table === selected.name)) {
                        names.add(table.name);
                    }
                });
                tables = state.tables.filter((table) => names.has(table.name));
            } else {
                tables = state.tables.filter((table) =>
                    table.name.toLocaleLowerCase().includes(search));
            }
        }
        renderEr(canvas, tables);
    };
    filter.addEventListener("input", draw);
    view.append(filter, canvas);
    draw();
}

async function renderSystemPage(route) {
    const view = byId("studioView");
    clear(view);
    view.appendChild(make("h2", "studio-view-header", `System · ${route.view}`));
    try {
        if (route.view === "catalog") {
            const pre = make("pre", "json-view");
            pre.textContent = JSON.stringify(state.tables, null, 2);
            view.appendChild(pre);
        } else if (route.view === "freelist") {
            const response = await getJson("/api/v1/inspect/freelist");
            view.appendChild(make("pre", "json-view", JSON.stringify(response.data, null, 2)));
        } else if (route.view === "wal") {
            const response = await getJson("/api/v1/inspect/wal?limit=100");
            view.appendChild(make("pre", "json-view", JSON.stringify(response.data, null, 2)));
        } else if (route.view === "page") {
            const pageId = Number(route.page);
            const response = await getJson(`/api/v1/inspect/pages/${pageId}`);
            const page = response.data;
            const panel = make("div", "page-inspector");
            panel.appendChild(make("p", "mono",
                `Page ${page.page_id} · ${page.type} · checksum ${page.crc_ok ? "OK" : "FAILED"}`));
            if (route.slot !== undefined) {
                panel.appendChild(make("p", "muted", `RID slot highlight: ${route.slot}`));
            }
            const pre = make("pre", "json-view");
            const decodedSlots = page.decoded?.slots || page.decoded?.tuples || [];
            if (route.slot !== undefined && Array.isArray(decodedSlots)) {
                for (const slot of decodedSlots) {
                    if (String(slot.slot) === String(route.slot)) {
                        const highlighted = make("pre", "json-view");
                        highlighted.textContent = JSON.stringify(slot, null, 2);
                        highlighted.classList.add("focused-slot");
                        panel.appendChild(highlighted);
                    }
                }
            }
            pre.textContent = JSON.stringify(page, null, 2);
            panel.appendChild(pre);
            view.appendChild(panel);
        } else {
            throw new Error("Unknown system view");
        }
    } catch (error) {
        view.appendChild(make("p", "plain-error",
            `Request failed (${error.status || "network"}); ${error.message}`));
    }
}

function renderWelcome() {
    const view = byId("studioView");
    clear(view);
    view.append(make("h2", "", "CUSTOM-DB Studio"),
        make("p", "empty-state", state.tables.length
            ? "Choose a table in Explorer, open the ER diagram, or select top 100 from a table menu."
            : "No tables yet"));
    const er = make("button", "button button-secondary", "Open ER diagram");
    er.type = "button";
    er.addEventListener("click", () => routeTo("#/er"));
    view.appendChild(er);
}

function openTopQuery(table) {
    if (!safeIdentifier(table.name)) {
        setStatus("The table name cannot be safely inserted into a query.", true);
        return;
    }
    const sql = `SELECT * FROM ${table.name} LIMIT 100`;
    openQuery(sql);
}

function openQuery(sql) {
    const input = byId("sqlInput");
    input.value = sql;
    input.focus();
    input.scrollIntoView({ behavior: "smooth", block: "center" });
    setStatus("Query prepared in the SQL editor.");
}

function renderRoute() {
    const route = currentRoute();
    if (route.kind === "welcome") {
        state.table = null;
        renderWelcome();
    } else if (route.kind === "table") {
        const table = tableByName(route.name);
        if (!table) {
            state.table = null;
            const view = byId("studioView");
            clear(view);
            view.appendChild(make("p", "plain-error", "Table was dropped or does not exist."));
            return;
        }
        if (state.table?.name !== table.name) {
            state.rows = [];
            state.edits.clear();
            state.filters = [];
            state.order = table.columns.find((column) => column.pk)?.name || "";
            state.descending = false;
            state.pageIndex = 0;
            state.pageCursors = [null];
            state.visibleColumns = new Set(table.columns.map((column) => column.name));
            state.table = table;
        }
        state.subtab = route.subtab;
        renderTableView();
        if (route.subtab === "data" && !state.rows.length) loadRows();
    } else if (route.kind === "er") {
        state.table = null;
        renderErPage();
    } else if (route.kind === "system") {
        state.table = null;
        renderSystemPage(route);
    } else {
        byId("studioView").replaceChildren(make("p", "plain-error", route.message));
    }
}

async function confirmDrop(table) {
    if (state.readOnly !== false) return;
    const dialog = byId("studioDialog");
    clear(dialog);
    dialog.append(make("h2", "", `Drop ${table.name}?`),
        make("p", "", "This permanently removes the table and its data."));
    const actions = make("div", "dialog-actions");
    const cancel = make("button", "button button-secondary", "Cancel");
    cancel.type = "button";
    cancel.addEventListener("click", () => dialog.close());
    const drop = make("button", "button button-danger", "Drop table");
    drop.type = "button";
    drop.addEventListener("click", async () => {
        drop.disabled = true;
        try {
            const response = await getJson("/api/v1/query", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ sql: `DROP TABLE ${table.name}` })
            });
            dialog.close();
            state.table = null;
            routeTo("#/");
            setStatus(`Dropped table ${table.name}.`);
            window.dispatchEvent(new Event("custom-db:refresh-catalog"));
        } catch (error) {
            displayError(error, "Drop table");
            drop.disabled = false;
        }
    });
    actions.append(cancel, drop);
    dialog.appendChild(actions);
    dialog.showModal();
}

function handleF5(event) {
    if (event.key === "F5" && !event.target.matches("input,textarea,select,[contenteditable=true]")) {
        event.preventDefault();
        window.dispatchEvent(new Event("custom-db:refresh-catalog"));
    }
}

async function setEngineMode() {
    try {
        const result = await getJson("/api/v1/snapshot");
        state.readOnly = Boolean(result.data?.engine?.read_only);
        const label = state.readOnly ? "READ ONLY" : "WRITABLE";
        byId("studioMode").textContent = label;
        byId("studioMode").classList.toggle("mode-readonly", state.readOnly);
        byId("studioMode").classList.toggle("mode-writable", !state.readOnly);
        if (state.contextTable) renderTree();
        if (state.table && state.subtab === "data") renderTableView();
    } catch (error) {
        state.readOnly = null;
        byId("studioMode").textContent = "MODE UNKNOWN";
        byId("studioMode").classList.add("mode-readonly");
        setStatus(`Cannot confirm engine mode: ${error.message}`, true);
    }
}

function acceptCatalog(event) {
    const tables = event.detail;
    if (!Array.isArray(tables)) {
        setStatus("Catalog response is invalid.", true);
        return;
    }
    state.tables = tables;
    renderTree();
    renderRoute();
    if (tables.length === 0) setStatus("No tables yet.");
}

function handleCatalogRefresh() {
    window.dispatchEvent(new Event("custom-db:catalog-request"));
}

export function startStudio() {
    byId("explorerFilter").addEventListener("input", renderTree);
    byId("explorerRefresh").addEventListener("click", handleCatalogRefresh);
    byId("openErButton").addEventListener("click", () => routeTo("#/er"));
    byId("erTab").addEventListener("click", () => routeTo("#/er"));
    byId("queryTab").addEventListener("click", () => {
        const query = byId("queryTab");
        byId("erTab").setAttribute("aria-selected", "false");
        query.setAttribute("aria-selected", "true");
        byId("sqlInput").focus();
    });
    byId("studioContextMenu").addEventListener("keydown", (event) => {
        const items = [...event.currentTarget.querySelectorAll("button:not(:disabled)")];
        const current = items.indexOf(document.activeElement);
        if (event.key === "Escape") {
            hideContextMenu();
        } else if (event.key === "ArrowDown" || event.key === "ArrowUp") {
            event.preventDefault();
            const delta = event.key === "ArrowDown" ? 1 : -1;
            items[(current + delta + items.length) % items.length]?.focus();
        }
    });
    document.addEventListener("click", (event) => {
        if (!event.target.closest("#studioContextMenu")) hideContextMenu();
    });
    document.addEventListener("keydown", handleF5);
    window.addEventListener("hashchange", renderRoute);
    window.addEventListener("hashchange", () => {
        const isEr = currentRoute().kind === "er";
        byId("erTab").setAttribute("aria-selected", String(isEr));
        byId("queryTab").setAttribute("aria-selected", String(!isEr));
    });
    window.addEventListener("custom-db:catalog", acceptCatalog);
    window.addEventListener("custom-db:refresh-catalog", handleCatalogRefresh);
    renderWelcome();
    setEngineMode();
    if (window.location.hash) renderRoute();
}
