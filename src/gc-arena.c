#include "dragonruby.h"

#define MAX_ARENAS 64

// Skipped during GC traversal.
#define GC_RED 7

#ifndef MRB
#define MRB(method) api->method
#endif

#pragma region Structs

typedef struct {
  union {
    struct RData data;
    struct RCptr ptr;
  } as;
} ObjectSlot;

struct gc_arena_page {
  struct gc_arena_page *next;
  void *start;
  void *last;
  void *ptr;
  void *end;
};

struct gc_arena {
  mrb_gc gc;
  size_t initial_objects;
  void *beg;
  void *end;
  struct gc_arena_page *page;
};

struct gc_arena_stats {
  size_t pages;
  size_t total_memory;
  size_t used_memory;
  size_t total_objects;
  size_t live_objects;
  size_t free_objects;
  size_t total_storage;
  size_t used_storage;
  size_t free_storage;
};

struct gc_arena_eval_cb_data {
  mrb_value self;
  mrb_value block;
  mrb_gc original_gc;
  void *original_allocf_ud;
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
    if (ptr >= page->start && ptr < page->end) break;
    next = page->next;
  } while ((page = next));

  return page;
}

static inline void gc_arena_stats(mrb_state *mrb, struct gc_arena *arena, struct gc_arena_stats *stats) {
  *stats = (struct gc_arena_stats){
    .total_objects = arena->gc.live,
    .live_objects = arena->gc.live,
  };

  struct gc_arena_page *page = arena->page;
  while (page) {
    stats->pages += 1;
    stats->total_storage += page->end - page->start;
    stats->free_storage += page->end - page->ptr;
    stats->used_storage += page->ptr - page->start;
    page = page->next;
  }

  struct mrb_heap_page *heap = arena->gc.free_heaps;
  while (heap) {
    struct RCptr *ptr = (struct RCptr *)heap->freelist;
    while (ptr) {
      stats->total_objects++;
      stats->free_objects++;
      ptr = ptr->p;
    }

    heap = heap->free_next;
  }

  stats->total_storage -= sizeof(ObjectSlot) * stats->total_objects;
  stats->used_storage -= sizeof(ObjectSlot) * stats->total_objects;
}

static inline void *add_page(struct gc_arena *arena, size_t size) {
  size_t page_size = size + sizeof(ObjectSlot) * 1024;
  struct gc_arena_page *new = malloc(sizeof(struct gc_arena_page) + page_size);
  *new = (struct gc_arena_page){
    .start = new + 1,
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

static void gc_arena_reset(mrb_state *mrb, struct gc_arena *arena) {
  mrb_heap_page *heap = arena->gc.heaps;
  while (heap->next) {
    heap = heap->next;
  }

  struct gc_arena_page *page = arena->page;
  struct gc_arena_page *last, *next;
  while (page->next) {
    next = page->next;
    free(page);
    page = next;
  }

  *heap = (mrb_heap_page){.freelist = gc_arena_initialize_heap(heap, arena->initial_objects)};

  page->ptr = (void *)(heap + 1) + sizeof(ObjectSlot) * arena->initial_objects;
  arena->page = page;
  arena->gc.live = 0;
  arena->gc.sweeps = NULL;
  arena->gc.heaps = heap;
  arena->gc.free_heaps = heap;
}

struct gc_arena *gc_arena_allocate(mrb_state *mrb, size_t object_count, size_t storage_bytes) {
  // @NOTE We're allocating a single chunk of memory to house the arena and all
  //       the anticipated data. This isn't strictly necessary — we could make
  //       separate allocations — but it simplifies cleanup later.
  size_t gc_arena_size = 0;
  gc_arena_size += sizeof(struct gc_arena_page);
  gc_arena_size += sizeof(struct mrb_heap_page);
  gc_arena_size += sizeof(ObjectSlot) * object_count;
  gc_arena_size += storage_bytes;
  void *ptr = malloc(gc_arena_size);

  // Portion out the allocated memory.
  struct gc_arena_page *page = ptr;
  page->end = ptr + gc_arena_size;
  ptr = page + 1;

  mrb_heap_page *heap = ptr;
  ptr += sizeof(struct mrb_heap_page) + sizeof(ObjectSlot) * object_count;

  // Initialize our values.
  struct gc_arena *arena = &gc_arenas[gc_arena_count];
  *page = (struct gc_arena_page){.start = heap + 1, .ptr = ptr, .end = page->end};
  *heap = (mrb_heap_page){.freelist = gc_arena_initialize_heap(heap, object_count)};
  *arena = (struct gc_arena){
    .gc = {
      .heaps = heap,
      .free_heaps = heap,
      .current_white_part = GC_RED,
      .disabled = TRUE,
    },
    .initial_objects = object_count,
    .beg = page,
    .end = page->end,
    .page = page,
  };

  gc_arena_count++;
  return arena;
}

mrb_value gc_arena_eval_body(struct mrb_state *mrb, mrb_value data_cptr) {
  struct gc_arena_eval_cb_data *data = mrb_cptr(data_cptr);
  struct gc_arena *arena = MRB(mrb_get_datatype)(mrb, data->self, &gc_arena_data_type);

  // Backup mrb_state props.
  data->original_gc = mrb->gc;
  data->original_allocf_ud = mrb->allocf_ud;

  // Swap in the Arena's GC and allocator.
  mrb->gc = arena->gc;
  mrb->gc.arena = data->original_gc.arena;
  mrb->gc.arena_idx = data->original_gc.arena_idx;
  mrb->gc.arena_capa = data->original_gc.arena_capa;
  mrb->allocf_ud = arena;

  // Evaluate the block.
  return MRB(mrb_yield_argv)(mrb, data->block, 0, NULL);
}

mrb_value gc_arena_eval_ensure(struct mrb_state *mrb, mrb_value data_cptr) {
  struct gc_arena_eval_cb_data *data = mrb_cptr(data_cptr);
  struct gc_arena *arena = MRB(mrb_get_datatype)(mrb, data->self, &gc_arena_data_type);

  // Restore mrb_state props.
  arena->gc = mrb->gc;
  mrb->gc = data->original_gc;
  mrb->allocf_ud = data->original_allocf_ud;

  return mrb_nil_value();
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
 * @overload allocate(objects:, storage: 0)
 *   @param objects [Integer] The number of objects to allocate space for.
 *   @param storage [Integer] Additional bytes of storage to allocate.
 #   @return GC::Arena
 */
mrb_value gc_arena_allocate_cm(mrb_state *mrb, mrb_value cls) {
  if (is_arena(mrb->allocf_ud)) {
    MRB(mrb_raise)(mrb, MRB(mrb_class_get)(mrb, "RuntimeError"), "Nested Arenas are not supported.");
  }

  mrb_value values[2];
  const mrb_kwargs kwargs = {
    .num = 2,
    .required = 1,
    .table = (const mrb_sym[2]){
      MRB(mrb_intern_static)(mrb, "objects", 7),
      MRB(mrb_intern_static)(mrb, "storage", 7),
    },
    .values = values,
  };
  MRB(mrb_get_args)(mrb, ":", &kwargs);
  if (mrb_undef_p(values[1])) values[1] = mrb_fixnum_value(0);

  struct gc_arena *arena = gc_arena_allocate(mrb, mrb_fixnum(values[0]), mrb_fixnum(values[1]));

  struct RData *obj = MRB(mrb_data_object_alloc)(mrb, mrb_class_ptr(cls), arena, &gc_arena_data_type);
  return mrb_obj_value(obj);
}

/*
 * Document-method: GC::Arena#eval
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
  struct gc_arena *arena = MRB(mrb_get_datatype)(mrb, self, &gc_arena_data_type);
  mrb_value block;
  MRB(mrb_get_args)(mrb, "&", &block);

  struct gc_arena_eval_cb_data data = {.self = self, .block = block};
  mrb_value data_cptr = mrb_obj_value(&(struct RCptr){.tt = MRB_TT_CPTR, .p = &data});

  return MRB(mrb_ensure)(mrb, gc_arena_eval_body, data_cptr, gc_arena_eval_ensure, data_cptr);
}

/*
 * Document-method: GC::Arena#reset
 *
 * Resets the Arena's allocator.
 *
 * > [!IMPORTANT]
 * > This will invalidate references to every object in this Arena! It is your
 * > responsibility to ensure that you no longer reference those objects.
 *
 * This enables scratch and periodic Arenas to quickly and quietly discard all
 * data and prepare for further use. Manually resetting is faster than
 * allocating a new Arena, and substantially faster than delegating to the GC.
 *
 * @example Scratch Storage
 *   $scratch = Arena.new(objects: 2048)
 *   def tick(...)
 *     $scratch.reset
 *     $scratch.eval do
 *       simulate_game
 *       render_game
 *     end
 *   end
 *
 * @return [nil]
 */
mrb_value gc_arena_reset_m(mrb_state *mrb, mrb_value self) {
  struct gc_arena *arena = MRB(mrb_get_datatype)(mrb, self, &gc_arena_data_type);
  if (mrb->allocf_ud == arena) arena->gc = mrb->gc;
  gc_arena_reset(mrb, arena);
  if (mrb->allocf_ud == arena) mrb->gc = arena->gc;
  return mrb_nil_value();
}

/*
 * Document-method: GC::Arena#stats
 *
 * Provides details about the utilization of this Arena. These details can be
 * used to determine appropriate values for preallocation, by inspecting these
 * values either after the Arena has been fully populated or periodically just
 * before resetting the Arena (to determine a "high water mark" for
 * object/memory consumption).
 *
 * @return [Hash] Detailed statistics about this Arena.
 *   * `pages`
 *       * This indicates the number of memory pages that have been allocated
 *         since the Arena was created. Numbers greater than `1` indicate that
 *         usage has exceeded the initialization capacity.
 *   * `total_objects`
 *       * This represents the total number of object slots currently available.
 *   * `live_objects`
 *       * This represents the number of object slots which have been filled since
 *         the Arena was created.
 *   * `free_objects`
 *       * This represents the number of unpopulated object slots.
 *   * `total_storage`
 *       * This represents the total number of bytes allocated for additional
 *         object data storage.
 *   * `used_storage`
 *       * This represents the number of bytes of additional object storage
 *         currently being used.
 *   * `free_storage`
 *       * This represents the number of bytes allocated but as-yet unused.
 *       * Note that this number *may* be higher than expected, as data near the
 *         end of a page may be left indefinitely "free" if the next allocation is
 *         larger than the remaining available space.
 */
mrb_value gc_arena_stats_m(mrb_state *mrb, mrb_value self) {
  struct gc_arena *arena = MRB(mrb_get_datatype)(mrb, self, &gc_arena_data_type);

  // Sync GC details if the arena is currently "live".
  if (mrb->allocf_ud == arena) arena->gc = mrb->gc;

  struct gc_arena_stats stats;
  gc_arena_stats(mrb, arena, &stats);

  mrb_value hash = MRB(mrb_hash_new)(mrb);
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "pages", 5)), mrb_fixnum_value(stats.pages));
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "total_objects", 13)), mrb_fixnum_value(stats.total_objects));
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "live_objects", 12)), mrb_fixnum_value(stats.live_objects));
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "free_objects", 12)), mrb_fixnum_value(stats.free_objects));
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "total_storage", 13)), mrb_fixnum_value(stats.total_storage));
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "used_storage", 12)), mrb_fixnum_value(stats.used_storage));
  MRB(mrb_hash_set)(mrb, hash, mrb_symbol_value(MRB(mrb_intern_static)(mrb, "free_storage", 12)), mrb_fixnum_value(stats.free_storage));

  return hash;
}

void drb_register_c_extensions_with_api(mrb_state *mrb, struct drb_api_t *drb) {
  api = drb;
  if (fallback_allocf) return;

  fallback_allocf = mrb->allocf;
  mrb->allocf = gc_arena_allocf;

  struct RClass *GC = MRB(mrb_module_get)(mrb, "GC");
  struct RClass *Arena = MRB(mrb_define_class_under)(mrb, GC, "Arena", mrb->object_class);
  MRB_SET_INSTANCE_TT(Arena, MRB_TT_DATA);

  MRB(mrb_undef_class_method)(mrb, Arena, "new");
  MRB(mrb_define_class_method)(mrb, Arena, "allocate", gc_arena_allocate_cm, MRB_ARGS_KEY(2, 1));
  MRB(mrb_define_method)(mrb, Arena, "eval", gc_arena_eval_m, MRB_ARGS_BLOCK());
  MRB(mrb_define_method)(mrb, Arena, "reset", gc_arena_reset_m, MRB_ARGS_NONE());
  MRB(mrb_define_method)(mrb, Arena, "stats", gc_arena_stats_m, MRB_ARGS_NONE());

#if false
  // This pseudo-code exists to document the Ruby API for YARD.
  GC = rb_define_module("GC");
  Arena = rb_define_class_under(GC, "Arena", rb_cObject);

  rb_define_singleton_method(Arena, "allocate", gc_arena_allocate_cm, -1);
  rb_define_method(Arena, "eval", gc_arena_eval_m, 0);
  rb_define_method(Arena, "reset", gc_arena_reset_m, 0);
  rb_define_method(Arena, "stats", gc_arena_stats_m, 0);
#endif
}
