/*
 *RocksDBCheckpointUtils.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbserver/RocksDBCheckpointUtils.actor.h"

#ifdef SSD_ROCKSDB_EXPERIMENTAL
#include <rocksdb/db.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/types.h>
#include <rocksdb/version.h>
#endif // SSD_ROCKSDB_EXPERIMENTAL

#include "fdbclient/FDBTypes.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbclient/StorageCheckpoint.h"
#include "fdbserver/CoroFlow.h"
#include "fdbserver/Knobs.h"
#include "flow/IThreadPool.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/Trace.h"
#include "flow/flow.h"

#include "flow/actorcompiler.h" // has to be last include

FDB_DEFINE_BOOLEAN_PARAM(CheckpointAsKeyValues);

#ifdef SSD_ROCKSDB_EXPERIMENTAL
// Enforcing rocksdb version to be 7.7.3.
static_assert((ROCKSDB_MAJOR == 7 && ROCKSDB_MINOR == 7 && ROCKSDB_PATCH == 3),
              "Unsupported rocksdb version. Update the rocksdb to 7.7.3 version");

namespace {

using DB = rocksdb::DB*;
using CF = rocksdb::ColumnFamilyHandle*;

const KeyRef persistVersion = "\xff\xffVersion"_sr;
const KeyRef readerInitialized = "\xff\xff/ReaderInitialized"_sr;
const std::string checkpointCf = "RocksDBCheckpoint";
const std::string checkpointReaderSubDir = "/reader";
const std::string rocksDefaultCf = "default";

rocksdb::ExportImportFilesMetaData getMetaData(const CheckpointMetaData& checkpoint) {
	rocksdb::ExportImportFilesMetaData metaData;
	if (checkpoint.getFormat() != DataMoveRocksCF) {
		return metaData;
	}

	RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(checkpoint);
	metaData.db_comparator_name = rocksCF.dbComparatorName;

	for (const LiveFileMetaData& fileMetaData : rocksCF.sstFiles) {
		rocksdb::LiveFileMetaData liveFileMetaData;
		liveFileMetaData.size = fileMetaData.size;
		liveFileMetaData.name = fileMetaData.name;
		liveFileMetaData.file_number = fileMetaData.file_number;
		liveFileMetaData.db_path = fileMetaData.db_path;
		liveFileMetaData.smallest_seqno = fileMetaData.smallest_seqno;
		liveFileMetaData.largest_seqno = fileMetaData.largest_seqno;
		liveFileMetaData.smallestkey = fileMetaData.smallestkey;
		liveFileMetaData.largestkey = fileMetaData.largestkey;
		liveFileMetaData.num_reads_sampled = fileMetaData.num_reads_sampled;
		liveFileMetaData.being_compacted = fileMetaData.being_compacted;
		liveFileMetaData.num_entries = fileMetaData.num_entries;
		liveFileMetaData.num_deletions = fileMetaData.num_deletions;
		liveFileMetaData.temperature = static_cast<rocksdb::Temperature>(fileMetaData.temperature);
		liveFileMetaData.oldest_blob_file_number = fileMetaData.oldest_blob_file_number;
		liveFileMetaData.oldest_ancester_time = fileMetaData.oldest_ancester_time;
		liveFileMetaData.file_creation_time = fileMetaData.file_creation_time;
		liveFileMetaData.file_checksum = fileMetaData.file_checksum;
		liveFileMetaData.file_checksum_func_name = fileMetaData.file_checksum_func_name;
		liveFileMetaData.column_family_name = fileMetaData.column_family_name;
		liveFileMetaData.level = fileMetaData.level;
		metaData.files.push_back(liveFileMetaData);
	}

	return metaData;
}

rocksdb::Slice toSlice(StringRef s) {
	return rocksdb::Slice(reinterpret_cast<const char*>(s.begin()), s.size());
}

StringRef toStringRef(rocksdb::Slice s) {
	return StringRef(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

rocksdb::ColumnFamilyOptions getCFOptions() {
	rocksdb::ColumnFamilyOptions options;
	return options;
}

rocksdb::Options getOptions() {
	rocksdb::Options options({}, getCFOptions());
	options.create_if_missing = true;
	options.db_log_dir = g_network->isSimulated() ? "" : SERVER_KNOBS->LOG_DIRECTORY;
	return options;
}

// Set some useful defaults desired for all reads.
rocksdb::ReadOptions getReadOptions() {
	rocksdb::ReadOptions options;
	options.background_purge_on_iterator_cleanup = true;
	return options;
}

void logRocksDBError(const rocksdb::Status& status, const std::string& method) {
	auto level = status.IsTimedOut() ? SevWarn : SevError;
	TraceEvent e(level, "RocksDBCheckpointReaderError");
	e.detail("Error", status.ToString()).detail("Method", method).detail("RocksDBSeverity", status.severity());
	if (status.IsIOError()) {
		e.detail("SubCode", status.subcode());
	}
}

Error statusToError(const rocksdb::Status& s) {
	if (s.IsIOError()) {
		return io_error();
	} else if (s.IsTimedOut()) {
		return transaction_too_old();
	} else {
		return unknown_error();
	}
}

// RocksDBCheckpointReader reads a RocksDB checkpoint, and returns the key-value pairs via nextKeyValues.
class RocksDBCheckpointReader : public ICheckpointReader {
public:
	class RocksDBCheckpointIterator : public ICheckpointIterator {
	public:
		RocksDBCheckpointIterator(RocksDBCheckpointReader* reader, const KeyRange& range)
		  : reader(reader), range(range) {
			ASSERT(reader != nullptr);
			ASSERT(reader->db != nullptr);
			ASSERT(reader->cf != nullptr);
			this->beginSlice = toSlice(this->range.begin);
			this->endSlice = toSlice(this->range.end);
			rocksdb::ReadOptions options = getReadOptions();
			options.iterate_lower_bound = &beginSlice;
			options.iterate_upper_bound = &endSlice;
			options.fill_cache = false; // Optimized for bulk scan.
			options.readahead_size = SERVER_KNOBS->ROCKSDB_CHECKPOINT_READ_AHEAD_SIZE;
			const uint64_t deadlineMicros =
			    reader->db->GetEnv()->NowMicros() + SERVER_KNOBS->ROCKSDB_READ_CHECKPOINT_TIMEOUT * 1000000;
			options.deadline = std::chrono::microseconds(deadlineMicros);
			this->iterator = std::unique_ptr<rocksdb::Iterator>(reader->db->NewIterator(options, reader->cf));
			iterator->Seek(this->beginSlice);
		}

		~RocksDBCheckpointIterator() { this->reader->numIter--; }

		Future<RangeResult> nextBatch(const int rowLimit, const int ByteLimit) override;

		rocksdb::Iterator* getIterator() { return iterator.get(); }

		const rocksdb::Slice& end() const { return this->endSlice; }

	private:
		RocksDBCheckpointReader* const reader;
		const KeyRange range;
		rocksdb::Slice beginSlice;
		rocksdb::Slice endSlice;
		std::unique_ptr<rocksdb::Iterator> iterator;
	};

	RocksDBCheckpointReader(const CheckpointMetaData& checkpoint, UID logID);

	Future<Void> init(StringRef token) override;

	Future<RangeResult> nextKeyValues(const int rowLimit, const int byteLimit) override { throw not_implemented(); }

	Future<Standalone<StringRef>> nextChunk(const int byteLimit) override { throw not_implemented(); }

	Future<Void> close() override { return doClose(this); }

	std::unique_ptr<ICheckpointIterator> getIterator(KeyRange range) override;

	bool inUse() const override { return this->numIter > 0; }

private:
	struct Reader : IThreadPoolReceiver {
		struct OpenAction : TypedAction<Reader, OpenAction> {
			OpenAction(CheckpointMetaData checkpoint) : checkpoint(std::move(checkpoint)) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			const CheckpointMetaData checkpoint;
			ThreadReturnPromise<Void> done;
		};

		struct CloseAction : TypedAction<Reader, CloseAction> {
			CloseAction(std::string path, bool deleteOnClose) : path(path), deleteOnClose(deleteOnClose) {}
			double getTimeEstimate() const override { return SERVER_KNOBS->COMMIT_TIME_ESTIMATE; }

			std::string path;
			bool deleteOnClose;
			ThreadReturnPromise<Void> done;
		};

		struct ReadRangeAction : TypedAction<Reader, ReadRangeAction>, FastAllocated<ReadRangeAction> {
			ReadRangeAction(int rowLimit, int byteLimit, RocksDBCheckpointIterator* iterator)
			  : rowLimit(rowLimit), byteLimit(byteLimit), iterator(iterator), startTime(timer_monotonic()) {}

			double getTimeEstimate() const override { return SERVER_KNOBS->READ_RANGE_TIME_ESTIMATE; }

			const int rowLimit, byteLimit;
			RocksDBCheckpointIterator* const iterator;
			const double startTime;
			ThreadReturnPromise<RangeResult> result;
		};

		explicit Reader(DB& db, CF& cf);
		~Reader() override {}

		void init() override {}

		void action(OpenAction& a);

		void action(CloseAction& a);

		void action(ReadRangeAction& a);

		rocksdb::Status tryOpenForRead(const std::string& path);

		rocksdb::Status importCheckpoint(const std::string& path, const CheckpointMetaData& checkpoint);

		rocksdb::Status closeInternal(const std::string& path, const bool deleteOnClose);

		DB& db;
		CF& cf;
		std::vector<rocksdb::ColumnFamilyHandle*> handles;
		double readRangeTimeout;
	};

	Future<RangeResult> nextBatch(const int rowLimit, const int byteLimit, RocksDBCheckpointIterator* iterator);

	ACTOR static Future<Void> doClose(RocksDBCheckpointReader* self);

	DB db = nullptr;
	CF cf = nullptr;
	std::string path;
	const UID id;
	Version version;
	CheckpointMetaData checkpoint;
	Reference<IThreadPool> threads;
	Future<Void> openFuture;
	int numIter;
};

Future<RangeResult> RocksDBCheckpointReader::RocksDBCheckpointIterator::nextBatch(const int rowLimit,
                                                                                  const int ByteLimit) {
	return this->reader->nextBatch(rowLimit, ByteLimit, this);
}

RocksDBCheckpointReader::RocksDBCheckpointReader(const CheckpointMetaData& checkpoint, UID logID)
  : id(logID), checkpoint(checkpoint), numIter(0) {
	if (g_network->isSimulated()) {
		threads = CoroThreadPool::createThreadPool();
	} else {
		threads = createGenericThreadPool();
	}
	for (int i = 0; i < SERVER_KNOBS->ROCKSDB_CHECKPOINT_READER_PARALLELISM; ++i) {
		threads->addThread(new Reader(db, cf), "fdb-rocks-cr");
	}
}

Future<Void> RocksDBCheckpointReader::init(StringRef token) {
	if (openFuture.isValid()) {
		return openFuture;
	}

	auto a = std::make_unique<Reader::OpenAction>(this->checkpoint);
	openFuture = a->done.getFuture();
	threads->post(a.release());
	return openFuture;
}

Future<RangeResult> RocksDBCheckpointReader::nextBatch(const int rowLimit,
                                                       const int byteLimit,
                                                       RocksDBCheckpointIterator* iterator) {
	auto a = std::make_unique<Reader::ReadRangeAction>(rowLimit, byteLimit, iterator);
	auto res = a->result.getFuture();
	threads->post(a.release());
	return res;
}

std::unique_ptr<ICheckpointIterator> RocksDBCheckpointReader::getIterator(KeyRange range) {
	++this->numIter;
	return std::unique_ptr<ICheckpointIterator>(new RocksDBCheckpointIterator(this, range));
}

RocksDBCheckpointReader::Reader::Reader(DB& db, CF& cf) : db(db), cf(cf) {}

void RocksDBCheckpointReader::Reader::action(RocksDBCheckpointReader::Reader::OpenAction& a) {
	TraceEvent(SevDebug, "RocksDBCheckpointReaderInitBegin").detail("Checkpoint", a.checkpoint.toString());
	ASSERT(cf == nullptr);

	const CheckpointMetaData& checkpoint = a.checkpoint;
	const CheckpointFormat format = checkpoint.getFormat();
	if (format != DataMoveRocksCF) {
		TraceEvent(SevDebug, "RocksDBCheckpointReaderError").detail("InvalidFormat", checkpoint.toString());
		a.done.sendError(not_implemented());
		return;
	}

	RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(checkpoint);
	ASSERT(!rocksCF.sstFiles.empty());
	const std::string path = rocksCF.sstFiles.front().db_path + checkpointReaderSubDir;

	rocksdb::Status status = tryOpenForRead(path);
	if (!status.ok()) {
		platform::eraseDirectoryRecursive(path);
		status = importCheckpoint(path, checkpoint);
		if (status.ok()) {
			status = tryOpenForRead(path);
		}
	}

	if (!status.ok()) {
		a.done.sendError(statusToError(status));
		return;
	}

	a.done.send(Void());
	TraceEvent(SevDebug, "RocksDBCheckpointReaderInitEnd").detail("Path", path).detail("ColumnFamily", cf->GetName());
}

void RocksDBCheckpointReader::Reader::action(RocksDBCheckpointReader::Reader::CloseAction& a) {
	closeInternal(a.path, a.deleteOnClose);
	a.done.send(Void());
}

void RocksDBCheckpointReader::Reader::action(RocksDBCheckpointReader::Reader::ReadRangeAction& a) {
	TraceEvent(SevDebug, "RocksDBCheckpointReaderReadRangeBegin");
	ASSERT(a.iterator != nullptr);

	RangeResult result;
	if (a.rowLimit == 0 || a.byteLimit == 0) {
		a.result.send(result);
		return;
	}

	// For now, only forward scan is supported.
	ASSERT(a.rowLimit > 0);

	rocksdb::Iterator* iter = a.iterator->getIterator();
	int accumulatedBytes = 0;
	rocksdb::Status s;
	while (iter->Valid() && iter->key().compare(a.iterator->end()) < 0) {
		KeyValueRef kv(toStringRef(iter->key()), toStringRef(iter->value()));
		accumulatedBytes += sizeof(KeyValueRef) + kv.expectedSize();
		result.push_back_deep(result.arena(), kv);
		iter->Next();
		if (result.size() >= a.rowLimit || accumulatedBytes >= a.byteLimit) {
			break;
		}
	}

	s = iter->status();

	if (!s.ok()) {
		logRocksDBError(s, "ReadRange");
		a.result.sendError(statusToError(s));
		return;
	}

	if (result.empty()) {
		a.result.sendError(end_of_stream());
	} else {
		a.result.send(result);
	}
}

rocksdb::Status RocksDBCheckpointReader::Reader::tryOpenForRead(const std::string& path) {
	std::vector<std::string> columnFamilies;
	const rocksdb::Options options = getOptions();
	rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, path, &columnFamilies);
	if (std::find(columnFamilies.begin(), columnFamilies.end(), rocksDefaultCf) == columnFamilies.end() ||
	    std::find(columnFamilies.begin(), columnFamilies.end(), checkpointCf) == columnFamilies.end()) {
		return rocksdb::Status::Aborted();
	}

	const rocksdb::ColumnFamilyOptions cfOptions = getCFOptions();
	std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
	for (const std::string& name : columnFamilies) {
		descriptors.emplace_back(name, cfOptions);
	}
	status = rocksdb::DB::OpenForReadOnly(options, path, descriptors, &handles, &db);
	if (!status.ok()) {
		logRocksDBError(status, "OpenForReadOnly");
		return status;
	}

	rocksdb::PinnableSlice value;
	rocksdb::ReadOptions readOptions = getReadOptions();
	status = db->Get(readOptions, db->DefaultColumnFamily(), toSlice(readerInitialized), &value);
	if (!status.ok() && !status.IsNotFound()) {
		logRocksDBError(status, "CheckpointCheckInitState");
		return status;
	}

	if (status.IsNotFound()) {
		status = closeInternal(path, /*deleteOnClose=*/true);
		if (!status.ok()) {
			return status;
		} else {
			delete db;
			TraceEvent(SevDebug, "RocksDBCheckpointReaderTryOpenError").detail("Path", path);
			return rocksdb::Status::Aborted();
		}
	}

	ASSERT(handles.size() == 2);
	for (rocksdb::ColumnFamilyHandle* handle : handles) {
		if (handle->GetName() == checkpointCf) {
			TraceEvent(SevDebug, "RocksDBCheckpointCF").detail("Path", path).detail("ColumnFamily", handle->GetName());
			cf = handle;
			break;
		}
	}

	ASSERT(db != nullptr && cf != nullptr);
	return rocksdb::Status::OK();
}

rocksdb::Status RocksDBCheckpointReader::Reader::importCheckpoint(const std::string& path,
                                                                  const CheckpointMetaData& checkpoint) {
	std::vector<std::string> columnFamilies;
	const rocksdb::Options options = getOptions();
	rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, path, &columnFamilies);
	if (std::find(columnFamilies.begin(), columnFamilies.end(), rocksDefaultCf) == columnFamilies.end()) {
		columnFamilies.push_back(rocksDefaultCf);
	}

	const rocksdb::ColumnFamilyOptions cfOptions = getCFOptions();
	std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
	for (const std::string& name : columnFamilies) {
		descriptors.emplace_back(name, cfOptions);
	}

	status = rocksdb::DB::Open(options, path, descriptors, &handles, &db);
	if (!status.ok()) {
		logRocksDBError(status, "CheckpointReaderOpen");
		return status;
	}

	rocksdb::ExportImportFilesMetaData metaData = getMetaData(checkpoint);
	rocksdb::ImportColumnFamilyOptions importOptions;
	importOptions.move_files = false;
	status = db->CreateColumnFamilyWithImport(cfOptions, checkpointCf, importOptions, metaData, &cf);
	if (!status.ok()) {
		logRocksDBError(status, "CheckpointReaderImportCheckpoint");
		return status;
	}
	handles.push_back(cf);
	TraceEvent(SevDebug, "RocksDBCheckpointReaderImportedCF");

	rocksdb::WriteOptions writeOptions;
	writeOptions.sync = !SERVER_KNOBS->ROCKSDB_UNSAFE_AUTO_FSYNC;
	status = db->Put(writeOptions, toSlice(readerInitialized), toSlice("1"_sr));
	if (!status.ok()) {
		logRocksDBError(status, "CheckpointReaderPersistInitKey");
		return status;
	}
	ASSERT(db != nullptr && cf != nullptr);

	return closeInternal(path, /*deleteOnClose=*/false);
}

rocksdb::Status RocksDBCheckpointReader::Reader::closeInternal(const std::string& path, const bool deleteOnClose) {
	if (db == nullptr) {
		return rocksdb::Status::OK();
	}

	for (rocksdb::ColumnFamilyHandle* handle : handles) {
		if (handle != nullptr) {
			TraceEvent("RocksDBCheckpointReaderDestroyCF").detail("Path", path).detail("CF", handle->GetName());
			db->DestroyColumnFamilyHandle(handle);
		}
	}
	handles.clear();

	rocksdb::Status s = db->Close();
	if (!s.ok()) {
		logRocksDBError(s, "Close");
	}

	if (deleteOnClose) {
		rocksdb::ColumnFamilyOptions options;
		std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
		descriptors.emplace_back(rocksDefaultCf, options);
		descriptors.emplace_back(checkpointCf, options);
		s = rocksdb::DestroyDB(path, getOptions(), descriptors);
		if (!s.ok()) {
			logRocksDBError(s, "Destroy");
		} else {
			TraceEvent(SevDebug, "RocksDBCheckpointReader").detail("Path", path).detail("Method", "Destroy");
		}
	}

	TraceEvent(SevDebug, "RocksDBCheckpointReader").detail("Path", path).detail("Method", "Close");
	return s;
}

ACTOR Future<Void> RocksDBCheckpointReader::doClose(RocksDBCheckpointReader* self) {
	if (self == nullptr)
		return Void();

	auto a = new RocksDBCheckpointReader::Reader::CloseAction(self->path, false);
	auto f = a->done.getFuture();
	self->threads->post(a);
	wait(f);

	if (self != nullptr) {
		wait(self->threads->stop());
	}

	if (self != nullptr) {
		if (self->db != nullptr) {
			delete self->db;
		}
		delete self;
	}

	return Void();
}

class RocksDBSstFileWriter : public IRocksDBSstFileWriter {
public:
	RocksDBSstFileWriter()
	  : writer(std::make_unique<rocksdb::SstFileWriter>(rocksdb::EnvOptions(), rocksdb::Options())), hasData(false){};

	void open(const std::string localFile) override;

	void write(const KeyRef key, const ValueRef value) override;

	bool finish() override;

private:
	std::unique_ptr<rocksdb::SstFileWriter> writer;
	std::string localFile;
	bool hasData;
};

void RocksDBSstFileWriter::open(const std::string localFile) {
	try {
		this->localFile = abspath(localFile);
		rocksdb::Status status = this->writer->Open(this->localFile);
		if (!status.ok()) {
			TraceEvent(SevError, "RocksDBSstFileWriterWrapperOpenFileError")
			    .detail("LocalFile", this->localFile)
			    .detail("Status", status.ToString());
			throw failed_to_create_checkpoint_shard_metadata();
		}
	} catch (Error& e) {
		throw e;
	}
}

void RocksDBSstFileWriter::write(const KeyRef key, const ValueRef value) {
	try {
		rocksdb::Status status = this->writer->Put(toSlice(key), toSlice(value));
		if (!status.ok()) {
			TraceEvent(SevError, "RocksDBSstFileWriterWrapperWriteError")
			    .detail("LocalFile", this->localFile)
			    .detail("Key", key)
			    .detail("Value", value)
			    .detail("Status", status.ToString());
			throw failed_to_create_checkpoint_shard_metadata();
		}
		this->hasData = true;
	} catch (Error& e) {
		throw e;
	}
}

bool RocksDBSstFileWriter::finish() {
	try {
		if (!this->hasData) {
			// writer->finish() cannot create sst file with no entries
			// So, we have to check whether any data set to be written to sst file before writer->finish()
			return false;
		}
		rocksdb::Status status = this->writer->Finish();
		if (!status.ok()) {
			TraceEvent(SevError, "RocksDBSstFileWriterWrapperCloseError")
			    .detail("LocalFile", this->localFile)
			    .detail("Status", status.ToString());
			throw failed_to_create_checkpoint_shard_metadata();
		}
		return true;
	} catch (Error& e) {
		throw e;
	}
}

// RocksDBCFCheckpointReader reads an exported RocksDB Column Family checkpoint, and returns the serialized
// checkpoint via nextChunk.
class RocksDBCFCheckpointReader : public ICheckpointReader {
public:
	RocksDBCFCheckpointReader(const CheckpointMetaData& checkpoint, UID logID)
	  : checkpoint_(checkpoint), id_(logID), file_(Reference<IAsyncFile>()), offset_(0) {}

	Future<Void> init(StringRef token) override;

	Future<RangeResult> nextKeyValues(const int rowLimit, const int byteLimit) override { throw not_implemented(); }

	Future<Standalone<StringRef>> nextChunk(const int byteLimit) override;

	Future<Void> close() override;

private:
	ACTOR static Future<Void> doInit(RocksDBCFCheckpointReader* self) {
		ASSERT(self != nullptr);
		try {
			state Reference<IAsyncFile> _file = wait(IAsyncFileSystem::filesystem()->open(
			    self->path_, IAsyncFile::OPEN_READONLY | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_NO_AIO, 0));
			self->file_ = _file;
			TraceEvent("RocksDBCheckpointReaderOpenFile").detail("File", self->path_);
		} catch (Error& e) {
			TraceEvent(SevWarnAlways, "ServerGetCheckpointFileFailure")
			    .errorUnsuppressed(e)
			    .detail("File", self->path_);
			throw e;
		}

		return Void();
	}

	ACTOR static Future<Standalone<StringRef>> getNextChunk(RocksDBCFCheckpointReader* self, int byteLimit) {
		int blockSize = std::min(64 * 1024, byteLimit); // Block size read from disk.
		state Standalone<StringRef> buf = makeAlignedString(_PAGE_SIZE, blockSize);
		int bytesRead = wait(self->file_->read(mutateString(buf), blockSize, self->offset_));
		if (bytesRead == 0) {
			throw end_of_stream();
		}

		self->offset_ += bytesRead;
		return buf.substr(0, bytesRead);
	}

	ACTOR static Future<Void> doClose(RocksDBCFCheckpointReader* self) {
		wait(delay(0, TaskPriority::FetchKeys));
		delete self;
		return Void();
	}

	CheckpointMetaData checkpoint_;
	UID id_;
	Reference<IAsyncFile> file_;
	int offset_;
	std::string path_;
};

Future<Void> RocksDBCFCheckpointReader::init(StringRef token) {
	ASSERT_EQ(this->checkpoint_.getFormat(), DataMoveRocksCF);
	const std::string name = token.toString();
	this->offset_ = 0;
	this->path_.clear();
	const RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(this->checkpoint_);
	for (const auto& sstFile : rocksCF.sstFiles) {
		if (sstFile.name == name) {
			this->path_ = sstFile.db_path + sstFile.name;
			break;
		}
	}

	if (this->path_.empty()) {
		TraceEvent("RocksDBCheckpointReaderInitFileNotFound").detail("File", this->path_);
		return checkpoint_not_found();
	}

	return doInit(this);
}

Future<Standalone<StringRef>> RocksDBCFCheckpointReader::nextChunk(const int byteLimit) {
	return getNextChunk(this, byteLimit);
}

Future<Void> RocksDBCFCheckpointReader::close() {
	return doClose(this);
}

// Fetch a single sst file from storage server. The progress is checkpointed via cFun.
ACTOR Future<Void> fetchCheckpointFile(Database cx,
                                       std::shared_ptr<CheckpointMetaData> metaData,
                                       int idx,
                                       std::string dir,
                                       std::function<Future<Void>(const CheckpointMetaData&)> cFun,
                                       int maxRetries = 3) {
	state RocksDBColumnFamilyCheckpoint rocksCF;
	ObjectReader reader(metaData->serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCF);

	// Skip fetched file.
	if (rocksCF.sstFiles[idx].fetched && rocksCF.sstFiles[idx].db_path == dir) {
		return Void();
	}

	state std::string remoteFile = rocksCF.sstFiles[idx].name;
	state std::string localFile = dir + rocksCF.sstFiles[idx].name;
	ASSERT(!metaData->src.empty());
	state UID ssID = metaData->src.front();

	state Transaction tr(cx);
	state StorageServerInterface ssi;
	loop {
		try {
			tr.setOption(FDBTransactionOptions::READ_SYSTEM_KEYS);
			Optional<Value> ss = wait(tr.get(serverListKeyFor(ssID)));
			if (!ss.present()) {
				throw checkpoint_not_found();
			}
			ssi = decodeServerListValue(ss.get());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}

	state int attempt = 0;
	state int64_t offset = 0;
	state Reference<IAsyncFile> asyncFile;
	loop {
		offset = 0;
		try {
			asyncFile = Reference<IAsyncFile>();
			++attempt;
			TraceEvent("FetchCheckpointFileBegin")
			    .detail("RemoteFile", remoteFile)
			    .detail("TargetUID", ssID.toString())
			    .detail("StorageServer", ssi.id().toString())
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt);

			wait(IAsyncFileSystem::filesystem()->deleteFile(localFile, true));
			const int64_t flags = IAsyncFile::OPEN_ATOMIC_WRITE_AND_CREATE | IAsyncFile::OPEN_READWRITE |
			                      IAsyncFile::OPEN_CREATE | IAsyncFile::OPEN_UNCACHED | IAsyncFile::OPEN_NO_AIO;
			wait(store(asyncFile, IAsyncFileSystem::filesystem()->open(localFile, flags, 0666)));

			state ReplyPromiseStream<FetchCheckpointReply> stream =
			    ssi.fetchCheckpoint.getReplyStream(FetchCheckpointRequest(metaData->checkpointID, remoteFile));
			TraceEvent("FetchCheckpointFileReceivingData")
			    .detail("RemoteFile", remoteFile)
			    .detail("TargetUID", ssID.toString())
			    .detail("StorageServer", ssi.id().toString())
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt);
			loop {
				state FetchCheckpointReply rep = waitNext(stream.getFuture());
				wait(asyncFile->write(rep.data.begin(), rep.data.size(), offset));
				wait(asyncFile->flush());
				offset += rep.data.size();
			}
		} catch (Error& e) {
			if (e.code() != error_code_end_of_stream ||
			    (g_network->isSimulated() && attempt == 1 && deterministicRandom()->coinflip())) {
				TraceEvent("FetchCheckpointFileError")
				    .errorUnsuppressed(e)
				    .detail("RemoteFile", remoteFile)
				    .detail("StorageServer", ssi.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt);
				if (attempt >= maxRetries) {
					throw e;
				}
			} else {
				wait(asyncFile->sync());
				int64_t fileSize = wait(asyncFile->size());
				TraceEvent("FetchCheckpointFileEnd")
				    .detail("RemoteFile", remoteFile)
				    .detail("StorageServer", ssi.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt)
				    .detail("DataSize", offset)
				    .detail("FileSize", fileSize);
				rocksCF.sstFiles[idx].db_path = dir;
				rocksCF.sstFiles[idx].fetched = true;
				metaData->serializedCheckpoint = ObjectWriter::toValue(rocksCF, IncludeVersion());
				if (cFun) {
					wait(cFun(*metaData));
				}
				return Void();
			}
		}
	}
}

// TODO: Return when a file exceeds a limit.
ACTOR Future<Void> fetchCheckpointRange(Database cx,
                                        std::shared_ptr<CheckpointMetaData> metaData,
                                        KeyRange range,
                                        std::string dir,
                                        std::shared_ptr<rocksdb::SstFileWriter> writer,
                                        std::function<Future<Void>(const CheckpointMetaData&)> cFun,
                                        int maxRetries = 3) {
	state std::string localFile =
	    dir + "/" + UID(metaData->checkpointID.first(), deterministicRandom()->randomUInt64()).toString() + ".sst";
	RocksDBCheckpointKeyValues rkv = getRocksKeyValuesCheckpoint(*metaData);
	TraceEvent("FetchCheckpointRange")
	    .detail("InitialState", metaData->toString())
	    .detail("RocksCheckpointKeyValues", rkv.toString())
	    .detail("FilePath", localFile);

	for (const auto& file : rkv.fetchedFiles) {
		ASSERT(!file.range.intersects(range));
	}

	ASSERT(!metaData->src.empty());
	state UID ssID = metaData->src.front();
	state Transaction tr(cx);
	state StorageServerInterface ssi;
	loop {
		tr.setOption(FDBTransactionOptions::LOCK_AWARE);
		tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
		try {
			Optional<Value> ss = wait(tr.get(serverListKeyFor(ssID)));
			if (!ss.present()) {
				TraceEvent(SevWarnAlways, "FetchCheckpointRangeStorageServerNotFound")
				    .detail("SSID", ssID)
				    .detail("InitialState", metaData->toString());
				throw checkpoint_not_found();
			}
			ssi = decodeServerListValue(ss.get());
			break;
		} catch (Error& e) {
			wait(tr.onError(e));
		}
	}

	ASSERT(ssi.id() == ssID);

	state int attempt = 0;
	state int64_t totalBytes = 0;
	state rocksdb::Status status;
	state Optional<Error> error;
	loop {
		totalBytes = 0;
		++attempt;
		try {
			TraceEvent(SevInfo, "FetchCheckpointRangeBegin")
			    .detail("CheckpointID", metaData->checkpointID)
			    .detail("Range", range.toString())
			    .detail("TargetStorageServerUID", ssID)
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt)
			    .log();

			wait(IAsyncFileSystem::filesystem()->deleteFile(localFile, true));
			status = writer->Open(localFile);
			if (!status.ok()) {
				Error e = statusToError(status);
				TraceEvent(SevError, "FetchCheckpointRangeOpenFileError")
				    .detail("LocalFile", localFile)
				    .detail("Status", status.ToString());
				throw e;
			}

			state ReplyPromiseStream<FetchCheckpointKeyValuesStreamReply> stream =
			    ssi.fetchCheckpointKeyValues.getReplyStream(
			        FetchCheckpointKeyValuesRequest(metaData->checkpointID, range));
			TraceEvent(SevDebug, "FetchCheckpointKeyValuesReceivingData")
			    .detail("CheckpointID", metaData->checkpointID)
			    .detail("Range", range.toString())
			    .detail("TargetStorageServerUID", ssID.toString())
			    .detail("LocalFile", localFile)
			    .detail("Attempt", attempt)
			    .log();

			loop {
				FetchCheckpointKeyValuesStreamReply rep = waitNext(stream.getFuture());
				for (int i = 0; i < rep.data.size(); ++i) {
					status = writer->Put(toSlice(rep.data[i].key), toSlice(rep.data[i].value));
					if (!status.ok()) {
						Error e = statusToError(status);
						TraceEvent(SevError, "FetchCheckpointRangeWriteError")
						    .detail("LocalFile", localFile)
						    .detail("Key", rep.data[i].key.toString())
						    .detail("Value", rep.data[i].value.toString())
						    .detail("Status", status.ToString());
						throw e;
					}
					totalBytes += rep.data[i].expectedSize();
				}
			}
		} catch (Error& e) {
			Error err = e;
			if (totalBytes > 0) {
				status = writer->Finish();
				if (!status.ok()) {
					err = statusToError(status);
				}
			}
			if (err.code() != error_code_end_of_stream) {
				TraceEvent(SevWarn, "FetchCheckpointFileError")
				    .errorUnsuppressed(err)
				    .detail("CheckpointID", metaData->checkpointID)
				    .detail("Range", range.toString())
				    .detail("TargetStorageServerUID", ssID.toString())
				    .detail("LocalFile", localFile)
				    .detail("Attempt", attempt);
				if (attempt >= maxRetries) {
					error = err;
					break;
				}
			} else {
				if (totalBytes > 0) {
					RocksDBCheckpointKeyValues rcp = getRocksKeyValuesCheckpoint(*metaData);
					rcp.fetchedFiles.emplace_back(localFile, range, totalBytes);
					metaData->serializedCheckpoint = ObjectWriter::toValue(rcp, IncludeVersion());
				}
				if (!fileExists(localFile)) {
					TraceEvent(SevWarn, "FetchCheckpointRangeEndFileNotFound")
					    .detail("CheckpointID", metaData->checkpointID)
					    .detail("Range", range.toString())
					    .detail("TargetStorageServerUID", ssID.toString())
					    .detail("LocalFile", localFile)
					    .detail("Attempt", attempt)
					    .detail("TotalBytes", totalBytes);
				} else {
					TraceEvent(SevInfo, "FetchCheckpointRangeEnd")
					    .detail("CheckpointID", metaData->checkpointID)
					    .detail("Range", range.toString())
					    .detail("TargetStorageServerUID", ssID.toString())
					    .detail("LocalFile", localFile)
					    .detail("Attempt", attempt)
					    .detail("TotalBytes", totalBytes);
					break;
				}
			}
		}
	}

	if (error.present()) {
		throw error.get();
	}

	return Void();
}

ACTOR Future<Void> fetchCheckpointRanges(Database cx,
                                         std::shared_ptr<CheckpointMetaData> metaData,
                                         std::string dir,
                                         std::function<Future<Void>(const CheckpointMetaData&)> cFun) {
	RocksDBCheckpointKeyValues rkv = getRocksKeyValuesCheckpoint(*metaData);
	TraceEvent("FetchCheckpointRanges")
	    .detail("InitialState", metaData->toString())
	    .detail("RocksCheckpointKeyValues", rkv.toString());

	KeyRangeMap<CheckpointFile> fileMap;
	for (const auto& file : rkv.fetchedFiles) {
		fileMap.insert(file.range, file);
	}

	std::vector<Future<Void>> fs;
	for (const auto& range : rkv.ranges) {
		auto ranges = fileMap.intersectingRanges(range);
		for (auto r = ranges.begin(); r != ranges.end(); ++r) {
			CheckpointFile& file = r->value();
			KeyRangeRef currentRange = range & r->range();
			if (!file.isValid()) {
				std::shared_ptr<rocksdb::SstFileWriter> writer =
				    std::make_shared<rocksdb::SstFileWriter>(rocksdb::EnvOptions(), rocksdb::Options());
				fs.push_back(fetchCheckpointRange(cx, metaData, currentRange, dir, writer, cFun));
			}
		}
	}
	wait(waitForAll(fs));
	if (cFun) {
		wait(cFun(*metaData));
	}

	return Void();
}
} // namespace

ACTOR Future<CheckpointMetaData> fetchRocksDBCheckpoint(Database cx,
                                                        CheckpointMetaData initialState,
                                                        std::string dir,
                                                        std::function<Future<Void>(const CheckpointMetaData&)> cFun) {
	TraceEvent(SevInfo, "FetchRocksCheckpointBegin")
	    .detail("InitialState", initialState.toString())
	    .detail("CheckpointDir", dir);

	ASSERT(!initialState.ranges.empty());

	state std::shared_ptr<CheckpointMetaData> metaData = std::make_shared<CheckpointMetaData>(initialState);

	if (metaData->getFormat() == DataMoveRocksCF) {
		state RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(initialState);
		TraceEvent(SevDebug, "RocksDBCheckpointMetaData").detail("RocksCF", rocksCF.toString());

		state int i = 0;
		state std::vector<Future<Void>> fs;
		for (; i < rocksCF.sstFiles.size(); ++i) {
			fs.push_back(fetchCheckpointFile(cx, metaData, i, dir, cFun));
			TraceEvent(SevDebug, "GetCheckpointFetchingFile")
			    .detail("FileName", rocksCF.sstFiles[i].name)
			    .detail("Server", describe(metaData->src));
		}
		wait(waitForAll(fs));
	} else if (metaData->getFormat() == RocksDBKeyValues) {
		wait(fetchCheckpointRanges(cx, metaData, dir, cFun));
	} else if (metaData->getFormat() == RocksDB) {
		throw not_implemented();
	}

	return *metaData;
}

ACTOR Future<Void> deleteRocksCheckpoint(CheckpointMetaData checkpoint) {
	state CheckpointFormat format = checkpoint.getFormat();
	state std::unordered_set<std::string> dirs;
	if (format == DataMoveRocksCF) {
		RocksDBColumnFamilyCheckpoint rocksCF = getRocksCF(checkpoint);
		TraceEvent(SevInfo, "DeleteRocksColumnFamilyCheckpoint", checkpoint.checkpointID)
		    .detail("CheckpointID", checkpoint.checkpointID)
		    .detail("RocksCF", rocksCF.toString());

		for (const LiveFileMetaData& file : rocksCF.sstFiles) {
			dirs.insert(file.db_path);
		}
	} else if (format == RocksDB) {
		RocksDBCheckpoint rocksCheckpoint = getRocksCheckpoint(checkpoint);
		TraceEvent(SevInfo, "DeleteRocksCheckpoint", checkpoint.checkpointID)
		    .detail("CheckpointID", checkpoint.checkpointID)
		    .detail("RocksCheckpoint", rocksCheckpoint.toString());
		dirs.insert(rocksCheckpoint.checkpointDir);
	} else {
		ASSERT(false);
	}

	state std::unordered_set<std::string>::iterator it = dirs.begin();
	for (; it != dirs.end(); ++it) {
		const std::string dir = *it;
		platform::eraseDirectoryRecursive(dir);
		TraceEvent(SevInfo, "DeleteCheckpointRemovedDir", checkpoint.checkpointID)
		    .detail("CheckpointID", checkpoint.checkpointID)
		    .detail("Dir", dir);
		wait(delay(0, TaskPriority::FetchKeys));
	}

	return Void();
}
#else
ACTOR Future<CheckpointMetaData> fetchRocksDBCheckpoint(Database cx,
                                                        CheckpointMetaData initialState,
                                                        std::string dir,
                                                        std::function<Future<Void>(const CheckpointMetaData&)> cFun) {
	wait(delay(0));
	return initialState;
}

ACTOR Future<Void> deleteRocksCheckpoint(CheckpointMetaData checkpoint) {
	wait(delay(0));
	return Void();
}
#endif // SSD_ROCKSDB_EXPERIMENTAL

int64_t getTotalFetchedBytes(const std::vector<CheckpointMetaData>& checkpoints) {
	int64_t totalBytes = 0;
	for (const auto& checkpoint : checkpoints) {
		const CheckpointFormat format = checkpoint.getFormat();
		if (format == DataMoveRocksCF) {
			// TODO: Returns the checkpoint size of a RocksDB Column Family.
		} else if (format == RocksDB) {
			auto rcp = getRocksCheckpoint(checkpoint);
			for (const auto& file : rcp.fetchedFiles) {
				totalBytes += file.size;
			}
		}
	}
	return totalBytes;
}

ICheckpointReader* newRocksDBCheckpointReader(const CheckpointMetaData& checkpoint,
                                              const CheckpointAsKeyValues checkpointAsKeyValues,
                                              UID logID) {
#ifdef SSD_ROCKSDB_EXPERIMENTAL
	const CheckpointFormat format = checkpoint.getFormat();
	if (format == DataMoveRocksCF && !checkpointAsKeyValues) {
		return new RocksDBCFCheckpointReader(checkpoint, logID);
	} else {
		return new RocksDBCheckpointReader(checkpoint, logID);
	}
#endif // SSD_ROCKSDB_EXPERIMENTAL
	return nullptr;
}

std::unique_ptr<IRocksDBSstFileWriter> newRocksDBSstFileWriter() {
#ifdef SSD_ROCKSDB_EXPERIMENTAL
	std::unique_ptr<IRocksDBSstFileWriter> sstWriter = std::make_unique<RocksDBSstFileWriter>();
	return sstWriter;
#endif // SSD_ROCKSDB_EXPERIMENTAL
	return nullptr;
}

RocksDBColumnFamilyCheckpoint getRocksCF(const CheckpointMetaData& checkpoint) {
	RocksDBColumnFamilyCheckpoint rocksCF;
	ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCF);
	return rocksCF;
}

RocksDBCheckpoint getRocksCheckpoint(const CheckpointMetaData& checkpoint) {
	RocksDBCheckpoint rocksCheckpoint;
	ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCheckpoint);
	return rocksCheckpoint;
}

RocksDBCheckpointKeyValues getRocksKeyValuesCheckpoint(const CheckpointMetaData& checkpoint) {
	RocksDBCheckpointKeyValues rocksCheckpoint;
	ObjectReader reader(checkpoint.serializedCheckpoint.begin(), IncludeVersion());
	reader.deserialize(rocksCheckpoint);
	return rocksCheckpoint;
}