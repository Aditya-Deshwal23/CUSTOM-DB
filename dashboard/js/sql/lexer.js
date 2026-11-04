import { FUNCTIONS, KEYWORDS } from "./keywords.js";

const isIdentifierStart = (character) => /[A-Za-z_]/.test(character);
const isIdentifierPart = (character) => /[A-Za-z0-9_$]/.test(character);

export function tokenize(source, options = {}) {
    const tokens = [];
    let index = 0;
    let line = 1;
    let column = 1;
    const advance = (character) => {
        if (character === "\n") {
            line += 1;
            column = 1;
        } else {
            column += 1;
        }
    };
    const push = (kind, start, tokenLine, tokenColumn) => {
        const text = source.slice(start, index);
        tokens.push({
            kind,
            text,
            start,
            end: index,
            line: tokenLine,
            column: tokenColumn
        });
    };

    while (index < source.length) {
        const character = source[index];
        if (/\s/.test(character)) {
            advance(character);
            index += 1;
            continue;
        }
        const start = index;
        const tokenLine = line;
        const tokenColumn = column;
        if (character === "-" && source[index + 1] === "-") {
            while (index < source.length && source[index] !== "\n") {
                advance(source[index]);
                index += 1;
            }
            if (options.includeComments) push("comment", start, tokenLine, tokenColumn);
            continue;
        }
        if (character === "/" && source[index + 1] === "*") {
            advance(source[index++]);
            advance(source[index++]);
            let closed = false;
            while (index < source.length) {
                if (source[index] === "*" && source[index + 1] === "/") {
                    advance(source[index++]);
                    advance(source[index++]);
                    closed = true;
                    break;
                }
                advance(source[index++]);
            }
            if (!closed) {
                if (!options.tolerant) {
                    throw new SyntaxError("Unterminated block comment", {
                        cause: { line: tokenLine, column: tokenColumn }
                    });
                }
            }
            if (options.includeComments || !closed) push("comment", start, tokenLine, tokenColumn);
            continue;
        }
        if (isIdentifierStart(character)) {
            advance(source[index++]);
            while (index < source.length && isIdentifierPart(source[index])) {
                advance(source[index++]);
            }
            const text = source.slice(start, index);
            const upper = text.toUpperCase();
            const kind = FUNCTIONS.has(upper) ? "function" : KEYWORDS.has(upper) ? "keyword" : "identifier";
            tokens.push({ kind, text, start, end: index, line: tokenLine, column: tokenColumn });
            continue;
        }
        if (/\d/.test(character) || (character === "." && /\d/.test(source[index + 1] || ""))) {
            if (character === ".") {
                advance(source[index++]);
            }
            while (/\d/.test(source[index] || "")) advance(source[index++]);
            if (source[index] === "." && character !== ".") {
                advance(source[index++]);
                while (/\d/.test(source[index] || "")) advance(source[index++]);
            }
            if (/[eE]/.test(source[index] || "")) {
                let exponent = index + 1;
                if (/[+-]/.test(source[exponent] || "")) exponent += 1;
                if (/\d/.test(source[exponent] || "")) {
                    while (index <= exponent) advance(source[index++]);
                    while (/\d/.test(source[index] || "")) advance(source[index++]);
                }
            }
            push("number", start, tokenLine, tokenColumn);
            continue;
        }
        if (character === "'") {
            advance(source[index++]);
            let closed = false;
            while (index < source.length) {
                const value = source[index];
                advance(source[index++]);
                if (value === "'") {
                    if (source[index] === "'") {
                        advance(source[index++]);
                    } else {
                        closed = true;
                        break;
                    }
                }
            }
            if (!closed) {
                if (!options.tolerant) {
                    throw new SyntaxError("Unterminated string literal", {
                        cause: { line: tokenLine, column: tokenColumn }
                    });
                }
            }
            push("string", start, tokenLine, tokenColumn);
            continue;
        }
        if (character === "$") {
            advance(source[index++]);
            const digits = index;
            while (/\d/.test(source[index] || "")) advance(source[index++]);
            if (digits === index) {
                if (options.tolerant && index === source.length) {
                    push("parameter", start, tokenLine, tokenColumn);
                    continue;
                }
                if (index < source.length && isIdentifierStart(source[index])) {
                    advance(source[index++]);
                    while (index < source.length && isIdentifierPart(source[index])) {
                        advance(source[index++]);
                    }
                    push("identifier", start, tokenLine, tokenColumn);
                    continue;
                }
                if (options.tolerant) {
                    push("unknown", start, tokenLine, tokenColumn);
                    continue;
                }
                throw new SyntaxError("Parameter placeholder must have a numeric index", {
                    cause: { line: tokenLine, column: tokenColumn }
                });
            }
            push("parameter", start, tokenLine, tokenColumn);
            continue;
        }
        const pair = source.slice(index, index + 2);
        if (["<>", "!=", "<=", ">="].includes(pair)) {
            advance(source[index++]);
            advance(source[index++]);
            push("operator", start, tokenLine, tokenColumn);
            continue;
        }
        advance(source[index++]);
        const kind = "(),.;[]{}".includes(character) ? "punctuation" :
            "+-*/%=<>!".includes(character) ? "operator" : "unknown";
        push(kind, start, tokenLine, tokenColumn);
    }
    return tokens;
}

export function tokenClass(token) {
    return token.kind === "function" ? "sql-function" : `sql-${token.kind}`;
}
