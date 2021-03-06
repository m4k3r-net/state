#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "dict.h"
#include "str.h"

static void atom_atexit(void);

/*
 * The atom dictionary maps NUL-terminated C string keys to
 * pointers to unique struct str_seg values.
 */
static const char empty_atom[1];
static struct dict *atom_global_dict;

/**
 * A simple hash function over nul-terminated C strings.
 * TODO find a better hashing function
 *
 * @return an unsigned integer which is a hash over the
 *         content of the C string.
 */
static unsigned
strhash(const char *s)
{
	unsigned h = 0;

	while (*s) {
		h = (h << 1) ^ *s++;
	}
	return h;
}

/**
 * Decrements the reference count of a shared segment.
 * Frees the segment when nothing refers to it.
 * This is used as the free function in the atom dictionary.
 * XXX It is too coupled to the implementation of #str_free().
 */
static void
str_seg_release(struct str_seg *seg)
{
	if (!--seg->refs) {
		free(seg);
	}
}

/**
 * Releases the atom dictionary, to be called during #exit().
 */
static void
atom_atexit()
{
	dict_free(atom_global_dict);
	atom_global_dict = 0;
}

/**
 * Obtain or create a pointer to the global atom dictionary.
 * (Singleton factory)
 *
 * @returns the global atom dictionary.
 */
static struct dict *
atom_dict()
{
	if (!atom_global_dict) {
		atom_global_dict = dict_new(
			(void(*)(void *))str_seg_release,
			(int(*)(const void *, const void *))strcmp,
			(unsigned(*)(const void *))strhash
		);
		atexit(atom_atexit);
	}
	return atom_global_dict;
}

atom
atom_from_str(struct str *str)
{
	struct dict *dict;
	unsigned len;
	struct str_seg *a, *b;

	if (!str) {
		return empty_atom;
	}

	len = str_len(str);
	b = malloc(sizeof (struct str_seg) + len);
	str_copy(str, b->data, 0, len);
	b->data[len] = '\0';

	dict = atom_dict();
	a = dict_get(dict, b->data);
	if (a) {
		free(b);
	} else {
		a = b;
		b->refs = 1;
		dict_put(dict, b->data, b);
	}
	return a->data;
}

atom
atom_s(const char *s)
{
	struct dict *dict;
	struct str_seg *seg;

	if (!s) {
		return 0;
	}
	if (!*s) {
		return empty_atom;
	}

	dict = atom_dict();
	seg = dict_get(dict, s);
	if (!seg) {
		int len = strlen(s);
		seg = malloc(sizeof (struct str_seg) + len);
		seg->refs = 1;
		memcpy(seg->data, s, len + 1);
		dict_put(dict, seg->data, seg);
	}
	return seg->data;
}

atom
atom_sn(const char *s, unsigned len)
{
	struct dict *dict;
	struct str_seg *nseg, *oseg;

	if (!len) {
		return empty_atom;
	}

	nseg = malloc(sizeof (struct str_seg) + len);
	nseg->refs = 1;
	memcpy(nseg->data, s, len);
	nseg->data[len] = '\0';

	dict = atom_dict();
	oseg = dict_get(dict, nseg->data);
	if (!oseg) {
		oseg = nseg;
		dict_put(dict, oseg->data, oseg);
	} else {
		free(nseg);
	}
	return oseg->data;
}

struct str **
atom_xstr(struct str **ret, atom a)
{
	struct dict *dict;
	struct str_seg *seg;
	struct str dummy;

	if (!a || !*a) {
		return ret;
	}

	dict = atom_dict();
	seg = dict_get(dict, a);
	dummy.seg = seg;
	dummy.offset = 0;
	dummy.len = strlen(a);
	dummy.next = 0;
	return str_xcat(ret, &dummy);
}

str *
atom_to_str(atom a)
{
	str *ret;

	*atom_xstr(&ret, a) = NULL;
	return ret;
}
