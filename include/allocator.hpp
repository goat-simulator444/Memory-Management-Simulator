// allocator.hpp
// Public API for the custom memory allocator implemented in src/allocator.cpp.

#pragma once

#include <cstddef> // std::size_t
#include <string>  // std::string

// Strategy used to choose a free block from the heap.
enum class FitStrategy
{
	First,
	Best,
	Worst,
};

// Allocate a block of memory of the given size using the specified fit strategy.
// Returns an integer handle that is the byte offset from the start of the heap,
// or -1 on failure.
int allocator_malloc(std::size_t size, FitStrategy strategy);

// Convenience overload: specify the strategy as a string
// ("first", "best", "worst" and common variants).
int allocator_malloc(std::size_t size, const std::string &strategy);

// Backward-compatible default: first-fit strategy.
int allocator_malloc(std::size_t size);

// Free a previously allocated block identified by its handle (offset into heap).
void allocator_free(int id);

// Dump the allocator's internal state to stdout.
void allocator_dump();

// Print basic allocator statistics (total used/free bytes and blocks).
void allocator_stats();
