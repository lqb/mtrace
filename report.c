/*
 * report events to client
 *
 * This file is part of mtrace-ng.
 * Copyright (C) 2015 Stefani Seibold <stefani@seibold.net>
 *
 * This work was sponsored by Rohde & Schwarz GmbH & Co. KG, Munich/Germany.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "backend.h"
#include "backtrace.h"
#include "common.h"
#include "debug.h"
#include "library.h"
#include "memtrace.h"
#include "options.h"
#include "report.h"
#include "server.h"
#include "task.h"
#include "trace.h"

static int report_alloc64(struct task *task, enum mt_operation op, unsigned long ptr, unsigned long size, unsigned int depth, struct library_symbol *libsym)
{
	unsigned int i = 0;
	struct mt_alloc_payload_64 *alloc = alloca(sizeof(*alloc) + depth * sizeof(uint64_t));

	alloc->ptr = (uint64_t)ptr;
	alloc->size = (uint64_t)size;

	if (depth) {
		if (libsym)
			alloc->data[i++] = libsym->addr;

		if (backtrace_init_unwind(task) >= 0) {
			while(i < depth) {
				if (backtrace_location_type(task) != LIBTYPE_LOADER) {
					alloc->data[i] = (uint64_t)backtrace_get_ip(task);

					if (!i || alloc->data[i - 1] != alloc->data[i]) {
						if (!alloc->data[i])
							break;

						 ++i;
					}
				}

				if (backtrace_step(task) < 0)
					break;
			}
		}
	}

	skip_breakpoint(task, task->event.e_un.breakpoint);

	return server_send_msg(op, task->leader->pid, task->pid, alloc, sizeof(*alloc) + i * sizeof(uint64_t));
}

static int report_alloc32(struct task *task, enum mt_operation op, unsigned long ptr, unsigned long size, int depth, struct library_symbol *libsym)
{
	int i = 0;
	struct mt_alloc_payload_32 *alloc = alloca(sizeof(*alloc) + depth * sizeof(uint32_t));

	alloc->ptr = (uint32_t)ptr;
	alloc->size = (uint32_t)size;

	if (depth) {
		if (libsym)
			alloc->data[i++] = libsym->addr;

		if (backtrace_init_unwind(task) >= 0) {
			while(i < depth) {
				if (backtrace_location_type(task) != LIBTYPE_LOADER) {
					alloc->data[i] = (uint32_t)backtrace_get_ip(task);

					if (!i || alloc->data[i - 1] != alloc->data[i]) {
						if (!alloc->data[i])
							break;

						++i;
					}
				}

				if (backtrace_step(task) < 0)
					break;
			}
		}
	}

	skip_breakpoint(task, task->event.e_un.breakpoint);

	return server_send_msg(op, task->leader->pid, task->pid, alloc, sizeof(*alloc) + i * sizeof(uint32_t));
}

static int report_alloc(struct task *task, enum mt_operation op, unsigned long ptr, unsigned long size, int depth, struct library_symbol *libsym)
{
	if (!ptr)
		return 0;

	if (!server_connected())
		return -1;

	debug(DEBUG_FUNCTION, "%d [%d]: %#lx %lu", op, task->pid, ptr, size);

	if (task_is_64bit(task))
		return report_alloc64(task, op, ptr, size, depth, libsym);
	else
		return report_alloc32(task, op, ptr, size, depth, libsym);
}

static int _null(struct task *task, struct library_symbol *libsym)
{
	return 0;
}

static int _report_alloc_op(struct task *task, struct library_symbol *libsym, enum mt_operation op)
{
	if (!server_connected())
		return -1;

	unsigned long size = fetch_param(task, 0);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, op, ret, size, options.bt_depth, libsym);
}

static int _report_malloc(struct task *task, struct library_symbol *libsym)
{
	return _report_alloc_op(task, libsym, MT_MALLOC);
}

static int _report_new(struct task *task, struct library_symbol *libsym)
{
	return _report_alloc_op(task, libsym, options.sanity ? MT_NEW : MT_MALLOC);
}

static int _report_new_array(struct task *task, struct library_symbol *libsym)
{
	return _report_alloc_op(task, libsym, options.sanity ? MT_NEW_ARRAY : MT_MALLOC);
}

static int _report_free_op(struct task *task, struct library_symbol *libsym, enum mt_operation op)
{
	if (!server_connected())
		return -1;

	unsigned long addr = fetch_param(task, 0);

	return report_alloc(task, op, addr, 0, options.sanity ? options.bt_depth : 0, NULL);
}

static int report_free(struct task *task, struct library_symbol *libsym)
{
	return _report_free_op(task, libsym, MT_FREE);
}

static int report_delete(struct task *task, struct library_symbol *libsym)
{
	return _report_free_op(task, libsym, options.sanity ? MT_DELETE : MT_FREE);
}

static int report_delete_array(struct task *task, struct library_symbol *libsym)
{
	return _report_free_op(task, libsym, options.sanity ? MT_DELETE_ARRAY : MT_FREE);
}

static int _report_realloc(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long addr = fetch_param(task, 0);
	unsigned long size = fetch_param(task, 1);
	unsigned long ret = fetch_retval(task);

	if (ret)
		return report_alloc(task, MT_REALLOC, ret, size, options.bt_depth, libsym);
	else
		return report_alloc(task, MT_REALLOC_FAILED, addr, 1, options.bt_depth, libsym);
}

static int report_realloc(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long addr = fetch_param(task, 0);
	unsigned long size = fetch_param(task, 1);

	return report_alloc(task, MT_REALLOC_ENTER, addr, size, options.bt_depth, libsym);
}

static int _report_calloc(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long size = fetch_param(task, 0) * fetch_param(task, 1);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, MT_MALLOC, ret, size, options.bt_depth, libsym);
}

static int _report_mmap(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long ret = fetch_retval(task);

	if ((void *)ret == MAP_FAILED)
		return 0;

	unsigned long size = fetch_param(task, 1);

	return report_alloc(task, MT_MMAP, ret, size, options.bt_depth, libsym);
}

static int _report_mmap64(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long ret = fetch_retval(task);

	if ((void *)ret == MAP_FAILED)
		return 0;

	union {
		uint64_t l;
		struct {
			uint32_t v1;
			uint32_t v2;
		} v;
	} size;

	size.l = fetch_param(task, 1);
	
	if (!task_is_64bit(task)) {
		size.v.v1 = fetch_param(task, 1);
		size.v.v2 = fetch_param(task, 2);
	}
	else
		size.l = fetch_param(task, 1);

	return report_alloc(task, MT_MMAP64, ret, size.l, options.bt_depth, libsym);
}

static int report_munmap(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long addr = fetch_param(task, 0);
	unsigned long size = fetch_param(task, 1);

	return report_alloc(task, MT_MUNMAP, addr, size, 0, libsym);
}

static int _report_memalign(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long size = fetch_param(task, 1);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, MT_MEMALIGN, ret, size, options.bt_depth, libsym);
}

static int _report_posix_memalign(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long ret = fetch_retval(task);

	if (ret)
		return 0;

	unsigned long size = fetch_param(task, 2);
	unsigned long ptr = fetch_param(task, 0);
	unsigned long new_ptr;

	if (task_is_64bit(task))
		copy_from_proc(task, ARCH_ADDR_T(ptr), &new_ptr, sizeof(new_ptr));
	else {
		uint32_t tmp;

		copy_from_proc(task, ARCH_ADDR_T(ptr), &tmp, sizeof(tmp));

		new_ptr = tmp;
	}

	return report_alloc(task, MT_POSIX_MEMALIGN, new_ptr, size, options.bt_depth, libsym);
}

static int _report_aligned_alloc(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long size = fetch_param(task, 1);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, MT_ALIGNED_ALLOC, ret, size, options.bt_depth, libsym);
}

static int _report_valloc(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long size = fetch_param(task, 0);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, MT_VALLOC, ret, size, options.bt_depth, libsym);
}

static int _report_pvalloc(struct task *task, struct library_symbol *libsym)
{
	if (!server_connected())
		return -1;

	unsigned long size = fetch_param(task, 0);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, MT_PVALLOC, ret, size, options.bt_depth, libsym);
}

static int report_mremap(struct task *task, struct library_symbol *libsym)
{
	unsigned long addr = fetch_param(task, 0);
	unsigned long size = fetch_param(task, 1);

	return report_alloc(task, MT_MUNMAP, addr, size, 0, libsym);
}

static int _report_mremap(struct task *task, struct library_symbol *libsym)
{
	unsigned long size = fetch_param(task, 2);
	unsigned long ret = fetch_retval(task);

	return report_alloc(task, MT_MMAP, ret, size, options.bt_depth, libsym);
}

static const struct function flist[] = {
	{ "malloc",						"malloc",		0,	NULL,			_report_malloc },
	{ "free",						"free",			0,	report_free,		NULL },
	{ "realloc",						"realloc",		0,	report_realloc,		_report_realloc },
	{ "calloc",						"calloc",		0,	NULL,			_report_calloc },
	{ "posix_memalign",					"posix_memalign",	0,	NULL,			_report_posix_memalign },
	{ "mmap",						"mmap",			0,	NULL,			_report_mmap },
	{ "mmap64",						"mmap64",		1,	NULL,			_report_mmap64 },
	{ "munmap",						"munmap",		0,	report_munmap,		_null },
	{ "memalign",						"memalign",		0,	NULL,			_report_memalign },
	{ "aligned_alloc",					"aligned_alloc",	1,	NULL,			_report_aligned_alloc },
	{ "valloc",						"valloc",		1,	NULL,			_report_valloc },
	{ "pvalloc",						"pvalloc",		1,	NULL,			_report_pvalloc },
	{ "mremap",						"mremap",		0,	report_mremap,		_report_mremap },
	{ "cfree",						"cfree",		1,	report_free,		NULL },

	{ "new(unsigned int)",					"_Znwj",		1,	NULL,			_report_new },
	{ "new[](unsigned int)",				"_Znaj",		1,	NULL,			_report_new_array },
	{ "new(unsigned int, std::nothrow_t const&)",		"_ZnwjRKSt9nothrow_t",	1,	NULL,			_report_new },
	{ "new[](unsigned int, std::nothrow_t const&)",		"_ZnajRKSt9nothrow_t",	1,	NULL,			_report_new_array },

	{ "new(unsigned long)",					"_Znwm",		1,	NULL,			_report_new },
	{ "new[](unsigned long)",				"_Znam",		1,	NULL,			_report_new_array },
	{ "new(unsigned long, std::nothrow_t const&)",		"_ZnwmRKSt9nothrow_t",	1,	NULL,			_report_new },
	{ "new[](unsigned long, std::nothrow_t const&)",	"_ZnamRKSt9nothrow_t",	1,	NULL,			_report_new_array },

	{ "delete(void*)",					"_ZdlPv",		1,	report_delete,		NULL },
	{ "delete[](void*)",					"_ZdaPv",		1,	report_delete_array,	NULL },
	{ "delete(void*, std::nothrow_t const&)",		"_ZdlPvRKSt9nothrow_t",	1,	report_delete,		NULL },
	{ "delete[](void*, std::nothrow_t const&)",		"_ZdaPvRKSt9nothrow_t",	1,	report_delete_array,	NULL },
};

const struct function *flist_matches_symbol(const char *sym_name)
{
	unsigned int i;

	for(i = 0; i < ARRAY_SIZE(flist); ++i) {
		if (options.nocpp && flist[i].name[0] == '_')
			continue;

		if (!strcmp(sym_name, flist[i].name))
			return &flist[i];
	}
	return 0;
}

int _report_map(struct task *task, struct library *lib, enum mt_operation op)
{
	struct libref *libref = lib->libref;
	size_t len = strlen(libref->filename) + 1;
	struct mt_map_payload *payload = alloca(sizeof(struct mt_map_payload) + len);
	payload->addr = libref->load_addr;
	payload->offset = libref->load_offset;
	payload->size = libref->load_size;

	memcpy(payload->filename, libref->filename, len);

	return server_send_msg(op, task->pid, 0, payload, sizeof(struct mt_map_payload) + len);
}

int report_add_map(struct task *task, struct library *lib)
{
	if (!server_connected())
		return -1;

	return _report_map(task, lib, MT_ADD_MAP);
}

int report_del_map(struct task *task, struct library *lib)
{
	if (!server_connected())
		return -1;

	return _report_map(task, lib, MT_DEL_MAP);
}

int report_info(int do_trace)
{
	struct memtrace_info mt_info;

	if (!server_connected())
		return -1;

	mt_info.version = MEMTRACE_SI_VERSION;
	mt_info.mode = 0;
	mt_info.do_trace = do_trace ? 1 : 0;
	mt_info.stack_depth = options.bt_depth;

	if (options.verbose)
		mt_info.mode |= MEMTRACE_SI_VERBOSE;

	if (options.follow_exec)
		mt_info.mode |= MEMTRACE_SI_EXEC;

	if (options.follow)
		mt_info.mode |= MEMTRACE_SI_FORK;

	return server_send_msg(MT_INFO, 0, 0, &mt_info, sizeof(mt_info));
}

int report_scan(pid_t pid, const void *data, unsigned int data_len)
{
	if (!server_connected())
		return -1;

	return server_send_msg(MT_SCAN, pid, 0, data, data_len);
}

int report_attach(struct task *task)
{
	struct mt_attached_payload state = { .attached = task->attached };

	if (!server_connected())
		return -1;

	return server_send_msg(task_is_64bit(task) ? MT_ATTACH64 : MT_ATTACH, task->pid, 0, &state, sizeof(state));
}

int report_fork(struct task *task, struct task *ptask)
{
	struct mt_pid_payload fork_pid = { .pid = ptask->leader->pid };

	if (!server_connected())
		return -1;

	return server_send_msg(MT_FORK, task->pid, 0, &fork_pid, sizeof(fork_pid));
}

int report_exit(struct task *task)
{
	if (!server_connected())
		return -1;

	return server_send_msg(MT_EXIT, task->pid, 0, NULL, 0);
}

int report_about_exit(struct task *task)
{
	if (!server_connected())
		return -1;

	return server_send_msg(MT_ABOUT_EXIT, task->pid, 0, NULL, 0);
}

int report_nofollow(struct task *task)
{
	if (!server_connected())
		return -1;

	return server_send_msg(MT_NOFOLLOW, task->pid, 0, NULL, 0);
}

int report_detach(struct task *task)
{
	if (!server_connected())
		return -1;

	return server_send_msg(MT_DETACH, task->pid, 0, NULL, 0);
}

int report_disconnect(void)
{
	if (!server_connected())
		return -1;

	return server_send_msg(MT_DISCONNECT, 0, 0, NULL, 0);
}

static void report_process(struct task *leader)
{
	struct list_head *it;

	report_attach(leader);

	list_for_each(it, &leader->libraries_list) {
		struct library *lib = container_of(it, struct library, list);

		report_add_map(leader, lib);
	}
}

int report_processes(void)
{
	if (!server_connected())
		return -1;

	each_process(&report_process);

	return 0;
}

