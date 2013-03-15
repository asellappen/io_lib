#ifdef HAVE_CONFIG_H
#include "io_lib_config.h"
#endif

#include <string.h>
#include <assert.h>

#include "io_lib/sam_header.h"

void sam_header_error(char *msg, char *line, int len, int lno) {
    int j;
    
    for (j = 0; j < len && line[j] != '\n'; j++)
	;
    fprintf(stderr, "%s at line %d: \"%.*s\"\n", msg, lno, j, line);
}

void sam_header_dump(SAM_hdr *hdr) {
    HashIter *iter = HashTableIterCreate();
    HashItem *hi;
    char *str = dstring_str(hdr->text);
    int i;

    printf("===DUMP===\n");
    while ((hi = HashTableIterNext(hdr->h, iter))) {
	SAM_hdr_type *t1, *t2;

	t1 = t2 = hi->data.p;
	printf("Type %.2s, count %d\n", hi->key, t1->prev->order+1);

	do {
	    SAM_hdr_tag *tag;
	    printf(">>>%d ", t1->order);
	    for (tag = t1->tag; tag; tag=tag->next) {
		printf("\"%.2s\":\"%.*s\"\t",
		       str+tag->idx, tag->len-3, str+tag->idx+3);
	    }
	    putchar('\n');
	    t1 = t1->next;
	} while (t1 != t2);
    }

    /* Dump out PG chains */
    printf("\n@PG chains:\n");
    for (i = 0; i < hdr->npg_end; i++) {
	int j;
	printf("  %d:", i);
	for (j = hdr->pg_end[i]; j != -1; j = hdr->pg[j].prev_id) {
	    printf("%s%d(%.*s)", 
		   j == hdr->pg_end[i] ? " " : "->",
		   j, hdr->pg[j].name_len, hdr->pg[j].name);
	}
	printf("\n");
    }

    puts("===END DUMP===");

    HashTableIterDestroy(iter);
}

static int sam_header_update_hashes(SAM_hdr *sh,
				    char *type,
				    SAM_hdr_type *h_type) {
    /* Add to reference hash? */
    if (type[0] == 'S' && type[1] == 'Q') {
	SAM_hdr_tag *tag;
	char *str = dstring_str(sh->text);
	int nref = sh->nref;

	sh->ref = realloc(sh->ref, (sh->nref+1)*sizeof(*sh->ref));
	if (!sh->ref)
	    return -1;

	tag = h_type->tag;
	sh->ref[nref].name = NULL;
	sh->ref[nref].len  = 0;
	sh->ref[nref].tag  = tag;

	while (tag) {
	    if (str[tag->idx+0] == 'S' &&
		str[tag->idx+1] == 'N') {
		if (!(sh->ref[nref].name = malloc(tag->len)))
		    return -1;
		strncpy(sh->ref[nref].name, &str[tag->idx+3],
			tag->len-3);
		sh->ref[nref].name[tag->len-3] = 0;
	    } else if (str[tag->idx+0] == 'L' &&
		       str[tag->idx+1] == 'N') {
		sh->ref[nref].len = atoi(&str[tag->idx+3]);
	    }
	    tag = tag->next;
	}

	if (sh->ref[nref].name) {
	    HashData hd;
	    hd.i = nref;
	    if (!HashTableAdd(sh->ref_hash, sh->ref[nref].name, 0, hd, NULL))
		return -1;
	}

	sh->nref++;
    }

    /* Add to read-group hash? */
    if (type[0] == 'R' && type[1] == 'G') {
	SAM_hdr_tag *tag;
	char *str = dstring_str(sh->text);
	int nrg = sh->nrg;

	sh->rg = realloc(sh->rg, (sh->nrg+1)*sizeof(*sh->rg));
	if (!sh->rg)
	    return -1;

	tag = h_type->tag;
	sh->rg[nrg].name = NULL;
	sh->rg[nrg].name_len = 0;
	sh->rg[nrg].tag  = tag;
	sh->rg[nrg].id   = nrg;

	while (tag) {
	    if (str[tag->idx+0] == 'I' &&
		str[tag->idx+1] == 'D') {
		if (!(sh->rg[nrg].name = malloc(tag->len)))
		    return -1;
		strncpy(sh->rg[nrg].name, &str[tag->idx+3], tag->len-3);
		sh->rg[nrg].name[tag->len-3] = 0;
		sh->rg[nrg].name_len = strlen(sh->rg[nrg].name);
	    }
	    tag = tag->next;
	}

	if (sh->rg[nrg].name) {
	    HashData hd;
	    hd.i = nrg;
	    if (!HashTableAdd(sh->rg_hash, sh->rg[nrg].name, 0, hd, NULL))
		return -1;
	}

	sh->nrg++;
    }

    /* Add to program hash? */
    if (type[0] == 'P' && type[1] == 'G') {
	SAM_hdr_tag *tag;
	char *str = dstring_str(sh->text);
	int npg = sh->npg;

	sh->pg = realloc(sh->pg, (sh->npg+1)*sizeof(*sh->pg));
	if (!sh->pg)
	    return -1;

	tag = h_type->tag;
	sh->pg[npg].name = NULL;
	sh->pg[npg].name_len = 0;
	sh->pg[npg].tag  = tag;
	sh->pg[npg].id   = npg;
	sh->pg[npg].prev_id = -1;

	while (tag) {
	    if (str[tag->idx+0] == 'I' &&
		str[tag->idx+1] == 'D') {
		if (!(sh->pg[npg].name = malloc(tag->len)))
		    return -1;
		strncpy(sh->pg[npg].name, &str[tag->idx+3], tag->len-3);
		sh->pg[npg].name[tag->len-3] = 0;
		sh->pg[npg].name_len = strlen(sh->pg[npg].name);
	    } else if (str[tag->idx+0] == 'P' &&
		       str[tag->idx+1] == 'P') {
		// Resolve later if needed
		HashItem *hi = HashTableSearch(sh->pg_hash,
					       &str[tag->idx+3],
					       tag->len-3);
		if (hi) {
		    sh->pg[npg].prev_id = sh->pg[hi->data.i].id;

		    /* Unmark previous entry as a PG termination */
		    if (sh->npg_end > 0 &&
			sh->pg_end[sh->npg_end-1] == hi->data.i) {
			sh->npg_end--;
		    } else {
			int i;
			for (i = 0; i < sh->npg_end; i++) {
			    if (sh->pg_end[i] == hi->data.i) {
				memmove(&sh->pg_end[i], &sh->pg_end[i+1],
					(sh->npg_end - i)*sizeof(*sh->pg_end));
				sh->npg_end--;
			    }
			}
		    }
		} else {
		    sh->pg[npg].prev_id = -1;
		}
	    }
	    tag = tag->next;
	}

	if (sh->pg[npg].name) {
	    HashData hd;
	    hd.i = npg;
	    if (!HashTableAdd(sh->pg_hash, sh->pg[npg].name, 0, hd, NULL))
		return -1;
	}

	/* Add to npg_end[] array. Remove later if we find a PP line */
	if (sh->npg_end >= sh->npg_end_alloc) {
	    sh->npg_end_alloc = sh->npg_end_alloc
		? sh->npg_end_alloc*2
		: 4;
	    sh->pg_end = realloc(sh->pg_end,
				 sh->npg_end_alloc * sizeof(int));
	    if (!sh->pg_end)
		return -1;
	}
	sh->pg_end[sh->npg_end++] = npg;

	sh->npg++;
    }

    return 0;
}

/*
 * Appends a formatted line to an existing SAM header.
 * Line is a full SAM header record, eg "@SQ\tSN:foo\tLN:100", with
 * optional new-line. If it contains more than 1 line then multiple lines
 * will be added in order.
 *
 * Len is the length of the text data, or 0 if unknown (in which case
 * it should be null terminated).
 *
 * Returns 0 on success
 *        -1 on failure
 */
int sam_header_add_lines(SAM_hdr *sh, char *lines, int len) {
    int i, lno = 1, text_offset;
    HashItem *hi;
    HashData hd;
    char *hdr;

    if (!len)
	len = strlen(lines);
    hdr = DSTRING_STR(sh->text);
    if (hdr)
	hdr += (text_offset = DSTRING_LEN(sh->text));
    else
	text_offset = 0;
    dstring_nappend(sh->text, lines, len);
    if (!hdr)
	hdr = DSTRING_STR(sh->text);

    for (i = 0; i < len; i++) {
	char *type;
	int l_start = i, new;
	SAM_hdr_type *h_type;
	SAM_hdr_tag *h_tag, *last;

	if (hdr[i] != '@') {
	    int j;
	    for (j = i; j < len && hdr[j] != '\n'; j++)
		;
	    sam_header_error("Header line does not start with '@'",
			     &hdr[l_start], len - l_start, lno);
	    return -1;
	}

	type = &hdr[i+1];
	i += 3;
	if (hdr[i] == '\n')
	    continue;

	// Add the header line type
	h_type = pool_alloc(sh->type_pool);
	hd.p = h_type;
	if (!(hi = HashTableAdd(sh->h, type, 2, hd, &new)))
	    return -1;

	// Form the ring, either with self or other lines of this type
	if (!new) {
	    SAM_hdr_type *t = hi->data.p, *p;
	    p = t->prev;
	    
	    assert(p->next = t);
	    p->next = h_type;
	    h_type->prev = p;

	    t->prev = h_type;
	    h_type->next = t;
	    h_type->order = p->order+1;
	} else {
	    h_type->prev = h_type->next = h_type;
	    h_type->order = 0;
	}

	// Parse the tags on this line
	last = NULL;
	do {
	    int j;
	    assert(hdr[i] == '\t');
	    for (j = ++i; j < len && hdr[j] != '\n' && hdr[j] != '\t'; j++)
		;
	    
	    h_tag = pool_alloc(sh->tag_pool);
	    h_tag->idx = i + text_offset;
	    h_tag->len = j-i;
	    h_tag->next = NULL;
	    
	    if (last)
		last->next = h_tag;
	    else
		h_type->tag = h_tag;

	    last = h_tag;
	    i = j;
	} while (i < len && hdr[i] != '\n');

	/* Update RG/SQ hashes */
	if (-1 == sam_header_update_hashes(sh, type, h_type))
	    return -1;
    }

    return 0;
}

/*
 * Adds a single line to a SAM header.
 * Specify type and one or more key,value pairs, ending with the NULL key.
 * Eg. sam_header_add(h, "SQ", "ID", "foo", "LN", "100", NULL).
 *
 * Returns index for specific entry on success (eg 2nd SQ, 4th RG)
 *        -1 on failure
 */
int sam_header_add(SAM_hdr *sh, char *type, ...) {
    va_list args;
    va_start(args, type);
    return sam_header_vadd(sh, type, args, NULL);
}

int sam_header_vadd(SAM_hdr *sh, char *type, va_list ap, ...) {
    va_list args;
    HashItem *hi;
    HashData hd;
    SAM_hdr_type *h_type;
    SAM_hdr_tag *h_tag, *last;
    int new;

#if defined(HAVE_VA_COPY)
    va_list ap_local;
#endif

    if (-1 == dstring_append_char(sh->text, '@'))
	return -1;
    if (-1 == dstring_nappend(sh->text, type, 2))
	return -1;

    h_type = pool_alloc(sh->type_pool);
    hd.p = h_type;
    if (!(hi = HashTableAdd(sh->h, type, 2, hd, &new)))
	return -1;
    
    // Form the ring, either with self or other lines of this type
    if (!new) {
	SAM_hdr_type *t = hi->data.p, *p;
	p = t->prev;
	    
	assert(p->next = t);
	p->next = h_type;
	h_type->prev = p;

	t->prev = h_type;
	h_type->next = t;
	h_type->order = p->order + 1;
    } else {
	h_type->prev = h_type->next = h_type;
	h_type->order = 0;
    }

    last = NULL;

    // Any ... varargs
    va_start(args, ap);
    for (;;) {
	char *k, *v;
	
	if (!(k = (char *)va_arg(args, char *)))
	    break;
	v = va_arg(args, char *);

	if (-1 == dstring_append_char(sh->text, '\t'))
	    return -1;

	h_tag = pool_alloc(sh->tag_pool);
	h_tag->idx = DSTRING_LEN(sh->text);
	
	if (-1 == dstring_append(sh->text, k))
	    return -1;
	if (-1 == dstring_append_char(sh->text, ':'))
	    return -1;
	if (-1 == dstring_append(sh->text, v))
	    return -1;

	h_tag->len = DSTRING_LEN(sh->text) - h_tag->idx;
	h_tag->next = NULL;

	if (last)
	    last->next = h_tag;
	else
	    h_type->tag = h_tag;
	
	last = h_tag;
    }
    va_end(args);

#if defined(HAVE_VA_COPY)
    va_copy(ap_local, ap);
#   define ap ap_local
#endif

    // Plus the specified va_list params
    for (;;) {
	char *k, *v;
	
	if (!(k = (char *)va_arg(ap, char *)))
	    break;
	v = va_arg(ap, char *);

	if (-1 == dstring_append_char(sh->text, '\t'))
	    return -1;

	h_tag = pool_alloc(sh->tag_pool);
	h_tag->idx = DSTRING_LEN(sh->text);
	
	if (-1 == dstring_append(sh->text, k))
	    return -1;
	if (-1 == dstring_append_char(sh->text, ':'))
	    return -1;
	if (-1 == dstring_append(sh->text, v))
	    return -1;

	h_tag->len = DSTRING_LEN(sh->text) - h_tag->idx;
	h_tag->next = NULL;

	if (last)
	    last->next = h_tag;
	else
	    h_type->tag = h_tag;
	
	last = h_tag;
    }
    va_end(ap);

    if (-1 == dstring_append_char(sh->text, '\n'))
	return -1;

    if (-1 == sam_header_update_hashes(sh, type, h_type))
	return -1;

    return h_type->order;
}

/*
 * Returns the first header item matching 'type'. If ID is non-NULL it checks
 * for the tag ID: and compares against the specified ID.
 *
 * Returns NULL if no type/ID is found
 */
SAM_hdr_type *sam_header_find(SAM_hdr *hdr, char *type,
			      char *ID_key, char *ID_value) {
    HashItem *hi;
    SAM_hdr_type *t1, *t2;
    char *str = dstring_str(hdr->text);

    if (!(hi = HashTableSearch(hdr->h, type, 2)))
	return NULL;

    if (!ID_key)
	return hi->data.p;

    t1 = t2 = hi->data.p;
    do {
	SAM_hdr_tag *tag;
	for (tag = t1->tag; tag; tag = tag->next) {
	    if (str[tag->idx  ] == ID_key[0] &&
		str[tag->idx+1] == ID_key[1]) {
		char *cp1 = &str[tag->idx+3];
		char *cp2 = ID_value;
		while (*cp1 == *cp2)
		    cp1++, cp2++;
		if (*cp2)
		    continue;
		if (*cp1 != '\t' && *cp1 != '\n' && *cp1 != '\0')
		    continue;
		return t1;
	    }
	}
	t1 = t1->next;
    } while (t1 != t2);

    return NULL;
}

/*
 * As per SAM_hdr_type, but returns a complete line of formatted text
 * for a specific head type/ID combination. If ID is NULL then it returns
 * the first line of the specified type.
 *
 * The returned string is malloced and should be freed by the calling
 * function with free().
 *
 * Returns NULL if no type/ID is found.
 */
char *sam_header_find_line(SAM_hdr *hdr, char *type,
			   char *ID_key, char *ID_value) {
    SAM_hdr_type *ty = sam_header_find(hdr, type, ID_key, ID_value);
    dstring_t *ds;
    SAM_hdr_tag *tag;
    char *str = dstring_str(hdr->text);
    int r = 0;

    if (!ty)
	return NULL;

    if (NULL == (ds = dstring_create(NULL)))
	return NULL;

    // Paste together the line from the hashed copy
    r |= dstring_append_char(ds, '@');
    r |= dstring_append(ds, type);
    for (tag = ty->tag; tag; tag = tag->next) {
	r |= dstring_append_char(ds, '\t');
	r |= dstring_nappend(ds, str+tag->idx, tag->len);
    }

    if (r) {
	dstring_destroy(ds);
	return NULL;
    }

    // Steal the dstring copy and return that.
    str = DSTRING_STR(ds);
    DSTRING_STR(ds) = NULL;
    dstring_destroy(ds);

    return str;
}


/*
 * Looks for a specific key in a single sam header line.
 * If prev is non-NULL it also fills this out with the previous tag, to
 * permit use in key removal. *prev is set to NULL when the tag is the first
 * key in the list. When a tag isn't found, prev (if non NULL) will be the last
 * tag in the existing list.
 *
 * Returns the tag pointer on success
 *         NULL on failure
 */
SAM_hdr_tag *sam_header_find_key(SAM_hdr *sh,
				 SAM_hdr_type *type,
				 char *key,
				 SAM_hdr_tag **prev) {
    SAM_hdr_tag *tag, *p = NULL;
    char *str = DSTRING_STR(sh->text);

    for (tag = type->tag; tag; p = tag, tag = tag->next) {
	if (str[tag->idx+0] == key[0] &&
	    str[tag->idx+1] == key[1]) {
	    if (prev)
		*prev = p;
	    return tag;
	}
    }

    if (prev)
	*prev = p;

    return NULL;
}


// Temporary function unti we rewrite sam_header to use string_pool_t
char *sam_header_find_key2(SAM_hdr *sh,
			   SAM_hdr_type *type,
			   char *key,
			   int *len /* out */) {
    SAM_hdr_tag *tag = sam_header_find_key(sh, type, key, NULL);
    if (!tag)
	return NULL;
    if (len) *len = tag->len;
    return dstring_str(sh->text) + tag->idx;
}


/*
 * Adds or updates tag key,value pairs in a header line.
 * Eg for adding M5 tags to @SQ lines or updating sort order for the
 * @HD line (although use the sam_header_sort_order() function for
 * HD manipulation, which is a wrapper around this funuction).
 *
 * Specify multiple key,value pairs ending in NULL.
 *
 * Returns 0 on success
 *        -1 on failure
 */
int sam_header_update(SAM_hdr *hdr, SAM_hdr_type *type, ...) {
    va_list ap;

    va_start(ap, type);
    
    for (;;) {
	char *k, *v;
	SAM_hdr_tag *tag, *prev;

	if (!(k = (char *)va_arg(ap, char *)))
	    break;
	v = va_arg(ap, char *);

	tag = sam_header_find_key(hdr, type, k, &prev);
	if (!tag) {
	    if (!(tag = pool_alloc(hdr->tag_pool)))
		return -1;
	    if (prev)
		prev->next = tag;
	    else
		type->tag = tag;
	}

	tag->idx = DSTRING_LEN(hdr->text);
	if (0 != dstring_appendf(hdr->text, "%2.2s:%s", k, v))
	    return -1;
	tag->len = DSTRING_LEN(hdr->text) - tag->idx;
	tag->next = NULL;
    }

    va_end(ap);

    return 0;
}

static void sam_header_free_internals(SAM_hdr *hdr);
/*
 * Reconstructs the dstring from the header hash table.
 * Returns 0 on success
 *        -1 on failure
 */
int sam_header_rebuild(SAM_hdr *hdr) {
    /* Order: HD then others */
    HashItem *hi;
    HashIter *iter = HashTableIterCreate();
    dstring_t *ds = dstring_create(NULL);
    char *str = dstring_str(hdr->text);

    if (!iter || !ds)
	return -1;

    if ((hi = HashTableSearch(hdr->h, "HD", 2))) {
	SAM_hdr_type *ty = hi->data.p;
	SAM_hdr_tag *tag;
	if (-1 == dstring_append(ds, "@HD"))
	    return -1;
	for (tag = ty->tag; tag; tag = tag->next) {
	    if (-1 == dstring_append_char(ds, '\t'))
		return -1;
	    if (-1 == dstring_nappend(ds, str+tag->idx, tag->len))
		return -1;
	}
	if (-1 == dstring_append_char(ds, '\n'))
	    return -1;
    }

    while ((hi = HashTableIterNext(hdr->h, iter))) {
	SAM_hdr_type *t1, *t2;
	if (hi->key[0] == 'H' && hi->key[1] == 'D')
	    continue;

	t1 = t2 = hi->data.p;
	do {
	    SAM_hdr_tag *tag;
	    if (-1 == dstring_append_char(ds, '@'))
		return -1;
	    if (-1 == dstring_nappend(ds, hi->key, 2))
		return -1;
	    for (tag = t1->tag; tag; tag=tag->next) {
		if (-1 == dstring_append_char(ds, '\t'))
		    return -1;
		if (-1 == dstring_nappend(ds, str+tag->idx, tag->len))
		    return -1;
	    }
	    if (-1 == dstring_append_char(ds, '\n'))
		return -1;
	    t1 = t1->next;
	} while (t1 != t2);
    }

    HashTableIterDestroy(iter);

    /* We now need to reparse too as indices changed - messy! */
    // FIXME: switch to using Misc/string_alloc.c for efficient string pool
    {
	SAM_hdr *h2 = sam_header_parse(dstring_str(ds), dstring_length(ds));
	sam_header_free_internals(hdr);
	*hdr = *h2;
	dstring_destroy(ds);
	free(h2);
    }
	
    //dstring_destroy(hdr->text);
    //hdr->text = ds;

    return 0;
}


/*
 * Tokenises a SAM header into a hash table.
 * Also extracts a few bits on specific data types, such as @RG lines.
 *
 * Returns a SAM_hdr struct on success (free with sam_header_free())
 *         NULL on failure
 */
SAM_hdr *sam_header_parse(char *hdr, int len) {
    SAM_hdr *sh = calloc(1, sizeof(*sh));

    if (!sh)
	return NULL;
    
    sh->h = HashTableCreate(16, HASH_FUNC_HSIEH |
			        HASH_DYNAMIC_SIZE);
    if (!sh->h)
	goto err;

    sh->ID_cnt = 1;

    sh->nref = 0;
    sh->ref  = NULL;
    if (!(sh->ref_hash = HashTableCreate(16, HASH_FUNC_HSIEH |
					 HASH_DYNAMIC_SIZE |
					 HASH_NONVOLATILE_KEYS)))
	goto err;

    sh->nrg = 0;
    sh->rg  = NULL;
    if (!(sh->rg_hash = HashTableCreate(16, HASH_FUNC_HSIEH |
					HASH_DYNAMIC_SIZE |
					HASH_NONVOLATILE_KEYS)))
	goto err;

    sh->npg = 0;
    sh->pg  = NULL;
    sh->npg_end = sh->npg_end_alloc = 0;
    sh->pg_end = NULL;
    if (!(sh->pg_hash = HashTableCreate(16, HASH_FUNC_HSIEH |
					HASH_DYNAMIC_SIZE |
					HASH_NONVOLATILE_KEYS)))
	goto err;

    if (!(sh->text = dstring_create(NULL)))
	goto err;

    if (!(sh->tag_pool = pool_create(sizeof(SAM_hdr_tag))))
	goto err;

    if (!(sh->type_pool = pool_create(sizeof(SAM_hdr_type))))
	goto err;

    /* Parse the header, line by line */
    if (-1 == sam_header_add_lines(sh, hdr, len))
	goto err;

    //sam_header_dump(sh);
    //sam_header_add(sh, "RG", "ID", "foo", "SM", "bar", NULL);
    //sam_header_rebuild(sh);
    //printf(">>%s<<", DSTRING_STR(sh->text));

    //parse_references(sh);
    //parse_read_groups(sh);

    sam_header_link_pg(sh);
    //sam_header_dump(sh);

    return sh;

 err:
    if (sh->h)
	HashTableDestroy(sh->h, 0);

    if (sh->tag_pool)
	pool_destroy(sh->tag_pool);

    if (sh->type_pool)
	pool_destroy(sh->type_pool);

    free(sh);

    return NULL;
}

/*
 * Deallocates all storage used by a SAM_hdr struct.
 */
static void sam_header_free_internals(SAM_hdr *hdr) {
    if (hdr->text)
	dstring_destroy(hdr->text);

    if (hdr->h)
	HashTableDestroy(hdr->h, 0);

    if (hdr->ref_hash)
	HashTableDestroy(hdr->ref_hash, 0);

    if (hdr->ref) {
	int i;
	for (i = 0; i < hdr->nref; i++)
	    if (hdr->ref[i].name)
		free(hdr->ref[i].name);
	free(hdr->ref);
    }

    if (hdr->rg_hash)
	HashTableDestroy(hdr->rg_hash, 0);

    if (hdr->rg) {
	int i;
	for (i = 0; i < hdr->nrg; i++)
	    if (hdr->rg[i].name)
		free(hdr->rg[i].name);
	free(hdr->rg);
    }

    if (hdr->pg_hash)
	HashTableDestroy(hdr->pg_hash, 0);

    if (hdr->pg) {
	int i;
	for (i = 0; i < hdr->npg; i++)
	    if (hdr->pg[i].name)
		free(hdr->pg[i].name);
	free(hdr->pg);
    }

    if (hdr->pg_end)
	free(hdr->pg_end);

    if (hdr->type_pool)
	pool_destroy(hdr->type_pool);

    if (hdr->tag_pool)
	pool_destroy(hdr->tag_pool);
}

void sam_header_free(SAM_hdr *hdr) {
    if (!hdr)
	return;

    sam_header_free_internals(hdr);
    free(hdr);
}

int sam_header_length(SAM_hdr *hdr) {
    return dstring_length(hdr->text);
}

char *sam_header_str(SAM_hdr *hdr) {
    return dstring_str(hdr->text);
}

/*
 * Looks up a reference sequence by name and returns the numerical ID.
 * Returns -1 if unknown reference.
 */
int sam_header_name2ref(SAM_hdr *hdr, char *ref) {
    HashItem *hi = HashTableSearch(hdr->ref_hash, ref, strlen(ref));
    return hi ? hi->data.i : -1;
}

/*
 * Looks up a read-group by name and returns a pointer to the start of the
 * associated tag list.
 *
 * Returns NULL on failure
 */
SAM_RG *sam_header_find_rg(SAM_hdr *hdr, char *rg) {
    HashItem *hi = HashTableSearch(hdr->rg_hash, rg, 0);
    return hi ? &hdr->rg[hi->data.i] : NULL;
}


/*
 * Fixes any PP links in @PG headers.
 * If the entries are in order then this doesn't need doing, but incase
 * our header is out of order this goes through the sh->pg[] array
 * setting the prev_id field.
 *
 * Note we can have multiple complete chains. This code should identify the
 * tails of these chains as these are the entries we have to link to in
 * subsequent PP records.
 *
 * Returns 0 on sucess
 *        -1 on failure (indicating broken PG/PP records)
 */
int sam_header_link_pg(SAM_hdr *hdr) {
    int i, j, ret = 0;
    char *str = dstring_str(hdr->text);

    hdr->npg_end_alloc = hdr->npg;
    hdr->pg_end = realloc(hdr->pg_end, hdr->npg * sizeof(*hdr->pg_end));
    if (!hdr->pg_end)
	return -1;

    for (i = 0; i < hdr->npg; i++)
	hdr->pg_end[i] = i;

    for (i = 0; i < hdr->npg; i++) {
	HashItem *hi;
	SAM_hdr_tag *tag;

	for (tag = hdr->pg[i].tag; tag; tag = tag->next) {
	    if (str[tag->idx+0] == 'P' && str[tag->idx+1] == 'P')
		break;
	}
	if (!tag) {
	    /* Chain start points */
	    continue;
	}

	hi = HashTableSearch(hdr->pg_hash, &str[tag->idx+3], tag->len-3);
	if (!hi) {
	    ret = -1;
	    continue;
	}

	hdr->pg[i].prev_id = hdr->pg[hi->data.i].id;
	hdr->pg_end[hi->data.i] = -1;
    }

    for (i = j = 0; i < hdr->npg; i++) {
	if (hdr->pg_end[i] != -1)
	    hdr->pg_end[j++] = hdr->pg_end[i];
    }
    hdr->npg_end = j;

    return ret;
}

/*
 * Returns a unique ID from a base name.
 *
 * The value returned is valid until the next call to
 * this function.
 */
char *sam_header_PG_ID(SAM_hdr *sh, char *name) {
    if (!(HashTableSearch(sh->pg_hash, name, 0)))
	return name;

    do {
	sprintf(sh->ID_buf, "%.1000s.%d", name, sh->ID_cnt++);
    } while (HashTableSearch(sh->pg_hash, sh->ID_buf, 0));

    return sh->ID_buf;
}

/*
 * Add an @PG line.
 *
 * If we wish complete control over this use sam_header_add() directly. This
 * function uses that, but attempts to do a lot of tedious house work for
 * you too.
 *
 * - It will generate a suitable ID if the supplied one clashes.
 * - It will generate multiple @PG records if we have multiple PG chains.
 *
 * Call it as per sam_header_add() with a series of key,value pairs ending
 * in NULL.
 *
 * Returns 0 on success
 *        -1 on failure
 */
int sam_header_add_PG(SAM_hdr *sh, char *name, ...) {
    va_list args;
    va_start(args, name);

    if (sh->npg_end) {
	/* Copy ends array to avoid us looping while modifying it */
	int *end = malloc(sh->npg_end * sizeof(int));
	int i, nends = sh->npg_end;
	char PP[1024];

	if (!end)
	    return -1;

	memcpy(end, sh->pg_end, nends * sizeof(*end));

	for (i = 0; i < nends; i++) {
	    int len = MIN(1023, sh->pg[end[i]].name_len);
	    strncpy(PP, sh->pg[end[i]].name, len);
	    PP[len] = 0;

	    sam_header_vadd(sh, "PG", args,
			    "ID", sam_header_PG_ID(sh, name),
			    "PN", name,
			    "PP", PP,
			    NULL);
	}

	free(end);
    } else {
	sam_header_vadd(sh, "PG", args,
			"ID", sam_header_PG_ID(sh, name),
			"PN", name,
			NULL);
    }

    //sam_header_dump(sh);

    return 0;
}

/*
 * A function to help with construction of CL tags in @PG records.
 * Takes an argc, argv pair and returns a single space-separated string.
 * This string should be deallocated by the calling function.
 * 
 * Returns malloced char * on success
 *         NULL on failure
 */
char *stringify_argv(int argc, char *argv[]) {
    char *str, *cp;
    size_t nbytes = 1;
    int i, j;

    /* Allocate */
    for (i = 0; i < argc; i++) {
	nbytes += strlen(argv[i]) + 1;
    }
    if (!(str = malloc(nbytes)))
	return NULL;

    /* Copy */
    cp = str;
    for (i = 0; i < argc; i++) {
	j = 0;
	while (argv[i][j]) {
	    if (argv[i][j] == '\t')
		*cp++ = ' ';
	    else
		*cp++ = argv[i][j];
	    j++;
	}
	*cp++ = ' ';
    }
    *cp++ = 0;

    return str;
}
