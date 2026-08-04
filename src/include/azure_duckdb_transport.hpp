#pragma once

#include "duckdb/common/optional_ptr.hpp"

#include <azure/core/http/transport.hpp>
#include <memory>
#include <string>

namespace duckdb {

class FileOpener;

// HttpTransport backed by DuckDB's HTTPUtil (the httpfs curl client and its
// connection cache when httpfs is loaded), replacing the Azure SDK's own
// libcurl transport. Proxy settings override the ones resolved from the
// opener.
std::shared_ptr<Azure::Core::Http::HttpTransport>
CreateDuckDBTransport(optional_ptr<FileOpener> opener, const std::string &proxy, const std::string &proxy_username,
                      const std::string &proxy_password);

} // namespace duckdb
