import { KEYWORDS } from "./keywords.js";
import { tokenize } from "./lexer.js";

const CLAUSES = new Set(["SELECT", "FROM", "WHERE", "GROUP", "HAVING", "ORDER", "LIMIT", "OFFSET"]);

export function formatSql(source) {
    const tokens = tokenize(source, { includeComments: true, tolerant: true });
    let output = "";
    let previousEnd = 0;
    let depth = 0;
    for (const token of tokens) {
        const gap = source.slice(previousEnd, token.start);
        output += gap;
        const upper = token.text.toUpperCase();
        if (depth === 0 && CLAUSES.has(upper) && output.trim()) {
            output = output.replace(/[ \t]+$/, "");
            if (!output.endsWith("\n")) output += "\n";
        }
        output += KEYWORDS.has(upper) && token.kind !== "comment" ? upper : token.text;
        previousEnd = token.end;
        if (token.text === "(") depth += 1;
        if (token.text === ")") depth = Math.max(0, depth - 1);
    }
    output += source.slice(previousEnd);
    return output.trim();
}
