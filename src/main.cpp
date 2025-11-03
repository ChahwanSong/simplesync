#include "sync.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [--keep-extra] <source_dir> <destination_dir>\n"
              << "  --keep-extra   Preserve files that exist only in the destination directory.\n"
              << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }

    bool keep_extra = false;
    std::vector<std::string> positional_args;
    positional_args.reserve(2);

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--keep-extra") {
            keep_extra = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            positional_args.push_back(arg);
        }
    }

    if (positional_args.size() != 2) {
        std::cerr << "Error: expected source and destination directories.\n" << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    const std::filesystem::path source = positional_args[0];
    const std::filesystem::path destination = positional_args[1];

    mfs::SyncOptions options;
    options.remove_extraneous = !keep_extra;

    try {
        mfs::DirectorySyncer syncer(options);
        mfs::SyncStats stats = syncer.synchronize(source, destination);
        mfs::print_report(stats);
        mfs::print_synced_metadata(stats.synced_entries);
    } catch (const std::exception& ex) {
        std::cerr << "Synchronization failed: " << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
