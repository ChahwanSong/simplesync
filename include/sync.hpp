#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mfs {

struct SyncOptions {
    bool remove_extraneous{true};
};

struct FileMetadata {
    std::filesystem::path file;
    int depth{0};
    bool detail{false};
    std::uint64_t mode{0};
    std::uint64_t uid{0};
    std::uint64_t gid{0};
    std::uint64_t atime{0};
    std::uint64_t atime_nsec{0};
    std::uint64_t mtime{0};
    std::uint64_t mtime_nsec{0};
    std::uint64_t ctime{0};
    std::uint64_t ctime_nsec{0};
    std::uint64_t size{0};
};

struct SyncStats {
    std::size_t entries_scanned{0};
    std::size_t files_copied{0};
    std::size_t files_skipped{0};
    std::size_t files_deleted{0};
    std::size_t directories_created{0};
    std::uintmax_t bytes_copied{0};
    std::chrono::duration<double> scan_elapsed{};
    std::chrono::duration<double> copy_elapsed{};
    std::chrono::duration<double> prune_elapsed{};
    std::chrono::duration<double> total_elapsed{};
    std::vector<FileMetadata> synced_entries{};
};

class DirectorySyncer {
public:
    explicit DirectorySyncer(SyncOptions options = {});

    SyncStats synchronize(const std::filesystem::path& source,
                          const std::filesystem::path& destination);

private:
    SyncOptions options_;

    void validate_inputs(const std::filesystem::path& source,
                         const std::filesystem::path& destination);
    void ensure_destination_root(const std::filesystem::path& destination);
    void copy_from_source(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         SyncStats& stats);
    void prune_destination(const std::filesystem::path& source,
                           const std::filesystem::path& destination,
                           SyncStats& stats);

    bool collect_metadata(const std::filesystem::path& path, int depth, FileMetadata& out);
    void log_lstat_error(const std::filesystem::path& path, int err);
};

void print_report(const SyncStats& stats);
void print_synced_metadata(const std::vector<FileMetadata>& entries);

} // namespace mfs
