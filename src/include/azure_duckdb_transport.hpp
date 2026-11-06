#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/optional_ptr.hpp"

#include <azure/core/context.hpp>
#include <azure/core/http/transport.hpp>
#include <memory>
#include <string>

namespace duckdb {

class FileOpener;

struct AzureWriteBody {
	const_data_ptr_t data = nullptr;
	idx_t size = 0;
};

const Azure::Core::Context::Key &AzureReadTargetsKey();
const Azure::Core::Context::Key &AzureWriteBodyKey();

// HttpTransport backed by DuckDB's HTTPUtil (the httpfs curl client and its
// connection cache when httpfs is loaded), replacing the Azure SDK's own
// libcurl transport. Proxy settings override the ones resolved from the
// opener.
std::shared_ptr<Azure::Core::Http::HttpTransport> CreateDuckDBTransport(optional_ptr<FileOpener> opener,
                                                                        const std::string &proxy,
                                                                        const std::string &proxy_username,
                                                                        const std::string &proxy_password);

} // namespace duckdb
