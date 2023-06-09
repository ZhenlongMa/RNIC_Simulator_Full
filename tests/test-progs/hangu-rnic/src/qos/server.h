#define TRANS_WRDMA_DATA "Hello World!  Hello RDMA Write! Hello World!  Hello RDMA Write!"
#define TRANS_RRDMA_DATA "Hello World!  Hello RDMA Read ! Hello World!  Hello RDMA Read !"

struct perf_record
{
    uint64_t *qp_data_count;
};

struct qp_comm_record
{
    uint64_t *qp_data_count;
    uint64_t *cqe_count;
};

// struct perf_record record;