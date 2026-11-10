#pragma once

#include "duckdb/common/file_system.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/http/http_util.hpp"

#include <azure/core/context.hpp>
#include <azure/core/http/transport.hpp>
#include <azure/core/io/body_stream.hpp>
#include <memory>
#include <span>
#include <string>

namespace duckdb {

class FileOpener;

class AzureWriteBodyStream final : public Azure::Core::IO::BodyStream {
public:
	explicit AzureWriteBodyStream(std::span<const HTTPBodyPiece> pieces_p);

	int64_t Length() const override {
		return NumericCast<int64_t>(size);
	}
	void Rewind() override {
		reader.Seek(0);
	}
	std::span<const HTTPBodyPiece> Pieces() const {
		return pieces;
	}

private:
	size_t OnRead(uint8_t *buffer, size_t count, Azure::Core::Context const &context) override;

	std::span<const HTTPBodyPiece> pieces;
	HTTPBodyReader reader;
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
