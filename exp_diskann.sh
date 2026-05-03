source ./setup_diskann.sh
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

