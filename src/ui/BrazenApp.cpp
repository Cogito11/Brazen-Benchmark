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
#include "../core/RegisterTests.h"
#include "../core/tests/DiskIoTest.h"
#include "../core/tests/RamBandwidthTest.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
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

// Disk I/O write+read chunk size presets. 16 MB (index 1) matches
// DiskIoTest's own hardcoded default.
constexpr int kSsdChunkSizesMB[] = {4, 16, 64, 256};
const char* kSsdChunkSizeLabels[] = {"4 MB", "16 MB", "64 MB", "256 MB"};

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
    RegisterBuiltInTests();
    m_selected.assign(TestRegistry::Instance().Count(), false);
    // Default every test selected: selection on the CPU tab *is* the
    // primary way to choose what runs, so "Run Benchmark" with the
    // tab's defaults untouched should do the obviously-expected thing
    // (run everything in that category).
    std::fill(m_selected.begin(), m_selected.end(), true);
    m_hardwareInfo = QueryHardwareInfo();
    RefreshSystemInventory();
}

BrazenApp::~BrazenApp() = default;

void BrazenApp::SetGpuRunner(GpuTestRunner* runner) {
    m_gpuRunner = runner;
    if (m_gpuRunner && m_gpuRunner->IsSupported()) {
        m_hardwareInfo.gpuVendor = m_gpuRunner->GetVendor();
        m_hardwareInfo.gpuRenderer = m_gpuRunner->GetRenderer();
    }
}

void BrazenApp::RefreshSystemInventory() {
    m_allGpus = QueryAllGpus();
    m_allDrives = QueryAllDrives();

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
}

float BrazenApp::AutoButtonWidth(const char* label, float minWidth) const {
    float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFontSize();
    return std::max(width, minWidth);
}

void BrazenApp::AddExternalResult(const BenchmarkResult& result) {
    AddResultEntry(result);
}

void BrazenApp::AddResultEntry(const BenchmarkResult& result) {
    m_resultEntries.push_back({result, NowTimestampString()});
    m_resultSelected.push_back(false);
}

void BrazenApp::PollManager() {
    for (auto& r : m_manager.DrainResults()) {
        AddResultEntry(r);
        if (!r.cancelled) {
            auto& latestMap = (r.mode == RunMode::SingleCore) ? m_latestSingleCore : m_latestMultiCore;
            latestMap[r.testName] = r;
        }
    }

    for (auto& line : m_manager.DrainLog()) {
        m_log.push_back(line);
        while (m_log.size() > kMaxLogLines) m_log.pop_front();
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
        if (progress.busy) {
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
    if (ImGui::SmallButton("Cancel All")) {
        m_manager.CancelAll();
        if (m_gpuRunner) m_gpuRunner->Cancel();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void BrazenApp::DrawSidebar() {
    ImGui::Spacing();
    DrawSidebarButton("Benchmarks", SidebarView::Benchmarks);
    DrawSidebarButton("Results", SidebarView::Results);
    DrawSidebarButton("Log", SidebarView::Log);
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
        if (ImGui::BeginTabItem("SSD")) {
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
    for (size_t i = 0; i < reg.Count(); ++i) {
        if (reg.Samples()[i]->GetCategory() != TestCategory::Cpu) continue;
        if (!m_selected[i]) continue;
        if (m_cpuSettings.mode == CategoryRunMode::SingleCoreOnly || m_cpuSettings.mode == CategoryRunMode::Both)
            m_manager.Enqueue(i, RunMode::SingleCore, m_cpuSettings.durationSeconds);
        if (m_cpuSettings.mode == CategoryRunMode::MultiCoreOnly || m_cpuSettings.mode == CategoryRunMode::Both)
            m_manager.Enqueue(i, RunMode::MultiCore, m_cpuSettings.durationSeconds);
    }
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

    ImGui::Spacing();
    ImGui::Text("Threads");
    if (DrawHelpButton("ram_threads"))
        ImGui::TextWrapped(
            "Auto uses every logical core for the multi-core run. "
            "Override it to deliberately test with fewer threads.");
    ImGui::Checkbox("Use all cores (auto)", &m_ramSettings.autoThreadCount);
    if (!m_ramSettings.autoThreadCount) {
        ImGui::SetNextItemWidth(240);
        int maxThreads = std::max(1, static_cast<int>(m_hardwareInfo.logicalCores));
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
    if (ImGui::Button("Run Benchmark", ImVec2(AutoButtonWidth("Run Benchmark", 150.0f), 0)))
        StartRamBenchmark();
}

void BrazenApp::StartRamBenchmark() {
    size_t bufferBytes = static_cast<size_t>(kRamBufferSizesMB[m_ramSettings.bufferSizeIndex]) * 1024ull * 1024ull;
    unsigned threadOverride = m_ramSettings.autoThreadCount ? 0u
                                                             : static_cast<unsigned>(m_ramSettings.threadCountOverride);

    // Two separate instances rather than one shared_ptr reused across
    // both Enqueue calls: BenchmarkRunner treats the prototype as
    // read-only and Clone()s it per worker thread, so sharing would be
    // safe in principle, but each mode's job may run at a different time
    // and giving each its own instance avoids any doubt about that.
    if (m_ramSettings.mode == CategoryRunMode::SingleCoreOnly || m_ramSettings.mode == CategoryRunMode::Both)
        m_manager.Enqueue(std::make_shared<RamBandwidthTest>(bufferBytes), RunMode::SingleCore,
                           m_ramSettings.durationSeconds);
    if (m_ramSettings.mode == CategoryRunMode::MultiCoreOnly || m_ramSettings.mode == CategoryRunMode::Both)
        m_manager.Enqueue(std::make_shared<RamBandwidthTest>(bufferBytes), RunMode::MultiCore,
                           m_ramSettings.durationSeconds, threadOverride);

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
    if (!m_gpuRunner || !m_gpuRunner->IsSupported()) return;
    int w = kGpuResolutions[m_gpuSettings.resolutionIndex][0];
    int h = kGpuResolutions[m_gpuSettings.resolutionIndex][1];
    m_gpuRunner->SetResolution(w, h); // no-op if a run is already in progress
    unsigned workloadMask = (m_gpuSettings.runAlu ? 1u : 0u) |
                            (m_gpuSettings.runTexture ? 2u : 0u) |
                            (m_gpuSettings.runFill ? 4u : 0u);
    m_gpuRunner->RequestRun(m_gpuSettings.durationSeconds, workloadMask);
    m_currentView = SidebarView::Results;
}

void BrazenApp::StartSsdBenchmark() {
    if (m_ssdSettings.selectedDriveIndex < 0 ||
        m_ssdSettings.selectedDriveIndex >= static_cast<int>(m_allDrives.size()))
        return;
    const auto& drive = m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)];
    if (drive.path.empty()) return;

    size_t chunkBytes = static_cast<size_t>(kSsdChunkSizesMB[m_ssdSettings.chunkSizeIndex]) * 1024ull * 1024ull;
    std::filesystem::path targetDir(drive.path);

    // Write and Read are enqueued as separate DiskIoTest instances (see
    // DiskIoMode) so they show up as two distinct results rather than
    // one blended write+read number. Write and read throughput can
    // differ meaningfully on the same device. Each mode/operation
    // combination gets its own instance, same reasoning as
    // StartRamBenchmark: Enqueue's prototype is Clone()'d by
    // BenchmarkRunner, but giving each job its own instance avoids any
    // doubt about sharing state across jobs that may run at different
    // times.
    auto enqueueOp = [&](DiskIoMode op) {
        if (m_ssdSettings.mode == CategoryRunMode::SingleCoreOnly || m_ssdSettings.mode == CategoryRunMode::Both)
            m_manager.Enqueue(std::make_shared<DiskIoTest>(targetDir, op, chunkBytes), RunMode::SingleCore,
                               m_ssdSettings.durationSeconds);
        if (m_ssdSettings.mode == CategoryRunMode::MultiCoreOnly || m_ssdSettings.mode == CategoryRunMode::Both)
            m_manager.Enqueue(std::make_shared<DiskIoTest>(targetDir, op, chunkBytes), RunMode::MultiCore,
                               m_ssdSettings.durationSeconds);
    };
    enqueueOp(DiskIoMode::Write);
    enqueueOp(DiskIoMode::Read);

    m_currentView = SidebarView::Results;
}

void BrazenApp::DrawSsdBenchmarkTab() {
    ImGui::Spacing();
    ImGui::TextWrapped(
        "Running this produces two separate results, Disk Write and "
        "Disk Read, since throughput can differ between the two.");
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
                if (ImGui::Selectable(label.c_str(), isSelected) && writable)
                    m_ssdSettings.selectedDriveIndex = i;
                if (isSelected) ImGui::SetItemDefaultFocus();
                if (!writable) ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Chunk size");
    if (DrawHelpButton("ssd_chunk"))
        ImGui::TextWrapped(
            "How much data is written or read per timed step. Larger "
            "chunks better reflect sustained sequential throughput and "
            "also help the Disk Read result better reflect the actual "
            "drive rather than the OS's file cache (Brazen asks the "
            "kernel to drop the test file from cache before each "
            "timed read on Linux; Windows/macOS have no equivalent "
            "here, so treat Read numbers there with a bit of "
            "skepticism, especially at small chunk sizes).");
    ImGui::SetNextItemWidth(240);
    ImGui::Combo("##ssdchunk", &m_ssdSettings.chunkSizeIndex, kSsdChunkSizeLabels,
                  static_cast<int>(sizeof(kSsdChunkSizeLabels) / sizeof(kSsdChunkSizeLabels[0])));

    ImGui::Spacing();
    ImGui::Text("Mode");
    if (DrawHelpButton("ssd_mode"))
        ImGui::TextWrapped(
            "Single-Core writes/reads from one thread. Multi-Core runs "
            "several threads concurrently, each against its own "
            "temporary file on the same drive, to measure aggregate "
            "throughput under concurrent I/O. Both runs each once.");
    int mode = static_cast<int>(m_ssdSettings.mode);
    ImGui::RadioButton("Single-Core", &mode, static_cast<int>(CategoryRunMode::SingleCoreOnly));
    ImGui::SameLine();
    ImGui::RadioButton("Multi-Core", &mode, static_cast<int>(CategoryRunMode::MultiCoreOnly));
    ImGui::SameLine();
    ImGui::RadioButton("Both", &mode, static_cast<int>(CategoryRunMode::Both));
    m_ssdSettings.mode = static_cast<CategoryRunMode>(mode);

    ImGui::Spacing();
    ImGui::Text("Duration per test");
    if (DrawHelpButton("ssd_duration"))
        ImGui::TextWrapped("How long the timed write/read-back loop runs, per mode.");
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##ssdduration", &m_ssdSettings.durationSeconds, 1.0f, 30.0f, "%.0f s");

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
    bool canStart = m_ssdSettings.selectedDriveIndex >= 0 &&
                     m_ssdSettings.selectedDriveIndex < static_cast<int>(m_allDrives.size()) &&
                     !m_allDrives[static_cast<size_t>(m_ssdSettings.selectedDriveIndex)].path.empty();
    if (!canStart) ImGui::BeginDisabled();
    if (ImGui::Button("Run Benchmark", ImVec2(AutoButtonWidth("Run Benchmark", 150.0f), 0)))
        StartSsdBenchmark();
    if (!canStart) ImGui::EndDisabled();
    if (!canStart) {
        ImGui::SameLine();
        ImGui::TextDisabled("(select a writable drive)");
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
        if (entry.result.cancelled) continue;
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
        drawAverageRow("Storage", storageResults, "Run SSD benchmark");

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
                     r.affinityPinned ? "Yes" : "No", r.cancelled ? "Cancelled" : "Done");
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
            if (r.cancelled)
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
    ImGui::Text("Status: %s", r.cancelled ? "Cancelled" : "Completed");
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
    if (isCpuTest && !r.cancelled) {
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
    m_resultEntries.swap(kept);
    m_resultSelected.swap(keptSel);
    m_expandedResultIndex = -1;
}

void BrazenApp::ClearAllResults() {
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
            joined += line;
            joined += "\n";
        }
        ImGui::SetClipboardText(joined.c_str());
    }
    ImGui::Separator();
    for (const auto& line : m_log)
        ImGui::TextUnformatted(line.c_str());
    if (!m_log.empty())
        ImGui::SetScrollHereY(1.0f);
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
        "CPU, RAM, and SSD tests can each be run single-core, "
        "multi-core, or both, with a configurable duration and, for "
        "RAM and SSD, a configurable buffer/chunk size. The SSD tab "
        "lets you pick which detected drive to target. GPUs and drives "
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
        "way to clear everything at once. The Log view mirrors what "
        "each benchmark reports as it happens, and System Info shows "
        "what Brazen was able to detect about the machine it's running "
        "on.");
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

    ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Results");
    ImGui::Separator();
    ImGui::Checkbox("Ask for confirmation before clearing all results", &m_confirmBeforeClear);
}

} // namespace brazen
