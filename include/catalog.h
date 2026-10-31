#ifndef RELDB_CATALOG_H
#define RELDB_CATALOG_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "b_plus_tree.h"
#include "storage/table_heap.h"

namespace relational_db {

using SqlIndexTree = BPlusTree<IndexKey16, uint64_t, IndexKey16Less>;

struct CatalogIndex {
    std::string name;
    size_t column{0};
    bool unique{false};
    page_id_t meta_page_id{INVALID_PAGE_ID};
    std::unique_ptr<SqlIndexTree> tree;
};

struct CatalogTable {
    std::string name;
    Schema schema;
    std::vector<bool> not_null;
    std::unique_ptr<TableHeap> heap;
    std::vector<CatalogIndex> indexes;
};

class Catalog {
   public:
    Catalog(std::shared_ptr<BufferPoolManager> buffer_pool, page_id_t owner_meta_page_id);

    CatalogTable* FindTable(std::string_view name);
    const CatalogTable* FindTable(std::string_view name) const;
    const std::vector<std::unique_ptr<CatalogTable>>& Tables() const noexcept {
        return tables_;
    }

    CatalogTable& CreateTable(std::string name, Schema schema, std::vector<bool> not_null);
    CatalogIndex& CreateIndex(CatalogTable& table, std::string name, size_t column, bool unique);
    void DropTable(std::string_view name);
    void Reload();

   private:
    void Persist();
    void Load(page_id_t catalog_page_id);

    std::shared_ptr<BufferPoolManager> buffer_pool_;
    page_id_t owner_meta_page_id_;
    page_id_t catalog_page_id_{INVALID_PAGE_ID};
    std::vector<std::unique_ptr<CatalogTable>> tables_;
};

IndexKey16 IndexPrefix(const Value& value);
IndexKey16 IndexEntryKey(const Value& value, uint64_t packed_rid);
std::vector<uint64_t> FindIndexRids(const SqlIndexTree& tree, const Value& value);

}  // namespace relational_db

#endif
