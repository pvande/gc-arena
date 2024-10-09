# gc-arena

Opt-in manual memory management for DragonRuby.

> [!CAUTION]
> **This tool is very sharp.**
>
> mruby is built with an expectation of automatic memory management. Hidden
> allocations can happen in unexpected places, and these can be a source of
> difficult to diagnose bugs. Ruby allows you to remain ignorant about memory
> details; this tool is not so kind. Please take time to understand how to use
> this tool first.

## Why This Exists
Garbage collection is a wonderful feature of many programming languages,
including Ruby, that allows you to foucs on the the logical transformation of
data rather than how and where that data is stored. This tool is not without
cost however, and the cost of the garbage collector provided by mruby *in
particular* scales in proportion to the size of the tracked data.

Optimization techniques like preprocessing, memoization, indexing, and caching
— usually viewed as a trading memory for speed — end up being less effective
because of the increased cost of garbage collection. Large volumes of completely
static data end up being tested for liveness repeatedly. While changes
introduced in DragonRuby 6.2 have significantly redcuced the effective cost of
garbage collection, a full GC cycle can still introduce a noticeable periodic
delay or a significant delay at startup.

`gc-arena` provides memory pools that the GC _fully ignores_, giving you space
to store as much data as you want, at no additional cost, and to choose if and
when that data should be freed. Used wisely, this can be a form of manual memory
management with many of the same benefits as garbage collection, without the any
of the drawbacks.

## How It Works
When a new `GC::Arena` is created, it allocates a pool of memory, which will be
bound to that instance for its entire lifetime. `GC::Arena#eval` temporarily
swaps out the object heap and allocator used by mruby to utilize the Arena's
memory pool. All newly created objects — and any other allocations, like long
string storage — will live in the Arena's pool and are flagged so that they are
not traversed by the GC.

## When To Use It
Arena allocation can be a great choice when the cost of garbage collection
becomes noticeable, or when the cost of memory allocation is a concern.

Consider using an arena for:
* Large data caches that live for the entire runtime of the game.
* Scene data, like level geometry and objects.
* Per-frame data and intermediate calculations.

More generally, they can be useful whenever you have 1) some amount of 2) data
with a known lifetime. [Untangling Lifetimes] is a great resource for thinking
about how arenas work and why they are useful, from the perspective of a C
programmer.

## How To Use It

``` ruby
# Load the library.
def boot(...)
  $gtk.dlopen("gc-arena")
end

# Create a new Arena.
# Parameters allow you to specify how many object slots and how much additional
# memory should be preallocated to this arena, which can help reduce runtime
# allocation costs.
arena = GC::Arena.allocate(1000)

# Objects created outside the Arena are subject to normal garbage collection.
var = Garbage.new
var = nil # The GC will eventually reclaim the `Garbage` instance.

# Using `Arena#eval` will temporarily swap contexts.
arena.eval do
  # Objects created inside the Arena live within the Arena's memory pool.
  # They are not subject to garbage collection.
  var = Object.new
end

# `Arena#eval` also returns the value of the last expression.
my_object = arena.eval { Object.new }

# Unlike normal, garbage collected objects, references to Arena allocated
# objects will become invalid (causing errors and possibly crashes) when the
# Arena's memory is freed. This will happen automatically when the Arena is
# garbage collected.
arena = nil
```

## What Not To Do

### Use After Free
> [!IMPORTANT]
>  Rule: **Object references must not outlive their Arena.**

Arenas will free their associated memory when garbage collected, or when
explicitly asked to do so. This can lead to situations, unlike in typical Ruby
code, where an object's memory might be released while there are still
references to that memory location. Attempting to read or write from freed
memory is generally unsafe, may not fail *immediately*, and can result in
variables "suddenly" having unexpected new values, or application crashes.

> :no_entry_sign: Incorrect:
>
> ``` ruby
> def make_object
>   # @NOTE This arena will be freed when this function returns.
>   arena = GC::Arena.allocate(1)
>   arena.eval { Object.new }
> end
>
> obj = make_object # @ERROR `obj` now refers to freed memory.
>```

> :white_check_mark: Correct:
>
> ``` ruby
> $arena = GC::Arena.allocate(1)
>
> def make_object
>   $arena.eval { Object.new }
> end
>
> obj = make_object # @NOTE `obj` refers to memory owned by `$arena`.
> ```

---

> [!IMPORTANT]
>  Rule: **Arena objects must not reference objects with shorter lifetimes.**

This is functionally equivalent to the previous rule, but it's worth calling out
separately. Variables are not the only object references that can become invalid
— instance variables, hash keys and values, and array entries all form
references that *could* become the source of a use-after-free error.

Object references become invalid when the object is freed. For arena-allocated
objects, this will happen when the owning Arena is freed; for GC-allocated
objects, this will happen when the GC cannot find any live references to the
object. Because Arena-allocated objects are not traversed by the garbage
collector, they cannot form a "live reference" for the GC.

> :no_entry_sign: Incorrect:
>
> ``` ruby
> $arena = GC::Arena.allocate(1)
> list = $arena.eval { [] }
>
> begin
>   # @NOTE This arena will be freed when this block ends.
>   $temp = GC::Arena.allocate(1)
>   list << $temp.eval { Object.new }
> end
>
> # @ERROR `list` now contains a reference to freed memory.
>```

> :no_entry_sign: Incorrect:
>
> ``` ruby
> $arena = GC::Arena.allocate(1)
> list = $arena.eval { [] }
>
> def add_point(list)
>   # @NOTE This hash is GC-allocated, and will be freed after this method call.
>   list << { x: 0, y: 0 }
> end
>
> add_point(list) # @ERROR `list` now contains a reference to freed memory.
>```

> :white_check_mark: Correct:
>
> ``` ruby
> $arena = GC::Arena.allocate(1)
> list = $arena.eval { [] }
>
> def add_point(list)
>   list << $arena.eval { { x: 0, y: 0 } }
> end
>
> add_point(list) # @NOTE `list` only contains references owned by `$arena`.
> ```

### Resource Retention
> [!IMPORTANT]
>  Rule: **Objects bound to non-memory resources must live in the regular GC.**

Some types of objects — particularly objects from other C extensions — will have
teardown routines that are called when the object is garbage collected. Since
Arena-allocated objects aren't ever garbage collected, those routines are never
run. In cases where that behavior is more involved than just freeing allocated
memory, those objects should not be allocated inside an Arena, or retained by an
object allocated inside an Arena.

This will primarily be objects that act as a handle to an OS resource, like a
socket or a file descriptor. Creating these inside an Arena will cause the
object's memory to be freed, but the resource will remain bound until the
process terminates.

> :no_entry_sign: Incorrect:
>
> ``` ruby
> $arena = GC::Arena.allocate(1)
> $arena.eval do
>   # @ERROR `$stream` will never be properly cleaned up.
>   $stream = IO.popen("curl https://example.com/streaming-data-source")
> end
> ```

> :white_check_mark: Correct:
>
> ``` ruby
> # There is no correct way to reference these objects within an Arena.
> ```

[Untangling Lifetimes]:
    https://www.rfleury.com/p/untangling-lifetimes-the-arena-allocator
