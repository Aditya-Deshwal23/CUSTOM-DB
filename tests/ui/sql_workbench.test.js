import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import test from "node:test";
import { formatSql } from "../../dashboard/js/sql/formatter.js";
import { KEYWORDS } from "../../dashboard/js/sql/keywords.js";
import { detectParameters } from "../../dashboard/js/sql/parameters.js";
import { splitStatements, statementAt } from "../../dashboard/js/sql/statements.js";
import { tokenize } from "../../dashboard/js/sql/lexer.js";

test("lexer mirrors SQL strings, comments, parameters, and numeric tokens", () => {
    const tokens = tokenize("SELECT 'it''s; ok', $2, 1.25e-2 -- ; ignored\nFROM t");
    assert.deepEqual(tokens.map(({ kind, text }) => [kind, text]), [
        ["keyword", "SELECT"], ["string", "'it''s; ok'"], ["punctuation", ","],
        ["parameter", "$2"], ["punctuation", ","], ["number", "1.25e-2"],
        ["keyword", "FROM"], ["identifier", "t"]
    ]);
});

test("editing helpers tolerate incomplete strings, comments, and parameters", () => {
    assert.deepEqual(tokenize("'unfinished", { tolerant: true }).map((token) => token.kind),
        ["string"]);
    assert.deepEqual(tokenize("/* unfinished", { includeComments: true, tolerant: true })
        .map((token) => token.kind), ["comment"]);
    assert.deepEqual(splitStatements("SELECT '$1;'; SELECT $"), ["SELECT '$1;'", "SELECT $"]);
    assert.throws(() => tokenize("'unfinished"), /Unterminated string literal/);
});

test("splitter only separates top-level semicolons and locates the caret statement", () => {
    const script = "SELECT ';'; /* ; */ SELECT ($1); -- ;\nUPDATE t SET x = 1";
    assert.deepEqual(splitStatements(script), [
        "SELECT ';'", "/* ; */ SELECT ($1)", "-- ;\nUPDATE t SET x = 1"
    ]);
    assert.equal(statementAt(script, script.indexOf("$1")), "/* ; */ SELECT ($1)");
    assert.equal(statementAt("SELECT 1;", 9), "SELECT 1");
    assert.equal(statementAt("SELECT 1; -- trailing", 21), "SELECT 1");
});

test("formatter uppercases clauses without rewriting literals or comments", () => {
    assert.equal(
        formatSql("select 'where; from' as x -- where\nfrom t where id = 1 order by id limit 2"),
        "SELECT 'where; from' AS x -- where\nFROM t\nWHERE id = 1\nORDER BY id\nLIMIT 2"
    );
});

test("parameters are ordered, inferred, and retain explicit overrides", () => {
    assert.deepEqual(detectParameters("SELECT $2, $1, $2, $3", {
        1: "int", 3: "boolean"
    }), [
        { index: 1, type: "int", value: "" },
        { index: 2, type: "text", value: "" },
        { index: 3, type: "boolean", value: "" }
    ]);
});

test("generated keywords stay synchronized with server parser keywords", () => {
    const result = spawnSync("python3", ["scripts/gen_sql_keywords.py", "--check"], {
        encoding: "utf8"
    });
    assert.equal(result.status, 0, result.stderr || result.stdout);
    assert(KEYWORDS.has("SELECT"));
    assert(KEYWORDS.has("REFERENCES"));
});
