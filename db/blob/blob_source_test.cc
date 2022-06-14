//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "db/blob/blob_source.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include "db/blob/blob_file_reader.h"
#include "db/blob/blob_log_format.h"
#include "db/blob/blob_log_writer.h"
#include "db/blob/blob_source.h"
#include "env/mock_env.h"
#include "file/filename.h"
#include "file/read_write_util.h"
#include "file/writable_file_writer.h"
#include "options/cf_options.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/options.h"
#include "table/unique_id_impl.h"
#include "test_util/sync_point.h"
#include "test_util/testharness.h"
#include "util/compression.h"
#include "utilities/fault_injection_env.h"

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

class BlobSourceTest : public testing::Test {
 protected:
  BlobSourceTest() {
    mock_env_.reset(MockEnv::Create(Env::Default()));
    db_session_id_ = EncodeSessionId(base_session_upper_, base_session_lower_);
  }

  std::unique_ptr<Env> mock_env_;

  std::string db_session_id_;
  std::string db_id_ = std::string("1234");

  uint64_t base_session_upper_ = 1234;
  uint64_t base_session_lower_ = 5678;
};

TEST_F(BlobSourceTest, GetBlobsFromCache) {
  Options options;
  options.env = mock_env_.get();
  options.cf_paths.emplace_back(
      test::PerThreadDBPath(mock_env_.get(),
                            "BlobFileReaderTest_GetBlobsFromCache"),
      0);
  options.enable_blob_files = true;

  LRUCacheOptions co;
  co.capacity = 2048;
  co.num_shard_bits = 2;
  co.metadata_charge_policy = kDontChargeCacheMetadata;
  options.blob_cache = NewLRUCache(co);
  options.lowest_used_cache_tier = CacheTier::kVolatileTier;

  ImmutableOptions immutable_options(options);

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
  std::shared_ptr<Cache> backing_cache = NewLRUCache(capacity); // Blob file cache

  FileOptions file_options;
  constexpr HistogramImpl* blob_file_read_hist = nullptr;

  BlobSource blob_source(backing_cache.get(), &immutable_options,
                    &file_options, db_id_, db_session_id_, column_family_id,
                    blob_file_read_hist, nullptr /*IOTracer*/);

  ReadOptions read_options;
  read_options.verify_checksums = true;

  constexpr FilePrefetchBuffer* prefetch_buffer = nullptr;

    // GetBlob
    {
      PinnableSlice value;
      uint64_t bytes_read = 0;

      // filling cache = true
      read_options.fill_cache = true;
      for (size_t i = 0; i < num_blobs; ++i) {
        ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size, blob_offsets[i]));

        ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number, blob_offsets[i], file_size,
                                  blob_sizes[i], kNoCompression,
                                  prefetch_buffer, &value, &bytes_read));
        ASSERT_EQ(value, blobs[i]);
        ASSERT_EQ(bytes_read,
                  blob_sizes[i] + keys[i].size() + BlobLogRecord::kHeaderSize);

        ASSERT_TRUE(blob_source.TEST_BlobInCache(blob_file_number, file_size, blob_offsets[i]));
      }

      // Erase the blobs from the cache
      options.blob_cache->EraseUnRefEntries();

      // filling cache = false
      read_options.fill_cache = false;
      for (size_t i = 0; i < num_blobs; ++i) {
        ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size, blob_offsets[i]));

        ASSERT_OK(blob_source.GetBlob(read_options, keys[i], blob_file_number, blob_offsets[i], file_size,
                                  blob_sizes[i], kNoCompression,
                                  prefetch_buffer, &value, &bytes_read));
        ASSERT_EQ(value, blobs[i]);
        ASSERT_EQ(bytes_read,
                  blob_sizes[i] + keys[i].size() + BlobLogRecord::kHeaderSize);

        ASSERT_FALSE(blob_source.TEST_BlobInCache(blob_file_number, file_size, blob_offsets[i]));
      }
    }

    // Cache-only GetBlob
    // {
    //   read_options.read_tier = ReadTier::kBlockCacheTier;
    //   read_options.fill_cache = true;

    //   PinnableSlice value;
    //   uint64_t bytes_read = 0;

    //   for (size_t i = 0; i < num_blobs; ++i) {
    //     ASSERT_FALSE(source->TEST_BlobInCache(blob_offsets[i]));

    //     ASSERT_NOK(source->GetBlob(read_options, keys[i], blob_offsets[i],
    //                                blob_sizes[i], kNoCompression,
    //                                prefetch_buffer, &value, &bytes_read));
    //     ASSERT_TRUE(value.empty());
    //     ASSERT_EQ(bytes_read, 0);

    //     ASSERT_FALSE(source->TEST_BlobInCache(blob_offsets[i]));
    //   }
    // }
}

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
