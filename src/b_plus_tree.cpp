#include "b_plus_tree.h"

namespace relational_db {

static_assert(sizeof(bpt_detail::IndexMetaLayout) == 8,
              "INDEX_META layout must remain stable");

}  // namespace relational_db
