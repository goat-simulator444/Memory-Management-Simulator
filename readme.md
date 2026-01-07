# Custom Memory Allocator & Multi‑Level Cache Simulator

This project implements a simple heap allocator and a fully configurable multi‑level cache simulator, exposed via a text‑based CLI.

- **Allocator**: fixed 64&nbsp;KB heap, supports **first‑fit**, **best‑fit**, and **worst‑fit** placement strategies.
- **Safety checks**: detects reads from uninitialized / freed memory patterns.
- **Cache**: arbitrary number of cache levels (L1, L2, …) with configurable size, block size, associativity, and latency using an inclusive cache model with LFU+LRU replacement.
- **Statistics**: detailed per‑level and global stats for both the allocator and the cache hierarchy.

Core implementation:

- Allocator and heap logic: [src/allocator.cpp](src/allocator.cpp), [include/allocator.hpp](include/allocator.hpp)
- Cache simulator and menu: [src/cache.cpp](src/cache.cpp)
- CLI front‑end: [src/mainn.cpp](src/mainn.cpp)

Demo Video : https://drive.google.com/file/d/127vFHmHc-3mgU7mIgZ0JPNqNu3fMpxpT/view?usp=sharing  
---

## Building

On a typical g++/Clang setup:

```bash
make
```

Then run:

```bash
mainn
```

On Windows with MinGW, for example:

```bash
mingw32-make
.\mainn.exe
```

---

## CLI Overview

When you start the program you are dropped into the `allocator>` prompt. The following commands are available (see [src/mainn.cpp](src/mainn.cpp)):

- `malloc <size> [strategy]`  
	Allocate a block of `<size>` bytes.
	- `strategy` is optional and can be one of:
		- `first`, `first_fit`, `first-fit` – first‑fit
		- `best`, `best_fit`, `best-fit` – best‑fit
		- `worst`, `worst_fit`, `worst-fit` – worst‑fit
	- Returns an integer **id** for the allocated block which is used in other commands.

- `free <id>`  
	Free the previously allocated block identified by `<id>`.

- `dump`  
	Print a detailed dump of the internal heap block list, including offsets, sizes, allocation state, and cache‑related counters.

- `stats`  
	Show allocator statistics: total used/free bytes and blocks, internal/external fragmentation, largest free block, number of allocation requests and their success/failure rates, overall heap utilization, and **cache statistics for every level** in the hierarchy.

- `read <id> <off> <size>`  
	Read `<size>` bytes starting at offset `<off>` from block `<id>`.
	- Fails if the range is out of bounds or contains bytes that still look like *uninitialized* or *freed* memory.
	- On success, prints the data both as ASCII (non‑printables shown as `.`) and as hexadecimal bytes.

- `write <id> <off> <data...>`  
	Write the ASCII string `<data>` into block `<id>` starting at offset `<off>`.
	- Fails if the range is out of bounds or the destination region is considered invalid.
	- Each written byte generates a cache access so the cache statistics reflect realistic usage.

- `cache`  
	Enter an interactive **cache configuration menu** (described below).

- `help`  
	Display a short description of all commands.

- `exit` / `quit`  
	Leave the CLI.

---

## Allocation Strategies

The allocator manages a single contiguous heap region and keeps a linked list of blocks, each preceded by a small header. A **fit strategy** determines which free block is chosen to satisfy a new `malloc` request.

- **First‑fit**: traverse the free list and take the first block whose size is large enough.
- **Best‑fit**: choose the smallest free block that is still large enough, minimizing leftover space.
- **Worst‑fit**: choose the largest available free block, trying to leave larger contiguous regions elsewhere.

Internally, allocations are aligned to `std::max_align_t` and large free regions may be split into an allocated block plus a remainder free block. Adjacent free blocks are coalesced when memory is freed, which helps reduce external fragmentation.

### Safety & Debugging Patterns

- Newly allocated bytes are filled with a special *uninitialized* pattern.
- Freed bytes are filled with a distinct *freed* pattern.
- `read` fails if any byte in the requested range is still marked as uninitialized/freed.
- `write` simulates cache accesses for each byte written so that cache stats reflect real access behavior.

These checks make it easier to experiment with common memory‑safety bugs (use‑after‑free, uninitialized reads) without relying on the system allocator.

---

## Multi‑Level Cache System

The cache simulator models a hierarchy of levels (L1, L2, …) that sit in front of the heap. Every read/write performed by the allocator generates cache accesses; the cache tracks hits, misses, and penalties per level.

Key properties:

- **Arbitrary number of levels** – you can build just an L1 cache or a deeper hierarchy.
- **Per‑level configuration**:
	- total size in bytes
	- block size (line size) in bytes
	- associativity (1‑way direct mapped up to fully associative)
	- access latency in cycles
- **Replacement policy**: LFU (Least Frequently Used) with LRU (Least Recently Used) tie‑breaking within each set.
- **Inclusive cache**: when a miss occurs and data is fetched, lines are filled into all levels up to the level that satisfied the access.
- **Main memory latency**: global penalty in cycles for going all the way to memory; configurable.

### Cache Statistics

The cache keeps both global and per‑level statistics and prints them when:

- you run `stats` from the allocator prompt, or
- you choose "Dump cache statistics" from the cache menu.

Per‑level stats include:

- number of accesses, hits, and misses
- hit ratio per level
- average miss penalty propagated to lower levels / main memory
- configuration summary (size, block size, associativity, latency, number of sets)

Global stats include:

- total accesses, hits, misses
- overall hit ratio across the entire hierarchy
- average access penalty in cycles per access

---

## Cache Configuration Menu

From the main `allocator>` prompt, run:

- `cache`

You will see a nested prompt `allocator>cache>` with these options (see [src/cache.cpp](src/cache.cpp)):

1. **Initialize default cache**  
	 Reset the cache and create a default 2‑level hierarchy (e.g., small fast L1, larger slower L2) plus a default main‑memory latency.

2. **Reset cache (no levels)**  
	 Clear all cache levels and reset statistics. Useful if you want to build a hierarchy from scratch.

3. **Add cache level**  
	 Append a new level to the hierarchy by specifying:
	 - level size in bytes
	 - block size (line size) in bytes
	 - associativity (number of ways)
	 - access latency in cycles

4. **Configure existing cache level**  
	 Modify an existing level by index (L1 is 1, L2 is 2, …) and override its size, block size, associativity, and latency.

5. **Dump cache statistics**  
	 Print the full per‑level and global statistics described above.

0. **Exit cache menu**  
	 Return to the main allocator prompt.

---

## Typical Workflow

1. Start the CLI and, optionally, configure the cache via the `cache` menu.
2. Allocate one or more blocks with `malloc <size> [strategy]`.
3. Use `write` and `read` to generate realistic access patterns.
4. Inspect heap layout with `dump`.
5. Inspect allocator and per‑level cache performance with `stats`.

This makes the project a compact playground for experimenting with heap allocation strategies, fragmentation behaviour, and multi‑level cache design.


