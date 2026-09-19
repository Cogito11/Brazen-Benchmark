#pragma once
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
    #include <intrin.h>
#elif defined(__APPLE__)
    #include <sys/mount.h>
    #include <sys/sysctl.h>
    #include <sys/utsname.h>
#else
    #include <sys/utsname.h>
#endif

// CPUID is an x86-only instruction; instruction-set detection is simply
// unavailable on ARM (e.g. Apple Silicon Macs) and falls back to saying
// so rather than guessing.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #define BRAZEN_X86_CPUID 1
    #if !defined(_MSC_VER)
        #include <cpuid.h>
    #endif
#endif

namespace brazen {

struct HardwareInfo {
    std::string cpuModel = "Unknown CPU";
    std::string osName = "Unknown OS";
    unsigned logicalCores = 0;
    unsigned physicalCores = 0; // best-effort; falls back to logicalCores if it can't be determined

    unsigned long long totalRamBytes = 0; // 0 means "couldn't determine"

    // GPU info can't be queried here. It requires an active OpenGL
    // context, which doesn't exist yet when HardwareInfo is normally
    // gathered at BrazenApp construction. BrazenApp::SetGpuRunner()
    // copies these in from GpuTestRunner once the GL context (and thus
    // GpuTestRunner::Init()) has run.
    std::string gpuVendor;   // e.g. "NVIDIA Corporation"
    std::string gpuRenderer; // e.g. "NVIDIA GeForce RTX 4070/PCIe/SSE2"

    // Best-effort motherboard identification (see QueryMotherboardInto:
    // Windows reads this from the registry, not WMI).
    std::string motherboardVendor = "Unknown";
    std::string motherboardModel = "Unknown";

    // Best-effort info about the drive holding the app's current working
    // directory, not a full enumeration of every storage device in the
    // system. Model name is only resolved on Linux today; capacity/free
    // space come from std::filesystem::space() and work everywhere.
    std::string primaryDiskModel = "Unknown";
    unsigned long long primaryDiskTotalBytes = 0;
    unsigned long long primaryDiskFreeBytes = 0;

    // Detailed OS/system identification, in the spirit of what tools
    // like Geekbench show. osName above stays a plain category
    // ("Linux"/"Windows"/"macOS") since other code already switches on
    // it; these carry the human-readable detail.
    std::string osVersion = "Unknown";      // e.g. "Linux Mint 22.3" / "Windows 11 Pro 23H2 (Build 22631)" / "macOS 14.5"
    std::string kernelVersion = "Unknown";  // e.g. "Linux 6.8.0-31-generic x86_64" / "Darwin 23.5.0"
    std::string systemVendor = "Unknown";   // e.g. "ASUSTeK COMPUTER INC."
    std::string systemModel = "Unknown";    // e.g. "ASUSLaptop_Q540VJ"
    std::string biosVendor = "Unknown";
    std::string biosVersion = "Unknown";

    // CPU identification/topology beyond the model name above.
    std::string cpuIdentifier = "Unknown";      // e.g. "GenuineIntel Family 6 Model 186 Stepping 2"
    double cpuMaxFrequencyGHz = 0.0;            // 0 = couldn't determine. This is a ceiling (turbo/boost clock as
                                                 // reported by the OS), NOT a verified base clock. See
                                                 // QueryCpuMaxFrequencyGHz for why those aren't the same thing.
    std::string cpuInstructionSets = "Unknown"; // space-separated feature flags, or an explanation if unavailable

    struct CpuCacheInfo {
        // As reported for CPU core 0. On hybrid architectures (e.g.
        // Intel's performance/efficiency core designs), cache sizes can
        // genuinely differ between core types. This won't capture
        // that asymmetry, it just reports whichever core the OS
        // considers "cpu0".
        std::string l1Instruction = "Unknown";
        std::string l1Data = "Unknown";
        std::string l2 = "Unknown";
        std::string l3 = "Unknown";
    } cpuCache;
};

// One detected storage volume, from QueryAllDrives(). What "a drive"
// means here differs a bit by platform: on Linux it's a physical block
// device (paired with whichever mounted partition on it looks
// writable); on Windows and macOS it's a mounted volume/logical drive,
// since that's what's cheaply enumerable without extra linked
// dependencies (WMI / IOKit). path is empty if no writable location
// could be found for this entry (e.g. an unmounted or read-only device)
// BrazenApp's SSD tab disables selecting those for a benchmark run.
struct DriveInfo {
    std::string label;                    // display name (model, volume label, or drive letter)
    std::string path;                     // writable directory to target for I/O; empty = not testable
    unsigned long long totalBytes = 0;
    unsigned long long freeBytes = 0;
    bool isPrimary = false;               // true for the drive holding the app's working directory
};

// One detected GPU, from QueryAllGpus(). This is a plain OS-level
// adapter enumeration and is independent of GpuTestRunner/the active GL
// context. It exists so System Info and the GPU benchmark tab can
// show "here's everything installed", separately from "here's what's
// actually being benchmarked" (see BrazenApp::DrawGpuBenchmarkTab for
// why only the active one is selectable).
struct GpuInfo {
    std::string name;
};

namespace detail {

inline std::string TrimCopy(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

#if defined(_WIN32)
// Reads a single REG_SZ value, returning "" if the key/value doesn't
// exist rather than throwing. Callers treat an empty result as "not
// available" the same way they already do for sysfs/proc reads on
// Linux. Used for several of the queries below; none of them need WMI,
// since Windows exposes a surprising amount of DMI-equivalent data
// (system/board/BIOS manufacturer and product strings, CPU identifier)
// directly under HKLM\HARDWARE\DESCRIPTION\System.
inline std::string ReadRegistryString(HKEY root, const char* subkey, const char* value) {
    HKEY hKey;
    std::string result;
    if (RegOpenKeyExA(root, subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buf[256];
        DWORD size = sizeof(buf);
        if (RegQueryValueExA(hKey, value, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS)
            result = TrimCopy(std::string(buf));
        RegCloseKey(hKey);
    }
    return result;
}
#endif

inline std::string QueryCpuModelName() {
#if defined(_WIN32)
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                       "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buf[256];
        DWORD size = sizeof(buf);
        std::string result;
        if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr,
                              reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS) {
            result = TrimCopy(std::string(buf));
        }
        RegCloseKey(hKey);
        if (!result.empty()) return result;
    }
    return "Unknown CPU";
#elif defined(__APPLE__)
    char buf[256];
    size_t size = sizeof(buf);
    if (sysctlbyname("machdep.cpu.brand_string", buf, &size, nullptr, 0) == 0)
        return TrimCopy(std::string(buf));
    return "Unknown CPU";
#else
    // Linux (and other /proc-based systems): x86 kernels expose a
    // "model name" line; some ARM kernels only expose "Hardware"/"Model".
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("model name", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) return TrimCopy(line.substr(pos + 1));
        }
    }
    file.clear();
    file.seekg(0);
    while (std::getline(file, line)) {
        if (line.rfind("Hardware", 0) == 0 || line.rfind("Model", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) return TrimCopy(line.substr(pos + 1));
        }
    }
    return "Unknown CPU";
#endif
}

// Best-effort physical core count. Returns 0 if it can't be determined,
// in which case the caller should fall back to logical core count.
inline unsigned QueryPhysicalCoreCount() {
#if defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    if (length == 0) return 0;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(
        length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(buffer.data(), &length)) return 0;
    unsigned count = 0;
    for (auto& info : buffer)
        if (info.Relationship == RelationProcessorCore) count++;
    return count;
#elif defined(__APPLE__)
    int count = 0;
    size_t size = sizeof(count);
    if (sysctlbyname("hw.physicalcpu", &count, &size, nullptr, 0) == 0)
        return static_cast<unsigned>(count);
    return 0;
#else
    // Count unique (physical id, core id) pairs from /proc/cpuinfo.
    std::ifstream file("/proc/cpuinfo");
    std::string line;
    std::set<std::pair<int, int>> uniqueCores;
    int physId = 0, coreId = 0;
    bool havePhys = false, haveCore = false;
    while (std::getline(file, line)) {
        if (line.rfind("physical id", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                physId = std::atoi(line.substr(pos + 1).c_str());
                havePhys = true;
            }
        } else if (line.rfind("core id", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                coreId = std::atoi(line.substr(pos + 1).c_str());
                haveCore = true;
            }
        }
        if (havePhys && haveCore) {
            uniqueCores.insert({physId, coreId});
            havePhys = haveCore = false;
        }
    }
    return static_cast<unsigned>(uniqueCores.size());
#endif
}

// Best-effort total physical RAM in bytes. Returns 0 if it can't be
// determined.
inline unsigned long long QueryTotalRamBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<unsigned long long>(status.ullTotalPhys);
    return 0;
#elif defined(__APPLE__)
    unsigned long long bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0)
        return bytes;
    return 0;
#else
    // Linux: parse /proc/meminfo's "MemTotal" line (reported in kB).
    std::ifstream file("/proc/meminfo");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            auto pos = line.find_first_of("0123456789");
            if (pos != std::string::npos)
                return static_cast<unsigned long long>(std::atoll(line.c_str() + pos)) * 1024ull;
        }
    }
    return 0;
#endif
}

// Best-effort motherboard vendor/model. Windows reads it from
// HKLM\HARDWARE\DESCRIPTION\System\BIOS (BaseBoardManufacturer/
// BaseBoardProduct) rather than WMI; Linux and macOS have their own
// direct answers.
inline void QueryMotherboardInto(std::string& vendor, std::string& model) {
    vendor = "Unknown";
    model = "Unknown";
#if defined(_WIN32)
    std::string v = ReadRegistryString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "BaseBoardManufacturer");
    std::string m = ReadRegistryString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "BaseBoardProduct");
    if (!v.empty()) vendor = v;
    if (!m.empty()) model = m;
#elif defined(__APPLE__)
    char buf[256];
    size_t size = sizeof(buf);
    if (sysctlbyname("hw.model", buf, &size, nullptr, 0) == 0) {
        vendor = "Apple";
        model = TrimCopy(std::string(buf));
    }
#else
    std::ifstream vendorFile("/sys/class/dmi/id/board_vendor");
    std::ifstream modelFile("/sys/class/dmi/id/board_name");
    std::string v, m;
    if (vendorFile.good()) std::getline(vendorFile, v);
    if (modelFile.good()) std::getline(modelFile, m);
    v = TrimCopy(v);
    m = TrimCopy(m);
    if (!v.empty()) vendor = v;
    if (!m.empty()) model = m;
#endif
}

// Best-effort overall system identification: the laptop/PC model
// itself, distinct from the motherboard (which can differ, e.g. a
// laptop's board name vs. the manufacturer's marketing model name).
inline void QuerySystemModelInto(std::string& vendor, std::string& model) {
    vendor = "Unknown";
    model = "Unknown";
#if defined(_WIN32)
    std::string v = ReadRegistryString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemManufacturer");
    std::string m = ReadRegistryString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "SystemProductName");
    if (!v.empty()) vendor = v;
    if (!m.empty()) model = m;
#elif defined(__APPLE__)
    char buf[256];
    size_t size = sizeof(buf);
    if (sysctlbyname("hw.model", buf, &size, nullptr, 0) == 0) {
        vendor = "Apple";
        model = TrimCopy(std::string(buf));
    }
#else
    std::ifstream vendorFile("/sys/class/dmi/id/sys_vendor");
    std::ifstream modelFile("/sys/class/dmi/id/product_name");
    std::string v, m;
    if (vendorFile.good()) std::getline(vendorFile, v);
    if (modelFile.good()) std::getline(modelFile, m);
    v = TrimCopy(v);
    m = TrimCopy(m);
    if (!v.empty()) vendor = v;
    if (!m.empty()) model = m;
#endif
}

// Best-effort BIOS/firmware vendor and version.
inline void QueryBiosInto(std::string& vendor, std::string& version) {
    vendor = "Unknown";
    version = "Unknown";
#if defined(_WIN32)
    std::string v = ReadRegistryString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "BIOSVendor");
    std::string ver = ReadRegistryString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", "BIOSVersion");
    if (!v.empty()) vendor = v;
    if (!ver.empty()) version = ver;
#elif !defined(__APPLE__)
    // macOS's BIOS-equivalent (EFI firmware) doesn't have as clean a
    // userspace query as DMI/registry, so this is Linux-only for now.
    std::ifstream vendorFile("/sys/class/dmi/id/bios_vendor");
    std::ifstream versionFile("/sys/class/dmi/id/bios_version");
    std::string v, ver;
    if (vendorFile.good()) std::getline(vendorFile, v);
    if (versionFile.good()) std::getline(versionFile, ver);
    v = TrimCopy(v);
    ver = TrimCopy(ver);
    if (!v.empty()) vendor = v;
    if (!ver.empty()) version = ver;
#endif
}

// Human-readable OS name + version. osName (on HardwareInfo) stays a
// plain category since other code switches on it; this is the detailed
// string for display.
inline std::string QueryOsVersion() {
#if defined(_WIN32)
    std::string productName = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName");
    std::string displayVersion = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion");
    std::string buildNumber = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber");
    std::string result = productName.empty() ? "Windows" : productName;
    if (!displayVersion.empty()) result += " " + displayVersion;
    if (!buildNumber.empty()) result += " (Build " + buildNumber + ")";
    return result;
#elif defined(__APPLE__)
    char buf[64];
    size_t size = sizeof(buf);
    if (sysctlbyname("kern.osproductversion", buf, &size, nullptr, 0) == 0)
        return std::string("macOS ") + buf;
    return "macOS";
#else
    // Most distros (including Linux Mint) provide a friendly PRETTY_NAME
    // here; falls back to plain "Linux" if the file is missing/unusual.
    std::ifstream file("/etc/os-release");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;
        std::string val = line.substr(12);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        val = TrimCopy(val);
        if (!val.empty()) return val;
    }
    return "Linux";
#endif
}

// Kernel/build version string, separate from the friendly OS version
// above (e.g. "Linux Mint 22.3" vs. the actual kernel "6.8.0-31-generic").
inline std::string QueryKernelVersion() {
#if defined(_WIN32)
    std::string buildNumber = ReadRegistryString(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuildNumber");
    return buildNumber.empty() ? "Unknown" : ("Windows NT Build " + buildNumber);
#else
    struct utsname u{};
    if (uname(&u) != 0) return "Unknown";
    return std::string(u.sysname) + " " + u.release + " " + u.machine;
#endif
}

// "GenuineIntel Family 6 Model 186 Stepping 2" style identifier. On
// Linux, parsed from /proc/cpuinfo by comparing the trimmed key before
// the colon exactly, since "model" and "model name" are different fields
// there, and a naive rfind("model", 0) prefix match would confuse them.
inline std::string QueryCpuIdentifier() {
#if defined(_WIN32)
    std::string id = ReadRegistryString(HKEY_LOCAL_MACHINE,
                                         "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "Identifier");
    return id.empty() ? "Unknown" : id;
#elif defined(__APPLE__)
    // Apple Silicon has no x86-style family/model/stepping concept, and
    // even on Intel Macs there's no clean userspace query as direct as
    // Linux's /proc/cpuinfo or Windows' registry value. The brand
    // string (already captured as cpuModel) is the most useful "identity"
    // string available here.
    return "See CPU model above (identifier breakdown not available on macOS)";
#else
    std::ifstream file("/proc/cpuinfo");
    std::string line, vendorId, family, model, stepping;
    while (std::getline(file, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = TrimCopy(line.substr(0, colon));
        std::string val = TrimCopy(line.substr(colon + 1));
        if (key == "vendor_id") vendorId = val;
        else if (key == "cpu family") family = val;
        else if (key == "model") model = val;
        else if (key == "stepping") stepping = val;
        if (!vendorId.empty() && !family.empty() && !model.empty() && !stepping.empty()) break;
    }
    if (vendorId.empty() && family.empty() && model.empty()) return "Unknown";
    std::string result = vendorId;
    if (!family.empty()) result += (result.empty() ? "" : " ") + std::string("Family ") + family;
    if (!model.empty()) result += " Model " + model;
    if (!stepping.empty()) result += " Stepping " + stepping;
    return result;
#endif
}

// Best-effort CPU frequency ceiling, in GHz. Deliberately NOT called
// "base frequency" anywhere in this codebase: the cheap-to-query values
// available here (Linux's cpuinfo_max_freq, Windows' "~MHz" registry
// value) reflect the maximum/turbo frequency the OS is aware of, not
// the CPU's actual rated base clock. Those can differ substantially
// on modern CPUs with aggressive boost behavior, and getting the true
// base clock generally needs reading a model-specific register, which
// this header doesn't do.
inline double QueryCpuMaxFrequencyGHz() {
#if defined(_WIN32)
    HKEY hKey;
    double result = 0.0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                       0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD mhz = 0, size = sizeof(mhz);
        if (RegQueryValueExA(hKey, "~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &size) == ERROR_SUCCESS)
            result = static_cast<double>(mhz) / 1000.0;
        RegCloseKey(hKey);
    }
    return result;
#elif defined(__APPLE__)
    unsigned long long hz = 0;
    size_t size = sizeof(hz);
    if (sysctlbyname("hw.cpufrequency_max", &hz, &size, nullptr, 0) == 0 && hz > 0)
        return static_cast<double>(hz) / 1e9;
    // Apple Silicon doesn't expose hw.cpufrequency* at all (frequency
    // scaling is managed very differently there). 0 means "not
    // available", same convention as everywhere else in this file.
    return 0.0;
#else
    std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!f.good()) return 0.0;
    long long khz = 0;
    f >> khz;
    return khz > 0 ? static_cast<double>(khz) / 1'000'000.0 : 0.0;
#endif
}

inline std::string FormatCacheSize(unsigned long long bytes) {
    if (bytes == 0) return "Unknown";
    if (bytes < 1024ull * 1024ull) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        return buf;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

// Parses a sysfs cache "size" file (format is normally e.g. "48K", but
// this is lenient about the suffix) into a byte count.
inline unsigned long long ParseSysfsCacheSizeToBytes(const std::string& raw) {
    if (raw.empty()) return 0;
    size_t i = 0;
    while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) ++i;
    if (i == 0) return 0;
    unsigned long long value = std::strtoull(raw.substr(0, i).c_str(), nullptr, 10);
    if (i < raw.size() && (raw[i] == 'K' || raw[i] == 'k')) return value * 1024ull;
    if (i < raw.size() && (raw[i] == 'M' || raw[i] == 'm')) return value * 1024ull * 1024ull;
    return value; // no recognized suffix, assume it's already bytes
}

// Fills in L1i/L1d/L2/L3 sizes as reported for CPU core 0 (see the
// caveat on HardwareInfo::CpuCacheInfo about hybrid architectures).
inline void QueryCpuCacheInto(HardwareInfo::CpuCacheInfo& cache) {
#if defined(_WIN32)
    DWORD length = 0;
    GetLogicalProcessorInformation(nullptr, &length);
    if (length == 0) return;
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> buffer(length / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    if (!GetLogicalProcessorInformation(buffer.data(), &length)) return;
    for (auto& info : buffer) {
        if (info.Relationship != RelationCache) continue;
        const auto& c = info.Cache;
        std::string size = FormatCacheSize(c.Size);
        if (c.Level == 1 && c.Type == CacheInstruction) cache.l1Instruction = size;
        else if (c.Level == 1 && c.Type == CacheData) cache.l1Data = size;
        else if (c.Level == 2) cache.l2 = size;
        else if (c.Level == 3) cache.l3 = size;
    }
#elif defined(__APPLE__)
    auto readCache = [](const char* key) -> unsigned long long {
        unsigned long long bytes = 0;
        size_t size = sizeof(bytes);
        return sysctlbyname(key, &bytes, &size, nullptr, 0) == 0 ? bytes : 0;
    };
    cache.l1Instruction = FormatCacheSize(readCache("hw.l1icachesize"));
    cache.l1Data = FormatCacheSize(readCache("hw.l1dcachesize"));
    cache.l2 = FormatCacheSize(readCache("hw.l2cachesize"));
    cache.l3 = FormatCacheSize(readCache("hw.l3cachesize"));
#else
    std::error_code ec;
    const char* base = "/sys/devices/system/cpu/cpu0/cache";
    if (!std::filesystem::exists(base, ec) || ec) return;
    for (auto& entry : std::filesystem::directory_iterator(base, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("index", 0) != 0) continue;

        std::ifstream levelFile(entry.path() / "level");
        std::ifstream typeFile(entry.path() / "type");
        std::ifstream sizeFile(entry.path() / "size");
        int level = 0;
        std::string type, sizeRaw;
        if (levelFile.good()) levelFile >> level;
        if (typeFile.good()) std::getline(typeFile, type);
        if (sizeFile.good()) std::getline(sizeFile, sizeRaw);
        type = TrimCopy(type);

        std::string size = FormatCacheSize(ParseSysfsCacheSizeToBytes(TrimCopy(sizeRaw)));
        if (level == 1 && type == "Instruction") cache.l1Instruction = size;
        else if (level == 1 && type == "Data") cache.l1Data = size;
        else if (level == 2) cache.l2 = size;
        else if (level == 3) cache.l3 = size;
    }
#endif
}

// Raw CPUID leaf/subleaf query, uniform across MSVC (__cpuidex) and
// GCC/Clang (__cpuid_count) so the feature-bit tests below don't need
// their own per-compiler branches.
inline void CpuIdRaw(unsigned leaf, unsigned subleaf, unsigned regs[4]) {
#if defined(BRAZEN_X86_CPUID)
    #if defined(_MSC_VER)
        int out[4];
        __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
        regs[0] = static_cast<unsigned>(out[0]);
        regs[1] = static_cast<unsigned>(out[1]);
        regs[2] = static_cast<unsigned>(out[2]);
        regs[3] = static_cast<unsigned>(out[3]);
    #else
        __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
    #endif
#else
    regs[0] = regs[1] = regs[2] = regs[3] = 0;
#endif
}

// Space-separated x86 instruction-set feature flags, read directly from
// CPUID leaves 1 and 7 rather than parsed from a platform-specific text
// format. This is the one piece of hardware info in this file that's
// genuinely identical logic across Linux/Windows/Intel-macOS, since
// it's just reading CPU registers rather than an OS-provided string.
// Unavailable (rather than guessed at) on non-x86 hosts, e.g. Apple
// Silicon.
inline std::string QueryInstructionSets() {
#if !defined(BRAZEN_X86_CPUID)
    return "Not applicable (non-x86 architecture)";
#else
    unsigned regs[4];
    CpuIdRaw(0, 0, regs);
    unsigned maxLeaf = regs[0];
    if (maxLeaf < 1) return "Unknown";

    CpuIdRaw(1, 0, regs);
    unsigned ecx1 = regs[2];
    unsigned edx1 = regs[3];

    unsigned ebx7 = 0, ecx7 = 0;
    if (maxLeaf >= 7) {
        CpuIdRaw(7, 0, regs);
        ebx7 = regs[1];
        ecx7 = regs[2];
    }

    std::vector<const char*> feats;
    auto add = [&](bool present, const char* name) { if (present) feats.push_back(name); };

    add((edx1 >> 26) & 1, "sse2");
    add((ecx1 >> 0) & 1,  "sse3");
    add((ecx1 >> 1) & 1,  "pclmulqdq");
    add((ecx1 >> 9) & 1,  "ssse3");
    add((ecx1 >> 12) & 1, "fma3");
    add((ecx1 >> 19) & 1, "sse4.1");
    add((ecx1 >> 20) & 1, "sse4.2");
    add((ecx1 >> 25) & 1, "aes-ni");
    add((ecx1 >> 28) & 1, "avx");
    add((ecx1 >> 29) & 1, "f16c");
    add((ebx7 >> 3) & 1,  "bmi1");
    add((ebx7 >> 5) & 1,  "avx2");
    add((ebx7 >> 8) & 1,  "bmi2");
    add((ebx7 >> 16) & 1, "avx512f");
    add((ebx7 >> 29) & 1, "sha");
    add((ecx7 >> 9) & 1,  "vaes");

    if (feats.empty()) return "Unknown";
    std::string result;
    for (size_t i = 0; i < feats.size(); ++i) {
        if (i) result += " ";
        result += feats[i];
    }
    return result;
#endif
}

// Best-effort info about the drive holding the current working
// directory. Capacity/free space use std::filesystem::space(), which
// works cross-platform; the model name lookup is Linux-only (checks a
// couple of common device names under /sys/block rather than doing a
// full block-device enumeration).
inline void QueryPrimaryDiskInto(std::string& model, unsigned long long& totalBytes,
                                  unsigned long long& freeBytes) {
    model = "Unknown";
    totalBytes = 0;
    freeBytes = 0;

#if !defined(_WIN32) && !defined(__APPLE__)
    for (const char* dev : {"nvme0n1", "sda", "vda", "mmcblk0"}) {
        std::ifstream f(std::string("/sys/block/") + dev + "/device/model");
        if (!f.good()) continue;
        std::string m;
        std::getline(f, m);
        m = TrimCopy(m);
        if (!m.empty()) {
            model = m;
            break;
        }
    }
#endif

    std::error_code ec;
    auto space = std::filesystem::space(std::filesystem::current_path(), ec);
    if (!ec) {
        totalBytes = static_cast<unsigned long long>(space.capacity);
        freeBytes = static_cast<unsigned long long>(space.available);
    }
}

// ---- Drive enumeration ----

#if !defined(_WIN32) && !defined(__APPLE__)
// Parses /proc/mounts into (device name without "/dev/", mount point)
// pairs, skipping pseudo-filesystems (tmpfs, proc, etc.) that don't
// start with "/dev/".
inline std::string UnescapeMountFieldLinux(const std::string& field) {
    std::string result;
    result.reserve(field.size());
    for (size_t i = 0; i < field.size(); ++i) {
        if (field[i] == '\\' && i + 3 < field.size() &&
            field[i + 1] >= '0' && field[i + 1] <= '7' &&
            field[i + 2] >= '0' && field[i + 2] <= '7' &&
            field[i + 3] >= '0' && field[i + 3] <= '7') {
            int value = (field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0');
            result.push_back(static_cast<char>(value));
            i += 3;
        } else {
            result.push_back(field[i]);
        }
    }
    return result;
}

inline std::vector<std::pair<std::string, std::string>> ParseMountedPartitionsLinux() {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream file("/proc/mounts");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string dev, mnt, fstype;
        if (!(iss >> dev >> mnt >> fstype)) continue;
        if (dev.rfind("/dev/", 0) != 0) continue;
        out.emplace_back(UnescapeMountFieldLinux(dev.substr(5)), UnescapeMountFieldLinux(mnt));
    }
    return out;
}

// Maps a partition device name to the whole-disk device it belongs to,
// e.g. "sda1" -> "sda", "nvme0n1p1" -> "nvme0n1", "mmcblk0p1" ->
// "mmcblk0". Falls back to stripping trailing digits for anything that
// doesn't match the nvme/mmcblk "...pN" naming convention.
inline std::string BaseDeviceNameLinux(const std::string& partitionName) {
    if (partitionName.rfind("nvme", 0) == 0 || partitionName.rfind("mmcblk", 0) == 0) {
        auto pPos = partitionName.rfind('p');
        if (pPos != std::string::npos && pPos + 1 < partitionName.size()) {
            bool restIsDigits = true;
            for (size_t i = pPos + 1; i < partitionName.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(partitionName[i]))) { restIsDigits = false; break; }
            if (restIsDigits) return partitionName.substr(0, pPos);
        }
    }
    size_t end = partitionName.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(partitionName[end - 1]))) --end;
    return partitionName.substr(0, end);
}

inline std::vector<DriveInfo> QueryAllDrivesLinux() {
    std::vector<DriveInfo> out;
    std::error_code ec;
    if (!std::filesystem::exists("/sys/block", ec) || ec) return out;

    auto mounts = ParseMountedPartitionsLinux();

    for (auto& entry : std::filesystem::directory_iterator("/sys/block", ec)) {
        std::string dev = entry.path().filename().string();
        // loopN (loop devices), ramN (ramdisks), srN (optical) aren't
        // meaningful "drives" to list or benchmark.
        if (dev.rfind("loop", 0) == 0 || dev.rfind("ram", 0) == 0 || dev.rfind("sr", 0) == 0) continue;

        DriveInfo info;

        std::ifstream modelFile(entry.path() / "device" / "model");
        std::string model;
        if (modelFile.good()) std::getline(modelFile, model);
        model = TrimCopy(model);
        info.label = model.empty() ? dev : model;

        std::ifstream sizeFile(entry.path() / "size");
        unsigned long long sectors = 0;
        if (sizeFile.good()) sizeFile >> sectors;
        info.totalBytes = sectors * 512ull; // /sys/block/*/size is always in 512-byte sectors

        // Find a mounted partition living on this whole disk so we have
        // somewhere writable to point a disk I/O test at; prefer "/" if
        // it happens to be one of them, since it's the most likely to
        // still be present/writable.
        for (auto& [partName, mountPoint] : mounts) {
            if (BaseDeviceNameLinux(partName) != dev) continue;
            if (info.path.empty() || mountPoint == "/") info.path = mountPoint;
        }

        if (!info.path.empty()) {
            auto space = std::filesystem::space(info.path, ec);
            if (!ec) info.freeBytes = static_cast<unsigned long long>(space.available);
        }

        out.push_back(std::move(info));
    }
    return out;
}

#elif defined(_WIN32)

inline std::vector<DriveInfo> QueryAllDrivesWindows() {
    std::vector<DriveInfo> out;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        char root[4] = {static_cast<char>('A' + i), ':', '\\', '\0'};
        UINT type = GetDriveTypeA(root);
        // Fixed and removable only. Skip network shares/optical
        // drives/unknowns, which aren't meaningful benchmark targets.
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;

        ULARGE_INTEGER freeBytes{}, totalBytes{};
        if (!GetDiskFreeSpaceExA(root, nullptr, &totalBytes, &freeBytes)) continue;

        char volumeName[MAX_PATH + 1] = {0};
        GetVolumeInformationA(root, volumeName, sizeof(volumeName), nullptr, nullptr, nullptr, nullptr, 0);

        DriveInfo info;
        info.label = volumeName[0] ? (std::string(volumeName) + " (" + root + ")") : std::string(root);
        info.path = root;
        info.totalBytes = totalBytes.QuadPart;
        info.freeBytes = freeBytes.QuadPart;
        out.push_back(std::move(info));
    }
    return out;
}

#else // __APPLE__

inline std::vector<DriveInfo> QueryAllDrivesMac() {
    std::vector<DriveInfo> out;
    struct statfs* mounts = nullptr;
    int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < count; ++i) {
        std::string fstype = mounts[i].f_fstypename;
        if (fstype == "devfs" || fstype == "autofs") continue; // pseudo mounts, not real volumes

        DriveInfo info;
        info.label = mounts[i].f_mntonname;
        info.path = mounts[i].f_mntonname;
        std::error_code ec;
        auto space = std::filesystem::space(info.path, ec);
        if (!ec) {
            info.totalBytes = static_cast<unsigned long long>(space.capacity);
            info.freeBytes = static_cast<unsigned long long>(space.available);
        }
        out.push_back(std::move(info));
    }
    return out;
}

#endif

// ---- GPU enumeration ----

#if !defined(_WIN32) && !defined(__APPLE__)

// A handful of common PCI vendor IDs, just enough to turn a hex ID into
// a recognizable name. Not a full PCI ID database, so unknown vendors
// fall back to showing the raw hex ID rather than a name.
inline const char* PciVendorName(unsigned vendorId) {
    switch (vendorId) {
        case 0x10DE: return "NVIDIA";
        case 0x1002: case 0x1022: return "AMD";
        case 0x8086: return "Intel";
        case 0x13B5: return "ARM";
        case 0x1AE0: return "Google";
        default: return nullptr;
    }
}

inline std::vector<GpuInfo> QueryAllGpusLinux() {
    std::vector<GpuInfo> out;
    std::error_code ec;
    const char* pciDir = "/sys/bus/pci/devices";
    if (!std::filesystem::exists(pciDir, ec) || ec) return out;

    // Scanning the PCI bus directly (rather than /sys/class/drm) is
    // what makes an inactive dGPU visible: /sys/class/drm only lists
    // devices a kernel driver has actually probed and bound to, so a
    // laptop dGPU that's runtime-suspended or has no driver loaded yet
    // (common with hybrid/Optimus graphics) simply has no DRM node and
    // would be invisible to that approach. Every GPU that physically
    // exists shows up as a PCI device regardless of driver state.
    for (auto& entry : std::filesystem::directory_iterator(pciDir, ec)) {
        std::filesystem::path devicePath = entry.path();

        std::ifstream classFile(devicePath / "class");
        std::string classStr;
        if (classFile.good()) std::getline(classFile, classStr);
        // PCI class codes are 6 hex digits after "0x"; the base class
        // (first byte) for every kind of display controller: VGA
        // (0x030000), XGA (0x030100), 3D-only (0x030200), other
        // (0x038000) is 0x03, so this one prefix check catches all
        // of them without needing to enumerate every subclass.
        if (classStr.rfind("0x03", 0) != 0) continue;

        std::ifstream vendorFile(devicePath / "vendor");
        std::ifstream deviceFile(devicePath / "device");
        std::string vendorStr, deviceStr;
        if (vendorFile.good()) std::getline(vendorFile, vendorStr);
        if (deviceFile.good()) std::getline(deviceFile, deviceStr);
        unsigned vendorId = vendorStr.empty() ? 0 : static_cast<unsigned>(std::strtoul(vendorStr.c_str(), nullptr, 16));
        unsigned deviceId = deviceStr.empty() ? 0 : static_cast<unsigned>(std::strtoul(deviceStr.c_str(), nullptr, 16));

        // A "driver" symlink only exists if a kernel driver is
        // currently bound. Surfacing this tells the person why a
        // dGPU might not show up as the active/benchmarkable one, e.g.
        // an Optimus dGPU sitting powered-off until something needs it.
        bool driverBound = std::filesystem::exists(devicePath / "driver", ec) && !ec;

        const char* vendorName = PciVendorName(vendorId);
        char buf[128];
        if (vendorName)
            std::snprintf(buf, sizeof(buf), "%s GPU (device 0x%04x)%s", vendorName, deviceId,
                          driverBound ? "" : " (no driver bound)");
        else
            std::snprintf(buf, sizeof(buf), "PCI GPU (vendor 0x%04x, device 0x%04x)%s", vendorId, deviceId,
                          driverBound ? "" : " (no driver bound)");

        out.push_back(GpuInfo{buf});
    }
    return out;
}

#elif defined(_WIN32)

inline std::vector<GpuInfo> QueryAllGpusWindows() {
    std::vector<GpuInfo> out;
    DISPLAY_DEVICEA dd;
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesA(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue; // pseudo/mirror device, not a real adapter
        out.push_back(GpuInfo{dd.DeviceString});
    }
    return out;
}

#else // __APPLE__

inline std::vector<GpuInfo> QueryAllGpusMac() {
    // A real answer here needs IOKit (IOServiceMatching("IOPCIDevice")
    // et al.), which means linking -framework IOKit -framework
    // CoreFoundation, a build-system change this header intentionally
    // avoids requiring. BrazenApp falls back to showing just the active
    // GPU (from the OpenGL context) on this platform, with a note
    // explaining why the full list isn't available.
    return {};
}

#endif

// Actually confirms a directory is writable by the current process,
// rather than assuming any mounted/enumerated path is usable. This
// matters more than it might seem: on most single-partition desktop
// Linux installs, "/" is the only mount point for the primary disk, and
// a normal (non-root) user can't write there directly. Treating that
// mount root as the benchmark target silently failed every write, which
// is why the SSD test was reporting a flat 0 MB/s.
inline bool ProbeWritable(const std::filesystem::path& dir) {
    std::error_code ec;
    if (dir.empty() || !std::filesystem::exists(dir, ec) || ec) return false;
    std::filesystem::path probe = dir / ".brazen_write_probe.tmp";
    bool ok;
    {
        std::ofstream test(probe, std::ios::binary);
        ok = test.good();
    }
    std::filesystem::remove(probe, ec);
    return ok;
}

} // namespace detail

// Formats a byte count as a human-readable "X.X GB" string (decimal GB,
// matching how RAM/storage is conventionally marketed), or "Unknown" if
// the input is 0 (our convention for "couldn't determine").
inline std::string FormatBytesAsGB(unsigned long long bytes) {
    if (bytes == 0) return "Unknown";
    double gb = static_cast<double>(bytes) / (1000.0 * 1000.0 * 1000.0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f GB", gb);
    return buf;
}

// Enumerates storage volumes (see DriveInfo for what "a drive" means on
// each platform) and marks whichever one holds the app's current
// working directory as isPrimary, for a consistent highlight in the UI.
inline std::vector<DriveInfo> QueryAllDrives() {
    std::vector<DriveInfo> drives;
#if defined(_WIN32)
    drives = detail::QueryAllDrivesWindows();
#elif defined(__APPLE__)
    drives = detail::QueryAllDrivesMac();
#else
    drives = detail::QueryAllDrivesLinux();
#endif

    std::error_code ec;
    std::string cwd = std::filesystem::current_path(ec).string();
    int bestIndex = -1;
    size_t bestLen = 0;
    if (!ec) {
        for (size_t i = 0; i < drives.size(); ++i) {
            const auto& path = drives[i].path;
            if (path.empty() || cwd.rfind(path, 0) != 0) continue;
            if (path.size() > bestLen) {
                bestLen = path.size();
                bestIndex = static_cast<int>(i);
            }
        }
    }
    if (bestIndex >= 0) drives[static_cast<size_t>(bestIndex)].isPrimary = true;

    // Enumeration above only checks that a partition is *mounted*
    // somewhere. It says nothing about whether this process can
    // actually write there. Confirm it for real before ever offering a
    // drive as a benchmark target (see ProbeWritable's comment for why
    // this matters; this is the fix for the SSD test's 0 MB/s bug).
    for (auto& drive : drives) {
        if (drive.path.empty()) continue;

        std::string candidate = drive.path;
        if (drive.isPrimary) {
            // The app's own working directory is guaranteed to live on
            // this drive and is far more likely to be writable by a
            // normal user than the raw mount root (e.g. "/" on a
            // single-partition Linux install, which typically isn't).
            std::error_code cwdEc;
            auto cwd = std::filesystem::current_path(cwdEc);
            if (!cwdEc) candidate = cwd.string();
        }

        if (detail::ProbeWritable(candidate)) {
            drive.path = candidate;
        } else if (candidate != drive.path && detail::ProbeWritable(drive.path)) {
            // cwd wasn't writable for some reason, but the originally
            // enumerated mount point was. Keep that instead of giving
            // up entirely.
        } else {
            drive.path.clear(); // nowhere writable found; UI marks this drive not testable
        }
    }

    return drives;
}

// Enumerates GPUs at the OS level. See GpuInfo's comment: this is
// independent of (and doesn't know about) whichever GPU an active
// OpenGL context is actually bound to.
inline std::vector<GpuInfo> QueryAllGpus() {
#if defined(_WIN32)
    return detail::QueryAllGpusWindows();
#elif defined(__APPLE__)
    return detail::QueryAllGpusMac();
#else
    return detail::QueryAllGpusLinux();
#endif
}

inline HardwareInfo QueryHardwareInfo() {
    HardwareInfo info;
    info.cpuModel = detail::QueryCpuModelName();
    info.logicalCores = std::thread::hardware_concurrency();
    unsigned physical = detail::QueryPhysicalCoreCount();
    info.physicalCores = physical > 0 ? physical : info.logicalCores;
    info.totalRamBytes = detail::QueryTotalRamBytes();
    detail::QueryMotherboardInto(info.motherboardVendor, info.motherboardModel);
    detail::QuerySystemModelInto(info.systemVendor, info.systemModel);
    detail::QueryBiosInto(info.biosVendor, info.biosVersion);
    detail::QueryPrimaryDiskInto(info.primaryDiskModel, info.primaryDiskTotalBytes, info.primaryDiskFreeBytes);
    info.osVersion = detail::QueryOsVersion();
    info.kernelVersion = detail::QueryKernelVersion();
    info.cpuIdentifier = detail::QueryCpuIdentifier();
    info.cpuMaxFrequencyGHz = detail::QueryCpuMaxFrequencyGHz();
    info.cpuInstructionSets = detail::QueryInstructionSets();
    detail::QueryCpuCacheInto(info.cpuCache);
#if defined(_WIN32)
    info.osName = "Windows";
#elif defined(__APPLE__)
    info.osName = "macOS";
#elif defined(__linux__)
    info.osName = "Linux";
#else
    info.osName = "Unknown OS";
#endif
    return info;
}

} // namespace brazen
