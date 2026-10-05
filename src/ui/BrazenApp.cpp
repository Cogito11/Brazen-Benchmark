#include "BrazenApp.h"

// Supplied by the build (see CMakeLists.txt's BRAZEN_VERSION, set from
// the pushed git tag in .github/workflows/release.yml, or a rolling
// dev-build label otherwise). This fallback only kicks in for builds
// that don't go through that CMake target at all (e.g. compiling this
// file directly from an IDE project), so App Info always has something
// sensible to show rather than failing to compile.
#ifndef BRAZEN_VERSION
#define BRAZEN_VERSION "0.0.0-dev"
#endif
#include "../core/Log.h"
#include "../core/RegisterTests.h"
#include "../core/tests/DiskIoTest.h"
#include "../core/tests/RamBandwidthTest.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <string>

namespace brazen {
namespace {

const char* ModeLabel(RunMode mode) {
    switch (mode) {
        case RunMode::SingleCore: return "Single-Core";
        case RunMode::MultiCore:  return "Multi-Core";
        case RunMode::Gpu:        return "GPU";
    }
    return "?";
}

// RAM buffer size presets. 32 MB (index 2) matches the RAM test's
// original hardcoded default, kept as the default here so behavior
// doesn't silently change for anyone who never opens this setting.
constexpr int kRamBufferSizesMB[] = {8, 16, 32, 64, 128};
const char* kRamBufferSizeLabels[] = {"8 MB", "16 MB", "32 MB", "64 MB", "128 MB"};

// GPU render resolution presets. 512x512 (index 1) matches the GPU
// test's original hardcoded default.
constexpr int kGpuResolutions[][2] = {{256, 256}, {512, 512}, {1024, 1024}, {2048, 2048}};
const char* kGpuResolutionLabels[] = {"256 x 256 (fastest)", "512 x 512 (default)",
                                       "1024 x 1024", "2048 x 2048 (slowest)"};

// Disk test size presets: how much data is written, and then read back,
// per direction. The disk test runs until that much data has been moved,
// so this (not a duration) is what decides how long it takes. 1 GB
// (index 1) is DiskIoTest's own default.
constexpr int kSsdTestSizesMB[] = {512, 1024, 2048, 4096};
const char* kSsdTestSizeLabels[] = {"512 MB", "1 GB (default)", "2 GB", "4 GB"};
constexpr unsigned kMaxDiskBenchmarkThreads = 4;
// Always leave at least this much free on the drive being tested (the
// disk test enforces the same figure again when it starts).
constexpr unsigned long long kDiskFreeSpaceReserve = DiskIoTest::kFreeSpaceReserveBytes;
// The RAM test may use at most this share of the memory that is free
// (or of installed memory when free memory can't be determined).
constexpr double kRamBudgetShareOfAvailable = 0.60;
constexpr double kRamBudgetShareOfInstalled = 0.50;

// Worker threads for a "use every core" run: the cores this process may
// really use (can be fewer than the machine has in a container or under
// taskset), never fewer than one.
unsigned UsableCoreCount() {
    return std::max<unsigned>(1u, static_cast<unsigned>(AllowedCores().size()));
}

const char* CategoryModeLabel(CategoryRunMode mode) {
    switch (mode) {
        case CategoryRunMode::SingleCoreOnly: return "single-core";
        case CategoryRunMode::MultiCoreOnly:  return "multi-core";
        case CategoryRunMode::Both:           return "single- and multi-core";
    }
    return "?";
}

const char* ResultStatusLabel(const BenchmarkResult& r) {
    if (r.failed) return "Failed";
    if (r.cancelled) return "Cancelled";
    return "Done";
}

std::string NowTimestampString() {
    std::time_t t = std::time(nullptr);
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return buf;
}

} // namespace

BrazenApp::BrazenApp() {
    LogInfo("Init", "Initializing Brazen...");
    RegisterBuiltInTests();
    m_selected.assign(TestRegistry::Instance().Count(), false);
    // Default every test selected: selection on the CPU tab *is* the
    // primary way to choose what runs, so "Run Benchmark" with the
    // tab's defaults untouched should do the obviously-expected thing
    // (run everything in that category).
    std::fill(m_selected.begin(), m_selected.end(), true);
    LogInfo("Init", "Registered %zu CPU/RAM benchmark tests.", TestRegistry::Instance().Count());

    LogInfo("Hardware", "Detecting hardware...");
    m_hardwareInfo = QueryHardwareInfo();
    LogHardwareSummary();
    RefreshSystemInventory();
    LogInfo("Init", "Initialization complete. Type 'help' below for console commands.");
}

BrazenApp::~BrazenApp() = default;

void BrazenApp::SetGpuRunner(GpuTestRunner* runner) {
    m_gpuRunner = runner;
    if (!m_gpuRunner) {
        LogWarn("GPU", "No GPU test runner attached; GPU benchmarking is unavailable.");
        return;
    }
    if (m_gpuRunner->IsSupported()) {
        m_hardwareInfo.gpuVendor = m_gpuRunner->GetVendor();
        m_hardwareInfo.gpuRenderer = m_gpuRunner->GetRenderer();
        LogInfo("GPU", "GPU benchmarking ready on: %s (%s).", m_hardwareInfo.gpuRenderer.c_str(),
                m_hardwareInfo.gpuVendor.c_str());
    } else {
        LogWarn("GPU", "GPU benchmarking is unavailable: %s", m_gpuRunner->GetError().c_str());
    }
}

void BrazenApp::LogHardwareSummary() const {
    const HardwareInfo& hw = m_hardwareInfo;
    if (hw.cpuMaxFrequencyGHz > 0.0) {
        LogInfo("Hardware", "CPU: %s - %u cores / %u threads, up to %.2f GHz", hw.cpuModel.c_str(),
                hw.physicalCores, hw.logicalCores, hw.cpuMaxFrequencyGHz);
    } else {
        LogInfo("Hardware", "CPU: %s - %u cores / %u threads", hw.cpuModel.c_str(), hw.physicalCores,
                hw.logicalCores);
    }
    if (hw.cpuIdentifier != "Unknown")
        LogInfo("Hardware", "CPU identifier: %s", hw.cpuIdentifier.c_str());
    LogInfo("Hardware", "CPU cache: L1 data %s, L1 instruction %s, L2 %s, L3 %s",
            hw.cpuCache.l1Data.c_str(), hw.cpuCache.l1Instruction.c_str(), hw.cpuCache.l2.c_str(),
            hw.cpuCache.l3.c_str());
    if (hw.cpuInstructionSets != "Unknown")
        LogInfo("Hardware", "CPU instruction sets: %s", hw.cpuInstructionSets.c_str());
    if (hw.totalRamBytes > 0)
        LogInfo("Hardware", "RAM: %s installed", FormatBytesAsGB(hw.totalRamBytes).c_str());
    else
        LogWarn("Hardware", "RAM: total capacity could not be determined on this system.");
    LogInfo("Hardware", "OS: %s (%s)", hw.osVersion.c_str(), hw.kernelVersion.c_str());
    if (hw.systemVendor != "Unknown" || hw.systemModel != "Unknown")
        LogInfo("Hardware", "System: %s %s", hw.systemVendor.c_str(), hw.systemModel.c_str());
    if (hw.motherboardVendor != "Unknown" || hw.motherboardModel != "Unknown")
        LogInfo("Hardware", "Motherboard: %s %s", hw.motherboardVendor.c_str(), hw.motherboardModel.c_str());
    if (hw.biosVendor != "Unknown" || hw.biosVersion != "Unknown")
        LogInfo("Hardware", "BIOS: %s %s", hw.biosVendor.c_str(), hw.biosVersion.c_str());
}

void BrazenApp::RefreshSystemInventory() {
    LogInfo("Hardware", "Scanning for GPUs and drives...");
    m_allGpus = QueryAllGpus();
    m_allDrives = QueryAllDrives();

    if (m_allGpus.empty()) {
        LogWarn("Hardware", "The OS did not report any GPUs.");
    } else {
        LogInfo("Hardware", "GPUs reported by the OS: %zu", m_allGpus.size());
        for (size_t i = 0; i < m_allGpus.size(); ++i)
            LogInfo("Hardware", "  GPU %zu: %s", i + 1, m_allGpus[i].name.c_str());
    }

    // Point the SSD tab's drive selection at something sane by default:
    // prefer the primary drive (the one holding the app's working
    // directory) if it's actually writable, otherwise the first
    // writable drive found, otherwise leave it unselected (-1) so the
    // tab can say plainly that nothing testable was detected.
    m_ssdSettings.selectedDriveIndex = -1;
    for (size_t i = 0; i < m_allDrives.size(); ++i) {
        if (m_allDrives[i].path.empty()) continue;
        if (m_allDrives[i].isPrimary) {
            m_ssdSettings.selectedDriveIndex = static_cast<int>(i);
            break;
        }
        if (m_ssdSettings.selectedDriveIndex < 0)
            m_ssdSettings.selectedDriveIndex = static_cast<int>(i);
    }

    if (m_allDrives.empty()) {
        LogWarn("Hardware", "No drives were detected.");
    } else {
        LogInfo("Hardware", "Drives detected: %zu", m_allDrives.size());
        for (size_t i = 0; i < m_allDrives.size(); ++i) {
            const DriveInfo& d = m_allDrives[i];
            if (d.path.empty()) {
                LogInfo("Hardware", "  Drive %zu: %s - no writable location found, can't be used for the disk test",
                        i + 1, d.label.c_str());
            } else {
                LogInfo("Hardware", "  Drive %zu: %s - %s free of %s, test files go in %s%s", i + 1,
                        d.label.c_str(), FormatBytesAsGB(d.freeBytes).c_str(),
                        FormatBytesAsGB(d.totalBytes).c_str(), d.path.c_str(),
                        d.isPrimary ? " (holds the app's working directory)" : "");
            }
        }
    }
    if (m_ssdSettings.selectedDriveIndex >= 0) {
        const DriveInfo& d = m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)];
        LogInfo("Hardware", "Disk test will use drive %d (%s) unless you pick another.",
                m_ssdSettings.selectedDriveIndex + 1, d.label.c_str());
    } else {
        LogWarn("Hardware", "No writable drive is available for the disk test.");
    }
}

float BrazenApp::AutoButtonWidth(const char* label, float minWidth) const {
    float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFontSize();
    return std::max(width, minWidth);
}

void BrazenApp::AddExternalResult(const BenchmarkResult& result) {
    // GPU runs don't go through BenchmarkManager (which logs its own
    // results), so their outcome is logged here.
    if (result.failed) {
        // already logged as an error by GpuTestRunner::FailRun / the zero-data check
        if (result.notes.find("without producing any timing data") != std::string::npos)
            LogError("GPU", "%s failed: %s", result.testName.c_str(), result.notes.c_str());
    } else if (result.cancelled) {
        LogInfo("GPU", "%s cancelled after %.1f s.", result.testName.c_str(), result.elapsedSeconds);
    } else {
        LogInfo("GPU", "%s finished in %.1f s: %.2f %s", result.testName.c_str(), result.elapsedSeconds,
                result.score, result.unit.c_str());
    }
    AddResultEntry(result);
}

void BrazenApp::AddResultEntry(const BenchmarkResult& result) {
    m_resultEntries.push_back({result, NowTimestampString()});
    m_resultSelected.push_back(false);
}

void BrazenApp::PollManager() {
    for (auto& r : m_manager.DrainResults()) {
        AddResultEntry(r);
        if (!r.cancelled && !r.failed) {
            auto& latestMap = (r.mode == RunMode::SingleCore) ? m_latestSingleCore : m_latestMultiCore;
            latestMap[r.testName] = r;
        }
    }

    // "run all" holds the GPU step back until the worker queue has fully
    // drained, so GPU work never overlaps CPU/RAM/disk work.
    if (m_gpuPendingAfterQueue && m_manager.IsIdle() && !(m_gpuRunner && m_gpuRunner->IsBusy())) {
        m_gpuPendingAfterQueue = false;
        LogInfo("Run", "CPU/RAM/disk runs are finished; starting the GPU benchmark now.");
        StartGpuBenchmark();
    }

    IngestLogEntries();
}

void BrazenApp::IngestLogEntries() {
    for (auto& e : Logger::Instance().Drain()) {
        LogLine line;
        line.level = e.level;
        char level[8];
        std::snprintf(level, sizeof(level), "%-5s", ToString(e.level));
        line.text = e.timestamp + "  " + level + "  [" + e.category + "] " + e.message;
        m_log.push_back(std::move(line));
    }
    while (m_log.size() > kMaxLogLines) m_log.pop_front();
}

void BrazenApp::CancelEverything() {
    m_gpuPendingAfterQueue = false;
    m_manager.CancelAll();
    if (m_gpuRunner && m_gpuRunner->IsBusy()) {
        m_gpuRunner->Cancel();
        LogInfo("GPU", "Cancel requested for the running GPU benchmark.");
    }
}

void BrazenApp::DrawFrame() {
    PollManager();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags rootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("BrazenRoot", nullptr, rootFlags);

    if (m_currentView == SidebarView::Log && m_lastDrawnView != SidebarView::Log)
        m_logJumpToBottom = true; // land on the newest line when the view is opened
    m_lastDrawnView = m_currentView;

    DrawTitleBar();
    DrawStatusBar();
    ImGui::Spacing();

    // Sidebar width scales with the current font size (which already
    // reflects both monitor DPI and the in-app UI-scale slider) instead
    // of a fixed pixel count. It's nav-only now (just seven buttons), so
    // it can be considerably narrower than the old hardware-summary
    // sidebar.
    float sidebarWidth = ImGui::GetFontSize() * 13.0f;
    sidebarWidth = std::max(170.0f, std::min(sidebarWidth, 300.0f));

    ImGui::BeginChild("Left", ImVec2(sidebarWidth, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawSidebar();
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("Right", ImVec2(0, 0), true);
    switch (m_currentView) {
        case SidebarView::Benchmarks: DrawBenchmarksView(); break;
        case SidebarView::Results:    DrawResultsView();    break;
        case SidebarView::Log:        DrawLogView();        break;
        case SidebarView::SystemInfo: DrawSystemInfoView(); break;
        case SidebarView::AppInfo:    DrawAppInfoView();    break;
        case SidebarView::Settings:   DrawSettingsView();   break;
    }
    ImGui::EndChild();

    ImGui::End();
}

void BrazenApp::DrawTitleBar() {
    float barHeight = ImGui::GetFontSize() * 2.6f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.12f, 0.18f, 1.0f));
    ImGui::BeginChild("TitleBar", ImVec2(0, barHeight), false, ImGuiWindowFlags_NoScrollbar);

    float titleFontSize = m_titleFont ? m_titleFont->FontSize : ImGui::GetFontSize();
    ImGui::SetCursorPos(ImVec2(18.0f, (barHeight - titleFontSize) * 0.5f));
    if (m_titleFont) ImGui::PushFont(m_titleFont);
    ImGui::TextColored(ImVec4(0.96f, 0.79f, 0.30f, 1.0f), "BRAZEN BENCHMARK");
    if (m_titleFont) ImGui::PopFont();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// A slim strip under the title bar showing what's currently running,
// visible from any view (not just Results) with a one-click way to
// cancel. Carries over the old sidebar "Status" section's job now that
// the sidebar itself is nav-only.
void BrazenApp::DrawStatusBar() {
    bool busy = m_manager.IsBusy();
    bool gpuBusy = m_gpuRunner && m_gpuRunner->IsBusy();
    if (!busy && !gpuBusy) return;

    float barHeight = ImGui::GetFontSize() * 1.9f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.15f, 0.05f, 1.0f));
    ImGui::BeginChild("StatusBar", ImVec2(0, barHeight), false, ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPosY((barHeight - ImGui::GetFontSize()) * 0.5f);
    ImGui::SetCursorPosX(14.0f);
    if (busy) {
        BenchmarkManager::RunProgress progress = m_manager.GetCurrentProgress();
        if (progress.busy && progress.fixedWork) {
            // Runs until its work is done (e.g. disk): there is no fixed
            // duration to count down, so show what it's doing, how far
            // along it is if it knows, and how long it has been going.
            const ImVec4 gold(1.0f, 0.8f, 0.2f, 1.0f);
            ImGui::TextColored(gold, "%s: %s", progress.testName.c_str(),
                                progress.phase.empty() ? "running" : progress.phase.c_str());
            ImGui::SameLine();
            if (progress.fraction >= 0.0) {
                ImGui::ProgressBar(static_cast<float>(progress.fraction),
                                    ImVec2(ImGui::GetFontSize() * 9.0f, ImGui::GetFontSize()), "");
                ImGui::SameLine();
                ImGui::TextColored(gold, "%.0f%%  |  %.0f s elapsed", progress.fraction * 100.0,
                                    progress.elapsedSeconds);
            } else {
                ImGui::TextColored(gold, "%.0f s elapsed", progress.elapsedSeconds);
            }
        } else if (progress.busy) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s: %.1fs / %.0fs",
                                progress.testName.c_str(), progress.elapsedSeconds, progress.durationSeconds);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", m_manager.CurrentlyRunning().c_str());
        }
        ImGui::SameLine();
    }
    if (gpuBusy) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "GPU: %s", m_gpuRunner->StatusText().c_str());
        ImGui::SameLine();
    }
    size_t queued = m_manager.QueueSize();
    if (queued > 0) {
        ImGui::TextDisabled("(%zu more queued)", queued);
        ImGui::SameLine();
    }

    float btnWidth = ImGui::CalcTextSize("Cancel All").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - btnWidth - 14.0f);
    ImGui::SetCursorPosY((barHeight - ImGui::GetFontSize()) * 0.5f);
    if (ImGui::SmallButton("Cancel All"))
        CancelEverything();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void BrazenApp::DrawSidebar() {
    ImGui::Spacing();
    DrawSidebarButton("Benchmarks", SidebarView::Benchmarks);
    DrawSidebarButton("Results", SidebarView::Results);
    DrawSidebarButton("Log & Console", SidebarView::Log);
    DrawSidebarButton("System Info", SidebarView::SystemInfo);
    DrawSidebarButton("App Info", SidebarView::AppInfo);
    DrawSidebarButton("Settings", SidebarView::Settings);

    // Anchor Quit to the bottom of the sidebar, separated from
    // navigation, so it reads as a distinct, deliberate action rather
    // than one more view to click through.
    float remaining = ImGui::GetContentRegionAvail().y;
    float quitBlockHeight = ImGui::GetFontSize() * 2.6f;
    if (remaining > quitBlockHeight)
        ImGui::Dummy(ImVec2(0, remaining - quitBlockHeight));

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
    if (ImGui::Button("Quit", ImVec2(-1, ImGui::GetFontSize() * 2.0f)))
        m_quitRequested = true;
    ImGui::PopStyleColor(3);
}

void BrazenApp::DrawSidebarButton(const char* label, SidebarView view) {
    bool active = (m_currentView == view);
    if (active)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    if (ImGui::Button(label, ImVec2(-1, ImGui::GetFontSize() * 2.0f)))
        m_currentView = view;
    if (active)
        ImGui::PopStyleColor();
    ImGui::Spacing();
}

bool BrazenApp::DrawHelpButton(const char* strId) {
    ImGui::PushID(strId);
    ImGuiID key = ImGui::GetID("##open");
    ImGuiStorage* storage = ImGui::GetStateStorage();
    bool open = storage->GetBool(key, false);
    ImGui::SameLine();
    if (ImGui::SmallButton("?")) {
        open = !open;
        storage->SetBool(key, open);
    }
    ImGui::PopID();
    return open;
}

// ---------------------------------------------------------------------
// Benchmarks view
// ---------------------------------------------------------------------

void BrazenApp::DrawBenchmarksView() {
    ImGui::Spacing();
    if (ImGui::BeginTabBar("BenchmarkTabs")) {
        if (ImGui::BeginTabItem("CPU")) {
            m_currentBenchmarkTab = BenchmarkTab::Cpu;
            DrawCpuBenchmarkTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("GPU")) {
            m_currentBenchmarkTab = BenchmarkTab::Gpu;
            DrawGpuBenchmarkTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("RAM")) {
            m_currentBenchmarkTab = BenchmarkTab::Ram;
            DrawRamBenchmarkTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Disk")) {
            m_currentBenchmarkTab = BenchmarkTab::Ssd;
            DrawSsdBenchmarkTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void BrazenApp::DrawCpuBenchmarkTab() {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", m_hardwareInfo.cpuModel.c_str());
    ImGui::TextDisabled("%u cores / %u threads", m_hardwareInfo.physicalCores, m_hardwareInfo.logicalCores);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Mode");
    if (DrawHelpButton("cpu_mode"))
        ImGui::TextWrapped(
            "Single-Core runs one thread pinned to a core, showing raw "
            "per-core performance. Multi-Core spreads the same tests "
            "across every logical core to show total throughput. Both "
            "runs each selected test once in each mode.");
    int mode = static_cast<int>(m_cpuSettings.mode);
    ImGui::RadioButton("Single-Core", &mode, static_cast<int>(CategoryRunMode::SingleCoreOnly));
    ImGui::SameLine();
    ImGui::RadioButton("Multi-Core", &mode, static_cast<int>(CategoryRunMode::MultiCoreOnly));
    ImGui::SameLine();
    ImGui::RadioButton("Both", &mode, static_cast<int>(CategoryRunMode::Both));
    m_cpuSettings.mode = static_cast<CategoryRunMode>(mode);

    ImGui::Spacing();
    ImGui::Text("Tests to include");
    if (DrawHelpButton("cpu_tests"))
        ImGui::TextWrapped(
            "Uncheck any test you don't want run this time. Each test "
            "stresses a different part of the CPU (integer math, "
            "floating point, memory-adjacent hashing/sorting, etc.), so "
            "the composite score reflects a broad mix rather than one "
            "workload.");
    auto& reg = TestRegistry::Instance();
    int selectedCount = 0;
    for (size_t i = 0; i < reg.Count(); ++i) {
        if (reg.Samples()[i]->GetCategory() != TestCategory::Cpu) continue;
        ImGui::PushID(static_cast<int>(i));
        bool sel = m_selected[i];
        if (ImGui::Checkbox(reg.Samples()[i]->GetName().c_str(), &sel)) m_selected[i] = sel;
        ImGui::SameLine();
        ImGui::TextDisabled("- %s", reg.Samples()[i]->GetDescription().c_str());
        if (sel) selectedCount++;
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Text("Duration per test");
    if (DrawHelpButton("cpu_duration"))
        ImGui::TextWrapped(
            "How long each selected test runs, per mode. Longer runs "
            "average out short-term noise; shorter runs are quicker "
            "sanity checks.");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##cpuduration", &m_cpuSettings.durationSeconds, 1.0f, 30.0f, "%.0f s");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Reset Defaults", ImVec2(AutoButtonWidth("Reset Defaults", 140.0f), 0))) {
        m_cpuSettings = CpuSettings{};
        for (size_t i = 0; i < reg.Count(); ++i)
            if (reg.Samples()[i]->GetCategory() == TestCategory::Cpu) m_selected[i] = true;
    }
    ImGui::SameLine();
    bool canStart = selectedCount > 0;
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Run Benchmark", ImVec2(AutoButtonWidth("Run Benchmark", 150.0f), 0)))
        StartCpuBenchmark();
    if (!canStart) ImGui::EndDisabled();
    if (!canStart) {
        ImGui::SameLine();
        ImGui::TextDisabled("(select at least one test)");
    }
}

void BrazenApp::StartCpuBenchmark() {
    auto& reg = TestRegistry::Instance();
    int jobs = 0;
    std::string names;
    for (size_t i = 0; i < reg.Count(); ++i) {
        if (reg.Samples()[i]->GetCategory() != TestCategory::Cpu) continue;
        if (!m_selected[i]) continue;
        if (!names.empty()) names += ", ";
        names += reg.Samples()[i]->GetName();
        if (m_cpuSettings.mode == CategoryRunMode::SingleCoreOnly || m_cpuSettings.mode == CategoryRunMode::Both) {
            m_manager.Enqueue(i, RunMode::SingleCore, m_cpuSettings.durationSeconds);
            ++jobs;
        }
        if (m_cpuSettings.mode == CategoryRunMode::MultiCoreOnly || m_cpuSettings.mode == CategoryRunMode::Both) {
            m_manager.Enqueue(i, RunMode::MultiCore, m_cpuSettings.durationSeconds);
            ++jobs;
        }
    }
    if (jobs == 0) {
        LogWarn("Run", "CPU benchmark not started: no CPU tests are selected on the CPU tab.");
        return;
    }
    LogInfo("Run", "CPU benchmark queued: %s | %s | %.0f s per test | %d job%s.", names.c_str(),
            CategoryModeLabel(m_cpuSettings.mode), m_cpuSettings.durationSeconds, jobs, jobs == 1 ? "" : "s");
    m_currentView = SidebarView::Results;
}

void BrazenApp::DrawRamBenchmarkTab() {
    ImGui::Spacing();
    ImGui::TextWrapped("Total RAM: %s", FormatBytesAsGB(m_hardwareInfo.totalRamBytes).c_str());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Buffer size (per array, per thread)");
    if (DrawHelpButton("ram_buffer"))
        ImGui::TextWrapped(
            "Bigger sizes better defeat large L3 caches on high-end "
            "CPUs, at the cost of more memory used during the test.");
    ImGui::SetNextItemWidth(240);
    ImGui::Combo("##ramsize", &m_ramSettings.bufferSizeIndex, kRamBufferSizeLabels,
                  static_cast<int>(sizeof(kRamBufferSizeLabels) / sizeof(kRamBufferSizeLabels[0])));
    uint64_t requiredBytes = RamRequiredBytes();
    unsigned plannedThreads = RamPlannedThreads();
    ImGui::TextDisabled("Memory this run will allocate: %s (%u thread%s, 2 buffers each)",
                        FormatBytesAsGB(requiredBytes).c_str(), plannedThreads, plannedThreads == 1 ? "" : "s");
    {
        unsigned long long budget = RamBudgetBytes();
        unsigned long long available = detail::QueryAvailableRamBytes();
        if (available > 0)
            ImGui::TextDisabled("Free memory right now: %s (the test may use up to %s of it)",
                                FormatBytesAsGB(available).c_str(), FormatBytesAsGB(budget).c_str());
        else if (budget > 0)
            ImGui::TextDisabled("Free memory can't be measured here; the test may use up to %s (half of installed RAM).",
                                FormatBytesAsGB(budget).c_str());
    }

    ImGui::Spacing();
    ImGui::Text("Threads");
    if (DrawHelpButton("ram_threads"))
        ImGui::TextWrapped(
            "Auto uses every logical core this app is allowed to use "
            "for the multi-core run. "
            "Override it to deliberately test with fewer threads.");
    ImGui::Checkbox("Use all cores (auto)", &m_ramSettings.autoThreadCount);
    if (!m_ramSettings.autoThreadCount) {
        ImGui::SetNextItemWidth(240);
        int maxThreads = static_cast<int>(UsableCoreCount());
        m_ramSettings.threadCountOverride = std::min(m_ramSettings.threadCountOverride, maxThreads);
        ImGui::SliderInt("##ramthreads", &m_ramSettings.threadCountOverride, 1, maxThreads);
    }

    ImGui::Spacing();
    ImGui::Text("Mode");
    if (DrawHelpButton("ram_mode"))
        ImGui::TextWrapped(
            "Single-Core runs the copy on one thread. Multi-Core runs "
            "it across multiple threads at once, each hammering its own "
            "buffer, to measure total memory bandwidth. Both runs each "
            "once.");
    int mode = static_cast<int>(m_ramSettings.mode);
    ImGui::RadioButton("Single-Core", &mode, static_cast<int>(CategoryRunMode::SingleCoreOnly));
    ImGui::SameLine();
    ImGui::RadioButton("Multi-Core", &mode, static_cast<int>(CategoryRunMode::MultiCoreOnly));
    ImGui::SameLine();
    ImGui::RadioButton("Both", &mode, static_cast<int>(CategoryRunMode::Both));
    m_ramSettings.mode = static_cast<CategoryRunMode>(mode);

    ImGui::Spacing();
    ImGui::Text("Duration per test");
    if (DrawHelpButton("ram_duration"))
        ImGui::TextWrapped("How long the timed copy loop runs, per mode.");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##ramduration", &m_ramSettings.durationSeconds, 1.0f, 30.0f, "%.0f s");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Reset Defaults", ImVec2(AutoButtonWidth("Reset Defaults", 140.0f), 0)))
        m_ramSettings = RamSettings{};
    ImGui::SameLine();
    std::string ramWhyNot;
    const bool canStartRam = CanStartRam(&ramWhyNot);
    if (!canStartRam) ImGui::BeginDisabled();
    if (ImGui::Button("Run Benchmark", ImVec2(AutoButtonWidth("Run Benchmark", 150.0f), 0)))
        StartRamBenchmark();
    if (!canStartRam) ImGui::EndDisabled();
    if (!canStartRam) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "(%s)", ramWhyNot.c_str());
    }
}

unsigned BrazenApp::RamPlannedThreads() const {
    if (m_ramSettings.mode == CategoryRunMode::SingleCoreOnly) return 1;
    return m_ramSettings.autoThreadCount ? UsableCoreCount()
                                         : static_cast<unsigned>(std::max(1, m_ramSettings.threadCountOverride));
}

unsigned long long BrazenApp::RamRequiredBytes() const {
    return static_cast<unsigned long long>(kRamBufferSizesMB[m_ramSettings.bufferSizeIndex]) * 1024ull * 1024ull *
           2ull * RamPlannedThreads();
}

unsigned long long BrazenApp::RamBudgetBytes() const {
    unsigned long long available = detail::QueryAvailableRamBytes();
    if (available > 0) return static_cast<unsigned long long>(static_cast<double>(available) * kRamBudgetShareOfAvailable);
    if (m_hardwareInfo.totalRamBytes > 0)
        return static_cast<unsigned long long>(static_cast<double>(m_hardwareInfo.totalRamBytes) * kRamBudgetShareOfInstalled);
    return 0;
}

bool BrazenApp::CanStartRam(std::string* whyNot) const {
    unsigned long long budget = RamBudgetBytes();
    if (budget > 0 && RamRequiredBytes() > budget) {
        if (whyNot)
            *whyNot = "needs " + FormatBytesAsGB(RamRequiredBytes()) + " but only about " +
                      FormatBytesAsGB(budget) + " is safe to use; pick a smaller buffer, fewer threads or close other programs";
        return false;
    }
    return true;
}

void BrazenApp::StartRamBenchmark() {
    std::string why;
    if (!CanStartRam(&why)) {
        LogWarn("RAM", "RAM benchmark not started: %s.", why.c_str());
        return;
    }
    size_t bufferBytes = static_cast<size_t>(kRamBufferSizesMB[m_ramSettings.bufferSizeIndex]) * 1024ull * 1024ull;
    unsigned threadOverride = m_ramSettings.autoThreadCount ? 0u
                                                             : static_cast<unsigned>(m_ramSettings.threadCountOverride);

    // Two separate instances rather than one shared_ptr reused across
    // both Enqueue calls: BenchmarkRunner treats the prototype as
    // read-only and Clone()s it per worker thread, so sharing would be
    // safe in principle, but each mode's job may run at a different time
    // and giving each its own instance avoids any doubt about that.
    int jobs = 0;
    if (m_ramSettings.mode == CategoryRunMode::SingleCoreOnly || m_ramSettings.mode == CategoryRunMode::Both) {
        m_manager.Enqueue(std::make_shared<RamBandwidthTest>(bufferBytes), RunMode::SingleCore,
                           m_ramSettings.durationSeconds);
        ++jobs;
    }
    if (m_ramSettings.mode == CategoryRunMode::MultiCoreOnly || m_ramSettings.mode == CategoryRunMode::Both) {
        m_manager.Enqueue(std::make_shared<RamBandwidthTest>(bufferBytes), RunMode::MultiCore,
                           m_ramSettings.durationSeconds, threadOverride);
        ++jobs;
    }

    char threads[32];
    if (threadOverride == 0) std::snprintf(threads, sizeof(threads), "auto threads");
    else std::snprintf(threads, sizeof(threads), "%u threads", threadOverride);
    LogInfo("Run", "RAM benchmark queued: %d MB buffer | %s | %s | %.0f s per run | %d job%s.",
            kRamBufferSizesMB[m_ramSettings.bufferSizeIndex], CategoryModeLabel(m_ramSettings.mode),
            m_ramSettings.mode == CategoryRunMode::SingleCoreOnly ? "1 thread" : threads,
            m_ramSettings.durationSeconds, jobs, jobs == 1 ? "" : "s");

    m_currentView = SidebarView::Results;
}

void BrazenApp::DrawGpuBenchmarkTab() {
    ImGui::Spacing();
    bool gpuAvailable = m_gpuRunner && m_gpuRunner->IsSupported();
    if (gpuAvailable) {
        ImGui::TextWrapped("%s", m_hardwareInfo.gpuRenderer.c_str());
        ImGui::TextDisabled("%s", m_hardwareInfo.gpuVendor.c_str());
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "GPU benchmark unavailable");
        if (m_gpuRunner) ImGui::TextWrapped("%s", m_gpuRunner->GetError().c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("GPU to test");
    if (DrawHelpButton("gpu_select"))
        ImGui::TextWrapped(
            "OpenGL binds to one GPU when this window is created, and "
            "moving that binding to a different adapter at runtime "
            "needs platform-specific work (WGL/GLX/EGL device "
            "selection) this build doesn't implement. Only the Active "
            "entry can actually be benchmarked; the rest are listed for "
            "visibility into what's installed.");
    // Index 0 is always a synthetic "Active" entry (whatever OpenGL is
    // actually bound to); everything after it is from the OS-level
    // enumeration and is display-only.
    std::vector<std::string> gpuLabels;
    gpuLabels.push_back(gpuAvailable ? ("Active: " + m_hardwareInfo.gpuRenderer) : std::string("Active: unavailable"));
    for (const auto& gpu : m_allGpus)
        gpuLabels.push_back(gpu.name + " (detected only)");
    if (m_gpuSettings.selectedComboIndex >= static_cast<int>(gpuLabels.size()))
        m_gpuSettings.selectedComboIndex = 0;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##gpuselect", gpuLabels[static_cast<size_t>(m_gpuSettings.selectedComboIndex)].c_str())) {
        for (int i = 0; i < static_cast<int>(gpuLabels.size()); ++i) {
            bool isSelected = (i == m_gpuSettings.selectedComboIndex);
            if (ImGui::Selectable(gpuLabels[static_cast<size_t>(i)].c_str(), isSelected))
                m_gpuSettings.selectedComboIndex = i;
            if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    bool selectedIsActive = (m_gpuSettings.selectedComboIndex == 0);
    if (!selectedIsActive)
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                            "This build can only benchmark the Active GPU. Select it to run.");

    ImGui::Spacing();
    ImGui::Text("Tests to include");
    if (DrawHelpButton("gpu_tests"))
        ImGui::TextWrapped(
            "Each workload measures a different part of the GPU. ALU "
            "tests arithmetic throughput, Texture tests filtered sampling, "
            "and Fill Rate tests fullscreen raster throughput. All three "
            "are selected by default, but you can run only the workloads "
            "you need.");
    int selectedGpuTests = 0;
            ImGui::Checkbox("ALU", &m_gpuSettings.runAlu);
    ImGui::SameLine();
    ImGui::TextDisabled("- floating-point shader throughput");
    if (m_gpuSettings.runAlu) selectedGpuTests++;
    ImGui::Checkbox("Texture", &m_gpuSettings.runTexture);
    ImGui::SameLine();
    ImGui::TextDisabled("- filtered texture sampling");
    if (m_gpuSettings.runTexture) selectedGpuTests++;
    ImGui::Checkbox("Fill Rate", &m_gpuSettings.runFill);
    ImGui::SameLine();
    ImGui::TextDisabled("- fullscreen raster throughput");
    if (m_gpuSettings.runFill) selectedGpuTests++;

    ImGui::Spacing();
    ImGui::Text("Render resolution");
    if (DrawHelpButton("gpu_res"))
        ImGui::TextWrapped(
            "Higher resolutions mean more shader work per draw; mainly "
            "increases the amount of work in all three GPU workloads: "
            "ALU, texture sampling, and fill rate.");
    ImGui::SetNextItemWidth(240);
    ImGui::Combo("##gpures", &m_gpuSettings.resolutionIndex, kGpuResolutionLabels,
                  static_cast<int>(sizeof(kGpuResolutionLabels) / sizeof(kGpuResolutionLabels[0])));

    ImGui::Spacing();
    ImGui::Text("Duration");
    if (DrawHelpButton("gpu_duration"))
        ImGui::TextWrapped(
            "GPU work has no single/multi-core equivalent (a GL context "
            "can only run on one thread), so there's no mode selector "
            "here. The selected duration applies to each of the three "
            "GPU workloads, so a complete GPU run takes roughly three "
            "times that long.");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##gpuduration", &m_gpuSettings.durationSeconds, 1.0f, 30.0f, "%.0f s/workload");

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Reset Defaults", ImVec2(AutoButtonWidth("Reset Defaults", 140.0f), 0)))
        m_gpuSettings = GpuSettings{};
    ImGui::SameLine();
    bool canRunGpu = gpuAvailable && selectedIsActive && selectedGpuTests > 0;
    if (!canRunGpu) ImGui::BeginDisabled();
    bool runClicked = ImGui::Button("Run Benchmark", ImVec2(AutoButtonWidth("Run Benchmark", 150.0f), 0));
    if (!canRunGpu) ImGui::EndDisabled();
    if (runClicked) StartGpuBenchmark();
    if (selectedGpuTests == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("(select at least one test)");
    }
}

void BrazenApp::StartGpuBenchmark() {
    if (!m_gpuRunner || !m_gpuRunner->IsSupported()) {
        LogWarn("GPU", "GPU benchmark not started: %s",
                m_gpuRunner ? m_gpuRunner->GetError().c_str() : "GPU testing isn't set up.");
        return;
    }
    if (m_gpuRunner->IsBusy()) {
        LogWarn("GPU", "GPU benchmark not started: one is already running.");
        return;
    }
    int w = kGpuResolutions[m_gpuSettings.resolutionIndex][0];
    int h = kGpuResolutions[m_gpuSettings.resolutionIndex][1];
    unsigned workloadMask = (m_gpuSettings.runAlu ? 1u : 0u) |
                            (m_gpuSettings.runTexture ? 2u : 0u) |
                            (m_gpuSettings.runFill ? 4u : 0u);
    if (workloadMask == 0) {
        LogWarn("GPU", "GPU benchmark not started: no GPU workloads are selected on the GPU tab.");
        return;
    }
    m_gpuRunner->SetResolution(w, h); // no-op if a run is already in progress
    m_gpuRunner->RequestRun(m_gpuSettings.durationSeconds, workloadMask);

    std::string workloads;
    if (m_gpuSettings.runAlu) workloads += "ALU";
    if (m_gpuSettings.runTexture) workloads += std::string(workloads.empty() ? "" : ", ") + "Texture";
    if (m_gpuSettings.runFill) workloads += std::string(workloads.empty() ? "" : ", ") + "Fill Rate";
    LogInfo("GPU", "GPU benchmark started: %s | %d x %d | %.0f s per workload.", workloads.c_str(), w, h,
            m_gpuSettings.durationSeconds);
    m_currentView = SidebarView::Results;
}

unsigned long long BrazenApp::SsdRequiredBytes() const {
    unsigned long long size = static_cast<unsigned long long>(kSsdTestSizesMB[m_ssdSettings.testSizeIndex]) *
                              1024ull * 1024ull;
    unsigned threads = m_ssdSettings.mode == CategoryRunMode::SingleCoreOnly
        ? 1u
        : std::min(kMaxDiskBenchmarkThreads, UsableCoreCount());
    // Jobs run one after another and each job's files are deleted when it
    // ends, so the peak is the largest single job: a multi-core write
    // makes one file per worker, a read shares a single file.
    unsigned long long need = 0;
    if (m_ssdSettings.runWrite) need = std::max(need, size * threads);
    if (m_ssdSettings.runRead) need = std::max(need, size);
    return need;
}

bool BrazenApp::CanStartSsd(std::string* whyNot) const {
    auto fail = [&](const char* why) {
        if (whyNot) *whyNot = why;
        return false;
    };
    if (!m_ssdSettings.runWrite && !m_ssdSettings.runRead) return fail("select at least one test");
    int idx = m_ssdSettings.selectedDriveIndex;
    if (idx < 0 || idx >= static_cast<int>(m_allDrives.size())) return fail("no drive is selected");
    const DriveInfo& drive = m_allDrives[static_cast<size_t>(idx)];
    if (drive.path.empty()) return fail("the selected drive isn't writable");
    if (drive.freeBytes < SsdRequiredBytes() + kDiskFreeSpaceReserve)
        return fail("not enough free space on the selected drive for the test file(s) plus a safety reserve");
    return true;
}

void BrazenApp::StartSsdBenchmark() {
    std::string why;
    if (!CanStartSsd(&why)) {
        LogWarn("Disk", "Disk benchmark not started: %s.", why.c_str());
        return;
    }
    const auto& drive = m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)];
    uint64_t totalBytes = static_cast<uint64_t>(kSsdTestSizesMB[m_ssdSettings.testSizeIndex]) * 1024ull * 1024ull;
    std::filesystem::path targetDir(drive.path);
    const unsigned workers = std::min(kMaxDiskBenchmarkThreads, UsableCoreCount());

    // Write and Read are enqueued as separate DiskIoTest instances (see
    // DiskIoMode) so they show up as two distinct results rather than
    // one blended write+read number. Write and read throughput can
    // differ meaningfully on the same device. Each job gets its own
    // instance, same reasoning as StartRamBenchmark.
    //
    // The "duration" passed to Enqueue is only a safety time limit: the
    // disk test runs until all its data has been moved (see DiskIoTest).
    int jobs = 0;
    auto enqueueOp = [&](DiskIoMode op) {
        if (m_ssdSettings.mode == CategoryRunMode::SingleCoreOnly || m_ssdSettings.mode == CategoryRunMode::Both) {
            m_manager.Enqueue(std::make_shared<DiskIoTest>(targetDir, op, totalBytes), RunMode::SingleCore,
                               DiskIoTest::kSafetyLimitSeconds);
            ++jobs;
        }
        if (m_ssdSettings.mode == CategoryRunMode::MultiCoreOnly || m_ssdSettings.mode == CategoryRunMode::Both) {
            m_manager.Enqueue(std::make_shared<DiskIoTest>(targetDir, op, totalBytes), RunMode::MultiCore,
                               DiskIoTest::kSafetyLimitSeconds, workers);
            ++jobs;
        }
    };
    if (m_ssdSettings.runWrite) enqueueOp(DiskIoMode::Write);
    if (m_ssdSettings.runRead) enqueueOp(DiskIoMode::Read);

    LogInfo("Disk", "Disk benchmark queued: drive %d (%s) | %s | %d MB per direction | %s | %d job%s. "
                    "It runs until the data has been moved, so the time depends on the drive.",
            m_ssdSettings.selectedDriveIndex + 1, drive.label.c_str(),
            m_ssdSettings.runWrite && m_ssdSettings.runRead ? "write + read"
                : (m_ssdSettings.runWrite ? "write only" : "read only"),
            kSsdTestSizesMB[m_ssdSettings.testSizeIndex], CategoryModeLabel(m_ssdSettings.mode), jobs,
            jobs == 1 ? "" : "s");

    m_currentView = SidebarView::Results;
}

void BrazenApp::StartEverything() {
    LogInfo("Run", "Starting every benchmark that can run on this machine, using each tab's current settings.");
    StartCpuBenchmark();
    StartRamBenchmark();

    std::string why;
    if (CanStartSsd(&why)) StartSsdBenchmark();
    else LogWarn("Run", "Skipping the disk benchmark: %s.", why.c_str());

    if (m_gpuRunner && m_gpuRunner->IsSupported()) {
        m_gpuPendingAfterQueue = true;
        LogInfo("Run", "The GPU benchmark will start after the CPU/RAM/disk runs finish, so they don't disturb each other.");
    } else {
        LogWarn("Run", "Skipping the GPU benchmark: %s.",
                m_gpuRunner ? m_gpuRunner->GetError().c_str() : "GPU testing isn't set up");
    }
}

void BrazenApp::DrawSsdBenchmarkTab() {
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Writes a temporary file to the drive you pick, then reads it back, and "
        "reports the speed of each direction in MB/s (Disk Write and Disk Read "
        "results). The test runs until all of the data has been moved, so it "
        "takes as long as your drive needs: a fast NVMe drive finishes in "
        "seconds, a slow USB stick can take a few minutes. While it runs, the "
        "bar at the top shows progress and elapsed time.");
    ImGui::Spacing();

    ImGui::Text("Drive to test");
    if (DrawHelpButton("ssd_drive"))
        ImGui::TextWrapped(
            "Picking a drive here just means picking where the test "
            "file gets written. Brazen writes a temporary file to "
            "that drive's storage, reads it back, and deletes it "
            "afterward. Drives that couldn't be confirmed writable "
            "(not mounted, read-only, permission denied, etc.) are "
            "shown but can't be selected.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        RefreshSystemInventory();

    if (m_allDrives.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "No drives detected.");
    } else {
        std::string currentLabel = "(none writable)";
        if (m_ssdSettings.selectedDriveIndex >= 0 &&
            m_ssdSettings.selectedDriveIndex < static_cast<int>(m_allDrives.size())) {
            currentLabel = m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)].label;
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##ssddrive", currentLabel.c_str())) {
            for (int i = 0; i < static_cast<int>(m_allDrives.size()); ++i) {
                const auto& drive = m_allDrives[static_cast<size_t>(i)];
                bool writable = !drive.path.empty();
                if (!writable) ImGui::BeginDisabled();
                std::string label = drive.label;
                if (!drive.path.empty())
                    label += "  (" + FormatBytesAsGB(drive.freeBytes) + " free of " + FormatBytesAsGB(drive.totalBytes) + ")";
                else
                    label += "  (not writable/mounted)";
                bool isSelected = (i == m_ssdSettings.selectedDriveIndex);
                if (ImGui::Selectable(label.c_str(), isSelected) && writable && i != m_ssdSettings.selectedDriveIndex) {
                    m_ssdSettings.selectedDriveIndex = i;
                    LogInfo("Disk", "Disk test drive set to drive %d (%s).", i + 1, drive.label.c_str());
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
                if (!writable) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        if (m_ssdSettings.selectedDriveIndex >= 0 &&
            m_ssdSettings.selectedDriveIndex < static_cast<int>(m_allDrives.size())) {
            const auto& selectedDrive = m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)];
            if (!selectedDrive.path.empty()) {
                ImGui::TextWrapped("Temporary test files will be written to: %s",
                                   selectedDrive.path.c_str());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Test size");
    if (DrawHelpButton("ssd_size"))
        ImGui::TextWrapped(
            "How much data is written, and then read back. Larger sizes "
            "are a better measure of sustained speed, because many drives "
            "are fast for the first few hundred MB (a small cache) and "
            "slower after that. The trade-off is that larger sizes take "
            "longer and need more free space. The temporary file is "
            "deleted as soon as the test finishes.");
    ImGui::SetNextItemWidth(240);
    ImGui::Combo("##ssdsize", &m_ssdSettings.testSizeIndex, kSsdTestSizeLabels,
                  static_cast<int>(sizeof(kSsdTestSizeLabels) / sizeof(kSsdTestSizeLabels[0])));

    ImGui::Spacing();
    ImGui::Text("Tests to include");
    if (DrawHelpButton("ssd_tests"))
        ImGui::TextWrapped(
            "Write measures how fast data can be saved to the drive, including "
            "the time to make sure it has really reached the drive. Read "
            "measures how fast that data can be read back. Both are selected "
            "by default, but either one can be run on its own.");
    ImGui::Checkbox("Write", &m_ssdSettings.runWrite);
    ImGui::SameLine();
    ImGui::TextDisabled("- sequential writes, flushed to the drive");
    ImGui::Checkbox("Read", &m_ssdSettings.runRead);
    ImGui::SameLine();
    ImGui::TextDisabled("- sequential reads");
    ImGui::TextDisabled("Each direction moves the data once, then the temporary file is deleted.");

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced options")) {
        ImGui::Text("Mode");
        if (DrawHelpButton("ssd_mode"))
            ImGui::TextWrapped(
                "Single-Core writes/reads from one thread and is what most "
                "people mean by a drive speed test. Multi-Core runs several "
                "threads at once, each on its own temporary file on the same "
                "drive, to measure aggregate throughput under concurrent I/O. "
                "Both runs each once.");
        int mode = static_cast<int>(m_ssdSettings.mode);
        ImGui::RadioButton("Single-Core", &mode, static_cast<int>(CategoryRunMode::SingleCoreOnly));
        ImGui::SameLine();
        ImGui::RadioButton("Multi-Core", &mode, static_cast<int>(CategoryRunMode::MultiCoreOnly));
        ImGui::SameLine();
        ImGui::RadioButton("Both", &mode, static_cast<int>(CategoryRunMode::Both));
        m_ssdSettings.mode = static_cast<CategoryRunMode>(mode);
        if (m_ssdSettings.mode != CategoryRunMode::SingleCoreOnly) {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Multi-Core disk testing has tradeoffs:");
            ImGui::TextWrapped(
                "It uses more temporary space and measures concurrent I/O "
                "behavior rather than a drive's simple single-request speed. "
                "Multi-Core results should not be compared directly with "
                "Single-Core results or used as a SATA link-speed reading.");
            unsigned plannedThreads = std::min(kMaxDiskBenchmarkThreads, UsableCoreCount());
            ImGui::TextDisabled("Multi-Core uses up to %u workers, each with its own %s test file.",
                                plannedThreads, kSsdTestSizeLabels[m_ssdSettings.testSizeIndex]);
        }
    }

    // Required space depends on test size, mode and which tests are on.
    std::string whyNot;
    bool canStart = CanStartSsd(&whyNot);
    if (m_ssdSettings.selectedDriveIndex >= 0 &&
        m_ssdSettings.selectedDriveIndex < static_cast<int>(m_allDrives.size()) &&
        !m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)].path.empty()) {
        const auto& selectedDrive = m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)];
        ImGui::Spacing();
        ImGui::TextDisabled("Needs about %s free on this drive (has %s).",
                            FormatBytesAsGB(SsdRequiredBytes() + kDiskFreeSpaceReserve).c_str(),
                            FormatBytesAsGB(selectedDrive.freeBytes).c_str());
        if (selectedDrive.freeBytes < SsdRequiredBytes() + kDiskFreeSpaceReserve)
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                               "Not enough free space for this test size. Pick a smaller size or another drive.");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (ImGui::Button("Reset Defaults", ImVec2(AutoButtonWidth("Reset Defaults", 140.0f), 0))) {
        int keepDrive = m_ssdSettings.selectedDriveIndex;
        m_ssdSettings = SsdSettings{};
        m_ssdSettings.selectedDriveIndex = keepDrive;
    }
    ImGui::SameLine();
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Run Benchmark", ImVec2(AutoButtonWidth("Run Benchmark", 150.0f), 0)))
        StartSsdBenchmark();
    if (!canStart) ImGui::EndDisabled();
    if (!canStart) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", whyNot.c_str());
    }
}

// ---------------------------------------------------------------------
// Results view
// ---------------------------------------------------------------------

void BrazenApp::DrawScoreCard(const char* label, const CompositeScore& score) {
    ImGui::TextDisabled("%s", label);
    if (score.testsScored == 0) {
        ImGui::TextDisabled("No runs yet");
        return;
    }

    ImGui::Text("%.0f pts", score.points);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f), "%s", score.rank.c_str());

    if (score.complete)
        ImGui::TextDisabled("All %d tests scored", score.testsTotal);
    else
        ImGui::TextDisabled("%d / %d tests scored", score.testsScored, score.testsTotal);
}

void BrazenApp::DrawResultsView() {
    ImGui::Spacing();

    // Composite score summary, carried over from the old sidebar "Your
    // Score" panel. Still useful, just relocated now that the sidebar
    // is nav-only.
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Composite Score");
    if (DrawHelpButton("score_info"))
        ImGui::TextWrapped(
            "Each CPU test's result is divided by a reference baseline "
            "then multiplied by 1000, and the composite score is the "
            "average across every completed CPU test. Baselines are "
            "illustrative estimates, not calibrated against real "
            "reference hardware. Treat the rank as a fun relative "
            "label, most meaningful when comparing this machine "
            "against itself over time. Single-Core and Multi-Core "
            "use different rank thresholds: Multi-Core has a wider "
            "curve because aggregate throughput rises with core count.");
    ImGui::SameLine();
    if (ImGui::SmallButton("View Rank Table"))
        ImGui::OpenPopup("Cat Rank Table##CatRankTable");
    DrawCatRankTablePopup();
    ImGui::Separator();

    auto singleScore = ComputeComposite(m_latestSingleCore, RunMode::SingleCore);
    auto multiScore = ComputeComposite(m_latestMultiCore, RunMode::MultiCore);
    ImGui::BeginChild("ScoreSingle", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f - 6.0f, ImGui::GetFontSize() * 4.2f), true);
    DrawScoreCard("Single-Core", singleScore);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("ScoreMulti", ImVec2(0, ImGui::GetFontSize() * 4.2f), true);
    DrawScoreCard("Multi-Core", multiScore);
    ImGui::EndChild();

    // Keep one latest completed result per test and mode so repeated runs do
    // not distort the component summary. GPU workloads intentionally remain
    // separate because their units measure different things.
    std::map<std::string, BenchmarkResult> latestComponentResults;
    for (const auto& entry : m_resultEntries) {
        if (entry.result.cancelled || entry.result.failed) continue;
        std::string key = entry.result.testName + "#" +
                          std::to_string(static_cast<int>(entry.result.mode));
        latestComponentResults[key] = entry.result;
    }

    auto collectResults = [&latestComponentResults](auto matches) {
        std::vector<BenchmarkResult> results;
        for (const auto& item : latestComponentResults) {
            if (matches(item.second)) results.push_back(item.second);
        }
        return results;
    };
    auto averageScore = [](const std::vector<BenchmarkResult>& results) {
        double total = 0.0;
        for (const auto& result : results) total += result.score;
        return results.empty() ? 0.0 : total / static_cast<double>(results.size());
    };

    auto ramResults = collectResults([](const BenchmarkResult& result) {
        return result.testName == "RAM Bandwidth";
    });
    auto storageResults = collectResults([](const BenchmarkResult& result) {
        return result.testName == "Disk Read" || result.testName == "Disk Write";
    });
    auto gpuAluResults = collectResults([](const BenchmarkResult& result) {
        return result.testName == "GPU ALU";
    });
    auto gpuTextureResults = collectResults([](const BenchmarkResult& result) {
        return result.testName == "GPU Texture";
    });
    auto gpuFillResults = collectResults([](const BenchmarkResult& result) {
        return result.testName == "GPU Fill Rate";
    });

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Component Averages");
    if (DrawHelpButton("component_averages"))
        ImGui::TextWrapped(
            "These values use the latest completed result for each test and "
            "mode. RAM and storage share one unit, so their values are "
            "averaged directly. GPU workloads use different units and are "
            "shown separately rather than combined into a misleading number.");
    ImGui::Separator();
    if (ImGui::BeginTable("ComponentAverages", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Component");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Coverage");
        ImGui::TableHeadersRow();

        auto drawAverageRow = [&averageScore](const char* label, const std::vector<BenchmarkResult>& results,
                              const char* fallback) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            if (results.empty()) {
                ImGui::TextDisabled("No runs yet");
            } else {
                ImGui::Text("%.2f %s", averageScore(results), results.front().unit.c_str());
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", results.empty() ? fallback : "Latest result per mode");
        };

        drawAverageRow("RAM", ramResults, "Run RAM benchmark");
        drawAverageRow("Storage", storageResults, "Run Disk benchmark");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("GPU");
        ImGui::TableSetColumnIndex(1);
        if (gpuAluResults.empty() && gpuTextureResults.empty() && gpuFillResults.empty()) {
            ImGui::TextDisabled("No runs yet");
        } else {
            if (!gpuAluResults.empty())
                ImGui::Text("ALU %.2f GFLOPS", averageScore(gpuAluResults));
            if (!gpuTextureResults.empty())
                ImGui::Text("Texture %.2f GB/s", averageScore(gpuTextureResults));
            if (!gpuFillResults.empty())
                ImGui::Text("Fill %.2f Gpixels/s", averageScore(gpuFillResults));
        }
        ImGui::TableSetColumnIndex(2);
        int gpuWorkloads = (!gpuAluResults.empty() ? 1 : 0) +
                           (!gpuTextureResults.empty() ? 1 : 0) +
                           (!gpuFillResults.empty() ? 1 : 0);
        ImGui::TextDisabled("%d / 3 workloads", gpuWorkloads);

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::SmallButton("Delete Selected"))
        DeleteSelectedResults();
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear All Results History")) {
        if (m_confirmBeforeClear)
            ImGui::OpenPopup("Clear Results?##ConfirmClearResults");
        else
            ClearAllResults();
    }
    DrawClearResultsModal();
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy All")) {
        std::string out = "Date\tTest\tMode\tThreads\tScore\tDuration\tPinned\tStatus\n";
        for (auto it = m_resultEntries.rbegin(); it != m_resultEntries.rend(); ++it) {
            const auto& r = it->result;
            char line[300];
            snprintf(line, sizeof(line), "%s\t%s\t%s\t%d\t%.2f %s\t%.2f s\t%s\t%s\n",
                     it->timestamp.c_str(), r.testName.c_str(), ModeLabel(r.mode),
                     r.threadsUsed, r.score, r.unit.c_str(), r.elapsedSeconds,
                     r.affinityPinned ? "Yes" : "No", ResultStatusLabel(r));
            out += line;
        }
        ImGui::SetClipboardText(out.c_str());
    }
    ImGui::Separator();

    if (m_resultEntries.empty()) {
        ImGui::TextDisabled("No results yet. Run a benchmark from the Benchmarks view.");
        return;
    }

    // Results are session-only by design (see the App Info view). They
    // deliberately don't persist to disk, so this table is always empty
    // on a fresh launch.
    float availableHeight = ImGui::GetContentRegionAvail().y;
    float tableHeight = (m_expandedResultIndex >= 0) ? availableHeight * 0.55f : availableHeight;

    static ImGuiTableFlags tableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("ResultsTable", 6, tableFlags, ImVec2(0, tableHeight))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 26.0f);
        ImGui::TableSetupColumn("Date");
        ImGui::TableSetupColumn("Test");
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("Score");
        ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();

        // Newest first.
        for (int i = static_cast<int>(m_resultEntries.size()) - 1; i >= 0; --i) {
            const auto& entry = m_resultEntries[static_cast<size_t>(i)];
            const auto& r = entry.result;
            ImGui::PushID(i);
            ImGui::TableNextRow();

            // Column 1 first (with SpanAllColumns) so the whole row is
            // clickable to expand/collapse detail; the checkbox in
            // column 0 is drawn afterward and, being the smaller/later
            // widget, takes its own clicks without being swallowed by
            // the row-wide Selectable underneath it.
            ImGui::TableSetColumnIndex(1);
            bool expanded = (m_expandedResultIndex == i);
            if (ImGui::Selectable(entry.timestamp.c_str(), expanded, ImGuiSelectableFlags_SpanAllColumns))
                m_expandedResultIndex = expanded ? -1 : i;

            ImGui::TableSetColumnIndex(0);
            bool sel = m_resultSelected[static_cast<size_t>(i)];
            if (ImGui::Checkbox("##sel", &sel)) m_resultSelected[static_cast<size_t>(i)] = sel;

            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(r.testName.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(ModeLabel(r.mode));
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.2f %s", r.score, r.unit.c_str());
            ImGui::TableSetColumnIndex(5);
            if (r.failed)
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Failed");
            else if (r.cancelled)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Cancelled");
            else
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Done");

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (m_expandedResultIndex >= 0 && m_expandedResultIndex < static_cast<int>(m_resultEntries.size())) {
        ImGui::Separator();
        DrawResultDetail(m_resultEntries[static_cast<size_t>(m_expandedResultIndex)].result,
                          static_cast<size_t>(m_expandedResultIndex));
    }
}

void BrazenApp::DrawResultDetail(const BenchmarkResult& r, size_t index) {
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Result Detail");
    ImGui::Separator();
    ImGui::Text("Test: %s", r.testName.c_str());
    ImGui::Text("Mode: %s", ModeLabel(r.mode));
    if (r.mode != RunMode::Gpu)
        ImGui::Text("Threads used: %d", r.threadsUsed);
    ImGui::Text("Score: %.3f %s", r.score, r.unit.c_str());
    ImGui::Text("Duration: %.2f s", r.elapsedSeconds);
    ImGui::Text("Total operations counted: %llu", static_cast<unsigned long long>(r.totalOps));
    ImGui::Text("Status: %s", r.failed ? "Failed" : (r.cancelled ? "Cancelled" : "Completed"));
    if (!r.notes.empty()) {
        if (r.failed)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Reason: %s", r.notes.c_str());
        else
            ImGui::TextWrapped("Notes: %s", r.notes.c_str());
    }
    if (r.mode == RunMode::Gpu) {
        ImGui::TextDisabled("Core affinity: N/A for GPU work");
    } else if (r.affinityPinned) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Core affinity pinned: Yes");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Core affinity pinned: No");
        ImGui::TextDisabled("The OS rejected the pin request; this score may carry extra scheduler-induced noise.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    // Cat rank for this single result, standing in for the composite:
    // only meaningful for CPU tests, since BaselineForTest() only has
    // real reference numbers for those five (RAM/GPU/SSD would all fall
    // back to its default baseline of 1.0, producing a meaningless
    // number rather than an absent one).
    bool isCpuTest = false;
    for (const auto& sample : TestRegistry::Instance().Samples()) {
        if (sample->GetName() == r.testName && sample->GetCategory() == TestCategory::Cpu) {
            isCpuTest = true;
            break;
        }
    }
    if (isCpuTest && !r.cancelled && !r.failed) {
        double points = (r.score / BaselineForTest(r.testName)) * 1000.0;
        std::string rank = RankForPoints(points, r.mode);
        ImGui::Text("Standalone cat rank:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f), "%s", rank.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.0f pts, if this were your only test)", points);
    } else {
        ImGui::TextDisabled("Cat ranking is calibrated for CPU tests only.");
    }

    ImGui::Spacing();
    if (ImGui::Button("Delete This Result", ImVec2(AutoButtonWidth("Delete This Result", 180.0f), 0))) {
        m_resultEntries.erase(m_resultEntries.begin() + static_cast<long>(index));
        m_resultSelected.erase(m_resultSelected.begin() + static_cast<long>(index));
        m_expandedResultIndex = -1;
        return; // r/index are now dangling; nothing else below reads them.
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(AutoButtonWidth("Close", 120.0f), 0)))
        m_expandedResultIndex = -1;
}

void BrazenApp::DrawClearResultsModal() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20.0f, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Clear Results?##ConfirmClearResults", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("Clear all %zu result%s? This can't be undone.",
                            m_resultEntries.size(), m_resultEntries.size() == 1 ? "" : "s");
        ImGui::Spacing();
        if (ImGui::Button("Clear", ImVec2(AutoButtonWidth("Clear", 120.0f), 0))) {
            ClearAllResults();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(AutoButtonWidth("Cancel", 120.0f), 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void BrazenApp::DrawCatRankTablePopup() {
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 18.0f, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Cat Rank Table##CatRankTable", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Minimum composite points needed for each rank. See the '?' "
            "next to Composite Score for how points are computed. "
            "Multi-Core uses a wider scale because aggregate throughput "
            "increases with core count.");
        ImGui::Separator();
        if (ImGui::BeginTable("CatRanksTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Rank");
            ImGui::TableSetupColumn("Single-Core");
            ImGui::TableSetupColumn("Multi-Core");
            ImGui::TableHeadersRow();
            const auto& singleRanks = CatRankTable(RunMode::SingleCore);
            const auto& multiRanks = CatRankTable(RunMode::MultiCore);
            for (size_t i = 0; i < singleRanks.size(); ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(singleRanks[i].name.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%.0f", singleRanks[i].minPoints);
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.0f", multiRanks[i].minPoints);
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(AutoButtonWidth("Close", 120.0f), 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void BrazenApp::DeleteSelectedResults() {
    std::vector<ResultEntry> kept;
    std::vector<bool> keptSel;
    kept.reserve(m_resultEntries.size());
    keptSel.reserve(m_resultEntries.size());
    for (size_t i = 0; i < m_resultEntries.size(); ++i) {
        if (!m_resultSelected[i]) {
            kept.push_back(m_resultEntries[i]);
            keptSel.push_back(false);
        }
    }
    size_t removed = m_resultEntries.size() - kept.size();
    m_resultEntries.swap(kept);
    m_resultSelected.swap(keptSel);
    m_expandedResultIndex = -1;
    LogInfo("Results", "Deleted %zu selected result%s.", removed, removed == 1 ? "" : "s");
}

void BrazenApp::ClearAllResults() {
    LogInfo("Results", "Cleared all %zu result%s and reset the composite score.", m_resultEntries.size(),
            m_resultEntries.size() == 1 ? "" : "s");
    m_resultEntries.clear();
    m_resultSelected.clear();
    m_latestSingleCore.clear();
    m_latestMultiCore.clear();
    m_expandedResultIndex = -1;
}

// ---------------------------------------------------------------------
// Log view
// ---------------------------------------------------------------------

void BrazenApp::DrawLogView() {
    ImGui::Spacing();
    if (ImGui::SmallButton("Copy All")) {
        std::string joined;
        for (const auto& line : m_log) {
            joined += line.text;
            joined += "\n";
        }
        ImGui::SetClipboardText(joined.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) m_log.clear();
    ImGui::SameLine();
    ImGui::Checkbox("Info", &m_logShowInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warnings", &m_logShowWarn);
    ImGui::SameLine();
    ImGui::Checkbox("Errors", &m_logShowError);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines", m_log.size());
    ImGui::Separator();

    // Leave room under the log for the console's input row.
    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("LogScroll", ImVec2(0, -footer), false);

    // Follow the newest line only while the user is already at the bottom,
    // so scrolling up to read something isn't yanked away by new output.
    bool stickToBottom = m_logJumpToBottom || ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f;

    ImGui::PushTextWrapPos(0.0f);
    for (const auto& line : m_log) {
        switch (line.level) {
            case LogLevel::Info:
                if (!m_logShowInfo) continue;
                ImGui::TextUnformatted(line.text.c_str());
                break;
            case LogLevel::Warn:
                if (!m_logShowWarn) continue;
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1.0f), "%s", line.text.c_str());
                break;
            case LogLevel::Error:
                if (!m_logShowError) continue;
                ImGui::TextColored(ImVec4(1.0f, 0.42f, 0.42f, 1.0f), "%s", line.text.c_str());
                break;
        }
    }
    ImGui::PopTextWrapPos();

    if (stickToBottom) ImGui::SetScrollHereY(1.0f);
    m_logJumpToBottom = false;
    ImGui::EndChild();

    DrawConsoleInput();
}

// ---------------------------------------------------------------------
// Console (typed commands, in the Log view)
// ---------------------------------------------------------------------

void BrazenApp::DrawConsoleInput() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.96f, 0.79f, 0.30f, 1.0f), ">");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (m_focusConsole) {
        ImGui::SetKeyboardFocusHere();
        m_focusConsole = false;
    }
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
    if (ImGui::InputTextWithHint("##console", "Type a command, e.g. help or run cpu  (Up/Down = history)",
                                 m_consoleInput, sizeof(m_consoleInput), flags,
                                 &BrazenApp::ConsoleInputCallback, this)) {
        std::string line = m_consoleInput;
        m_consoleInput[0] = '\0';
        size_t first = line.find_first_not_of(" \t");
        size_t last = line.find_last_not_of(" \t");
        if (first != std::string::npos) {
            line = line.substr(first, last - first + 1);
            if (m_commandHistory.empty() || m_commandHistory.back() != line) {
                m_commandHistory.push_back(line);
                if (m_commandHistory.size() > 100) m_commandHistory.erase(m_commandHistory.begin());
            }
            m_historyPos = -1;
            m_logJumpToBottom = true;
            ExecuteCommand(line);
        }
        m_focusConsole = true; // keep the cursor in the box for the next command
    }
}

int BrazenApp::ConsoleInputCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory) return 0;
    BrazenApp* self = static_cast<BrazenApp*>(data->UserData);
    const int count = static_cast<int>(self->m_commandHistory.size());
    if (count == 0) return 0;

    const int previous = self->m_historyPos;
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (self->m_historyPos == -1) self->m_historyPos = count - 1;
        else if (self->m_historyPos > 0) --self->m_historyPos;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (self->m_historyPos != -1 && ++self->m_historyPos >= count) self->m_historyPos = -1;
    }
    if (previous != self->m_historyPos) {
        const char* text = self->m_historyPos >= 0
            ? self->m_commandHistory[static_cast<size_t>(self->m_historyPos)].c_str() : "";
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, text);
    }
    return 0;
}

void BrazenApp::ConsolePrint(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    detail::LogV(LogLevel::Info, "Console", fmt, args);
    va_end(args);
}

void BrazenApp::PrintStatus() {
    bool any = false;
    if (m_manager.IsBusy()) {
        BenchmarkManager::RunProgress p = m_manager.GetCurrentProgress();
        if (p.busy) {
            any = true;
            if (p.fixedWork) {
                if (p.fraction >= 0.0)
                    ConsolePrint("Running: %s - %s, %.0f%% done, %.0f s elapsed", p.testName.c_str(),
                                 p.phase.c_str(), p.fraction * 100.0, p.elapsedSeconds);
                else
                    ConsolePrint("Running: %s - %.0f s elapsed", p.testName.c_str(), p.elapsedSeconds);
            } else {
                ConsolePrint("Running: %s - %.1f s of %.0f s", p.testName.c_str(), p.elapsedSeconds,
                             p.durationSeconds);
            }
        }
    }
    if (m_gpuRunner && m_gpuRunner->IsBusy()) {
        any = true;
        ConsolePrint("Running: GPU - %s", m_gpuRunner->StatusText().c_str());
    }
    size_t queued = m_manager.QueueSize();
    if (queued > 0) {
        any = true;
        ConsolePrint("%zu more job%s queued.", queued, queued == 1 ? "" : "s");
    }
    if (m_gpuPendingAfterQueue) {
        any = true;
        ConsolePrint("The GPU benchmark is waiting for the other runs to finish.");
    }
    if (!any) ConsolePrint("Idle. Nothing is running or queued.");
}

void BrazenApp::PrintDrives() {
    if (m_allDrives.empty()) {
        ConsolePrint("No drives detected.");
        return;
    }
    ConsolePrint("Drives (* = selected for the disk test; use 'drive <number>' to change):");
    for (size_t i = 0; i < m_allDrives.size(); ++i) {
        const DriveInfo& d = m_allDrives[i];
        bool selected = static_cast<int>(i) == m_ssdSettings.selectedDriveIndex;
        if (d.path.empty()) {
            ConsolePrint("  %c %zu. %s - not writable, can't be tested", selected ? '*' : ' ', i + 1,
                         d.label.c_str());
        } else {
            ConsolePrint("  %c %zu. %s - %s free of %s (%s)", selected ? '*' : ' ', i + 1, d.label.c_str(),
                         FormatBytesAsGB(d.freeBytes).c_str(), FormatBytesAsGB(d.totalBytes).c_str(),
                         d.path.c_str());
        }
    }
}

void BrazenApp::PrintResults() {
    if (m_resultEntries.empty()) {
        ConsolePrint("No results yet. Try 'run cpu'.");
        return;
    }
    constexpr size_t kMaxShown = 30;
    size_t start = m_resultEntries.size() > kMaxShown ? m_resultEntries.size() - kMaxShown : 0;
    ConsolePrint("Results (%s):", start > 0 ? "most recent 30, oldest first" : "oldest first");
    for (size_t i = start; i < m_resultEntries.size(); ++i) {
        const ResultEntry& e = m_resultEntries[i];
        const BenchmarkResult& r = e.result;
        if (r.failed) {
            ConsolePrint("  %s  %s (%s): FAILED - %s", e.timestamp.c_str(), r.testName.c_str(), ModeLabel(r.mode),
                         r.notes.c_str());
        } else {
            ConsolePrint("  %s  %s (%s): %.2f %s%s", e.timestamp.c_str(), r.testName.c_str(), ModeLabel(r.mode),
                         r.score, r.unit.c_str(), r.cancelled ? "  [cancelled]" : "");
        }
    }
}

void BrazenApp::ExecuteCommand(const std::string& rawLine) {
    // Commands shouldn't yank the user out of the console the way the
    // buttons' "switch to Results" convenience does.
    const SidebarView keepView = m_currentView;

    ConsolePrint("> %s", rawLine.c_str());

    std::vector<std::string> tok;
    {
        std::istringstream iss(rawLine);
        std::string t;
        while (iss >> t) {
            for (auto& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            tok.push_back(std::move(t));
        }
    }
    if (tok.empty()) return;
    const std::string& cmd = tok[0];

    if (cmd == "help" || cmd == "?") {
        ConsolePrint("Console commands:");
        ConsolePrint("  help                         show this list");
        ConsolePrint("  status                       what is running or queued right now");
        ConsolePrint("  run cpu|ram|gpu|disk|all     start a benchmark with the settings from its tab");
        ConsolePrint("  cancel                       stop everything that is running or queued");
        ConsolePrint("  results                      list the recorded results");
        ConsolePrint("  sysinfo                      CPU, RAM, OS and system details");
        ConsolePrint("  drives                       list drives; '*' marks the one the disk test uses");
        ConsolePrint("  drive <number>               choose the drive for the disk test");
        ConsolePrint("  refresh                      re-scan GPUs and drives");
        ConsolePrint("  version                      show the Brazen version");
        ConsolePrint("  clear log | clear results    clear this log, or all recorded results");
        ConsolePrint("  quit                         close Brazen");
    } else if (cmd == "status") {
        PrintStatus();
    } else if (cmd == "version") {
        ConsolePrint("Brazen %s", BRAZEN_VERSION);
    } else if (cmd == "sysinfo" || cmd == "hw" || cmd == "hardware") {
        LogHardwareSummary();
        if (!m_hardwareInfo.gpuRenderer.empty())
            ConsolePrint("GPU (benchmarked): %s (%s)", m_hardwareInfo.gpuRenderer.c_str(),
                         m_hardwareInfo.gpuVendor.c_str());
        for (size_t i = 0; i < m_allGpus.size(); ++i)
            ConsolePrint("GPU %zu (OS-reported): %s", i + 1, m_allGpus[i].name.c_str());
    } else if (cmd == "drives") {
        PrintDrives();
    } else if (cmd == "drive") {
        if (tok.size() < 2) {
            ConsolePrint("Usage: drive <number>  (see 'drives' for the numbers)");
        } else {
            int n = std::atoi(tok[1].c_str());
            if (n < 1 || n > static_cast<int>(m_allDrives.size())) {
                LogWarn("Console", "There is no drive %s. Type 'drives' to see the list.", tok[1].c_str());
            } else if (m_allDrives[static_cast<size_t>(n - 1)].path.empty()) {
                LogWarn("Console", "Drive %d isn't writable, so it can't be used for the disk test.", n);
            } else {
                m_ssdSettings.selectedDriveIndex = n - 1;
                LogInfo("Disk", "Disk test drive set to drive %d (%s).", n,
                        m_allDrives[static_cast<size_t>(n - 1)].label.c_str());
            }
        }
    } else if (cmd == "refresh") {
        RefreshSystemInventory();
    } else if (cmd == "run") {
        if (tok.size() < 2) {
            ConsolePrint("Usage: run cpu|ram|gpu|disk|all");
        } else if (tok[1] == "cpu") {
            StartCpuBenchmark();
        } else if (tok[1] == "ram") {
            StartRamBenchmark();
        } else if (tok[1] == "gpu") {
            bool busy = !m_manager.IsIdle() || (m_gpuRunner && m_gpuRunner->IsBusy());
            if (busy && m_gpuRunner && m_gpuRunner->IsSupported()) {
                m_gpuPendingAfterQueue = true;
                LogInfo("Run", "Something is already running; the GPU benchmark will start once it finishes.");
            } else {
                StartGpuBenchmark();
            }
        } else if (tok[1] == "disk" || tok[1] == "ssd" || tok[1] == "storage") {
            StartSsdBenchmark();
        } else if (tok[1] == "all" || tok[1] == "everything") {
            StartEverything();
        } else {
            LogWarn("Console", "Don't know how to run '%s'. Try: run cpu, ram, gpu, disk or all.", tok[1].c_str());
        }
    } else if (cmd == "cancel" || cmd == "stop") {
        CancelEverything();
    } else if (cmd == "results") {
        PrintResults();
    } else if (cmd == "clear") {
        if (tok.size() >= 2 && tok[1] == "log") {
            m_log.clear();
        } else if (tok.size() >= 2 && tok[1] == "results") {
            ClearAllResults();
        } else {
            ConsolePrint("Usage: clear log  or  clear results");
        }
    } else if (cmd == "quit" || cmd == "exit") {
        LogInfo("App", "Quitting.");
        m_quitRequested = true;
    } else {
        LogWarn("Console", "Unknown command '%s'. Type 'help' for the list of commands.", cmd.c_str());
    }

    m_currentView = keepView;
}

// ---------------------------------------------------------------------
// System Info view
// ---------------------------------------------------------------------

void BrazenApp::DrawSystemInfoView() {
    ImGui::Spacing();
    ImGui::TextWrapped("A detailed look at the hardware this copy of Brazen is running on.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh"))
        RefreshSystemInventory();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "System");
    ImGui::Separator();
    ImGui::Text("Operating System: %s", m_hardwareInfo.osVersion.c_str());
    ImGui::Text("Kernel: %s", m_hardwareInfo.kernelVersion.c_str());
    ImGui::Text("Model: %s %s", m_hardwareInfo.systemVendor.c_str(), m_hardwareInfo.systemModel.c_str());
    ImGui::Text("Motherboard: %s %s", m_hardwareInfo.motherboardVendor.c_str(), m_hardwareInfo.motherboardModel.c_str());
    ImGui::Text("BIOS: %s %s", m_hardwareInfo.biosVendor.c_str(), m_hardwareInfo.biosVersion.c_str());
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "CPU");
    ImGui::Separator();
    ImGui::TextWrapped("Name: %s", m_hardwareInfo.cpuModel.c_str());
    ImGui::Text("Topology: %u %s, %u %s", m_hardwareInfo.physicalCores,
                m_hardwareInfo.physicalCores == 1 ? "Core" : "Cores",
                m_hardwareInfo.logicalCores, m_hardwareInfo.logicalCores == 1 ? "Thread" : "Threads");
    ImGui::TextWrapped("Identifier: %s", m_hardwareInfo.cpuIdentifier.c_str());
    if (m_hardwareInfo.cpuMaxFrequencyGHz > 0.0) {
        ImGui::Text("Max Frequency: %.2f GHz", m_hardwareInfo.cpuMaxFrequencyGHz);
        ImGui::SameLine();
        ImGui::TextDisabled("(turbo/boost ceiling as reported by the OS, not a verified base clock)");
    } else {
        ImGui::TextDisabled("Max Frequency: Unknown");
    }
    ImGui::Text("L1 Instruction Cache: %s", m_hardwareInfo.cpuCache.l1Instruction.c_str());
    ImGui::SameLine();
    ImGui::Text("  L1 Data Cache: %s", m_hardwareInfo.cpuCache.l1Data.c_str());
    ImGui::Text("L2 Cache: %s", m_hardwareInfo.cpuCache.l2.c_str());
    ImGui::SameLine();
    ImGui::Text("  L3 Cache: %s", m_hardwareInfo.cpuCache.l3.c_str());
    ImGui::TextDisabled("(cache sizes are as reported for core 0; hybrid performance/efficiency-core designs may not be fully represented)");
    ImGui::Spacing();
    ImGui::Text("Instruction Sets");
    ImGui::TextWrapped("%s", m_hardwareInfo.cpuInstructionSets.c_str());
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Memory");
    ImGui::Separator();
    ImGui::Text("Total installed: %s", FormatBytesAsGB(m_hardwareInfo.totalRamBytes).c_str());
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Graphics");
    ImGui::Separator();
    if (m_gpuRunner && m_gpuRunner->IsSupported()) {
        ImGui::TextWrapped("Active (used for benchmarking): %s", m_hardwareInfo.gpuRenderer.c_str());
        ImGui::TextDisabled("Vendor: %s", m_hardwareInfo.gpuVendor.c_str());
    } else if (m_gpuRunner) {
        ImGui::TextWrapped("Active GPU unavailable: %s", m_gpuRunner->GetError().c_str());
    } else {
        ImGui::TextDisabled("Active GPU not detected");
    }
    if (!m_allGpus.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("All detected GPUs (including any inactive/no-driver-bound dGPU):");
        for (const auto& gpu : m_allGpus)
            ImGui::BulletText("%s", gpu.name.c_str());
    } else {
        ImGui::TextDisabled("No additional GPUs detected beyond the active one (or full enumeration isn't available on this platform).");
    }
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Storage");
    ImGui::Separator();
    if (m_allDrives.empty()) {
        ImGui::TextDisabled("No drives detected.");
    } else if (ImGui::BeginTable("DrivesTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        // Even/equal-width columns (the default without an explicit
        // sizing policy) clipped "Not writable/mounted" since it's
        // wider than a quarter of the table. Same underlying issue as
        // the button widths, just showing up in a table this time. Size
        // Capacity/Free/Status to fit their actual longest content and
        // let Drive stretch into whatever's left.
        float cellPad = ImGui::GetStyle().CellPadding.x * 2.0f + 8.0f;
        float capacityWidth = ImGui::CalcTextSize("999.9 GB").x + cellPad;
        float statusWidth = std::max(ImGui::CalcTextSize("Not writable/mounted").x,
                                      ImGui::CalcTextSize("Testable").x) + cellPad;
        ImGui::TableSetupColumn("Drive", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Capacity", ImGuiTableColumnFlags_WidthFixed, capacityWidth);
        ImGui::TableSetupColumn("Free", ImGuiTableColumnFlags_WidthFixed, capacityWidth);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, statusWidth);
        ImGui::TableHeadersRow();
        for (const auto& drive : m_allDrives) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (drive.isPrimary) {
                ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f), "%s", drive.label.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(primary)");
            } else {
                ImGui::TextUnformatted(drive.label.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(FormatBytesAsGB(drive.totalBytes).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(drive.path.empty() ? "N/A" : FormatBytesAsGB(drive.freeBytes).c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(drive.path.empty() ? "Not writable/mounted" : "Testable");
        }
        ImGui::EndTable();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(
        "On Linux, drives are physical devices paired with a writable "
        "mounted partition; on Windows/macOS, each entry is a mounted "
        "volume/logical drive instead.");
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------
// App Info view
// ---------------------------------------------------------------------

void BrazenApp::DrawAppInfoView() {
    ImGui::Spacing();
    if (m_titleFont) ImGui::PushFont(m_titleFont);
    ImGui::TextColored(ImVec4(0.96f, 0.79f, 0.30f, 1.0f), "Brazen Benchmark");
    if (m_titleFont) ImGui::PopFont();
    ImGui::Text("Version %s", BRAZEN_VERSION);
    ImGui::Text("Developed By: Cole Bishop");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "About");
    ImGui::Separator();
    ImGui::TextWrapped(
        "Brazen Benchmark is a lightweight synthetic benchmarking "
        "tool for CPU, RAM, GPU, and drive performance. It runs a set "
        "of self-contained workloads: integer math, floating point, "
        "prime sieving, hashing, sorting, sequential RAM bandwidth, a "
        "GPU ALU, texture, and fill-rate tests, and sequential disk "
        "write/read-back, and reports throughput scores you can "
        "compare against this machine over time, or against another "
        "machine running the same tests.\n\n"
        "CPU and RAM tests can each be run single-core, multi-core, or "
        "both, with a configurable duration; RAM also has a configurable "
        "buffer size. The Disk test instead moves a fixed amount of data "
        "(you choose how much, and which detected drive to use) and runs "
        "until it is done, so it has no duration. GPUs and drives "
        "are both fully enumerated in System Info; GPU selection is "
        "shown too, though only the GPU actually driving this window "
        "can be benchmarked. See the '?' on the GPU tab for why.\n\n"
        "Completed CPU results feed a composite score, normalized "
        "against illustrative baselines and mapped to a cat-themed "
        "rank from Alley Cat up to Cheetah (see the rank table below), "
        "a fun relative label, not a scientifically calibrated "
        "figure.\n\n"
        "The Results view keeps a history of every run for the current "
        "session, nothing is written to disk between launches, "
        "with a detail view and delete for individual results, plus a "
        "way to clear everything at once. The Log & Console view "
        "records what the app is doing as it happens (hardware "
        "detection, every run, warnings and errors) and accepts typed "
        "commands, and System Info shows what Brazen was able to "
        "detect about the machine it's running on.");
    ImGui::Spacing();
    if (ImGui::Button("View Rankings", ImVec2(AutoButtonWidth("View Rankings", 180.0f), 0)))
        ImGui::OpenPopup("Cat Rank Table##CatRankTable");
    DrawCatRankTablePopup();
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Built With");
    ImGui::Separator();
    ImGui::BulletText("Dear ImGui: desktop user interface");
    ImGui::BulletText("GLFW: window creation and input");
    ImGui::BulletText("OpenGL 3.3 core profile: GPU benchmark rendering");
    ImGui::BulletText("C++17 standard library and platform APIs: benchmark and hardware access");
    ImGui::TextWrapped(
        "Dear ImGui and GLFW are third-party components. Their source and "
        "license notices are included with the project distribution.");
}

// ---------------------------------------------------------------------
// Settings view
// ---------------------------------------------------------------------

void BrazenApp::DrawSettingsView() {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Display");
    ImGui::Separator();
    ImGui::Text("UI scale");
    ImGui::SetNextItemWidth(240);
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SliderFloat("##uiscale", &io.FontGlobalScale, 0.7f, 2.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) io.FontGlobalScale = 1.0f;
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Console");
    ImGui::Separator();
    ImGui::TextWrapped("Commands can be typed at the bottom of the Log & Console view, e.g. run cpu, status or help.");
    if (ImGui::Button("Open Log & Console", ImVec2(AutoButtonWidth("Open Log & Console", 180.0f), 0))) {
        m_currentView = SidebarView::Log;
        m_focusConsole = true;
    }
    ImGui::Spacing();

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Results");
    ImGui::Separator();
    ImGui::Checkbox("Ask for confirmation before clearing all results", &m_confirmBeforeClear);
}

} // namespace brazen
