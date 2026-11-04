import { toTsv } from "./exporters.js";

export const GRID_ROW_HEIGHT = 32;

export function nextGridCell(row, column, key, rowCount, columnCount) {
    let nextRow = row;
    let nextColumn = column;
    if (key === "ArrowDown") nextRow += 1;
    else if (key === "ArrowUp") nextRow -= 1;
    else if (key === "ArrowRight") nextColumn += 1;
    else if (key === "ArrowLeft") nextColumn -= 1;
    else if (key === "Home") nextColumn = 0;
    else if (key === "End") nextColumn = columnCount - 1;
    else if (key === "PageDown") nextRow += 10;
    else if (key === "PageUp") nextRow -= 10;
    else return null;
    return {
        row: Math.max(0, Math.min(rowCount - 1, nextRow)),
        column: Math.max(0, Math.min(columnCount - 1, nextColumn))
    };
}

export function createVirtualGrid(container, columns, rows, options = {}) {
    const rowHeight = options.rowHeight || GRID_ROW_HEIGHT;
    const overscan = options.overscan ?? 8;
    const root = document.createElement("div");
    root.className = "data-grid";
    root.setAttribute("role", "grid");
    root.setAttribute("aria-label", options.label || "Table data");
    root.setAttribute("aria-rowcount", String(rows.length + 1));
    root.setAttribute("aria-colcount", String(columns.length + (options.showRid ? 1 : 0)));
    root.tabIndex = 0;

    const header = document.createElement("div");
    header.className = "grid-header";
    header.setAttribute("role", "row");
    header.setAttribute("aria-rowindex", "1");
    const visibleColumns = options.showRid ? [{ name: "RID", type: "RID" }, ...columns] : columns;
    for (const column of visibleColumns) {
        const cell = document.createElement("div");
        cell.className = "grid-header-cell";
        cell.setAttribute("role", "columnheader");
        cell.textContent = column.name;
        const resize = document.createElement("span");
        resize.className = "column-resize";
        resize.setAttribute("aria-hidden", "true");
        cell.appendChild(resize);
        header.appendChild(cell);
    }
    root.appendChild(header);

    const viewport = document.createElement("div");
    viewport.className = "grid-viewport";
    viewport.tabIndex = -1;
    const topSpace = document.createElement("div");
    const rowsNode = document.createElement("div");
    const bottomSpace = document.createElement("div");
    viewport.append(topSpace, rowsNode, bottomSpace);
    root.appendChild(viewport);
    container.replaceChildren(root);

    let active = { row: 0, column: 0 };
    let anchor = { row: 0, column: 0 };
    const columnWidths = visibleColumns.map(() => "160px");
    const applyColumnWidths = () => {
        const template = columnWidths.join(" ");
        header.style.gridTemplateColumns = template;
        rowsNode.querySelectorAll(".grid-row").forEach((row) => {
            row.style.gridTemplateColumns = template;
        });
    };
    const renderRows = () => {
        const start = Math.max(0, Math.floor(viewport.scrollTop / rowHeight) - overscan);
        const count = Math.ceil(viewport.clientHeight / rowHeight) + overscan * 2;
        const end = Math.min(rows.length, start + count);
        topSpace.style.height = `${start * rowHeight}px`;
        bottomSpace.style.height = `${Math.max(0, rows.length - end) * rowHeight}px`;
        const fragment = document.createDocumentFragment();
        for (let rowIndex = start; rowIndex < end; rowIndex += 1) {
            const row = rows[rowIndex];
            const rowNode = document.createElement("div");
            rowNode.className = "grid-row";
            rowNode.setAttribute("role", "row");
            rowNode.setAttribute("aria-rowindex", String(rowIndex + 2));
            rowNode.dataset.row = String(rowIndex);
            for (let columnIndex = 0; columnIndex < visibleColumns.length; columnIndex += 1) {
                const column = visibleColumns[columnIndex];
                const cell = document.createElement("div");
                cell.className = "grid-cell";
                cell.setAttribute("role", "gridcell");
                cell.dataset.column = String(columnIndex);
                if (options.getCellClass) {
                    const className = options.getCellClass(row, column);
                    if (className) cell.classList.add(...className.split(/\s+/).filter(Boolean));
                }
                if (active.row === rowIndex && active.column === columnIndex) {
                    cell.tabIndex = 0;
                }
                let value = options.showRid && columnIndex === 0
                    ? row.rid
                    : row.values?.[column.name] ?? row[column.name] ?? null;
                if (!(options.showRid && columnIndex === 0) && options.getDisplayValue) {
                    value = options.getDisplayValue(row, column, value);
                }
                if (options.showRid && columnIndex === 0 && options.onRidClick) {
                    const link = document.createElement("button");
                    link.type = "button";
                    link.className = "rid-link";
                    link.textContent = String(value);
                    link.addEventListener("click", () => options.onRidClick(row.rid));
                    cell.appendChild(link);
                } else {
                    cell.textContent = value === null ? "NULL" : String(value);
                }
                if (value === null) cell.classList.add("null-cell");
                if (column.type === "INT" || column.type === "BIGINT" || column.type === "DOUBLE") {
                    cell.classList.add("numeric-cell");
                }
                if (column.type === "BOOLEAN") cell.classList.add("boolean-cell");
                if (options.onCellDoubleClick && !(options.showRid && columnIndex === 0)) {
                    cell.addEventListener("dblclick", () => options.onCellDoubleClick({
                        row, rowIndex,
                        column: visibleColumns[columnIndex],
                        columnIndex: options.showRid ? columnIndex - 1 : columnIndex,
                        cell
                    }));
                }
                cell.addEventListener("click", (event) => {
                    if (event.shiftKey) {
                        active = { row: rowIndex, column: columnIndex };
                    } else {
                        anchor = { row: rowIndex, column: columnIndex };
                        active = { ...anchor };
                    }
                    root.querySelectorAll(".grid-cell.selected-cell")
                        .forEach((item) => item.classList.remove("selected-cell"));
                    const lowRow = Math.min(anchor.row, active.row);
                    const highRow = Math.max(anchor.row, active.row);
                    const lowColumn = Math.min(anchor.column, active.column);
                    const highColumn = Math.max(anchor.column, active.column);
                    root.querySelectorAll(".grid-row").forEach((visibleRow) => {
                        const currentRow = Number(visibleRow.dataset.row);
                        if (currentRow < lowRow || currentRow > highRow) return;
                        visibleRow.querySelectorAll(".grid-cell").forEach((selectedCell) => {
                            const currentColumn = Number(selectedCell.dataset.column);
                            if (currentColumn >= lowColumn && currentColumn <= highColumn) {
                                selectedCell.classList.add("selected-cell");
                            }
                        });
                    });
                    if (!event.shiftKey) cell.focus();
                });
                rowNode.appendChild(cell);
            }
            rowNode.style.gridTemplateColumns = columnWidths.join(" ");
            fragment.appendChild(rowNode);
        }
        rowsNode.replaceChildren(fragment);
        applyColumnWidths();
    };

    viewport.addEventListener("scroll", renderRows, { passive: true });
    root.addEventListener("keydown", (event) => {
        if (!rows.length) return;
        if (event.target !== root && !event.target.matches(".grid-cell")) return;
        const next = nextGridCell(active.row, active.column, event.key, rows.length, visibleColumns.length);
        if (!next) return;
        event.preventDefault();
        active = next;
        viewport.scrollTop = Math.max(0, active.row * rowHeight -
            Math.floor(viewport.clientHeight / 2));
        renderRows();
        root.querySelector(`[data-row="${active.row}"] [data-column="${active.column}"]`)?.focus();
    });
    root.addEventListener("copy", (event) => {
        const firstRow = Math.min(anchor.row, active.row);
        const lastRow = Math.max(anchor.row, active.row);
        const firstColumn = Math.min(anchor.column, active.column);
        const lastColumn = Math.max(anchor.column, active.column);
        const selectedColumns = visibleColumns.slice(firstColumn, lastColumn + 1)
            .filter((column) => column.name !== "RID");
        const selectedRows = rows.slice(firstRow, lastRow + 1)
            .map((row) => Object.fromEntries(selectedColumns.map((column) =>
                [column.name, row.values?.[column.name] ?? row[column.name] ?? null])));
        if (selectedRows.length && selectedColumns.length && event.clipboardData) {
            event.clipboardData.setData("text/plain", toTsv(selectedColumns, selectedRows));
            event.preventDefault();
        }
    });
    header.addEventListener("click", (event) => {
        if (event.target.closest(".column-resize")) return;
        const cell = event.target.closest(".grid-header-cell");
        if (!cell || !options.onSort) return;
        const columnIndex = [...header.children].indexOf(cell);
        const column = visibleColumns[columnIndex];
        if (column && column.name !== "RID") options.onSort(column);
    });
    renderRows();
    header.addEventListener("pointerdown", (event) => {
            const handle = event.target.closest(".column-resize");
            if (!handle) return;
            const cell = handle.parentElement;
            const initialWidth = cell.getBoundingClientRect().width;
            const startX = event.clientX;
            const columnIndex = [...header.children].indexOf(cell);
            const move = (moveEvent) => {
                columnWidths[columnIndex] =
                    `${Math.max(72, initialWidth + moveEvent.clientX - startX)}px`;
                applyColumnWidths();
            };
            const end = () => {
                document.removeEventListener("pointermove", move);
                document.removeEventListener("pointerup", end);
            };
            document.addEventListener("pointermove", move);
            document.addEventListener("pointerup", end, { once: true });
    });
    header.addEventListener("dblclick", (event) => {
            const cell = event.target.closest(".grid-header-cell");
            if (!cell) return;
            const columnIndex = [...header.children].indexOf(cell);
            const values = [visibleColumns[columnIndex]?.name,
                ...rows.slice(0, 100).map((row) => {
                    const column = visibleColumns[columnIndex];
                    return options.showRid && columnIndex === 0
                        ? row.rid : row.values?.[column?.name];
                })].map((value) => String(value ?? "NULL"));
            const width = Math.min(360, Math.max(72,
                Math.max(...values.map((value) => value.length)) * 8 + 30));
            columnWidths[columnIndex] = `${width}px`;
            applyColumnWidths();
    });
    return {
        root,
        viewport,
        render: renderRows,
        activeRow: () => rows[active.row] || null,
        selectedRows: () => {
            const firstRow = Math.min(anchor.row, active.row);
            const lastRow = Math.max(anchor.row, active.row);
            const firstColumn = Math.min(anchor.column, active.column);
            const lastColumn = Math.max(anchor.column, active.column);
            const selectedColumns = visibleColumns.slice(firstColumn, lastColumn + 1)
                .filter((column) => column.name !== "RID");
            return {
                columns: selectedColumns,
                rows: rows.slice(firstRow, lastRow + 1).map((row) =>
                    Object.fromEntries(selectedColumns.map((column) =>
                        [column.name, row.values?.[column.name] ?? row[column.name] ?? null])))
            };
        }
    };
}
