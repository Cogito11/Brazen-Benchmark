#pragma once
#include "../core/BenchmarkManager.h"
#include "../core/HardwareInfo.h"
#include "../core/Log.h"
#include "../core/ScoreCalculator.h"
#include "../core/TestRegistry.h"
#include "../gpu/GpuTestRunner.h"
#include <deque>
#include <map>
#include <string>
#include <vector>

struct ImGuiInputTextCallbackData;

struct GLFWwindow;
struct ImFont;

namespace brazen {

// How a category's selected tests should run when a benchmark is
// started from its tab.
enum class CategoryRunMode { SingleCoreOnly, MultiCoreOnly, Both };

// Top-level sidebar destinations. The sidebar is nav-only now (no
// hardware summary / score / status baked into it). Everything it
// used to show inline now lives in whichever view is the natural home
// for it (hardware -> System Info, composite score -> Results, running
// status -> the status bar under the title, drawn regardless of view).
enum class SidebarView {
    Benchmarks,
    Results,
    Log,
    SystemInfo,
    AppInfo,
    Settings
};

// Tabs inside the Benchmarks view.
enum class BenchmarkTab { Cpu, Gpu, Ram, Ssd };

// Owns all UI-side state and draws the ImGui frame each iteration of the
// main loop. Construction/destruction of the GLFW window + ImGui context
// happens in main.cpp; this class only cares about layout and state.
//
// Top-level layout: a title bar, a narrow nav-only sidebar on the left
// (Benchmarks / Results / Log / System Info / App Info / Settings /
// Quit), and a content panel on the right whose contents depend on
// which sidebar button is selected. Starting a benchmark from the
// Benchmarks view switches the content panel to Results, same spirit as
// the old "force the Results tab" behavior.
class BrazenApp {
public:
    BrazenApp();
    ~BrazenApp();

    // Optional larger font used for the title bar; call once after
    // construction, before the first DrawFrame(). Safe to leave unset.
    void SetTitleFont(ImFont* font) { m_titleFont = font; }

    // GPU testing is driven by main.cpp (it owns the GL context/render
    // loop), not by BrazenApp itself. This just gives the UI a way to
    // request runs and show status. Also copies GPU vendor/renderer into
    // m_hardwareInfo (available once GpuTestRunner::Init() has run) so
    // System Info has a single consistent data source. Safe to leave
    // unset (GPU tab just shows as unavailable).
    void SetGpuRunner(GpuTestRunner* runner);

    // Called by main.cpp when a GPU run (driven on the render thread,
    // outside BenchmarkManager) produces a result.
    void AddExternalResult(const BenchmarkResult& result);

    // Call once per frame, between ImGui::NewFrame() and ImGui::Render().
    void DrawFrame();

    // main.cpp checks this once per frame after DrawFrame() and, if
    // true, is responsible for telling GLFW to close the window.
    // BrazenApp doesn't own the window/GL context so it can't do that
    // itself.
    bool WantsQuit() const { return m_quitRequested; }

private:
    // A single benchmark result plus a wall-clock timestamp of when it
    // was recorded. BenchmarkResult itself (core/BenchmarkRunner.h)
    // deliberately doesn't carry a timestamp. It's a pure measurement
    // record used by console tooling too, so this is a thin UI-side
    // wrapper added purely so the Results view can show "date run".
    struct ResultEntry {
        BenchmarkResult result;
        std::string timestamp;
    };

    // ---- Layout ----
    void DrawTitleBar();
    void DrawStatusBar();
    void DrawSidebar();
    void DrawSidebarButton(const char* label, SidebarView view);

    // ---- Views (drawn in the main content panel) ----
    void DrawBenchmarksView();
    void DrawCpuBenchmarkTab();
    void DrawRamBenchmarkTab();
    void DrawGpuBenchmarkTab();
    void DrawSsdBenchmarkTab();

    void DrawResultsView();
    void DrawResultDetail(const BenchmarkResult& r, size_t index);
    void DrawClearResultsModal();
    void DeleteSelectedResults();
    void ClearAllResults();

    void DrawLogView();
    void DrawConsoleInput();
    void DrawSystemInfoView();
    void DrawAppInfoView();
    void DrawSettingsView();

    // Reusable popup (opened from the Results view and App Info) listing
    // every cat rank and its point threshold.
    void DrawCatRankTablePopup();

    // ---- Small shared helpers ----
    void DrawScoreCard(const char* label, const CompositeScore& score);
    // Draws a small "?" button tied to strId; returns true (and stays
    // true across frames, via ImGui's per-window state storage) once
    // it's been clicked, so the caller can show an inline explanation
    // right where it makes sense rather than in a popup.
    bool DrawHelpButton(const char* strId);
    // Width a button needs to fit `label` without clipping, given the
    // current font/UI scale, floored at minWidth. Fixed pixel widths
    // broke as soon as UI scale or DPI changed the font size (that's
    // why "Reset Defaults" and friends were getting cut off). This
    // measures the actual text instead.
    float AutoButtonWidth(const char* label, float minWidth = 0.0f) const;

    // Re-queries the GPU/drive lists from the OS. Called once at
    // construction and on demand from a "Refresh" button in System
    // Info; drive free space in particular can go stale over a long
    // session.
    void RefreshSystemInventory();

    // ---- Actions (enqueue work based on the relevant *Settings below) ----
    void StartCpuBenchmark();
    void StartRamBenchmark();
    void StartGpuBenchmark();
    void StartSsdBenchmark();
    // Everything that's currently runnable (CPU, RAM, disk if a drive is
    // usable, GPU if supported), each with its tab's saved settings. Used
    // by the console's "run all". The GPU part waits for the queued
    // CPU/RAM/disk jobs to finish so they don't disturb each other.
    void StartEverything();

    // Whether the Disk tab's current selection could be run right now, and
    // if not, a short reason. Shared by the tab's Run button and the console.
    bool CanStartSsd(std::string* whyNot) const;

    // RAM test safety. The multi-core run allocates 2 buffers per thread,
    // so a large buffer on many threads can need many GB. Exceeding free
    // memory doesn't give a clean error: Linux over-commits and then the
    // OOM killer ends the process, Windows starts paging until the machine
    // crawls. These size the run and refuse one that wouldn't fit.
    unsigned RamPlannedThreads() const;
    unsigned long long RamRequiredBytes() const;
    // Largest allocation the RAM test may use right now (a share of
    // *available* memory; of installed memory if availability is unknown).
    // 0 = unknown, i.e. nothing to compare against.
    unsigned long long RamBudgetBytes() const;
    bool CanStartRam(std::string* whyNot) const;
    // Bytes of free space the current Disk settings need on the target drive.
    unsigned long long SsdRequiredBytes() const;

    void PollManager();
    void IngestLogEntries();
    // Cancels the worker queue, any running GPU run, and a pending "run all" GPU step.
    void CancelEverything();

    // ---- Startup / inventory logging ----
    void LogHardwareSummary() const;

    // ---- Console (typed commands in the Log view) ----
    void ExecuteCommand(const std::string& line);
    void ConsolePrint(const char* fmt, ...) BRAZEN_PRINTF_LIKE(2, 3);
    void PrintSystemSummary();
    void PrintDrives();
    void PrintResults();
    void PrintStatus();
    static int ConsoleInputCallback(ImGuiInputTextCallbackData* data);
    void AddResultEntry(const BenchmarkResult& result);

    BenchmarkManager m_manager;
    HardwareInfo m_hardwareInfo;
    ImFont* m_titleFont = nullptr;
    GpuTestRunner* m_gpuRunner = nullptr;

    // OS-level GPU/drive enumeration, cached rather than re-queried
    // every frame (drive enumeration in particular walks the
    // filesystem). See RefreshSystemInventory().
    std::vector<GpuInfo> m_allGpus;
    std::vector<DriveInfo> m_allDrives;

    SidebarView m_currentView = SidebarView::Benchmarks;
    SidebarView m_lastDrawnView = SidebarView::Benchmarks;
    BenchmarkTab m_currentBenchmarkTab = BenchmarkTab::Cpu;

    // testIndex -> selected, indexed against TestRegistry (CPU + RAM
    // tests only. GPU isn't part of that registry, see GpuTestRunner.h
    // for why). Rendered as checkboxes on the CPU tab (RAM currently has
    // only one test, so it's always run).
    std::vector<bool> m_selected;

    std::vector<ResultEntry> m_resultEntries;
    // Parallel to m_resultEntries: which rows are checked for bulk
    // deletion via "Delete Selected".
    std::vector<bool> m_resultSelected;
    // Index into m_resultEntries whose detail is shown below the
    // results table; -1 means nothing is expanded.
    int m_expandedResultIndex = -1;

    // Latest completed result per test name, split by run mode, used to
    // compute the composite score/rank as tests finish. GPU results are
    // intentionally never added here. The composite score is
    // CPU-focused, and mixing GFLOPS into that average wouldn't mean
    // anything. Independent of m_resultEntries so deleting individual
    // history rows doesn't change the composite score; "Clear All"
    // resets both for a genuinely clean slate.
    std::map<std::string, BenchmarkResult> m_latestSingleCore;
    std::map<std::string, BenchmarkResult> m_latestMultiCore;

    // One rendered log line. `text` is pre-formatted at ingest time
    // ("12:00:01.123 INFO  [Hardware] ...") so drawing thousands of lines
    // per frame doesn't mean formatting thousands of strings per frame.
    struct LogLine {
        LogLevel level = LogLevel::Info;
        std::string text;
    };
    std::deque<LogLine> m_log;
    static constexpr size_t kMaxLogLines = 3000;

    // Log view state.
    bool m_logShowInfo = true;
    bool m_logShowWarn = true;
    bool m_logShowError = true;
    bool m_logJumpToBottom = true; // set when the view is opened so it lands on the newest line

    // Console state.
    char m_consoleInput[256] = {};
    std::vector<std::string> m_commandHistory;
    int m_historyPos = -1; // -1 = editing a fresh line; else index into m_commandHistory
    bool m_focusConsole = false;
    // "run all" queues CPU/RAM/disk on the worker; the GPU part is started
    // by PollManager() once that queue has drained.
    bool m_gpuPendingAfterQueue = false;

    bool m_quitRequested = false;

    // Settings-view toggle: whether "Clear All Results History" asks
    // for confirmation first.
    bool m_confirmBeforeClear = true;

    // Per-category settings, each remembered independently between tab
    // switches. A quick CPU sanity check and a long RAM soak test can
    // coexist without one overwriting the other's duration.
    struct CpuSettings {
        float durationSeconds = 8.0f;
        CategoryRunMode mode = CategoryRunMode::Both;
    } m_cpuSettings;

    struct RamSettings {
        float durationSeconds = 8.0f;
        CategoryRunMode mode = CategoryRunMode::Both;
        int bufferSizeIndex = 2;      // into kRamBufferSizesMB -> default 32 MB (today's original default)
        bool autoThreadCount = true;
        int threadCountOverride = 1;
    } m_ramSettings;

    struct GpuSettings {
        float durationSeconds = 8.0f;
        int resolutionIndex = 1;      // into kGpuResolutions -> default 512x512 (today's original default)
        bool runAlu = true;
        bool runTexture = true;
        bool runFill = true;
        // Index into the GPU tab's combo box: 0 is always "the active
        // GPU" (a synthetic entry, not part of m_allGpus); indices 1+
        // map to m_allGpus[index - 1]. Only index 0 can actually be run.
        // See DrawGpuBenchmarkTab for why.
        int selectedComboIndex = 0;
    } m_gpuSettings;

    // No duration and no chunk size: the disk test moves a fixed amount of
    // data and runs until that's done (see DiskIoTest), so the only size
    // knob is how much data to use.
    struct SsdSettings {
        CategoryRunMode mode = CategoryRunMode::SingleCoreOnly;
        int testSizeIndex = 1;        // into kSsdTestSizesMB -> default 1 GB per direction
        bool runWrite = true;
        bool runRead = true;
        // Index into m_allDrives of the drive to benchmark; -1 until a
        // drive with a usable path has been found/chosen.
        int selectedDriveIndex = -1;
    } m_ssdSettings;
};

} // namespace brazen
