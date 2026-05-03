#ifndef EXECUTOR_H
#define EXECUTOR_H

#include <string>
#include <memory>
#include <vector>
#include <sstream>
#include <functional>
#include "b_plus_tree.h"

namespace relational_db {

/**
 * @brief Simple database value for compatibility with older compilers.
 */
struct DbValue {
    enum Type { INT, DOUBLE, STRING, BOOL } type;
    int i_val;
    double d_val;
    std::string s_val;
    bool b_val;

    DbValue(int v) : type(INT), i_val(v), d_val(0), b_val(false) {}
    DbValue(double v) : type(DOUBLE), i_val(0), d_val(v), b_val(false) {}
    DbValue(const std::string& v) : type(STRING), i_val(0), d_val(0), s_val(v), b_val(false) {}
    DbValue(bool v) : type(BOOL), i_val(0), d_val(0), b_val(v) {}
};

using Row = std::vector<DbValue>;
using Predicate = std::function<bool(const Row&)>;

class Executor {
public:
    explicit Executor(std::shared_ptr<BufferPoolManager> bpm) 
        : bpm_(std::move(bpm)), tree_(bpm_, std::less<int>()) {}

    /**
     * @brief Executes a simple string-based query.
     * Backported to use std::string for compatibility.
     */
    std::string ExecuteQuery(const std::string& query);

    std::vector<Row> ScanWithPredicate(int start_key, int end_key, Predicate pred);

    static std::string FormatValue(const DbValue& val);

private:
    std::shared_ptr<BufferPoolManager> bpm_;
    BPlusTree<int, int, std::less<int>> tree_;
};

} // namespace relational_db

#endif // EXECUTOR_H
