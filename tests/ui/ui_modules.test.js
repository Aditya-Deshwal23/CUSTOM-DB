import assert from "node:assert/strict";
import test from "node:test";

import { filtersToJson, rowsUrl } from "../../dashboard/js/filter.js";
import { toCsv, toInsert, toJson, toMarkdown, toTsv } from "../../dashboard/js/exporters.js";
import { layoutRelations } from "../../dashboard/js/er-layout.js";
import { nextGridCell } from "../../dashboard/js/grid.js";

test("filter builder creates bounded, typed JSON predicates", () => {
    const columns = [
        { name: "id", type: "INT" },
        { name: "active", type: "BOOLEAN" },
        { name: "label", type: "VARCHAR", length: 20 }
    ];
    assert.deepEqual(filtersToJson([
        { column: "id", op: ">=", value: "12" },
        { column: "active", op: "=", value: "false" },
        { column: "label", op: "ISNULL" }
    ], columns), [
        { column: "id", op: ">=", value: 12 },
        { column: "active", op: "=", value: false },
        { column: "label", op: "ISNULL" }
    ]);
    assert.throws(() => filtersToJson([{ column: "id", op: "=", value: "1.5" }], columns));
    const url = rowsUrl("a table", {
        limit: 50, after: "token +/", order: "id", descending: true,
        filters: [{ column: "label", op: "=", value: "a&b" }]
    });
    const parsed = new URL(url, "http://localhost");
    assert.equal(parsed.pathname, "/api/v1/tables/a%20table/rows");
    assert.equal(parsed.searchParams.get("after"), "token +/");
    assert.equal(parsed.searchParams.get("order"), "id:desc");
    assert.deepEqual(JSON.parse(parsed.searchParams.get("filter")), [
        { column: "label", op: "=", value: "a&b" }
    ]);
});

test("copy and export formats preserve quoting, newlines, NULL, and Unicode", () => {
    const columns = [{ name: "text", type: "VARCHAR" }, { name: "value", type: "INT" }];
    const rows = [
        { text: 'line 1\n"quoted", café', value: null },
        { text: "O'Brien", value: 7 }
    ];
    assert.match(toTsv(columns, rows), /"line 1\n""quoted"", café"/);
    assert.match(toCsv(columns, rows), /"line 1\n""quoted"", café"/);
    assert.deepEqual(JSON.parse(toJson(columns, rows)), rows);
    assert.match(toInsert("notes", columns, rows), /NULL/);
    assert.match(toInsert("notes", columns, rows), /'O''Brien'/);
    assert.match(toInsert("notes", columns, rows), /café/);
    assert.match(toMarkdown(columns, rows), /line 1<br>"quoted", café/);
    assert.match(toMarkdown(columns, rows), /\| NULL \|/);
});

test("ER layout is stable and routes foreign-key edges orthogonally", () => {
    const tables = [
        { name: "child", columns: [{ name: "parent_id", references: { table: "parent" } }] },
        { name: "parent", columns: [{ name: "id", pk: true }] },
        { name: "solo", columns: [{ name: "id" }] }
    ];
    const first = layoutRelations(tables);
    const second = layoutRelations(tables);
    assert.deepEqual(first, second);
    assert.deepEqual(first.nodes.map((node) => node.name).sort(), ["child", "parent", "solo"]);
    assert.equal(first.edges.length, 1);
    assert.match(first.edges[0].path, /^M .+ H .+ V .+ H .+$/);
});

test("grid keyboard navigation clamps to the available row and column range", () => {
    assert.deepEqual(nextGridCell(0, 0, "ArrowUp", 10, 4), { row: 0, column: 0 });
    assert.deepEqual(nextGridCell(9, 3, "ArrowDown", 10, 4), { row: 9, column: 3 });
    assert.deepEqual(nextGridCell(4, 1, "ArrowRight", 10, 4), { row: 4, column: 2 });
    assert.equal(nextGridCell(4, 1, "Enter", 10, 4), null);
});
