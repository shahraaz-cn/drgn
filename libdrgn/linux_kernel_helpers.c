// Copyright 2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#include <inttypes.h>

#include "internal.h"

struct drgn_error *
linux_helper_radix_tree_lookup(struct drgn_object *res,
			       const struct drgn_object *root, uint64_t index)
{
	struct drgn_error *err;
	static const uint64_t RADIX_TREE_ENTRY_MASK = 3;
	uint64_t RADIX_TREE_INTERNAL_NODE;
	uint64_t RADIX_TREE_MAP_MASK;
	struct drgn_object node, tmp;
	struct drgn_member_info member;
	struct drgn_qualified_type node_type;

	drgn_object_init(&node, res->prog);
	drgn_object_init(&tmp, res->prog);

	/* node = root->xa_head */
	err = drgn_object_member_dereference(&node, root, "xa_head");
	if (!err) {
		err = drgn_program_find_type(res->prog, "struct xa_node *",
					     NULL, &node_type);
		if (err)
			goto out;
		RADIX_TREE_INTERNAL_NODE = 2;
	} else if (err->code == DRGN_ERROR_LOOKUP) {
		drgn_error_destroy(err);
		/* node = (void *)root.rnode */
		err = drgn_object_member_dereference(&node, root, "rnode");
		if (err)
			goto out;
		err = drgn_program_find_type(res->prog, "void *", NULL,
					     &node_type);
		if (err)
			goto out;
		err = drgn_object_cast(&node, node_type, &node);
		if (err)
			goto out;
		err = drgn_program_find_type(res->prog,
					     "struct radix_tree_node *", NULL,
					     &node_type);
		if (err)
			goto out;
		RADIX_TREE_INTERNAL_NODE = 1;
	} else {
		goto out;
	}

	err = drgn_program_member_info(res->prog,
				       drgn_type_type(node_type.type).type,
				       "slots", &member);
	if (err)
		goto out;
	if (drgn_type_kind(member.qualified_type.type) != DRGN_TYPE_ARRAY) {
		err = drgn_error_create(DRGN_ERROR_TYPE,
					"struct radix_tree_node slots member is not an array");
		goto out;
	}
	RADIX_TREE_MAP_MASK = drgn_type_length(member.qualified_type.type) - 1;

	for (;;) {
		uint64_t value;
		union drgn_value shift;
		uint64_t offset;

		err = drgn_object_read(&node, &node);
		if (err)
			goto out;
		err = drgn_object_read_unsigned(&node, &value);
		if (err)
			goto out;
		if ((value & RADIX_TREE_ENTRY_MASK) != RADIX_TREE_INTERNAL_NODE)
			break;
		err = drgn_object_set_unsigned(&node, node_type,
					       value & ~RADIX_TREE_INTERNAL_NODE,
					       0);
		if (err)
			goto out;
		err = drgn_object_member_dereference(&tmp, &node, "shift");
		if (err)
			goto out;
		err = drgn_object_read_integer(&tmp, &shift);
		if (err)
			goto out;
		if (shift.uvalue >= 64)
			offset = 0;
		else
			offset = (index >> shift.uvalue) & RADIX_TREE_MAP_MASK;
		err = drgn_object_member_dereference(&tmp, &node, "slots");
		if (err)
			goto out;
		err = drgn_object_subscript(&node, &tmp, offset);
		if (err)
			goto out;
	}

	err = drgn_object_copy(res, &node);
out:
	drgn_object_deinit(&tmp);
	drgn_object_deinit(&node);
	return err;
}

struct drgn_error *linux_helper_idr_find(struct drgn_object *res,
					 const struct drgn_object *idr,
					 uint64_t id)
{
	struct drgn_error *err;
	struct drgn_object tmp;

	drgn_object_init(&tmp, res->prog);

	/* id -= idr->idr_base */
	err = drgn_object_member_dereference(&tmp, idr, "idr_base");
	if (!err) {
		union drgn_value idr_base;

		err = drgn_object_read_integer(&tmp, &idr_base);
		if (err)
			goto out;
		id -= idr_base.uvalue;
	} else if (err->code == DRGN_ERROR_LOOKUP) {
		/* idr_base was added in v4.16. */
		drgn_error_destroy(err);
	} else {
		goto out;
	}

	/* radix_tree_lookup(&idr->idr_rt, id) */
	err = drgn_object_member_dereference(&tmp, idr, "idr_rt");
	if (err)
		goto out;
	err = drgn_object_address_of(&tmp, &tmp);
	if (err)
		goto out;
	err = linux_helper_radix_tree_lookup(res, &tmp, id);
out:
	drgn_object_deinit(&tmp);
	return err;
}

/*
 * Before Linux kernel commit 95846ecf9dac ("pid: replace pid bitmap
 * implementation with IDR API") (in v4.15), (struct pid_namespace).idr does not
 * exist, so we have to search pid_hash. We could implement pid_hashfn() and
 * only search that bucket, but it's different for 32-bit and 64-bit systems,
 * and it has changed at least once, in v4.7. Searching the whole hash table is
 * slower but foolproof.
 */
static struct drgn_error *
find_pid_in_pid_hash(struct drgn_object *res, const struct drgn_object *ns,
		     const struct drgn_object *pid_hash, uint64_t pid)
{
	struct drgn_error *err;
	struct drgn_qualified_type pidp_type, upid_type;
	struct drgn_member_info pid_chain_member, nr_member, ns_member;
	struct drgn_object node, tmp;
	uint64_t ns_addr;
	union drgn_value ns_level, pidhash_shift;
	uint64_t i;

	err = drgn_program_find_type(res->prog, "struct pid *", NULL,
				     &pidp_type);
	if (err)
		return err;
	err = drgn_program_find_type(res->prog, "struct upid", NULL,
				     &upid_type);
	if (err)
		return err;
	err = drgn_program_member_info(res->prog, upid_type.type, "pid_chain",
				       &pid_chain_member);
	if (err)
		return err;
	err = drgn_program_member_info(res->prog, upid_type.type, "nr",
				       &nr_member);
	if (err)
		return err;
	err = drgn_program_member_info(res->prog, upid_type.type, "ns",
				       &ns_member);
	if (err)
		return err;

	drgn_object_init(&node, res->prog);
	drgn_object_init(&tmp, res->prog);

	err = drgn_object_read(&tmp, ns);
	if (err)
		goto out;
	err = drgn_object_read_unsigned(&tmp, &ns_addr);
	if (err)
		goto out;
	err = drgn_object_member_dereference(&tmp, &tmp, "level");
	if (err)
		goto out;
	err = drgn_object_read_integer(&tmp, &ns_level);
	if (err)
		goto out;

	/* i = 1 << pidhash_shift */
	err = drgn_program_find_object(res->prog, "pidhash_shift", NULL,
				       DRGN_FIND_OBJECT_ANY, &tmp);
	if (err)
		goto out;
	err = drgn_object_read_integer(&tmp, &pidhash_shift);
	if (err)
		goto out;
	if (pidhash_shift.uvalue >= 64)
		i = 0;
	else
		i = UINT64_C(1) << pidhash_shift.uvalue;
	while (i--) {
		/* for (node = pid_hash[i].first; node; node = node->next) */
		err = drgn_object_subscript(&node, pid_hash, i);
		if (err)
			goto out;
		err = drgn_object_member(&node, &node, "first");
		if (err)
			goto out;
		for (;;) {
			uint64_t addr, tmp_addr;
			union drgn_value node_nr;
			uint64_t node_ns;
			char member[64];

			err = drgn_object_read(&node, &node);
			if (err)
				goto out;
			err = drgn_object_read_unsigned(&node, &addr);
			if (err)
				goto out;
			if (!addr)
				break;
			addr -= pid_chain_member.bit_offset / 8;

			/* tmp = container_of(node, struct upid, pid_chain)->nr */
			tmp_addr = addr + nr_member.bit_offset / 8;
			err = drgn_object_set_reference(&tmp, nr_member.qualified_type,
							tmp_addr, 0, 0,
							DRGN_PROGRAM_ENDIAN);
			if (err)
				goto out;
			err = drgn_object_read_integer(&tmp, &node_nr);
			if (err)
				goto out;
			if (node_nr.uvalue != pid)
				goto next;

			/* tmp = container_of(node, struct upid, pid_chain)->ns */
			tmp_addr = addr + ns_member.bit_offset / 8;
			err = drgn_object_set_reference(&tmp, ns_member.qualified_type,
							tmp_addr, 0, 0,
							DRGN_PROGRAM_ENDIAN);
			if (err)
				goto out;

			err = drgn_object_read_unsigned(&tmp, &node_ns);
			if (err)
				goto out;
			if (node_ns != ns_addr)
				goto next;

			sprintf(member, "numbers[%" PRIu64 "].pid_chain",
				ns_level.uvalue);
			err = drgn_object_container_of(res, &node,
						       drgn_type_type(pidp_type.type),
						       member);
			goto out;

next:
			err = drgn_object_member_dereference(&node, &node, "next");
			if (err)
				goto out;
		}
	}

	err = drgn_object_set_unsigned(res, pidp_type, 0, 0);
out:
	drgn_object_deinit(&tmp);
	drgn_object_deinit(&node);
	return err;
}

struct drgn_error *linux_helper_find_pid(struct drgn_object *res,
					 const struct drgn_object *ns,
					 uint64_t pid)
{
	struct drgn_error *err;
	struct drgn_object tmp;

	drgn_object_init(&tmp, res->prog);

	/* (struct pid *)idr_find(&ns->idr, pid) */
	err = drgn_object_member_dereference(&tmp, ns, "idr");
	if (!err) {
		struct drgn_qualified_type qualified_type;

		err = drgn_object_address_of(&tmp, &tmp);
		if (err)
			goto out;
		err = linux_helper_idr_find(&tmp, &tmp, pid);
		if (err)
			goto out;
		err = drgn_program_find_type(res->prog, "struct pid *", NULL,
					     &qualified_type);
		if (err)
			goto out;
		err = drgn_object_cast(res, qualified_type, &tmp);
	} else if (err->code == DRGN_ERROR_LOOKUP) {
		drgn_error_destroy(err);
		err = drgn_program_find_object(res->prog, "pid_hash", NULL,
					       DRGN_FIND_OBJECT_ANY, &tmp);
		if (err)
			goto out;
		err = find_pid_in_pid_hash(res, ns, &tmp, pid);
	}
out:
	drgn_object_deinit(&tmp);
	return err;
}

struct drgn_error *linux_helper_pid_task(struct drgn_object *res,
					 const struct drgn_object *pid,
					 uint64_t pid_type)
{
	struct drgn_error *err;
	struct drgn_qualified_type task_structp_type;
	struct drgn_qualified_type task_struct_type;
	bool truthy;
	struct drgn_object first;
	char member[64];

	drgn_object_init(&first, res->prog);

	err = drgn_program_find_type(res->prog, "struct task_struct *", NULL,
				     &task_structp_type);
	if (err)
		goto out;
	task_struct_type = drgn_type_type(task_structp_type.type);

	err = drgn_object_bool(pid, &truthy);
	if (!truthy)
		goto null;

	/* first = &pid->tasks[pid_type].first */
	err = drgn_object_member_dereference(&first, pid, "tasks");
	if (err)
		goto out;
	err = drgn_object_subscript(&first, &first, pid_type);
	if (err)
		goto out;
	err = drgn_object_member(&first, &first, "first");
	if (err)
		goto out;

	err = drgn_object_bool(&first, &truthy);
	if (err)
		goto out;
	if (!truthy)
		goto null;

	/* container_of(first, struct task_struct, pid_links[pid_type]) */
	sprintf(member, "pid_links[%" PRIu64 "]", pid_type);
	err = drgn_object_container_of(res, &first, task_struct_type, member);
	if (err && err->code == DRGN_ERROR_LOOKUP) {
		drgn_error_destroy(err);
		/* container_of(first, struct task_struct, pids[pid_type].node) */
		sprintf(member, "pids[%" PRIu64 "].node", pid_type);
		err = drgn_object_container_of(res, &first, task_struct_type,
					       member);
	}
out:
	drgn_object_deinit(&first);
	return err;

null:
	err = drgn_object_set_unsigned(res, task_structp_type, 0, 0);
	goto out;
}

struct drgn_error *linux_helper_find_task(struct drgn_object *res,
					  const struct drgn_object *ns,
					  uint64_t pid)
{
	struct drgn_error *err;
	struct drgn_object pid_obj;
	struct drgn_object pid_type_obj;
	union drgn_value pid_type;

	drgn_object_init(&pid_obj, res->prog);
	drgn_object_init(&pid_type_obj, res->prog);

	err = linux_helper_find_pid(&pid_obj, ns, pid);
	if (err)
		goto out;
	err = drgn_program_find_object(res->prog, "PIDTYPE_PID", NULL,
				       DRGN_FIND_OBJECT_CONSTANT,
				       &pid_type_obj);
	if (err)
		goto out;
	err = drgn_object_read_integer(&pid_type_obj, &pid_type);
	if (err)
		goto out;
	err = linux_helper_pid_task(res, &pid_obj, pid_type.uvalue);
out:
	drgn_object_deinit(&pid_type_obj);
	drgn_object_deinit(&pid_obj);
	return err;
}
