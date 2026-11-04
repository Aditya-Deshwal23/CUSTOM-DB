# CUSTOM-DB Studio UI design

Studio is a database workbench, not a telemetry landing page. The Explorer is
the persistent navigation rail; a table opens one of its Data, Structure,
Indexes, Constraints, DDL, Storage, or Relations views in the work area. The
ER diagram and system inspectors use the same work area. Existing SQL and
engine-monitoring surfaces remain available below it for compatibility.

## Interaction model

- Catalog data comes from one `GET /api/v1/catalog` response. Revalidation uses
  its ETag, on demand with F5, after DDL, and at a ten-second interval.
- Table rows come from the bounded keyset endpoint. The browser sends the
  structured filter JSON as the endpoint specifies; it never composes SQL from
  filter values.
- All API-provided names and values are rendered as text, not markup.
- Copy actions may write to the same-origin clipboard; clipboard reads remain
  disabled by the browser Permissions Policy.
- Writes are unavailable until the snapshot confirms the engine is writable.
  Cell edits require a primary key and are reviewed as parameterized statements
  before one atomic batch is sent.
- Unknown, unavailable, and estimated data stays explicitly labeled. The UI
  does not synthesize row counts, storage measurements, or metrics.
- The grid keeps a bounded number of row elements in the DOM. Data pages are
  capped at 500 rows; exporting all rows has an explicit 5,000-row cap and
  cancellation.

## Keyboard model

- Explorer: Tab enters the object list; Up/Down moves between visible table
  items; Right/Left expands/collapses a table; Enter opens its data view.
  Context menus support Up/Down and Escape.
- Grid: focus the grid, then use Up/Down/Left/Right, Home/End, and PageUp/
  PageDown. Shift-click extends a rectangular cell selection; Ctrl/Cmd+C copies
  it as TSV. Double-click begins an editable cell when writes are allowed.
- ER diagram: Tab reaches table nodes; Enter or Space opens the focused table.

Every grid row, cell, and header exposes the corresponding ARIA grid role and
index. The Explorer and table views are exercised with real API data in the
Playwright suite; the pure filter, export, layout, and keyboard helpers have
dependency-light Node tests.
