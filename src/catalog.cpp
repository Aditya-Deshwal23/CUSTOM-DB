#include "catalog.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "storage/storage_format.h"

namespace relational_db {
namespace {

constexpr uint32_t kCatalogVersion = 1;
constexpr size_t kCatalogPointerOffset = sizeof(bpt_detail::IndexMetaLayout) + sizeof(page_id_t);
constexpr size_t kCatalogHeaderSize = 6;
using BytePage = std::array<char, PAGE_DATA_SIZE>;

class Writer {
   public:
    void U8(uint8_t value) {
        bytes_.push_back(value);
    }

    void U16(uint16_t value) {
        U8(static_cast<uint8_t>(value));
        U8(static_cast<uint8_t>(value >> 8U));
    }

    void U32(uint32_t value) {
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            U8(static_cast<uint8_t>(value >> (byte * 8U)));
        }
    }

    void String(std::string_view value) {
        if (value.size() > std::numeric_limits<uint16_t>::max()) {
            throw DbError(ErrorCode::INVALID_ARGUMENT, "Catalog string is too long");
        }
        U16(static_cast<uint16_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    const std::vector<uint8_t>& Bytes() const noexcept {
        return bytes_;
    }

   private:
    std::vector<uint8_t> bytes_;
};

class Reader {
   public:
    Reader(const uint8_t* bytes, size_t size) : bytes_(bytes), size_(size) {}

    uint8_t U8() {
        Require(1);
        return bytes_[offset_++];
    }

    uint16_t U16() {
        const uint16_t low = U8();
        return static_cast<uint16_t>(low | (static_cast<uint16_t>(U8()) << 8U));
    }

    uint32_t U32() {
        uint32_t value = 0;
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            value |= static_cast<uint32_t>(U8()) << (byte * 8U);
        }
        return value;
    }

    std::string String() {
        const size_t length = U16();
        Require(length);
        const char* text = reinterpret_cast<const char*>(bytes_ + offset_);
        offset_ += length;
        return std::string(text, length);
    }

    size_t Offset() const noexcept {
        return offset_;
    }

   private:
    void Require(size_t count) const {
        if (count > size_ - offset_) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Catalog metadata is truncated");
        }
    }

    const uint8_t* bytes_;
    size_t size_;
    size_t offset_{0};
};

std::string CanonicalName(std::string_view name) {
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

page_id_t CreateIndexMeta(BufferPoolManager& buffer_pool) {
    WritePageGuard guard = buffer_pool.NewPageGuarded();
    const page_id_t page_id = guard.GetPageId();
    guard.SetPageType(PageType::INDEX_META);
    auto* body = guard.AsMut<BytePage>();
    body->fill(0);
    auto* meta = reinterpret_cast<bpt_detail::IndexMetaLayout*>(body->data());
    bpt_detail::WritePageId(meta->root_page_id.data(), INVALID_PAGE_ID);
    bpt_detail::WriteU16(meta->height.data(), 0);
    bpt_detail::WriteU16(meta->key_kind.data(), static_cast<uint16_t>(IndexKeyKind::NON_UNIQUE));
    return page_id;
}

void WriteIndexMeta(Writer& writer, const CatalogIndex& index) {
    writer.String(index.name);
    writer.U16(static_cast<uint16_t>(index.column));
    writer.U8(index.unique ? 1U : 0U);
    writer.U32(static_cast<uint32_t>(index.meta_page_id));
}

}  // namespace

Catalog::Catalog(std::shared_ptr<BufferPoolManager> buffer_pool, page_id_t owner_meta_page_id)
    : buffer_pool_(std::move(buffer_pool)), owner_meta_page_id_(owner_meta_page_id) {
    if (!buffer_pool_ || owner_meta_page_id_ <= 0) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Catalog requires a valid metadata page");
    }
    Reload();
}

CatalogTable* Catalog::FindTable(std::string_view name) {
    const std::string canonical = CanonicalName(name);
    const auto found = std::find_if(
        tables_.begin(), tables_.end(),
        [&canonical](const auto& table) { return CanonicalName(table->name) == canonical; });
    return found == tables_.end() ? nullptr : found->get();
}

const CatalogTable* Catalog::FindTable(std::string_view name) const {
    const std::string canonical = CanonicalName(name);
    const auto found = std::find_if(
        tables_.begin(), tables_.end(),
        [&canonical](const auto& table) { return CanonicalName(table->name) == canonical; });
    return found == tables_.end() ? nullptr : found->get();
}

CatalogTable& Catalog::CreateTable(std::string name, Schema schema, std::vector<bool> not_null) {
    if (FindTable(name) != nullptr) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Table already exists: " + name);
    }
    schema.Validate();
    if (not_null.size() != schema.columns.size()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Catalog nullability does not match schema");
    }

    auto table = std::make_unique<CatalogTable>();
    table->name = std::move(name);
    table->schema = std::move(schema);
    table->not_null = std::move(not_null);
    TableHeap heap = TableHeap::Create(*buffer_pool_);
    table->heap = std::make_unique<TableHeap>(std::move(heap));
    if (table->schema.primary_key) {
        const size_t column = *table->schema.primary_key;
        CatalogIndex index;
        index.name = "pk_" + table->name + "_" + table->schema.columns[column].name;
        index.column = column;
        index.unique = true;
        index.meta_page_id = CreateIndexMeta(*buffer_pool_);
        index.tree = std::make_unique<SqlIndexTree>(buffer_pool_, IndexKey16Less(),
                                                    index.meta_page_id, IndexKeyKind::NON_UNIQUE);
        table->indexes.push_back(std::move(index));
    }
    tables_.push_back(std::move(table));
    Persist();
    return *tables_.back();
}

CatalogIndex& Catalog::CreateIndex(CatalogTable& table, std::string name, size_t column,
                                   bool unique) {
    if (column >= table.schema.columns.size()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Index column is outside the table schema");
    }
    if (std::any_of(table.indexes.begin(), table.indexes.end(), [&name](const CatalogIndex& index) {
            return CanonicalName(index.name) == CanonicalName(name);
        })) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Index already exists: " + name);
    }

    CatalogIndex index;
    index.name = std::move(name);
    index.column = column;
    index.unique = unique;
    index.meta_page_id = CreateIndexMeta(*buffer_pool_);
    index.tree = std::make_unique<SqlIndexTree>(buffer_pool_, IndexKey16Less(), index.meta_page_id,
                                                IndexKeyKind::NON_UNIQUE);

    TableIterator iterator = table.heap->Begin();
    RID rid{};
    Tuple tuple;
    while (iterator.Next(rid, tuple)) {
        const Row row = DeserializeTuple(table.schema, tuple);
        const Value& value = row[column];
        if (std::holds_alternative<std::monostate>(value)) {
            continue;
        }
        if (unique) {
            for (uint64_t packed_rid : FindIndexRids(*index.tree, value)) {
                const RID existing_rid = RID::Unpack(packed_rid);
                const std::optional<Tuple> existing_tuple = table.heap->Get(existing_rid);
                if (!existing_tuple) {
                    throw DbError(ErrorCode::PAGE_CORRUPT,
                                  "Index references a missing row during CREATE INDEX",
                                  existing_rid.page_id);
                }
                const Row existing_row = DeserializeTuple(table.schema, *existing_tuple);
                if (existing_row[column] == value) {
                    throw DbError(ErrorCode::DUPLICATE_KEY,
                                  "Unique index contains duplicate values");
                }
            }
        }
        index.tree->Insert(IndexEntryKey(value, rid.Pack()), rid.Pack());
    }
    table.indexes.push_back(std::move(index));
    Persist();
    return table.indexes.back();
}

void Catalog::DropTable(std::string_view name) {
    const std::string canonical = CanonicalName(name);
    const auto found = std::find_if(
        tables_.begin(), tables_.end(),
        [&canonical](const auto& table) { return CanonicalName(table->name) == canonical; });
    if (found == tables_.end()) {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "Unknown table: " + std::string(name));
    }
    for (CatalogIndex& index : (*found)->indexes) {
        index.tree->Drop();
    }
    (*found)->heap->Drop();
    tables_.erase(found);
    Persist();
}

void Catalog::Reload() {
    page_id_t root = INVALID_PAGE_ID;
    {
        ReadPageGuard guard = buffer_pool_->FetchRead(owner_meta_page_id_);
        const auto* body = guard.As<BytePage>();
        root = bpt_detail::ReadPageId(
            reinterpret_cast<const uint8_t*>(body->data() + kCatalogPointerOffset));
    }
    if (root == 0) {
        root = INVALID_PAGE_ID;
    }
    catalog_page_id_ = root;
    tables_.clear();
    if (catalog_page_id_ != INVALID_PAGE_ID) {
        Load(catalog_page_id_);
    }
}

void Catalog::Load(page_id_t catalog_page_id) {
    ReadPageGuard guard = buffer_pool_->FetchRead(catalog_page_id);
    if (guard.GetPageType() != PageType::CATALOG) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Catalog root has the wrong page type",
                      catalog_page_id);
    }
    const auto* body = guard.As<BytePage>();
    std::vector<uint8_t> catalog_bytes(body->size());
    std::memcpy(catalog_bytes.data(), body->data(), body->size());
    guard.Drop();
    Reader reader(catalog_bytes.data(), catalog_bytes.size());
    if (reader.U32() != kCatalogVersion) {
        throw DbError(ErrorCode::UNSUPPORTED_VERSION, "Unsupported catalog version",
                      catalog_page_id);
    }
    const uint16_t table_count = reader.U16();
    tables_.reserve(table_count);
    for (uint16_t table_index = 0; table_index < table_count; ++table_index) {
        auto table = std::make_unique<CatalogTable>();
        table->name = reader.String();
        const page_id_t heap_page_id = static_cast<page_id_t>(reader.U32());
        const uint16_t column_count = reader.U16();
        const uint16_t primary_key = reader.U16();
        table->schema.columns.reserve(column_count);
        table->not_null.reserve(column_count);
        for (uint16_t column_index = 0; column_index < column_count; ++column_index) {
            Column column;
            column.name = reader.String();
            const uint8_t type = reader.U8();
            if (type > static_cast<uint8_t>(ColumnType::VARCHAR)) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Catalog column has an invalid type",
                              catalog_page_id);
            }
            column.type = static_cast<ColumnType>(type);
            column.varchar_length = reader.U16();
            table->not_null.push_back(reader.U8() != 0);
            table->schema.columns.push_back(std::move(column));
        }
        if (primary_key != UINT16_MAX) {
            table->schema.primary_key = primary_key;
        }
        table->schema.Validate();
        table->heap = std::make_unique<TableHeap>(*buffer_pool_, heap_page_id);
        const uint16_t index_count = reader.U16();
        table->indexes.reserve(index_count);
        for (uint16_t index_number = 0; index_number < index_count; ++index_number) {
            CatalogIndex index;
            index.name = reader.String();
            index.column = reader.U16();
            index.unique = reader.U8() != 0;
            index.meta_page_id = static_cast<page_id_t>(reader.U32());
            if (index.column >= table->schema.columns.size() || index.meta_page_id <= 0) {
                throw DbError(ErrorCode::PAGE_CORRUPT, "Catalog index metadata is invalid",
                              catalog_page_id);
            }
            index.tree = std::make_unique<SqlIndexTree>(
                buffer_pool_, IndexKey16Less(), index.meta_page_id, IndexKeyKind::NON_UNIQUE);
            table->indexes.push_back(std::move(index));
        }
        tables_.push_back(std::move(table));
    }
    if (reader.Offset() > catalog_bytes.size() ||
        table_count > catalog_bytes.size() / kCatalogHeaderSize) {
        throw DbError(ErrorCode::PAGE_CORRUPT, "Catalog table count is invalid", catalog_page_id);
    }
}

void Catalog::Persist() {
    Writer writer;
    writer.U32(kCatalogVersion);
    if (tables_.size() > std::numeric_limits<uint16_t>::max()) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Too many tables in catalog");
    }
    writer.U16(static_cast<uint16_t>(tables_.size()));
    for (const auto& table : tables_) {
        writer.String(table->name);
        writer.U32(static_cast<uint32_t>(table->heap->HeaderPageId()));
        if (table->schema.columns.size() > std::numeric_limits<uint16_t>::max() ||
            table->indexes.size() > std::numeric_limits<uint16_t>::max()) {
            throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Catalog table metadata is too large");
        }
        writer.U16(static_cast<uint16_t>(table->schema.columns.size()));
        writer.U16(table->schema.primary_key ? static_cast<uint16_t>(*table->schema.primary_key)
                                             : UINT16_MAX);
        for (size_t column_index = 0; column_index < table->schema.columns.size(); ++column_index) {
            const Column& column = table->schema.columns[column_index];
            writer.String(column.name);
            writer.U8(static_cast<uint8_t>(column.type));
            writer.U16(column.varchar_length);
            writer.U8(table->not_null[column_index] ? 1U : 0U);
        }
        writer.U16(static_cast<uint16_t>(table->indexes.size()));
        for (const CatalogIndex& index : table->indexes) {
            WriteIndexMeta(writer, index);
        }
    }
    if (writer.Bytes().size() > PAGE_DATA_SIZE) {
        throw DbError(ErrorCode::TUPLE_TOO_LARGE, "Catalog metadata exceeds one page");
    }

    if (catalog_page_id_ == INVALID_PAGE_ID) {
        WritePageGuard guard = buffer_pool_->NewPageGuarded();
        catalog_page_id_ = guard.GetPageId();
        guard.SetPageType(PageType::CATALOG);
        auto* body = guard.AsMut<BytePage>();
        body->fill(0);
        std::memcpy(body->data(), writer.Bytes().data(), writer.Bytes().size());
        guard.Drop();

        WritePageGuard owner_guard = buffer_pool_->FetchWrite(owner_meta_page_id_);
        auto* owner_body = owner_guard.AsMut<BytePage>();
        bpt_detail::WritePageId(
            reinterpret_cast<uint8_t*>(owner_body->data() + kCatalogPointerOffset),
            catalog_page_id_);
    } else {
        WritePageGuard guard = buffer_pool_->FetchWrite(catalog_page_id_);
        if (guard.GetPageType() != PageType::CATALOG) {
            throw DbError(ErrorCode::PAGE_CORRUPT, "Catalog root has the wrong page type",
                          catalog_page_id_);
        }
        auto* body = guard.AsMut<BytePage>();
        body->fill(0);
        std::memcpy(body->data(), writer.Bytes().data(), writer.Bytes().size());
    }
}

IndexKey16 IndexPrefix(const Value& value) {
    IndexKey16 key;
    if (const auto* integer = std::get_if<int32_t>(&value)) {
        key = EncodeIndexKey(*integer);
    } else if (const auto* integer = std::get_if<int64_t>(&value)) {
        key = EncodeIndexKey(*integer);
    } else if (const auto* number = std::get_if<double>(&value)) {
        uint64_t bits = 0;
        std::memcpy(&bits, number, sizeof(bits));
        const uint64_t sortable =
            (bits & (uint64_t{1} << 63U)) != 0 ? ~bits : bits ^ (uint64_t{1} << 63U);
        for (size_t byte = 0; byte < sizeof(sortable); ++byte) {
            key.bytes[byte] =
                static_cast<uint8_t>(sortable >> ((sizeof(sortable) - byte - 1U) * 8U));
        }
    } else if (const auto* boolean = std::get_if<bool>(&value)) {
        key = EncodeIndexKey(*boolean);
    } else if (const auto* text = std::get_if<std::string>(&value)) {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char ch : *text) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        for (size_t byte = 0; byte < sizeof(hash); ++byte) {
            key.bytes[byte] = static_cast<uint8_t>(hash >> ((sizeof(hash) - byte - 1U) * 8U));
        }
    } else {
        throw DbError(ErrorCode::INVALID_ARGUMENT, "NULL values do not have index keys");
    }
    return key;
}

IndexKey16 IndexEntryKey(const Value& value, uint64_t packed_rid) {
    IndexKey16 key = IndexPrefix(value);
    for (size_t byte = 0; byte < sizeof(packed_rid); ++byte) {
        key.bytes[sizeof(key.bytes) - byte - 1U] = static_cast<uint8_t>(packed_rid >> (byte * 8U));
    }
    return key;
}

std::vector<uint64_t> FindIndexRids(const SqlIndexTree& tree, const Value& value) {
    const IndexKey16 prefix = IndexPrefix(value);
    IndexKey16 low = prefix;
    IndexKey16 high = prefix;
    std::fill(low.bytes.begin() + 8, low.bytes.end(), 0);
    std::fill(high.bytes.begin() + 8, high.bytes.end(), UINT8_MAX);
    std::vector<uint64_t> result;
    for (const auto& entry : tree.RangeScan(low, high, true, true)) {
        if (std::equal(prefix.bytes.begin(), prefix.bytes.begin() + 8, entry.first.bytes.begin())) {
            result.push_back(entry.second);
        }
    }
    return result;
}

}  // namespace relational_db
