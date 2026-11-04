const element = (tag, className = "", text = "") => {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text) node.textContent = text;
    return node;
};

function flatten(node, depth = 0, rows = []) {
    rows.push({ node, depth });
    for (const child of node.children || []) flatten(child, depth + 1, rows);
    return rows;
}

export function renderPlan(container, planTree, planText = "", previous = null) {
    container.replaceChildren();
    if (!planTree) {
        container.append(element("pre", "plan-text", planText || "No structured plan was returned."));
        return;
    }
    const rows = flatten(planTree);
    const slowest = rows.reduce((slow, row) =>
        Number(row.node.time_us || 0) > Number(slow?.node.time_us || 0) ? row : slow, null);
    const table = element("table", "plan-table");
    const head = element("thead");
    const header = element("tr");
    for (const title of ["Operator", "Estimated rows", "Actual rows", "Time (µs)", "Δ time"]) {
        const cell = element("th", "", title);
        cell.scope = "col";
        header.appendChild(cell);
    }
    head.appendChild(header);
    table.append(head);
    const body = element("tbody");
    for (const { node, depth } of rows) {
        const row = element("tr", node === slowest?.node ? "plan-slowest" : "");
        const op = element("td", "plan-op", node.op || "Plan");
        op.style.paddingInlineStart = `${12 + depth * 20}px`;
        op.append(element("span", "", /Index/i.test(node.op || "") ? "▣ " : "▤ "));
        op.append(document.createTextNode(node.op || ""));
        row.append(op);
        for (const value of [node.est_rows, node.actual_rows, node.time_us]) {
            row.append(element("td", "", value ?? "—"));
        }
        const old = previous?.[node.id]?.time_us;
        row.append(element("td", "", old === undefined ? "—" : String(Number(node.time_us || 0) - old)));
        row.addEventListener("click", () => {
            const drawer = element("pre", "plan-detail", JSON.stringify(node, null, 2));
            container.querySelector(".plan-detail")?.remove();
            container.appendChild(drawer);
        });
        body.appendChild(row);
    }
    table.appendChild(body);
    container.appendChild(table);
    const actions = element("div", "plan-actions");
    const copyText = element("button", "button button-secondary", "Copy as text");
    copyText.type = "button";
    copyText.addEventListener("click", () => navigator.clipboard.writeText(planText || JSON.stringify(planTree, null, 2)));
    const copyJson = element("button", "button button-secondary", "Copy JSON");
    copyJson.type = "button";
    copyJson.addEventListener("click", () => navigator.clipboard.writeText(JSON.stringify(planTree, null, 2)));
    actions.append(copyText, copyJson);
    container.appendChild(actions);
}
