#include "dragonruby.h"

#define MAX_ARENAS 64

// Skipped during GC traversal.
#define GC_RED 7

#pragma region Structs

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

#pragma endregion

#pragma region Data

struct drb_api_t *api;

static void gc_arena_free(mrb_state *mrb, void *ptr);
const mrb_data_type gc_arena_data_type = {"Arena", gc_arena_free};

static uint8_t gc_arena_count = 0;
static struct gc_arena gc_arenas[MAX_ARENAS] = {0};
static mrb_allocf fallback_allocf;

#define arena_ptr(ptr) (struct gc_arena *)(ptr)
#define is_arena(ptr) (arena_ptr(ptr) >= &gc_arenas[0] && arena_ptr(ptr) < &gc_arenas[MAX_ARENAS])

#pragma endregion

#pragma region Implementation

static void gc_arena_free(mrb_state *mrb, void *ptr) {
  struct gc_arena *arena = ptr;
  struct gc_arena_page *page = arena->page;
  struct gc_arena_page *next;

  do {
    next = page->next;
    free(page);
  } while ((page = next));
}

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

void *alloc_with_arena(struct gc_arena *arena, size_t size) {
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
  if (ptr == NULL) return alloc_with_arena(ud, size);

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
  void *dest = alloc_with_arena(arena, size);
  size_t original_size = ((uint64_t *)ptr)[-1];
  memcpy(dest, ptr, size > original_size ? original_size : size);
  return dest;
}

static void *gc_arena_initialize_heap(struct mrb_heap_page *heap, size_t count) {
  ObjectSlot *slot = (ObjectSlot *)heap->objects;
  ObjectSlot *prev = NULL;

  while (count--) {
    slot->as.ptr = (struct RCptr){.tt = MRB_TT_FREE, .p = prev};
    prev = slot++;
  }

  return prev;
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
  heap->freelist = gc_arena_initialize_heap(heap, object_count);

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

#pragma endregion

#pragma region Ruby Interface

/*
 * Document-class: GC::Arena
 *
 * Arena-style memory management in Ruby.
 */

/*
 * Document-method: GC::Arena.allocate
 *
 * Allocates a new `GC::Arena`, reserving a pool of memory for objects and their
 * backing data.
 *
 * @overload allocate(objects:, extra: 0)
 *   @param objects [Integer] The number of objects to allocate space for.
 *   @param extra [Integer] Additional bytes of storage to allocate.
 */
mrb_value gc_arena_allocate_cm(mrb_state *mrb, mrb_value cls) {
  if (is_arena(mrb->allocf_ud)) {
    api->mrb_raise(mrb, api->mrb_class_get(mrb, "RuntimeError"), "Nested Arenas are not supported.");
  }

  mrb_value values[2];
  const mrb_kwargs kwargs = {
    .num = 2,
    .required = 1,
    .table = (const mrb_sym[2]){
      api->mrb_intern_static(mrb, "objects", 7),
      api->mrb_intern_static(mrb, "extra", 5),
    },
    .values = values,
  };
  api->mrb_get_args(mrb, ":", &kwargs);
  if (mrb_undef_p(values[1])) values[1] = mrb_fixnum_value(0);

  struct gc_arena *arena = gc_arena_allocate(mrb, mrb_fixnum(values[0]), mrb_fixnum(values[1]));

  struct RData *obj = api->mrb_data_object_alloc(mrb, mrb_class_ptr(cls), arena, &gc_arena_data_type);
  return mrb_obj_value(obj);
}

/*
 * Document-method: GC::Arena#eval
 *
 * @todo Implement exception handling.
 *
 * Substitutes this Arena in place of the current object pool and allocator,
 * forcing object creation within the given block to occur within this Arena.
 *
 * * Nested calls to `GC:::Arena#eval` should function as expected.
 * * Allocations performed by C extensions will also utilize this Arena if they
 *   perform allocations using the mruby provided APIs.
 *
 * @yield Nothing.
 * @return The block's result.
 */
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
  if (fallback_allocf) return;

  fallback_allocf = mrb->allocf;
  mrb->allocf = gc_arena_allocf;

  struct RClass *Arena = api->mrb_define_class_under(mrb, api->mrb_module_get(mrb, "GC"), "Arena", mrb->object_class);
  MRB_SET_INSTANCE_TT(Arena, MRB_TT_DATA);

  api->mrb_undef_class_method(mrb, Arena, "new");
  api->mrb_define_class_method(mrb, Arena, "allocate", gc_arena_allocate_cm, MRB_ARGS_KEY(2, 1));
  api->mrb_define_method(mrb, Arena, "eval", gc_arena_eval_m, MRB_ARGS_BLOCK());
  api->mrb_define_method(mrb, Arena, "allocated", gc_arena_allocated_m, MRB_ARGS_REQ(1));

#if false
  // This pseudo-code exists to document the Ruby API for YARD.
  GC = rb_define_module("GC");
  Arena = rb_define_class_under(GC, "Arena", rb_cObject);

  rb_define_singleton_method(Arena, "allocate", gc_arena_allocate_cm, -1);
  rb_define_method(Arena, "eval", gc_arena_eval_m, 0);
#endif
}
