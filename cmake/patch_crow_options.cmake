set(CROW_ROUTER_FILE "${crow_SOURCE_DIR}/include/crow/routing.h")
file(READ "${CROW_ROUTER_FILE}" CROW_ROUTER_CONTENT)

string(FIND "${CROW_ROUTER_CONTENT}" "if (found->rule_index) return found;" PATCH_ALREADY_APPLIED)
if(PATCH_ALREADY_APPLIED EQUAL -1)
  set(CROW_OPTIONS_BLOCK
      "            else if (req.method == HTTPMethod::Options)\n            {\n                std::string allow =")
  set(CROW_OPTIONS_PATCH
      "            else if (req.method == HTTPMethod::Options)\n            {\n                *found = per_methods_[static_cast<int>(method_actual)].trie.find(req.url);\n                if (found->rule_index) return found;\n                std::string allow =")
  string(FIND "${CROW_ROUTER_CONTENT}" "${CROW_OPTIONS_BLOCK}" CROW_OPTIONS_BLOCK_POSITION)
  if(CROW_OPTIONS_BLOCK_POSITION EQUAL -1)
    message(FATAL_ERROR "Crow OPTIONS handler changed; update the API preflight compatibility patch")
  endif()
  string(REPLACE "${CROW_OPTIONS_BLOCK}" "${CROW_OPTIONS_PATCH}" CROW_ROUTER_CONTENT
                 "${CROW_ROUTER_CONTENT}")
  file(WRITE "${CROW_ROUTER_FILE}" "${CROW_ROUTER_CONTENT}")
endif()

set(CROW_COMMON_FILE "${crow_SOURCE_DIR}/include/crow/common.h")
file(READ "${CROW_COMMON_FILE}" CROW_COMMON_CONTENT)
string(FIND "${CROW_COMMON_CONTENT}" "UNPROCESSABLE_ENTITY" CROW_STATUS_PATCHED)
if(CROW_STATUS_PATCHED EQUAL -1)
  set(CROW_STATUS_BLOCK "        EXPECTATION_FAILED            = 417,\n        PRECONDITION_REQUIRED")
  set(CROW_STATUS_PATCH
      "        EXPECTATION_FAILED             = 417,\n        UNPROCESSABLE_ENTITY            = 422,\n        PRECONDITION_REQUIRED")
  string(FIND "${CROW_COMMON_CONTENT}" "${CROW_STATUS_BLOCK}" CROW_STATUS_BLOCK_POSITION)
  if(CROW_STATUS_BLOCK_POSITION EQUAL -1)
    message(FATAL_ERROR "Crow status enum changed; update the API status compatibility patch")
  endif()
  string(REPLACE "${CROW_STATUS_BLOCK}" "${CROW_STATUS_PATCH}" CROW_COMMON_CONTENT
                 "${CROW_COMMON_CONTENT}")
  file(WRITE "${CROW_COMMON_FILE}" "${CROW_COMMON_CONTENT}")
endif()

string(FIND "${CROW_COMMON_CONTENT}" "INSUFFICIENT_STORAGE" CROW_QUOTA_STATUS_PATCHED)
if(CROW_QUOTA_STATUS_PATCHED EQUAL -1)
  set(CROW_QUOTA_STATUS_BLOCK
      "        GATEWAY_TIMEOUT               = 504,\n        VARIANT_ALSO_NEGOTIATES       = 506")
  set(CROW_QUOTA_STATUS_PATCH
      "        GATEWAY_TIMEOUT               = 504,\n        VARIANT_ALSO_NEGOTIATES       = 506,\n        INSUFFICIENT_STORAGE             = 507")
  string(FIND "${CROW_COMMON_CONTENT}" "${CROW_QUOTA_STATUS_BLOCK}"
         CROW_QUOTA_STATUS_BLOCK_POSITION)
  if(CROW_QUOTA_STATUS_BLOCK_POSITION EQUAL -1)
    message(FATAL_ERROR "Crow status enum changed; update the storage-quota status patch")
  endif()
  string(REPLACE "${CROW_QUOTA_STATUS_BLOCK}" "${CROW_QUOTA_STATUS_PATCH}" CROW_COMMON_CONTENT
                 "${CROW_COMMON_CONTENT}")
  file(WRITE "${CROW_COMMON_FILE}" "${CROW_COMMON_CONTENT}")
endif()

set(CROW_CONNECTION_FILE "${crow_SOURCE_DIR}/include/crow/http_connection.h")
file(READ "${CROW_CONNECTION_FILE}" CROW_CONNECTION_CONTENT)
string(FIND "${CROW_CONNECTION_CONTENT}" "status::UNPROCESSABLE_ENTITY" CROW_RESPONSE_PATCHED)
if(CROW_RESPONSE_PATCHED EQUAL -1)
  set(CROW_RESPONSE_BLOCK
      "              {status::EXPECTATION_FAILED, \"HTTP/1.1 417 Expectation Failed\\r\\n\"},\n              {status::PRECONDITION_REQUIRED")
  set(CROW_RESPONSE_PATCH
      "              {status::EXPECTATION_FAILED, \"HTTP/1.1 417 Expectation Failed\\r\\n\"},\n              {status::UNPROCESSABLE_ENTITY, \"HTTP/1.1 422 Unprocessable Entity\\r\\n\"},\n              {status::PRECONDITION_REQUIRED")
  string(FIND "${CROW_CONNECTION_CONTENT}" "${CROW_RESPONSE_BLOCK}"
         CROW_RESPONSE_BLOCK_POSITION)
  if(CROW_RESPONSE_BLOCK_POSITION EQUAL -1)
    message(FATAL_ERROR "Crow response map changed; update the API status compatibility patch")
  endif()
  string(REPLACE "${CROW_RESPONSE_BLOCK}" "${CROW_RESPONSE_PATCH}" CROW_CONNECTION_CONTENT
                 "${CROW_CONNECTION_CONTENT}")
endif()
string(FIND "${CROW_CONNECTION_CONTENT}" "status::INSUFFICIENT_STORAGE" CROW_QUOTA_RESPONSE_PATCHED)
if(CROW_QUOTA_RESPONSE_PATCHED EQUAL -1)
  set(CROW_QUOTA_RESPONSE_BLOCK
      "              {status::VARIANT_ALSO_NEGOTIATES, \"HTTP/1.1 506 Variant Also Negotiates\\r\\n\"},")
  set(CROW_QUOTA_RESPONSE_PATCH
      "              {status::VARIANT_ALSO_NEGOTIATES, \"HTTP/1.1 506 Variant Also Negotiates\\r\\n\"},\n              {status::INSUFFICIENT_STORAGE, \"HTTP/1.1 507 Insufficient Storage\\r\\n\"},")
  string(FIND "${CROW_CONNECTION_CONTENT}" "${CROW_QUOTA_RESPONSE_BLOCK}"
         CROW_QUOTA_RESPONSE_BLOCK_POSITION)
  if(CROW_QUOTA_RESPONSE_BLOCK_POSITION EQUAL -1)
    message(FATAL_ERROR "Crow response map changed; update the storage-quota status patch")
  endif()
  string(REPLACE "${CROW_QUOTA_RESPONSE_BLOCK}" "${CROW_QUOTA_RESPONSE_PATCH}"
                 CROW_CONNECTION_CONTENT "${CROW_CONNECTION_CONTENT}")
endif()
file(WRITE "${CROW_CONNECTION_FILE}" "${CROW_CONNECTION_CONTENT}")
