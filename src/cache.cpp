// Multi-level cache simulation usable from allocator.cpp and mainn.cpp.
//
// Features:
//   - Arbitrary number of cache levels (L1, L2, ...).
//   - Per-level configurable total size, block size, associativity and latency.
//   - Direct-mapped (associativity = 1) or set-associative caches.
//   - LFU (Least Frequently Used) replacement with LRU tie‑break.
//   - Tracks hits/misses per level, hit ratios and average miss penalties
//     (penalty propagation to lower levels and main memory).

#include <cstddef>
#include <cstdint>
#include <vector>
#include <iostream>
#include <iomanip>
#include <limits>

// ------------------------- Internal Types ------------------------- //

struct CacheLine
{
	bool valid = false;
	std::uintptr_t tag = 0;     // Tag of the cached block.
	std::uint64_t freq = 0;     // LFU counter.
	std::uint64_t last_used = 0; // For LRU tie-breaking.
};

struct CacheLevelStats
{
	std::uint64_t accesses = 0;
	std::uint64_t hits = 0;
	std::uint64_t misses = 0;
	std::uint64_t miss_penalty_accum = 0; // Extra penalty due to going to lower levels.
};

class CacheLevel
{
public:
	CacheLevel(std::size_t size_bytes,
	           std::size_t block_size,
	           std::size_t associativity,
	           std::size_t access_latency_cycles,
	           std::size_t level_index)
	    : m_size_bytes(size_bytes),
	      m_block_size(block_size ? block_size : 1),
	      m_associativity(associativity ? associativity : 1),
	      m_latency(access_latency_cycles ? access_latency_cycles : 1),
	      m_level_index(level_index)
	{
		if (m_block_size == 0)
			m_block_size = 1;

		// Determine number of lines and sets.
		std::size_t num_lines = m_size_bytes / m_block_size;
		if (num_lines == 0)
			num_lines = 1; // at least one line.

		if (m_associativity == 0)
			m_associativity = 1;
		if (m_associativity > num_lines)
			m_associativity = num_lines;

		m_num_sets = num_lines / m_associativity;
		if (m_num_sets == 0)
		{
			m_num_sets = 1;
			m_associativity = num_lines; // fully-associative fallback.
		}

		m_sets.resize(m_num_sets, std::vector<CacheLine>(m_associativity));
	}

	std::size_t latency() const { return m_latency; }
	std::size_t level_index() const { return m_level_index; }

	const CacheLevelStats &stats() const { return m_stats; }
	CacheLevelStats &stats() { return m_stats; }

	std::size_t size_bytes() const { return m_size_bytes; }
	std::size_t block_size() const { return m_block_size; }
	std::size_t associativity() const { return m_associativity; }
	std::size_t num_sets() const { return m_num_sets; }

	// Lookup an address. Returns true on hit and updates LFU/LRU data.
	bool access(std::uintptr_t addr, std::uint64_t timestamp)
	{
		std::size_t set_idx;
		std::uintptr_t tag;
		compute_index_tag(addr, set_idx, tag);

		auto &set = m_sets[set_idx];
		for (auto &line : set)
		{
			if (line.valid && line.tag == tag)
			{
				++line.freq;          // LFU count
				line.last_used = timestamp; // LRU tie-break
				return true;
			}
		}
		return false;
	}

	// Insert (or update) a line corresponding to addr using LFU replacement.
	void insert(std::uintptr_t addr, std::uint64_t timestamp)
	{
		std::size_t set_idx;
		std::uintptr_t tag;
		compute_index_tag(addr, set_idx, tag);

		auto &set = m_sets[set_idx];

		// First try to find an invalid line.
		for (auto &line : set)
		{
			if (!line.valid)
			{
				line.valid = true;
				line.tag = tag;
				line.freq = 1;
				line.last_used = timestamp;
				return;
			}
		}

		// No invalid line; choose victim via LFU with LRU tie-break.
		std::size_t victim_idx = 0;
		for (std::size_t i = 1; i < set.size(); ++i)
		{
			const auto &curr = set[i];
			const auto &vict = set[victim_idx];
			if (curr.freq < vict.freq)
			{
				victim_idx = i;
			}
			else if (curr.freq == vict.freq && curr.last_used < vict.last_used)
			{
				victim_idx = i; // older => replace first
			}
		}

		CacheLine &victim = set[victim_idx];
		victim.valid = true;
		victim.tag = tag;
		victim.freq = 1;
		victim.last_used = timestamp;
	}

private:
	void compute_index_tag(std::uintptr_t addr, std::size_t &set_idx, std::uintptr_t &tag) const
	{
		std::uintptr_t block_addr = addr / m_block_size;
		set_idx = static_cast<std::size_t>(block_addr % m_num_sets);
		tag = block_addr / m_num_sets;
	}

private:
	std::size_t m_size_bytes;
	std::size_t m_block_size;
	std::size_t m_associativity;
	std::size_t m_latency;      // cycles per access at this level
	std::size_t m_num_sets;
	std::size_t m_level_index;  // 0 for L1, 1 for L2, ...

	std::vector<std::vector<CacheLine>> m_sets;
	CacheLevelStats m_stats;
};

// ---------------------- Multi-level controller -------------------- //

class MultiLevelCache
{
public:
	MultiLevelCache() = default;

	void reset()
	{
		m_levels.clear();
		m_memory_latency = 100; // default main memory penalty (cycles)
		m_timestamp = 0;
		m_total_accesses = 0;
		m_total_hits = 0;
		m_total_misses = 0;
		m_total_penalty = 0;
	}

	void set_memory_latency(std::size_t latency_cycles)
	{
		m_memory_latency = latency_cycles ? latency_cycles : 1;
	}

	// Append a new cache level (L1 is index 0, L2 is 1, ...).
	void add_level(std::size_t size_bytes,
	              std::size_t block_size,
	              std::size_t associativity,
	              std::size_t access_latency_cycles)
	{
		std::size_t level_index = m_levels.size();
		m_levels.emplace_back(size_bytes, block_size, associativity, access_latency_cycles, level_index);
	}

	std::size_t level_count() const
	{
		return m_levels.size();
	}

	void configure_level(std::size_t level_index,
	                     std::size_t size_bytes,
	                     std::size_t block_size,
	                     std::size_t associativity,
	                     std::size_t access_latency_cycles)
	{
		if (level_index >= m_levels.size())
			return;
		m_levels[level_index] = CacheLevel(size_bytes, block_size, associativity, access_latency_cycles, level_index);
	}

	// Perform a read/write access and update statistics.
	// The access is address-based; allocator/main can choose any
	// scheme for mapping its ids/offsets to an address.
	void access(std::uintptr_t addr, bool /*is_write*/)
	{
		if (m_levels.empty())
			return;

		++m_timestamp;
		++m_total_accesses;

		std::size_t total_penalty = 0;
		bool hit_any = false;
		int level_hit = -1;

		struct MissRecord
		{
			int level;
			std::size_t penalty_upto_level; // including this level's latency
		};
		std::vector<MissRecord> miss_records;

		// Walk through each cache level.
		for (std::size_t i = 0; i < m_levels.size(); ++i)
		{
			CacheLevel &lvl = m_levels[i];
			CacheLevelStats &st = lvl.stats();

			total_penalty += lvl.latency();
			++st.accesses;

			if (lvl.access(addr, m_timestamp))
			{
				++st.hits;
				hit_any = true;
				level_hit = static_cast<int>(i);
				break;
			}
			else
			{
				++st.misses;
				miss_records.push_back({static_cast<int>(i), total_penalty});
			}
		}

		// If no cache level hit, go to main memory.
		if (!hit_any)
		{
			total_penalty += m_memory_latency;
			level_hit = static_cast<int>(m_levels.size()); // "memory" index
			++m_total_misses;
		}
		else
		{
			++m_total_hits;
		}

		// Propagate line into all levels up to and including the level
		// where the hit/memory access occurred (inclusive cache model).
		int fill_upto = level_hit;
		if (fill_upto == static_cast<int>(m_levels.size()))
		{
			// Miss in all levels, fetched from memory; fill all levels.
			fill_upto = static_cast<int>(m_levels.size()) - 1;
		}
		for (int i = 0; i <= fill_upto && i >= 0; ++i)
		{
			m_levels[static_cast<std::size_t>(i)].insert(addr, m_timestamp);
		}

		// Attribute miss penalty propagation to each level that missed.
		for (const auto &rec : miss_records)
		{
			if (rec.level >= 0 && static_cast<std::size_t>(rec.level) < m_levels.size())
			{
				std::size_t extra_penalty = 0;
				if (total_penalty > rec.penalty_upto_level)
					extra_penalty = total_penalty - rec.penalty_upto_level;
				m_levels[static_cast<std::size_t>(rec.level)].stats().miss_penalty_accum += extra_penalty;
			}
		}

		m_total_penalty += total_penalty;
	}

	void dump_stats(std::ostream &os) const
	{
		os << "Multi-level cache statistics:\n";
		os << "  Levels: " << m_levels.size() << "\n";
		os << "  Main memory latency: " << m_memory_latency << " cycles\n";
		os << "  Total accesses: " << m_total_accesses << "\n";
		os << "  Total hits:     " << m_total_hits << "\n";
		os << "  Total misses:   " << m_total_misses << "\n";
		double global_hit_ratio = 0.0;
		if (m_total_accesses)
			global_hit_ratio = 100.0 * static_cast<double>(m_total_hits) / static_cast<double>(m_total_accesses);
		os << "  Global hit ratio: " << std::fixed << std::setprecision(2)
		   << global_hit_ratio << "%\n";
		double avg_penalty = 0.0;
		if (m_total_accesses)
			avg_penalty = static_cast<double>(m_total_penalty) / static_cast<double>(m_total_accesses);
		os << "  Avg access penalty: " << avg_penalty << " cycles/access\n";

		os << "\nPer-level details:\n";
		for (std::size_t i = 0; i < m_levels.size(); ++i)
		{
			const CacheLevel &lvl = m_levels[i];
			const CacheLevelStats &st = lvl.stats();
			os << "  L" << (i + 1) << ": size=" << lvl.size_bytes()
			   << " bytes, block=" << lvl.block_size()
			   << " bytes, assoc=" << lvl.associativity()
			   << "-way, sets=" << lvl.num_sets()
			   << ", latency=" << lvl.latency() << " cycles\n";
			os << "     accesses=" << st.accesses
			   << ", hits=" << st.hits
			   << ", misses=" << st.misses;
			double hit_ratio = 0.0;
			if (st.accesses)
				hit_ratio = 100.0 * static_cast<double>(st.hits) / static_cast<double>(st.accesses);
			os << ", hit ratio=" << std::fixed << std::setprecision(2)
			   << hit_ratio << "%";
			double avg_miss_penalty = 0.0;
			if (st.misses)
				avg_miss_penalty = static_cast<double>(st.miss_penalty_accum) / static_cast<double>(st.misses);
			os << ", avg miss penalty to lower levels=" << avg_miss_penalty << " cycles\n";
		}
	}

private:
	std::vector<CacheLevel> m_levels;
	std::size_t m_memory_latency = 100; // cycles to access main memory
	std::uint64_t m_timestamp = 0;      // global logical time for LRU tie-breaks

	std::uint64_t m_total_accesses = 0;
	std::uint64_t m_total_hits = 0;   // hit in any cache level
	std::uint64_t m_total_misses = 0; // went to main memory
	std::uint64_t m_total_penalty = 0; // total cycles for all accesses
};

// -------------------------- Global API ---------------------------- //

static MultiLevelCache g_cache;

// Initialize cache with two default levels (L1, L2).
// Users can instead call cache_reset() + cache_add_level() manually.
void cache_init_default()
{
	g_cache.reset();
	// Example defaults:
	//   L1: 4 KB, 64-byte blocks, 4-way set-associative, 1 cycle latency.
	//   L2: 32 KB, 64-byte blocks, 8-way set-associative, 8 cycles latency.
	g_cache.add_level(4 * 1024, 64, 4, 1);
	g_cache.add_level(32 * 1024, 64, 8, 8);
	// Main memory latency (can be overridden).
	g_cache.set_memory_latency(100);
}

void cache_reset()
{
	g_cache.reset();
}

void cache_add_level(std::size_t size_bytes,
	                   std::size_t block_size,
	                   std::size_t associativity,
	                   std::size_t access_latency_cycles)
{
	g_cache.add_level(size_bytes, block_size, associativity, access_latency_cycles);
}

void cache_configure_level(std::size_t level_index,
	                        std::size_t size_bytes,
	                        std::size_t block_size,
	                        std::size_t associativity,
	                        std::size_t access_latency_cycles)
{
	g_cache.configure_level(level_index, size_bytes, block_size, associativity, access_latency_cycles);
}

std::size_t cache_get_level_count()
{
	return g_cache.level_count();
}

void cache_set_memory_latency(std::size_t latency_cycles)
{
	g_cache.set_memory_latency(latency_cycles);
}

// Perform a cache access. The address can be any value the caller
// wishes to use (e.g., g_heap offset, BlockHeader::id, or a pointer).
void cache_access(std::uintptr_t addr, bool is_write)
{
	g_cache.access(addr, is_write);
}

// Print cache statistics to std::cout.
void cache_dump_stats()
{
	g_cache.dump_stats(std::cout);
}

// Interactive cache configuration and testing menu.
void cache_menu_loop()
{
	using std::cin;
	using std::cout;
	using std::endl;

	bool running = true;
	while (running)
	{
		cout << "\n\n=== Cache Configuration Menu ===\n"
		     << "1) Initialize default cache\n"
		     << "2) Reset cache (no levels)\n"
		     << "3) Add cache level\n"
		     << "4) Configure existing cache level\n"
		     << "5) Dump cache statistics\n"
		     << "0) Exit cache menu\n"
		     << "\nallocator>cache> " << std::flush;

		int choice;
		if (!(cin >> choice))
		{
			cin.clear();
			cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
			continue;
		}

		switch (choice)
		{
		case 1:
			cache_init_default();
			break;
		case 2:
			cache_reset();
			break;
		case 3:
		{
			std::size_t size_bytes, block_size, associativity, latency;
			cout << "Enter level size in bytes: ";
			if (!(cin >> size_bytes)) break;
			cout << "Enter block size in bytes: ";
			if (!(cin >> block_size)) break;
			cout << "Enter associativity (ways): ";
			if (!(cin >> associativity)) break;
			cout << "Enter access latency (cycles): ";
			if (!(cin >> latency)) break;
			cache_add_level(size_bytes, block_size, associativity, latency);
			break;
		}
		case 4:
		{
			std::size_t level_count = cache_get_level_count();
			if (level_count == 0)
			{
				cout << "No cache levels to configure." << endl;
				break;
			}

			std::size_t level, size_bytes, block_size, associativity, latency;
			cout << "Existing levels: " << level_count << " (L1..L" << level_count << ")" << endl;
			cout << "Select level number to configure (1-based): ";
			if (!(cin >> level)) break;
			if (level == 0 || level > level_count)
			{
				cout << "Invalid level." << endl;
				break;
			}
			cout << "Enter new size in bytes: ";
			if (!(cin >> size_bytes)) break;
			cout << "Enter new block size in bytes: ";
			if (!(cin >> block_size)) break;
			cout << "Enter new associativity (ways): ";
			if (!(cin >> associativity)) break;
			cout << "Enter new access latency (cycles): ";
			if (!(cin >> latency)) break;
			cache_configure_level(level - 1, size_bytes, block_size, associativity, latency);
			break;
		}
		case 5:
			cache_dump_stats();
			break;
		case 0:
			running = false;
			break;
		default:
			cout << "Unknown option." << endl;
			break;
		}
	}
}
