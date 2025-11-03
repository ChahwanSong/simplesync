#include "sync.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path();
        path = base / fs::path("mfs_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void copy_tree(const fs::path& from, const fs::path& to) {
    fs::create_directories(to);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
}

std::string read_file(const fs::path& file) {
    std::ifstream input(file);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void assert_file_equals(const fs::path& expected, const fs::path& actual) {
    const std::string expected_content = read_file(expected);
    const std::string actual_content = read_file(actual);
    assert(expected_content == actual_content);
}

void test_default_sync(const fs::path& source_root, const fs::path& dest_root) {
    TempDir temp_source;
    TempDir temp_dest;
    copy_tree(source_root, temp_source.path);
    copy_tree(dest_root, temp_dest.path);

    mfs::DirectorySyncer syncer;
    auto stats = syncer.synchronize(temp_source.path, temp_dest.path);
    mfs::print_report(stats);
    mfs::print_synced_metadata(stats.synced_entries);

    // Validate expected outcomes.
    assert_file_equals(temp_source.path / "file1.txt", temp_dest.path / "file1.txt");
    assert_file_equals(temp_source.path / "dirA" / "file2.txt", temp_dest.path / "dirA" / "file2.txt");
    assert_file_equals(temp_source.path / "dirA" / "subdir" / "file3.txt",
                       temp_dest.path / "dirA" / "subdir" / "file3.txt");
    assert_file_equals(temp_source.path / "dirB" / "updated.txt", temp_dest.path / "dirB" / "updated.txt");

    assert(!fs::exists(temp_dest.path / "extra.txt"));
    assert(!fs::exists(temp_dest.path / "dirA" / "subdir" / "obsolete.txt"));

    // Stats should reflect work done.
    assert(stats.files_copied >= 2);
    assert(stats.files_deleted >= 1);
    assert(stats.bytes_copied > 0);

    std::set<fs::path> expected_rel_paths = {
        fs::path("file1.txt"),
        fs::path("dirB/updated.txt"),
        fs::path("dirA/subdir/file3.txt"),
    };
    std::set<fs::path> actual_rel_paths;
    for (const auto& meta : stats.synced_entries) {
        std::error_code ec;
        fs::path rel = fs::relative(meta.file, temp_source.path, ec);
        if (ec) {
            throw std::runtime_error("Failed to resolve relative path for " + meta.file.string());
        }
        actual_rel_paths.insert(rel);
    }
    assert(actual_rel_paths == expected_rel_paths);
}

void test_keep_extra(const fs::path& source_root, const fs::path& dest_root) {
    TempDir temp_source;
    TempDir temp_dest;
    copy_tree(source_root, temp_source.path);
    copy_tree(dest_root, temp_dest.path);

    mfs::SyncOptions options;
    options.remove_extraneous = false;

    mfs::DirectorySyncer syncer(options);
    auto stats = syncer.synchronize(temp_source.path, temp_dest.path);
    mfs::print_report(stats);
    mfs::print_synced_metadata(stats.synced_entries);

    assert(fs::exists(temp_dest.path / "extra.txt"));
    assert(stats.files_deleted == 0);
    assert(stats.synced_entries.size() == 3);
}

} // namespace

int main() {
    try {
        const fs::path project_root = fs::canonical(fs::path(__FILE__)).parent_path().parent_path();
        const fs::path source_root = project_root / "testdata" / "source_base";
        const fs::path dest_root = project_root / "testdata" / "dest_base";

        test_default_sync(source_root, dest_root);
        test_keep_extra(source_root, dest_root);

    } catch (const std::exception& ex) {
        std::cerr << "Test failure: " << ex.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception raised." << std::endl;
        return 1;
    }

    std::cout << "All tests passed." << std::endl;
    return 0;
}
