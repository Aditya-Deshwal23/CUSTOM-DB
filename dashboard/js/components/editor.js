import { tokenize, tokenClass } from "../sql/lexer.js";

const ESCAPE = /[&<>"']/g;
const escapeHtml = (value) => value.replace(ESCAPE, (character) => ({
    "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;", "'": "&#39;"
})[character]);
const PAIRS = new Map([["(", ")"], ["[", "]"], ["{", "}"], ["'", "'"], ["\"", "\""]]);
const CLOSERS = new Map([...PAIRS]
    .filter(([open, close]) => open !== close)
    .map(([open, close]) => [close, open]));
const LINE_HEIGHT = 21;

function findMatchingBracket(tokens, caret) {
    const bracketPairs = new Map([["(", ")"], ["[", "]"], ["{", "}"]]);
    const reverse = new Map([...bracketPairs].map(([open, close]) => [close, open]));
    const tokenIndex = tokens.findIndex((token) =>
        token.kind === "punctuation" &&
        (token.start === caret || token.end === caret));
    if (tokenIndex < 0) return -1;
    const current = tokens[tokenIndex].text;
    const opening = bracketPairs.has(current);
    const closing = reverse.has(current);
    if (!opening && !closing) return -1;
    const target = opening ? bracketPairs.get(current) : reverse.get(current);
    const direction = opening ? 1 : -1;
    let depth = 0;
    for (let index = tokenIndex; index >= 0 && index < tokens.length; index += direction) {
        const token = tokens[index];
        if (token.kind !== "punctuation") continue;
        if (token.text === current) depth += 1;
        if (token.text === target) depth -= 1;
        if (depth === 0) return index;
    }
    return -1;
}

export function createEditor({ textarea, highlight, gutter, currentLine, diagnostic, onChange,
    onFind, onAutocomplete }) {
    let composing = false;
    let frame = 0;
    let error = null;
    let autocompleteOpen = false;

    const render = () => {
        frame = 0;
        const source = textarea.value;
        const tokens = tokenize(source, { includeComments: true, tolerant: true });
        const matchingToken = findMatchingBracket(tokens, textarea.selectionStart);
        let output = "";
        let offset = 0;
        for (let index = 0; index < tokens.length; index += 1) {
            const token = tokens[index];
            output += escapeHtml(source.slice(offset, token.start));
            const errorLine = error && token.line === error.line &&
                token.column <= error.column && token.column + token.text.length > error.column;
            const matched = index === matchingToken;
            output += `<span class="${tokenClass(token)}${errorLine ? " sql-error-token" : ""}` +
                `${matched ? " sql-bracket-match" : ""}">` +
                `${escapeHtml(token.text)}</span>`;
            offset = token.end;
        }
        output += escapeHtml(source.slice(offset));
        highlight.innerHTML = `${output}\n`;
        const lineCount = source.split("\n").length;
        gutter.textContent = Array.from({ length: lineCount }, (_, index) =>
            `${error?.line === index + 1 ? "! " : ""}${index + 1}`).join("\n");
        gutter.style.height = `${Math.max(lineCount, 1) * LINE_HEIGHT}px`;
        highlight.scrollTop = textarea.scrollTop;
        highlight.scrollLeft = textarea.scrollLeft;
        gutter.scrollTop = textarea.scrollTop;
        const caretLine = source.slice(0, textarea.selectionStart).split("\n").length;
        currentLine.style.top = `${(caretLine - 1) * LINE_HEIGHT - textarea.scrollTop}px`;
        currentLine.hidden = false;
        gutter.dataset.activeLine = String(caretLine);
        if (error) {
            diagnostic.textContent = error.message || `Syntax error at line ${error.line}, column ${error.column}.`;
            diagnostic.hidden = false;
        } else {
            diagnostic.textContent = "";
            diagnostic.hidden = true;
        }
    };
    const scheduleRender = () => {
        if (!frame) frame = requestAnimationFrame(render);
    };
    const replaceSelection = (replacement, selectionStart, selectionEnd = selectionStart) => {
        const start = textarea.selectionStart;
        const end = textarea.selectionEnd;
        textarea.setRangeText(replacement, start, end, "preserve");
        textarea.setSelectionRange(selectionStart, selectionEnd);
        textarea.dispatchEvent(new Event("input", { bubbles: true }));
    };
    const setError = (value) => {
        error = value;
        scheduleRender();
    };
    const commentToggle = () => {
        const source = textarea.value;
        const start = source.lastIndexOf("\n", textarea.selectionStart - 1) + 1;
        const selectionEnd = textarea.selectionEnd;
        const finalLine = source.indexOf("\n", selectionEnd);
        const end = finalLine < 0 ? source.length : finalLine;
        const selected = source.slice(start, end);
        const lines = selected.split("\n");
        const remove = lines.every((line) => /^\s*--\s?/.test(line));
        const updated = lines.map((line) => remove
            ? line.replace(/^(\s*)-- ?/, "$1")
            : line.replace(/^(\s*)/, "$1-- "));
        const replacement = updated.join("\n");
        textarea.setRangeText(replacement, start, end, "select");
        textarea.dispatchEvent(new Event("input", { bubbles: true }));
    };
    const indentSelection = (outdent) => {
        const source = textarea.value;
        const start = source.lastIndexOf("\n", textarea.selectionStart - 1) + 1;
        let end = source.indexOf("\n", textarea.selectionEnd);
        if (end < 0) end = source.length;
        const selected = source.slice(start, end);
        const lines = selected.split("\n");
        const replacement = lines.map((line) => outdent
            ? line.replace(/^(?: {1,4}|\t)/, "")
            : `    ${line}`).join("\n");
        const startDelta = outdent ? Math.max(0, selected.match(/^(?: {1,4}|\t)/)?.[0].length || 0) : 0;
        textarea.setRangeText(replacement, start, end, "select");
        textarea.setSelectionRange(
            Math.max(start, textarea.selectionStart - startDelta),
            textarea.selectionEnd + (outdent ? 0 : lines.length * 4)
        );
        textarea.dispatchEvent(new Event("input", { bubbles: true }));
    };

    textarea.addEventListener("compositionstart", () => { composing = true; });
    textarea.addEventListener("compositionend", () => {
        composing = false;
        scheduleRender();
    });
    textarea.addEventListener("input", () => {
        error = null;
        onChange?.(textarea.value);
        scheduleRender();
    });
    textarea.addEventListener("click", scheduleRender);
    textarea.addEventListener("keyup", scheduleRender);
    textarea.addEventListener("scroll", scheduleRender, { passive: true });
    textarea.addEventListener("keydown", (event) => {
        if (composing || event.isComposing || event.keyCode === 229) return;
        const modifier = event.ctrlKey || event.metaKey;
        if (modifier && event.key.toLowerCase() === "/") {
            event.preventDefault();
            commentToggle();
            return;
        }

        if (modifier && event.key.toLowerCase() === "f") {
            event.preventDefault();
            onFind?.();
            return;
        }
        if (modifier && event.code === "Space") {
            event.preventDefault();
            autocompleteOpen = true;
            onAutocomplete?.(textarea, true);
            return;
        }
        if (event.key === "Tab") {
            event.preventDefault();
            if (textarea.selectionStart !== textarea.selectionEnd ||
                textarea.value.slice(textarea.selectionStart, textarea.selectionEnd).includes("\n")) {
                indentSelection(event.shiftKey);
            } else if (event.shiftKey) {
                const lineStart = textarea.value.lastIndexOf("\n", textarea.selectionStart - 1) + 1;
                const line = textarea.value.slice(lineStart, textarea.selectionStart);
                const indent = line.match(/^(?: {1,4}|\t)/)?.[0];
                if (indent) {
                    textarea.setRangeText("", lineStart, lineStart + indent.length, "preserve");
                    scheduleRender();
                }
            } else {
                const at = textarea.selectionStart;
                replaceSelection("    ", at + 4);
            }
            return;
        }
        if (event.key === "Enter" && !event.shiftKey) {
            const lineStart = textarea.value.lastIndexOf("\n", textarea.selectionStart - 1) + 1;
            const indent = textarea.value.slice(lineStart, textarea.selectionStart).match(/^\s*/)?.[0] || "";
            if (indent) {
                event.preventDefault();
                const at = textarea.selectionStart;
                replaceSelection(`\n${indent}`, at + indent.length + 1);
            }
            return;
        }
        if (event.key === "Enter" && event.ctrlKey) {
            event.preventDefault();
            return;
        }
        if (!event.altKey && !modifier && event.key.length === 1 &&
            (PAIRS.has(event.key) || CLOSERS.has(event.key))) {
            const start = textarea.selectionStart;
            const end = textarea.selectionEnd;
            const next = textarea.value[start];
            if (event.key === "'" && next === "'" && start === end) {
                event.preventDefault();
                textarea.setSelectionRange(start + 1, start + 1);
                return;
            }
            if (CLOSERS.has(event.key) && next === event.key && start === end) {
                event.preventDefault();
                textarea.setSelectionRange(start + 1, start + 1);
                return;
            }
            if (PAIRS.has(event.key) && start === end && event.key === "'" &&
                /[A-Za-z0-9_]/.test(textarea.value[start - 1] || "")) {
                return;
            }
            if (CLOSERS.has(event.key)) return;
            event.preventDefault();
            textarea.setRangeText(`${event.key}${PAIRS.get(event.key)}`, start, end, "select");
            textarea.setSelectionRange(start + 1, start + 1);
            textarea.dispatchEvent(new Event("input", { bubbles: true }));
        }
    });
    window.addEventListener("resize", scheduleRender);
    scheduleRender();
    return {
        textarea,
        setError,
        refresh: scheduleRender,
        focus: () => textarea.focus(),
        replace: (value) => {
            textarea.value = value;
            error = null;
            scheduleRender();
        },
        get autocompleteOpen() { return autocompleteOpen; },
        closeAutocomplete() { autocompleteOpen = false; }
    };
}
