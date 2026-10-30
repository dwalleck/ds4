/* Model-independent Windows feature boundaries. Link against the CPU core
 * and ds4_gpu_args.c with DS4_NO_GPU; no listeners or GPU runtime are needed. */
#include "../ds4_distributed.h"
#include "../ds4_gpu_args.h"
#include "../ds4_tp.h"

#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#error "test_windows_unsupported is a native Windows test"
#endif

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int unsupported(const char *err) {
    return strstr(err, "not supported on Windows") != NULL;
}

int main(void) {
    char err[256] = "";
    ds4_engine_options engine = {0};
    ds4_dist_options dist = {0};
    CHECK(ds4_dist_prepare_engine_options(&dist, &engine, err, sizeof(err)) == 0);
    CHECK(ds4_tp_validate_engine_options(&engine, err, sizeof(err)) == 1);
    CHECK(ds4_tp_adopt_distributed_options(&engine.tp, &dist, err, sizeof(err)) == 1);

    const ds4_distributed_role dist_roles[] = {
        DS4_DISTRIBUTED_COORDINATOR, DS4_DISTRIBUTED_WORKER
    };
    for (size_t i = 0; i < sizeof(dist_roles) / sizeof(dist_roles[0]); i++) {
        dist = (ds4_dist_options){.role = dist_roles[i]};
        engine = (ds4_engine_options){0};
        CHECK(ds4_dist_prepare_engine_options(&dist, &engine, err, sizeof(err)) != 0);
        CHECK(unsupported(err));
        CHECK(!engine.load_slice && engine.distributed.role == DS4_DISTRIBUTED_NONE);
        /* Validation also runs without an engine in bench/eval argument parsing. */
        CHECK(ds4_dist_prepare_engine_options(&dist, NULL, err, sizeof(err)) != 0);
        CHECK(unsupported(err));
        ds4_dist_session *session = NULL;
        CHECK(ds4_dist_session_create(&session, NULL, &dist, NULL, 0,
                                      err, sizeof(err)) != 0);
        CHECK(session == NULL && unsupported(err));
        CHECK(ds4_dist_run(NULL, &dist, NULL) != 0);
    }

    const ds4_tp_role tp_roles[] = {DS4_TP_LEADER, DS4_TP_WORKER};
    const ds4_tp_transport transports[] = {
        DS4_TP_TRANSPORT_AUTO, DS4_TP_TRANSPORT_TCP, DS4_TP_TRANSPORT_RDMA
    };
    for (size_t i = 0; i < sizeof(tp_roles) / sizeof(tp_roles[0]); i++) {
        for (size_t j = 0; j < sizeof(transports) / sizeof(transports[0]); j++) {
            engine = (ds4_engine_options){0};
            engine.tp.role = tp_roles[i];
            engine.tp.transport = transports[j];
            CHECK(!ds4_tp_validate_engine_options(&engine, err, sizeof(err)));
            CHECK(unsupported(err));
            ds4_tp *tp = NULL;
            CHECK(!ds4_tp_create(&tp, &engine.tp, NULL, err, sizeof(err)));
            CHECK(tp == NULL && unsupported(err));
        }
    }
    CHECK(ds4_tp_worker_run(NULL, NULL) != 0);

    engine = (ds4_engine_options){0};
    engine.tp.requested = true;
    dist = (ds4_dist_options){.role = DS4_DISTRIBUTED_COORDINATOR};
    CHECK(!ds4_tp_adopt_distributed_options(&engine.tp, &dist, err, sizeof(err)));
    CHECK(unsupported(err));
    CHECK(dist.role == DS4_DISTRIBUTED_COORDINATOR && engine.tp.role == DS4_TP_NONE);
    CHECK(!ds4_tp_validate_engine_options(&engine, err, sizeof(err)));
    CHECK(unsupported(err));

    /* Standalone transport flags must not bypass the unsupported diagnostic. */
    engine = (ds4_engine_options){0};
    engine.tp.transport = DS4_TP_TRANSPORT_RDMA;
    CHECK(!ds4_tp_validate_engine_options(&engine, err, sizeof(err)));
    CHECK(unsupported(err));
    engine = (ds4_engine_options){0};
    engine.tp.rdma_device = "unused";
    CHECK(!ds4_tp_validate_engine_options(&engine, err, sizeof(err)));
    CHECK(unsupported(err));
    engine = (ds4_engine_options){0};
    engine.tp.rdma_gid_index_set = true;
    CHECK(!ds4_tp_validate_engine_options(&engine, err, sizeof(err)));
    CHECK(unsupported(err));
    engine = (ds4_engine_options){0};
    engine.cuda_tensor_parallel = true;
    CHECK(!ds4_tp_validate_engine_options(&engine, err, sizeof(err)));
    CHECK(unsupported(err));

    ds4_gpu_config gpu = {0};
    bool skip = false;
    CHECK(parse_gpu_vram_arg("40,12", NULL, &gpu, &skip, err, sizeof(err)) != 0);
    CHECK(gpu.n_gpus == 0 && unsupported(err));
    CHECK(parse_gpu_vram_arg("auto", "0,1", &gpu, &skip, err, sizeof(err)) != 0);
    CHECK(gpu.n_gpus == 0 && unsupported(err));
    CHECK(parse_gpu_vram_arg(NULL, "0,1", &gpu, &skip, err, sizeof(err)) != 0);
    CHECK(gpu.n_gpus == 0 && unsupported(err));
    CHECK(parse_gpu_vram_arg("40", "2", &gpu, &skip, err, sizeof(err)) == 0);
    CHECK(!skip && gpu.n_gpus == 1 && gpu.device_indices[0] == 2);
    CHECK(gpu.vram_bytes[0] == (size_t)40 * 1024 * 1024 * 1024);
    CHECK(parse_gpu_vram_arg("0", NULL, &gpu, &skip, err, sizeof(err)) == 0);
    CHECK(skip && gpu.n_gpus == 0);

    puts("test_windows_unsupported: all tests passed");
    return 0;
}
