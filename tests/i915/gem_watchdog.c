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
#define WATCHDOG_THRESHOLD (100)
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

static unsigned int e2ring(int fd, const struct intel_execution_engine2 *e)
{
    return gem_class_instance_to_eb_flags(fd, e->class, e->instance);
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
		engines_threshold[RENDER_CLASS].timeout_us = threshold;
		break;
	case I915_EXEC_BLT:
		if (__gem_context_get_param(fd, &arg) == -ENODEV)
			return -ENODEV;
		else
			engines_threshold[COPY_ENGINE_CLASS].timeout_us = threshold;
		break;
	case I915_EXEC_BSD:
		engines_threshold[VIDEO_DECODE_CLASS].timeout_us = threshold;
		break;
	case I915_EXEC_VEBOX:
		engines_threshold[VIDEO_ENHANCEMENT_CLASS].timeout_us = threshold;
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

static int gem_reset_stats(int fd, int ctx_id,
			   struct local_drm_i915_reset_stats *rs)
{
	memset(rs, 0, sizeof(*rs));
	rs->ctx_id = ctx_id;
	rs->reset_count = -1;

	igt_assert(drmIoctl(fd, GET_RESET_STATS_IOCTL, rs) == 0);
	igt_assert(rs->reset_count != -1);

	return 0;
}

static void inject_hang(uint32_t fd, unsigned engine_id, uint32_t ctx_id,
			 unsigned flags)
{
	igt_hang_t hang;
	hang = igt_hang_ctx(fd, ctx_id, engine_id, flags);
	gem_sync(fd, hang.spin->handle);
}

static void long_batch(int fd, int prio1, int prio2, int reset_ctx1,
			int reset_ctx2)
{
	unsigned engine_id = 0;
	unsigned nengine = 0;
	const struct intel_execution_engine2 *e;
	struct local_drm_i915_reset_stats rs;
	uint32_t ctx[8];
	uint32_t scratch[8];
	uint32_t ctx2[8];
	uint32_t scratch2[8];
	int *fence, i = 0;
	int active_count = 0;
	unsigned flags = HANG_ALLOW_CAPTURE;
	const uint64_t batch_timeout_ms = timeout_100ms * 4;

	fence = (int *)malloc(sizeof(int)*8);
	igt_assert(fence);

	/* Test#1: create some work and let it run */
	for_each_engine_class_instance(fd, e) {
		engine_id = e2ring(fd, e);

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
		igt_info("Test #1: ctx1 --> execute on %s\n",e->name);
		nengine++;
	}

	for (i = 0; i < nengine; i++) {
		close(fence[i]);
		gem_context_destroy(fd, ctx[i]);
		gem_context_destroy(fd, ctx2[i]);
		gem_close(fd, scratch[i]);
		gem_close(fd, scratch2[i]);
	}

	nengine = 0;

	/* Test#2: create some work, set a gpu watchdog timeout and cancel
	   the execution */
	for_each_engine_class_instance(fd, e) {
		engine_id = e2ring(fd, e);

		/* get current batch_active count */
		igt_assert_eq(gem_reset_stats(fd, 0, &rs), 0);
		active_count = rs.batch_active;
		igt_assert_eq(rs.batch_active, active_count);

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

		if (context_set_watchdog(fd, engine_id, e->name,
					 ctx[nengine],
					 WATCHDOG_THRESHOLD) == -ENODEV) {
			igt_info("No support for gpu h/w watchdog on %s\n",
			 e->name);
			goto skip_case2;
		}
		clear_error_state(fd);
		inject_hang(fd, engine_id, ctx[nengine], flags);
		active_count++;

		/* Now check the engine was reset successfully*/
		//igt_assert_eq(sync_fence_status(*fence), -EIO);
		igt_info("Test #2 --> ctx1 set watchdog and cancel on %s"
			" with fence status: 0x%x \n", e->name,
						 sync_fence_status(*fence));

		/* check batch_active count once again after the reset */
		igt_assert_eq(gem_reset_stats(fd, 0, &rs), 0);
		active_count = rs.batch_active;
		igt_assert_eq(rs.batch_active, active_count);

skip_case2:
		nengine++;
	}

	for (i = 0; i < nengine; i++) {
		close(fence[i]);
		gem_context_destroy(fd, ctx[i]);
		gem_context_destroy(fd, ctx2[i]);
		gem_close(fd, scratch[i]);
		gem_close(fd, scratch2[i]);
	}

	nengine = 0;

	/* Test#3: create 2 ctx, set a gpu watchdog timeout on both,
	   and either reset or execute */
	for_each_engine_class_instance(fd, e) {
		engine_id = e2ring(fd, e);

		/* get current batch_active count */
		igt_assert_eq(gem_reset_stats(fd, 0, &rs), 0);
		active_count = rs.batch_active;
		igt_assert_eq(rs.batch_active, active_count);

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

		if (context_set_watchdog(fd, engine_id, e->name,
					 ctx2[nengine],
					 WATCHDOG_THRESHOLD) == -ENODEV) {
			igt_info("No support for gpu h/w watchdog on %s\n",
			 e->name);
			goto skip_case3;
		}

		if (reset_ctx2) {
			clear_error_state(fd);
			inject_hang(fd, engine_id, ctx2[nengine], flags);
			active_count++;

			/* Now check the engine was reset successfully */
			//igt_assert_eq(sync_fence_status(*fence), -EIO);
			igt_info("Test #3 ctx1/ctx2 --> set watchdog and"
				" cancel ctx2 on %s with fence status: 0x%x \n",
					e->name, sync_fence_status(*fence));

			/* check batch_active count once again after the reset */
			igt_assert_eq(gem_reset_stats(fd, 0, &rs), 0);
			active_count = rs.batch_active;
			igt_assert_eq(rs.batch_active, active_count);
		}

		context_set_watchdog(fd, engine_id, e->name,
					 ctx[nengine],
					 WATCHDOG_THRESHOLD);
		if (reset_ctx1) {
			clear_error_state(fd);
			inject_hang(fd, engine_id, ctx[nengine], flags);
			active_count++;

			/* Now check the engine was reset successfully */
			//igt_assert_eq(sync_fence_status(*fence), -EIO);
			igt_info("Test #3: ctx1/ctx2 --> set watchdog and"
				" cancel ctx1 on %s with fence status: 0x%x\n",
				e->name, sync_fence_status(*fence));

			/* check batch_active count once again after the reset */
			igt_assert_eq(gem_reset_stats(fd, 0, &rs), 0);
			active_count = rs.batch_active;
			igt_assert_eq(rs.batch_active, active_count);
		}
skip_case3:
		nengine++;
	}

	for (i = 0; i < nengine; i++) {
		close(fence[i]);
		gem_context_destroy(fd, ctx[i]);
		gem_context_destroy(fd, ctx2[i]);
		gem_close(fd, scratch[i]);
		gem_close(fd, scratch2[i]);
	}
	nengine = 0;
}

igt_main
{
	int fd;
	unsigned int nengine = 0, i=0;
	unsigned int engine;
	const struct intel_execution_engine2 *e;
	struct {
		char *name;
		int prio[2];
		bool reset[2];
	} tests[] = {
		{"ctx1-execute-ctx2-execute-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {false, false}},
		{"ctx1-reset-ctx2-execute-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {true, false}},
		{"ctx2-reset-ctx1-execute-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {false, true}},
		{"ctx2-reset-ctx1-reset-all-engines",
		{DEFAULT_PRIO, DEFAULT_PRIO}, {true, true}},
		{"ctx1-high-prio-execute-ctx2-low-prio-execute-all-engines",
		{MAX_PRIO, MIN_PRIO}, {false, false}},
		{"ctx1-high-prio-reset-ctx2-low-prio-execute-all-engines",
		{MAX_PRIO, MIN_PRIO}, {true, false}},
		{"ctx1-low-prio-reset-ctx2-high-prio-execute-all-engines",
		{MIN_PRIO, MAX_PRIO}, {true, false}},
		{"ctx1-low-prio-reset-ctx2-high-prio-reset-all-engines",
		{MIN_PRIO, MAX_PRIO}, {true, true}},
	};

	igt_skip_on_simulation();

	igt_fixture {
		fd = drm_open_driver(DRIVER_INTEL);
		igt_require_gem(fd);

		for_each_physical_engine(fd, engine)
			nengine++;
		igt_require(nengine);
	}

	igt_subtest_group {
		for(i = 0; i < ARRAY_SIZE(tests); i++) {
			igt_subtest_f("%s", tests[i].name) {
				long_batch( fd, tests[i].prio[0],
						tests[i].prio[1],
						tests[i].reset[0],
						tests[i].reset[1]);
			}
		}
	}

    igt_fixture {
	close(fd);
    }
}
