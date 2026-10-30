#include <array>
#include <iostream>

#include "disk_manager.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: disk_sync_probe DATABASE_PATH\n";
        return 2;
    }

    relational_db::FileDiskManager manager(argv[1]);
    manager.Extend(1);
    std::array<char, relational_db::PAGE_DATA_SIZE> page{};
    page.fill('s');
    manager.WritePage(1, page.data());
    manager.Sync();
    return 0;
}
