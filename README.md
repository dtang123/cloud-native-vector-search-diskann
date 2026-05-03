# Experiment Setup
## Index Build
```
cd build
make build_disk_index
```
```
~/DiskANN/build/apps/build_disk_index --data_type float --dist_fn l2 --data_path ~/GIST1M/gist_base.bin --index_path_prefix ~/diskANN_index_v2/ann -R 64 -L 128 --QD 120 -B 0.25 -M 6.0 > ~/diskann_build.log 2>&1
```
## Minio Setup
I assume you have already set up minio container. If not, look in my SPANN repo.
```
~/mc cp /path/to/ann_disk.index minio/warehouse/diskann/ann_disk.index
```
## Setup Script
Copies index metadata to ramdisk and verifies Minio
```
#!/bin/bash
# =============================================================================
# setup_and_run_diskann.sh
#
# Prepares the environment for cloud-native DiskANN search and runs
# the parameter sweep experiment matching exp_diskann.sh.
#
# Architecture:
#   /dev/shm/diskann_metadata/   (RAM — already tmpfs on EC2, no mount needed)
#   ├── ann_pq_compressed.bin    loaded into process RAM at startup
#   ├── ann_pq_pivots.bin
#   └── ann_disk.index           header only (4KB) in S3/MinIO mode
#
#   MinIO (local) / S3 (cloud)
#   └── warehouse/diskann/ann_disk.index   fetched via Range GETs at query time
#
# Usage:
#   chmod +x setup_and_run_diskann.sh
#   ./setup_and_run_diskann.sh              # MinIO mode (default)
#   USE_LOCAL=1 ./setup_and_run_diskann.sh  # local NVMe baseline
#   USE_S3=1    ./setup_and_run_diskann.sh  # real AWS S3
# =============================================================================

# ── User-configurable variables ───────────────────────────────────────────────

# MinIO defaults — override with env vars to switch to real S3
MINIO_ENDPOINT="${MINIO_ENDPOINT:-http://localhost:19000}"
S3_BUCKET="${S3_BUCKET:-warehouse}"
S3_INDEX_KEY="diskann/ann_disk.index"

DISKANN_ROOT="${HOME}/DiskANN"
LOCAL_INDEX_DIR="${HOME}/diskANN_index"
QUERY_FILE="/hdd_root/tang627/GIST1M/gist_query.bin"
GT_FILE="/hdd_root/tang627/GIST1M/gist_groundtruth.bin"
RESULT_DIR="${HOME}"
RESULTS_FILE="diskann_results.txt"

RAMDISK_MOUNT="/dev/shm/diskann_metadata"

USE_LOCAL="${USE_LOCAL:-0}"
USE_S3="${USE_S3:-0}"          # set to 1 to bypass MinIO and use real AWS S3
DATA_TYPE="${DATA_TYPE:-float}"
DIST_FN="${DIST_FN:-l2}"
RECALL_AT=10

set -euo pipefail

# ── Derived paths ─────────────────────────────────────────────────────────────
SEARCH_BIN="${DISKANN_ROOT}/build/apps/search_disk_index"

# =============================================================================
# STEP 1 — Validate prerequisites
# =============================================================================
echo "[1/5] Checking prerequisites..."

if [[ ! -f "${SEARCH_BIN}" ]]; then
    echo "ERROR: search binary not found: ${SEARCH_BIN}"
    echo "       Run: cd ~/DiskANN/build && make -j"
    exit 1
fi

for f in \
    "${LOCAL_INDEX_DIR}/ann_pq_compressed.bin" \
    "${LOCAL_INDEX_DIR}/ann_pq_pivots.bin"     \
    "${LOCAL_INDEX_DIR}/ann_disk.index"        \
    "${QUERY_FILE}"                            \
    "${GT_FILE}"; do
    if [[ ! -f "${f}" ]]; then
        echo "ERROR: required file not found: ${f}"
        exit 1
    fi
done

# Verify MinIO is reachable (skip check if using real S3 or local mode)
if [[ "${USE_LOCAL}" != "1" && "${USE_S3}" != "1" ]]; then
    echo "    Checking MinIO is reachable at ${MINIO_ENDPOINT}..."
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" \
        "${MINIO_ENDPOINT}/minio/health/live" || echo "000")
    if [[ "${HTTP_CODE}" != "200" ]]; then
        echo "ERROR: MinIO not reachable at ${MINIO_ENDPOINT} (HTTP ${HTTP_CODE})"
        echo "       Run: cd ~/my-lab && docker compose up -d"
        exit 1
    fi
    echo "    MinIO OK (HTTP ${HTTP_CODE})"

	# Verify the index file exists in MinIO
    echo "    Checking index file exists in MinIO..."
    if ! ~/mc stat minio/warehouse/${S3_INDEX_KEY} > /dev/null 2>&1; then
        echo "ERROR: Index not found in MinIO: warehouse/${S3_INDEX_KEY}"
        echo "       Upload it with:"
        echo "       ~/mc cp ${LOCAL_INDEX_DIR}/ann_disk.index minio/warehouse/diskann/ann_disk.index"
        exit 1
    fi
    echo "    Index file found in MinIO ($(~/mc stat minio/warehouse/${S3_INDEX_KEY} | grep Size | awk '{print $3, $4}'))"
fi

echo "    All prerequisites found."

# =============================================================================
# STEP 2 — Copy index metadata files to /dev/shm (RAM)
# =============================================================================
echo "[2/5] Copying index metadata to RAM (${RAMDISK_MOUNT})..."

mkdir -p "${RAMDISK_MOUNT}"

cp "${LOCAL_INDEX_DIR}/ann_pq_compressed.bin" "${RAMDISK_MOUNT}/"
cp "${LOCAL_INDEX_DIR}/ann_pq_pivots.bin"     "${RAMDISK_MOUNT}/"

if [[ "${USE_LOCAL}" == "1" ]]; then
    echo "    LOCAL mode: copying full ann_disk.index to RAM..."
    cp "${LOCAL_INDEX_DIR}/ann_disk.index" "${RAMDISK_MOUNT}/"
else
    echo "    S3/MinIO mode: copying ann_disk.index header only (4KB)..."
    dd if="${LOCAL_INDEX_DIR}/ann_disk.index" \
       of="${RAMDISK_MOUNT}/ann_disk.index"   \
       bs=4096 count=128 status=none
fi

echo "    /dev/shm usage: $(du -sh ${RAMDISK_MOUNT} | cut -f1)"
ls -lh "${RAMDISK_MOUNT}/"

# =============================================================================
# STEP 3 — Set environment variables
# =============================================================================
echo "[3/5] Setting environment variables..."

export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-us-east-2}"
export AWS_MAX_ATTEMPTS="${AWS_MAX_ATTEMPTS:-3}"
export AWS_RETRY_MODE="${AWS_RETRY_MODE:-adaptive}"
export AWS_DEFAULT_S3_MAX_CONNECTIONS="${AWS_DEFAULT_S3_MAX_CONNECTIONS:-32}"

if [[ "${USE_LOCAL}" != "1" && "${USE_S3}" != "1" ]]; then
    # MinIO credentials
    export AWS_ACCESS_KEY_ID="admin"
    export AWS_SECRET_ACCESS_KEY="password"
    export MINIO_ENDPOINT="${MINIO_ENDPOINT}"
    echo "    Mode: MinIO (${MINIO_ENDPOINT})"
elif [[ "${USE_S3}" == "1" ]]; then
    # Real AWS S3 — credentials come from IAM role or ~/.aws/credentials
    unset MINIO_ENDPOINT
    export S3_BUCKET="${S3_BUCKET:-cloud-native-vector-search-tang627}"
    echo "    Mode: Real AWS S3 (bucket: ${S3_BUCKET})"
fi

echo "    AWS_DEFAULT_REGION=${AWS_DEFAULT_REGION}"
echo "    AWS_RETRY_MODE=${AWS_RETRY_MODE}"
echo "    S3_BUCKET=${S3_BUCKET}"
echo "    S3_INDEX_KEY=${S3_INDEX_KEY}"

# =============================================================================
# STEP 4 — Set OpenMP and system environment variables
# =============================================================================
echo "[4/5] Setting system environment variables..."

export OMP_PROC_BIND=close
export OMP_PLACES=cores

CURRENT_LIMIT=$(ulimit -n)
if ulimit -n 65536 2>/dev/null; then
    echo "    Open file limit raised to 65536"
else
    echo "    WARNING: Could not raise open file limit (current=${CURRENT_LIMIT})"
fi

# =============================================================================
# STEP 5 — Run experiment sweep
# =============================================================================
echo "[5/5] Running experiment sweep..."

mkdir -p "${RESULT_DIR}"

if [[ "${USE_LOCAL}" == "1" ]]; then
    INDEX_PREFIX="${RAMDISK_MOUNT}/ann"
    echo "    Mode: LOCAL NVMe baseline"
else
    INDEX_PREFIX="s3://${S3_BUCKET}/diskann/ann"
    echo "    Mode: $([ "${USE_S3}" == "1" ] && echo "Real AWS S3" || echo "MinIO") cloud-native"
    echo "    Index: s3://${S3_BUCKET}/${S3_INDEX_KEY}"
fi

rm -f diskann_results.txt

echo "════════════════════════════════════════════════"
echo "  Mode         : $([ "${USE_LOCAL}" == "1" ] && echo "LOCAL" || ([ "${USE_S3}" == "1" ] && echo "AWS S3" || echo "MinIO"))"
echo "  Endpoint     : $([ "${USE_LOCAL}" == "1" ] && echo "N/A" || ([ "${USE_S3}" == "1" ] && echo "AWS default" || echo "${MINIO_ENDPOINT}"))"
echo "  Index prefix : ${INDEX_PREFIX}"
echo "  Query file   : ${QUERY_FILE}"
echo "  GT file      : ${GT_FILE}"
echo "  Results file : ${RESULTS_FILE}"
echo "════════════════════════════════════════════════"
```
## Experiment Script
```
cd build
make search_disk_index
```
```
# Sweep parameters — matching exp_diskann.sh
for search_len in 10 20 40 80 160; do
    for num_concurrent_queries in 1 4 16; do
        echo "${search_len} ${num_concurrent_queries}" | tee -a "${RESULTS_FILE}"
 
        for trial in 1 2 3 4 5; do
            echo "  search_list=${search_len} threads=${num_concurrent_queries} trial=${trial}"
 
            export OMP_NUM_THREADS="${num_concurrent_queries}"
		"${SEARCH_BIN}"                                               \
    		--data_type   "${DATA_TYPE}"                              \
    		--dist_fn     "${DIST_FN}"                                \
    		--index_path_prefix "s3://${S3_BUCKET}/diskANN_index/ann" \
    		--local_path_prefix "${RAMDISK_MOUNT}/ann"                \
 		--query_file  "${QUERY_FILE}"                             \
    		--gt_file     "${GT_FILE}"                                \
    		--result_path "${RESULT_DIR}/result"                      \
    		--recall_at   "${RECALL_AT}"                              \
    		--search_list "${search_len}"                             \
    		-W 16                                                     \
    		-T "${num_concurrent_queries}"				  \
                &>> diskann_results.txt 
            wait
        done
    done
done

```
# DiskANN

[![DiskANN Main](https://github.com/microsoft/DiskANN/actions/workflows/push-test.yml/badge.svg?branch=main)](https://github.com/microsoft/DiskANN/actions/workflows/push-test.yml)
[![PyPI version](https://img.shields.io/pypi/v/diskannpy.svg)](https://pypi.org/project/diskannpy/)
[![Downloads shield](https://pepy.tech/badge/diskannpy)](https://pepy.tech/project/diskannpy)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

[![DiskANN Paper](https://img.shields.io/badge/Paper-NeurIPS%3A_DiskANN-blue)](https://papers.nips.cc/paper/9527-rand-nsg-fast-accurate-billion-point-nearest-neighbor-search-on-a-single-node.pdf)
[![DiskANN Paper](https://img.shields.io/badge/Paper-Arxiv%3A_Fresh--DiskANN-blue)](https://arxiv.org/abs/2105.09613)
[![DiskANN Paper](https://img.shields.io/badge/Paper-Filtered--DiskANN-blue)](https://harsha-simhadri.org/pubs/Filtered-DiskANN23.pdf)


DiskANN is a suite of scalable, accurate and cost-effective approximate nearest neighbor search algorithms for large-scale vector search that support real-time changes and simple filters.
This code is based on ideas from the [DiskANN](https://papers.nips.cc/paper/9527-rand-nsg-fast-accurate-billion-point-nearest-neighbor-search-on-a-single-node.pdf), [Fresh-DiskANN](https://arxiv.org/abs/2105.09613) and the [Filtered-DiskANN](https://harsha-simhadri.org/pubs/Filtered-DiskANN23.pdf) papers with further improvements. 
This code forked off from [code for NSG](https://github.com/ZJULearning/nsg) algorithm.

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/).
For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or
contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.

See [guidelines](CONTRIBUTING.md) for contributing to this project.

## Linux build:

Install the following packages through apt-get

```bash
sudo apt install make cmake g++ libaio-dev libgoogle-perftools-dev clang-format libboost-all-dev
```

### Install Intel MKL
#### Ubuntu 20.04 or newer
```bash
sudo apt install libmkl-full-dev
```

#### Earlier versions of Ubuntu
Install Intel MKL either by downloading the [oneAPI MKL installer](https://www.intel.com/content/www/us/en/developer/tools/oneapi/onemkl.html) or using [apt](https://software.intel.com/en-us/articles/installing-intel-free-libs-and-python-apt-repo) (we tested with build 2019.4-070 and 2022.1.2.146).

```
# OneAPI MKL Installer
wget https://registrationcenter-download.intel.com/akdlm/irc_nas/18487/l_BaseKit_p_2022.1.2.146.sh
sudo sh l_BaseKit_p_2022.1.2.146.sh -a --components intel.oneapi.lin.mkl.devel --action install --eula accept -s
```

### Build
```bash
mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j 
```

## Windows build:

The Windows version has been tested with Enterprise editions of Visual Studio 2022, 2019 and 2017. It should work with the Community and Professional editions as well without any changes. 

**Prerequisites:**

* CMake 3.15+ (available in VisualStudio 2019+ or from https://cmake.org)
* NuGet.exe (install from https://www.nuget.org/downloads)
    * The build script will use NuGet to get MKL, OpenMP and Boost packages.
* DiskANN git repository checked out together with submodules. To check out submodules after git clone:
```
git submodule init
git submodule update
```

* Environment variables: 
    * [optional] If you would like to override the Boost library listed in windows/packages.config.in, set BOOST_ROOT to your Boost folder.

**Build steps:**
* Open the "x64 Native Tools Command Prompt for VS 2019" (or corresponding version) and change to DiskANN folder
* Create a "build" directory inside it
* Change to the "build" directory and run
```
cmake ..
```
OR for Visual Studio 2017 and earlier:
```
<full-path-to-installed-cmake>\cmake ..
```
**This will create a diskann.sln solution**. Now you can:

- Open it from VisualStudio and build either Release or Debug configuration.
- `<full-path-to-installed-cmake>\cmake --build build`
- Use MSBuild:
```
msbuild.exe diskann.sln /m /nologo /t:Build /p:Configuration="Release" /property:Platform="x64"
```

* This will also build gperftools submodule for libtcmalloc_minimal dependency.
* Generated binaries are stored in the x64/Release or x64/Debug directories.

## Usage:

Please see the following pages on using the compiled code:

- [Commandline interface for building and search SSD based indices](workflows/SSD_index.md)  
- [Commandline interface for building and search in memory indices](workflows/in_memory_index.md) 
- [Commandline examples for using in-memory streaming indices](workflows/dynamic_index.md)
- [Commandline interface for building and search in memory indices with label data and filters](workflows/filtered_in_memory.md)
- [Commandline interface for building and search SSD based indices with label data and filters](workflows/filtered_ssd_index.md)
- [diskannpy - DiskANN as a python extension module](python/README.md)

Please cite this software in your work as:

```
@misc{diskann-github,
   author = {Simhadri, Harsha Vardhan and Krishnaswamy, Ravishankar and Srinivasa, Gopal and Subramanya, Suhas Jayaram and Antonijevic, Andrija and Pryce, Dax and Kaczynski, David and Williams, Shane and Gollapudi, Siddarth and Sivashankar, Varun and Karia, Neel and Singh, Aditi and Jaiswal, Shikhar and Mahapatro, Neelam and Adams, Philip and Tower, Bryan and Patel, Yash}},
   title = {{DiskANN: Graph-structured Indices for Scalable, Fast, Fresh and Filtered Approximate Nearest Neighbor Search}},
   url = {https://github.com/Microsoft/DiskANN},
   version = {0.6.1},
   year = {2023}
}
```
