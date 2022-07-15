//  Copyright (c) Meta Platforms, Inc. and affiliates.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/blob/blob_source.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "cache/compressed_secondary_cache.h"
#include "db/blob/blob_file_cache.h"
#include "db/blob/blob_file_reader.h"
#include "db/blob/blob_log_format.h"
#include "db/blob/blob_log_writer.h"
#include "db/db_test_util.h"
#include "file/filename.h"
#include "file/read_write_util.h"
#include "options/cf_options.h"
#include "rocksdb/options.h"
#include "util/compression.h"
#include "util/random.h"

namespace ROCKSDB_NAMESPACE {

namespace {

// Creates a test blob file with `num` blobs in it.
void WriteBlobFile(const ImmutableOptions& immutable_options,
                   uint32_t column_family_id, bool has_ttl,
                   const ExpirationRange& expiration_range_header,
                   const ExpirationRange& expiration_range_footer,
                   uint64_t blob_file_number, const std::vector<Slice>& keys,
                   const std::vector<Slice>& blobs, CompressionType compression,
                   std::vector<uint64_t>& blob_offsets,
                   std::vector<uint64_t>& blob_sizes) {
  assert(!immutable_options.cf_paths.empty());
  size_t num = keys.size();
  assert(num == blobs.size());
  assert(num == blob_offsets.size());
  assert(num == blob_sizes.size());

  const std::string blob_file_path =
      BlobFileName(immutable_options.cf_paths.front().path, blob_file_number);
  std::unique_ptr<FSWritableFile> file;
  ASSERT_OK(NewWritableFile(immutable_options.fs.get(), blob_file_path, &file,
                            FileOptions()));

  std::unique_ptr<WritableFileWriter> file_writer(new WritableFileWriter(
      std::move(file), blob_file_path, FileOptions(), immutable_options.clock));

  constexpr Statistics* statistics = nullptr;
  constexpr bool use_fsync = false;
  constexpr bool do_flush = false;

  BlobLogWriter blob_log_writer(std::move(file_writer), immutable_options.clock,
                                statistics, blob_file_number, use_fsync,
                                do_flush);

  BlobLogHeader header(column_family_id, compression, has_ttl,
                       expiration_range_header);

  ASSERT_OK(blob_log_writer.WriteHeader(header));

  std::vector<std::string> compressed_blobs(num);
  std::vector<Slice> blobs_to_write(num);
  if (kNoCompression == compression) {
    for (size_t i = 0; i < num; ++i) {
      blobs_to_write[i] = blobs[i];
      blob_sizes[i] = blobs[i].size();
    }
  } else {
    CompressionOptions opts;
    CompressionContext context(compression);
    constexpr uint64_t sample_for_compression = 0;
    CompressionInfo info(opts, context, CompressionDict::GetEmptyDict(),
                         compression, sample_for_compression);

    constexpr uint32_t compression_format_version = 2;

    for (size_t i = 0; i < num; ++i) {
      ASSERT_TRUE(CompressData(blobs[i], info, compression_format_version,
                               &compressed_blobs[i]));
      blobs_to_write[i] = compressed_blobs[i];
      blob_sizes[i] = compressed_blobs[i].size();
    }
  }

  for (size_t i = 0; i < num; ++i) {
    uint64_t key_offset = 0;
    ASSERT_OK(blob_log_writer.AddRecord(keys[i], blobs_to_write[i], &key_offset,
                                        &blob_offsets[i]));
  }

  BlobLogFooter footer;
  footer.blob_count = num;
  footer.expiration_range = expiration_range_footer;

  std::string checksum_method;
  std::string checksum_value;
  ASSERT_OK(
      blob_log_writer.AppendFooter(footer, &checksum_method, &checksum_value));
}

}  // anonymous namespace

class BlobSourceTest : public DBTestBase {
 protected:
 public:
  explicit BlobSourceTest()
      : DBTestBase("blob_source_test", /*env_do_fsync=*/true) {
    options_.env = env_;
    options_.enable_blob_files = true;
    options_.create_if_missing = true;

    LRUCacheOptions co;
    co.capacity = 8 << 20;
    co.num_shard_bits = 2;
    co.metadata_charge_policy = kDontChargeCacheMetadata;
    options_.blob_cache = NewLRUCache(co);
    options_.lowest_used_cache_tier = CacheTier::kVolatileTier;

    assert(db_->GetDbIdentity(db_id_).ok());
    assert(db_->GetDbSessionId(db_session_id_).ok());
  }

  Options options_;
  std::string db_id_;
  std::string db_session_id_;
};

TEST_F(BlobSourceTest, GetBlobsFromCache) {
  options_.cf_paths.emplace_back(
      test::PerThreadDBPath(env_, "BlobSourceTest_GetBlobsFromCache"), 0);

  options_.statistics = CreateDBStatistics();
  Statistics* statistics = options_.statistics.get();
  assert(statistics);

  DestroyAndReopen(options_);

  ImmutableOptions immutable_options(options_);

  constexpr uint32_t column_family_id = 1;
  constexpr bool has_ttl = false;
  constexpr ExpirationRange expiration_range;
  constexpr uint64_t blob_file_number = 1;
  constexpr size_t num_blobs = 16;

  std::vector<std::string> key_strs;
  std::vector<std::string> blob_strs;

  for (size_t i = 0; i < num_blobs; ++i) {
    key_strs.push_back("key" + std::to_string(i));
    blob_strs.push_back("blob" + std::to_string(i));
  }

  std::vector<Slice> keys;
  std::vector<Slice> blobs;

  uint64_t file_size = BlobLogHeader::kSize;
  for (size_t i = 0; i < num_blobs; ++i) {
    keys.push_back({key_strs[i]});
    blobs.push_back({blob_strs[i]});
    file_size += BlobLogRecord::kHeaderSize + keys[i].size() + blobs[i].size();
  }
  file_size += BlobLogFooter::kSize;

  std::vector<uint64_t> blob_offsets(keys.size());
  std::vector<uint64_t> blob_sizes(keys.size());

  WriteBlobFile(immutable_options, column_family_id, has_ttl, expiration_range,
                expiration_range, blob_file_number, keys, blobs, kNoCompression,
                blob_offsets, blob_sizes);

  constexpr size_t capacity = 1024;
  std::shared_ptr<Cache> backing_cache =
      NewLRUCache(capacity);  // Blob file cache

  FileOptions file_options;
  constexpr HistogramImpl* blob_file_read_hist = nullptr;

  std::unique_ptr<BlobFileCache> blob_file_cache(new BlobFileCache(
      backing_cache.get(), &immutable_options, &file_options, column_family_id,
      blob_file_read_hist, nullptr /*IOTracer*/));

  BlobSource blob_source(&immutable_options, db_id_, db_session_id_,
                         blob_file_cache.get());

  ReadOptions read_options;
  read_options.verify_checksums = true;

  constexpr FilePrefetchBuffer* prefetch_buffer = nullptr;

  {
    // GetBlob
    std::vector<PinnableSlice> values(keys.size());
    uint64_t bytes_read = 0;
    uint64_t blob_bytes = 0;
    uint64_t total_bytes = 0;

    read_options.fill_cache = false;
    get_perf_context()->Reset();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));

      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    kNoCompression, prefetch_buffer, &values[i],
                                    &bytes_read));
      ASSERT_EQ(values[i], blobs[i]);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));
      total_bytes += bytes_read;
    }

    // Retrieved the blob cache num_blobs * 3 times via TEST_BlobInCache,
    // GetBlob, and TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, num_blobs);
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, total_bytes);
    ASSERT_GE((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);

    read_options.fill_cache = true;
    blob_bytes = 0;
    total_bytes = 0;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));

      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    kNoCompression, prefetch_buffer, &values[i],
                                    &bytes_read));
      ASSERT_EQ(values[i], blobs[i]);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      blob_bytes += blob_sizes[i];
      total_bytes += bytes_read;
      ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, i);
      ASSERT_EQ((int)get_perf_context()->blob_read_count, i + 1);
      ASSERT_EQ((int)get_perf_context()->blob_read_byte, total_bytes);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));

      ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, i + 1);
      ASSERT_EQ((int)get_perf_context()->blob_read_count, i + 1);
      ASSERT_EQ((int)get_perf_context()->blob_read_byte, total_bytes);
    }

    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, num_blobs);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, num_blobs);
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, total_bytes);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), num_blobs);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), num_blobs);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ), blob_bytes);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE),
              blob_bytes);

    read_options.fill_cache = true;
    total_bytes = 0;
    blob_bytes = 0;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));

      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    kNoCompression, prefetch_buffer, &values[i],
                                    &bytes_read));
      ASSERT_EQ(values[i], blobs[i]);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));
      total_bytes += bytes_read;    // on-disk blob record size
      blob_bytes += blob_sizes[i];  // cached blob value size
    }

    // Retrieved the blob cache num_blobs * 3 times via TEST_BlobInCache,
    // GetBlob, and TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, num_blobs * 3);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);  // without i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);   // without i/o

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), num_blobs * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ),
              blob_bytes * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);

    // Cache-only GetBlob
    read_options.read_tier = ReadTier::kBlockCacheTier;
    total_bytes = 0;
    blob_bytes = 0;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));

      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    kNoCompression, prefetch_buffer, &values[i],
                                    &bytes_read));
      ASSERT_EQ(values[i], blobs[i]);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));
      total_bytes += bytes_read;
      blob_bytes += blob_sizes[i];
    }

    // Retrieved the blob cache num_blobs * 3 times via TEST_BlobInCache,
    // GetBlob, and TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, num_blobs * 3);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);  // without i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);   // without i/o

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), num_blobs * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ),
              blob_bytes * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }

  options_.blob_cache->EraseUnRefEntries();

  {
    // Cache-only GetBlob
    std::vector<PinnableSlice> values(keys.size());
    uint64_t bytes_read = 0;

    read_options.read_tier = ReadTier::kBlockCacheTier;
    read_options.fill_cache = true;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));

      ASSERT_TRUE(blob_source
                      .GetBlob(read_options, keys[i], blob_file_number,
                               blob_offsets[i], file_size, blob_sizes[i],
                               kNoCompression, prefetch_buffer, &values[i],
                               &bytes_read)
                      .IsIncomplete());
      ASSERT_TRUE(values[i].empty());
      ASSERT_EQ(bytes_read, 0);

      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));
    }

    // Retrieved the blob cache num_blobs * 3 times via TEST_BlobInCache,
    // GetBlob, and TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }

  {
    // GetBlob from non-existing file
    std::vector<PinnableSlice> values(keys.size());
    uint64_t bytes_read = 0;
    uint64_t file_number = 100;  // non-existing file

    read_options.read_tier = ReadTier::kReadAllTier;
    read_options.fill_cache = true;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_FALSE(blob_source.TEST_BlobInCache(file_number, file_size,
                                                blob_offsets[i]));

      ASSERT_TRUE(blob_source
                      .GetBlob(read_options, keys[i], file_number,
                               blob_offsets[i], file_size, blob_sizes[i],
                               kNoCompression, prefetch_buffer, &values[i],
                               &bytes_read)
                      .IsIOError());
      ASSERT_TRUE(values[i].empty());
      ASSERT_EQ(bytes_read, 0);

      ASSERT_FALSE(blob_source.TEST_BlobInCache(file_number, file_size,
                                                blob_offsets[i]));
    }

    // Retrieved the blob cache num_blobs * 3 times via TEST_BlobInCache,
    // GetBlob, and TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 3);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }
}

TEST_F(BlobSourceTest, GetCompressedBlobs) {
  if (!Snappy_Supported()) {
    return;
  }

  const CompressionType compression = kSnappyCompression;

  options_.cf_paths.emplace_back(
      test::PerThreadDBPath(env_, "BlobSourceTest_GetCompressedBlobs"), 0);

  DestroyAndReopen(options_);

  ImmutableOptions immutable_options(options_);

  constexpr uint32_t column_family_id = 1;
  constexpr bool has_ttl = false;
  constexpr ExpirationRange expiration_range;
  constexpr size_t num_blobs = 256;

  std::vector<std::string> key_strs;
  std::vector<std::string> blob_strs;

  for (size_t i = 0; i < num_blobs; ++i) {
    key_strs.push_back("key" + std::to_string(i));
    blob_strs.push_back("blob" + std::to_string(i));
  }

  std::vector<Slice> keys;
  std::vector<Slice> blobs;

  for (size_t i = 0; i < num_blobs; ++i) {
    keys.push_back({key_strs[i]});
    blobs.push_back({blob_strs[i]});
  }

  std::vector<uint64_t> blob_offsets(keys.size());
  std::vector<uint64_t> blob_sizes(keys.size());

  constexpr size_t capacity = 1024;
  auto backing_cache = NewLRUCache(capacity);  // Blob file cache

  FileOptions file_options;
  std::unique_ptr<BlobFileCache> blob_file_cache(new BlobFileCache(
      backing_cache.get(), &immutable_options, &file_options, column_family_id,
      nullptr /*HistogramImpl*/, nullptr /*IOTracer*/));

  BlobSource blob_source(&immutable_options, db_id_, db_session_id_,
                         blob_file_cache.get());

  ReadOptions read_options;
  read_options.verify_checksums = true;

  uint64_t bytes_read = 0;
  std::vector<PinnableSlice> values(keys.size());

  {
    // Snappy Compression
    const uint64_t file_number = 1;

    read_options.read_tier = ReadTier::kReadAllTier;

    WriteBlobFile(immutable_options, column_family_id, has_ttl,
                  expiration_range, expiration_range, file_number, keys, blobs,
                  compression, blob_offsets, blob_sizes);

    CacheHandleGuard<BlobFileReader> blob_file_reader;
    ASSERT_OK(blob_source.GetBlobFileReader(file_number, &blob_file_reader));
    ASSERT_NE(blob_file_reader.GetValue(), nullptr);

    const uint64_t file_size = blob_file_reader.GetValue()->GetFileSize();
    ASSERT_EQ(blob_file_reader.GetValue()->GetCompressionType(), compression);

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_NE(blobs[i].size() /*uncompressed size*/,
                blob_sizes[i] /*compressed size*/);
    }

    read_options.fill_cache = true;
    read_options.read_tier = ReadTier::kReadAllTier;
    get_perf_context()->Reset();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_FALSE(blob_source.TEST_BlobInCache(file_number, file_size,
                                                blob_offsets[i]));
      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    compression, nullptr /*prefetch_buffer*/,
                                    &values[i], &bytes_read));
      ASSERT_EQ(values[i], blobs[i] /*uncompressed blob*/);
      ASSERT_NE(values[i].size(), blob_sizes[i] /*compressed size*/);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[i]));
    }

    ASSERT_GE((int)get_perf_context()->blob_decompress_time, 0);

    read_options.read_tier = ReadTier::kBlockCacheTier;
    get_perf_context()->Reset();

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[i]));

      // Compressed blob size is passed in GetBlob
      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    compression, nullptr /*prefetch_buffer*/,
                                    &values[i], &bytes_read));
      ASSERT_EQ(values[i], blobs[i] /*uncompressed blob*/);
      ASSERT_NE(values[i].size(), blob_sizes[i] /*compressed size*/);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[i]));
    }

    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);
  }
}

TEST_F(BlobSourceTest, MultiGetBlobsFromMultiFiles) {
  options_.cf_paths.emplace_back(
      test::PerThreadDBPath(env_, "BlobSourceTest_MultiGetBlobsFromMultiFiles"),
      0);

  options_.statistics = CreateDBStatistics();
  Statistics* statistics = options_.statistics.get();
  assert(statistics);

  DestroyAndReopen(options_);

  ImmutableOptions immutable_options(options_);

  constexpr uint32_t column_family_id = 1;
  constexpr bool has_ttl = false;
  constexpr ExpirationRange expiration_range;
  constexpr uint64_t blob_files = 2;
  constexpr size_t num_blobs = 32;

  std::vector<std::string> key_strs;
  std::vector<std::string> blob_strs;

  for (size_t i = 0; i < num_blobs; ++i) {
    key_strs.push_back("key" + std::to_string(i));
    blob_strs.push_back("blob" + std::to_string(i));
  }

  std::vector<Slice> keys;
  std::vector<Slice> blobs;

  uint64_t file_size = BlobLogHeader::kSize;
  uint64_t blob_value_bytes = 0;
  for (size_t i = 0; i < num_blobs; ++i) {
    keys.push_back({key_strs[i]});
    blobs.push_back({blob_strs[i]});
    blob_value_bytes += blobs[i].size();
    file_size += BlobLogRecord::kHeaderSize + keys[i].size() + blobs[i].size();
  }
  file_size += BlobLogFooter::kSize;
  const uint64_t blob_records_bytes =
      file_size - BlobLogHeader::kSize - BlobLogFooter::kSize;

  std::vector<uint64_t> blob_offsets(keys.size());
  std::vector<uint64_t> blob_sizes(keys.size());

  {
    // Write key/blob pairs to multiple blob files.
    for (size_t i = 0; i < blob_files; ++i) {
      const uint64_t file_number = i + 1;
      WriteBlobFile(immutable_options, column_family_id, has_ttl,
                    expiration_range, expiration_range, file_number, keys,
                    blobs, kNoCompression, blob_offsets, blob_sizes);
    }
  }

  constexpr size_t capacity = 10;
  std::shared_ptr<Cache> backing_cache =
      NewLRUCache(capacity);  // Blob file cache

  FileOptions file_options;
  constexpr HistogramImpl* blob_file_read_hist = nullptr;

  std::unique_ptr<BlobFileCache> blob_file_cache(new BlobFileCache(
      backing_cache.get(), &immutable_options, &file_options, column_family_id,
      blob_file_read_hist, nullptr /*IOTracer*/));

  BlobSource blob_source(&immutable_options, db_id_, db_session_id_,
                         blob_file_cache.get());

  ReadOptions read_options;
  read_options.verify_checksums = true;

  uint64_t bytes_read = 0;

  {
    // MultiGetBlob
    read_options.fill_cache = true;
    read_options.read_tier = ReadTier::kReadAllTier;

    autovector<BlobFileReadRequests> blob_reqs;
    std::array<autovector<BlobReadRequest>, blob_files> blob_reqs_in_file;
    std::array<PinnableSlice, num_blobs * blob_files> value_buf;
    std::array<Status, num_blobs * blob_files> statuses_buf;

    for (size_t i = 0; i < blob_files; ++i) {
      const uint64_t file_number = i + 1;
      for (size_t j = 0; j < num_blobs; ++j) {
        blob_reqs_in_file[i].emplace_back(
            keys[j], blob_offsets[j], blob_sizes[j], kNoCompression,
            &value_buf[i * num_blobs + j], &statuses_buf[i * num_blobs + j]);
      }
      blob_reqs.emplace_back(file_number, file_size, blob_reqs_in_file[i]);
    }

    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    blob_source.MultiGetBlob(read_options, blob_reqs, &bytes_read);

    for (size_t i = 0; i < blob_files; ++i) {
      const uint64_t file_number = i + 1;
      for (size_t j = 0; j < num_blobs; ++j) {
        ASSERT_OK(statuses_buf[i * num_blobs + j]);
        ASSERT_EQ(value_buf[i * num_blobs + j], blobs[j]);
        ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                                 blob_offsets[j]));
      }
    }

    // Retrieved all blobs from 2 blob files twice via MultiGetBlob and
    // TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count,
              num_blobs * blob_files);
    ASSERT_EQ((int)get_perf_context()->blob_read_count,
              num_blobs * blob_files);  // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte,
              blob_records_bytes * blob_files);  // blocking i/o
    ASSERT_GE((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS),
              num_blobs * blob_files);  // MultiGetBlob
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT),
              num_blobs * blob_files);  // TEST_BlobInCache
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD),
              num_blobs * blob_files);  // MultiGetBlob
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ),
              blob_value_bytes * blob_files);  // TEST_BlobInCache
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE),
              blob_value_bytes * blob_files);  // MultiGetBlob

    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    autovector<BlobReadRequest> fake_blob_reqs_in_file;
    std::array<PinnableSlice, num_blobs> fake_value_buf;
    std::array<Status, num_blobs> fake_statuses_buf;

    const uint64_t fake_file_number = 100;
    for (size_t i = 0; i < num_blobs; ++i) {
      fake_blob_reqs_in_file.emplace_back(
          keys[i], blob_offsets[i], blob_sizes[i], kNoCompression,
          &fake_value_buf[i], &fake_statuses_buf[i]);
    }

    // Add a fake multi-get blob request.
    blob_reqs.emplace_back(fake_file_number, file_size, fake_blob_reqs_in_file);

    blob_source.MultiGetBlob(read_options, blob_reqs, &bytes_read);

    // Check the real blob read requests.
    for (size_t i = 0; i < blob_files; ++i) {
      const uint64_t file_number = i + 1;
      for (size_t j = 0; j < num_blobs; ++j) {
        ASSERT_OK(statuses_buf[i * num_blobs + j]);
        ASSERT_EQ(value_buf[i * num_blobs + j], blobs[j]);
        ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                                 blob_offsets[j]));
      }
    }

    // Check the fake blob request.
    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_TRUE(fake_statuses_buf[i].IsIOError());
      ASSERT_TRUE(fake_value_buf[i].empty());
      ASSERT_FALSE(blob_source.TEST_BlobInCache(fake_file_number, file_size,
                                                blob_offsets[i]));
    }

    // Retrieved all blobs from 3 blob files (including the fake one) twice
    // via MultiGetBlob and TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count,
              num_blobs * blob_files * 2);
    ASSERT_EQ((int)get_perf_context()->blob_read_count,
              0);  // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte,
              0);  // blocking i/o
    ASSERT_GE((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    // Fake blob requests: MultiGetBlob and TEST_BlobInCache
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 2);
    // Real blob requests: MultiGetBlob and TEST_BlobInCache
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT),
              num_blobs * blob_files * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    // Real blob requests: MultiGetBlob and TEST_BlobInCache
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ),
              blob_value_bytes * blob_files * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }
}

TEST_F(BlobSourceTest, MultiGetBlobsFromCache) {
  options_.cf_paths.emplace_back(
      test::PerThreadDBPath(env_, "BlobSourceTest_MultiGetBlobsFromCache"), 0);

  options_.statistics = CreateDBStatistics();
  Statistics* statistics = options_.statistics.get();
  assert(statistics);

  DestroyAndReopen(options_);

  ImmutableOptions immutable_options(options_);

  constexpr uint32_t column_family_id = 1;
  constexpr bool has_ttl = false;
  constexpr ExpirationRange expiration_range;
  constexpr uint64_t blob_file_number = 1;
  constexpr size_t num_blobs = 16;

  std::vector<std::string> key_strs;
  std::vector<std::string> blob_strs;

  for (size_t i = 0; i < num_blobs; ++i) {
    key_strs.push_back("key" + std::to_string(i));
    blob_strs.push_back("blob" + std::to_string(i));
  }

  std::vector<Slice> keys;
  std::vector<Slice> blobs;

  uint64_t file_size = BlobLogHeader::kSize;
  for (size_t i = 0; i < num_blobs; ++i) {
    keys.push_back({key_strs[i]});
    blobs.push_back({blob_strs[i]});
    file_size += BlobLogRecord::kHeaderSize + keys[i].size() + blobs[i].size();
  }
  file_size += BlobLogFooter::kSize;

  std::vector<uint64_t> blob_offsets(keys.size());
  std::vector<uint64_t> blob_sizes(keys.size());

  WriteBlobFile(immutable_options, column_family_id, has_ttl, expiration_range,
                expiration_range, blob_file_number, keys, blobs, kNoCompression,
                blob_offsets, blob_sizes);

  constexpr size_t capacity = 10;
  std::shared_ptr<Cache> backing_cache =
      NewLRUCache(capacity);  // Blob file cache

  FileOptions file_options;
  constexpr HistogramImpl* blob_file_read_hist = nullptr;

  std::unique_ptr<BlobFileCache> blob_file_cache(new BlobFileCache(
      backing_cache.get(), &immutable_options, &file_options, column_family_id,
      blob_file_read_hist, nullptr /*IOTracer*/));

  BlobSource blob_source(&immutable_options, db_id_, db_session_id_,
                         blob_file_cache.get());

  ReadOptions read_options;
  read_options.verify_checksums = true;

  constexpr FilePrefetchBuffer* prefetch_buffer = nullptr;

  {
    // MultiGetBlobFromOneFile
    uint64_t bytes_read = 0;
    std::array<Status, num_blobs> statuses_buf;
    std::array<PinnableSlice, num_blobs> value_buf;
    autovector<BlobReadRequest> blob_reqs;

    for (size_t i = 0; i < num_blobs; i += 2) {  // even index
      blob_reqs.emplace_back(keys[i], blob_offsets[i], blob_sizes[i],
                             kNoCompression, &value_buf[i], &statuses_buf[i]);
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));
    }

    read_options.fill_cache = true;
    read_options.read_tier = ReadTier::kReadAllTier;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    // Get half of blobs
    blob_source.MultiGetBlobFromOneFile(read_options, blob_file_number,
                                        file_size, blob_reqs, &bytes_read);

    uint64_t fs_read_bytes = 0;
    uint64_t ca_read_bytes = 0;
    for (size_t i = 0; i < num_blobs; ++i) {
      if (i % 2 == 0) {
        ASSERT_OK(statuses_buf[i]);
        ASSERT_EQ(value_buf[i], blobs[i]);
        fs_read_bytes +=
            blob_sizes[i] + keys[i].size() + BlobLogRecord::kHeaderSize;
        ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                 blob_offsets[i]));
        ca_read_bytes += blob_sizes[i];
      } else {
        statuses_buf[i].PermitUncheckedError();
        ASSERT_TRUE(value_buf[i].empty());
        ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                  blob_offsets[i]));
      }
    }

    constexpr int num_even_blobs = num_blobs / 2;
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, num_even_blobs);
    ASSERT_EQ((int)get_perf_context()->blob_read_count,
              num_even_blobs);  // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte,
              fs_read_bytes);  // blocking i/o
    ASSERT_GE((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), num_even_blobs);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), num_even_blobs);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ),
              ca_read_bytes);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE),
              ca_read_bytes);

    // Get the rest of blobs
    for (size_t i = 1; i < num_blobs; i += 2) {  // odd index
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));

      ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number,
                                    blob_offsets[i], file_size, blob_sizes[i],
                                    kNoCompression, prefetch_buffer,
                                    &value_buf[i], &bytes_read));
      ASSERT_EQ(value_buf[i], blobs[i]);
      ASSERT_EQ(bytes_read,
                BlobLogRecord::kHeaderSize + keys[i].size() + blob_sizes[i]);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));
    }

    // Cache-only MultiGetBlobFromOneFile
    read_options.read_tier = ReadTier::kBlockCacheTier;
    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    blob_reqs.clear();
    for (size_t i = 0; i < num_blobs; ++i) {
      blob_reqs.emplace_back(keys[i], blob_offsets[i], blob_sizes[i],
                             kNoCompression, &value_buf[i], &statuses_buf[i]);
    }

    blob_source.MultiGetBlobFromOneFile(read_options, blob_file_number,
                                        file_size, blob_reqs, &bytes_read);

    uint64_t blob_bytes = 0;
    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_OK(statuses_buf[i]);
      ASSERT_EQ(value_buf[i], blobs[i]);
      ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                               blob_offsets[i]));
      blob_bytes += blob_sizes[i];
    }

    // Retrieved the blob cache num_blobs * 2 times via GetBlob and
    // TEST_BlobInCache.
    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, num_blobs * 2);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);  // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);   // blocking i/o
    ASSERT_GE((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), num_blobs * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ),
              blob_bytes * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }

  options_.blob_cache->EraseUnRefEntries();

  {
    // Cache-only MultiGetBlobFromOneFile
    uint64_t bytes_read = 0;
    read_options.read_tier = ReadTier::kBlockCacheTier;

    std::array<Status, num_blobs> statuses_buf;
    std::array<PinnableSlice, num_blobs> value_buf;
    autovector<BlobReadRequest> blob_reqs;

    for (size_t i = 0; i < num_blobs; i++) {
      blob_reqs.emplace_back(keys[i], blob_offsets[i], blob_sizes[i],
                             kNoCompression, &value_buf[i], &statuses_buf[i]);
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));
    }

    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    blob_source.MultiGetBlobFromOneFile(read_options, blob_file_number,
                                        file_size, blob_reqs, &bytes_read);

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_TRUE(statuses_buf[i].IsIncomplete());
      ASSERT_TRUE(value_buf[i].empty());
      ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size,
                                                blob_offsets[i]));
    }

    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);  // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);   // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }

  {
    // MultiGetBlobFromOneFile from non-existing file
    uint64_t bytes_read = 0;
    uint64_t non_existing_file_number = 100;
    read_options.read_tier = ReadTier::kReadAllTier;

    std::array<Status, num_blobs> statuses_buf;
    std::array<PinnableSlice, num_blobs> value_buf;
    autovector<BlobReadRequest> blob_reqs;

    for (size_t i = 0; i < num_blobs; i++) {
      blob_reqs.emplace_back(keys[i], blob_offsets[i], blob_sizes[i],
                             kNoCompression, &value_buf[i], &statuses_buf[i]);
      ASSERT_FALSE(blob_source.TEST_BlobInCache(non_existing_file_number,
                                                file_size, blob_offsets[i]));
    }

    get_perf_context()->Reset();
    statistics->Reset().PermitUncheckedError();

    blob_source.MultiGetBlobFromOneFile(read_options, non_existing_file_number,
                                        file_size, blob_reqs, &bytes_read);

    for (size_t i = 0; i < num_blobs; ++i) {
      ASSERT_TRUE(statuses_buf[i].IsIOError());
      ASSERT_TRUE(value_buf[i].empty());
      ASSERT_FALSE(blob_source.TEST_BlobInCache(non_existing_file_number,
                                                file_size, blob_offsets[i]));
    }

    ASSERT_EQ((int)get_perf_context()->blob_cache_hit_count, 0);
    ASSERT_EQ((int)get_perf_context()->blob_read_count, 0);  // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_read_byte, 0);   // blocking i/o
    ASSERT_EQ((int)get_perf_context()->blob_checksum_time, 0);
    ASSERT_EQ((int)get_perf_context()->blob_decompress_time, 0);

    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_MISS), num_blobs * 2);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_HIT), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_ADD), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_READ), 0);
    ASSERT_EQ(statistics->getTickerCount(BLOB_DB_CACHE_BYTES_WRITE), 0);
  }
}

class BlobSecondaryCacheTest : public DBTestBase {
 protected:
 public:
  explicit BlobSecondaryCacheTest()
      : DBTestBase("blob_secondary_cache_test", /*env_do_fsync=*/true) {
    options_.env = env_;
    options_.enable_blob_files = true;
    options_.create_if_missing = true;

    // Set a small cache capacity to evict entries from the cache, and to test
    // that secondary cache is used properly.
    lru_cache_ops_.capacity = 1024;
    lru_cache_ops_.num_shard_bits = 2;
    lru_cache_ops_.metadata_charge_policy = kDontChargeCacheMetadata;

    secondary_cache_opts_.capacity = 8 << 20;  // 8 MB
    secondary_cache_opts_.num_shard_bits = 2;
    secondary_cache_opts_.metadata_charge_policy = kDontChargeCacheMetadata;

    // Read blobs from the secondary cache if they are not in the primary cache
    options_.lowest_used_cache_tier = CacheTier::kNonVolatileBlockTier;

    assert(db_->GetDbIdentity(db_id_).ok());
    assert(db_->GetDbSessionId(db_session_id_).ok());
  }

  Options options_;

  LRUCacheOptions lru_cache_ops_;
  CompressedSecondaryCacheOptions secondary_cache_opts_;

  std::string db_id_;
  std::string db_session_id_;
};

TEST_F(BlobSecondaryCacheTest, GetBlobsFromSecondaryCache) {
  if (!Snappy_Supported()) {
    return;
  }

  secondary_cache_opts_.compression_type = kSnappyCompression;
  lru_cache_ops_.secondary_cache =
      NewCompressedSecondaryCache(secondary_cache_opts_);
  options_.blob_cache = NewLRUCache(lru_cache_ops_);

  options_.cf_paths.emplace_back(
      test::PerThreadDBPath(
          env_, "BlobSecondaryCacheTest_GetBlobsFromSecondaryCache"),
      0);

  options_.statistics = CreateDBStatistics();
  Statistics* statistics = options_.statistics.get();
  assert(statistics);

  DestroyAndReopen(options_);

  ImmutableOptions immutable_options(options_);

  constexpr uint32_t column_family_id = 1;
  constexpr bool has_ttl = false;
  constexpr ExpirationRange expiration_range;
  constexpr uint64_t file_number = 1;

  Random rnd(301);

  std::vector<std::string> key_strs;
  std::vector<std::string> blob_strs;
  key_strs.push_back("key0");
  blob_strs.push_back(rnd.RandomString(1020));
  key_strs.push_back("key1");
  blob_strs.push_back(rnd.RandomString(1020));

  std::vector<Slice> keys;
  std::vector<Slice> blobs;
  keys.push_back({key_strs[0]});
  blobs.push_back({blob_strs[0]});
  keys.push_back({key_strs[1]});
  blobs.push_back({blob_strs[1]});

  std::vector<uint64_t> blob_offsets(keys.size());
  std::vector<uint64_t> blob_sizes(keys.size());

  WriteBlobFile(immutable_options, column_family_id, has_ttl, expiration_range,
                expiration_range, file_number, keys, blobs, kNoCompression,
                blob_offsets, blob_sizes);

  constexpr size_t capacity = 1024;
  std::shared_ptr<Cache> backing_cache = NewLRUCache(capacity);

  FileOptions file_options;
  constexpr HistogramImpl* blob_file_read_hist = nullptr;

  std::unique_ptr<BlobFileCache> blob_file_cache(new BlobFileCache(
      backing_cache.get(), &immutable_options, &file_options, column_family_id,
      blob_file_read_hist, nullptr /*IOTracer*/));

  BlobSource blob_source(&immutable_options, db_id_, db_session_id_,
                         blob_file_cache.get());

  CacheHandleGuard<BlobFileReader> file_reader;
  ASSERT_OK(blob_source.GetBlobFileReader(file_number, &file_reader));
  ASSERT_NE(file_reader.GetValue(), nullptr);
  const uint64_t file_size = file_reader.GetValue()->GetFileSize();
  ASSERT_EQ(file_reader.GetValue()->GetCompressionType(), kNoCompression);

  ReadOptions read_options;
  read_options.verify_checksums = true;

  auto blob_cache = options_.blob_cache;
  auto secondary_cache = lru_cache_ops_.secondary_cache;

  Cache::CreateCallback create_cb = [&](const void* buf, size_t size,
                                        void** out_obj,
                                        size_t* charge) -> Status {
    std::string* blob = new std::string();
    blob->assign(static_cast<const char*>(buf), size);
    *out_obj = blob;
    *charge = size;
    return Status::OK();
  };

  {
    // GetBlob
    std::vector<PinnableSlice> values(keys.size());

    read_options.fill_cache = true;
    get_perf_context()->Reset();

    // key0 should be filled to the primary cache from the blob file.
    ASSERT_OK(blob_source.GetBlob(read_options, keys[0], file_number,
                                  blob_offsets[0], file_size, blob_sizes[0],
                                  kNoCompression, nullptr /* prefetch_buffer */,
                                  &values[0], nullptr /* bytes_read */));
    ASSERT_EQ(values[0], blobs[0]);
    ASSERT_TRUE(
        blob_source.TEST_BlobInCache(file_number, file_size, blob_offsets[0]));

    // key0 should be demoted to the secondary cache, and key1 should be filled
    // to the primary cache from the blob file.
    ASSERT_OK(blob_source.GetBlob(read_options, keys[1], file_number,
                                  blob_offsets[1], file_size, blob_sizes[1],
                                  kNoCompression, nullptr /* prefetch_buffer */,
                                  &values[1], nullptr /* bytes_read */));
    ASSERT_EQ(values[1], blobs[1]);
    ASSERT_TRUE(
        blob_source.TEST_BlobInCache(file_number, file_size, blob_offsets[1]));

    OffsetableCacheKey base_cache_key(db_id_, db_session_id_, file_number,
                                      file_size);

    // blob_cache here only looks at the primary cache since we didn't provide
    // the cache item helper for the secondary cache. However, since key0 is
    // demoted to the secondary cache, we shouldn't be able to find it in the
    // primary cache.
    {
      CacheKey cache_key = base_cache_key.WithOffset(blob_offsets[0]);
      const Slice key0 = cache_key.AsSlice();
      auto handle0 = blob_cache->Lookup(key0, statistics);
      ASSERT_EQ(handle0, nullptr);
      blob_cache->Release(handle0);

      bool found = false;
      auto sec_handle0 = secondary_cache->Lookup(key0, create_cb, true, found);
      ASSERT_TRUE(found);
      ASSERT_NE(sec_handle0, nullptr);
      ASSERT_TRUE(sec_handle0->IsReady());
      auto value = static_cast<std::string*>(sec_handle0->Value());
      ASSERT_EQ(*value, blobs[0]);
      delete value;

      // For blob source interface, after a cache miss occurs in the primary
      // cache, key0 can be retrieved in the secondary cache.
      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[0]));
    }

    // key1 should exist in the primary cache.
    {
      CacheKey cache_key = base_cache_key.WithOffset(blob_offsets[1]);
      const Slice key1 = cache_key.AsSlice();
      auto handle1 = blob_cache->Lookup(key1, statistics);
      ASSERT_NE(handle1, nullptr);
      blob_cache->Release(handle1);

      bool found = false;
      auto sec_handle1 = secondary_cache->Lookup(key1, create_cb, true, found);
      ASSERT_FALSE(found);
      ASSERT_NE(sec_handle1, nullptr);
      ASSERT_EQ(sec_handle1->Value(), nullptr);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[1]));
    }

    // key0 promotion should fail due to the blob cache being at capacity,
    // but the lookup should still succeed
    {
      CacheKey cache_key = base_cache_key.WithOffset(blob_offsets[0]);
      const Slice key0 = cache_key.AsSlice();
      auto handle0 = blob_cache->Lookup(key0, statistics);
      ASSERT_EQ(handle0, nullptr);
      blob_cache->Release(handle0);

      bool found = false;
      auto sec_handle0 = secondary_cache->Lookup(key0, create_cb, true, found);
      ASSERT_TRUE(found);
      ASSERT_NE(sec_handle0, nullptr);
      ASSERT_TRUE(sec_handle0->IsReady());
      auto value = static_cast<std::string*>(sec_handle0->Value());
      ASSERT_EQ(*value, blobs[0]);
      delete value;

      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[0]));
    }

    // key1 should still be in the primary cache because key0 wasn't added back
    // into it.
    {
      CacheKey cache_key = base_cache_key.WithOffset(blob_offsets[1]);
      const Slice key1 = cache_key.AsSlice();
      auto handle1 = blob_cache->Lookup(key1, statistics);
      ASSERT_NE(handle1, nullptr);
      blob_cache->Release(handle1);

      bool found = false;
      auto sec_handle1 = secondary_cache->Lookup(key1, create_cb, true, found);
      ASSERT_FALSE(found);
      ASSERT_NE(sec_handle1, nullptr);
      ASSERT_EQ(sec_handle1->Value(), nullptr);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[1]));
    }

    // check if we can retrieve the same values we inserted even some of the
    // key/value pairs are in the compressed secondary cache.
    ASSERT_OK(blob_source.GetBlob(read_options, keys[0], file_number,
                                  blob_offsets[0], file_size, blob_sizes[0],
                                  kNoCompression, nullptr /* prefetch_buffer */,
                                  &values[0], nullptr /* bytes_read */));
    ASSERT_EQ(values[0], blobs[0]);

    ASSERT_OK(blob_source.GetBlob(read_options, keys[1], file_number,
                                  blob_offsets[1], file_size, blob_sizes[1],
                                  kNoCompression, nullptr /* prefetch_buffer */,
                                  &values[1], nullptr /* bytes_read */));
    ASSERT_EQ(values[1], blobs[1]);

    // key0 promotion should succeed due to key1 was erased from the primary
    // cache.
    {
      CacheKey cache_key = base_cache_key.WithOffset(blob_offsets[1]);
      const Slice key1 = cache_key.AsSlice();
      blob_cache->Erase(key1);

      auto handle1 = blob_cache->Lookup(key1, statistics);
      ASSERT_EQ(handle1, nullptr);
      blob_cache->Release(handle1);

      ASSERT_FALSE(blob_source.TEST_BlobInCache(file_number, file_size,
                                                blob_offsets[1]));

      cache_key = base_cache_key.WithOffset(blob_offsets[0]);
      const Slice key0 = cache_key.AsSlice();

      // before we promote key0 to the primary cache
      bool found = false;
      auto sec_handle0 = secondary_cache->Lookup(key0, create_cb, true, found);
      ASSERT_TRUE(found);
      ASSERT_NE(sec_handle0, nullptr);
      ASSERT_TRUE(sec_handle0->IsReady());
      auto value = static_cast<std::string*>(sec_handle0->Value());
      ASSERT_EQ(*value, blobs[0]);
      delete value;

      auto handle0 = blob_cache->Lookup(key0, statistics);
      ASSERT_NE(handle0, nullptr);
      blob_cache->Release(handle0);

      // after we promote key0 to the primary cache
      found = false;
      sec_handle0 = secondary_cache->Lookup(key0, create_cb, true, found);
      ASSERT_FALSE(found);
      ASSERT_NE(sec_handle0, nullptr);
      ASSERT_EQ(sec_handle0->Value(), nullptr);

      ASSERT_TRUE(blob_source.TEST_BlobInCache(file_number, file_size,
                                               blob_offsets[0]));
    }
  }
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
