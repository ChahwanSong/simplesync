#include "sync.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace mfs {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

DirectorySyncer::DirectorySyncer(SyncOptions options) : options_(options) {}

SyncStats DirectorySyncer::synchronize(const fs::path& source, const fs::path& destination) {
    SyncStats stats{};
    const auto total_start = Clock::now();

    const int total_steps = options_.remove_extraneous ? 4 : 3;
    std::cout << "[1/" << total_steps << "] Validating input directories..." << std::endl;
    validate_inputs(source, destination);

    std::cout << "[2/" << total_steps << "] Preparing destination directory tree..." << std::endl;
    ensure_destination_root(destination);
    if (fs::equivalent(source, destination)) {
        throw std::runtime_error("Source and destination resolve to the same location.");
    }

    std::cout << "[3/" << total_steps << "] Copying new and updated entries from source..." << std::endl;
    copy_from_source(source, destination, stats);

    if (options_.remove_extraneous) {
        std::cout << "[4/" << total_steps << "] Pruning entries that no longer exist in source..." << std::endl;
        prune_destination(source, destination, stats);
    } else {
        std::cout << "[3/" << total_steps << "] Skipping prune stage (extraneous files retained)." << std::endl;
    }

    stats.total_elapsed = Clock::now() - total_start;
    return stats;
}

void DirectorySyncer::validate_inputs(const fs::path& source, const fs::path& destination) {
    if (!fs::exists(source)) {
        throw std::runtime_error("Source directory does not exist: " + source.string());
    }
    if (!fs::is_directory(source)) {
        throw std::runtime_error("Source path is not a directory: " + source.string());
    }

    if (fs::exists(destination) && !fs::is_directory(destination)) {
        throw std::runtime_error("Destination exists but is not a directory: " + destination.string());
    }
}

void DirectorySyncer::ensure_destination_root(const fs::path& destination) {
    if (!fs::exists(destination)) {
        fs::create_directories(destination);
        std::cout << "    Created destination root: " << destination << std::endl;
    }
}

void DirectorySyncer::copy_from_source(const fs::path& source,
                                       const fs::path& destination,
                                       SyncStats& stats) {
    const auto stage_start = Clock::now();
    fs::directory_options options = fs::directory_options::skip_permission_denied;

    for (fs::recursive_directory_iterator it(source, options), end; it != end; ++it) {
        const fs::directory_entry& entry = *it;
        ++stats.entries_scanned;

        FileMetadata src_meta;
        if (!collect_metadata(entry.path(), static_cast<int>(it.depth()), src_meta)) {
            if (entry.is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        const bool is_symlink = S_ISLNK(static_cast<mode_t>(src_meta.mode));
        if (is_symlink) {
            std::cout << "    Skipping symlink: " << entry.path() << std::endl;
            it.disable_recursion_pending();
            ++stats.files_skipped;
            continue;
        }

        const bool is_directory = S_ISDIR(static_cast<mode_t>(src_meta.mode));

        fs::path relative_path;
        try {
            relative_path = fs::relative(entry.path(), source);
        } catch (const fs::filesystem_error& ex) {
            std::cerr << "    Warning: failed to compute relative path for " << entry.path()
                      << ": " << ex.what() << std::endl;
            if (entry.is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        const fs::path dest_path = destination / relative_path;

        if (is_directory) {
            if (!fs::exists(dest_path)) {
                try {
                    fs::create_directories(dest_path);
                    ++stats.directories_created;
                    std::cout << "    Created directory: " << dest_path << std::endl;
                    stats.synced_entries.push_back(src_meta);
                } catch (const fs::filesystem_error& ex) {
                    std::cerr << "    Warning: failed to create directory " << dest_path << ": " << ex.what()
                              << std::endl;
                    it.disable_recursion_pending();
                }
            }
            continue;
        }

        const bool is_regular = S_ISREG(static_cast<mode_t>(src_meta.mode));
        if (!is_regular) {
            std::cout << "    Skipping non-regular entry: " << entry.path() << std::endl;
            ++stats.files_skipped;
            continue;
        }

        bool should_copy = false;
        const std::uintmax_t source_size = src_meta.size;

        if (!fs::exists(dest_path)) {
            should_copy = true;
        } else if (!fs::is_regular_file(dest_path)) {
            std::cout << "    Destination entry is not a regular file (will replace): " << dest_path << std::endl;
            try {
                fs::remove_all(dest_path);
                should_copy = true;
            } catch (const fs::filesystem_error& ex) {
                std::cerr << "    Warning: failed to remove non-regular destination entry " << dest_path
                          << ": " << ex.what() << std::endl;
                continue;
            }
        } else {
            FileMetadata dest_meta;
            if (!collect_metadata(dest_path, static_cast<int>(it.depth()), dest_meta)) {
                should_copy = true;
            } else {
                const bool dest_is_symlink = S_ISLNK(static_cast<mode_t>(dest_meta.mode));
                if (dest_is_symlink) {
                    std::cout << "    Destination entry is a symlink (will replace): " << dest_path << std::endl;
                    try {
                        fs::remove(dest_path);
                        should_copy = true;
                    } catch (const fs::filesystem_error& ex) {
                        std::cerr << "    Warning: failed to remove symlink " << dest_path << ": " << ex.what()
                                  << std::endl;
                        continue;
                    }
                } else {
                    const std::uintmax_t dest_size = dest_meta.size;
                    const bool size_differs = source_size != dest_size;
                    const bool time_newer = (src_meta.mtime > dest_meta.mtime) ||
                                            (src_meta.mtime == dest_meta.mtime &&
                                             src_meta.mtime_nsec > dest_meta.mtime_nsec);
                    if (size_differs || time_newer) {
                        should_copy = true;
                    }
                }
            }
        }

        if (should_copy) {
            try {
                fs::create_directories(dest_path.parent_path());
            } catch (const fs::filesystem_error& ex) {
                std::cerr << "    Warning: failed to ensure parent directory for " << dest_path << ": " << ex.what()
                          << std::endl;
                continue;
            }

            const auto copy_start = Clock::now();
            try {
                fs::copy_file(entry.path(), dest_path, fs::copy_options::overwrite_existing);
                const auto copy_end = Clock::now();
                stats.copy_elapsed += copy_end - copy_start;
                ++stats.files_copied;
                stats.bytes_copied += source_size;
                std::cout << "    Copied file: " << entry.path() << " -> " << dest_path << " (" << source_size
                          << " bytes)" << std::endl;
                stats.synced_entries.push_back(src_meta);
            } catch (const fs::filesystem_error& ex) {
                std::cerr << "    Warning: failed to copy " << entry.path() << " to " << dest_path
                          << ": " << ex.what() << std::endl;
                continue;
            }
        } else {
            ++stats.files_skipped;
        }
    }

    stats.scan_elapsed = Clock::now() - stage_start;
}

void DirectorySyncer::prune_destination(const fs::path& source,
                                        const fs::path& destination,
                                        SyncStats& stats) {
    const auto prune_start = Clock::now();

    fs::directory_options options = fs::directory_options::skip_permission_denied;

    struct RemovalCandidate {
        fs::path path;
        bool is_directory;
        std::size_t depth;
    };

    std::vector<RemovalCandidate> candidates;

    for (fs::recursive_directory_iterator it(destination, options), end; it != end; ++it) {
        const fs::directory_entry& entry = *it;
        FileMetadata dest_meta;
        if (!collect_metadata(entry.path(), static_cast<int>(it.depth()), dest_meta)) {
            if (entry.is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        const bool is_symlink = S_ISLNK(static_cast<mode_t>(dest_meta.mode));
        if (is_symlink) {
            std::cout << "    Skipping symlink in destination: " << entry.path() << std::endl;
            continue;
        }

        fs::path relative_path;
        try {
            relative_path = fs::relative(entry.path(), destination);
        } catch (const fs::filesystem_error& ex) {
            std::cerr << "    Warning: failed to compute relative path for destination entry " << entry.path()
                      << ": " << ex.what() << std::endl;
            if (entry.is_directory()) {
                it.disable_recursion_pending();
            }
            continue;
        }

        const fs::path source_match = source / relative_path;
        if (fs::exists(source_match)) {
            continue;
        }

        const bool is_dir = S_ISDIR(static_cast<mode_t>(dest_meta.mode));
        const std::size_t depth = static_cast<std::size_t>(std::distance(entry.path().begin(), entry.path().end()));
        candidates.push_back(RemovalCandidate{entry.path(), is_dir, depth});
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const RemovalCandidate& lhs, const RemovalCandidate& rhs) { return lhs.depth > rhs.depth; });

    for (const auto& candidate : candidates) {
        try {
            if (candidate.is_directory) {
                std::cout << "    Removing extraneous directory: " << candidate.path << std::endl;
                const std::uintmax_t removed = fs::remove_all(candidate.path);
                stats.files_deleted += removed;
            } else {
                std::cout << "    Removing extraneous file: " << candidate.path << std::endl;
                if (fs::remove(candidate.path)) {
                    ++stats.files_deleted;
                }
            }
        } catch (const fs::filesystem_error& ex) {
            std::cerr << "    Warning: failed to remove " << candidate.path << ": " << ex.what() << std::endl;
        }
    }

    stats.prune_elapsed = Clock::now() - prune_start;
}

bool DirectorySyncer::collect_metadata(const fs::path& path, int depth, FileMetadata& out) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        log_lstat_error(path, errno);
        return false;
    }

    out.file = path;
    out.depth = depth;
    out.detail = true;
    out.mode = static_cast<std::uint64_t>(st.st_mode);
    out.uid = static_cast<std::uint64_t>(st.st_uid);
    out.gid = static_cast<std::uint64_t>(st.st_gid);
    out.size = static_cast<std::uint64_t>(st.st_size);

#if defined(__APPLE__) || defined(__MACH__)
    const auto atime = st.st_atimespec;
    const auto mtime = st.st_mtimespec;
    const auto ctime = st.st_ctimespec;
#else
    const auto atime = st.st_atim;
    const auto mtime = st.st_mtim;
    const auto ctime = st.st_ctim;
#endif

    out.atime = static_cast<std::uint64_t>(atime.tv_sec);
    out.atime_nsec = static_cast<std::uint64_t>(atime.tv_nsec);
    out.mtime = static_cast<std::uint64_t>(mtime.tv_sec);
    out.mtime_nsec = static_cast<std::uint64_t>(mtime.tv_nsec);
    out.ctime = static_cast<std::uint64_t>(ctime.tv_sec);
    out.ctime_nsec = static_cast<std::uint64_t>(ctime.tv_nsec);

    return true;
}

void DirectorySyncer::log_lstat_error(const fs::path& path, int err) {
    std::cerr << "    Error: lstat failed for " << path << ": " << std::strerror(err) << " (errno " << err << ")"
              << std::endl;
}

void print_report(const SyncStats& stats) {
    std::cout << "\n=== Synchronization Summary ===" << std::endl;
    std::cout << "  Entries scanned:      " << stats.entries_scanned << std::endl;
    std::cout << "  Files copied:         " << stats.files_copied << std::endl;
    std::cout << "  Files skipped:        " << stats.files_skipped << std::endl;
    std::cout << "  Directories created:  " << stats.directories_created << std::endl;
    std::cout << "  Entries deleted:      " << stats.files_deleted << std::endl;
    std::cout << "  Bytes copied:         " << stats.bytes_copied << std::endl;

    auto print_duration = [](const std::string& label, const std::chrono::duration<double>& d) {
        std::cout << "  " << std::setw(20) << std::left << (label + ":") << std::fixed << std::setprecision(3)
                  << d.count() << " s" << std::endl;
    };

    print_duration("Scan elapsed", stats.scan_elapsed);
    print_duration("Copy elapsed", stats.copy_elapsed);
    print_duration("Prune elapsed", stats.prune_elapsed);
    print_duration("Total elapsed", stats.total_elapsed);

    const double total_seconds = stats.total_elapsed.count();
    if (total_seconds > 0.0) {
        const double mb = static_cast<double>(stats.bytes_copied) / (1024.0 * 1024.0);
        const double throughput = mb / total_seconds;
        std::cout << "  Effective throughput: " << std::fixed << std::setprecision(3) << throughput << " MiB/s"
                  << std::endl;
    } else {
        std::cout << "  Effective throughput: n/a" << std::endl;
    }
}

void print_synced_metadata(const std::vector<FileMetadata>& entries) {
    if (entries.empty()) {
        std::cout << "\nNo entries were synchronized." << std::endl;
        return;
    }

    std::cout << "\n=== Synchronized Source Entries ===" << std::endl;
    for (const auto& meta : entries) {
        std::cout << "  Path: " << meta.file << "\n"
                  << "    depth: " << meta.depth << "\n"
                  << "    mode: " << meta.mode << "\n"
                  << "    uid: " << meta.uid << ", gid: " << meta.gid << "\n"
                  << "    size: " << meta.size << " bytes\n"
                  << "    mtime: " << meta.mtime << "s + " << meta.mtime_nsec << "ns\n"
                  << "    atime: " << meta.atime << "s + " << meta.atime_nsec << "ns\n"
                  << "    ctime: " << meta.ctime << "s + " << meta.ctime_nsec << "ns\n";
    }
}

} // namespace mfs
