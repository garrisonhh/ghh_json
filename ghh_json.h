#ifndef GHH_JSON_H
#define GHH_JSON_H

#include <stddef.h>

typedef struct json_object {
	struct json_object *next;

	const char *key; // may or may not be null

	enum json_object_type {
		JSON_OBJECT,
		JSON_ARRAY,
		JSON_NUMBER,
		JSON_STRING,
		JSON_TRUE,
		JSON_FALSE,
		JSON_NULL
	} type;

	union json_object_data {
		struct json_object *objects;
		char *string;
		double number;
	} data;
} json_object_t;

typedef struct json {
	json_object_t *root;

	// allocator
	char **pages;
	size_t cur_page, page_cap; // tracks allocator pages
	size_t used; // tracks current page stack
} json_t;

void json_load(json_t *, char *text);
void json_load_file(json_t *, const char *filepath);
void json_unload(json_t *);

void json_print(json_t *);

#ifdef GHH_JSON_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

// memory control
#ifndef JSON_MALLOC
#define JSON_MALLOC(size) malloc(size)
#endif
#ifndef JSON_FREE
#define JSON_FREE(size) free(size)
#endif

// size of stack-allocated file reading buffer
#ifndef JSON_FREAD_BUF_SIZE
#define JSON_FREAD_BUF_SIZE 1024
#endif

// size of each json_t allocator page
#ifndef JSON_PAGE_SIZE
#define JSON_PAGE_SIZE 4096
#endif

#define JSON_INIT_PAGE_CAP 8

#ifdef JSON_DEBUG_INFO
#define JSON_DEBUG(...) printf(__VA_ARGS__)
#else
#define JSON_DEBUG(...)
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

typedef struct json_parse_ctx {
	json_t *json;
	const char *text;
	size_t index;
} json_parse_ctx_t;

// fat functions use fat pointers to track memory allocation with JSON_MALLOC
// and JSON_FREE. this is useful for allocating things that aren't on a json_t
// allocator
static void *json_fat_alloc(size_t size) {
	size_t *ptr = JSON_MALLOC(sizeof(*ptr) + size);

	*ptr = size;

	return (void *)(ptr + 1);
}

static inline void json_fat_free(void *ptr) {
	JSON_FREE((size_t *)ptr - 1);
}

static void *json_fat_realloc(void *ptr, size_t size) {
	void *new_ptr = json_fat_alloc(size);

	if (ptr) {
		memcpy(new_ptr, ptr, *((size_t *)ptr - 1));
		json_fat_free(ptr);
	}

	return new_ptr;
}

static void json_contextual_error(json_parse_ctx_t *ctx) {
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
	printf("%6s | %*s\n", "", (int)(ctx->index - line_index), "^");
}

// internal json_t initializer
static void json_make(json_t *json) {
	*json = (json_t){
		.root = NULL,
		.page_cap = JSON_INIT_PAGE_CAP,
		.pages = json_fat_alloc(JSON_INIT_PAGE_CAP * sizeof(*json->pages))
	};

	json->pages[0] = JSON_MALLOC(JSON_PAGE_SIZE);
}

static void json_parse(json_t *json, const char *text);

void json_load(json_t *json, char *text) {
	json_make(json);
	json_parse(json, text);
}

void json_unload(json_t *json) {
	for (size_t i = 0; i <= json->cur_page; ++i)
		JSON_FREE(json->pages[i]);

	json_fat_free(json->pages);
}

void json_load_file(json_t *json, const char *filepath) {
    // open and check for existance
    FILE *file = fopen(filepath, "r");

    if (!file)
        JSON_ERROR("could not open file: \"%s\"\n", filepath);

    // read text through buffer
    char *text = NULL;
    char buffer[JSON_FREAD_BUF_SIZE];
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
        text = json_fat_realloc(text, text_len);

        memcpy(text + last_len, buffer, num_read * sizeof(*buffer));
    } while (!done);

    text[text_len - 1] = '\0';

	JSON_DEBUG("--- text ---\n%s\n", text);

	// load and cleanup
	json_load(json, text);

	json_fat_free(text);
    fclose(file);
}

// allocates on a json_t stack
static void *json_alloc(json_t *json, size_t size) {
	// allocate new page when needed
	if (json->used + size > JSON_PAGE_SIZE) {
		// allocate more space for page ptrs when needed
		if (++json->cur_page == json->page_cap) {
			json->page_cap <<= 1;
			json->pages = json_fat_realloc(
				json->pages,
				json->page_cap * sizeof(*json->pages)
			);
		}

		json->pages[json->cur_page] = JSON_MALLOC(JSON_PAGE_SIZE);
		json->used = 0;
	}

	// return page space
	void *ptr = json->pages[json->cur_page] + json->used;

	json->used += size;

	return ptr;
}

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

// skip whitespace to start of next token
static void json_next_token(json_parse_ctx_t *ctx) {
	while (1) {
		if (!json_is_whitespace(ctx->text[ctx->index]))
			return;

		++ctx->index;
	}
}

// compare next token with passed token
static bool json_token_equals(
	json_parse_ctx_t *ctx, const char *token, size_t length
) {
	for (size_t i = 0; i < length; ++i)
		if (ctx->text[ctx->index + i] != token[i])
			return false;

	return true;
}

// error if next token does not equal passed token, otherwise skip
static void json_expect_token(
	json_parse_ctx_t *ctx, const char *token, size_t length
) {
	if (!json_token_equals(ctx, token, length))
		JSON_CTX_ERROR(ctx, "unknown token, expected \"%s\"/\n", token);

	ctx->index += length;
}

// error if next character is not a valid json string character, otherwise
// return char and skip
static char json_expect_str_char(json_parse_ctx_t *ctx) {
	if (ctx->text[ctx->index] == '\\') {
		// escape codes
		char ch;

		switch (ctx->text[++ctx->index]) {
#define ESC_CASE(a, b) case a: ch = b; break
		ESC_CASE('"', '\"');
		ESC_CASE('\\', '\\');
		ESC_CASE('/', '/');
		ESC_CASE('b', '\b');
		ESC_CASE('f', '\f');
		ESC_CASE('n', '\n');
		ESC_CASE('r', '\r');
		ESC_CASE('t', '\t');
#undef ESC_CASE
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
static char *json_expect_string(json_parse_ctx_t *ctx) {
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
	char *str = json_alloc(ctx->json, (length + 1) * sizeof(*str));

	ctx->index = start_index;

	for (size_t i = 0; i < length; ++i)
		str[i] = json_expect_str_char(ctx);

	str[length] = '\0';

	++ctx->index; // skip ending double quote

	return str;
}

static json_object_t *json_expect_object(json_parse_ctx_t *, json_object_t *);
static json_object_t *json_expect_array(json_parse_ctx_t *, json_object_t *);

// fills object in with value
static void json_expect_value(json_parse_ctx_t *ctx, json_object_t *obj) {
	switch (ctx->text[ctx->index]) {
	case '{':
		json_expect_object(ctx, obj);
		obj->type = JSON_OBJECT;

		break;
	case '[':
		json_expect_array(ctx, obj);
		obj->type = JSON_ARRAY;

		break;
	case '"':
		obj->data.string = json_expect_string(ctx);
		obj->type = JSON_STRING;

		break;
	case 't':
		json_expect_token(ctx, "true", 4);
		obj->type = JSON_TRUE;

		break;
	case 'f':
		json_expect_token(ctx, "false", 5);
		obj->type = JSON_FALSE;

		break;
	case 'n':
		json_expect_token(ctx, "null", 4);
		obj->type = JSON_NULL;

		break;
	default:
		JSON_CTX_ERROR(ctx, "unknown token, expected value.\n");
	}
}

static json_object_t *json_object_create(json_parse_ctx_t *ctx) {
	json_object_t *obj = json_alloc(ctx->json, sizeof(*obj));

	obj->next = obj->data.objects = NULL;

	return obj;
}

static json_object_t *json_expect_array(
	json_parse_ctx_t *ctx, json_object_t *obj
) {
	json_object_t **tail = &obj->data.objects;

	++ctx->index; // skip '['

	// parse key/value pairs
	while (1) {
		// get pair
		json_object_t *child = json_alloc(ctx->json, sizeof(*obj));

		json_next_token(ctx);
		json_expect_value(ctx, child);

		child->key = NULL;
		child->next = NULL;

		// store pair on parent list
		*tail = child;
		tail = &child->next;

		// iterate
		json_next_token(ctx);

		if (ctx->text[ctx->index] == ']') {
			++ctx->index;
			break;
		}

		json_expect_token(ctx, ",", 1);
	}

	return obj;
}

static json_object_t *json_expect_object(
	json_parse_ctx_t *ctx, json_object_t *obj
) {
	json_object_t **tail = &obj->data.objects;

	++ctx->index; // skip '{'

	// parse key/value pairs
	while (1) {
		// get pair
		json_object_t *child = json_alloc(ctx->json, sizeof(*obj));

		json_next_token(ctx);

		child->key = json_expect_string(ctx);
		child->next = NULL;

		json_next_token(ctx);
		json_expect_token(ctx, ":", 1);
		json_next_token(ctx);
		json_expect_value(ctx, child);

		// store pair on parent list
		*tail = child;
		tail = &child->next;

		// iterate
		json_next_token(ctx);

		if (ctx->text[ctx->index] == '}') {
			++ctx->index;
			break;
		}

		json_expect_token(ctx, ",", 1);
	}

	return obj;
}

static void json_parse(json_t *json, const char *text) {
	json_parse_ctx_t ctx = {
		.json = json,
		.text = text,
		.index = 0
	};

	// start recursive parse at root
	json_next_token(&ctx);

	switch (ctx.text[ctx.index]) {
	case '{':
		json->root = json_object_create(&ctx);
		json_expect_object(&ctx, json->root);
		json->root->type = JSON_OBJECT;

		break;
	case '[':
		json->root = json_object_create(&ctx);
		json_expect_array(&ctx, json->root);
		json->root->type = JSON_ARRAY;

		break;
	case '\0': // empty json is still valid json
		json->root = NULL;

		break;
	default:
		JSON_CTX_ERROR(&ctx, "invalid json root.\n");
	}
}

static void json_print_level(int level) {
	printf("%*s", level << 2, "");
}

static void json_print_lower(json_object_t *obj, int level) {
	// print start of collection
	printf(obj->type == JSON_OBJECT ? "{\n" : "[\n");

	// print children
	++level;

	for (json_object_t *trav = obj->data.objects; trav; trav = trav->next) {
		json_print_level(level);

		if (trav->key)
			printf("\"%s\": ", trav->key);
	
		switch (trav->type) {
		case JSON_OBJECT:
		case JSON_ARRAY:
			json_print_lower(trav, level);

			break;
		case JSON_STRING:
			printf("\"%s\"\n", trav->data.string);

			break;
		default:
			printf("%%UNHANDLED%%\n");
			break;
		}
	}

	--level;

	// print end of collection
	json_print_level(level);
	printf(obj->type == JSON_OBJECT ? "}\n" : "]\n");
}

void json_print(json_t *json) {
	json_print_lower(json->root, 0);
}

#endif // GHH_JSON_IMPL
#endif // GHH_JSON_H
