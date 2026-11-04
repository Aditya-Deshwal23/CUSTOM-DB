function valueForCell(value) {
    return value === null || value === undefined ? "" : String(value);
}

export function toTsv(columns, rows) {
    const quote = (value) => {
        const text = value === null || value === undefined ? "NULL" : String(value);
        return /["\t\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
    };
    return [
        columns.map((column) => quote(column.name)).join("\t"),
        ...rows.map((row) => columns.map((column) => quote(row[column.name])).join("\t"))
    ].join("\n");
}

export function toCsv(columns, rows) {
    const quote = (value) => {
        const text = valueForCell(value);
        return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
    };
    return [
        columns.map((column) => quote(column.name)).join(","),
        ...rows.map((row) => columns.map((column) => quote(row[column.name])).join(","))
    ].join("\r\n");
}

export function toJson(columns, rows) {
    return JSON.stringify(rows.map((row) => Object.fromEntries(
        columns.map((column) => [column.name, row[column.name] ?? null])
    )), null, 2);
}

export function toInsert(table, columns, rows) {
    const identifier = /^[A-Za-z_][A-Za-z0-9_]*$/;
    if (!identifier.test(table) || columns.some((column) => !identifier.test(column.name))) {
        throw new TypeError("SQL INSERT export requires simple SQL identifiers");
    }
    const sqlValue = (value) => {
        if (value === null || value === undefined) {
            return "NULL";
        }
        if (typeof value === "string") {
            return `'${value.replaceAll("'", "''")}'`;
        }
        if (typeof value === "boolean") {
            return value ? "TRUE" : "FALSE";
        }
        if (typeof value === "number" && Number.isFinite(value)) {
            return String(value);
        }
        throw new TypeError("Unsupported SQL INSERT value");
    };
    const names = columns.map((column) => column.name).join(", ");
    return rows.map((row) =>
        `INSERT INTO ${table} (${names}) VALUES (${columns.map((column) =>
            sqlValue(row[column.name])).join(", ")});`
    ).join("\n");
}

export function toMarkdown(columns, rows) {
    const cell = (value) => (value === null || value === undefined ? "NULL" : String(value))
        .replaceAll("|", "\\|")
        .replaceAll("\r", " ").replaceAll("\n", "<br>");
    return [
        `| ${columns.map((column) => cell(column.name)).join(" | ")} |`,
        `| ${columns.map(() => "---").join(" | ")} |`,
        ...rows.map((row) => `| ${columns.map((column) => cell(row[column.name])).join(" | ")} |`)
    ].join("\n");
}
