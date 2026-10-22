#pragma once

#include "azure_filesystem.hpp"
#include "azure_parsed_url.hpp"
#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

#include <azure/storage/blobs/blob_client.hpp>
#include <azure/storage/blobs/blob_service_client.hpp>
#include <azure/storage/blobs/block_blob_client.hpp>
#include <string>

namespace duckdb {

class AzureBlobContextState : public AzureContextState {
public:
	AzureBlobContextState(Azure::Storage::Blobs::BlobServiceClient client, const AzureOptions &options);
	Azure::Storage::Blobs::BlobContainerClient GetBlobContainerClient(const std::string &blobContainerName) const;
	~AzureBlobContextState() override = default;

private:
	Azure::Storage::Blobs::BlobServiceClient service_client;
};

class AzureBlobStorageFileSystem;

class AzureBlobStorageFileHandle : public AzureFileHandle {
public:
	AzureBlobStorageFileHandle(AzureBlobStorageFileSystem &fs, const OpenFileInfo &info, FileOpenFlags flags,
	                           const AzureOptions &options, Azure::Storage::Blobs::BlockBlobClient blob_client,
	                           optional_ptr<DatabaseInstance> db);
	~AzureBlobStorageFileHandle() override;

	void StageWriteBuffer();
	void Sync();
	void Close() override;
	void StageBlock(const string &block_id, const_data_ptr_t data, idx_t size) noexcept;
	void FinishStaging(string error) noexcept;

public:
	Azure::Storage::Blobs::BlockBlobClient blob_client;
	size_t committed_block_count = 0; // maintain position while open; only used if Sync() called before Close()
	uint32_t staged_block_count = 0;
	duckdb::unique_ptr<data_t[]> write_buffer;
	idx_t write_buffer_offset = 0;

private:
	void AwaitStaging(idx_t limit);
	void AwaitStagingNoThrow() noexcept;

	optional_ptr<DatabaseInstance> db;
	annotated_mutex staging_lock;
	idx_t blocks_in_flight DUCKDB_GUARDED_BY(staging_lock) = 0;
	string staging_error DUCKDB_GUARDED_BY(staging_lock);
};

class AzureBlobStorageFileSystem : public AzureStorageFileSystem {
public:
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;
	bool ListFilesExtended(const string &directory, const std::function<void(OpenFileInfo &info)> &callback,
	                       optional_ptr<FileOpener> opener) override;

	bool SupportsListFilesExtended() const override {
		return true;
	}

	// FS methods
	bool CanHandleFile(const string &fpath) override;
	void CreateDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override {
	}
	FileExistence TryDirectoryExists(const string &dirname, optional_ptr<FileOpener> opener,
	                                 optional_ptr<ErrorData> error) override;
	FileExistence TryFileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr,
	                            optional_ptr<ErrorData> error = nullptr) override;
	string GetName() const override {
		return "AzureBlobStorageFileSystem";
	}

	// From AzureFilesystem
	void LoadRemoteFileInfo(AzureFileHandle &handle) override;
	int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	void FileSync(FileHandle &handle) override;

	void RemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	virtual bool TryRemoveFile(const string &filename, optional_ptr<FileOpener> opener) override;
	void RemoveFiles(const vector<string> &filenames, optional_ptr<FileOpener> opener = nullptr) override;
	void RemoveDirectory(const string &directory, optional_ptr<FileOpener> opener = nullptr) override;
	void MoveFile(const string &source, const string &target, optional_ptr<FileOpener> opener = nullptr) override;

	ProducerToken &UploadProducer(DatabaseInstance &db);

public:
	static const string SCHEME;
	static const string SHORT_SCHEME;

	static const string PATH_PREFIX;
	static const string SHORT_PATH_PREFIX;

protected:
	// From AzureFilesystem
	const string &GetContextPrefix() const override {
		return PATH_PREFIX;
	}
	shared_ptr<AzureContextState> CreateStorageContext(optional_ptr<FileOpener> opener, const string &path,
	                                                   const AzureParsedUrl &parsed_url) override;
	unique_ptr<AzureFileHandle> CreateHandle(const OpenFileInfo &info, FileOpenFlags flags,
	                                         optional_ptr<FileOpener> opener) override;

	void ReadRange(AzureFileHandle &handle, idx_t file_offset, char *buffer_out, idx_t buffer_out_len) override;

private:
	annotated_mutex upload_producer_lock;
	unique_ptr<ProducerToken> upload_producer DUCKDB_GUARDED_BY(upload_producer_lock);
};

} // namespace duckdb
