/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "igt.h"
#include "igt_sysfs.h"
#include "sw_sync.h"

#include <pthread.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/signal.h>
#include "i915/gem_ring.h"

#define LOCAL_I915_EXEC_BSD_SHIFT	(13)
#define LOCAL_I915_EXEC_BSD_RING1 	(1 << LOCAL_I915_EXEC_BSD_SHIFT)
#define LOCAL_I915_EXEC_BSD_RING2 	(2 << LOCAL_I915_EXEC_BSD_SHIFT)

#define MAX_PRIO LOCAL_I915_CONTEXT_MAX_USER_PRIORITY
#define DEFAULT_PRIO LOCAL_I915_CONTEXT_DEFAULT_PRIORITY
#define MIN_PRIO LOCAL_I915_CONTEXT_MIN_USER_PRIORITY
#define HIGH 1
#define LOW 0
#define WATCHDOG_OVER_THRESHOLD (2070000)
#define WATCHDOG_BELOW_THRESHOLD (1200000)
#define WATCHDOG_THRESHOLD (1200)
#define MAX_ENGINES 5
#define RENDER_CLASS 0
#define VIDEO_DECODE_CLASS 1
#define VIDEO_ENHANCEMENT_CLASS 2
#define COPY_ENGINE_CLASS 3
#define LOCAL_I915_CONTEXT_PARAM_WATCHDOG 0x10

#define GET_RESET_STATS_IOCTL DRM_IOWR(DRM_COMMAND_BASE + 0x32, struct local_drm_i915_reset_stats)
struct local_drm_i915_reset_stats {
	__u32 ctx_id;
	__u32 flags;
	__u32 reset_count;
	__u32 batch_active;
	__u32 batch_pending;
	__u32 pad;
};

const uint64_t timeout_100ms = 100000000LL;
float timedifference_msec(struct timeval t0, struct timeval t1);

struct drm_i915_gem_watchdog_timeout {
	union {
		struct {
			/*
			 * Engine class & instance to be configured or queried.
			 */
			__u16 engine_class;
			__u16 engine_instance;
		};
		/* Index based addressing mode */
		__u32 index;
	};
	/* GPU Engine watchdog resets timeout in us */
	__u32 timeout_us;
};

static void clear_error_state(int fd)
{
	int dir;

	dir = igt_sysfs_open(fd);

	if (dir < 0)
		return;

	/* Any write to the error state clears it */
	igt_sysfs_set(dir, "error", "");
	close(dir);
}

static int context_set_watchdog(int fd, unsigned engine_id,
				 const char *engine_name,
                                 unsigned ctx_id, unsigned threshold)
{
	struct drm_i915_gem_watchdog_timeout engines_threshold[MAX_ENGINES];
	struct drm_i915_gem_context_param arg = {
		.param = LOCAL_I915_CONTEXT_PARAM_WATCHDOG,
		.ctx_id = ctx_id,
		.size = sizeof(engines_threshold),
		.value = (uint64_t)&engines_threshold
	};

	memset(&engines_threshold, 0, sizeof(engines_threshold));

	switch (engine_id & I915_EXEC_RING_MASK) {
	case I915_EXEC_RENDER:
		engines_threshold[0].timeout_us = threshold;
		engines_threshold[0].engine_class = RENDER_CLASS;
		engines_threshold[0].engine_instance = 0;
		break;
	case I915_EXEC_BLT:
		if (__gem_context_get_param(fd, &arg) == -ENODEV)
			return -ENODEV;
		else
			engines_threshold[3].timeout_us = threshold;
			engines_threshold[3].engine_class = COPY_ENGINE_CLASS;
			engines_threshold[3].engine_instance = 0;
		break;
	case I915_EXEC_BSD:
		engines_threshold[1].timeout_us = threshold;
		engines_threshold[1].engine_class = VIDEO_DECODE_CLASS;
		engines_threshold[1].engine_instance = 0;
		break;
	case I915_EXEC_VEBOX:
		engines_threshold[2].timeout_us = threshold;
		engines_threshold[2].engine_class = VIDEO_ENHANCEMENT_CLASS;
		engines_threshold[2].engine_instance = 0;
		break;
	default:
		break;
	}

	gem_context_set_param(fd, &arg);

	return 0;
}

static void batch_buffer_factory(uint32_t fd, uint32_t ctx_id, unsigned exec_id,
				 uint32_t target, uint32_t offset,
				 uint32_t *handle, uint64_t timeout,
				 int *fence, int fence_index)
{
    struct drm_i915_gem_exec_object2 obj[2];
    struct drm_i915_gem_relocation_entry reloc;
    struct drm_i915_gem_execbuffer2 execbuf;
    igt_spin_t *spin = NULL;
    const uint32_t bbe = MI_BATCH_BUFFER_END;
    int i = 0;

    gem_quiescent_gpu(fd);

    memset(&execbuf, 0, sizeof(execbuf));
    memset(&obj, 0, sizeof(obj));
    memset(&reloc, 0, sizeof(reloc));

    execbuf.buffers_ptr = to_user_pointer(obj);

    execbuf.buffer_count = 2;
    execbuf.flags = exec_id | I915_EXEC_FENCE_OUT ;

    obj[0].handle = target;
    obj[1].handle = gem_create(fd, 4096);

    obj[1].relocation_count = 1;
    obj[1].relocs_ptr = to_user_pointer(&reloc);

    reloc.target_handle = obj[0].handle;
    reloc.read_domains = I915_GEM_DOMAIN_COMMAND;
    reloc.write_domain = I915_GEM_DOMAIN_COMMAND;
    reloc.delta = offset * sizeof(uint32_t);

    reloc.offset = i * sizeof(uint32_t);
    gem_write(fd, obj[1].handle, 0, &bbe, sizeof(bbe));

    __sync_synchronize();

    gem_sync(fd, obj[1].handle);
    execbuf.rsvd1 = ctx_id;
    execbuf.rsvd2 = -1;

    spin = igt_spin_new(fd, .dependency = obj[0].handle);
    igt_spin_set_timeout(spin, timeout);
    igt_assert(gem_bo_busy(fd, obj[0].handle));

    gem_execbuf_wr(fd, &execbuf);
    igt_spin_free(fd, spin);

    fence[fence_index] = execbuf.rsvd2 >> 32;

    gem_close(fd, obj[1].handle);
    gem_quiescent_gpu(fd);
}

static void inject_hang(uint32_t fd, unsigned engine_id, uint32_t ctx_id,
			 unsigned flags)
{
	igt_hang_t hang;
	hang = igt_hang_ctx(fd, ctx_id, engine_id, flags);
	gem_sync(fd, hang.spin->handle);
}

/* Test#1: create some work and let it run on all engines */
static void long_batch_test1(int fd, int prio1, int prio2, int reset_ctx1,
			int reset_ctx2, unsigned threshold)
{
	unsigned engine_id = 0;
	unsigned nengine = 0;
	struct intel_execution_engine2 *e_;
	uint32_t ctx[8];
	uint32_t scratch[8];
	uint32_t ctx2[8];
	uint32_t scratch2[8];
	int *fence, i = 0;
	const uint64_t batch_timeout_ms = timeout_100ms * 4;

	fence = (int *)malloc(sizeof(int)*8);
	igt_assert(fence);

	__for_each_physical_engine(fd, e_) {
		engine_id = e_->flags;

		scratch2[nengine] = gem_create(fd, 4096);
		ctx2[nengine] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx2[nengine], prio2);
		batch_buffer_factory(fd, ctx2[nengine], engine_id,
				 scratch2[nengine], 0, NULL,
				 batch_timeout_ms, fence, nengine);

		scratch[nengine] = gem_create(fd, 4096);
		ctx[nengine] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx[nengine], prio1);
		batch_buffer_factory(fd, ctx[nengine], engine_id,
				 scratch[nengine], 0, NULL,
				 batch_timeout_ms, fence, nengine);
		igt_info("Test #1: ctx1/ctx2 --> execute on %s\n",e_->name);
		nengine++;
	}

	for (i = 0; i < nengine; i++) {
		close(fence[i]);
		gem_context_destroy(fd, ctx[i]);
		gem_context_destroy(fd, ctx2[i]);
		gem_close(fd, scratch[i]);
		gem_close(fd, scratch2[i]);
	}
}

float timedifference_msec(struct timeval t0, struct timeval t1)
{
    return (t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f;
}

/* Test#2: submit a long batch and after half of the expected runtime
   submit a higher priority batch and then try to cancel the execution*/
static void long_batch_test2(int fd, int prio1, int prio2, int reset_ctx1,
			int reset_ctx2, unsigned threshold)
{
	unsigned engine_id = 0;
	unsigned nengine = 0;
	struct intel_execution_engine2 *e_;
	uint32_t ctx[8];
	uint32_t scratch[8];
	uint32_t ctx2[8];
	uint32_t scratch2[8];
	int *fence, i = 0;
	unsigned flags = HANG_ALLOW_CAPTURE;
	const uint64_t batch_timeout_ms = timeout_100ms * 4;
	struct timeval t0;
	struct timeval t1;
	float elapsed;

	fence = (int *)malloc(sizeof(int)*8);
	igt_assert(fence);

	nengine = 0;

	__for_each_physical_engine(fd, e_) {
		engine_id = e_->flags;

		scratch2[nengine] = gem_create(fd, 4096);
		ctx2[nengine] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx2[nengine], prio2);
		batch_buffer_factory(fd, ctx2[nengine], engine_id,
				 scratch2[nengine], 0, NULL,
				 batch_timeout_ms, fence, nengine);

		gettimeofday(&t0, 0);
		while(elapsed < 200) {
			gettimeofday(&t1, 0);
			elapsed += timedifference_msec(t0, t1);
		}

		scratch[nengine] = gem_create(fd, 4096);
		ctx[nengine] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx[nengine], prio1);
		batch_buffer_factory(fd, ctx[nengine], engine_id,
				 scratch[nengine], 0, NULL,
				 batch_timeout_ms, fence, nengine);

		if (context_set_watchdog(fd, engine_id, e_->name,
					 ctx[nengine],
					 threshold) == -ENODEV) {
			igt_info("No support for gpu h/w watchdog on %s\n",
			 e_->name);
			goto skip_case;
		}
		clear_error_state(fd);
		inject_hang(fd, engine_id, ctx[nengine], flags);
		/* Now check the engine was reset successfully */
		//igt_assert_eq(sync_fence_status(*fence), -EIO);
		igt_info("Test #2 ctx1/ctx2 --> set watchdog and"
				" cancel ctx2 at half expected run with higher"
				" priority engine: %s, fence status: 0x%x \n",
					e_->name, sync_fence_status(*fence));
skip_case:
		nengine++;
	}

	for (i = 0; i < nengine; i++) {
		close(fence[i]);
		gem_context_destroy(fd, ctx[i]);
		gem_context_destroy(fd, ctx2[i]);
		gem_close(fd, scratch[i]);
		gem_close(fd, scratch2[i]);
	}
}

/* Test#3: create 2 ctx, set a gpu watchdog timeout on both,
   and either execute or cancel */
static void long_batch_test3(int fd, int prio1, int prio2, int reset_ctx1,
			int reset_ctx2, unsigned threshold)
{
	unsigned engine_id = 0;
	unsigned nengine = 0;
	struct intel_execution_engine2 *e_;
	uint32_t ctx[8];
	uint32_t scratch[8];
	uint32_t ctx2[8];
	uint32_t scratch2[8];
	int *fence, i = 0;
	unsigned flags = HANG_ALLOW_CAPTURE;
	const uint64_t batch_timeout_ms = timeout_100ms * 4;

	fence = (int *)malloc(sizeof(int)*8);
	igt_assert(fence);

	nengine = 0;

	__for_each_physical_engine(fd, e_) {
		engine_id = e_->flags;

		scratch2[nengine] = gem_create(fd, 4096);
		ctx2[nengine] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx2[nengine], prio2);
		batch_buffer_factory(fd, ctx2[nengine], engine_id,
				 scratch2[nengine], 0, NULL,
				 batch_timeout_ms, fence, nengine);

		scratch[nengine] = gem_create(fd, 4096);
		ctx[nengine] = gem_context_create(fd);
		gem_context_set_priority(fd, ctx[nengine], prio1);
		batch_buffer_factory(fd, ctx[nengine], engine_id,
				 scratch[nengine], 0, NULL,
				 batch_timeout_ms, fence, nengine);

		if (context_set_watchdog(fd, engine_id, e_->name,
					 ctx2[nengine],
					 threshold) == -ENODEV) {
			igt_info("No support for gpu h/w watchdog on %s\n",
			 e_->name);
			goto skip_case;
		}

		if (reset_ctx2) {
			clear_error_state(fd);
			inject_hang(fd, engine_id, ctx2[nengine], flags);

			/* Now check the engine was reset successfully */
			//igt_assert_eq(sync_fence_status(*fence), -EIO);
			igt_info("Test #3 ctx1/ctx2 --> set watchdog and"
				" cancel ctx2 on %s with fence status: 0x%x \n",
					e_->name, sync_fence_status(*fence));
		}

		context_set_watchdog(fd, engine_id, e_->name,
					 ctx[nengine],
					 threshold);
		if (reset_ctx1) {
			clear_error_state(fd);
			inject_hang(fd, engine_id, ctx[nengine], flags);

			/* Now check the engine was reset successfully */
			//igt_assert_eq(sync_fence_status(*fence), -EIO);
			igt_info("Test #3: ctx1/ctx2 --> set watchdog and"
				" cancel ctx1 on %s with fence status: 0x%x\n",
				e_->name, sync_fence_status(*fence));
		}
skip_case:
		nengine++;
	}

	for (i = 0; i < nengine; i++) {
		close(fence[i]);
		gem_context_destroy(fd, ctx[i]);
		gem_context_destroy(fd, ctx2[i]);
		gem_close(fd, scratch[i]);
		gem_close(fd, scratch2[i]);
	}
}

igt_main
{
	int fd;
	unsigned int i=0;
	struct {
		char *name;
		int prio[2];
		bool reset[2];
		unsigned threshold;
	} tests[] = {
		{"ctx1-exec-ctx2-exec-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {false, false}, WATCHDOG_THRESHOLD},
		{"ctx1-low-prio-exec-ctx2-high-prio-reset-after-half-time-all-engines",
		{MAX_PRIO, MIN_PRIO}, {false, false}, WATCHDOG_THRESHOLD},
		{"ctx1-reset-ctx2-exec-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {true, false}, WATCHDOG_THRESHOLD},
		{"ctx1-reset-ctx2-exec-all-engines-below-threshold",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {true, false},
		 WATCHDOG_BELOW_THRESHOLD},
		{"ctx1-reset-ctx2-exec-all-engines-over-threshold",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {true, false},
		 WATCHDOG_OVER_THRESHOLD},
		{"ctx2-reset-ctx1-exec-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {false, true}, WATCHDOG_THRESHOLD},
		{"ctx2-reset-ctx1-reset-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {true, true}, WATCHDOG_THRESHOLD},
		{"ctx1-high-prio-reset-ctx2-low-prio-exec-all-engines",
		{MAX_PRIO, MIN_PRIO}, {true, false}, WATCHDOG_THRESHOLD},
		{"ctx1-low-prio-reset-ctx2-high-prio-exec-all-engines",
		{MIN_PRIO, MAX_PRIO}, {true, false}, WATCHDOG_THRESHOLD},
		{"ctx1-low-prio-reset-ctx2-high-prio-reset-all-engines",
		{MIN_PRIO, MAX_PRIO}, {true, true}, WATCHDOG_THRESHOLD},
	};

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);
	}

	igt_subtest_group {
		igt_subtest_f("%s", tests[0].name) {
			long_batch_test1( fd, tests[0].prio[0],
					tests[0].prio[1],
					tests[0].reset[0],
					tests[0].reset[1],
					tests[0].threshold);
		}

		igt_subtest_f("%s", tests[1].name) {
			long_batch_test2( fd, tests[1].prio[0],
					tests[1].prio[1],
					tests[1].reset[0],
					tests[1].reset[1],
					tests[1].threshold);
		}

		for(i = 2; i < ARRAY_SIZE(tests); i++) {
			igt_subtest_f("%s", tests[i].name) {
				long_batch_test3( fd, tests[i].prio[0],
						tests[i].prio[1],
						tests[i].reset[0],
						tests[i].reset[1],
						tests[i].threshold);
			}
		}
	}

    igt_fixture {
	close(fd);
    }
}
