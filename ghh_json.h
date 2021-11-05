#ifndef GHH_JSON_H
#define GHH_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

// licensing info at end of file.

#include <stddef.h>
#include <stdio.h>
#include <stdbool.h>

typedef enum json_type {
    JSON_OBJECT,
    JSON_ARRAY,
    JSON_STRING,
    JSON_NUMBER,
    JSON_TRUE,
    JSON_FALSE,
    JSON_NULL
} json_type_e;

typedef struct json_object {
    union json_obj_data {
        struct json_hmap *hmap;
        struct json_vec *vec;
        char *string;
        double number;
    } data;

    json_type_e type;
} json_object_t;

typedef struct json {
    json_object_t *root;

    // allocators
    struct json_tptr **tracked; // fat pointer array of tracked pointers
    char **pages; // fat pointer
    size_t cur_tracked, tracked_cap; // tracks tracked pointers
    size_t cur_page, page_cap; // tracks allocator pages
    size_t used; // tracks current page stack
} json_t;

void json_load(json_t *, char *text);
void json_load_empty(json_t *);
void json_load_file(json_t *, const char *filepath);
void json_unload(json_t *);

// returns a string allocated with JSON_MALLOC
char *json_serialize(json_object_t *, bool mini, int indent, size_t *out_len);

// take an object, retrieve data and cast
json_object_t *json_get_object(json_object_t *, char *key);
// returns actual, mutable array pointer
json_object_t **json_get_array(json_object_t *, char *key, size_t *out_size);
char *json_get_string(json_object_t *, char *key);
double json_get_number(json_object_t *, char *key);
bool json_get_bool(json_object_t *, char *key);

// cast an object to a data type
json_object_t **json_to_array(json_object_t *, size_t *out_size);
char *json_to_string(json_object_t *);
double json_to_number(json_object_t *);
bool json_to_bool(json_object_t *);

// remove a json_object from another json_object (unordered)
json_object_t *json_pop(json_t *, json_object_t *, char *key);
// pop but ordered, this is O(n) rather than O(1) removal time
json_object_t *json_pop_ordered(json_t *, json_object_t *, char *key);

// add a json_object to another json_object
void json_put(json_t *, json_object_t *, char *key, json_object_t *child);

// equivalent to calling json_new_type() and then json_put().
// return the new object
json_object_t *json_put_object(json_t *, json_object_t *, char *key);
void json_put_array(
    json_t *, json_object_t *, char *key, json_object_t **objects, size_t size
);
void json_put_string(json_t *, json_object_t *, char *key, char *string);
void json_put_number(json_t *, json_object_t *, char *key, double number);
void json_put_bool(json_t *, json_object_t *, char *key, bool value);
void json_put_null(json_t *, json_object_t *, char *key);

// create a json data type on a json_t memory context
json_object_t *json_new_object(json_t *);
json_object_t *json_new_array(json_t *, json_object_t **objects, size_t size);
json_object_t *json_new_string(json_t *, char *string);
json_object_t *json_new_number(json_t *, double number);
json_object_t *json_new_bool(json_t *, bool value);
json_object_t *json_new_null(json_t *);

#ifdef GHH_JSON_IMPL

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

// errors + debugging ==========================================================

#ifdef JSON_DEBUG_INFO
#define JSON_DEBUG(...) printf(__VA_ARGS__)
#else
#define JSON_DEBUG(...)
#endif

#ifndef NDEBUG
#define JSON_ASSERT(cond, ...) if (!(cond)) JSON_ERROR(__VA_ARGS__)
#else
#define JSON_ASSERT(...)
#endif

#define JSON_ERROR(...)\
    do {\
        fprintf(stderr, "JSON ERROR: ");\
        fprintf(stderr, __VA_ARGS__);\
        exit(-1);\
    } while (0)

#define JSON_CTX_ERROR(ctx, ...)\
    do {\
        fprintf(stderr, "JSON ERROR: ");\
        fprintf(stderr, __VA_ARGS__);\
        json_contextual_error(ctx);\
        exit(-1);\
    } while (0)

#ifndef NDEBUG
// could do this with an X macro but I think it would reduce clarity
static const char *json_types[] = {
    "JSON_OBJECT",
    "JSON_ARRAY",
    "JSON_STRING",
    "JSON_NUMBER",
    "JSON_TRUE",
    "JSON_FALSE",
    "JSON_NULL"
};
#endif

typedef struct json_ctx {
    json_t *json;
    const char *text;
    size_t index;
} json_ctx_t;

static void json_contextual_error(json_ctx_t *ctx) {
    // get line number and line index
    size_t line = 1, line_index = 0;

    for (size_t i = 0; i < ctx->index; ++i) {
        if (ctx->text[i] == '\n') {
            ++line;
            line_index = i + 1;
        }
    }

    // get line length
    size_t i = line_index;
    int line_length = 0;

    while (ctx->text[i] != '\n' && ctx->text[i] != '\0') {
        ++line_length;
        ++i;
    }

    // print formatted line data
    printf("%6zu | %.*s\n", line, line_length, ctx->text + line_index);
    printf("%6s | %*s\n", "", (int)(ctx->index - line_index) + 1, "^");
}

// memory ======================================================================

#ifndef JSON_MALLOC
#define JSON_MALLOC(size) malloc(size)
#endif
#ifndef JSON_FREE
#define JSON_FREE(ptr) free(ptr)
#endif

// size of fread() buffer
#ifndef JSON_FREAD_BUF_SIZE
#define JSON_FREAD_BUF_SIZE 4096
#endif

// size of each json_t allocator page, increasing this results pretty directly
// in less cache misses
#ifndef JSON_PAGE_SIZE
#define JSON_PAGE_SIZE 65536
#endif

// initial sizes of stretchy buffers for json_t allocators
#define JSON_INIT_PAGE_CAP 8
#define JSON_INIT_TRACKED_CAP 256

// tracked pointer header
typedef struct json_tptr {
    size_t size, index;
} json_tptr_t;

// fat functions use fat pointers to track memory allocation with JSON_MALLOC
// and JSON_FREE. this is useful for allocating things that aren't on a json_t
// allocator
static void *json_fat_alloc(size_t size) {
    size_t *ptr = (size_t *)JSON_MALLOC(sizeof(*ptr) + size);

    *ptr++ = size;

    return ptr;
}

static inline void json_fat_free(void *ptr) {
    JSON_FREE((size_t *)ptr - 1);
}

static void *json_fat_realloc(void *ptr, size_t size) {
    void *new_ptr = json_fat_alloc(size);

    if (ptr) {
        // copy min(old_size, new_size)
        size_t copy_size = *((size_t *)ptr - 1);

        if (copy_size > size)
            copy_size = size;

        memcpy(new_ptr, ptr, copy_size);
        json_fat_free(ptr);
    }

    return new_ptr;
}

static void *json_tracked_alloc(json_t *json, size_t size) {
    // allocate tracked pointer
    json_tptr_t *tptr = (json_tptr_t *)JSON_MALLOC(sizeof(*tptr) + size);

    tptr->size = size;
    tptr->index = json->cur_tracked++;

    // push tracked pointer on tracked array
    if (json->cur_tracked == json->tracked_cap) {
        size_t old_cap = json->tracked_cap;

        json->tracked_cap <<= 1;
        json->tracked = (json_tptr_t **)json_fat_realloc(
            json->tracked,
            json->tracked_cap * sizeof(*json->tracked)
        );

        for (size_t i = old_cap; i < json->tracked_cap; ++i)
            json->tracked[i] = NULL;
    }

    json->tracked[tptr->index] = tptr;

    JSON_DEBUG("tracked alloc %zu.\n", tptr->index);

    return tptr + 1;
}

static void json_tracked_free(json_t *json, void *ptr) {
    size_t index = ((json_tptr_t *)ptr - 1)->index;

    JSON_FREE(json->tracked[index]);

    json->tracked[index] = NULL;

    JSON_DEBUG("tracked free %zu.\n", index);
}

// does NOT handle null pointers
static void *json_tracked_realloc(json_t *json, void *ptr, size_t size) {
    JSON_ASSERT(ptr, "json_tracked_realloc doesn't support null pointers.\n");

    // allocate new tracked pointer
    json_tptr_t *old_tptr = (json_tptr_t *)ptr - 1;
    json_tptr_t *new_tptr = (json_tptr_t *)JSON_MALLOC(
        sizeof(*new_tptr) + size
    );

    new_tptr->size = size;
    new_tptr->index = old_tptr->index;

    // copy data 
    size_t copy_size = old_tptr->size > new_tptr->size
        ? new_tptr->size : old_tptr->size;

    memcpy(new_tptr + 1, ptr, copy_size);

    // replace old_tptr in tracked array
    JSON_FREE(old_tptr);

    json->tracked[new_tptr->index] = new_tptr;

    JSON_DEBUG("tracked realloc %zu.\n", new_tptr->index);

    return new_tptr + 1;
}

// allocates on a json_t page
static void *json_page_alloc(json_t *json, size_t size) {
    // allocate new page when needed
    if (json->used + size > JSON_PAGE_SIZE) {
        if (size >= JSON_PAGE_SIZE) {
            JSON_DEBUG("allocating custom page and new page.\n");

            // size too big for pages, give this pointer its own page
            json->pages[++json->cur_page] = (char *)JSON_MALLOC(size);

            void *ptr = json->pages[json->cur_page];

            json->pages[++json->cur_page] = (char *)JSON_MALLOC(JSON_PAGE_SIZE);
            json->used = 0;

            return ptr;
        } else {
            JSON_DEBUG("allocating new page.\n");

            // allocate new page
            if (++json->cur_page == json->page_cap) {
                json->page_cap <<= 1;
                json->pages = (char **)json_fat_realloc(
                    json->pages,
                    json->page_cap * sizeof(*json->pages)
                );
            }

            json->pages[json->cur_page] = (char *)JSON_MALLOC(JSON_PAGE_SIZE);
            json->used = 0;
        }
    }

    // return page space
    void *ptr = json->pages[json->cur_page] + json->used;

    json->used += size;

    return ptr;
}

// array (vector) ==============================================================

#define JSON_VEC_INIT_CAP 8

typedef struct json_vec {
    void **data; // fat ptr
    size_t size, cap, min_cap;
} json_vec_t;

static void json_vec_alloc_one(json_t *json, json_vec_t *vec) {
    if (vec->size + 1 > vec->cap) {
        vec->cap <<= 1;
        vec->data = (void **)json_tracked_realloc(
            json,
            vec->data,
            vec->cap * sizeof(*vec->data)
        );
    }
}

static void json_vec_free_one(json_t *json, json_vec_t *vec) {
    JSON_ASSERT(vec->size, "popped from vec of size zero.\n");

    if (vec->cap > vec->min_cap && vec->size < vec->cap >> 2) {
        vec->cap >>= 1;
        vec->data = (void **)json_tracked_realloc(
            json,
            vec->data,
            vec->cap * sizeof(*vec->data)
        );
    }

    --vec->size;
}

static void json_vec_make(json_t *json, json_vec_t *vec, size_t init_cap) {
    vec->size = 0;
    vec->min_cap = vec->cap = init_cap;

    vec->data = (void **)json_tracked_alloc(
        json,
        vec->cap * sizeof(*vec->data)
    );
}

static void json_vec_push(json_t *json, json_vec_t *vec, void *item) {
    json_vec_alloc_one(json, vec);
    vec->data[vec->size++] = item;
}

static void *json_vec_pop(json_t *json, json_vec_t *vec) {
    json_object_t *object = (json_object_t *)vec->data[vec->size - 1];
    json_vec_free_one(json, vec);

    return object;
}

static void *json_vec_del(json_t *json, json_vec_t *vec, size_t index) {
    JSON_ASSERT(
        index < vec->size,
        "attempted to delete vec item past it's size.\n"
    );

    void *item = vec->data[index];

    vec->data[index] = json_vec_pop(json, vec);

    return item;
}

static void *json_vec_del_ordered(
    json_t *json, json_vec_t *vec, size_t index
) {
    JSON_ASSERT(
        index < vec->size,
        "attempted to delete vec item past it's size.\n"
    );

    void *item = vec->data[index];

    memcpy(
        vec->data + index,
        vec->data + index + 1,
        (vec->size - index - 1)  * sizeof(*vec->data)
    );

    json_vec_free_one(json, vec);

    return item;
}

// hashmap =====================================================================

#define JSON_HMAP_INIT_CAP 8

#if INTPTR_MAX == INT64_MAX
// 64 bit
typedef uint64_t json_hash_t;
#define JSON_FNV_PRIME 0x00000100000001b3
#define JSON_FNV_BASIS 0xcbf29ce484222325
#else
// 32 bit
typedef uint32_t json_hash_t;
#define JSON_FNV_PRIME 0x01000193a
#define JSON_FNV_BASIS 0x0811c9dc5
#endif

typedef struct json_hnode {
    json_object_t *object;
    json_hash_t hash;
    size_t index, steps;
} json_hnode_t;

typedef struct json_hmap {
    json_vec_t vec; // stores keys in order
    json_hnode_t *nodes; // fat ptr
    size_t size, cap, min_cap;
} json_hmap_t;

// fnv-1a hash function (http://isthe.com/chongo/tech/comp/fnv/)
static json_hash_t json_hash_str(char *str) {
    json_hash_t hash = JSON_FNV_BASIS;

    while (*str)
        hash = (hash ^ *str++) * JSON_FNV_PRIME;

    return hash;
}

static json_hnode_t *json_hnodes_alloc(json_t *json, size_t num_nodes) {
    json_hnode_t *nodes = (json_hnode_t *)json_tracked_alloc(
        json,
        num_nodes * sizeof(*nodes)
    );

    for (size_t i = 0; i < num_nodes; ++i)
        nodes[i].hash = 0;

    return nodes;
}

static void json_hmap_put_node(json_t *, json_hmap_t *, json_hnode_t *);

static void json_hmap_rehash(json_t *json, json_hmap_t *hmap, size_t new_cap) {
    json_hnode_t *old_nodes = hmap->nodes;
    size_t old_cap = hmap->cap;

    hmap->cap = new_cap;
    hmap->nodes = json_hnodes_alloc(json, hmap->cap);
    hmap->size = 0;

    for (size_t i = 0; i < old_cap; ++i) {
        if (old_nodes[i].hash) {
            old_nodes[i].index = old_nodes[i].hash % hmap->cap;
            old_nodes[i].steps = 0;

            json_hmap_put_node(json, hmap, &old_nodes[i]);
        }
    }

    json_tracked_free(json, old_nodes);
}

static inline void json_hmap_alloc_slot(json_t *json, json_hmap_t *hmap) {
    if (hmap->size + 1 > hmap->cap >> 1)
        json_hmap_rehash(json, hmap, hmap->cap << 1);

    ++hmap->size;
}

static inline void json_hmap_free_slot(json_t *json, json_hmap_t *hmap) {
    if (hmap->size < hmap->cap >> 2 && hmap->cap > hmap->min_cap)
        json_hmap_rehash(json, hmap, hmap->cap >> 1);

    --hmap->size;
}

static void json_hmap_make(json_t *json, json_hmap_t *hmap, size_t init_cap) {
    json_vec_make(json, &hmap->vec, init_cap);

    hmap->size = 0;
    hmap->cap = hmap->min_cap = init_cap;
    hmap->nodes = json_hnodes_alloc(json, hmap->cap);
}

static json_hnode_t *json_hmap_get_node(json_hmap_t *hmap, json_hash_t hash) {
    size_t index = hash % hmap->cap;

    // iterate through hash chain until match or empty node is found
    while (hmap->nodes[index].hash) {
        if (hmap->nodes[index].hash == hash)
            return &hmap->nodes[index];

        index = (index + 1) % hmap->cap;
    }

    return NULL;
}

// for rehash + put
static void json_hmap_put_node(
    json_t *json, json_hmap_t *hmap, json_hnode_t *node
) {
    size_t index = node->index;

    // find suitable bucket
    while (hmap->nodes[index].hash) {
        if (hmap->nodes[index].hash == node->hash) {
            // found matching bucket
            hmap->nodes[index].object = node->object;
            return;
        }

        index = (index + 1) % hmap->cap;
        ++node->steps;
    }

    // found empty bucket
    hmap->nodes[index] = *node;
    json_hmap_alloc_slot(json, hmap);
}

static void json_hmap_put(
    json_t *json, json_hmap_t *hmap, char *key, json_object_t *object
) {
    json_hnode_t node;

    json_vec_push(json, &hmap->vec, key);

    node.object = object;
    node.hash = json_hash_str(key);
    node.index = node.hash % hmap->cap;

    json_hmap_put_node(json, hmap, &node);
}

static json_object_t *json_hmap_get(json_hmap_t *hmap, char *key) {
    json_hnode_t *node = json_hmap_get_node(hmap, json_hash_str(key));

    return node ? node->object : NULL;
}

static json_object_t *json_hmap_del(
    json_t *json, json_hmap_t *hmap, char *key, bool order
) {
    json_hash_t hash = json_hash_str(key);
    size_t index = hash % hmap->cap;

    // find node
    while (hmap->nodes[index].hash != hash) {
        if (!hmap->nodes[index].hash)
            return NULL; // node doesn't exist

        index = (index + 1) % hmap->cap;
    }

    json_object_t *object = hmap->nodes[index].object;

    // replace chain
    size_t last = index, steps = 0;

    while (hmap->nodes[index = (index + 1) % hmap->cap].hash) {
        if (hmap->nodes[index].steps >= ++steps) {
            // found node that is a valid chain replacement
            hmap->nodes[last] = hmap->nodes[index];
            hmap->nodes[last].steps -= steps;
            last = index;
            steps = 0;
        }
    }

    // last node in chain is now a duplicate
    hmap->nodes[last].hash = 0;
    json_hmap_free_slot(json, hmap);

    // remove key from vec
    for (size_t i = 0; i < hmap->vec.size; ++i) {
        if (!strcmp(key, (char *)hmap->vec.data[i])) {
            if (order)
                json_vec_del_ordered(json, &hmap->vec, i);
            else
                json_vec_del(json, &hmap->vec, i);

            break;
        }
    }

    return object;
}

// parsing =====================================================================

// for mapping escape sequences
#define JSON_ESCAPE_CHARACTERS_X\
    X('"', '\"')\
    X('\\', '\\')\
    X('/', '/')\
    X('b', '\b')\
    X('f', '\f')\
    X('n', '\n')\
    X('r', '\r')\
    X('t', '\t')

static bool json_is_whitespace(char ch) {
    switch (ch) {
    case 0x20:
    case 0x0A:
    case 0x0D:
    case 0x09:
        return true;
    default:
        return false;
    }
}

static inline bool json_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

// skip whitespace to start of next token
static void json_next_token(json_ctx_t *ctx) {
    while (1) {
        if (!json_is_whitespace(ctx->text[ctx->index]))
            return;

        ++ctx->index;
    }
}

// compare next token with passed token
static bool json_token_equals(
    json_ctx_t *ctx, const char *token, size_t length
) {
    for (size_t i = 0; i < length; ++i)
        if (ctx->text[ctx->index + i] != token[i])
            return false;

    return true;
}

// error if next token does not equal passed token, otherwise skip
static void json_expect_token(
    json_ctx_t *ctx, const char *token, size_t length
) {
    if (!json_token_equals(ctx, token, length))
        JSON_CTX_ERROR(ctx, "unknown token, expected \"%s\".\n", token);

    ctx->index += length;
}

// error if next character is not a valid json string character, otherwise
// return char and skip
static char json_expect_str_char(json_ctx_t *ctx) {
    if (ctx->text[ctx->index] == '\\') {
        // escape codes
        char ch;

        switch (ctx->text[++ctx->index]) {
#define X(a, b) case a: ch = b; break;
        JSON_ESCAPE_CHARACTERS_X
#undef X
        case 'u':
            JSON_CTX_ERROR(
                ctx,
                "ghh_json does not support unicode escape sequences currently."
                "\n"
            );
        default:
            JSON_CTX_ERROR(
                ctx,
                "unknown character escape: '%c' (%hhX)\n",
                ctx->text[ctx->index], ctx->text[ctx->index]
            );
        }

        ++ctx->index;

        return ch;
    } else {
        // raw character
        return ctx->text[ctx->index++];
    }
}

// return string allocated on ctx allocator if valid string, otherwise error
static char *json_expect_string(json_ctx_t *ctx) {
    // verify string and count string length
    if (ctx->text[ctx->index++] != '\"')
        JSON_CTX_ERROR(ctx, "unknown token, expected string.\n");

    size_t start_index = ctx->index;
    size_t length = 0;

    while (ctx->text[ctx->index] != '\"') {
        switch (ctx->text[ctx->index]) {
        default:
            json_expect_str_char(ctx);
            ++length;

            break;
        case '\n':
        case '\0':
            JSON_CTX_ERROR(ctx, "string ended unexpectedly.\n");
        }
    }

    // read string
    char *str = (char *)json_page_alloc(ctx->json, (length + 1) * sizeof(*str));

    ctx->index = start_index;

    for (size_t i = 0; i < length; ++i)
        str[i] = json_expect_str_char(ctx);

    str[length] = '\0';

    ++ctx->index; // skip ending double quote

    return str;
}

static double json_expect_number(json_ctx_t *ctx) {
    // minus symbol
    bool negative = ctx->text[ctx->index] == '-';

    if (negative)
        ++ctx->index;

    // integral component
    double num = 0.0;

    if (!json_is_digit(ctx->text[ctx->index]))
        JSON_CTX_ERROR(ctx, "expected digit.\n");

    while (json_is_digit(ctx->text[ctx->index])) {
        num *= 10.0;
        num += ctx->text[ctx->index++] - '0';
    }

    // fractional component
    if (ctx->text[ctx->index] == '.') {
        ++ctx->index;

        if (!json_is_digit(ctx->text[ctx->index]))
            JSON_CTX_ERROR(ctx, "expected digit.\n");

        double fract = 0.0, mult = 1.0;

        while (json_is_digit(ctx->text[ctx->index])) {
            fract += (double)(ctx->text[ctx->index++] - '0') * mult;
            mult *= 0.1;
        }

        num += fract;
    }

    if (negative)
        num = -num;

    // exponential component
    if (ctx->text[ctx->index] == 'e' || ctx->text[ctx->index] == 'E') {
        ++ctx->index;

        // read exponent
        bool neg_exp = false;
        if (ctx->text[ctx->index] == '+' || ctx->text[ctx->index] == '-')
            neg_exp = ctx->text[ctx->index++] == '-';

        if (!json_is_digit(ctx->text[ctx->index]))
            JSON_CTX_ERROR(ctx, "expected digit.\n");

        long exponent = 0;

        while (json_is_digit(ctx->text[ctx->index])) {
            exponent *= 10;
            exponent += ctx->text[ctx->index++] - '0';
        }

        // calculate
        if (neg_exp) {
            while (exponent--)
                num *= 0.1;
        } else {
            while (exponent--)
                num *= 10.0;
        }
    }

    return num;
}

static json_object_t *json_expect_obj(json_ctx_t *, json_object_t *);
static json_object_t *json_expect_array(json_ctx_t *, json_object_t *);

// fills object in with value
static void json_expect_value(json_ctx_t *ctx, json_object_t *object) {
    switch (ctx->text[ctx->index]) {
    case '{':
        json_expect_obj(ctx, object);
        object->type = JSON_OBJECT;

        break;
    case '[':
        json_expect_array(ctx, object);
        object->type = JSON_ARRAY;

        break;
    case '"':
        object->data.string = json_expect_string(ctx);
        object->type = JSON_STRING;

        break;
    case 't':
        json_expect_token(ctx, "true", 4);
        object->type = JSON_TRUE;

        break;
    case 'f':
        json_expect_token(ctx, "false", 5);
        object->type = JSON_FALSE;

        break;
    case 'n':
        json_expect_token(ctx, "null", 4);
        object->type = JSON_NULL;

        break;
    default:;
        // could be number
        if (json_is_digit(ctx->text[ctx->index])
         || ctx->text[ctx->index] == '-') {
            object->data.number = json_expect_number(ctx);
            object->type = JSON_NUMBER;

            break;
        }

        JSON_CTX_ERROR(ctx, "unknown token, expected value.\n");
    }
}

static json_object_t *json_expect_array(
    json_ctx_t *ctx, json_object_t *object
) {
    json_vec_t *vec = (json_vec_t *)json_page_alloc(ctx->json, sizeof(*vec));
    json_vec_make(ctx->json, vec, JSON_VEC_INIT_CAP);

    object->data.vec = vec;

    ++ctx->index; // skip '['

    // check for empty array
    json_next_token(ctx);

    if (ctx->text[ctx->index] == ']') {
        ++ctx->index;

        return object;
    }

    // parse key/value pairs
    while (1) {
        // get child and store
        json_object_t *child = (json_object_t *)json_page_alloc(
            ctx->json,
            sizeof(*child)
        );

        json_expect_value(ctx, child);

        json_vec_push(ctx->json, vec, child);

        // iterate
        json_next_token(ctx);

        if (ctx->text[ctx->index] == ']') {
            ++ctx->index;
            break;
        }

        json_expect_token(ctx, ",", 1);
        json_next_token(ctx);
    }

    return object;
}

static json_object_t *json_expect_obj(json_ctx_t *ctx, json_object_t *object) {
    json_hmap_t *hmap = (json_hmap_t *)json_page_alloc(
        ctx->json,
        sizeof(*hmap)
    );
    json_hmap_make(ctx->json, hmap, JSON_HMAP_INIT_CAP);

    object->data.hmap = hmap;

    ++ctx->index; // skip '{'

    // check for empty object
    json_next_token(ctx);

    if (ctx->text[ctx->index] == '}') {
        ++ctx->index;

        return object;
    }

    // parse key/value pairs
    while (1) {
        // get pair and store
        json_object_t *child = (json_object_t *)json_page_alloc(
            ctx->json,
            sizeof(*object)
        );

        char *key = json_expect_string(ctx);

        json_next_token(ctx);
        json_expect_token(ctx, ":", 1);
        json_next_token(ctx);
        json_expect_value(ctx, child);

        json_hmap_put(ctx->json, hmap, key, child);

        // iterate
        json_next_token(ctx);

        if (ctx->text[ctx->index] == '}') {
            ++ctx->index;
            break;
        }

        json_expect_token(ctx, ",", 1);
        json_next_token(ctx);
    }

    return object;
}

static void json_parse(json_t *json, const char *text) {
    json_ctx_t ctx;

    ctx.json = json;
    ctx.text = text;
    ctx.index = 0;

    // recursive parse at root
    json_next_token(&ctx);

    switch (ctx.text[ctx.index]) {
    case '{':
        json->root = (json_object_t *)json_page_alloc(
            ctx.json,
            sizeof(*json->root)
        );

        json_expect_obj(&ctx, json->root);
        json->root->type = JSON_OBJECT;

        json_next_token(&ctx);
        json_expect_token(&ctx, "", 1);

        break;
    case '[':
        json->root = (json_object_t *)json_page_alloc(
            ctx.json,
            sizeof(*json->root)
        );

        json_expect_array(&ctx, json->root);
        json->root->type = JSON_ARRAY;

        json_next_token(&ctx);
        json_expect_token(&ctx, "", 1);

        break;
    case '\0': // empty json is still valid json
        json->root = NULL;

        break;
    default:
        JSON_CTX_ERROR(&ctx, "invalid json root.\n");
    }
}

// lifetime api ================================================================

void json_load_empty(json_t *json) {
    json->root = NULL;

    // page allocator
    json->cur_page = json->used = 0;
    json->page_cap = JSON_INIT_PAGE_CAP;
    json->pages = (char **)json_fat_alloc(
        json->page_cap * sizeof(*json->pages)
    );

    json->pages[0] = (char *)JSON_MALLOC(JSON_PAGE_SIZE);

    // tracking allocator
    json->cur_tracked = 0;
    json->tracked_cap = JSON_INIT_TRACKED_CAP;
    json->tracked = (json_tptr_t **)json_fat_alloc(
        json->tracked_cap * sizeof(*json->tracked)
    );

    JSON_DEBUG("tracked size %zu.\n", *((size_t *)json->tracked - 1));

    for (size_t i = 0; i < json->tracked_cap; ++i)
        json->tracked[i] = NULL;
}

static void json_parse(json_t *json, const char *text);

void json_load(json_t *json, char *text) {
    json_load_empty(json);
    json_parse(json, text);
}

void json_load_file(json_t *json, const char *filepath) {
    JSON_DEBUG("reading\n");

    // open and check for existance
    FILE *file = fopen(filepath, "r");

    if (!file)
        JSON_ERROR("could not open file: \"%s\"\n", filepath);

    // read text through buffer
    char *text = NULL;
    char *buffer = (char *)JSON_MALLOC(JSON_FREAD_BUF_SIZE);
    size_t text_len = 0;
    bool done = false;

    do {
        size_t num_read = fread(
            buffer,
            sizeof(*buffer),
            JSON_FREAD_BUF_SIZE,
            file
        );

        if (num_read != JSON_FREAD_BUF_SIZE) {
            done = true;
            ++num_read;
        }

        // copy buffer into doc->text
        size_t last_len = text_len;

        text_len += num_read;
        text = (char *)json_fat_realloc(text, text_len);

        memcpy(text + last_len, buffer, num_read * sizeof(*buffer));
    } while (!done);

    text[text_len - 1] = '\0';

    free(buffer);

    // load and cleanup
    JSON_DEBUG("loading\n");

    json_load(json, text);

    json_fat_free(text);
    fclose(file);
}

// recursively free object hashmap and array vectors
void json_unload(json_t *json) {
    // free pages
    for (size_t i = 0; i <= json->cur_page; ++i)
        JSON_FREE(json->pages[i]);

    json_fat_free(json->pages);

    // free tracked
    for (size_t i = 0; i <= json->cur_tracked; ++i)
        if (json->tracked[i])
            JSON_FREE(json->tracked[i]);

    json_fat_free(json->tracked);
}

// serialization api ===========================================================

#ifndef JSON_SERIALIZER_BUF_SIZE
#define JSON_SERIALIZER_BUF_SIZE 1024
#endif

#define JSON_STRINGY_INIT_CAP 256

// string builder
typedef struct json_stringy {
    char *str;
    size_t pos, cap;
} json_stringy_t;

// serialization context
typedef struct json_serializer {
    json_stringy_t stringy;
    char *buf;
    int level;

    int indent, nlwidth;
    bool mini;
} json_serializer_t;

static void json_stringy_make(json_stringy_t *stringy) {
    stringy->cap = JSON_STRINGY_INIT_CAP;
    stringy->str = (char *)json_fat_alloc(stringy->cap);
    stringy->pos = 0;
}

static inline void json_stringy_kill(json_stringy_t *stringy) {
    json_fat_free(stringy->str);
}

static void json_stringy_append(
    json_stringy_t *stringy, const char *str, size_t len
) {
    if (stringy->pos + len >= stringy->cap) {
        stringy->cap <<= 1;
        stringy->str = (char *)json_fat_realloc(
            stringy->str,
            stringy->cap * sizeof(*stringy->str)
        );
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    strncpy(stringy->str + stringy->pos, str, len);
#pragma GCC diagnostic pop

    stringy->pos += len;
}

static void json_serialize_string(json_serializer_t *ser_ctx, char *str) {
    json_stringy_append(&ser_ctx->stringy, "\"", 1);

    while (*str) {
        switch (*str) {
#define X(a, b)\
        case b:\
            sprintf(ser_ctx->buf, "\\%c", a);\
            json_stringy_append(&ser_ctx->stringy, ser_ctx->buf, 2);\
            break;

        JSON_ESCAPE_CHARACTERS_X
#undef X
        default:
            json_stringy_append(&ser_ctx->stringy, str, 1);\
            break;
        }

        ++str;
    }

    json_stringy_append(&ser_ctx->stringy, "\"", 1);
}

static inline void json_serialize_indent(json_serializer_t *ser_ctx) {
    if (!ser_ctx->mini) {
        int indent = ser_ctx->level * ser_ctx->indent;

        sprintf(ser_ctx->buf, "%*s", indent, "");
        json_stringy_append(&ser_ctx->stringy, ser_ctx->buf, indent);
    }
}

static void json_serialize_array(json_serializer_t *, json_object_t *);
static void json_serialize_obj(json_serializer_t *, json_object_t *);

static void json_serialize_value(
    json_serializer_t *ser_ctx, json_object_t *object
) {
    switch (object->type) {
    case JSON_OBJECT:
        json_serialize_obj(ser_ctx, object);

        break;
    case JSON_ARRAY:
        json_serialize_array(ser_ctx, object);

        break;
    case JSON_STRING:
        json_serialize_string(ser_ctx, object->data.string);

        break;
    case JSON_NUMBER:
        if ((long)object->data.number == object->data.number)
            sprintf(ser_ctx->buf, "%ld", (long)object->data.number);
        else
            sprintf(ser_ctx->buf, "%lf", object->data.number);

        json_stringy_append(
            &ser_ctx->stringy,
            ser_ctx->buf,
            strlen(ser_ctx->buf)
        );

        break;
    case JSON_TRUE:
        json_stringy_append(&ser_ctx->stringy, "true", 4);

        break;
    case JSON_FALSE:
        json_stringy_append(&ser_ctx->stringy, "false", 5);

        break;
    case JSON_NULL:
        json_stringy_append(&ser_ctx->stringy, "null", 4);

        break;
    }
}

static void json_serialize_array(
    json_serializer_t *ser_ctx, json_object_t *object
) {
    json_stringy_append(&ser_ctx->stringy, "[\n", ser_ctx->nlwidth);

    ++ser_ctx->level;

    for (size_t i = 0; i < object->data.vec->size; ++i) {
        if (i)
            json_stringy_append(&ser_ctx->stringy, ",\n", ser_ctx->nlwidth);

        json_serialize_indent(ser_ctx);
        json_serialize_value(
            ser_ctx,
            (json_object_t *)object->data.vec->data[i]
        );
    }

    if (!ser_ctx->mini)
        json_stringy_append(&ser_ctx->stringy, "\n", 1);

    --ser_ctx->level;

    json_serialize_indent(ser_ctx);
    json_stringy_append(&ser_ctx->stringy, "]", 1);
}

static void json_serialize_obj(
    json_serializer_t *ser_ctx, json_object_t *object
) {
    json_stringy_append(&ser_ctx->stringy, "{\n", ser_ctx->nlwidth);

    ++ser_ctx->level;

    json_hmap_t *hmap = object->data.hmap;
    json_vec_t *vec = &hmap->vec;

    for (size_t i = 0; i < vec->size; ++i) {
        if (i) {
            json_stringy_append(&ser_ctx->stringy, ",\n", ser_ctx->nlwidth);
        }

        json_serialize_indent(ser_ctx);
        json_serialize_string(ser_ctx, (char *)vec->data[i]);
        json_stringy_append(&ser_ctx->stringy, ": ", ser_ctx->nlwidth);

        json_serialize_value(
            ser_ctx,
            json_hmap_get(hmap, (char *)vec->data[i])
        );
    }

    if (!ser_ctx->mini)
        json_stringy_append(&ser_ctx->stringy, "\n", 1);

    --ser_ctx->level;

    json_serialize_indent(ser_ctx);
    json_stringy_append(&ser_ctx->stringy, "}", 1);
}

char *json_serialize(
    json_object_t *object, bool mini, int indent, size_t *out_len
) {
    if (!object)
        JSON_ERROR("attempted to serialize a NULL object.\n");

    // create and use serializer
    json_serializer_t ser_ctx;

    json_stringy_make(&ser_ctx.stringy);

    ser_ctx.buf = (char *)JSON_MALLOC(JSON_SERIALIZER_BUF_SIZE);
    ser_ctx.level = 0;
    ser_ctx.indent = indent;
    ser_ctx.mini = mini;
    ser_ctx.nlwidth = ser_ctx.mini ? 1 : 2;

    json_serialize_value(&ser_ctx, object);
    json_stringy_append(&ser_ctx.stringy, "\n", 2);

    JSON_FREE(ser_ctx.buf);

    // return serialized string
    size_t len = ser_ctx.stringy.pos;
    char *serialized = (char *)JSON_MALLOC((len + 1) * sizeof(*serialized));

    if (out_len)
        *out_len = len;

    strcpy(serialized, ser_ctx.stringy.str);
    json_stringy_kill(&ser_ctx.stringy);

    return serialized;
}

// get/put/to api functions ====================================================

#define JSON_ASSERT_PROPER_CAST(json_type)\
    JSON_ASSERT(\
        object->type == json_type,\
        "attempted to cast %s to %s.\n",\
        json_types[object->type], json_types[json_type]\
    )

static inline json_object_t *json_empty_object(json_t *json) {
    return (json_object_t *)json_page_alloc(json, sizeof(json_object_t));
}

json_object_t *json_get_object(json_object_t *object, char *key) {
    JSON_ASSERT(
        object->type == JSON_OBJECT,
        "attempted to get child \"%s\" from a non-object.\n",
        key
    );

    return json_hmap_get(object->data.hmap, key);
}

json_object_t **json_get_array(
    json_object_t *object, char *key, size_t *out_size
) {
    return json_to_array(json_get_object(object, key), out_size);
}

char *json_get_string(json_object_t *object, char *key) {
    return json_to_string(json_get_object(object, key));
}

double json_get_number(json_object_t *object, char *key) {
    return json_to_number(json_get_object(object, key));
}

bool json_get_bool(json_object_t *object, char *key) {
    return json_to_bool(json_get_object(object, key));
}

json_object_t **json_to_array(json_object_t *object, size_t *out_size) {
    JSON_ASSERT_PROPER_CAST(JSON_ARRAY);

    json_vec_t *vec = object->data.vec;

    if (out_size)
        *out_size = vec->size;

    return (json_object_t **)vec->data;
}

char *json_to_string(json_object_t *object) {
    JSON_ASSERT_PROPER_CAST(JSON_STRING);

    return object->data.string;
}

double json_to_number(json_object_t *object) {
    JSON_ASSERT_PROPER_CAST(JSON_NUMBER);

    return object->data.number;
}

bool json_to_bool(json_object_t *object) {
    JSON_ASSERT(
        object->type == JSON_TRUE || object->type == JSON_FALSE,
        "attempted to cast %s to bool.\n",
        json_types[object->type]
    );

    return object->type == JSON_TRUE;
}

json_object_t *json_pop(json_t *json, json_object_t *object, char *key) {
    return json_hmap_del(json, object->data.hmap, key, false);
}

json_object_t *json_pop_ordered(
    json_t *json, json_object_t *object, char *key
) {
    return json_hmap_del(json, object->data.hmap, key, true);
}

json_object_t *json_new_object(json_t *json) {
    json_object_t *object = json_empty_object(json);

    object->type = JSON_OBJECT;
    object->data.hmap = (json_hmap_t *)json_page_alloc(
        json,
        sizeof(*object->data.hmap)
    );

    json_hmap_make(json, object->data.hmap, JSON_HMAP_INIT_CAP);

    return object;
}

json_object_t *json_new_array(
    json_t *json, json_object_t **objects, size_t size
) {
    json_object_t *object = json_empty_object(json);

    object->type = JSON_ARRAY;
    object->data.vec = (json_vec_t *)json_page_alloc(
        json,
        sizeof(*object->data.vec)
    );

    json_vec_make(json, object->data.vec, size);

    for (size_t i = 0; i < size; ++i)
        json_vec_push(json, object->data.vec, objects[i]);

    return object;
}

json_object_t *json_new_string(json_t *json, char *string) {
    json_object_t *object = json_empty_object(json);

    object->type = JSON_STRING;
    object->data.string = string;

    return object;
}

json_object_t *json_new_number(json_t *json, double number) {
    json_object_t *object = json_empty_object(json);

    object->type = JSON_NUMBER;
    object->data.number = number;

    return object;
}

json_object_t *json_new_bool(json_t *json, bool value) {
    json_object_t *object = json_empty_object(json);

    object->type = value ? JSON_TRUE : JSON_FALSE;

    return object;
}

json_object_t *json_new_null(json_t *json) {
    json_object_t *object = json_empty_object(json);

    object->type = JSON_NULL;

    return object;
}

void json_put(
    json_t *json, json_object_t *object, char *key, json_object_t *child
) {
    JSON_ASSERT(
        object->type == JSON_OBJECT,
        "called put_object on a non-object.\n"
    );

    json_hmap_put(json, object->data.hmap, key, child);
}

json_object_t *json_put_object(
    json_t *json, json_object_t *object, char *key
) {
    json_object_t *child = json_new_object(json);

    json_put(json, object, key, child);

    return child;
}

void json_put_array(
    json_t *json, json_object_t *object, char *key,
    json_object_t **objects, size_t size
) {
    json_put(json, object, key, json_new_array(json, objects, size));
}

void json_put_string(
    json_t *json, json_object_t *object, char *key, char *string
) {
    json_put(json, object, key, json_new_string(json, string));
}

void json_put_number(
    json_t *json, json_object_t *object, char *key, double number
) {
    json_put(json, object, key, json_new_number(json, number));
}

void json_put_bool(json_t *json, json_object_t *object, char *key, bool value) {
    json_put(json, object, key, json_new_bool(json, value));
}

void json_put_null(json_t *json, json_object_t *object, char *key) {
    json_put(json, object, key, json_new_null(json));
}

#endif // GHH_JSON_IMPL

#ifdef __cplusplus
}
#endif

#endif // GHH_JSON_H

/*

The MIT License (MIT)

Copyright (c) 2021 garrisonhh / Garrison Hinson-Hasty

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the “Software”), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

*/
