const NODE_WIDTH = 230;
const NODE_GAP_X = 100;
const NODE_GAP_Y = 34;
const HEADER_HEIGHT = 34;
const COLUMN_HEIGHT = 22;

export function layoutRelations(tables) {
    const names = tables.map((table) => table.name).sort((a, b) => a.localeCompare(b));
    const byName = new Map(tables.map((table) => [table.name, table]));
    const edges = [];
    for (const table of tables) {
        for (const column of table.columns || []) {
            const target = column.references?.table;
            if (target && byName.has(target)) {
                edges.push({
                    source: table.name, target, column: column.name,
                    targetColumn: column.references.column
                });
            }
        }
    }
    edges.sort((a, b) => a.source.localeCompare(b.source) ||
        a.target.localeCompare(b.target) || a.column.localeCompare(b.column));

    const layers = new Map(names.map((name) => [name, 0]));
    for (let pass = 0; pass < names.length; pass += 1) {
        let changed = false;
        for (const edge of edges) {
            const next = Math.min(names.length - 1, (layers.get(edge.target) || 0) + 1);
            if ((layers.get(edge.source) || 0) < next) {
                layers.set(edge.source, next);
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }

    const members = new Map();
    for (const name of names) {
        const layer = layers.get(name);
        if (!members.has(layer)) {
            members.set(layer, []);
        }
        members.get(layer).push(name);
    }
    for (const [layer, layerNames] of members) {
        if (layer === 0) {
            layerNames.sort((a, b) => a.localeCompare(b));
            continue;
        }
        const previous = members.get(layer - 1) || [];
        const previousOrder = new Map(previous.map((name, index) => [name, index]));
        const barycentre = (name) => {
            const neighbours = edges.filter((edge) => edge.source === name &&
                layers.get(edge.target) === layer - 1).map((edge) => previousOrder.get(edge.target));
            return neighbours.length
                ? neighbours.reduce((sum, index) => sum + index, 0) / neighbours.length
                : Number.POSITIVE_INFINITY;
        };
        layerNames.sort((a, b) => barycentre(a) - barycentre(b) || a.localeCompare(b));
    }

    const nodes = [];
    for (const [layer, layerNames] of [...members.entries()].sort(([a], [b]) => a - b)) {
        let y = 24;
        layerNames.forEach((name, index) => {
            const table = byName.get(name);
            const height = HEADER_HEIGHT + Math.max((table.columns || []).length, 1) * COLUMN_HEIGHT;
            nodes.push({
                name,
                layer,
                x: 24 + layer * (NODE_WIDTH + NODE_GAP_X),
                y,
                width: NODE_WIDTH,
                height,
                columns: table.columns || [],
                rowEstimate: table.row_estimate ?? 0
            });
            y += height + NODE_GAP_Y;
        });
    }
    const nodeByName = new Map(nodes.map((node) => [node.name, node]));
    const routedEdges = edges.map((edge) => {
        const source = nodeByName.get(edge.source);
        const target = nodeByName.get(edge.target);
        const startX = source.x;
        const startY = source.y + HEADER_HEIGHT + (source.columns.findIndex(
            (column) => column.name === edge.column) + 0.5) * COLUMN_HEIGHT;
        const endX = target.x + target.width;
        const targetColumnIndex = target.columns.findIndex(
            (column) => column.name === edge.targetColumn);
        const endY = target.y + HEADER_HEIGHT + (Math.max(0, targetColumnIndex) + 0.5) * COLUMN_HEIGHT;
        const middleX = Math.floor((startX + endX) / 2);
        return {
            ...edge, startX, startY, endX, endY,
            path: `M ${startX} ${startY} H ${middleX} V ${endY} H ${endX}`
        };
    });
    return { nodes, edges: routedEdges };
}
