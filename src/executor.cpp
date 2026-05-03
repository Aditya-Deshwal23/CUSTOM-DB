#include "executor.h"
#include <iostream>
#include <sstream>

namespace relational_db {

static std::string nextToken(const std::string& sv, size_t& pos) {
    while (pos < sv.size() && sv[pos] == ' ') ++pos;
    if (pos >= sv.size()) return "";
    
    size_t start = pos;
    while (pos < sv.size() && sv[pos] != ' ') ++pos;
    return sv.substr(start, pos - start);
}

std::string Executor::ExecuteQuery(const std::string& query) {
    size_t pos = 0;
    std::string command = nextToken(query, pos);

    if (command == "INSERT") {
        std::string key_s = nextToken(query, pos);
        std::string val_s = nextToken(query, pos);
        
        try {
            int key = std::stoi(key_s);
            int value = std::stoi(val_s);
            bool success = tree_.Insert(key, value);
            if (success) {
                return "Inserted (" + std::to_string(key) + ", " + std::to_string(value) + ")";
            } else {
                return "Key " + std::to_string(key) + " already exists.";
            }
        } catch (...) {
            return "Syntax error: INSERT <key> <value>";
        }
    } 
    else if (command == "SELECT") {
        std::string key_s = nextToken(query, pos);
        try {
            int key = std::stoi(key_s);
            std::vector<int> result;
            if (tree_.GetValue(key, &result)) {
                return "Result: " + std::to_string(result[0]);
            } else {
                return "Key " + std::to_string(key) + " not found.";
            }
        } catch (...) {
            return "Syntax error: SELECT <key>";
        }
    }

    return "Unknown command. Use INSERT or SELECT.";
}

std::vector<Row> Executor::ScanWithPredicate(int start_key, int end_key, Predicate pred) {
    std::vector<Row> results;
    std::vector<int> raw_values;
    tree_.Scan(start_key, end_key, &raw_values);
    
    int current_key = start_key;
    for (size_t i = 0; i < raw_values.size(); ++i) {
        Row row;
        row.push_back(DbValue(current_key));
        row.push_back(DbValue(raw_values[i]));
        
        if (pred(row)) {
            results.push_back(row);
        }
        ++current_key;
    }
    
    return results;
}

std::string Executor::FormatValue(const DbValue& val) {
    switch (val.type) {
        case DbValue::INT: return std::to_string(val.i_val);
        case DbValue::DOUBLE: return std::to_string(val.d_val);
        case DbValue::STRING: return "'" + val.s_val + "'";
        case DbValue::BOOL: return val.b_val ? "TRUE" : "FALSE";
        default: return "UNKNOWN";
    }
}

} // namespace relational_db
