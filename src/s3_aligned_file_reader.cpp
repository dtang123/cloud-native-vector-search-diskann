// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.
//
// S3AlignedFileReader implementation.
// Drop into: src/s3_aligned_file_reader.cpp

#include "s3_aligned_file_reader.h"
#include "utils.h"         // diskann::cout, IS_512_ALIGNED

#include <cassert>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>
#include <iostream>
#include <cstdlib>

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
// Build a boto-style range header string.
// S3 ranges are inclusive on both ends.
inline std::string make_range(uint64_t offset, uint64_t len)
{
    return "bytes=" + std::to_string(offset) + "-" +
           std::to_string(offset + len - 1);
}
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// SLRUCache — Two-segment (probation / protected) LRU
// ─────────────────────────────────────────────────────────────────────────────

SLRUCache::SLRUCache(size_t total_bytes, float protected_ratio)
    : prob_used_(0), prot_used_(0)
{
    // Guard against degenerate ratios
    if (protected_ratio <= 0.0f) protected_ratio = 0.01f;
    if (protected_ratio >= 1.0f) protected_ratio = 0.99f;

    prot_cap_ = static_cast<size_t>(total_bytes * protected_ratio);
    prob_cap_ = total_bytes - prot_cap_;
}

// ── Internal eviction ────────────────────────────────────────────────────────
void SLRUCache::evict_from(std::list<Block> &seg,
                            std::unordered_map<uint64_t,
                                std::list<Block>::iterator> &idx,
                            size_t &seg_used,
                            size_t  needed)
{
    // Evict from the tail (LRU end) until we have enough room.
    while (seg_used + needed > (seg.empty() ? needed : (seg_used + seg.back().data.size()))
           && !seg.empty())
    {
        auto &victim = seg.back();
        seg_used -= victim.data.size();
        idx.erase(victim.offset);
        seg.pop_back();
    }
}

size_t SLRUCache::size_bytes() const
{
    std::shared_lock lk(mtx_);
    return prob_used_ + prot_used_;
}

// ── get ──────────────────────────────────────────────────────────────────────
bool SLRUCache::get(uint64_t offset, void *buf, size_t len)
{
    // Try read lock first (fast path: block is already in protected_)
    {
        std::shared_lock lk(mtx_);

        auto pit = prot_idx_.find(offset);
        if (pit != prot_idx_.end())
        {
            // Hit in protected_ → copy data, move to MRU end (need write lock)
            // We drop shared lock and re-acquire exclusive below.
            // (Double-check after re-lock to handle races.)
        }

        auto bit = prob_idx_.find(offset);
        if (bit == prob_idx_.end() && pit == prot_idx_.end())
        {
            // Definite miss — no need for write lock.
            misses_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    // Possible hit — take exclusive lock and re-check.
    std::unique_lock lk(mtx_);

    // ── Check protected segment ──────────────────────────────────────────────
    auto pit = prot_idx_.find(offset);
    if (pit != prot_idx_.end())
    {
        auto it = pit->second;
        if (it->data.size() == len)
        {
            std::memcpy(buf, it->data.data(), len);
            // Move to MRU end of protected_
            protected_.splice(protected_.begin(), protected_, it);
            hits_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Size mismatch (shouldn't happen in practice): treat as miss.
    }

    // ── Check probation segment ──────────────────────────────────────────────
    auto bit = prob_idx_.find(offset);
    if (bit != prob_idx_.end())
    {
        auto it = bit->second;
        if (it->data.size() == len)
        {
            // Copy data out
            std::memcpy(buf, it->data.data(), len);

            // Promote from probation → protected
            Block promoted = std::move(*it);
            prob_used_ -= promoted.data.size();
            probation_.erase(it);
            prob_idx_.erase(bit);

            // Make room in protected segment if needed
            while (prot_used_ + promoted.data.size() > prot_cap_ &&
                   !protected_.empty())
            {
                // Demote protected tail → probation
                Block &demoted = protected_.back();
                size_t dsz = demoted.data.size();
                // Make room in probation too if needed
                while (prob_used_ + dsz > prob_cap_ && !probation_.empty())
                {
                    prob_used_ -= probation_.back().data.size();
                    prob_idx_.erase(probation_.back().offset);
                    probation_.pop_back();
                }
                prob_used_ += dsz;
                probation_.push_front(std::move(demoted));
                prob_idx_[probation_.front().offset] = probation_.begin();
                prot_used_ -= dsz;
                protected_.pop_back();
                prot_idx_.erase(probation_.front().offset);
            }

            // Insert at MRU end of protected_
            prot_used_ += promoted.data.size();
            protected_.push_front(std::move(promoted));
            prot_idx_[protected_.front().offset] = protected_.begin();

            hits_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }

    misses_.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// ── put ──────────────────────────────────────────────────────────────────────
void SLRUCache::put(uint64_t offset, const void *buf, size_t len)
{
    if (len == 0 || len > prob_cap_) return; // too large to cache

    std::unique_lock lk(mtx_);

    // Skip if already cached (avoid double-insert races)
    if (prob_idx_.count(offset) || prot_idx_.count(offset)) return;

    // Evict from probation tail until there is room
    while (prob_used_ + len > prob_cap_ && !probation_.empty())
    {
        prob_used_ -= probation_.back().data.size();
        prob_idx_.erase(probation_.back().offset);
        probation_.pop_back();
    }

    // Insert new block at MRU end of probation
    Block blk;
    blk.offset = offset;
    blk.data.resize(len);
    std::memcpy(blk.data.data(), buf, len);

    prob_used_ += len;
    probation_.push_front(std::move(blk));
    prob_idx_[offset] = probation_.begin();
}

// ─────────────────────────────────────────────────────────────────────────────
// S3AlignedFileReader
// ─────────────────────────────────────────────────────────────────────────────

S3AlignedFileReader::S3AlignedFileReader(size_t cache_bytes)
{
    // Initialise AWS SDK (idempotent if already initialised elsewhere)
    Aws::InitAPI(sdk_options_);
    sdk_initialised_ = true;

    // Build S3 client — uses the standard credential chain:
    //   1. Environment variables (AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY)
    //   2. ~/.aws/credentials
    //   3. EC2 instance metadata (IAM role) ← works out of the box on EC2
    Aws::Client::ClientConfiguration cfg;
    const char *region_env = std::getenv("AWS_DEFAULT_REGION");
    cfg.region = (region_env != nullptr) ? region_env : "us-east-1";
    // Use path-style addressing for compatibility with all S3-compatible stores
    s3_client_ = std::make_shared<Aws::S3::S3Client>(
        cfg,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
        /*useVirtualAddressing=*/false);

    // Optionally wire up the SLRU cache
    if (cache_bytes > 0)
    {
        cache_ = std::make_unique<SLRUCache>(cache_bytes, 0.8f);
        diskann::cout << "S3AlignedFileReader: SLRU cache enabled ("
                      << cache_bytes / (1024 * 1024) << " MB)" << std::endl;
    }
    else
    {
        diskann::cout << "S3AlignedFileReader: no cache (pure S3 IO)" << std::endl;
    }
}

S3AlignedFileReader::~S3AlignedFileReader()
{
    close();
    if (sdk_initialised_)
    {
        Aws::ShutdownAPI(sdk_options_);
        sdk_initialised_ = false;
    }
}

// ── URL parsing ──────────────────────────────────────────────────────────────
// Parses "s3://bucket-name/path/to/file" into bucket and key.
void S3AlignedFileReader::parse_s3_url(const std::string &url,
                                        std::string       &bucket,
                                        std::string       &key)
{
    const std::string prefix = "s3://";
    if (url.compare(0, prefix.size(), prefix) != 0)
    {
        throw std::invalid_argument(
            "S3AlignedFileReader::open: fname must start with s3://  got: " + url);
    }
    std::string rest = url.substr(prefix.size()); // "bucket/key/path"
    auto slash = rest.find('/');
    if (slash == std::string::npos)
    {
        throw std::invalid_argument(
            "S3AlignedFileReader::open: no key found in URL: " + url);
    }
    bucket = rest.substr(0, slash);
    key    = rest.substr(slash + 1);
}

// ── open / close ─────────────────────────────────────────────────────────────
void S3AlignedFileReader::open(const std::string &fname)
{
    parse_s3_url(fname, bucket_, key_);
    diskann::cout << "S3AlignedFileReader: opened s3://" << bucket_
                  << "/" << key_ << std::endl;
}

void S3AlignedFileReader::close()
{
    bucket_.clear();
    key_.clear();
}

// ── Thread registration (mirrors LinuxAlignedFileReader) ─────────────────────
// We keep a dummy IOContext per thread so that pq_flash_index's calls to
// get_ctx() / register_thread() / deregister_thread() do not crash.
// The actual IO does not use io_context_t at all for S3.

void S3AlignedFileReader::register_thread()
{
    auto id = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(ctx_mut);
    if (ctx_map.find(id) != ctx_map.end()) return; // already registered
    ctx_map[id] = IOContext{};
}

void S3AlignedFileReader::deregister_thread()
{
    auto id = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(ctx_mut);
    ctx_map.erase(id);
}

void S3AlignedFileReader::deregister_all_threads()
{
    std::unique_lock<std::mutex> lk(ctx_mut);
    ctx_map.clear();
}

IOContext &S3AlignedFileReader::get_ctx()
{
    std::unique_lock<std::mutex> lk(ctx_mut);
    if (ctx_map.find(std::this_thread::get_id()) == ctx_map.end())
    {
        std::cerr << "S3AlignedFileReader::get_ctx: unregistered thread" << std::endl;
        return bad_ctx_;
    }
    return ctx_map[std::this_thread::get_id()];  // ← operator[] returns non-const ref
}
// ── Core S3 fetch (single AlignedRead) ───────────────────────────────────────
void S3AlignedFileReader::s3_range_get(const AlignedRead &req) const
{
    assert(!bucket_.empty() && !key_.empty());

    Aws::S3::Model::GetObjectRequest s3_req;
    s3_req.SetBucket(bucket_.c_str());
    s3_req.SetKey(key_.c_str());
    s3_req.SetRange(make_range(req.offset, req.len).c_str());

    auto outcome = s3_client_->GetObject(s3_req);
    if (!outcome.IsSuccess())
    {
        throw std::runtime_error(
            std::string("S3AlignedFileReader: GetObject failed: ") +
            outcome.GetError().GetMessage().c_str());
    }

    auto &body = outcome.GetResultWithOwnership().GetBody();
    body.read(static_cast<char *>(req.buf), static_cast<std::streamsize>(req.len));

    auto n_read = body.gcount();
    if (static_cast<uint64_t>(n_read) != req.len)
    {
        throw std::runtime_error(
            "S3AlignedFileReader: short read: expected " +
            std::to_string(req.len) + " got " + std::to_string(n_read));
    }
}

// ── read (the hot path) ───────────────────────────────────────────────────────
// Each AlignedRead is dispatched in its own thread so that the beam-search
// reads within one hop are issued in parallel — matching the async flavour
// that LinuxAlignedFileReader emulates via libaio.
void S3AlignedFileReader::read(std::vector<AlignedRead> &read_reqs,
                                IOContext                & /*ctx*/,
                                bool                      /*async*/)
{
    if (read_reqs.empty()) return;

    // Lambda executed per-request
    auto serve_one = [&](const AlignedRead &req)
    {
        // ── 512-byte alignment checks (same as LinuxAlignedFileReader) ──────
        assert(IS_512_ALIGNED(req.offset));
        assert(IS_512_ALIGNED(req.len));
        assert(IS_512_ALIGNED(req.buf));

        // ── Cache lookup ────────────────────────────────────────────────────
        if (cache_ && cache_->get(req.offset, req.buf, req.len))
        {
            return; // cache hit — nothing more to do
        }

        // ── S3 fetch ─────────────────────────────────────────────────────────
        s3_range_get(req);

        // ── Populate cache ───────────────────────────────────────────────────
        if (cache_)
        {
            cache_->put(req.offset, req.buf, req.len);
        }
    };

    if (read_reqs.size() == 1)
    {
        // Fast path: avoid thread overhead for single reads
        serve_one(read_reqs[0]);
        return;
    }

    // Parallel path: one thread per read request (mirrors libaio batch)
    std::vector<std::thread> workers;
    workers.reserve(read_reqs.size());

    // Capture exceptions from worker threads
    std::vector<std::exception_ptr> errors(read_reqs.size(), nullptr);

    for (size_t i = 0; i < read_reqs.size(); ++i)
    {
        workers.emplace_back([&, i]()
        {
            try
            {
                serve_one(read_reqs[i]);
            }
            catch (...)
            {
                errors[i] = std::current_exception();
            }
        });
    }

    for (auto &t : workers) t.join();

    // Re-throw the first error encountered (if any)
    for (auto &ep : errors)
    {
        if (ep) std::rethrow_exception(ep);
    }
}

// ── Diagnostics ──────────────────────────────────────────────────────────────
void S3AlignedFileReader::print_stats() const
{
    if (!cache_)
    {
        diskann::cout << "S3AlignedFileReader: cache disabled" << std::endl;
        return;
    }
    uint64_t h = cache_->hits();
    uint64_t m = cache_->misses();
    uint64_t total = h + m;
    float hit_rate = total > 0 ? 100.0f * h / total : 0.0f;
    diskann::cout << "S3AlignedFileReader cache stats: "
                  << "hits=" << h << " misses=" << m
                  << " hit_rate=" << hit_rate << "%"
                  << " size=" << cache_->size_bytes() / (1024 * 1024) << "MB"
                  << std::endl;
}
