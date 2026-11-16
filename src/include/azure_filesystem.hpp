#pragma once

#include "azure_parsed_url.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/logging/file_system_logger.hpp"
#include "duckdb/main/client_context_state.hpp"

#include <azure/core/datetime.hpp>
#include <azure/storage/common/storage_exception.hpp>
#include <cstdint>
#include <ctime>

namespace duckdb {

struct AzureOptions {
	// 8 MiB - Base block size, copies rclone's default for concurrent transfers
	// 4000 MiB - Azure doc'd max per Append/StageBlock
	static const idx_t WRITE_BLOCK_SIZE_DEFAULT = (idx_t)8 * 1024 * 1024;
	static const idx_t WRITE_BLOCK_SIZE_MAX = (idx_t)4000 * 1024 * 1024;

	idx_t read_buffer_size = (idx_t)8 * 1024 * 1024;
	idx_t write_block_size = WRITE_BLOCK_SIZE_DEFAULT;
	idx_t write_staged_blocks_per_commit = 0;
	idx_t write_blocks_in_flight = 20;
};

class AzureContextState : public ClientContextState {
public:
	const AzureOptions options;

public:
	virtual bool IsValid() const;
	void QueryEnd() override;

	template <class TARGET>
	TARGET &As() {
		D_ASSERT(dynamic_cast<TARGET *>(this));
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &As() const {
		D_ASSERT(dynamic_cast<const TARGET *>(this));
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	explicit AzureContextState(const AzureOptions &options);

protected:
	bool is_valid;
};

//! The client a handle's requests go through. A request refused with 403 is sent once more through a client made
//! from the secret as it is now, which then replaces the handle's: credentials that expired may have been replaced
//! since the handle opened
template <class CLIENT>
class AzureHandleClient {
public:
	explicit AzureHandleClient(CLIENT client_p) : client(make_shared_ptr<const CLIENT>(std::move(client_p))) {
	}

	shared_ptr<const CLIENT> Get() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return client;
	}

	//! Sends [request] through the client, and through [current]() after a 403
	template <class REQUEST, class CURRENT>
	auto Send(REQUEST &&request, CURRENT &&current) -> decltype(request(std::declval<const CLIENT &>())) {
		try {
			return request(*Get());
		} catch (const Azure::Storage::StorageException &e) {
			if (e.StatusCode != Azure::Core::Http::HttpStatusCode::Forbidden) {
				throw;
			}
		}
		auto fresh = make_shared_ptr<const CLIENT>(current());
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			client = fresh;
		}
		return request(*fresh);
	}

private:
	annotated_mutex lock;
	shared_ptr<const CLIENT> client DUCKDB_GUARDED_BY(lock);
};

class AzureStorageFileSystem;

class AzureFileHandle : public FileHandle {
public:
	virtual bool PostConstruct();
	virtual void Abort();

	bool IsRemoteLoaded() {
		return is_remote_loaded;
	}

	FileType GetType() {
		return file_type;
	}

protected:
	AzureFileHandle(AzureStorageFileSystem &fs, const OpenFileInfo &info, FileOpenFlags flags, FileType file_type,
	                const AzureOptions &options);

public:
	FileOpenFlags flags;
	bool aborted = false;

	// File info
	bool is_remote_loaded;
	FileType file_type;
	idx_t length;
	timestamp_t last_modified;
	string etag;

	// Read buffer
	duckdb::unique_ptr<data_t[]> read_buffer;
	// Read info
	idx_t buffer_available;
	idx_t buffer_idx;
	idx_t file_offset;
	idx_t buffer_start;
	idx_t buffer_end;

	const AzureOptions options;
};

class AzureStorageFileSystem : public FileSystem {
public:
	// FS methods
	duckdb::unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener = nullptr) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void ReadScattered(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location) override;
	void ReadStreamed(FileHandle &handle, std::span<const FileReadTarget> targets, idx_t location,
	                  FileReadProgress &progress) override;
	bool CanSeek() override {
		return true;
	}
	bool OnDiskFile(FileHandle &handle) override {
		return false;
	}
	bool IsPipe(const string &filename, optional_ptr<FileOpener> opener = nullptr) override {
		return false;
	}
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	string GetVersionTag(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	void AbortFileWrite(FileHandle &handle) override;

	bool LoadFileInfo(AzureFileHandle &handle);

	string PathSeparator(const string &path) override {
		return "/";
	}

protected:
	unique_ptr<FileHandle> OpenFileExtended(const OpenFileInfo &info, FileOpenFlags flags,
	                                        optional_ptr<FileOpener> opener) override;

	bool SupportsOpenFileExtended() const override {
		return true;
	}

	virtual duckdb::unique_ptr<AzureFileHandle> CreateHandle(const OpenFileInfo &info, FileOpenFlags flags,
	                                                         optional_ptr<FileOpener> opener) = 0;
	//! GETs exactly the bytes [targets] take at [file_offset], into them in order; a target without data skips its
	//! bytes. [progress], when given, is told of the targets filled as the bytes arrive
	virtual void ReadRange(AzureFileHandle &handle, idx_t file_offset, std::span<const FileReadTarget> targets,
	                       optional_ptr<FileReadProgress> progress) = 0;

	virtual const string &GetContextPrefix() const = 0;
	shared_ptr<AzureContextState> GetOrCreateStorageContext(optional_ptr<FileOpener> opener, const string &path,
	                                                        const AzureParsedUrl &parsed_url);
	virtual shared_ptr<AzureContextState> CreateStorageContext(optional_ptr<FileOpener> opener, const string &path,
	                                                           const AzureParsedUrl &parsed_url) = 0;

	virtual void LoadRemoteFileInfo(AzureFileHandle &handle) = 0;
	static AzureOptions ParseAzureOptions(optional_ptr<FileOpener> opener);

public:
	static timestamp_t ToTimestamp(const Azure::DateTime &dt);
	static string StripETagQuotes(string etag);
};

} // namespace duckdb
