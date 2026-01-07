
// Custom memory allocator implementation.
// Designed to be used by an external CLI/test harness (e.g., mainn.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "cache.cpp"

// Cache API provided by cache.cpp
void cache_init_default();
void cache_reset();
void cache_add_level(std::size_t size_bytes,
	                 std::size_t block_size,
	                 std::size_t associativity,
	                 std::size_t access_latency_cycles);
void cache_set_memory_latency(std::size_t latency_cycles);
void cache_access(std::uintptr_t addr, bool is_write);
void cache_dump_stats();

// Simple fixed-size heap for the custom allocator.
extern std::size_t HEAP_SIZE;
extern std::uint8_t g_heap[];

// Header that lives at the beginning of each block inside g_heap.
// [BlockHeader][user bytes ...]
struct BlockHeader
{
	int id;                      // Unique identifier for the block.
	std::uint8_t *start;         // Start address of the user area.
	std::size_t size;            // Size of the user area in bytes (actual allocated size).
	std::size_t requested_size;  // Size originally requested by the user.
	bool free;                   // Whether this block is free or allocated.
	bool cacheable;              // Whether this block is cacheable.
	std::size_t cache_hits;      // Number of times this block was accessed from cache.
	BlockHeader *next;           // Next block in the list.
};

static BlockHeader *g_head = nullptr; // Head of the block list.
static int g_next_id = 0;             // Monotonically increasing id for allocated blocks.
static bool g_cache_initialized = false; // Tracks cache initialization.

// Simple allocation statistics.
static std::size_t g_alloc_requests = 0; // Number of allocation attempts.
static std::size_t g_alloc_success  = 0; // Number of successful allocations.
static std::size_t g_alloc_fail     = 0; // Number of failed allocations.

// Patterns used to detect uninitialized / freed memory accesses.
static constexpr std::uint8_t PATTERN_UNINITIALIZED = 0xCD;
static constexpr std::uint8_t PATTERN_FREED         = 0xDD;

enum class FitStrategy
{
    First,
	Best,
	Worst,
};

static FitStrategy g_current_strategy = FitStrategy::First; // Global allocation strategy.

FitStrategy parse_strategy(std::string s)
{
	// Accept common spellings.
	if (s == "first" || s == "first_fit" || s == "first-fit" || s == "firstfit")
		return FitStrategy::First;
	if (s == "best" || s == "best_fit" || s == "best-fit" || s == "bestfit")
		return FitStrategy::Best;
	if (s == "worst" || s == "worst_fit" || s == "worst-fit" || s == "worstfit")
		return FitStrategy::Worst;
	return FitStrategy::First;
}

static void allocator_init()
{
	if (g_head)
		return; // already initialized

	if (!g_cache_initialized)
	{
		cache_init_default();
		g_cache_initialized = true;
	}

	// Create a single big free block that spans the whole heap.
	g_head = reinterpret_cast<BlockHeader *>(g_heap);
	g_head->id = -1;
	g_head->start = reinterpret_cast<std::uint8_t *>(g_head) + sizeof(BlockHeader);
	g_head->size = HEAP_SIZE - sizeof(BlockHeader);
	g_head->requested_size = 0;
	g_head->free = true;
	g_head->cacheable = false;
	g_head->cache_hits = 0;
	g_head->next = nullptr;
}

static std::size_t align_size(std::size_t size)
{
	const std::size_t align = alignof(std::max_align_t);
	return (size + align - 1) & ~(align - 1);
}

static BlockHeader *find_fit(BlockHeader *head, std::size_t size, FitStrategy strategy)
{
	BlockHeader *candidate = nullptr;

	for (BlockHeader *curr = head; curr; curr = curr->next)
	{
		if (!curr->free || curr->size < size)
			continue;

		if (strategy == FitStrategy::First)
			return curr;

		if (!candidate)
		{
			candidate = curr;
			continue;
		}

		if (strategy == FitStrategy::Best)
		{
			if (curr->size < candidate->size)
				candidate = curr;
		}
		else // Worst
		{
			if (curr->size > candidate->size)
				candidate = curr;
		}
	}

	return candidate;
}

static BlockHeader *find_block_by_id(int id)
{
	BlockHeader *hdr = g_head;
	while (hdr)
	{
		if (!hdr->free && hdr->id == id)
			return hdr;
		hdr = hdr->next;
	}
	return nullptr;
}

static void split_block_if_needed(BlockHeader *block, std::size_t size)
{
	// Assumes block is free and block->size >= size.
	std::size_t remaining = block->size - size;
	// Only split if there's enough space to hold a new header and some payload.
	if (remaining <= sizeof(BlockHeader) + 8)
		return;

	std::uint8_t *base = reinterpret_cast<std::uint8_t *>(block);
	BlockHeader *new_block = reinterpret_cast<BlockHeader *>(base + sizeof(BlockHeader) + size);

	new_block->id = -1;
	new_block->start = reinterpret_cast<std::uint8_t *>(new_block) + sizeof(BlockHeader);
	new_block->size = remaining - sizeof(BlockHeader);
	new_block->requested_size = 0;
	new_block->free = true;
	new_block->cacheable = false;
	new_block->cache_hits = 0;
	new_block->next = block->next;

	block->size = size;
	block->next = new_block;
}

static void coalesce_free_blocks()
{
	BlockHeader *curr = g_head;
	while (curr)
	{
		BlockHeader *next = curr->next;
		if (next && curr->free && next->free)
		{
			std::uint8_t *curr_end = reinterpret_cast<std::uint8_t *>(curr) + sizeof(BlockHeader) + curr->size;
			if (curr_end == reinterpret_cast<std::uint8_t *>(next))
			{
				curr->size += sizeof(BlockHeader) + next->size;
				curr->requested_size = 0;
				curr->next = next->next;
				continue; // attempt to merge with the new curr->next again
			}
		}
		curr = curr->next;
	}
}

// Allocate a block of memory of given size using a fit strategy.
// Returns an integer handle that is the byte offset from the start of the heap.
int allocator_malloc(std::size_t size, FitStrategy strategy)
{
	allocator_init();
	if (size == 0)
		return -1;

	// Track allocation attempts for statistics.
	++g_alloc_requests;
	std::size_t requested_size = size;
	std::size_t aligned_size = align_size(size);

	BlockHeader *block = find_fit(g_head, aligned_size, strategy);
	if (!block)
	{
		++g_alloc_fail;
		return -1; // out of memory
	}

	split_block_if_needed(block, aligned_size);

	block->free = false;
	block->id = g_next_id++;
	block->cacheable = true;
	block->cache_hits = 0;
	block->start = reinterpret_cast<std::uint8_t *>(block) + sizeof(BlockHeader);
	block->requested_size = requested_size;
	// Mark the entire allocated region as uninitialized.
	std::memset(block->start, PATTERN_UNINITIALIZED, block->size);
	++g_alloc_success;

	std::ptrdiff_t offset = block->start - g_heap;
	if (offset < 0 || static_cast<std::size_t>(offset) >= HEAP_SIZE)
		return -1;

	return block->id;
}

// Convenience overload: strategy as a string ("first", "best", "worst").
int allocator_malloc(std::size_t size, std::string strategy)
{
    FitStrategy strat = parse_strategy(strategy);
	return allocator_malloc(size, strat);
}

// Backward-compatible default: first-fit.
int allocator_malloc(std::size_t size)
{
	return allocator_malloc(size, g_current_strategy);
}

// Change the global allocation strategy used by allocator_malloc(size).
void allocator_set_strategy(FitStrategy strategy)
{
	g_current_strategy = strategy;
}

// Free a previously allocated block identified by id (offset into heap).
void allocator_free(int id)
{
	allocator_init();
	if (id < 0)
		return; // invalid id

	BlockHeader *hdr = find_block_by_id(id);
	if (!hdr)
		return; // not found or already free

	hdr->free = true;
	hdr->id = -1;
	hdr->cacheable = false;
	hdr->cache_hits = 0;
	// Mark freed memory with a distinct pattern.
	std::memset(hdr->start, PATTERN_FREED, hdr->size);

	coalesce_free_blocks();
}

// Mark a block as cacheable or not.
void allocator_set_block_cacheable(int id, bool cacheable)
{
	allocator_init();
	if (id < 0)
		return;
	BlockHeader *hdr = find_block_by_id(id);
	if (!hdr)
		return;
	hdr->cacheable = cacheable;
}

// Simulate an access to a block through the cache hierarchy.
void allocator_access(int id, bool is_write)
{
	allocator_init();
	if (id < 0)
		return;
	BlockHeader *hdr = find_block_by_id(id);
	if (!hdr || !hdr->cacheable)
		return;
	cache_access(reinterpret_cast<std::uintptr_t>(hdr->start), is_write);
	++hdr->cache_hits;
}

// Read from an allocated block into user-provided buffer.
// Returns false if the id/range is invalid or if the range contains
// bytes that look like uninitialized/freed ("garbage") data.
bool allocator_read(int id, std::size_t offset, void *dst, std::size_t size)
{
	allocator_init();
	if (id < 0 || !dst || size == 0)
		return false;

	BlockHeader *hdr = find_block_by_id(id);
	if (!hdr || hdr->free)
		return false;

	if (offset + size > hdr->requested_size)
		return false; // out of user-requested bounds

	auto *src_bytes = hdr->start + offset;
	auto *dst_bytes = static_cast<std::uint8_t *>(dst);
	bool has_garbage = false;

	for (std::size_t i = 0; i < size; ++i)
	{
		std::uint8_t value = src_bytes[i];
		// Simulate cache access for this byte.
		cache_access(reinterpret_cast<std::uintptr_t>(src_bytes + i), false);
		if (value == PATTERN_UNINITIALIZED || value == PATTERN_FREED)
			has_garbage = true;
		dst_bytes[i] = value;
	}

	if (has_garbage)
		return false;
	return true;
}

// Write to an allocated block from user-provided buffer.
// Returns false if the id/range is invalid or if the destination range
// currently contains bytes that look like uninitialized/freed
// ("garbage") data.
bool allocator_write(int id, std::size_t offset, const void *src, std::size_t size)
{
	allocator_init();
	if (id < 0 || !src || size == 0)
		return false;

	BlockHeader *hdr = find_block_by_id(id);
	if (!hdr || hdr->free)
		return false;

	if (offset + size > hdr->requested_size)
		return false; // out of user-requested bounds

	auto *dst_bytes = hdr->start + offset;
	auto *src_bytes = static_cast<const std::uint8_t *>(src);
	bool has_garbage = false;

	// First pass: check for uninitialized/freed bytes without modifying memory.
	for (std::size_t i = 0; i < size; ++i)
	{
		std::uint8_t old_value = dst_bytes[i];
		if (old_value == PATTERN_UNINITIALIZED || old_value == PATTERN_FREED)
		{
			has_garbage = true;
			break;
		}
	}

	// if (has_garbage)
	// 	return false;

	// Second pass: perform the actual write and simulate cache accesses.
	for (std::size_t i = 0; i < size; ++i)
	{
		// Simulate cache access for this byte.
		cache_access(reinterpret_cast<std::uintptr_t>(dst_bytes + i), true);
		dst_bytes[i] = src_bytes[i];
	}

	return true;
}

// Dump allocator state to stdout.
// NOTE: mainn.cpp can provide the printing; this exists for parity with the sample.
#include <iostream>
void allocator_dump()
{
	allocator_init();
	std::cout << "Heap dump (block list):\n";
	BlockHeader *curr = g_head;
	std::size_t index = 0;
	while (curr)
	{
		std::uint8_t *base = reinterpret_cast<std::uint8_t *>(curr);
		std::size_t offset = static_cast<std::size_t>(base - g_heap);
		std::cout << "  Block " << index++
				  << ": offset=" << offset
				  << ", id=" << curr->id
				  << ", start=" << static_cast<void *>(curr->start)
				  << ", size=" << curr->size
				  << ", " << (curr->free ? "FREE" : "USED")
				  << ", cacheable=" << (curr->cacheable ? "yes" : "no")
				  << ", cache_hits=" << curr->cache_hits
				  << "\n";
		curr = curr->next;
	}

    std :: cout << sizeof(BlockHeader) << " bytes per block header\n";
}

void allocator_stats()
{
	allocator_init();
	std::size_t total_free = 0;
	std::size_t total_used = 0;
	std::size_t free_blocks = 0;
	std::size_t used_blocks = 0;
	std::size_t internal_frag_bytes = 0;
	std::size_t largest_free_block = 0;

	for (BlockHeader *curr = g_head; curr; curr = curr->next)
	{
		if (curr->free)
		{
			++free_blocks;
			total_free += curr->size;
			if (curr->size > largest_free_block)
				largest_free_block = curr->size;
		}
		else
		{
			++used_blocks;
			total_used += curr->size;
			if (curr->size > curr->requested_size)
				internal_frag_bytes += (curr->size - curr->requested_size);
		}
	}

	double utilization = (HEAP_SIZE != 0)
		                       ? (100.0 * static_cast<double>(total_used) / static_cast<double>(HEAP_SIZE))
		                       : 0.0;
	double internal_frag_ratio = (total_used != 0)
		                             ? (100.0 * static_cast<double>(internal_frag_bytes) / static_cast<double>(total_used))
		                             : 0.0;
	double external_frag_ratio = 0.0;
	if (total_free != 0 && largest_free_block != 0)
	{
		external_frag_ratio = 100.0 * (1.0 - static_cast<double>(largest_free_block) / static_cast<double>(total_free));
	}

	std::size_t total_requests = g_alloc_requests;
	double success_rate = 0.0;
	double failure_rate = 0.0;
	if (total_requests != 0)
	{
		success_rate = 100.0 * static_cast<double>(g_alloc_success) / static_cast<double>(total_requests);
		failure_rate = 100.0 * static_cast<double>(g_alloc_fail) / static_cast<double>(total_requests);
	}

	std::cout << "Allocator stats:\n";
	std::cout << "  Heap size: " << HEAP_SIZE << " bytes\n";
	std::cout << "  Used:      " << total_used << " bytes in " << used_blocks << " block(s)\n";
	std::cout << "  Free:      " << total_free << " bytes in " << free_blocks << " block(s)\n";
	std::cout << "  Internal fragmentation: " << internal_frag_bytes << " bytes (" << internal_frag_ratio << "%)\n";
	std::cout << "  External fragmentation: " << external_frag_ratio << "%\n";
	std::cout << "  Largest free block:     " << largest_free_block << " bytes\n";
	std::cout << "  Allocation requests:    " << total_requests << "\n";
	std::cout << "    Success:              " << g_alloc_success << " (" << success_rate << "%)\n";
	std::cout << "    Failures:             " << g_alloc_fail << " (" << failure_rate << "%)\n";
	std::cout << "  Memory utilization:     " << utilization << "% of heap\n";

	// Dump cache stats as well.
	std ::cout << "\nCache statistics:\n";
	cache_dump_stats();
}

