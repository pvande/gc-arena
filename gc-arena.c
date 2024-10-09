#include "dragonruby.h"

struct drb_api_t *api;

#define MAX_ARENAS 64

// Skipped during GC traversal.
#define GC_RED 7

typedef struct {
  union {
    struct RData data;
    struct RCptr ptr;
  } as;
} ObjectSlot;

struct gc_arena_page {
  struct gc_arena_page *next;
  void *last;
  void *ptr;
  void *end;
};

struct gc_arena {
  mrb_gc gc;
  void *beg;
  void *end;
  struct gc_arena_page *page;
};

static void gc_arena_free(mrb_state *mrb, void *ptr) {
  struct gc_arena *arena = ptr;
  struct gc_arena_page *page = arena->page;
  struct gc_arena_page *next;

  do {
    next = page->next;
    free(page);
  } while ((page = next));
}

const mrb_data_type gc_arena_data_type = {"Arena", gc_arena_free};

static uint8_t gc_arena_count = 0;
static struct gc_arena gc_arenas[MAX_ARENAS] = {0};
static mrb_allocf fallback_allocf;

#define arena_ptr(ptr) (struct gc_arena *)(ptr)
#define is_arena(ptr) (arena_ptr(ptr) >= &gc_arenas[0] && arena_ptr(ptr) < &gc_arenas[MAX_ARENAS])

static inline struct gc_arena_page *is_in_arena(struct gc_arena *arena, void *ptr) {
  if (!is_arena(arena) || ptr < arena->beg || ptr >= arena->end) return NULL;
  struct gc_arena_page *page = arena->page;
  struct gc_arena_page *next;
  do {
    if (ptr > (void *)page && ptr < page->end) break;
    next = page->next;
  } while ((page = next));

  return page;
}

static inline void *add_page(struct gc_arena *arena, size_t size) {
  size_t page_size = size + sizeof(ObjectSlot) * 1024;
  struct gc_arena_page *new = malloc(sizeof(struct gc_arena_page) + page_size);
  *new = (struct gc_arena_page){
    .next = arena->page,
    .ptr = new + 1,
    .end = (void *)(new + 1) + page_size,
  };

  if (arena->beg > new->ptr) arena->beg = new->ptr;
  if (arena->end < new->end) arena->end = new->end;

  arena->page = new;
  return new;
}

void *_gc_arena_alloc(struct gc_arena *arena, size_t size) {
  struct gc_arena_page *page = arena->page;
  size_t tagged_size = size + sizeof(uint64_t) + (8 - size & 7) % 8;

  if (tagged_size > page->end - page->ptr) {
    page = add_page(arena, tagged_size);
  }

  uint64_t *tag = page->ptr;
  page->last = tag + 1;

  *tag = size;
  page->ptr += tagged_size;
  return page->last;
}

void *gc_arena_allocf(struct mrb_state *mrb, void *ptr, size_t size, void *ud) {
  if (!is_arena(ud) && (!size || !ptr)) return fallback_allocf(mrb, ptr, size, ud);

  // Handle free() calls.
  if (size == 0) return NULL;

  // Handle malloc() calls.
  if (ptr == NULL) return _gc_arena_alloc(ud, size);

  // Handle realloc() calls.
  struct gc_arena *arena = ud;
  struct gc_arena_page *page = is_in_arena(arena, ptr);
  if (!page) {
    arena = NULL;
    for (int idx = 0; idx < gc_arena_count; idx++) {
      page = is_in_arena(&gc_arenas[idx], ptr);
      if (page) {
        arena = &gc_arenas[idx];
        break;
      }
    }
  }

  if (!arena) return fallback_allocf(mrb, ptr, size, ud);

  // Extend the pointer if there's enough space remaining on that page.
  if (ptr == page->last && ptr + size <= page->end) {
    ((uint64_t *)ptr)[-1] = size;
    page->ptr = ptr + size + (8 - size & 7) % 8;
    return ptr;
  }

  // Step 3: Allocate a new page and copy over the data.
  void *dest = _gc_arena_alloc(arena, size);
  size_t original_size = ((uint64_t *)ptr)[-1];
  memcpy(dest, ptr, size > original_size ? original_size : size);
  return dest;
}

struct gc_arena *gc_arena_allocate(mrb_state *mrb, size_t object_count, size_t extra_bytes) {
  // @NOTE We're allocating a single chunk of memory to house the arena and all
  //       the anticipated data. This isn't strictly necessary — we could make
  //       separate allocations — but it simplifies cleanup later.
  size_t gc_arena_size = 0;
  gc_arena_size += sizeof(struct gc_arena_page);
  gc_arena_size += sizeof(struct mrb_heap_page);
  gc_arena_size += sizeof(ObjectSlot) * object_count;
  gc_arena_size += sizeof(struct RBasic *) * MRB_GC_ARENA_SIZE;
  gc_arena_size += extra_bytes;
  void *ptr = malloc(gc_arena_size);

  // Portion out the allocated memory.
  struct gc_arena_page *page = ptr;
  page->end = ptr + gc_arena_size;
  ptr = page + 1;

  void *mrb_gc_arena = ptr;
  ptr += sizeof(struct RBasic *) * MRB_GC_ARENA_SIZE;

  mrb_heap_page *heap = ptr;
  ptr += sizeof(struct mrb_heap_page) + sizeof(ObjectSlot) * object_count;

  page->ptr = ptr;

  // Initialize our heap page.
  *heap = (struct mrb_heap_page){.freelist = (void *)heap->objects};
  ObjectSlot *slot = (ObjectSlot *)heap->objects + object_count - 1;
  slot->as.ptr = (struct RCptr){.tt = MRB_TT_FREE};
  while (slot >= (ObjectSlot *)heap->objects) {
    slot->as.ptr = (struct RCptr){.tt = MRB_TT_FREE, .p = slot + 1};
    slot--;
  }

  // Initialize our arena.
  struct gc_arena *arena = &gc_arenas[gc_arena_count];
  *arena = (struct gc_arena){
    .gc = {
      .heaps = heap,
      .free_heaps = heap,
      .arena = mrb_gc_arena,
      .arena_capa = MRB_GC_ARENA_SIZE,
      .current_white_part = GC_RED,
      .disabled = TRUE,
    },
    .beg = page,
    .end = page->end,
    .page = page,
  };

  gc_arena_count++;
  return arena;
}

size_t gc_arena_page_available(struct gc_arena_page *page) {
  return page->end - page->ptr;
}

// @TODO Support keyword arguments for: total_bytes, object_count, extra_bytes.
mrb_value gc_arena_allocate_cm(mrb_state *mrb, mrb_value cls) {
  mrb_int object_count = 0;
  mrb_int extra_bytes = 0;
  api->mrb_get_args(mrb, "i|i", &object_count, &extra_bytes);

  struct gc_arena *arena = gc_arena_allocate(mrb, object_count, extra_bytes);

  struct RData *obj = api->mrb_data_object_alloc(mrb, mrb_class_ptr(cls), arena, &gc_arena_data_type);
  return mrb_obj_value(obj);
}

// @TODO This needs error handling!
mrb_value gc_arena_eval_m(mrb_state *mrb, mrb_value self) {
  struct gc_arena *arena = api->mrb_get_datatype(mrb, self, &gc_arena_data_type);
  mrb_value block;
  api->mrb_get_args(mrb, "&", &block);

  // Backup mrb_state props.
  mrb_gc original_gc = mrb->gc;
  void *original_allocf_ud = mrb->allocf_ud;

  // Swap in the Arena's GC and allocator.
  mrb->gc = arena->gc;
  mrb->allocf_ud = arena;

  // Evaluate the block.
  mrb_value value = api->mrb_yield_argv(mrb, block, 0, NULL);

  // Restore mrb_state props.
  arena->gc = mrb->gc;
  mrb->gc = original_gc;
  mrb->allocf_ud = original_allocf_ud;

  return value;
}

mrb_value gc_arena_allocated_m(mrb_state *mrb, mrb_value self) {
  struct gc_arena *arena = api->mrb_get_datatype(mrb, self, &gc_arena_data_type);
  mrb_value block;
  api->mrb_get_args(mrb, "&", &block);

  size_t total_bytes = 0;

  struct gc_arena_page *page = arena->page;
  do {
    total_bytes += page->end - page->ptr;
  } while ((page = page->next));

  size_t objects = mrb->allocf_ud == arena ? mrb->gc.live : arena->gc.live;
  size_t object_bytes = objects * sizeof(ObjectSlot);

  return mrb_fixnum_value(objects);
}

void drb_register_c_extensions_with_api(mrb_state *mrb, struct drb_api_t *drb) {
  api = drb;

  fallback_allocf = mrb->allocf;
  mrb->allocf = gc_arena_allocf;

  struct RClass *Arena = api->mrb_define_class_under(mrb, api->mrb_module_get(mrb, "GC"), "Arena", mrb->object_class);
  MRB_SET_INSTANCE_TT(Arena, MRB_TT_DATA);

  api->mrb_undef_class_method(mrb, Arena, "new");
  api->mrb_define_class_method(mrb, Arena, "allocate", gc_arena_allocate_cm, MRB_ARGS_REQ(1));
  api->mrb_define_method(mrb, Arena, "eval", gc_arena_eval_m, MRB_ARGS_BLOCK());
  api->mrb_define_method(mrb, Arena, "allocated", gc_arena_allocated_m, MRB_ARGS_REQ(1));
}
