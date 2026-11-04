const FILTER_OPERATORS = new Set(["=", "<>", "<", "<=", ">", ">=", "LIKE", "ISNULL"]);

export function filtersToJson(filters, columns) {
    return filters.filter((filter) => filter.column && filter.op).map((filter) => {
        const column = columns.find((item) => item.name === filter.column);
        if (!column) {
            throw new TypeError(`Unknown filter column: ${filter.column}`);
        }
        const op = String(filter.op).toUpperCase();
        if (!FILTER_OPERATORS.has(op)) {
            throw new TypeError(`Unsupported filter operator: ${op}`);
        }
        if (op === "ISNULL") {
            return { column: column.name, op };
        }
        let value = filter.value;
        if (column.type === "INT" || column.type === "BIGINT") {
            value = Number(value);
            if (!Number.isSafeInteger(value)) {
                throw new TypeError(`${column.type} filter must be a safe integer`);
            }
        } else if (column.type === "DOUBLE") {
            value = Number(value);
            if (!Number.isFinite(value)) {
                throw new TypeError("DOUBLE filter must be finite");
            }
        } else if (column.type === "BOOLEAN") {
            if (value !== true && value !== false && value !== "true" && value !== "false") {
                throw new TypeError("BOOLEAN filter must be true or false");
            }
            value = value === true || value === "true";
        } else if (column.type === "VARCHAR") {
            value = String(value);
            if (value.length > Number(column.length || 1024)) {
                throw new TypeError("VARCHAR filter exceeds the column length");
            }
        }
        return { column: column.name, op, value };
    });
}
export function rowsUrl(table, options = {}) {
    const query = new URLSearchParams();
    query.set("limit", String(options.limit ?? 100));
    if (options.after) {
        query.set("after", options.after);
    }
    if (options.order) {
        query.set("order", `${options.order}${options.descending ? ":desc" : ""}`);
    }
    if (options.filters?.length) {
        query.set("filter", JSON.stringify(options.filters));
    }
    return `/api/v1/tables/${encodeURIComponent(table)}/rows?${query.toString()}`;
}
