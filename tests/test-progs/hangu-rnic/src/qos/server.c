#include "librdma.h"
#include "server.h"

int svr_update_qps(struct rdma_resc *resc) {
    for (int i = 0; i < resc->num_qp * resc->num_rem; ++i) {
        /* Modify Local QP */
        struct ibv_qp *qp = resc->qp[i];
        qp->ctx = resc->ctx;
        qp->flag = 0;
        qp->type = QP_TYPE_RC;
        qp->cq = resc->cq[i % TEST_CQ_NUM];
        qp->snd_wqe_offset = 0;
        qp->rcv_wqe_offset = 0;
        qp->lsubnet.llid = resc->ctx->lid;
        qp->dest_qpn = (cpu_id << RESC_LIM_LOG) + (i / resc->num_rem) + 1;
        qp->snd_psn = 0;
        qp->ack_psn = 0;
        qp->exp_psn = 0;
        qp->dsubnet.dlid = (i % resc->num_rem) + resc->ctx->lid + 1;
        qp->group_id = resc->qos_group[0]->id;
        qp->indicator = BW_QP;
        qp->weight = 2;
        RDMA_PRINT(Server, "svr_update_qps: start modify_qp, dlid %d, src_qp 0x%x, dst_qp 0x%x, cqn 0x%x, i %d\n", 
                qp->dsubnet.dlid, qp->qp_num, qp->dest_qpn, qp->cq->cq_num, i);
        // ibv_modify_qp(resc->ctx, qp);
    }
    ibv_modify_batch_qp(resc->ctx, resc->qp[0], resc->num_qp * resc->num_rem);

    /* update the sum of QP weight in this group */
    int group_qp_total_weight = 0;
    for (int i = 0; i < resc->num_qp * resc->num_rem; i++)
    {
        struct ibv_qp *qp = resc->qp[i];
        group_qp_total_weight += qp->weight;
    }
    resc->qos_group[0]->total_qp_weight = group_qp_total_weight;

    return 0;
}

int svr_update_info(struct rdma_resc *resc) {
    RDMA_PRINT(Server, "Start svr_update_info\n");
    int sum = 0, num;
    struct rdma_cr *cr_info;
    uint16_t *dest_info = (uint16_t *)malloc(sizeof(uint16_t) * resc->num_rem);

    while (sum < resc->num_rem) {
        cr_info = rdma_listen(resc, &num); /* listen connection request from client (QP0) */
        if (num == 0) { /* no cr_info is acquired */
            continue;
        }
        RDMA_PRINT(Server, "svr_update_info: rdma_listen end, Polled %d CR data\n", num);
        
        for (int i = 0; i < num; ++i) {

            /* get remote addr information */
            resc->rinfo[sum].dlid  = cr_info[i].src_lid;
            resc->rinfo[sum].raddr = cr_info[i].raddr;
            resc->rinfo[sum].rkey  = cr_info[i].rkey;
            ++sum;

            /* Generate Connect Request to respond client */
            cr_info[i].flag = CR_TYPE_ACK;
            cr_info[i].raddr = (uintptr_t)resc->mr[0]->addr;
            cr_info[i].rkey  = resc->mr[0]->lkey;
            dest_info[i] = cr_info[i].src_lid;

            RDMA_PRINT(Server, "svr_update_info: sum %d resc_num_rem %d, cr_info[i].raddr %ld cr_info[i].rkey %d dest_info[i] %d\n", 
                    sum, resc->num_rem, cr_info[i].raddr, cr_info[i].rkey, dest_info[i]);
        }
        RDMA_PRINT(Server, "svr_update_info: start rdma_connect, sum %d\n", sum);
        rdma_connect(resc, cr_info, dest_info, num); /* post connection request to client (QP0) */
        free(cr_info);
    }
    free(dest_info);

    /* modify qp in server side */
    svr_update_qps(resc);

    return 0;
}

int svr_fill_mr (struct ibv_mr *mr, uint32_t offset) {

// #define TRANS_WRDMA_DATA "Hello World!  Hello RDMA Write! Hello World!  Hello RDMA Write!"
// #define TRANS_RRDMA_DATA "Hello World!  Hello RDMA Read ! Hello World!  Hello RDMA Read !"
    
    char *string = (char *)(mr->addr + offset);
    memcpy(string, TRANS_WRDMA_DATA, sizeof(TRANS_WRDMA_DATA));

    return 0;
}

int svr_post_send (struct rdma_resc *resc, struct ibv_qp *qp, int wr_num, uint32_t offset, uint8_t op_mode) {

    // RDMA_PRINT(Server, "enter svr_post_send %d\n", wr_num);
    struct ibv_wqe wqe[TEST_WR_NUM];
    struct ibv_mr *local_mr = (resc->mr)[0];
    
    if (op_mode == OPMODE_RDMA_WRITE) {

        for (int i = 0; i < wr_num; ++i) {
            wqe[i].length = sizeof(TRANS_WRDMA_DATA) * 16 * 128;
            wqe[i].mr = local_mr;
            wqe[i].offset = offset;

            /* Add RDMA Write element */
            wqe[i].trans_type = IBV_TYPE_RDMA_WRITE;
            wqe[i].flag       = (i == wr_num - 1) ? WR_FLAG_SIGNALED : 0;
            wqe[i].rdma.raddr = resc->rinfo->raddr + (sizeof(TRANS_WRDMA_DATA) - 1) * i + offset;
            wqe[i].rdma.rkey  = resc->rinfo->rkey;
        }
        
    } else if (op_mode == OPMODE_RDMA_READ) {
        
        for (int i = 0; i < wr_num; ++i) {
            wqe[i].length = sizeof(TRANS_RRDMA_DATA);
            wqe[i].mr = local_mr;
            wqe[i].offset = (sizeof(TRANS_RRDMA_DATA) - 1) * i + offset;

            /* Add RDMA Read element */
            wqe[i].trans_type = IBV_TYPE_RDMA_READ;
            wqe[i].flag       = (i == wr_num - 1) ? WR_FLAG_SIGNALED : 0;
            wqe[i].rdma.raddr = resc->rinfo->raddr + offset;
            wqe[i].rdma.rkey  = resc->rinfo->rkey;
        }
        
    }
    
    ibv_post_send(resc->ctx, wqe, qp, wr_num);

    return 0;
}

static void usage(const char *argv0) {
    printf("Usage:\n");
    printf("  %s            start a client and build connection\n", argv0);
    printf("  %s <host>     connect to server at <host>\n", argv0);
    printf("\n");
    printf("Options:\n");
    printf("  -s, --svr-lid=<lid>               server's lid (default 0x0)\n");
    printf("  -t, --num-client=<num_client>     number of clients (default 1)\n");
    printf("  -c, --cpu-id=<cpu_id>             id of the cpu (default 0)\n");
    printf("  -m, --op-mode=<op_mode>           opcode mode (default 0, which is RDMA Write)\n");
}

double latency_test(struct rdma_resc *resc, int num_qp, uint8_t op_mode) {
    uint64_t start_time, end_time, con_time = 0;
    struct cpl_desc **desc = resc->desc;
    uint8_t polling;
    uint8_t ibv_type[] = {IBV_TYPE_RDMA_WRITE, IBV_TYPE_RDMA_READ};

    for (int i = 0; i < num_client; ++i) {
        for (int j = 0; j < num_qp; ++j) {
            struct ibv_qp *qp = resc->qp[i * num_qp + j];

            start_time = get_time(resc->ctx);
            svr_post_send(resc, resc->qp[i * num_qp + j], LATENCY_WR_NUM, 0, op_mode);

            polling = 1;
            while (polling) {
                int res = ibv_poll_cpl(qp->cq, desc, MAX_CPL_NUM);
                for (int j  = 0; j < res; ++j) {
                    if (desc[j]->trans_type == ibv_type[op_mode]) {
                        polling = 0;
                        end_time = get_time(resc->ctx);
                        break;
                    }
                }
            }
            con_time += (end_time - start_time);
            RDMA_PRINT(Server, "latency_test consume_time %.2lf ns\n", ((end_time - start_time) / 1000.0));
        }
    }

    return ((con_time * 1.0) / (num_qp * num_client * 1000.0));
}

// double throughput_test(struct rdma_resc *resc, uint8_t op_mode, uint32_t offset, uint64_t *start_time, uint64_t *end_time, uint64_t *con_time, uint64_t *snd_cnt) {
//     uint8_t ibv_type[] = {IBV_TYPE_RDMA_WRITE, IBV_TYPE_RDMA_READ};
//     int num_qp = resc->num_qp;
//     int num_cq = resc->num_cq;

//     uint64_t *qp_data_cnt = (uint64_t *)malloc(sizeof(uint64_t) * num_qp * num_client);
//     memset(qp_data_cnt, 0, sizeof(uint64_t) * num_qp * num_client);
    
//     struct qp_comm_record record;
//     record.qp_data_count = (uint64_t *)malloc(sizeof(uint64_t) * (num_qp * num_client + 1));
//     record.cqe_count = (uint64_t *)malloc(sizeof(uint64_t) * (num_qp * num_client + 1));
//     memset(record.qp_data_count, 0, sizeof(uint64_t) * (num_qp * num_client + 1));
//     memset(record.cqe_count, 0, sizeof(uint64_t) * (num_qp * num_client + 1));
//     int wr_num = 5;
    
//     /* Start to post all the QPs */
//     for (int i = 0; i < num_client; ++i) {
//         for (int j = 0; j < num_qp; ++j) {
//             struct ibv_qp *qp = resc->qp[i * num_qp + j];
//             svr_post_send(resc, resc->qp[i * num_qp + j], wr_num, offset, op_mode); // (4096 / num_qp) * j
//         }
//     }
    
//     /* polling for completion */
//     struct cpl_desc **desc = resc->desc;
//     do { // snd_cnt < (num_qp * TEST_WR_NUM * num_client)
//         for (int i = 0; i < num_cq; ++i) {
//             int res = ibv_poll_cpl(resc->cq[i], desc, MAX_CPL_NUM);

//             if (res) {
//                 if (*start_time == 0) {
//                     *start_time = get_time(resc->ctx);
//                 }
//                 for (int j = 0; j < res; ++j) {
//                     if (desc[j]->trans_type == ibv_type[op_mode]) {
//                         *snd_cnt += 1;
//                         record.cqe_count[desc[j]->qp_num]++;
//                         if (record.cqe_count[desc[j]->qp_num] % wr_num == 0)
//                         {
//                             uint32_t qp_ptr = (desc[j]->qp_num & RESC_LIM_MASK) - 1; /* the mapping relation between qpn and qp array */
//                             svr_post_send(resc, resc->qp[qp_ptr], wr_num, offset, op_mode); // (4096 / num_qp) * (qp_ptr % num_qp)
//                         }
//                     }
//                     else
//                     {
//                         fprintf(stderr, "Wrong trans type! trans type: %d, ibv type: %d\n", desc[j]->trans_type, ibv_type[op_mode]);
//                     }
//                 }
//             }
//         }
//         *end_time = get_time(resc->ctx);
//         *con_time = *end_time - *start_time;
//         RDMA_PRINT(Server, "con_time: %ld/%lu\n", *con_time, 40UL * MS);
//     } while ((*con_time < 10UL * MS) || (*start_time == 0));

//     for (int i = 0; i < num_client * num_qp + 1; i++)
//     {
//         // note that this is not indexed by QPN!
//         RDMA_PRINT(Server, "QP[%d] cqe count: %ld\n", i, record.cqe_count[i]);
//     }
//     RDMA_PRINT(Server, "time: %ld\n", *con_time);
//     return (*snd_cnt * 1000000.0) / *con_time; /* message rate */
// }

double throughput_test(struct ibv_context *ctx, struct rdma_resc **grp_resc, uint8_t op_mode, uint32_t offset, uint64_t *start_time, uint64_t *end_time, uint64_t *con_time, uint64_t *snd_cnt) {
    uint8_t ibv_type[] = {IBV_TYPE_RDMA_WRITE, IBV_TYPE_RDMA_READ};
    // int num_qp = resc->num_qp;
    // int num_cq = resc->num_cq;
    int num_qp;
    int qos_group_num = ctx->group_num - 1;
    for (int i = 0; i < qos_group_num; i++)
    {
        num_qp += ctx->qos_group->qp_num;
    }
    
    struct qp_comm_record record;
    record.qp_data_count = (uint64_t *)malloc(sizeof(uint64_t) * (num_qp * num_client));
    record.cqe_count = (uint64_t *)malloc(sizeof(uint64_t) * (num_qp * num_client));
    memset(record.qp_data_count, 0, sizeof(uint64_t) * (num_qp * num_client));
    memset(record.cqe_count, 0, sizeof(uint64_t) * (num_qp * num_client));
    int wr_num = 5;
    
    /* Start to post all the QPs at beginning */
    for (int k = 0; k < qos_group_num; k++) // exclude CM group
    {
        struct rdma_resc *resc = grp_resc[k];
        for (int i = 0; i < num_client; ++i) {
            for (int j = 0; j < resc->num_qp; ++j) {
                struct ibv_qp *qp = resc->qp[i * resc->num_qp + j];
                svr_post_send(resc, resc->qp[i * resc->num_qp + j], wr_num, offset, op_mode); // (4096 / num_qp) * j
            }
        }
    }
    
    /* polling for completion */
    do { // snd_cnt < (num_qp * TEST_WR_NUM * num_client)
        for (int grp_id = 0; grp_id < ctx->group_num - 1; grp_id++)
        {
            struct rdma_resc *resc = grp_resc[grp_id];
            struct cpl_desc **desc = resc->desc;
            int num_cq = resc->num_cq;
            for (int i = 0; i < num_cq; ++i) {
                int res = ibv_poll_cpl(resc->cq[i], desc, MAX_CPL_NUM);
                if (res) {
                    if (*start_time == 0) {
                        *start_time = get_time(resc->ctx);
                    }
                    for (int j = 0; j < res; ++j) {
                        if (desc[j]->trans_type == ibv_type[op_mode]) {
                            *snd_cnt += 1;
                            record.cqe_count[desc[j]->qp_num]++;
                            if (record.cqe_count[desc[j]->qp_num] % wr_num == 0)
                            {
                                uint32_t qp_ptr = (desc[j]->qp_num & RESC_LIM_MASK) - 1; /* the mapping relation between qpn and qp array */
                                svr_post_send(resc, resc->qp[qp_ptr], wr_num, offset, op_mode); // (4096 / num_qp) * (qp_ptr % num_qp)
                            }
                        }
                        else
                        {
                            fprintf(stderr, "Wrong trans type! trans type: %d, ibv type: %d\n", desc[j]->trans_type, ibv_type[op_mode]);
                        }
                    }
                }
            }
        }
        *end_time = get_time(ctx);
        *con_time = *end_time - *start_time;
        RDMA_PRINT(Server, "con_time: %ld/%lu\n", *con_time, 40UL * MS);
    } while ((*con_time < 10UL * MS) || (*start_time == 0));

    for (int i = 0; i < num_client * num_qp; i++)
    {
        // note that this is not indexed by QPN!
        RDMA_PRINT(Server, "QP[%d] cqe count: %ld\n", i, record.cqe_count[i]);
    }
    RDMA_PRINT(Server, "time: %ld\n", *con_time);
    return (*snd_cnt * 1000000.0) / *con_time; /* message rate */
}

struct rdma_resc *set_group_resource(struct ibv_context *ctx, int num_mr, int num_cq, int num_qp, uint16_t llid, int num_rem, int grp_weight)
{
    uint8_t  op_mode = OPMODE_RDMA_WRITE; /* 0: RDMA Write; 1: RDMA Read */
    uint32_t offset;
    struct rdma_resc *resc = rdma_resc_init(ctx, num_mr, num_cq, num_qp, llid, num_client);
    RDMA_PRINT(Server, "group resource initialized!\n");
    struct ibv_qos_group *group = create_comm_group(resc->ctx, grp_weight);
    RDMA_PRINT(Server, "group created! group id: %d, group weight: %d\n", group->id, group->weight);
    resc->qos_group[0] = group;

    /* Connect QPs to client's QP */
    svr_update_info(resc);

    RDMA_PRINT(Server, "Server finishes connection!\n");

    // set_group_granularity(resc);

    /* If this is RDMA WRITE, write data to mr, preparing for server writting */
    if (op_mode == OPMODE_RDMA_WRITE) {
        offset = 0;
        svr_fill_mr(resc->mr[0], offset);
    }
    return resc;
}

int main (int argc, char **argv) {
    uint64_t snd_cnt = 0;
    uint16_t svr_lid = 0;
    uint8_t  op_mode = OPMODE_RDMA_WRITE; /* 0: RDMA Write; 1: RDMA Read */

    num_client = 1;
    cpu_id     = 0;
    sprintf(id_name, "%d", svr_lid);

    
    while (1) {
        int c;

        static struct option long_options[] = {
            { .name = "server-lid",   .has_arg = 1, .val = 's' },
            { .name = "num-client",   .has_arg = 1, .val = 't' },
            { .name = "cpu-id"    ,   .has_arg = 1, .val = 'c' },
            { .name = "op-mode"   ,   .has_arg = 1, .val = 'm' },
            { 0 }
        };

        c = getopt_long(argc, argv, "s:t:c:m:", long_options, NULL);
        if (c == -1)
            break;

        switch (c) {
          case 's':
            if (!sscanf(optarg, "%hd", &svr_lid)) {
                RDMA_PRINT(Server, "Error in svr_lid parser. Exit.\n");
                exit(-1);
            }
            break;

          case 't':
            if (!sscanf(optarg, "%d", &num_client)) {
                RDMA_PRINT(Server, "Error in num client parser. Exit.\n");
                exit(-1);
            }
            break;
          case 'c':
            if (!sscanf(optarg, "%hhd", &cpu_id)) {
                RDMA_PRINT(Server, "Error in cpu id parser. Exit.\n");
                exit(-1);
            }
            break;
          case 'm':
            if (!sscanf(optarg, "%hhd", &op_mode)) {
                RDMA_PRINT(Server, "Error in op mode parser. Exit.\n");
                exit(-1);
            }
            break;

          default:
            usage(argv[0]);
            return 1;
        }
    }

    uint64_t start_time, end_time, con_time;
    double latency, msg_rate, bandwidth;
    uint32_t offset = 0;

    sprintf(id_name, "%d", svr_lid);
    RDMA_PRINT(Server, "llid is %hd\n", svr_lid);
    RDMA_PRINT(Server, "num_client %d\n", num_client);

    int num_mr = 1;
    int num_cq = TEST_CQ_NUM;
    int grp1_num_qp = 2;
    int grp2_num_qp = 1;
    int grp1_weight = 15;
    int grp2_weight = 2;
    struct ibv_context *ib_context = (struct ibv_context *)malloc(sizeof(struct ibv_context));;

    /* device initialization */
    ibv_open_device(ib_context, svr_lid);
    RDMA_PRINT(Server, "ibv_open_device : doorbell address 0x%lx\n", (long int)ib_context->dvr);
    struct rdma_resc *grp1_resc = set_group_resource(ib_context, num_mr, num_cq, grp1_num_qp, svr_lid, num_client, grp1_weight);
    RDMA_PRINT(Server, "group1 resource created!\n");
    struct rdma_resc *grp2_resc = set_group_resource(ib_context, num_mr, num_cq, grp2_num_qp, svr_lid, num_client, grp2_weight);
    RDMA_PRINT(Server, "group2 resource created!\n");
    set_all_granularity(ib_context);

    /* sync to make sure that we could get start */
    rdma_recv_sync(grp1_resc);

    /* Inform other CPUs that we can start the latency test */
    // cpu_sync(ib_context);

    /* Start Latency test */
    // latency = latency_test(resc, num_qp, op_mode);
    RDMA_PRINT(Server, "latency test end!\n");

    
    /* Inform other CPUs that we can start the message rate test */
    cpu_sync(ib_context);
    
    /* Start Post Send */
    start_time = get_time(ib_context);
    RDMA_PRINT(Server, "start rdma_post_send0: %ld\n", start_time);
    start_time = 0;
    struct rdma_resc **grp_resc = (struct rdma_resc**)malloc(sizeof(struct rdma_resc *) * (ib_context->group_num - 1));
    grp_resc[0] = grp1_resc;
    grp_resc[1] = grp2_resc;
    msg_rate = throughput_test(ib_context, grp_resc, op_mode, offset, &start_time, &end_time, &con_time, &snd_cnt);

    if (op_mode == OPMODE_RDMA_WRITE) {
        bandwidth = msg_rate * sizeof(TRANS_WRDMA_DATA);
    } else if (op_mode == OPMODE_RDMA_READ) {
        bandwidth = msg_rate * sizeof(TRANS_RRDMA_DATA);
    }
    RDMA_PRINT(Server, "start time %lu end time %lu consumed time is %lu, send cnt: %lu, bandwidth %.2lf MB/s, msg_rate %.2lf Mops/s, latency %.2lf ns\n", 
            start_time, end_time, con_time, snd_cnt, bandwidth, msg_rate, latency);

    /* Inform Client that Transmission has completed */
    rdma_recv_sync(grp1_resc);

    RDMA_PRINT(Server, "rdma_recv_sync finished!\n");

    /* Inform other CPUs that we can exit */
    cpu_sync(ib_context);

    /* close the fd */
    RDMA_PRINT(Server, "fd : %d\n", ((struct hghca_context*)ib_context->dvr)->fd);
    // close(((struct hghca_context*)resc->ctx->dvr)->fd);
    close(((struct hghca_context*)ib_context->dvr)->fd);
    
    return 0;
}