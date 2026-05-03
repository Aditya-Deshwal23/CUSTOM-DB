# Engineering Post-Mortem: Technical Challenges in Custom Relational DB Development

This document serves as a high-level technical retrospective for the development of the Custom Relational Database Engine and its associated Web Dashboard. It focuses exclusively on the non-trivial architectural and environmental obstacles encountered during implementation.

---

### 1. C++ Macro Pollution & Crow/ASIO Compilation Failures (Exit Code 2)

**The Difficulty:**
During the integration of the Crow HTTP framework and the ASIO networking library, the compiler (GCC/MinGW) repeatedly exited with `Exit Code 2`. The errors were cryptic, referencing internal ASIO types and `TryAcquireSRWLockExclusive` syntax failures.

Investigation revealed that a legacy polyfill header, `dummy_mutex.h`, contained global macros:
```cpp
#define mutex std::mutex
#define shared_mutex std::shared_mutex
```
These macros leaked into the header-only Crow and ASIO libraries. Because these libraries use their own internal synchronization primitives and sometimes conditionally define their own mutex wrappers, the macro expansion corrupted the library source code, leading to massive redeclaration and syntax errors.

**The Resolution:**
The `dummy_mutex.h` file was gutted of all `#define` macros. The core database engine (Buffer Pool Manager, B+ Tree Index, and Page Latch system) was refactored to use explicit C++17 native types:
- Transitioned all `mutex` references to `std::mutex`.
- Upgraded the concurrency model to use `std::shared_mutex` for Read-Write locking (shared/exclusive access) across the Buffer Pool and B+ Tree root.
- Resulted in a clean compilation of the Crow framework without namespace pollution.

### 2. Cross-Origin Resource Sharing (CORS) & Port Mismatches

**The Difficulty:**
The "Living Dashboard" was hosted via a VS Code Live Server (typically ports 5500 or 5501), while the C++ Database Engine served its API on port 8080. Browser security policies initially blocked all telemetry requests due to CORS (Cross-Origin Resource Sharing) violations. Fetch requests were failing silently or returning `403 Forbidden` on preflight `OPTIONS` requests.

**The Resolution:**
- **Backend:** Integrated the `crow::CORSHandler` middleware into the `ApiServer`. Configured it to explicitly allow the dashboard's origins and permit both `GET` and `POST` methods.
- **Frontend:** Implemented a dynamic `API_BASE_URL` detector in `index.js` that checks `window.location.port`. If the frontend is running on standard dev ports (5500/5501), it automatically targets the absolute URL `http://127.0.0.1:8080`; otherwise, it uses relative paths for production deployment.

### 3. Windows Smart App Control Execution Blocks (0x800711C7)

**The Difficulty:**
Developing a systems-level application on Windows 11 triggered aggressive OS security hurdles. Locally compiled binaries (`relational_db.exe`) were frequently blocked by **Smart App Control** or **Windows Defender** with error `0x800711C7`. Because the binary was unsigned and performed low-level disk I/O and networking, it was flagged as a potential threat.

**The Resolution:**
- Documented the requirement to add the `build/` and `build_cli/` directories to Windows Security exclusion lists.
- Manually unblocked the specific executable in File Properties to bypass the digital signature requirement during the iterative development cycle.

### 4. Frontend Scroll Traps & D3.js Event Hijacking

**The Difficulty:**
The "Immersive Engine Cockpit" initially suffered from a "Scroll Trap" bug. To create a full-screen feel, the layout used `height: 100vh` and `overflow: hidden`. However, the **Relational Schema Topology** (built with SVG/D3 logic) had active zoom/pan event listeners that intercepted the mouse wheel. This prevented the user from scrolling down to see the SQL Console or the Buffer Pool Heatmap, effectively trapping them at the top of the page.

**The Resolution:**
- **CSS:** Refactored the global layout to use `min-height: 100vh` and `overflow-y: auto`, allowing the document to grow naturally.
- **JavaScript:** Completely disabled the D3/Canvas-style zoom and pan listeners within `initPanZoom()`. Hardcoded the Schema Atlas to a fixed height of 450px with a static viewport scale to ensure visibility without hijacking user input.

### 5. State Management Between Live & Simulation Modes

**The Difficulty:**
A critical requirement was for the dashboard to function as a standalone portfolio piece even when the C++ backend was offline. This created a complex state management problem: switching between "Live" and "Simulation" modes often led to "Ghost Timers"—where multiple `setInterval` calls were updating the UI simultaneously, causing flickering metrics and duplicated animations.

**The Resolution:**
- Implemented a robust `toggleMode()` function in `index.js` that uses strict `clearInterval` logic to kill previous telemetry loops before starting new ones.
- Wrapped the telemetry update logic in a `try/catch` block. If the Live Engine becomes unreachable, the system triggers an alert and automatically failsafe-reverts to Simulation Mode, preserving the UX integrity without requiring a page refresh.
