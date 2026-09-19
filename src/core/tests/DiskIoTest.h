#pragma once
#include "../IBenchmarkTest.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <vector>

#if defined(_WIN32)
    #include <io.h>
    #include <fcntl.h>
    #include <sys/stat.h>
#elif defined(__linux__)
    #include <fcntl.h>
    #include <unistd.h>
#elif defined(__APPLE__)
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace brazen {

// Which half of a write-then-read-back cycle a DiskIoTest instance
// measures. Reported as two separate benchmark results ("Disk Write"
// and "Disk Read") rather than one blended number, since write and read
// throughput can differ meaningfully on the same device.
enum class DiskIoMode { Write, Read };

// Sequential throughput test against a caller-chosen target directory.
// "Which drive to benchmark" (see the SSD tab's drive picker) ultimately
// just means "which directory this test writes its temp file into".
// BrazenApp::StartSsdBenchmark resolves the selected DriveInfo to a
// confirmed-writable path and constructs this test with it directly.
//
// Like RamBandwidthTest's buffer size, the target directory is a
// per-instance construction parameter chosen at UI time rather than a
// fixed default, so this test is NOT registered in TestRegistry (which
// assumes every test can be default-constructed). It's built and
// Enqueue()'d directly, the same way BrazenApp already handles a
// custom-sized RamBandwidthTest.
//
// This is a synthetic, relative benchmark like the others in Brazen,
// useful for comparing this drive against itself over time, or against
// another drive/machine running the same test, not a substitute for a
// proper queue-depth-sweeping storage benchmark (CrystalDiskMark, fio,
// etc). Two OS-caching pitfalls that naive disk benchmarks commonly fall
// into, and how this one avoids them:
//   - Writes can look artificially fast if the OS just absorbs them into
//     the page cache and reports success before the data has actually
//     reached the physical device, especially at low thread counts,
//     where there's little write pressure to force a real flush. Every
//     write chunk here is followed by an explicit flush-to-device call
//     (fsync/F_FULLFSYNC/_commit) specifically so the timed duration
//     includes the time to actually persist the data, not just hand it
//     to the OS.
//   - Reads can look artificially fast if they hit data still sitting in
//     the page cache from a write this same test just did. On Linux this
//     test asks the kernel to drop the file's cached pages
//     (posix_fadvise) before each timed read to reduce that effect;
//     Windows/macOS have no equivalent wired up here, so Read numbers
//     there should be taken with a bit more skepticism, especially at
//     small chunk sizes on a machine with lots of free RAM.
class DiskIoTest : public IBenchmarkTest {
public:
    explicit DiskIoTest(std::filesystem::path targetDir, DiskIoMode mode, size_t chunkBytes = kDefaultChunkBytes)
                : m_targetDir(std::move(targetDir)), m_mode(mode), m_chunkBytes(chunkBytes),
                    m_readFixture(mode == DiskIoMode::Read ? std::make_shared<ReadFixture>() : nullptr) {}

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

    void Setup() override {
        m_buffer.assign(m_chunkBytes, 0);

        // Non-trivial fill pattern (not all-zero) so the OS/filesystem
        // can't quietly special-case an all-zero page as a shortcut.
        uint32_t seed = 2463534242u;
        for (auto& b : m_buffer) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            b = static_cast<unsigned char>(seed & 0xFF);
        }

        if (m_mode == DiskIoMode::Read) {
            // Prepare one flushed fixture for all read workers. Preparing a
            // separate multi-chunk file per worker made multi-core reads
            // spend most of their wall time writing duplicate setup data.
            std::lock_guard<std::mutex> lock(m_readFixture->mutex);
            if (!m_readFixture->ready) {
                std::random_device rd;
                char nameBuf[64];
                for (unsigned attempt = 0; attempt < 8; ++attempt) {
                    std::snprintf(nameBuf, sizeof(nameBuf), "brazen_disktest_%08x_%08x.tmp", rd(), rd());
                    m_readFixture->path = m_targetDir / nameBuf;
                    std::error_code ec;
                    if (!std::filesystem::exists(m_readFixture->path, ec) && !ec) break;
                    m_readFixture->path.clear();
                }
                if (m_readFixture->path.empty()) return;

                std::FILE* f = std::fopen(m_readFixture->path.string().c_str(), "wb");
                if (!f) return;
                size_t written = 0;
                bool complete = true;
                for (size_t chunk = 0; chunk < kFileChunkCount; ++chunk) {
                    size_t current = std::fwrite(m_buffer.data(), 1, m_buffer.size(), f);
                    written += current;
                    if (current != m_buffer.size()) {
                        complete = false;
                        break;
                    }
                }
                bool flushed = complete && std::fflush(f) == 0 && FlushToDevice(f);
                bool closed = std::fclose(f) == 0;
                if (!flushed || !closed || written != m_buffer.size() * kFileChunkCount) {
                    std::error_code ec;
                    std::filesystem::remove(m_readFixture->path, ec);
                    m_readFixture->path.clear();
                    return;
                }
                m_readFixture->ready = true;
            }
            m_filePath = m_readFixture->path;
        } else {
            std::random_device rd;
            char nameBuf[64];
            for (unsigned attempt = 0; attempt < 8; ++attempt) {
                std::snprintf(nameBuf, sizeof(nameBuf), "brazen_disktest_%08x_%08x.tmp", rd(), rd());
                m_filePath = m_targetDir / nameBuf;
                std::error_code ec;
                if (!std::filesystem::exists(m_filePath, ec) && !ec) break;
                m_filePath.clear();
            }
            if (m_filePath.empty()) return;
        }
        m_file = std::fopen(m_filePath.string().c_str(), m_mode == DiskIoMode::Write ? "wb+" : "rb");
        if (!m_file) {
            std::error_code ec;
            std::filesystem::remove(m_filePath, ec);
            m_filePath.clear();
            return;
        }
        m_fileSize = m_mode == DiskIoMode::Write
            ? m_chunkBytes * kFileChunkCount
            : m_chunkBytes * kFileChunkCount;
        m_setupSucceeded = true;
    }

    uint64_t RunWorkChunk() override {
        if (!m_setupSucceeded) return 0;

        if (m_mode == DiskIoMode::Write) {
            if (std::fseek(m_file, static_cast<long>(m_fileOffset), SEEK_SET) != 0) return 0;
            size_t written = std::fwrite(m_buffer.data(), 1, m_buffer.size(), m_file);
            bool flushed = written == m_buffer.size() && std::fflush(m_file) == 0 && FlushToDevice(m_file);
            m_fileOffset += written;
            if (m_fileOffset >= m_fileSize) m_fileOffset = 0;
            return flushed ? static_cast<uint64_t>(written) : 0;
        }

        DropFromPageCache();
        if (std::fseek(m_file, static_cast<long>(m_fileOffset), SEEK_SET) != 0) return 0;
        size_t read = std::fread(m_buffer.data(), 1, m_buffer.size(), m_file);
        m_fileOffset += read;
        if (m_fileOffset >= m_fileSize) m_fileOffset = 0;
        return static_cast<uint64_t>(read);
    }

    double ComputeScore(uint64_t totalOps, double elapsedSeconds) const override {
        if (elapsedSeconds <= 0.0) return 0.0;
        double bytesPerSec = static_cast<double>(totalOps) / elapsedSeconds;
        return bytesPerSec / (1000.0 * 1000.0); // MB/s (decimal), matching how storage speed is usually marketed
    }

    std::unique_ptr<IBenchmarkTest> Clone() const override {
        return std::make_unique<DiskIoTest>(*this);
    }

    // Confirms the target directory both exists and is actually
    // writable (rather than just assuming a selected drive is usable,
    // it may have been unmounted, or be read-only) before offering the
    // Run button.
    bool IsAvailable() const override {
        std::error_code ec;
        if (m_targetDir.empty() || !std::filesystem::is_directory(m_targetDir, ec) || ec) return false;
        std::random_device rd;
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            char nameBuf[64];
            std::snprintf(nameBuf, sizeof(nameBuf), ".brazen_write_probe_%08x_%08x.tmp", rd(), rd());
            std::filesystem::path probe = m_targetDir / nameBuf;
            int fd = OpenExclusive(probe);
            if (fd < 0) continue;
            CloseProbe(fd);
            std::filesystem::remove(probe, ec);
            return !ec;
        }
        return false;
    }

    ~DiskIoTest() override {
        if (m_file) std::fclose(m_file);
        if (m_readFixture) return;
        if (m_filePath.empty()) return;
        std::error_code ec;
        std::filesystem::remove(m_filePath, ec);
    }

    static constexpr size_t kDefaultChunkBytes = 16ull * 1024 * 1024; // 16 MB per chunk
    // Keep the per-worker fixture bounded. Multi-core runs are intentionally
    // capped by the UI/queue to avoid multiplying this into many gigabytes.
    static constexpr size_t kFileChunkCount = 2;

private:
    struct ReadFixture {
        ~ReadFixture() {
            if (!path.empty()) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }

        std::mutex mutex;
        std::filesystem::path path;
        bool ready = false;
    };

    // Forces data written via `f` out of the OS page cache and onto the
    // physical device, so a write chunk's timing includes real
    // persistence rather than just handing bytes to the OS. Without
    // this, a write test at low thread counts can measure page-cache
    // speed instead of drive speed, which is exactly what was
    // happening before this existed: single-threaded Write was scoring
    // *faster* than Read, which is backwards for real storage hardware
    // and was the tell that writes weren't actually reaching the disk.
    static bool FlushToDevice(std::FILE* f) {
#if defined(_WIN32)
        return _commit(_fileno(f)) == 0;
#elif defined(__APPLE__)
        return fcntl(fileno(f), F_FULLFSYNC) == 0; // fsync() on macOS doesn't guarantee the drive's own write cache is flushed; F_FULLFSYNC does
#else
        return fsync(fileno(f)) == 0;
#endif
    }

    static int OpenExclusive(const std::filesystem::path& path) {
#if defined(_WIN32)
        return _open(path.string().c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        return ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
#endif
    }

    static void CloseProbe(int fd) {
#if defined(_WIN32)
        _close(fd);
#else
        ::close(fd);
#endif
    }

    // Best-effort: asks the kernel to evict this file's pages from the
    // OS page cache before a timed read, so RunWorkChunk() measures
    // actual device reads rather than a cache hit from the write this
    // same test just did. Only implemented on Linux (posix_fadvise);
    // silently a no-op elsewhere. Uses a plain POSIX fd opened
    // separately from the iostream read that follows, rather than
    // trying to get a native handle out of std::ifstream.
    void DropFromPageCache() const {
#if defined(__linux__)
        int fd = ::open(m_filePath.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            ::close(fd);
        }
#endif
    }

    std::filesystem::path m_targetDir;
    std::filesystem::path m_filePath;
    DiskIoMode m_mode;
    size_t m_chunkBytes;
    std::FILE* m_file = nullptr;
    size_t m_fileOffset = 0;
    size_t m_fileSize = 0;
    std::shared_ptr<ReadFixture> m_readFixture;
    bool m_setupSucceeded = false;
    std::vector<unsigned char> m_buffer;
};

} // namespace brazen
