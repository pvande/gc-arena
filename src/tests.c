#include "gc-arena.c"
#include "../vendor/utest.h"

// This is a reproduction of the mruby default allocf function.
void *test_allocf(struct mrb_state *mrb, void *ptr, size_t size, void *ud) {
  if (size == 0) {
    free(ptr);
    return NULL;
  } else {
    return realloc(ptr, size);
  }
}
static mrb_allocf fallback_allocf = test_allocf;

UTEST(gc_alloc, initialization) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 2, 0);
  ASSERT_TRUE(arena);

  struct RCptr *ptr1 = (void *)arena->gc.free_heaps->freelist;
  ASSERT_TRUE(ptr1);

  struct RCptr *ptr2 = ptr1->p;
  ASSERT_TRUE(ptr2);

  struct RCptr *ptr3 = ptr2->p;
  ASSERT_FALSE(ptr3);
}

UTEST(alloc_with_arena, basic_alloc) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr = alloc_with_arena(arena, 8);
  ASSERT_TRUE(ptr);
}

UTEST(alloc_with_arena, multiple_allocations_in_single_page) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = alloc_with_arena(arena, 8);
  ASSERT_TRUE(ptr1);

  void *ptr2 = alloc_with_arena(arena, 8);
  ASSERT_TRUE(ptr2);

  ASSERT_EQ(0, gc_arena_page_available(arena->page));

  ASSERT_EQ(16, ptr2 - ptr1);
}

UTEST(alloc_with_arena, unaligned_allocations) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = alloc_with_arena(arena, 2);
  ASSERT_TRUE(ptr1);

  void *ptr2 = alloc_with_arena(arena, 2);
  ASSERT_TRUE(ptr2);

  ASSERT_EQ(16, ptr2 - ptr1);
}

UTEST(alloc_with_arena, multiple_allocations_on_multiple_pages) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = alloc_with_arena(arena, 16);
  ASSERT_TRUE(ptr1);

  ASSERT_EQ(8, gc_arena_page_available(arena->page));

  void *ptr2 = alloc_with_arena(arena, 16);
  ASSERT_TRUE(ptr2);

  ASSERT_NE(24, ptr2 - ptr1);
}

UTEST(gc_arena_allocf, alloc_without_arena) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, NULL);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(32, gc_arena_page_available(arena->page));

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, NULL);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(32, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, alloc_with_arena) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, free_without_arena) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, NULL);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(32, gc_arena_page_available(arena->page));

  gc_arena_allocf(NULL, ptr1, 0, NULL); // size=0 -> free()
  ASSERT_EQ(32, gc_arena_page_available(arena->page));

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, NULL);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(32, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, free_with_arena) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));

  gc_arena_allocf(NULL, ptr1, 0, arena); // size=0 -> free()
  ASSERT_EQ(16, gc_arena_page_available(arena->page));

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, realloc_with_arena_can_reallocate_into_same_page) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, ptr1, 24, arena);
  ASSERT_EQ(ptr1, ptr2);
  ASSERT_EQ(0, strcmp(ptr2, "Hello"));
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, realloc_with_arena_can_only_reallocate_trailing_allocations_into_same_page) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 64);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(48, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(32, gc_arena_page_available(arena->page));

  // `ptr1` is not the most recent allocation on the page, so a new allocation
  // must be made; since there is not enough space remaining (including the
  // allocation tag), the allocation is made on a new page.
  void *ptr3 = gc_arena_allocf(NULL, ptr1, 32, arena);
  ASSERT_TRUE(ptr3);
  ASSERT_EQ(0, strcmp(ptr3, "Hello"));
  ASSERT_TRUE(arena->page->next);
  ASSERT_EQ(32, gc_arena_page_available(arena->page->next));

  // `ptr2` is not the most recent allocation *overall*, but is the most recent
  // allocation on the page, and since the requested size can fit in the
  // remaining space, the allocation can be resized in place.
  void *ptr4 = gc_arena_allocf(NULL, ptr2, 16, arena);
  ASSERT_TRUE(ptr4);
  ASSERT_EQ(ptr2, ptr4);
  ASSERT_EQ(24, gc_arena_page_available(arena->page->next));
}

UTEST(gc_arena_allocf, realloc_with_arena_will_allocate_new_pages) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 24);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(8, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, ptr1, 24, arena);
  ASSERT_NE(ptr1, ptr2);
  ASSERT_EQ(0, strcmp(ptr2, "Hello"));
  ASSERT_NE(0, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, realloc_with_arena_will_identify_correct_arena) {
  struct gc_arena *arena_a = gc_arena_allocate(NULL, 0, 32);
  struct gc_arena *arena_b = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena_a);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena_a->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, ptr1, 24, arena_b);
  ASSERT_TRUE(ptr2);
  ASSERT_TRUE(is_in_arena(arena_a, ptr2));
  ASSERT_FALSE(is_in_arena(arena_b, ptr2));
  ASSERT_EQ(ptr1, ptr2);
  ASSERT_EQ(0, strcmp(ptr2, "Hello"));
  ASSERT_EQ(0, gc_arena_page_available(arena_a->page));
  ASSERT_EQ(32, gc_arena_page_available(arena_b->page));
}

UTEST(gc_arena_allocf, realloc_with_arena_can_shrink_an_allocation) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 64);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 16, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(40, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, ptr1, 6, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(ptr1, ptr2);
  ASSERT_EQ(48, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, realloc_with_arena_will_truncate_smaller_allocations_when_necessary) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 64);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 16, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(40, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(24, gc_arena_page_available(arena->page));

  void *ptr3 = gc_arena_allocf(NULL, ptr1, 4, arena);
  ASSERT_TRUE(ptr3);
  ASSERT_NE(ptr1, ptr3);
  ASSERT_EQ(8, gc_arena_page_available(arena->page));
  ASSERT_EQ(0, strncmp(ptr3, "Hell", 4));
  ASSERT_NE(((char *)ptr3)[4], 'o');
}

UTEST(gc_arena_allocf, realloc_without_arena) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, NULL);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(32, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, ptr1, 24, NULL);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(0, strcmp(ptr2, "Hello"));
  ASSERT_EQ(32, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_allocf, realloc_without_arena_will_identify_correct_arena) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, ptr1, 24, NULL);
  ASSERT_TRUE(ptr2);
  ASSERT_TRUE(is_in_arena(arena, ptr2));
  ASSERT_EQ(ptr1, ptr2);
  ASSERT_EQ(0, strcmp(ptr2, "Hello"));
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
}

UTEST(gc_arena_reset, alloc_yields_old_pointers_after_reset) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 0, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
  strcpy(ptr2, "Goodbye");

  gc_arena_reset(NULL, arena);

  void *new_ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(new_ptr1);
  ASSERT_EQ(ptr1, new_ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));
  ASSERT_EQ(0, strcmp(new_ptr1, "Hello"));

  void *new_ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(new_ptr2);
  ASSERT_EQ(ptr2, new_ptr2);
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
  ASSERT_EQ(0, strcmp(new_ptr2, "Goodbye"));
}

UTEST(gc_arena_reset, alloc_yields_old_pointers_after_reset_with_prealloc_objects) {
  struct gc_arena *arena = gc_arena_allocate(NULL, 4, 32);

  void *ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));
  strcpy(ptr1, "Hello");

  void *ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(ptr2);
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
  strcpy(ptr2, "Goodbye");

  gc_arena_reset(NULL, arena);

  void *new_ptr1 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(new_ptr1);
  ASSERT_EQ(ptr1, new_ptr1);
  ASSERT_EQ(16, gc_arena_page_available(arena->page));
  ASSERT_EQ(0, strcmp(new_ptr1, "Hello"));

  void *new_ptr2 = gc_arena_allocf(NULL, NULL, 8, arena);
  ASSERT_TRUE(new_ptr2);
  ASSERT_EQ(ptr2, new_ptr2);
  ASSERT_EQ(0, gc_arena_page_available(arena->page));
  ASSERT_EQ(0, strcmp(new_ptr2, "Goodbye"));
}

UTEST_MAIN()
