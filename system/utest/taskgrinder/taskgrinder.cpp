// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <inttypes.h>

#include <magenta/compiler.h>
#include <magenta/status.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/object.h>
#include <magenta/types.h>
#include <mx/handle.h>
#include <mx/job.h>
#include <mxtl/auto_lock.h>
#include <mxtl/mutex.h>

#include "vector.h"

/*
threads creating and removing children (by closing final handles)
thread walking children, getting handles, calling INFO on them, closing them

handle to deep leaf job, none in between, let the whole thing collapse

hard part is seeing if we actually hit any corner cases
*/

#define RETURN_IF_ERROR(x)                                             \
    do {                                                               \
        mx_status_t TRY_status__ = (x);                                \
        if (TRY_status__ != MX_OK) {                                   \
            fprintf(stderr, "%s:%d: %s failed: %s (%d)\n",             \
                    __func__, __LINE__, #x,                            \
                    mx_status_get_string(TRY_status__), TRY_status__); \
            return TRY_status__;                                       \
        }                                                              \
    } while (0)

typedef Vector<mx::handle> HandleVector;

bool is_good_handle(mx_handle_t h) {
    mx_info_handle_basic_t info;
    mx_status_t s = mx_object_get_info(h, MX_INFO_HANDLE_BASIC,
                                       &info, sizeof(info), nullptr, nullptr);
    return s == MX_OK;
}

void tvtest() {
    static constexpr size_t kNumHandles = 16;
    mx_handle_t raw_handles[kNumHandles];
    {
        Vector<mx::handle> handles;
        for (size_t i = 0; i < kNumHandles; i++) {
            mx::handle h;
            mx_status_t s = mx_event_create(0u, h.reset_and_get_address());
            if (s != MX_OK) {
                fprintf(stderr, "Can't create event %zu: %d\n", i, s);
                return;
            }
            raw_handles[i] = h.get();
            handles.push_back(mxtl::move(h));
            MX_DEBUG_ASSERT(!h.is_valid());
            MX_DEBUG_ASSERT(handles[handles.size() - 1].get() == raw_handles[i]);
        }

        for (const auto& h : handles) {
            MX_DEBUG_ASSERT(is_good_handle(h.get()));
            printf("Good: %" PRIu32 "\n", h.get());
        }
    }

    for (size_t i = 0; i < kNumHandles; i++) {
        MX_DEBUG_ASSERT(!is_good_handle(raw_handles[i]));
        printf("Bad: %" PRIu32 "\n", raw_handles[i]);
    }
    printf("*** ok ***\n");
}

// Creates child jobs until it hits the bottom, closing intermediate handles
// along the way.
mx_status_t create_max_height_job(mx_handle_t parent_job,
                                  mx::handle* leaf_job) {
    bool first = true;
    mx_handle_t prev_job = parent_job;
    while (true) {
        mx_handle_t child_job;
        mx_status_t s = mx_job_create(prev_job, 0u, &child_job);
        if (s == MX_ERR_OUT_OF_RANGE) {
            // Hit the max job height.
            leaf_job->reset(prev_job);
            return MX_OK;
        }

        if (!first) {
            mx_handle_close(prev_job);
        } else {
            first = false;
        }

        if (s != MX_OK) {
            leaf_job->reset();
            return s;
        }
        //xxx give it a unique name; supply prefix
        mx_object_set_property(child_job, MX_PROP_NAME, "tg-job", 7);
        prev_job = child_job;
    }
}

// Creates some number of jobs under a parent job, pushing their handles
// onto the output vector.
mx_status_t create_child_jobs(mx_handle_t parent_job, size_t n,
                              HandleVector* out_handles) {
    for (size_t i = 0; i < n; i++) {
        mx::handle child;
        RETURN_IF_ERROR(mx_job_create(parent_job, 0u,
                                      child.reset_and_get_address()));
        //xxx give it a unique name; supply prefix
        child.set_property(MX_PROP_NAME, "tg-job", 7);
        out_handles->push_back(mxtl::move(child));
    }
    return MX_OK;
}

// Creates some number of processes under a parent job, pushing their handles
// onto the output vector.
mx_status_t create_child_processes(mx_handle_t parent_job, size_t n,
                                   HandleVector* out_handles) {
    for (size_t i = 0; i < n; i++) {
        mx::handle child;
        mx::handle vmar;
        //xxx give it a unique name; supply prefix
        RETURN_IF_ERROR(mx_process_create(parent_job, "tg-proc", 8, 0u,
                                          child.reset_and_get_address(),
                                          vmar.reset_and_get_address()));
        out_handles->push_back(mxtl::move(child));
        // Let the VMAR handle close.
    }
    return MX_OK;
}

// Creates some number of threads under a parent process, pushing their handles
// onto the output vector.
mx_status_t create_child_threads(mx_handle_t parent_process, size_t n,
                                 HandleVector* out_handles) {
    for (size_t i = 0; i < n; i++) {
        mx::handle child;
        //xxx give it a unique name; supply prefix
        RETURN_IF_ERROR(mx_thread_create(parent_process, "tg-thread", 10,
                                         0u, child.reset_and_get_address()));
        out_handles->push_back(mxtl::move(child));
    }
    return MX_OK;
}

//xxx something that keeps creating children, writing handles to a pool?
//xxx another thing that reads handles out of the pool and closes them?
//xxx watch out for synchronization on that pool serializing things
//xxx   could have a thread grab a bunch of handles and then operate on them
//xxx   on its own

//xxx child-walker function: take this process or job, walk its children;
//  maybe recurse

class HandleRegistry {
public:
    void AddJobs(HandleVector* jobs) {
        if (!jobs->empty()) {
            mxtl::AutoLock al(&jobs_lock_);
            Merge(jobs, &jobs_, &num_jobs_);
        }
    }

    void AddProcesses(HandleVector* processes) {
        if (!processes->empty()) {
            mxtl::AutoLock al(&processes_lock_);
            Merge(processes, &processes_, &num_processes_);
        }
    }

    void AddThreads(HandleVector* threads) {
        if (!threads->empty()) {
            mxtl::AutoLock al(&threads_lock_);
            Merge(threads, &threads_, &num_threads_);
        }
    }

    __attribute__((warn_unused_result)) mx_handle_t ReleaseRandomJob() {
        mxtl::AutoLock al(&jobs_lock_);
        return ReleaseRandomHandle(&jobs_, &num_jobs_);
    }

    __attribute__((warn_unused_result)) mx_handle_t ReleaseRandomProcess() {
        mxtl::AutoLock al(&processes_lock_);
        return ReleaseRandomHandle(&processes_, &num_processes_);
    }

    __attribute__((warn_unused_result)) mx_handle_t ReleaseRandomThread() {
        mxtl::AutoLock al(&threads_lock_);
        return ReleaseRandomHandle(&threads_, &num_threads_);
    }

    __attribute__((warn_unused_result)) mx_handle_t ReleaseRandomTask() {
        size_t total = num_jobs_ + num_processes_ + num_threads_;
        const size_t r = rand() % total;
        if (r < num_jobs_) {
            return ReleaseRandomJob();
        } else if (r < num_jobs_ + num_processes_) {
            return ReleaseRandomProcess();
        } else {
            return ReleaseRandomThread();
        }
        //xxx try another if we picked a list with no handles
    }

    //xxx use atomics
    size_t num_jobs() const { return num_jobs_; }
    size_t num_processes() const { return num_processes_; }
    size_t num_threads() const { return num_threads_; }
    size_t num_tasks() const {
        return num_jobs_ + num_processes_ + num_threads_;
    }

private:
    static void Merge(HandleVector* src, HandleVector* dst, size_t* count) {
        const size_t dst_size = dst->size(); // No holes after this index.
        //xxx use an iterator
        size_t di = 0; // Destination index
        for (auto& sit : *src) {
            if (!sit.is_valid()) {
                continue;
            }
            // Look for a hole in the destination.
            while (di < dst_size && (*dst)[di].is_valid()) {
                di++;
            }
            if (di < dst_size) {
                (*dst)[di] = mxtl::move(sit);
            } else {
                dst->push_back(mxtl::move(sit));
            }
        }
        *count += src->size();
    }

    static __attribute__((warn_unused_result))
    mx_handle_t
    ReleaseRandomHandle(HandleVector* hv, size_t* count) {
        const size_t size = hv->size();
        if (size == 0) {
            return MX_HANDLE_INVALID;
        }
        const size_t start = rand() % size;
        //xxx use an iterator
        for (size_t i = start; i < size; i++) {
            if ((*hv)[i].is_valid()) {
                (*count)--;
                return (*hv)[i].release();
            }
        }
        for (size_t i = 0; i < start; i++) {
            if ((*hv)[i].is_valid()) {
                (*count)--;
                return (*hv)[i].release();
            }
        }
        return MX_HANDLE_INVALID;
    }

    mutable mxtl::Mutex jobs_lock_;
    HandleVector jobs_; // TA_GUARDED(jobs_lock_);
    size_t num_jobs_; // TA_GUARDED(jobs_lock_);

    mutable mxtl::Mutex processes_lock_;
    HandleVector processes_; // TA_GUARDED(processes_lock_);
    size_t num_processes_; // TA_GUARDED(processes_lock_);

    mutable mxtl::Mutex threads_lock_;
    HandleVector threads_; // TA_GUARDED(threads_lock_);
    size_t num_threads_; // TA_GUARDED(threads_lock_);
};

//xxx Pass in as a param
static constexpr size_t kMaxTasks = 1000;

#define MTRACE(args...) printf(args)

mx_status_t mutate(HandleRegistry* registry) {
    size_t total = registry->num_tasks();

    enum {
        OP_ADD,
        OP_DELETE,
    } op_class;

    // Randomly pick between add, mutate, and delete.
    if (total < kMaxTasks / 10) {
        op_class = OP_ADD;
    } else if (total > (9 * kMaxTasks) / 10) {
        op_class = OP_DELETE;
    } else {
        op_class = (rand() % 32) < 16 ? OP_ADD : OP_DELETE;
    }

    enum {
        TARGET_JOB,
        TARGET_PROCESS,
        TARGET_THREAD,
    } op_target;
    const size_t r = rand() % 48;
    if (r < 16) {
        op_target = TARGET_JOB;
    } else if (r < 32) {
        op_target = TARGET_PROCESS;
    } else {
        op_target = TARGET_THREAD;
    }

    // Handles that should go into the registry before we return.
    HandleVector jobs;
    HandleVector processes;
    HandleVector threads;

    switch (op_class) {
    case OP_ADD: {
        const size_t num_children = rand() % 5 + 1;
        switch (op_target) {
        case TARGET_JOB: {
            mx_handle_t parent = registry->ReleaseRandomJob();
            if (parent != MX_HANDLE_INVALID) {
                MTRACE("Create %zu jobs\n", num_children);
                jobs.push_back(mx::handle(parent));
                create_child_jobs(parent, num_children, &jobs);
                //xxx if creation fails with BAD_STATE, the parent
                //xxx is probably dead; don't put it back in the list
            }
            //xxx chance to create super-deep job
        } break;
        case TARGET_PROCESS: {
            mx_handle_t parent = registry->ReleaseRandomJob();
            if (parent != MX_HANDLE_INVALID) {
                MTRACE("Create %zu processes\n", num_children);
                jobs.push_back(mx::handle(parent));
                create_child_processes(parent, num_children, &processes);
            }
        } break;
        case TARGET_THREAD: {
            mx_handle_t parent = registry->ReleaseRandomProcess();
            if (parent != MX_HANDLE_INVALID) {
                MTRACE("Create %zu threads\n", num_children);
                processes.push_back(mx::handle(parent));
                create_child_threads(parent, num_children, &threads);
            }
        } break;
        }
    } break;
    case OP_DELETE: {
        const bool kill = rand() % 32 < 16;
        const bool close = rand() % 32 < 16;
        if (kill || close) {
            mx_handle_t task = registry->ReleaseRandomTask();
            if (task != MX_HANDLE_INVALID) {
                if (kill) {
                    MTRACE("Kill one\n");
                    mx_task_kill(task);
                }
                if (close) {
                    MTRACE("Close one\n");
                    mx_handle_close(task);
                } else {
                    MTRACE("(Close one)\n");
                    //xxx need to figure out the type so we can put it back.
                    mx_handle_close(task);
                }
            }
        }
    } break;
    }

    registry->AddJobs(&jobs);
    registry->AddProcesses(&processes);
    registry->AddThreads(&threads);

    return MX_OK;
}

mx_status_t buildup(const mx::handle& root_job) {
    HandleRegistry registry;
    {
        HandleVector jobs;
        jobs.push_back(mx::handle(root_job.get())); //xxx can't let them delete this
        registry.AddJobs(&jobs);
    }
    for (int i = 0; i < 1000; i++) {
        mutate(&registry);
        if (i > 0 && i % 100 == 0) {
            printf("%d mutations. Press a key:\n", i);
            fgetc(stdin);
        }
    }
    printf("Mutations done. Press a key:\n");
    fgetc(stdin);

    printf("Done.\n");
    return MX_OK;
}

int main(int argc, char** argv) {
    //tvtest();
    mx::handle test_root_job;
    mx_status_t s = mx_job_create(mx_job_default(), 0u,
                                  test_root_job.reset_and_get_address());
    if (s != MX_OK) {
        return s;
    }
    test_root_job.set_property(MX_PROP_NAME, "tg-root", 8);
    return buildup(test_root_job);
}
