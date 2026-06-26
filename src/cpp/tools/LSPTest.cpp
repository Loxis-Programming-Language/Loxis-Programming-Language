// LSPTest — build Loxis LSP, install VS Code extension, open test/main.lx in VS Code.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ============================================================
// Process helpers (Windows)
// ============================================================

#ifdef _WIN32

static int runProcess(const char* app, std::string cmdLine, const fs::path& cwd, bool quiet) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (quiet) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};

    std::vector<char> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back('\0');

    const std::string cwdStr = cwd.string();
    if (!CreateProcessA(app, buf.data(), nullptr, nullptr, FALSE,
                        quiet ? CREATE_NO_WINDOW : 0,
                        nullptr, cwdStr.c_str(), &si, &pi)) {
        std::cerr << "CreateProcess failed: " << cmdLine << "\n";
        return -1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
}

// Run a shell command via cmd.exe /c (for pnpm, powershell, etc.)
static int runCommand(const std::string& cmd, const fs::path& cwd, bool quiet = false) {
    return runProcess("C:\\Windows\\System32\\cmd.exe", "/c " + cmd, cwd, quiet);
}

static std::string quote(const std::string& s) {
    return "\"" + s + "\"";
}

static fs::path getExePath() {
    std::vector<char> buf(512);
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0) throw std::runtime_error("GetModuleFileName failed");
    if (n >= buf.size()) {
        buf.resize(4096);
        n = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    }
    return fs::path(std::string(buf.data(), n));
#else
    return fs::absolute(fs::path("LSPTest"));
#endif
}

static fs::path findRepoRoot() {
    if (const char* env = std::getenv("LOXIS_ROOT")) {
        return fs::path(env);
    }
    auto p = getExePath().parent_path();
    if (p.filename() == "Debug" || p.filename() == "Release") p = p.parent_path();
    if (p.filename() == "build" || p.filename() == "cmake-build-debug") return p.parent_path();
    if (fs::exists(p / "CMakeLists.txt")) return p;
    return fs::path("D:/Development/Loxis");
}

static fs::path findLspRoot(const fs::path& loxisRoot) {
    if (const char* env = std::getenv("LOXIS_LSP_ROOT")) {
        return fs::path(env);
    }
    const fs::path sibling = loxisRoot.parent_path() / "Loxis-LSP";
    if (fs::exists(sibling)) return sibling;
    return fs::path("D:/Development/Loxis-LSP");
}

static fs::path findMainLx(const fs::path& loxisRoot) {
    const std::vector<fs::path> candidates = {
        loxisRoot / "src" / "test" / "main.lx",
        loxisRoot / "src" / "test" / "pkgdemo" / "main.lx",
    };
    for (const auto& c : candidates) {
        if (fs::exists(c)) return fs::absolute(c);
    }
    throw std::runtime_error(
        "main.lx not found under src/test (tried main.lx and pkgdemo/main.lx)");
}

static std::string findCodeExe() {
    if (const char* env = std::getenv("LOXIS_CODE")) return env;

    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) {
        const std::vector<fs::path> guesses = {
            fs::path(localAppData) / "Programs" / "Microsoft VS Code" / "Code.exe",
            fs::path(localAppData) / "Programs" / "Microsoft VS Code" / "bin" / "code.cmd",
        };
        for (const auto& g : guesses) {
            if (fs::exists(g)) return g.string();
        }
    }

    FILE* pipe = _popen("where code.cmd 2>nul", "r");
    if (pipe) {
        char line[512]{};
        if (fgets(line, sizeof(line), pipe)) {
            _pclose(pipe);
            std::string p = line;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
            if (!p.empty() && fs::exists(p)) return p;
        } else {
            _pclose(pipe);
        }
    }

    throw std::runtime_error(
        "VS Code not found. Install VS Code or set LOXIS_CODE to Code.exe / code.cmd path.");
}

static std::string findPnpm() {
    if (const char* env = std::getenv("LOXIS_PNPM")) return quote(env);
    FILE* pipe = _popen("where pnpm 2>nul", "r");
    if (pipe) {
        char line[512]{};
        if (fgets(line, sizeof(line), pipe)) {
            _pclose(pipe);
            std::string p = line;
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
            if (!p.empty()) return quote(p);
        } else {
            _pclose(pipe);
        }
    }
    return "pnpm";
}

static void step(const char* label) {
    std::cerr << "==> " << label << "\n";
}

static void printUsage(const char* exe) {
    std::cerr <<
        "Usage: " << exe << " [options]\n"
        "\n"
        "Build Loxis LSP, install the VS Code extension, open src/test/**/main.lx.\n"
        "\n"
        "Options:\n"
        "  --no-open     Build + install only, do not launch VS Code\n"
        "  --no-build    Skip pnpm build (use existing dist/)\n"
        "  -h, --help    Show help\n"
        "\n"
        "Environment:\n"
        "  LOXIS_ROOT      Loxis repo root (default: auto-detect)\n"
        "  LOXIS_LSP_ROOT  Loxis-LSP repo root\n"
        "  LOXIS_CODE      Path to code.cmd / Code.exe\n"
        "  LOXIS_PNPM      Path to pnpm\n";
}

int main(int argc, char* argv[]) {
    try {
        bool openVsCode = true;
        bool doBuild = true;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-h" || arg == "--help") { printUsage(argv[0]); return 0; }
            if (arg == "--no-open") openVsCode = false;
            else if (arg == "--no-build") doBuild = false;
            else {
                std::cerr << "Unknown option: " << arg << "\n";
                printUsage(argv[0]);
                return 1;
            }
        }

        const fs::path loxisRoot = findRepoRoot();
        const fs::path lspRoot = findLspRoot(loxisRoot);
        const fs::path mainLx = findMainLx(loxisRoot);
        const fs::path installPs1 = lspRoot / "vscode-extension" / "install.ps1";

        if (!fs::exists(lspRoot)) {
            std::cerr << "Loxis-LSP not found: " << lspRoot << "\n";
            return 1;
        }
        if (!fs::exists(installPs1)) {
            std::cerr << "install.ps1 not found: " << installPs1 << "\n";
            return 1;
        }

        std::cerr << "Loxis root:  " << fs::absolute(loxisRoot) << "\n";
        std::cerr << "LSP root:    " << fs::absolute(lspRoot) << "\n";
        std::cerr << "Open file:   " << mainLx << "\n\n";

        if (doBuild) {
            step("pnpm install (if needed)");
            const std::string pnpm = findPnpm();
            int rc = runCommand(pnpm + " install", lspRoot, true);
            if (rc != 0) std::cerr << "warning: pnpm install exited " << rc << "\n";

            step("pnpm run build");
            rc = runCommand(pnpm + " run build", lspRoot, false);
            if (rc != 0) {
                std::cerr << "error: LSP build failed (exit " << rc << ")\n";
                return 1;
            }
        } else {
            step("skip build (--no-build)");
            if (!fs::exists(lspRoot / "dist" / "server.bundle.js")) {
                std::cerr << "error: dist/server.bundle.js missing\n";
                return 1;
            }
        }

        step("install VS Code extension");
        std::ostringstream ps;
        ps << "powershell -NoProfile -ExecutionPolicy Bypass -File "
           << quote(installPs1.string());
        int rc = runCommand(ps.str(), lspRoot, false);
        if (rc != 0) {
            std::cerr << "error: extension install failed (exit " << rc << ")\n";
            return 1;
        }

        if (!openVsCode) {
            std::cerr << "\nDone. Extension installed. Open " << mainLx << " in VS Code manually.\n";
            return 0;
        }

        step("launch VS Code");
        const std::string code = findCodeExe();
        std::string cmdLine = quote(code) + " -n -g " + quote(mainLx.string());
        rc = runProcess(nullptr, std::move(cmdLine), loxisRoot, false);
        if (rc != 0) {
            std::cerr << "warning: code exited " << rc << " (file may still have opened)\n";
        }

        std::cerr << "\nDone. VS Code should show " << mainLx.filename().string()
                  << " with Loxis LSP active.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "LSPTest error: " << e.what() << "\n";
        return 2;
    }
}

#else

int main() {
    std::cerr << "LSPTest is Windows-only (builds VS Code extension).\n";
    return 1;
}

#endif