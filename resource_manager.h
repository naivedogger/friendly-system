#ifndef SDS_RESOURCE_MANAGER_H
#define SDS_RESOURCE_MANAGER_H

#include <map>
#include <queue>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <infiniband/verbs.h>
#define RED "\033[1;31m"
#define DEFAULT "\033[0m"

#define CACHE_SIZE 4096

#ifndef NDEBUG
#define SDS_INFO(format...)                                                                     \
        do {                                                                                    \
            char __buf[1024];                                                                   \
            snprintf(__buf, 1024, format);                                                      \
            fprintf(stderr, "[I] %s:%d: %s(): %s\n", __FILE__, __LINE__, __func__, __buf);      \
        } while (0)
#else
#define SDS_INFO(format...)
#endif

#define SDS_WARN(format...)                                                                     \
        do {                                                                                    \
            char __buf[1024];                                                                   \
            snprintf(__buf, 1024, format);                                                      \
            fprintf(stderr, RED "[W] %s:%d: %s(): %s\n" DEFAULT, __FILE__, __LINE__, __func__, __buf);    \
        } while (0)

#define SDS_PERROR(str)                                                                         \
        do {                                                                                    \
            fprintf(stderr, RED "[E] %s:%d: %s(): %s: %s\n" DEFAULT, __FILE__, __LINE__, __func__, str, strerror(errno)); \
        } while (0)

using node_t = uint8_t;
using mr_id_t = uint8_t;
const static int kMemoryRegions = 2;
const static mr_id_t MAIN_MEMORY_MR_ID = 0;
const static mr_id_t DEVICE_MEMORY_MR_ID = 1;
const static int MR_FULL_PERMISSION = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
            IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;

struct QueuePair {
            ibv_qp *qp;
            int class_id;
            QueuePair *next;

        public:
            QueuePair(ibv_qp *qp) : qp(qp), next(nullptr), class_id(-1) {}

            ~QueuePair() {
                if (qp) {
                    ibv_destroy_qp(qp);
                    qp = nullptr;
        }
    }
};

struct MemoryRegionMeta {
    uintptr_t addr;
    size_t length;
    uint32_t rkey;
    int valid;
};

struct RemoteNode {
    QueuePair **qp_list;
    MemoryRegionMeta peer_mr_list[kMemoryRegions];
    int local_node_id;                  // index of the node_list array
    int peer_node_id;                   // my node id in the remote side, useful in RPC
    int qp_size;                        // sizeof qp_list

public:
    RemoteNode() : qp_list(nullptr), qp_size(0), local_node_id(-1), peer_node_id(-1) {}

    ~RemoteNode() {
        delete[]qp_list;
    }
};

struct CompQueue {
            ibv_cq *cq;

            CompQueue(ibv_cq *cq) : cq(cq) {}

            ~CompQueue() {
                if (cq) {
                    ibv_destroy_cq(cq);
                    cq = nullptr;
                }
            }
        };

struct Resource {
            std::vector<QueuePair *> qp_list;
            std::vector<CompQueue *> cq_list;
            std::vector<QueuePair *> free_list; // size == max_class_id
            std::vector<ibv_srq *> srq_list;
            std::map<uint64_t, int> class_id_map;
            int next_class_id = 0;
        };

struct ibv_context *ib_ctx_;
uint16_t ib_lid_;
struct ibv_pd *ib_pd_;

struct ibv_mr *mr_list_[kMemoryRegions];
struct ibv_dm *dm_list_[kMemoryRegions];
uint8_t ib_port_;
RemoteNode *node_list_;
Resource resource_;
ibv_port_attr port_attr_;
ibv_device_attr device_attr_;
void *cache_;
size_t cache_size_;

#endif //SDS_RESOURCE_MANAGER_H