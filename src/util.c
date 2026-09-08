// Small shared helpers: allocation, string formatting, and a hash map.
//
// A note on memory: cinder never frees. A compiler is a batch process that
// reads one translation unit and exits, so every allocation is live until the
// end anyway, and the arena the OS gives back on exit is faster and far more
// reliable than tracking ownership through an AST. This is what most real
// compilers do for the front end, and it is worth saying out loud rather than
// leaving it to look like an oversight.

#include "cinder.h"

void *xcalloc(size_t n, size_t size) {
  void *p = calloc(n, size);
  if (!p)
    error("out of memory");
  return p;
}

char *xstrndup(char *p, int len) {
  char *buf = xcalloc(1, len + 1);
  memcpy(buf, p, len);
  buf[len] = '\0';
  return buf;
}

void strarray_push(StrArray *arr, char *s) {
  if (!arr->data) {
    arr->data = xcalloc(8, sizeof(char *));
    arr->capacity = 8;
  }
  if (arr->capacity == arr->len) {
    char **old = arr->data;
    arr->data = xcalloc(arr->capacity * 2, sizeof(char *));
    memcpy(arr->data, old, sizeof(char *) * arr->capacity);
    arr->capacity *= 2;
  }
  arr->data[arr->len++] = s;
}

// format is sprintf into a fresh buffer. open_memstream is not available on
// Windows, so the length is measured with a trial vsnprintf first.
char *format(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);

  char *buf = xcalloc(1, len + 1);
  va_start(ap, fmt);
  vsnprintf(buf, len + 1, fmt, ap);
  va_end(ap);
  return buf;
}

//
// Hash map: open addressing with linear probing.
//
// Deletion uses a tombstone rather than back-shifting, because back-shifting a
// linear probe sequence is easy to get subtly wrong and deletion is rare here
// -- it happens for #undef and when a scope is popped.
//

#define INIT_SIZE 16
#define HIGH_WATERMARK 70 // rehash above this percent full
#define LOW_WATERMARK 50  // rehash down to this percent full

// A tombstone is a distinct non-null pointer, so a probe knows to keep going
// rather than stopping at what looks like an empty slot.
#define TOMBSTONE ((void *)-1)

static uint64_t fnv_hash(char *s, int len) {
  uint64_t hash = 0xcbf29ce484222325;
  for (int i = 0; i < len; i++) {
    hash *= 0x100000001b3;
    hash ^= (unsigned char)s[i];
  }
  return hash;
}

static bool match(HashEntry *ent, char *key, int keylen) {
  return ent->key && ent->key != TOMBSTONE && ent->keylen == keylen &&
         memcmp(ent->key, key, keylen) == 0;
}

static HashEntry *get_entry(HashMap *map, char *key, int keylen) {
  if (!map->buckets)
    return NULL;

  for (int i = 0; i < map->capacity; i++) {
    HashEntry *ent =
        &map->buckets[(fnv_hash(key, keylen) + i) % map->capacity];
    if (match(ent, key, keylen))
      return ent;
    if (ent->key == NULL)
      return NULL;
  }
  unreachable();
}

static void rehash(HashMap *map) {
  // Count only live entries; tombstones are what the rehash is clearing out,
  // and sizing the new table from the raw count would grow it needlessly.
  int nkeys = 0;
  for (int i = 0; i < map->capacity; i++)
    if (map->buckets[i].key && map->buckets[i].key != TOMBSTONE)
      nkeys++;

  int cap = map->capacity;
  while ((nkeys * 100) / cap >= LOW_WATERMARK)
    cap = cap * 2;

  HashMap map2 = {0};
  map2.buckets = xcalloc(cap, sizeof(HashEntry));
  map2.capacity = cap;

  for (int i = 0; i < map->capacity; i++) {
    HashEntry *ent = &map->buckets[i];
    if (ent->key && ent->key != TOMBSTONE)
      hashmap_put2(&map2, ent->key, ent->keylen, ent->val);
  }
  *map = map2;
}

static HashEntry *get_or_insert_entry(HashMap *map, char *key, int keylen) {
  if (!map->buckets) {
    map->buckets = xcalloc(INIT_SIZE, sizeof(HashEntry));
    map->capacity = INIT_SIZE;
  } else if ((map->used * 100) / map->capacity >= HIGH_WATERMARK) {
    rehash(map);
  }

  for (int i = 0; i < map->capacity; i++) {
    HashEntry *ent =
        &map->buckets[(fnv_hash(key, keylen) + i) % map->capacity];

    if (match(ent, key, keylen))
      return ent;

    // A tombstone can be reused, and because the probe already established
    // that the key is not present further along, this is safe.
    if (ent->key == TOMBSTONE) {
      ent->key = key;
      ent->keylen = keylen;
      return ent;
    }

    if (ent->key == NULL) {
      ent->key = key;
      ent->keylen = keylen;
      map->used++;
      return ent;
    }
  }
  unreachable();
}

void *hashmap_get(HashMap *map, char *key) {
  return hashmap_get2(map, key, strlen(key));
}

void *hashmap_get2(HashMap *map, char *key, int keylen) {
  HashEntry *ent = get_entry(map, key, keylen);
  return ent ? ent->val : NULL;
}

void hashmap_put(HashMap *map, char *key, void *val) {
  hashmap_put2(map, key, strlen(key), val);
}

void hashmap_put2(HashMap *map, char *key, int keylen, void *val) {
  HashEntry *ent = get_or_insert_entry(map, key, keylen);
  ent->val = val;
}

void hashmap_delete(HashMap *map, char *key) {
  hashmap_delete2(map, key, strlen(key));
}

void hashmap_delete2(HashMap *map, char *key, int keylen) {
  HashEntry *ent = get_entry(map, key, keylen);
  if (ent)
    ent->key = TOMBSTONE;
}

int align_to(int n, int align) { return (n + align - 1) / align * align; }
