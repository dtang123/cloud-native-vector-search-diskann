# Step 1: Build Executables
I assume you have awssdk already installed. Link awssdk path using cmake
```
git clone {THIS REPO}
# cd into repo
mkdir -p build
cd build
cmake -DAWSSDK_ROOT=/path/to/awssdk ..
make build_disk_index
make search_disk_index
```
# Step 2: Build Index
R: max number of neighbors in graph
L: candidate list size when building
QD: Quantized dimension for product quantization (PQ)
B: bits per subspace (PQ param)
M: number of subspaces (PQ param)
Additional parameters may be available. Refer to microsoft DiskANN repo or source code.
```
./build/apps/build_disk_index --data_type float --dist_fn l2 --data_path ~/GIST1M/gist_base.bin --index_path_prefix ~/diskANN_index_v2/ann -R 64 -L 128 --QD 120 -B 0.25 -M 6.0 > ~/diskann_build.log 2>&1
```
# Step 3:  Minio Setup
I assume you have installed mc. 
I have provided the docker-compose.yml file to create the Minio container in root directory. This will mimic an S3 bucket, allowing us to request from it using aws\_sdk. Use below command to start up bucket.
```
docker compose up -d
```
# Step 4: Copy index file into MinIO
```
~/mc cp /path/to/ann_disk.index minio/warehouse/diskann/ann_disk.index
```
# Step 5: Setup Script
Fill out local index file paths in setup\_diskann.sh which I have provided. Copies index metadata to ramdisk and verifies Minio
# Step 6: Setup traffic control to mimic cloud native indexing
This will add latency to the requests to MinIO. This will mimic cloud native indexing
```
sudo tc qdisc add dev lo root netem delay 31ms 20ms distribution normal
```
# Step 7: Run Experiment Script
Runs index search. Can customize caching, threads, search len, experimental runs to your liking.
```
./exp_diskann.sh
```
