# ghh_json.h

a single-header ISO-C99 json loader.

## why?

obviously this isn't the first json library
written for C, so why would I write this?

1. easy build
there are great json C libraries with clean APIs and many more features than my
implementation out there, but they require build system integration which
immediately adds work I don't want to deal with.

2. easy usage
there are other single-header json libraries for C out there, but I wasn't super
into the way they are structured. I wrote this the way I would expect a json
library to behave. this is more of a taste thing than anything, if you don't
like how ghh_json works check out another implementation.

## usage

###### note: ghh_json does not currently support unicode escape sequences, like "\u0123". otherwise it is json specification conforming.

to include in your project:

```c
#define GHH_JSON_IMPL
#include "ghh_json.h"
```

the basic program structure looks like this:

```c
json_t json;
json_load_file(&json, "mydata.json");

// do stuff ...

json_unload(&json);
json_load(&json, "{ \"raw\": \"json data.\"}");

// do stuff...

json_unload(&json);
```

there are only 3 types you need to think about:
- `json_t`, a reusable memory context for json objects
  - access root element through `.root`.
- `json_type_e`, json type enum
- types are: `JSON_OBJECT, JSON_ARRAY, JSON_STRING, JSON_NUMBER, JSON_TRUE,
  JSON_FALSE, JSON_NULL`
- `json_object_t`, a tagged union
  - access type through `.type`
  - access data using `json_get` and `json_to` functions

## api

### settings

```c
// define your own allocation functions
#define JSON_MALLOC(size)
#define JSON_FREE(size)

// change the size of the char buffer for fread().
#define JSON_FREAD_BUF_SIZE

// the json_t allocator works by allocating pages to accommodate objects and
// data. increasing this means less allocations during parsing.
#define JSON_PAGE_SIZE
```

### json_t lifetime

```c
// load json from a string.
void json_load(json_t *, char *text);
// load json from a file.
void json_load_file(json_t *, const char *filepath);
// free all memory associated with loaded json.
void json_unload(json_t *);
```

### data access

```c
// retrieve a key from an object.
// if NDEBUG is not defined, will type check the root object.
json_object_t *json_get_object(json_object_t *, char *key);
json_object_t **json_get_array(json_object_t *, char *key, size_t *out_size);
char *json_get_string(json_object_t *, char *key);
double json_get_number(json_object_t *, char *key);
bool json_get_bool(json_object_t *, char *key);

// cast an object to a type.
// if NDEBUG is not defined, will type check the object.
json_object_t **json_to_array(json_object_t *, size_t *out_size);
char *json_to_string(json_object_t *);
double json_to_number(json_object_t *);
bool json_to_bool(json_object_t *);
```

### io

```c
// prints valid json given a root object.
void json_print(json_object_t *);
void json_fprint(FILE *, json_object_t *);
```
