#pragma once
#include "../IBenchmarkTest.h"
#include "../Log.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <malloc.h>
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
#endif

namespace brazen {

// Which half of a write-then-read-back cycle a DiskIoTest instance
// measures. Reported as two separate benchmark results ("Disk Write"
// and "Disk Read") rather than one blended number, since write and read
// throughput can differ meaningfully on the same device.
enum class DiskIoMode { Write, Read };

namespace disk_detail {

// A 4 KB-aligned heap buffer. Unbuffered/direct I/O requires the buffer
// address, transfer size and file offset to all be sector-aligned.
class AlignedBuffer {
public:
    AlignedBuffer() = default;
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    ~AlignedBuffer() { Reset(); }

    bool Allocate(size_t bytes, size_t alignment = 4096) {
        Reset();
#if defined(_WIN32)
        m_ptr = static_cast<unsigned char*>(_aligned_malloc(bytes, alignment));
#else
        void* p = nullptr;
        if (posix_memalign(&p, alignment, bytes) != 0) p = nullptr;
        m_ptr = static_cast<unsigned char*>(p);
#endif
        m_size = m_ptr ? bytes : 0;
        return m_ptr != nullptr;
    }

    void Reset() {
        if (!m_ptr) return;
#if defined(_WIN32)
        _aligned_free(m_ptr);
#else
        std::free(m_ptr);
#endif
        m_ptr = nullptr;
        m_size = 0;
    }

    unsigned char* data() { return m_ptr; }
    const unsigned char* data() const { return m_ptr; }
    size_t size() const { return m_size; }

private:
    unsigned char* m_ptr = nullptr;
    size_t m_size = 0;
};

inline std::string LastOsError() {
#if defined(_WIN32)
    DWORD code = GetLastError();
    char* msg = nullptr;
    DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, reinterpret_cast<LPSTR>(&msg), 0, nullptr);
    std::string out = (len && msg) ? std::string(msg, len) : std::string("error");
    if (msg) LocalFree(msg);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out + " (code " + std::to_string(static_cast<unsigned long>(code)) + ")";
#else
    int code = errno;
    return std::string(std::strerror(code)) + " (errno " + std::to_string(code) + ")";
#endif
}

// Thin cross-platform wrapper over positional file I/O that tries to
// bypass the OS file cache:
//   Windows: FILE_FLAG_NO_BUFFERING (+ WRITE_THROUGH for writes)
//   Linux:   O_DIRECT
//   macOS:   F_NOCACHE
// If the filesystem refuses (network shares, tmpfs on older kernels, some
// FUSE mounts...), it falls back to ordinary buffered I/O and IsDirect()
// reports false so the caller can tell the user their numbers may be
// flattered by caching. Plain OS calls rather than <cstdio> so the exact
// flags are under our control and errors carry a real reason.
class RawFile {
public:
    enum class Mode { CreateForWrite, OpenForRead };

    RawFile() = default;
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;
    ~RawFile() { Close(); }

    bool Open(const std::filesystem::path& path, Mode mode, bool preferDirect, std::string* error) {
        Close();
        m_direct = false;
#if defined(_WIN32)
        const DWORD access = mode == Mode::CreateForWrite ? GENERIC_WRITE : GENERIC_READ;
        const DWORD disposition = mode == Mode::CreateForWrite ? CREATE_NEW : OPEN_EXISTING;
        DWORD baseFlags = FILE_ATTRIBUTE_NORMAL;
        if (preferDirect) {
            DWORD directFlags = baseFlags | FILE_FLAG_NO_BUFFERING |
                                (mode == Mode::CreateForWrite ? FILE_FLAG_WRITE_THROUGH : 0);
            m_handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                                   directFlags, nullptr);
            if (m_handle != INVALID_HANDLE_VALUE) m_direct = true;
        }
        if (m_handle == INVALID_HANDLE_VALUE) {
            m_handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr, disposition,
                                   baseFlags, nullptr);
        }
        if (m_handle == INVALID_HANDLE_VALUE) {
            if (error) *error = LastOsError();
            return false;
        }
        return true;
#else
        int flags = (mode == Mode::CreateForWrite ? (O_WRONLY | O_CREAT | O_EXCL) : O_RDONLY);
    #ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
    #endif
        const std::string p = path.string();
        m_fd = -1;
    #if defined(__linux__)
        if (preferDirect) {
            m_fd = ::open(p.c_str(), flags | O_DIRECT, 0600);
            if (m_fd >= 0) {
                m_direct = true;
            } else if (mode == Mode::CreateForWrite) {
                // Some filesystems create the file and *then* reject
                // O_DIRECT; clear that up so the retry's O_EXCL can succeed.
                ::unlink(p.c_str());
            }
        }
    #endif
        if (m_fd < 0) m_fd = ::open(p.c_str(), flags, 0600);
        if (m_fd < 0) {
            if (error) *error = LastOsError();
            return false;
        }
    #if defined(__APPLE__)
        if (preferDirect && ::fcntl(m_fd, F_NOCACHE, 1) != -1) m_direct = true;
    #endif
        return true;
#endif
    }

    bool IsOpen() const {
#if defined(_WIN32)
        return m_handle != INVALID_HANDLE_VALUE;
#else
        return m_fd >= 0;
#endif
    }
    bool IsDirect() const { return m_direct; }

    // Writes exactly `bytes` at `offset` or fails.
    bool WriteAt(const void* data, size_t bytes, uint64_t offset, std::string* error) {
#if defined(_WIN32)
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD written = 0;
        if (!WriteFile(m_handle, data, static_cast<DWORD>(bytes), &written, &ov) || written != bytes) {
            if (error) *error = LastOsError();
            return false;
        }
        return true;
#else
        const unsigned char* p = static_cast<const unsigned char*>(data);
        size_t remaining = bytes;
        uint64_t off = offset;
        while (remaining > 0) {
            ssize_t n = ::pwrite(m_fd, p, remaining, static_cast<off_t>(off));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (error) *error = LastOsError();
                return false;
            }
            if (n == 0) {
                if (error) *error = "the drive stopped accepting data";
                return false;
            }
            p += n;
            off += static_cast<uint64_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        return true;
#endif
    }

    // Reads up to `bytes` at `offset`; *bytesRead < bytes means end of file.
    bool ReadAt(void* data, size_t bytes, uint64_t offset, size_t* bytesRead, std::string* error) {
        *bytesRead = 0;
#if defined(_WIN32)
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(m_handle, data, static_cast<DWORD>(bytes), &got, &ov)) {
            if (GetLastError() == ERROR_HANDLE_EOF) return true;
            if (error) *error = LastOsError();
            return false;
        }
        *bytesRead = got;
        return true;
#else
        unsigned char* p = static_cast<unsigned char*>(data);
        size_t total = 0;
        while (total < bytes) {
            ssize_t n = ::pread(m_fd, p + total, bytes - total, static_cast<off_t>(offset + total));
            if (n < 0) {
                if (errno == EINTR) continue;
                if (error) *error = LastOsError();
                return false;
            }
            if (n == 0) break; // EOF
            total += static_cast<size_t>(n);
        }
        *bytesRead = total;
        return true;
#endif
    }

    // Forces everything written so far onto the physical device.
    bool Flush(std::string* error) {
#if defined(_WIN32)
        if (!FlushFileBuffers(m_handle)) {
            if (error) *error = LastOsError();
            return false;
        }
        return true;
#elif defined(__APPLE__)
        // fsync() on macOS doesn't flush the drive's own write cache; F_FULLFSYNC does.
        if (::fcntl(m_fd, F_FULLFSYNC) == -1 && ::fsync(m_fd) != 0) {
            if (error) *error = LastOsError();
            return false;
        }
        return true;
#else
        if (::fsync(m_fd) != 0) {
            if (error) *error = LastOsError();
            return false;
        }
        return true;
#endif
    }

    // Best-effort, only used when direct I/O wasn't available: evict a
    // range from the OS page cache so a read measures the device rather
    // than RAM. (Only Linux has a usable hint for this.)
    void DropCache(uint64_t offset, uint64_t length) {
#if defined(__linux__)
        if (m_fd >= 0)
            ::posix_fadvise(m_fd, static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_DONTNEED);
#else
        (void)offset;
        (void)length;
#endif
    }

    void Close() {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
#endif
    }

private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif
    bool m_direct = false;
};

inline std::filesystem::path MakeUniqueTestPath(const std::filesystem::path& dir) {
    std::random_device rd;
    char nameBuf[64];
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        std::snprintf(nameBuf, sizeof(nameBuf), "brazen_disktest_%08x_%08x.tmp", rd(), rd());
        std::filesystem::path candidate = dir / nameBuf;
        std::error_code ec;
        if (!std::filesystem::exists(candidate, ec) && !ec) return candidate;
    }
    return {};
}

} // namespace disk_detail

// Sequential throughput test against a caller-chosen target directory.
// "Which drive to benchmark" just means "which directory the temporary
// test file is written into". BrazenApp::StartSsdBenchmark resolves the
// selected drive to a confirmed-writable path and constructs this test
// with it directly (it can't live in TestRegistry, which assumes every
// test is default-constructible).
//
// How it works -- deliberately simple and predictable:
//   * Disk Write: writes `totalBytes` of incompressible data to a new temp
//     file in 4 MB blocks, then flushes it to the physical device. The
//     flush is inside the timed section, so the score is durable write
//     speed rather than "how fast the OS accepted the data into RAM".
//   * Disk Read: first prepares a temp file of `totalBytes` (NOT timed;
//     progress is shown as "Preparing test file"), then reads it back
//     once, start to finish.
//   * It runs until that work is done (see RunsToCompletion()), so how
//     long it takes depends on the drive: fast NVMe drives finish in
//     seconds, a USB stick can take minutes. There is no duration to
//     pick because the amount of data, not the clock, sets the result.
//     The duration the runner is given is only a safety time limit.
//   * All I/O bypasses the OS file cache where the platform/filesystem
//     allows it (see disk_detail::RawFile), which is what previously made
//     Read (and sometimes Write) numbers unrealistically high, most of all
//     on Windows and macOS. If bypassing isn't possible the result says
//     so in its notes instead of silently reporting cache speed.
//   * Temp files are removed when the test object is destroyed (finished,
//     cancelled or failed). Any error, e.g. a full or read-only drive, marks the
//     result as failed with the reason instead of quietly scoring 0.
//
// Like the other tests it's a synthetic, relative benchmark, useful for
// comparing a drive with itself over time or with another machine
// running the same test, not a queue-depth-sweeping storage tool like
// fio or CrystalDiskMark.
class DiskIoTest : public IBenchmarkTest {
public:
    static constexpr size_t kIoBlockBytes = 4ull * 1024 * 1024;           // 4 MB per I/O request
    static constexpr uint64_t kDefaultTotalBytes = 1024ull * 1024 * 1024; // 1 GB per direction
    // Safety net only, not something users choose. If a device is so slow
    // or stalled that the work takes longer than this, the run stops and
    // reports what it managed.
    static constexpr double kSafetyLimitSeconds = 300.0;
    // Upper bound on the data one run will ever move per file. Bigger
    // requests are clamped (and would anyway be refused by the free-space
    // check), which also keeps the block-rounding arithmetic far from overflow.
    static constexpr uint64_t kMaxTotalBytes = 1024ull * 1024 * 1024 * 1024; // 1 TB
    // Always leave at least this much free on the drive being tested.
    static constexpr uint64_t kFreeSpaceReserveBytes = 512ull * 1024 * 1024;
    // Temp files older than this are assumed to be leftovers from a run that
    // crashed or lost power, and are removed before the next run. It must
    // comfortably exceed the longest a live file can sit untouched
    // (the safety limit above plus preparation time).
    static constexpr int kStaleFileMinutes = 15;

    explicit DiskIoTest(std::filesystem::path targetDir, DiskIoMode mode,
                        uint64_t totalBytes = kDefaultTotalBytes)
        : DiskIoTest(std::move(targetDir), mode, RoundToBlocks(totalBytes), std::make_shared<Shared>()) {}

    std::string GetName() const override {
        return m_mode == DiskIoMode::Write ? "Disk Write" : "Disk Read";
    }
    std::string GetDescription() const override {
        return m_mode == DiskIoMode::Write
            ? "Sequential write throughput to the selected drive"
            : "Sequential read throughput from the selected drive";
    }
    TestCategory GetCategory() const override { return TestCategory::Ssd; }
    std::string GetUnit() const override { return "MB/s"; }

    bool RunsToCompletion() const override { return true; }
    bool IsComplete() const override {
        return m_done || !m_ready || m_shared->failed.load(std::memory_order_relaxed);
    }
    void SetCancelFlag(const std::atomic<bool>* cancel) override { m_cancel = cancel; }

    void Setup() override {
        if (m_shared->failed.load()) return;

        if (!m_buffer.Allocate(kIoBlockBytes)) {
            Fail("Couldn't allocate the 4 MB I/O buffer.");
            return;
        }
        FillWithIncompressibleData();

        if (!m_shared->cleanedStale.exchange(true)) RemoveStaleTestFiles(m_targetDir);

        std::string err;
        if (m_mode == DiskIoMode::Write) {
            // Re-check free space now, not only when the run was queued:
            // other programs (or earlier jobs) may have used it since, and
            // filling a drive completely can destabilise the system. All
            // writers of this job account for their file together.
            uint64_t committed = m_shared->spaceReserved.fetch_add(m_totalBytes) + m_totalBytes;
            if (!HasRoomFor(committed)) return;
            m_path = disk_detail::MakeUniqueTestPath(m_targetDir);
            if (m_path.empty()) {
                Fail("Couldn't pick an unused temporary file name in " + m_targetDir.string());
                return;
            }
            if (!m_file.Open(m_path, disk_detail::RawFile::Mode::CreateForWrite, true, &err)) {
                m_path.clear();
                Fail("Couldn't create the test file in " + m_targetDir.string() + ": " + err);
                return;
            }
        } else {
            if (!EnsureReadFixture()) return; // failure or cancel already recorded
            if (!m_file.Open(m_shared->fixturePath, disk_detail::RawFile::Mode::OpenForRead, true, &err)) {
                Fail("Couldn't open the test file for reading: " + err);
                return;
            }
        }

        if (!m_file.IsDirect()) m_shared->anyBuffered.store(true);
        if (!m_shared->announced.exchange(true)) {
            LogInfo("Disk", "%s: %llu MB in %llu MB blocks on %s, %s.", GetName().c_str(),
                    static_cast<unsigned long long>(m_totalBytes / (1024 * 1024)),
                    static_cast<unsigned long long>(kIoBlockBytes / (1024 * 1024)), m_targetDir.string().c_str(),
                    m_file.IsDirect() ? "OS cache bypassed (direct I/O)"
                                      : "OS cache could NOT be bypassed on this drive");
        }

        m_shared->bytesExpected.fetch_add(m_totalBytes);
        m_ready = true;
        m_shared->measuring.store(true);
    }

    uint64_t RunWorkChunk() override {
        if (!m_ready || m_done || m_shared->failed.load(std::memory_order_relaxed)) return 0;
        std::string err;

        if (m_mode == DiskIoMode::Write) {
            if (!m_file.WriteAt(m_buffer.data(), kIoBlockBytes, m_offset, &err)) {
                Fail("Write failed after " + std::to_string(m_offset / (1024 * 1024)) + " MB: " + err);
                m_done = true;
                return 0;
            }
            m_offset += kIoBlockBytes;
            m_shared->bytesDone.fetch_add(kIoBlockBytes);
            if (m_offset >= m_totalBytes) {
                // Flush is part of the timed loop on purpose: the score
                // includes getting the data onto the device, not just into
                // the OS's write cache.
                if (!m_file.Flush(&err)) {
                    Fail("Flushing the test file to the drive failed: " + err);
                    m_done = true;
                    return 0;
                }
                m_file.Close();
                m_done = true;
            }
            return kIoBlockBytes;
        }

        // Buffered fallback only: make sure this block isn't already cached.
        if (!m_file.IsDirect()) m_file.DropCache(m_offset, kIoBlockBytes);
        size_t got = 0;
        if (!m_file.ReadAt(m_buffer.data(), kIoBlockBytes, m_offset, &got, &err)) {
            Fail("Read failed after " + std::to_string(m_offset / (1024 * 1024)) + " MB: " + err);
            m_done = true;
            return 0;
        }
        if (got != kIoBlockBytes) {
            Fail("The test file ended early (read " + std::to_string((m_offset + got) / (1024 * 1024)) +
                 " MB of " + std::to_string(m_totalBytes / (1024 * 1024)) + " MB).");
            m_done = true;
            return 0;
        }
        m_offset += got;
        m_shared->bytesDone.fetch_add(got);
        if (m_offset >= m_totalBytes) {
            m_file.Close();
            m_done = true;
        }
        return got;
    }

    double ComputeScore(uint64_t totalOps, double elapsedSeconds) const override {
        if (elapsedSeconds <= 0.0) return 0.0;
        double bytesPerSec = static_cast<double>(totalOps) / elapsedSeconds;
        return bytesPerSec / (1000.0 * 1000.0); // MB/s (decimal), matching how storage speed is usually marketed
    }

    TestProgress GetProgress() const override {
        TestProgress p;
        if (m_shared->failed.load()) {
            p.phase = "Failed";
            return p;
        }
        if (!m_shared->measuring.load()) {
            if (m_mode == DiskIoMode::Read) {
                p.phase = "Preparing test file (not timed)";
                uint64_t total = m_shared->prepTotal.load();
                p.fraction = total ? Clamp01(static_cast<double>(m_shared->prepDone.load()) / total) : 0.0;
            } else {
                p.phase = "Creating test file";
                p.fraction = 0.0;
            }
            return p;
        }
        p.phase = m_mode == DiskIoMode::Write ? "Writing" : "Reading";
        uint64_t expected = m_shared->bytesExpected.load();
        p.fraction = expected ? Clamp01(static_cast<double>(m_shared->bytesDone.load()) / expected) : 0.0;
        return p;
    }

    bool HasFailed() const override { return m_shared->failed.load(); }

    std::string StatusNote() const override {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        if (m_shared->failed.load()) return m_shared->error;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%llu MB file, %llu MB blocks, ",
                      static_cast<unsigned long long>(m_totalBytes / (1024 * 1024)),
                      static_cast<unsigned long long>(kIoBlockBytes / (1024 * 1024)));
        std::string note = buf;
        note += m_shared->anyBuffered.load()
            ? "buffered I/O: this drive/filesystem doesn't allow bypassing the OS cache, so speeds "
              "(Read especially) may be higher than the drive can really deliver"
            : "direct I/O (OS cache bypassed)";
        return note;
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        // Each clone gets its own file handle and buffer, but they all
        // share one Shared block: progress counters, failure state, and
        // (for Read) the single prepared test file.
        return std::unique_ptr<IBenchmarkTest>(new DiskIoTest(m_targetDir, m_mode, m_totalBytes, m_shared));
    }

    // Confirms the target directory both exists and is actually
    // writable (it may have been unmounted, or be read-only).
    bool IsAvailable() const override {
        std::error_code ec;
        if (m_targetDir.empty() || !std::filesystem::is_directory(m_targetDir, ec) || ec) return false;
        std::random_device rd;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            char nameBuf[64];
            std::snprintf(nameBuf, sizeof(nameBuf), ".brazen_write_probe_%08x_%08x.tmp", rd(), rd());
            std::filesystem::path probe = m_targetDir / nameBuf;
            disk_detail::RawFile f;
            if (!f.Open(probe, disk_detail::RawFile::Mode::CreateForWrite, false, nullptr)) continue;
            f.Close();
            std::filesystem::remove(probe, ec);
            return !ec;
        }
        return false;
    }

    ~DiskIoTest() override {
        m_file.Close(); // must be closed before it can be deleted on Windows
        if (m_mode == DiskIoMode::Write && !m_path.empty()) {
            std::error_code ec;
            std::filesystem::remove(m_path, ec);
        }
    }

private:
    // State shared by the prototype and every clone of one queued job.
    struct Shared {
        ~Shared() {
            if (!fixturePath.empty()) {
                std::error_code ec;
                std::filesystem::remove(fixturePath, ec);
            }
        }

        std::atomic<uint64_t> bytesDone{0};     // bytes moved in the timed phase, all threads
        std::atomic<uint64_t> bytesExpected{0}; // what all threads together will move
        std::atomic<uint64_t> prepDone{0};      // read-fixture preparation progress (untimed)
        std::atomic<uint64_t> prepTotal{0};
        std::atomic<bool> measuring{false};     // false while still setting up / preparing
        std::atomic<bool> failed{false};
        std::atomic<bool> anyBuffered{false};   // some handle couldn't bypass the OS cache
        std::atomic<bool> announced{false};     // one-time "here's how this run works" log line
        std::atomic<bool> cleanedStale{false};  // leftover-file sweep done for this job
        std::atomic<uint64_t> spaceReserved{0}; // bytes of temp files all writers will create

        std::mutex mutex; // guards `error` and the read-fixture fields below
        std::string error;
        std::filesystem::path fixturePath;
        bool fixtureReady = false;
        bool fixtureAttempted = false;
    };

    DiskIoTest(std::filesystem::path targetDir, DiskIoMode mode, uint64_t totalBytes,
               std::shared_ptr<Shared> shared)
        : m_targetDir(std::move(targetDir)), m_mode(mode), m_totalBytes(totalBytes),
          m_shared(std::move(shared)) {}

    static uint64_t RoundToBlocks(uint64_t bytes) {
        if (bytes < kIoBlockBytes) bytes = kIoBlockBytes;
        if (bytes > kMaxTotalBytes) bytes = kMaxTotalBytes;
        return ((bytes + kIoBlockBytes - 1) / kIoBlockBytes) * kIoBlockBytes;
    }

    static double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

    bool Cancelled() const { return m_cancel && m_cancel->load(std::memory_order_relaxed); }

    void Fail(const std::string& message) {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        if (!m_shared->failed.load()) {
            m_shared->error = message;
            m_shared->failed.store(true);
        }
    }

    // Fills the I/O buffer with xorshift noise so drives/filesystems that
    // compress or special-case zero pages can't make results look better
    // than the hardware really is.
    void FillWithIncompressibleData() {
        uint64_t* words = reinterpret_cast<uint64_t*>(m_buffer.data());
        size_t count = m_buffer.size() / sizeof(uint64_t);
        uint64_t x = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < count; ++i) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            words[i] = x;
        }
    }

    // Read mode: one test file shared by all worker threads, written once
    // (untimed) before any reading starts. Returns false if preparing it
    // failed or was cancelled; failures are recorded via Fail(), a cancel
    // is not a failure (the runner reports the run as cancelled).
    bool EnsureReadFixture() {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        if (m_shared->fixtureReady) return true;
        if (m_shared->fixtureAttempted || m_shared->failed.load()) return false;
        m_shared->fixtureAttempted = true;

        if (!HasRoomForLocked(m_totalBytes)) return false;
        std::filesystem::path path = disk_detail::MakeUniqueTestPath(m_targetDir);
        if (path.empty()) {
            FailLocked("Couldn't pick an unused temporary file name in " + m_targetDir.string());
            return false;
        }
        m_shared->fixturePath = path; // from here on ~Shared() cleans it up, even on failure

        disk_detail::RawFile f;
        std::string err;
        if (!f.Open(path, disk_detail::RawFile::Mode::CreateForWrite, true, &err)) {
            FailLocked("Couldn't create the test file in " + m_targetDir.string() + ": " + err);
            return false;
        }
        LogInfo("Disk", "Preparing a %llu MB test file on %s (not timed)...",
                static_cast<unsigned long long>(m_totalBytes / (1024 * 1024)), m_targetDir.string().c_str());
        m_shared->prepTotal.store(m_totalBytes);
        for (uint64_t off = 0; off < m_totalBytes; off += kIoBlockBytes) {
            if (Cancelled()) return false;
            if (!f.WriteAt(m_buffer.data(), kIoBlockBytes, off, &err)) {
                FailLocked("Preparing the test file failed after " + std::to_string(off / (1024 * 1024)) +
                           " MB: " + err);
                return false;
            }
            m_shared->prepDone.fetch_add(kIoBlockBytes);
        }
        if (!f.Flush(&err)) {
            FailLocked("Flushing the test file to the drive failed: " + err);
            return false;
        }
        // Buffered fallback only: the pages we just wrote are still in the
        // OS cache, so evict the whole file before the timed read begins.
        if (!f.IsDirect()) f.DropCache(0, 0); // length 0 = to end of file
        f.Close();
        m_shared->fixtureReady = true;
        LogInfo("Disk", "Test file ready; starting the timed read.");
        return true;
    }

    // True if the target drive can hold `bytesNeeded` of temp files plus
    // the safety reserve; otherwise records a failure and returns false. If
    // the OS can't report free space we don't block the run, since a real
    // out-of-space condition is still caught (and reported) while writing.
    bool HasRoomFor(uint64_t bytesNeeded) {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        return HasRoomForLocked(bytesNeeded);
    }

    bool HasRoomForLocked(uint64_t bytesNeeded) {
        std::error_code ec;
        std::filesystem::space_info info = std::filesystem::space(m_targetDir, ec);
        if (ec) return true;
        const uint64_t available = static_cast<uint64_t>(info.available);
        if (bytesNeeded > available || available - bytesNeeded < kFreeSpaceReserveBytes) {
            FailLocked("Not enough free space on " + m_targetDir.string() + ": the test needs " +
                       std::to_string(bytesNeeded / (1024 * 1024)) + " MB plus a " +
                       std::to_string(kFreeSpaceReserveBytes / (1024 * 1024)) + " MB safety reserve, but only " +
                       std::to_string(available / (1024 * 1024)) + " MB is free.");
            return false;
        }
        return true;
    }

    // Deletes test files left behind by a run that crashed, was killed or
    // lost power. Only files that follow our naming pattern *and* haven't
    // been touched for kStaleFileMinutes are removed, so a second Brazen
    // instance working in the same folder is never disturbed.
    static void RemoveStaleTestFiles(const std::filesystem::path& dir) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        const auto cutoff = fs::file_time_type::clock::now() - std::chrono::minutes(kStaleFileMinutes);
        int removed = 0;
        for (fs::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) break;
            std::error_code fileEc;
            const fs::path& p = it->path();
            const std::string name = p.filename().string();
            const bool ours = (name.rfind("brazen_disktest_", 0) == 0 && name.size() > 4 &&
                               name.compare(name.size() - 4, 4, ".tmp") == 0) ||
                              (name.rfind(".brazen_write_probe_", 0) == 0);
            if (!ours || !it->is_regular_file(fileEc) || fileEc) continue;
            auto written = fs::last_write_time(p, fileEc);
            if (fileEc || written > cutoff) continue;
            if (fs::remove(p, fileEc) && !fileEc) ++removed;
        }
        if (removed > 0)
            LogInfo("Disk", "Removed %d leftover test file%s from an earlier run in %s.", removed,
                    removed == 1 ? "" : "s", dir.string().c_str());
    }

    // Same as Fail(), for callers that already hold m_shared->mutex.
    void FailLocked(const std::string& message) {
        if (!m_shared->failed.load()) {
            m_shared->error = message;
            m_shared->failed.store(true);
        }
    }

    std::filesystem::path m_targetDir;
    DiskIoMode m_mode;
    uint64_t m_totalBytes;
    std::shared_ptr<Shared> m_shared;

    // Per-instance (per worker thread) state.
    const std::atomic<bool>* m_cancel = nullptr;
    disk_detail::RawFile m_file;
    disk_detail::AlignedBuffer m_buffer;
    std::filesystem::path m_path; // Write mode: this instance's own temp file
    uint64_t m_offset = 0;
    bool m_ready = false;
    bool m_done = false;
};

} // namespace brazen
