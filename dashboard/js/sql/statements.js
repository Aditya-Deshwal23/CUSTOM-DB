import { tokenize } from "./lexer.js";

export function splitStatements(source) {
    const tokens = tokenize(source, { tolerant: true });
    const statements = [];
    let depth = 0;
    let start = 0;
    for (const token of tokens) {
        if (token.text === "(") depth += 1;
        if (token.text === ")") depth = Math.max(0, depth - 1);
        if (token.text === ";" && depth === 0) {
            const statement = source.slice(start, token.start).trim();
            if (statement) statements.push(statement);
            start = token.end;
        }
    }
    const rest = source.slice(start).trim();
    if (rest) statements.push(rest);
    return statements;
}

export function statementAt(source, caret) {
    const boundedCaret = Math.max(0, Math.min(source.length, caret));
    const tokens = tokenize(source, { tolerant: true });
    let depth = 0;
    let start = 0;
    let previousStart = 0;
    let previousEnd = -1;
    for (const token of tokens) {
        if (token.start >= boundedCaret) break;
        if (token.text === "(") depth += 1;
        if (token.text === ")") depth = Math.max(0, depth - 1);
        if (token.text === ";" && depth === 0) {
            previousStart = start;
            previousEnd = token.start;
            start = token.end;
        }
    }
    depth = 0;
    for (const token of tokens) {
        if (token.start < boundedCaret) continue;
        if (token.text === "(") depth += 1;
        if (token.text === ")") depth = Math.max(0, depth - 1);
        if (token.text === ";" && depth === 0) {
            return source.slice(start, token.start).trim();
        }
    }
    const remaining = source.slice(start).trim();
    if (remaining && tokens.some((token) => token.start >= start && token.start < boundedCaret)) {
        return remaining;
    }
    return previousEnd >= 0 ? source.slice(previousStart, previousEnd).trim() : "";
}
