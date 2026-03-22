// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.
//
// S3AlignedFileReader: drop-in replacement for LinuxAlignedFileReader
// that services DiskANN's AlignedRead requests via AWS S3 Range GETs
// instead of local pread() calls.
//
// Drop into:
//   include/s3_aligned_file_reader.h
//
// Usage: pass "s3://bucket-name/path/to/ann_disk.index" to open().

#pragma once

#include "aligned_file_reader.h"

#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <list>
#include <unordered_map>
#include <shared_mutex>

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>

// ─────────────────────────────────────────────────────────────────────────────
// SLRU Cache
//
// Two-segment LRU (probation + protected) matching the paper's design.
// All public methods are thread-safe.
// ─────────────────────────────────────────────────────────────────────────────
class SLRUCache
{
  public:
    // total_bytes  : total cache capacity in bytes
    // protected_ratio : fraction of capacity reserved for protected segment
    //                   (paper uses 0.8, i.e. 80% protected / 20% probation)
    explicit SLRUCache(size_t total_bytes, float protected_ratio = 0.8f);
    ~SLRUCache() = default;

    // Returns true and copies data into buf on a hit.
    // Returns false on a miss (buf is untouched).
    bool get(uint64_t offset, void *buf, size_t len);

    // Insert a block. Evicts from probation (then protected) as needed.
    void put(uint64_t offset, const void *buf, size_t len);

    // Diagnostics
    uint64_t hits() const   { return hits_.load();   }
    uint64_t misses() const { return misses_.load(); }
    size_t   size_bytes() const;

  private:
    struct Block {
        uint64_t           offset;
        std::vector<char>  data;
    };

    // Evict from a segment until free_bytes_ >= needed
    void evict_from(std::list<Block>            &seg,
                    std::unordered_map<uint64_t,
                        std::list<Block>::iterator> &idx,
                    size_t                       &seg_used,
                    size_t                        needed);

    mutable std::shared_mutex mtx_;

    // Probation segment (newly inserted blocks land here)
    std::list<Block>                                    probation_;
    std::unordered_map<uint64_t, std::list<Block>::iterator> prob_idx_;
    size_t prob_cap_;    // capacity in bytes
    size_t prob_used_;   // current usage in bytes

    // Protected segment (blocks promoted after a second hit)
    std::list<Block>                                    protected_;
    std::unordered_map<uint64_t, std::list<Block>::iterator> prot_idx_;
    size_t prot_cap_;
    size_t prot_used_;

    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// S3AlignedFileReader
//
// Implements AlignedFileReader using AWS S3 Range GETs.
// Each AlignedRead { offset, len, buf } becomes:
//   GET s3://<bucket>/<key>  Range: bytes=offset-(offset+len-1)
//
// An optional SLRU cache sits in front of S3 to amortise repeated reads
// of hot index sectors (graph entry point, top-level PQ nodes, etc.).
// ─────────────────────────────────────────────────────────────────────────────
class S3AlignedFileReader : public AlignedFileReader
{
  public:
    // cache_bytes == 0  →  no cache (pure S3 every time)
    // cache_bytes  > 0  →  SLRU cache of that size in front of S3
    explicit S3AlignedFileReader(size_t cache_bytes = 0);
    ~S3AlignedFileReader() override;

    // ── AlignedFileReader interface ──────────────────────────────────────────

    // fname must be "s3://bucket-name/path/to/file"
    // e.g.  "s3://my-bucket/diskANN_index/ann_disk.index"
    void open(const std::string &fname) override;
    void close() override;

    // Each AlignedRead is served either from cache or via S3 Range GET.
    // All reads within one call are issued in parallel (one thread per read,
    // matching the async flavour that LinuxAlignedFileReader emulates).
    void read(std::vector<AlignedRead> &read_reqs,
              IOContext                &ctx,
              bool                      async = false) override;

    // Thread registration: LinuxAlignedFileReader uses these to allocate
    // per-thread io_context_t objects. We still maintain the ctx_map so
    // that callers (pq_flash_index) can call get_ctx() safely, but for
    // S3 IO we do not actually use the io_context_t.
    void register_thread()         override;
    void deregister_thread()       override;
    void deregister_all_threads()  override;
    IOContext &get_ctx()           override;

    // ── Diagnostics ─────────────────────────────────────────────────────────
    void print_stats() const;

  private:
    // Parse "s3://bucket/key/path" → (bucket, key)
    static void parse_s3_url(const std::string &url,
                             std::string       &bucket,
                             std::string       &key);

    // Fetch one AlignedRead from S3 (no cache involvement)
    void s3_range_get(const AlignedRead &req) const;

    // ── AWS state ────────────────────────────────────────────────────────────
    Aws::SDKOptions              sdk_options_;
    bool                         sdk_initialised_{false};
    std::shared_ptr<Aws::S3::S3Client> s3_client_;
    std::string                  bucket_;
    std::string                  key_;

    // ── SLRU cache (nullptr when cache_bytes == 0) ───────────────────────────
    std::unique_ptr<SLRUCache>   cache_;

    // ── Per-thread IOContext map (mirrors LinuxAlignedFileReader) ─────────────
    // ctx_map / ctx_mut are inherited from AlignedFileReader.
    IOContext bad_ctx_;
};
