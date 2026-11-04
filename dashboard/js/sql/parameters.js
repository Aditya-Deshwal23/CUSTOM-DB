import { tokenize } from "./lexer.js";

export function detectParameters(source, overrides = {}) {
    const indices = new Set(tokenize(source).filter((token) => token.kind === "parameter")
        .map((token) => Number(token.text.slice(1))));
    if ([...indices].some((index) => !Number.isSafeInteger(index) || index < 1 || index > 50)) {
        throw new RangeError("SQL parameters must use indices from $1 through $50.");
    }
    const values = [...indices].sort((left, right) => left - right);
    if (values.length !== 0 && values.at(-1) !== values.length) {
        throw new RangeError("SQL parameter indices must be contiguous starting at $1.");
    }
    return values.map((index) => ({
        index,
        type: overrides[index] || inferType(overrides.values?.[index - 1] ?? ""),
        value: overrides.values?.[index - 1] ?? ""
    }));
}

export function inferType(value) {
    const text = String(value).trim();
    if (/^[+-]?\d+$/.test(text)) return "int";
    if (/^[+-]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?$/.test(text) && /[.eE]/.test(text)) {
        return "float";
    }
    if (/^(true|false)$/i.test(text)) return "boolean";
    if (/^null$/i.test(text)) return "null";
    return "text";
}

export function typedParameterValue(value, type) {
    if (type === "int") {
        const number = Number(value);
        if (!Number.isSafeInteger(number)) throw new TypeError("Parameter must be a safe integer.");
        return number;
    }
    if (type === "float") {
        const number = Number(value);
        if (!Number.isFinite(number)) throw new TypeError("Parameter must be a finite number.");
        return number;
    }
    if (type === "boolean") {
        if (!/^(true|false)$/i.test(String(value))) throw new TypeError("Parameter must be true or false.");
        return String(value).toLowerCase() === "true";
    }
    if (type === "null") return null;
    return String(value);
}
