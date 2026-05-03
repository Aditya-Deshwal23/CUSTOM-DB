# Relational Storage Engine: Build & Run Instructions


Follow these steps to compile and launch the C++ storage engine backend and the observability dashboard.

## 1. Prerequisites
Ensure you have the following installed:
- **CMake** (v3.14+)
- **GCC** (v15+ recommended for full C++17/20 support) or **Visual Studio 2022**
- **MSYS2** (if on Windows, for modern GCC/MinGW)

## 2. Compilation (C++ Backend)

Open your terminal in the project root and execute:

```bash
# 1. Create and enter build directory
mkdir build
cd build

# 2. Configure the project
# On Windows with MSYS2:
cmake .. -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe

# 3. Build the executable
cmake --build . --config Release
```

## 3. Launching the Engine

Once built, run the executable:

```bash
# From the build directory
./relational_db.exe
```

The engine will initialize and start the **Crow HTTP Server** on **port 8080**. It also starts a background simulation thread that processes random SELECT and INSERT operations.

## 4. Accessing the Dashboard

### Option A: Direct (via Crow)
The C++ server itself serves the dashboard. Open:
[http://localhost:8080](http://localhost:8080)

### Option B: Local Simulation (Resume/Demo Mode)
If you just want to demo the frontend without the C++ engine running, open `dashboard/index.html` using the **Live Server** extension in VS Code or any simple HTTP server:

```bash
cd dashboard
python -m http.server 5500
```
Then visit [http://localhost:5500](http://localhost:5500). The UI will automatically detect the missing backend and switch to **Local Simulation Mode**, populating all metrics and the schema topology with realistic mock data.

## 5. Console Commands
Try entering these commands in the dashboard SQL Console to see execution traces:
- `SELECT * FROM Users`
- `INSERT 42 100`
- `SELECT 10`


