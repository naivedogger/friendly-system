// 尝试下操控SRQ + 独立QP
#define USE_MLX5DV
#ifdef USE_MLX5DV
#include <infiniband/mlx5dv.h>
#endif

#include <infiniband/verbs.h>
#include <unistd.h>
#include <algorithm>
#include <vector>
#include <map>
#include "resource_manager.h"
#include <sys/mman.h>
#include <iostream>
#include <cassert>

#define MAX_NODES 128
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)

int open_mlx_device(const char *device_name) {
        int num_devices = 0;
        ibv_device **device_list = ibv_get_device_list(&num_devices);
        if (!device_list || num_devices <= 0) {
            return -1;
        }
        if (strlen(device_name) == 0) {
#ifdef USE_MLX5DV
            assert(mlx5dv_is_supported(device_list[0]));
#endif
            ib_ctx_ = ibv_open_device(device_list[0]);
            ibv_free_device_list(device_list);
            return ib_ctx_ ? 0 : -1;
        } else {
            for (int i = 0; i < num_devices; ++i) {
                const char *target_device_name = ibv_get_device_name(device_list[i]);
                if (target_device_name && strcmp(target_device_name, device_name) == 0) {
#ifdef USE_MLX5DV
                    assert(mlx5dv_is_supported(device_list[i]));
#endif
                    ib_ctx_ = ibv_open_device(device_list[i]);
                    if (ib_ctx_) {
                        ibv_free_device_list(device_list);
                        return 0;
                    }
                }
            }
        }
        ibv_free_device_list(device_list);
        return -1;
}

static inline void *mmap_huge_page(size_t capacity) {
        void *addr = (char *) mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
                                   MAP_ANON | MAP_PRIVATE | MAP_HUGETLB | MAP_HUGE_2MB,
                                   -1, 0);
        if (addr == MAP_FAILED) {
            SDS_PERROR("mmap");
            SDS_INFO("Please check if you have enough huge pages to be allocated");
            exit(EXIT_FAILURE);
        }
        return addr;
    }

int register_main_memory(void *addr, size_t length, int perm) {
        ibv_mr *mr = ibv_reg_mr(ib_pd_, addr, length, perm);
        if (!mr) {
            SDS_PERROR("ibv_reg_mr");
            return -1;
        }
        assert(!mr_list_[MAIN_MEMORY_MR_ID]);
        mr_list_[MAIN_MEMORY_MR_ID] = mr;
        return 0;
}

void clear(){
    resource_.free_list.clear();
        for (auto entry: resource_.qp_list) {
            delete entry;
        }
        resource_.qp_list.clear();
        for (auto entry: resource_.cq_list) {
            delete entry;
        }
        resource_.cq_list.clear();
        for (auto srq: resource_.srq_list) {
            ibv_destroy_srq(srq);
        }
        resource_.srq_list.clear();
        delete[]node_list_;
        for (int i = 0; i < kMemoryRegions; ++i) {
            if (mr_list_[i]) {
                ibv_dereg_mr(mr_list_[i]);
            }
            if (dm_list_[i]) {
                ibv_free_dm(dm_list_[i]);
            }
        }
        if (ib_pd_) {
            ibv_dealloc_pd(ib_pd_);
            ib_pd_ = nullptr;
        }
        if (ib_ctx_) {
            ibv_close_device(ib_ctx_);
            ib_ctx_ = nullptr;
        }
}

int main(){
    // 开始初始化，包括ibv_open_device，ibv_query_port，ibv_query_device，ibv_alloc_pd
    // 这一部分的逻辑可以看看smart的resource_manager构造函数（因为Initiator里面有一个RM对象，会调用构造函数）
    std::fill(mr_list_, mr_list_ + kMemoryRegions, nullptr);
    std::fill(dm_list_, dm_list_ + kMemoryRegions, nullptr);
    node_list_ = new RemoteNode[MAX_NODES];
    resource_.free_list.resize(100, nullptr);
    ib_port_ = 1;
    std::string ib_name = "";
    if (open_mlx_device(ib_name.c_str())) {
        SDS_WARN("open mlx5 device failed");
        exit(EXIT_FAILURE);
    }
    if (ibv_query_port(ib_ctx_, ib_port_, &port_attr_)) { // 看上去是在初始化port_attr_
        SDS_PERROR("ibv_query_port");
        exit(EXIT_FAILURE);
    }
    if (ibv_query_device(ib_ctx_, &device_attr_)) {
        SDS_PERROR("ibv_query_device");
        exit(EXIT_FAILURE);
    }
    assert(device_attr_.atomic_cap != IBV_ATOMIC_NONE);
    ib_lid_ = port_attr_.lid;
    ib_pd_ = ibv_alloc_pd(ib_ctx_);
    if (!ib_pd_) {
        SDS_PERROR("ibv_alloc_pd");
        exit(EXIT_FAILURE);
    }
    // 初始化完成
    // 继续执行Initiator的逻辑
    cache_size_ = CACHE_SIZE;
    cache_ = mmap_huge_page(cache_size_);
    if (register_main_memory(cache_, cache_size_, MR_FULL_PERMISSION)) { // 注册main memory
        exit(EXIT_FAILURE);
    }

    // TODO：一方面继续做Initiator，把必要的逻辑搬过来
    // 另一方面，弄好qp_list，并且区分开SRQ和QP
    // 后续可能把这两部分的代码分别再抽象成RM和Initiator两个类

    ibv_srq_init_attr srq_init_attr;
    srq_init_attr.attr.max_wr = 16; // 这个地方选的值是initiator.h的kCapacity
    srq_init_attr.attr.max_sge = 1;
    ibv_srq* srq = ibv_create_srq(ib_pd_, &srq_init_attr); // 感觉这个应该放在server上
    resource_.srq_list.push_back(srq); // 方便后续回收
    assert(srq != nullptr);

    // 创建QP，并且将其关联到上面的SRQ，后续可以把这部分抽象成一个函数
    ibv_cq *cq;
    ibv_qp_init_attr attr;
    ibv_qp *qp;
    int class_id = resource_.next_class_id++;
    // cq = ibv_create_cq(ib_ctx_, config_.max_cqe_size, nullptr, nullptr, 0);
    cq = ibv_create_cq(ib_ctx_, 256, nullptr, nullptr, 0);
    if (!cq) {
        SDS_PERROR("ibv_create_cq");
        return -1;
    }
    resource_.cq_list.push_back(new CompQueue(cq));
    attr.send_cq = cq;
    attr.recv_cq = cq;
    attr.sq_sig_all = false;
    attr.qp_type = IBV_QPT_RC;
    // attr.cap.max_send_wr = attr.cap.max_recv_wr = config_.max_wqe_size;
    attr.cap.max_send_wr = attr.cap.max_recv_wr = 256;
    // attr.cap.max_send_sge = attr.cap.max_recv_sge = config_.max_sge_size;
    attr.cap.max_send_sge = attr.cap.max_recv_sge = 1;
    // attr.cap.max_inline_data = config_.max_inline_data;
    attr.cap.max_inline_data = 64;
    attr.srq = srq; // 这个srq不知道创建时的参数对不对

    qp = ibv_create_qp(ib_pd_, &attr);
    if (!qp) {
        SDS_PERROR("ibv_create_qp");
        return -1;
    }
    auto entry = new QueuePair(qp);
    entry->class_id = class_id;
    entry->next = resource_.free_list[class_id];
    resource_.free_list[class_id] = entry;
    resource_.qp_list.push_back(entry);
    // 设想：在服务器创建了一个QP，并且它关联到了一个SRQ，也就是说这个QP是SQ+SRQ


    // 释放资源
    clear();
    return 0;
}
