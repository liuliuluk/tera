// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/db.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "leveldb/cache.h"
#include "leveldb/env.h"
#include "leveldb/table.h"
#include "leveldb/write_batch.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "db/log_format.h"
#include "db/version_set.h"
#include "util/logging.h"
#include "util/testharness.h"
#include "util/testutil.h"
#include "util/string_ext.h"

namespace leveldb {

static const int kValueSize = 1000;

class CorruptionTest {
 public:
  test::ErrorEnv env_;
  std::string dbname_;
  Cache* tiny_cache_;
  Options options_;
  DB* db_;

  CorruptionTest() {
    tiny_cache_ = NewLRUCache(100);
    options_.env = &env_;
    dbname_ = test::TmpDir() + "/db_test";
    DestroyDB(dbname_, options_);

    db_ = NULL;
    Reopen();
  }

  ~CorruptionTest() {
     delete db_;
     DestroyDB(dbname_, Options());
     delete tiny_cache_;
  }

  Status TryReopen(Options* options = NULL) {
    delete db_;
    db_ = NULL;
    Options opt = (options ? *options : options_);
    opt.env = &env_;
    opt.block_cache = tiny_cache_;
    return DB::Open(opt, dbname_, &db_);
  }

  void Reopen(Options* options = NULL) {
    ASSERT_OK(TryReopen(options));
  }

  void RepairDB() {
    delete db_;
    db_ = NULL;
    ASSERT_OK(::leveldb::RepairDB(dbname_, options_));
  }

  void Build(int n) {
    std::string key_space, value_space;
    WriteBatch batch;
    for (int i = 0; i < n; i++) {
      //if ((i % 100) == 0) fprintf(stderr, "@ %d of %d\n", i, n);
      Slice key = Key(i, &key_space);
      batch.Clear();
      batch.Put(key, Value(i, &value_space));
      ASSERT_OK(db_->Write(WriteOptions(), &batch));
    }
  }

  void Check(int min_expected, int max_expected) {
    uint64_t next_expected = 0;
    int missed = 0;
    int bad_keys = 0;
    int bad_values = 0;
    int correct = 0;
    std::string value_space;
    Iterator* iter = db_->NewIterator(ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      uint64_t key;
      Slice in(iter->key());
      if (!ConsumeDecimalNumber(&in, &key) ||
          !in.empty() ||
          key < next_expected) {
        bad_keys++;
        continue;
      }
      missed += (key - next_expected);
      next_expected = key + 1;
      if (iter->value() != Value(key, &value_space)) {
        bad_values++;
      } else {
        correct++;
      }
    }
    delete iter;

    fprintf(stderr,
            "expected=%d..%d; got=%d; bad_keys=%d; bad_values=%d; missed=%d\n",
            min_expected, max_expected, correct, bad_keys, bad_values, missed);
    ASSERT_LE(min_expected, correct);
    ASSERT_GE(max_expected, correct);
  }

  void Corrupt(FileType filetype, int offset, int bytes_to_corrupt, int lg_id = -1) {
    // Pick file to corrupt
    std::string db_path = dbname_;
    if (lg_id >= 0) {
        db_path = dbname_ + "/" + Uint64ToString(lg_id);
    }
    std::vector<std::string> filenames;
    ASSERT_OK(env_.GetChildren(db_path, &filenames));
    uint64_t number;
    FileType type;
    std::string fname;
    int picked_number = -1;
    for (size_t i = 0; i < filenames.size(); i++) {
      if (ParseFileName(filenames[i], &number, &type) &&
          type == filetype &&
          int(number) > picked_number) {  // Pick latest file
        fname = db_path + "/" + filenames[i];
        picked_number = number;
      }
    }
    ASSERT_TRUE(!fname.empty()) << filetype;

    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      const char* msg = strerror(errno);
      ASSERT_TRUE(false) << fname << ": " << msg;
    }

    if (offset < 0) {
      // Relative to end of file; make it absolute
      if (-offset > sbuf.st_size) {
        offset = 0;
      } else {
        offset = sbuf.st_size + offset;
      }
    }
    if (offset > sbuf.st_size) {
      offset = sbuf.st_size;
    }
    if (offset + bytes_to_corrupt > sbuf.st_size) {
      bytes_to_corrupt = sbuf.st_size - offset;
    }

    // Do it
    std::string contents;
    Status s = ReadFileToString(Env::Default(), fname, &contents);
    ASSERT_TRUE(s.ok()) << s.ToString();
    for (int i = 0; i < bytes_to_corrupt; i++) {
      contents[i + offset] ^= 0x80;
    }
    s = WriteStringToFile(Env::Default(), contents, fname);
    ASSERT_TRUE(s.ok()) << s.ToString();
  }

  int Property(const std::string& name) {
    std::string property;
    int result;
    if (db_->GetProperty(name, &property) &&
        sscanf(property.c_str(), "%d", &result) == 1) {
      return result;
    } else {
      return -1;
    }
  }

  // Return the ith key
  Slice Key(int i, std::string* storage) {
    char buf[100];
    snprintf(buf, sizeof(buf), "%016d", i);
    storage->assign(buf, strlen(buf));
    return Slice(*storage);
  }

  // Return the value to associate with the specified key
  Slice Value(int k, std::string* storage) {
    Random r(k);
    return test::RandomString(&r, kValueSize, storage);
  }
};

TEST(CorruptionTest, Recovery) {
  Build(100);
  Check(100, 100);
  Corrupt(kLogFile, 19, 1);      // WriteBatch tag for first record
  Corrupt(kLogFile, log::kBlockSize + 1000, 1);  // Somewhere in second block
  Reopen();

  // The 64 records in the first two log blocks are completely lost.
  // Check(36, 36);
  // But no data will be lost as long as the table is NORMALLY closed.
  Check(100, 100);
}

TEST(CorruptionTest, RecoverWriteError) {
  env_.writable_file_error_ = true;
  Status s = TryReopen();
  ASSERT_TRUE(!s.ok());
}

TEST(CorruptionTest, NewFileErrorDuringWrite) {
  // Do enough writing to force minor compaction
  env_.writable_file_error_ = true;
  const int num = 3 + (Options().write_buffer_size / kValueSize);
  std::string value_storage;
  Status s;
  for (int i = 0; s.ok() && i < num; i++) {
    WriteBatch batch;
    batch.Put("a", Value(100, &value_storage));
    s = db_->Write(WriteOptions(), &batch);
  }
  ASSERT_TRUE(!s.ok());
  ASSERT_GE(env_.num_writable_file_errors_, 1);
  env_.writable_file_error_ = false;
  Reopen();
}

TEST(CorruptionTest, TableFile) {
  Build(100);
  DBTable* dbi = reinterpret_cast<DBTable*>(db_);
  dbi->TEST_CompactMemTable();
  dbi->TEST_CompactRange(0, NULL, NULL);
  dbi->TEST_CompactRange(1, NULL, NULL);

  Corrupt(kTableFile, 100, 1, 0);
  Check(99, 99);
}

TEST(CorruptionTest, TableFileIndexData) {
  Build(10000);  // Enough to build multiple Tables
  DBTable* dbi = reinterpret_cast<DBTable*>(db_);
  dbi->TEST_CompactMemTable();

  Corrupt(kTableFile, -2000, 500, 0);
  Reopen();
  Check(5000, 9999);
}

TEST(CorruptionTest, MissingDescriptor) {
  Build(1000);
  RepairDB();
  Reopen();
  Check(1000, 1000);
}

TEST(CorruptionTest, SequenceNumberRecovery) {
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "v1"));
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "v2"));
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "v3"));
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "v4"));
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "v5"));
  RepairDB();
  Reopen();
  std::string v;
  ASSERT_OK(db_->Get(ReadOptions(), "foo", &v));
  ASSERT_EQ("v5", v);
  // Write something.  If sequence number was not recovered properly,
  // it will be hidden by an earlier write.
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "v6"));
  ASSERT_OK(db_->Get(ReadOptions(), "foo", &v));
  ASSERT_EQ("v6", v);
  Reopen();
  ASSERT_OK(db_->Get(ReadOptions(), "foo", &v));
  ASSERT_EQ("v6", v);
}

TEST(CorruptionTest, CorruptedDescriptor) {
  ASSERT_OK(db_->Put(WriteOptions(), "foo", "hello"));
  DBTable* dbi = reinterpret_cast<DBTable*>(db_);
  dbi->TEST_CompactMemTable();
  dbi->TEST_CompactRange(0, NULL, NULL);

  Corrupt(kDescriptorFile, 0, 1000, 0);
  Status s = TryReopen();
  ASSERT_TRUE(!s.ok());

  RepairDB();
  Reopen();
  std::string v;
  ASSERT_OK(db_->Get(ReadOptions(), "foo", &v));
  ASSERT_EQ("hello", v);
}

TEST(CorruptionTest, CompactionInputError) {
  Build(10);
  DBTable* dbi = reinterpret_cast<DBTable*>(db_);
  dbi->TEST_CompactMemTable();
  const int last = config::kMaxMemCompactLevel;
  ASSERT_EQ(1, Property("leveldb.num-files-at-level" + NumberToString(last)));

  Corrupt(kTableFile, 100, 1, 0);
  Check(9, 9);

  // Force compactions by writing lots of values
  Build(10000);
  Check(10000, 10000);
}

#if 0
TEST(CorruptionTest, CompactionInputErrorParanoid) {
  Options options;
  options.paranoid_checks = true;
  options.write_buffer_size = 1048576;
  Reopen(&options);
  DBTable* dbi = reinterpret_cast<DBTable*>(db_);

  // Fill levels >= 1 so memtable compaction outputs to level 1
  for (int level = 1; level < config::kNumLevels; level++) {
    dbi->Put(WriteOptions(), "", "begin");
    dbi->Put(WriteOptions(), "~", "end");
    Status s = dbi->TEST_CompactMemTable();
    ASSERT_TRUE(s.ok()) << ": status: " << s.ToString();
  }

  Build(10);
  dbi->TEST_CompactMemTable();
  ASSERT_EQ(1, Property("leveldb.num-files-at-level0"));

  Corrupt(kTableFile, 100, 1, 0);
  Check(9, 9);

  // Write must eventually fail because of corrupted table
  Status s;
  std::string tmp1, tmp2;
  for (int i = 0; i < 10000 && s.ok(); i++) {
    s = db_->Put(WriteOptions(), Key(i, &tmp1), Value(i, &tmp2));
  }
  ASSERT_TRUE(!s.ok()) << "write did not fail in corrupted paranoid db";
}
#endif

TEST(CorruptionTest, UnrelatedKeys) {
  Build(10);
  DBTable* dbi = reinterpret_cast<DBTable*>(db_);
  dbi->TEST_CompactMemTable();
  Corrupt(kTableFile, 100, 1, 0);

  std::string tmp1, tmp2;
  ASSERT_OK(db_->Put(WriteOptions(), Key(1000, &tmp1), Value(1000, &tmp2)));
  std::string v;
  ASSERT_OK(db_->Get(ReadOptions(), Key(1000, &tmp1), &v));
  ASSERT_EQ(Value(1000, &tmp2).ToString(), v);
  dbi->TEST_CompactMemTable();
  ASSERT_OK(db_->Get(ReadOptions(), Key(1000, &tmp1), &v));
  ASSERT_EQ(Value(1000, &tmp2).ToString(), v);
}

}  // namespace leveldb

int main(int argc, char** argv) {
  return leveldb::test::RunAllTests();
}