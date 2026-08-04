#include "azure_duckdb_transport.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

#include <azure/core/http/http.hpp>
#include <azure/core/http/raw_response.hpp>
#include <azure/core/io/body_stream.hpp>

#include <cstdlib>
#include <cstring>

namespace duckdb {

namespace {

// RawResponse body stream that owns its bytes (the SDK's MemoryBodyStream does not).
class OwningBodyStream final : public Azure::Core::IO::BodyStream {
public:
	explicit OwningBodyStream(std::vector<uint8_t> data_p) : data(std::move(data_p)) {
	}
	int64_t Length() const override {
		return static_cast<int64_t>(data.size());
	}
	void Rewind() override {
		offset = 0;
	}

private:
	size_t OnRead(uint8_t *buffer, size_t count, Azure::Core::Context const &context) override {
		(void)context;
		size_t n = MinValue<size_t>(count, data.size() - offset);
		std::memcpy(buffer, data.data() + offset, n);
		offset += n;
		return n;
	}

	std::vector<uint8_t> data;
	size_t offset = 0;
};

class DuckDBTransport final : public Azure::Core::Http::HttpTransport {
public:
	DuckDBTransport(HTTPUtil &http_util_p, unique_ptr<HTTPParams> params_p)
	    : http_util(http_util_p), params(std::move(params_p)) {
	}

	std::unique_ptr<Azure::Core::Http::RawResponse> Send(Azure::Core::Http::Request &request,
	                                                     Azure::Core::Context const &context) override;

private:
	static std::vector<uint8_t> DrainRequestBody(Azure::Core::Http::Request &request,
	                                             Azure::Core::Context const &context);

	HTTPUtil &http_util;
	unique_ptr<HTTPParams> params;
};

std::vector<uint8_t> DuckDBTransport::DrainRequestBody(Azure::Core::Http::Request &request,
                                                       Azure::Core::Context const &context) {
	std::vector<uint8_t> body;
	auto *stream = request.GetBodyStream();
	if (!stream) {
		return body;
	}
	auto len = stream->Length();
	if (len < 0) {
		return stream->ReadToEnd(context);
	}
	// The pipeline RetryPolicy rewinds the stream between attempts; a fresh
	// attempt always starts at position 0, so no rewind is needed here.
	body.resize(static_cast<size_t>(len));
	auto read = stream->ReadToCount(body.data(), body.size(), context);
	body.resize(read);
	return body;
}

std::unique_ptr<Azure::Core::Http::RawResponse> DuckDBTransport::Send(Azure::Core::Http::Request &request,
                                                                      Azure::Core::Context const &context) {
	using Azure::Core::Http::HttpMethod;
	using Azure::Core::Http::TransportException;

	context.ThrowIfCancelled();

	const auto url = request.GetUrl().GetAbsoluteUrl();
	const auto &method = request.GetMethod();

	// Headers were already signed by the SDK pipeline: pass them through
	// verbatim. Content-Type is carried separately for PUT because the duckdb
	// client sets it from the request info.
	HTTPHeaders headers;
	std::string content_type;
	for (const auto &header : request.GetHeaders()) {
		if (method == HttpMethod::Put && StringUtil::CIEquals(header.first, "content-type")) {
			content_type = header.second;
			continue;
		}
		headers.Insert(header.first, header.second);
	}

	auto run = [&](BaseRequest &info) {
		info.try_request = true;
		unique_ptr<HTTPClient> client;
		return http_util.SendRequest(info, client);
	};

	unique_ptr<HTTPResponse> response;
	std::vector<uint8_t> body_out;
	std::vector<uint8_t> body_in;

	if (method == HttpMethod::Get) {
		GetRequestInfo info(
		    url, headers, *params,
		    [&](const HTTPResponse &resp) {
			    if (resp.HasHeader("Content-Length")) {
				    auto value = resp.GetHeaderValue("Content-Length");
				    char *end = nullptr;
				    auto content_length = std::strtoull(value.c_str(), &end, 10);
				    if (end != value.c_str()) {
					    body_out.reserve(content_length);
				    }
			    }
			    return true;
		    },
		    [&](const_data_ptr_t data, idx_t data_length) {
			    if (context.IsCancelled()) {
				    return false;
			    }
			    body_out.insert(body_out.end(), data, data + data_length);
			    return true;
		    });
		response = run(info);
	} else if (method == HttpMethod::Put) {
		body_in = DrainRequestBody(request, context);
		PutRequestInfo info(url, headers, *params, body_in.data(), body_in.size(), content_type);
		response = run(info);
	} else if (method == HttpMethod::Post) {
		body_in = DrainRequestBody(request, context);
		PostRequestInfo info(url, headers, *params, body_in.data(), body_in.size());
		response = run(info);
		body_out.assign(info.buffer_out.begin(), info.buffer_out.end());
	} else if (method == HttpMethod::Head) {
		HeadRequestInfo info(url, headers, *params);
		response = run(info);
	} else if (method == HttpMethod::Delete) {
		DeleteRequestInfo info(url, headers, *params);
		response = run(info);
	} else {
		throw TransportException("HTTP method '" + method.ToString() +
		                         "' is not supported by the duckdb azure transport");
	}

	context.ThrowIfCancelled();

	if (!response) {
		throw TransportException("request to '" + url + "' returned no response");
	}
	if (response->HasRequestError()) {
		throw TransportException("request to '" + url + "' failed: " + response->GetRequestError());
	}
	const auto status = static_cast<int>(response->status);
	if (status <= 0) {
		throw TransportException("request to '" + url + "' failed: " + response->GetError());
	}

	if (body_out.empty() && !response->body.empty()) {
		body_out.assign(response->body.begin(), response->body.end());
	}

	auto raw = std::make_unique<Azure::Core::Http::RawResponse>(
	    1, 1, static_cast<Azure::Core::Http::HttpStatusCode>(status), response->reason);
	for (auto it = response->headers.cbegin(); it != response->headers.cend(); ++it) {
		raw->SetHeader(it->first, it->second);
	}
	raw->SetBodyStream(std::make_unique<OwningBodyStream>(std::move(body_out)));
	return raw;
}

} // namespace

std::shared_ptr<Azure::Core::Http::HttpTransport>
CreateDuckDBTransport(optional_ptr<FileOpener> opener, const std::string &proxy, const std::string &proxy_username,
                      const std::string &proxy_password) {
	auto db = FileOpener::TryGetDatabase(opener);
	if (!db) {
		throw InternalException("azure transport requires a database instance");
	}
	auto &http_util = HTTPUtil::Get(*db);
	auto params = http_util.InitializeParameters(opener, nullptr);
	if (!proxy.empty()) {
		auto proxy_value = proxy;
		HTTPUtil::ParseHTTPProxyHost(proxy_value, params->http_proxy, params->http_proxy_port);
		params->http_proxy_username = proxy_username;
		params->http_proxy_password = proxy_password;
	}
	return std::make_shared<DuckDBTransport>(http_util, std::move(params));
}

} // namespace duckdb

// Safety net for the SDK's default-transport path (BUILD_TRANSPORT_CUSTOM_ADAPTER):
// duckdb_azure injects its transport into every client, so reaching this means a
// client was built without TransportOptions.
std::shared_ptr<Azure::Core::Http::HttpTransport> AzureSdkGetCustomHttpTransport() {
	class NoTransport final : public Azure::Core::Http::HttpTransport {
		std::unique_ptr<Azure::Core::Http::RawResponse> Send(Azure::Core::Http::Request &,
		                                                     Azure::Core::Context const &) override {
			throw Azure::Core::Http::TransportException(
			    "no HTTP transport injected for this Azure client (duckdb_azure bug)");
		}
	};
	return std::make_shared<NoTransport>();
}
