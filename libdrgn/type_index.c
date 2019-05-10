// Copyright 2018-2019 - Omar Sandoval
// SPDX-License-Identifier: GPL-3.0+

#include <limits.h>
#include <string.h>

#include "internal.h"
#include "language.h"
#include "type_index.h"

/* These functions compare the underlying type by reference, not by value. */

static struct hash_pair
drgn_pointer_type_hash(struct drgn_type * const *key)
{
	struct drgn_qualified_type referenced_type;
	size_t hash;

	referenced_type = drgn_type_type(*key);
	hash = hash_combine((uintptr_t)referenced_type.type,
			    referenced_type.qualifiers);
	return hash_pair_from_avalanching_hash(hash);
}

static bool drgn_pointer_type_eq(struct drgn_type * const *a,
				 struct drgn_type * const *b)
{
	struct drgn_qualified_type referenced_a, referenced_b;

	referenced_a = drgn_type_type(*a);
	referenced_b = drgn_type_type(*b);
	return (referenced_a.type == referenced_b.type &&
		referenced_a.qualifiers == referenced_b.qualifiers);
}

DEFINE_HASH_SET_FUNCTIONS(drgn_pointer_type_set, struct drgn_type *,
			  drgn_pointer_type_hash, drgn_pointer_type_eq)

static struct hash_pair
drgn_array_type_hash(struct drgn_type * const *key)
{
	struct drgn_type *type = *key;
	struct drgn_qualified_type referenced_type;
	size_t hash;

	referenced_type = drgn_type_type(type);
	hash = hash_combine((uintptr_t)referenced_type.type,
			    referenced_type.qualifiers);
	hash = hash_combine(hash, drgn_type_is_complete(type));
	hash = hash_combine(hash, drgn_type_length(type));
	return hash_pair_from_avalanching_hash(hash);
}

static bool drgn_array_type_eq(struct drgn_type * const *ap,
			       struct drgn_type * const *bp)
{
	struct drgn_type *a = *ap, *b = *bp;
	struct drgn_qualified_type referenced_a, referenced_b;

	referenced_a = drgn_type_type(a);
	referenced_b = drgn_type_type(b);
	return (referenced_a.type == referenced_b.type &&
		referenced_a.qualifiers == referenced_b.qualifiers &&
		drgn_type_is_complete(a) == drgn_type_is_complete(b) &&
		drgn_type_length(a) == drgn_type_length(b));
}

DEFINE_HASH_SET_FUNCTIONS(drgn_array_type_set, struct drgn_type *,
			  drgn_array_type_hash, drgn_array_type_eq)

static struct hash_pair drgn_member_hash_pair(const struct drgn_member_key *key)
{
	size_t hash;

	if (key->name)
		hash = cityhash_size_t(key->name, key->name_len);
	else
		hash = 0;
	hash = hash_combine((uintptr_t)key->type, hash);
	return hash_pair_from_avalanching_hash(hash);
}

static bool drgn_member_eq(const struct drgn_member_key *a,
			   const struct drgn_member_key *b)
{
	return (a->type == b->type && a->name_len == b->name_len &&
		(!a->name_len || memcmp(a->name, b->name, a->name_len) == 0));
}

DEFINE_HASH_MAP_FUNCTIONS(drgn_member_map, struct drgn_member_key,
			  struct drgn_member_value, drgn_member_hash_pair,
			  drgn_member_eq)

DEFINE_HASH_SET_FUNCTIONS(drgn_type_set, struct drgn_type *,
			  hash_pair_ptr_type, hash_table_scalar_eq)

void drgn_type_index_init(struct drgn_type_index *tindex)
{
	tindex->finders = NULL;
	memset(tindex->primitive_types, 0, sizeof(tindex->primitive_types));
	drgn_pointer_type_set_init(&tindex->pointer_types);
	drgn_array_type_set_init(&tindex->array_types);
	drgn_member_map_init(&tindex->members);
	drgn_type_set_init(&tindex->members_cached);
	tindex->word_size = 0;
}

static void free_pointer_types(struct drgn_type_index *tindex)
{
	struct drgn_pointer_type_set_pos pos;

	pos = drgn_pointer_type_set_first_pos(&tindex->pointer_types);
	while (pos.item) {
		free(*pos.item);
		drgn_pointer_type_set_next_pos(&pos);
	}
	drgn_pointer_type_set_deinit(&tindex->pointer_types);
}

static void free_array_types(struct drgn_type_index *tindex)
{
	struct drgn_array_type_set_pos pos;

	pos = drgn_array_type_set_first_pos(&tindex->array_types);
	while (pos.item) {
		free(*pos.item);
		drgn_array_type_set_next_pos(&pos);
	}
	drgn_array_type_set_deinit(&tindex->array_types);
}

void drgn_type_index_deinit(struct drgn_type_index *tindex)
{
	struct drgn_type_finder *finder;

	drgn_member_map_deinit(&tindex->members);
	drgn_type_set_deinit(&tindex->members_cached);
	free_array_types(tindex);
	free_pointer_types(tindex);

	finder = tindex->finders;
	while (finder) {
		struct drgn_type_finder *next = finder->next;

		free(finder);
		finder = next;
	}
}

struct drgn_error *drgn_type_index_add_finder(struct drgn_type_index *tindex,
					      drgn_type_find_fn fn, void *arg)
{
	struct drgn_type_finder *finder;

	finder = malloc(sizeof(*finder));
	if (!finder)
		return &drgn_enomem;
	finder->fn = fn;
	finder->arg = arg;
	finder->next = tindex->finders;
	tindex->finders = finder;
	return NULL;
}

void drgn_type_index_remove_finder(struct drgn_type_index *tindex)
{
	struct drgn_type_finder *finder;

	finder = tindex->finders->next;
	free(tindex->finders);
	tindex->finders = finder;
}

/* Default long and unsigned long are 64 bits. */
static struct drgn_type default_primitive_types[DRGN_PRIMITIVE_TYPE_NUM];
/* 32-bit versions of long and unsigned long. */
static struct drgn_type default_long_32bit;
static struct drgn_type default_unsigned_long_32bit;

__attribute__((constructor(200)))
static void default_primitive_types_init(void)
{
	size_t i;

	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_CHAR],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_CHAR][0],
			   1, true);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_SIGNED_CHAR],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_SIGNED_CHAR][0],
			   1, true);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_UNSIGNED_CHAR],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_UNSIGNED_CHAR][0],
			   1, false);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_SHORT],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_SHORT][0],
			   2, true);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_UNSIGNED_SHORT],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_UNSIGNED_SHORT][0],
			   2, false);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_INT],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_INT][0], 4,
			   true);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_UNSIGNED_INT],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_UNSIGNED_INT][0],
			   4, false);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_LONG],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_LONG][0],
			   8, true);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_UNSIGNED_LONG],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_UNSIGNED_LONG][0],
			   8, false);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_LONG_LONG],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_LONG_LONG][0],
			   8, true);
	drgn_int_type_init(&default_primitive_types[DRGN_C_TYPE_UNSIGNED_LONG_LONG],
			   drgn_primitive_type_spellings[DRGN_C_TYPE_UNSIGNED_LONG_LONG][0],
			   8, false);
	drgn_bool_type_init(&default_primitive_types[DRGN_C_TYPE_BOOL],
			    drgn_primitive_type_spellings[DRGN_C_TYPE_BOOL][0],
			    1);
	drgn_float_type_init(&default_primitive_types[DRGN_C_TYPE_FLOAT],
			     drgn_primitive_type_spellings[DRGN_C_TYPE_FLOAT][0],
			     4);
	drgn_float_type_init(&default_primitive_types[DRGN_C_TYPE_DOUBLE],
			     drgn_primitive_type_spellings[DRGN_C_TYPE_DOUBLE][0],
			     8);
	drgn_float_type_init(&default_primitive_types[DRGN_C_TYPE_LONG_DOUBLE],
			     drgn_primitive_type_spellings[DRGN_C_TYPE_LONG_DOUBLE][0],
			     16);
	for (i = 0; i < ARRAY_SIZE(default_primitive_types); i++) {
		if (drgn_primitive_type_kind[i] == DRGN_TYPE_VOID ||
		    i == DRGN_C_TYPE_SIZE_T || i == DRGN_C_TYPE_PTRDIFF_T)
			continue;
		assert(drgn_type_primitive(&default_primitive_types[i]) == i);
	}

	drgn_int_type_init(&default_long_32bit,
			   drgn_primitive_type_spellings[DRGN_C_TYPE_LONG][0],
			   4, true);
	assert(drgn_type_primitive(&default_long_32bit) ==
	       DRGN_C_TYPE_LONG);

	drgn_int_type_init(&default_unsigned_long_32bit,
			   drgn_primitive_type_spellings[DRGN_C_TYPE_UNSIGNED_LONG][0],
			   4, false);
	assert(drgn_type_primitive(&default_unsigned_long_32bit) ==
	       DRGN_C_TYPE_UNSIGNED_LONG);
}

/*
 * Like @ref drgn_type_index_find_parsed(), but sets <tt>ret->type</tt> to NULL
 * and returns @c NULL if the type is not found instead of returning a @ref
 * DRGN_ERROR_LOOKUP error.
 */
static struct drgn_error *
drgn_type_index_find_parsed_internal(struct drgn_type_index *tindex,
				     enum drgn_type_kind kind, const char *name,
				     size_t name_len, const char *filename,
				     struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	struct drgn_type_finder *finder;

	finder = tindex->finders;
	while (finder) {
		err = finder->fn(kind, name, name_len, filename, finder->arg,
				 ret);
		if (err)
			return err;
		if (ret->type) {
			if (drgn_type_kind(ret->type) != kind) {
				return drgn_error_create(DRGN_ERROR_TYPE,
							 "type find callback returned wrong kind of type");
			}
			return NULL;
		}
		finder = finder->next;
	}
	ret->type = NULL;
	return NULL;
}

struct drgn_error *
drgn_type_index_find_primitive(struct drgn_type_index *tindex,
			       enum drgn_primitive_type type,
			       struct drgn_type **ret)
{
	struct drgn_error *err;
	struct drgn_qualified_type qualified_type;
	enum drgn_type_kind kind;
	const char * const *spellings;
	size_t i;

	if (tindex->primitive_types[type]) {
		*ret = tindex->primitive_types[type];
		return NULL;
	}

	kind = drgn_primitive_type_kind[type];
	if (kind == DRGN_TYPE_VOID) {
		*ret = &drgn_void_type;
		goto out;
	}

	spellings = drgn_primitive_type_spellings[type];
	for (i = 0; spellings[i]; i++) {
		err = drgn_type_index_find_parsed_internal(tindex, kind,
							   spellings[i],
							   strlen(spellings[i]),
							   NULL,
							   &qualified_type);
		if (err) {
			return err;
		} else if (qualified_type.type &&
			   drgn_type_primitive(qualified_type.type) == type) {
			*ret = qualified_type.type;
			goto out;
		}
	}

	/* long and unsigned long default to the word size. */
	if (type == DRGN_C_TYPE_LONG || type == DRGN_C_TYPE_UNSIGNED_LONG) {
		if (!tindex->word_size) {
			return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
						 "word size has not been set");
		}
		if (tindex->word_size == 4) {
			*ret = (type == DRGN_C_TYPE_LONG ?
				&default_long_32bit :
				&default_unsigned_long_32bit);
			goto out;
		}
	}
	/*
	 * size_t and ptrdiff_t default to typedefs of whatever integer type
	 * matches the word size.
	 */
	if (type == DRGN_C_TYPE_SIZE_T || type == DRGN_C_TYPE_PTRDIFF_T) {
		static enum drgn_primitive_type integer_types[2][3] = {
			{
				DRGN_C_TYPE_UNSIGNED_LONG,
				DRGN_C_TYPE_UNSIGNED_LONG_LONG,
				DRGN_C_TYPE_UNSIGNED_INT,
			},
			{
				DRGN_C_TYPE_LONG,
				DRGN_C_TYPE_LONG_LONG,
				DRGN_C_TYPE_INT,
			},
		};

		if (!tindex->word_size) {
			return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
						 "word size has not been set");
		}
		for (i = 0; i < 3; i++) {
			enum drgn_primitive_type integer_type;

			integer_type = integer_types[type == DRGN_C_TYPE_PTRDIFF_T][i];
			err = drgn_type_index_find_primitive(tindex,
							     integer_type,
							     &qualified_type.type);
			if (err)
				return err;
			if (drgn_type_size(qualified_type.type) ==
			    tindex->word_size) {
				qualified_type.qualifiers = 0;
				*ret = (type == DRGN_C_TYPE_SIZE_T ?
					&tindex->default_size_t :
					&tindex->default_ptrdiff_t);
				drgn_typedef_type_init(*ret, spellings[0],
						       qualified_type);
				goto out;
			}
		}
		return drgn_error_format(DRGN_ERROR_INVALID_ARGUMENT,
					 "no suitable integer type for %s",
					 spellings[0]);
	}

	*ret = &default_primitive_types[type];

out:
	tindex->primitive_types[type] = *ret;
	return NULL;
}

struct drgn_error *
drgn_type_index_find_parsed(struct drgn_type_index *tindex,
			    enum drgn_type_kind kind, const char *name,
			    size_t name_len, const char *filename,
			    struct drgn_qualified_type *ret)
{
	struct drgn_error *err;
	int precision;

	err = drgn_type_index_find_parsed_internal(tindex, kind, name, name_len,
						   filename, ret);
	if (err)
		return err;
	else if (ret->type)
		return NULL;

	precision = name_len < INT_MAX ? (int)name_len : INT_MAX;
	if (filename) {
		return drgn_error_format(DRGN_ERROR_LOOKUP,
					 "could not find '%s %.*s' in '%s'",
					 drgn_type_kind_spelling[kind], precision, name,
					 filename);
	} else {
		return drgn_error_format(DRGN_ERROR_LOOKUP,
					 "could not find '%s %.*s'",
					 drgn_type_kind_spelling[kind], precision, name);
	}
}

struct drgn_error *
drgn_type_index_pointer_type(struct drgn_type_index *tindex,
			     struct drgn_qualified_type referenced_type,
			     struct drgn_type **ret)
{
	struct drgn_type key, *type = &key;
	struct drgn_pointer_type_set_pos pos;
	struct hash_pair hp;

	if (!tindex->word_size) {
		return drgn_error_create(DRGN_ERROR_INVALID_ARGUMENT,
					 "word size has not been set");
	}

	drgn_pointer_type_init(type, tindex->word_size, referenced_type);
	hp = drgn_pointer_type_set_hash(&type);
	pos = drgn_pointer_type_set_search_pos(&tindex->pointer_types, &type,
					       hp);
	if (pos.item) {
		*ret = *pos.item;
		return NULL;
	}

	type = malloc(sizeof(*type));
	if (!type)
		return &drgn_enomem;
	*type = key;

	if (!drgn_pointer_type_set_insert_searched(&tindex->pointer_types,
						   &type, hp)) {
		free(type);
		return &drgn_enomem;
	}
	*ret = type;
	return NULL;
}

static struct drgn_error *
drgn_type_index_array_type_internal(struct drgn_type_index *tindex,
				    struct drgn_type *key,
				    struct drgn_type **ret)
{
	struct drgn_type *type;
	struct drgn_array_type_set_pos pos;
	struct hash_pair hp;

	hp = drgn_array_type_set_hash(&key);
	pos = drgn_array_type_set_search_pos(&tindex->array_types, &key, hp);
	if (pos.item) {
		*ret = *pos.item;
		return NULL;
	}

	type = malloc(sizeof(*type));
	if (!type)
		return &drgn_enomem;
	*type = *key;

	if (!drgn_array_type_set_insert_searched(&tindex->array_types, &type,
						 hp)) {
		free(type);
		return &drgn_enomem;
	}
	*ret = type;
	return NULL;
}

struct drgn_error *
drgn_type_index_array_type(struct drgn_type_index *tindex, uint64_t length,
			   struct drgn_qualified_type element_type,
			   struct drgn_type **ret)
{
	struct drgn_type key;

	drgn_array_type_init(&key, length, element_type);
	return drgn_type_index_array_type_internal(tindex, &key, ret);
}

struct drgn_error *
drgn_type_index_incomplete_array_type(struct drgn_type_index *tindex,
				      struct drgn_qualified_type element_type,
				      struct drgn_type **ret)
{
	struct drgn_type key;

	drgn_array_type_init_incomplete(&key, element_type);
	return drgn_type_index_array_type_internal(tindex, &key, ret);
}

static struct drgn_error *
drgn_type_index_cache_members(struct drgn_type_index *tindex,
			      struct drgn_type *outer_type,
			      struct drgn_type *type, uint64_t bit_offset)
{
	struct drgn_error *err;
	struct drgn_type_member *members;
	size_t num_members, i;

	if (!drgn_type_has_members(type))
		return NULL;

	members = drgn_type_members(type);
	num_members = drgn_type_num_members(type);
	for (i = 0; i < num_members; i++) {
		struct drgn_type_member *member;

		member = &members[i];
		if (member->name) {
			struct drgn_member_key key = {
				.type = outer_type,
				.name = member->name,
				.name_len = strlen(member->name),
			};
			struct drgn_member_value value = {
				.type = &member->type,
				.bit_offset = bit_offset + member->bit_offset,
				.bit_field_size = member->bit_field_size,
			};

			if (!drgn_member_map_insert(&tindex->members, &key,
						    &value))
				return &drgn_enomem;
		} else {
			struct drgn_qualified_type member_type;

			err = drgn_member_type(member, &member_type);
			if (err)
				return err;
			err = drgn_type_index_cache_members(tindex, outer_type,
							    member_type.type,
							    bit_offset +
							    member->bit_offset);
			if (err)
				return err;
		}
	}
	return NULL;
}

struct drgn_error *drgn_type_index_find_member(struct drgn_type_index *tindex,
					       struct drgn_type *type,
					       const char *member_name,
					       size_t member_name_len,
					       struct drgn_member_value **ret)
{
	struct drgn_error *err;
	const struct drgn_member_key key = {
		.type = drgn_underlying_type(type),
		.name = member_name,
		.name_len = member_name_len,
	};
	struct hash_pair hp, cached_hp;
	struct drgn_member_value *value;

	hp = drgn_member_map_hash(&key);
	value = drgn_member_map_search_hashed(&tindex->members, &key, hp);
	if (value) {
		*ret = value;
		return NULL;
	}

	/*
	 * Cache miss. One of the following is true:
	 *
	 * 1. The type isn't a structure or union, which is a type error.
	 * 2. The type hasn't been cached, which means we need to cache it and
	 *    check again.
	 * 3. The type has already been cached, which means the member doesn't
	 *    exist.
	 */
	if (!drgn_type_has_members(key.type)) {
		return drgn_type_error("'%s' is not a structure or union",
				       type);
	}
	cached_hp = drgn_type_set_hash(&key.type);
	if (drgn_type_set_search_hashed(&tindex->members_cached, &key.type,
					cached_hp))
		return drgn_error_member_not_found(type, member_name);

	err = drgn_type_index_cache_members(tindex, key.type, key.type, 0);
	if (err)
		return err;

	if (!drgn_type_set_insert_searched(&tindex->members_cached, &key.type,
					   cached_hp))
		return &drgn_enomem;

	value = drgn_member_map_search_hashed(&tindex->members, &key, hp);
	if (value) {
		*ret = value;
		return NULL;
	}

	return drgn_error_member_not_found(type, member_name);
}
