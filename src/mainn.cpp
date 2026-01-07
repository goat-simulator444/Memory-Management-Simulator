// Simple CLI interface for a custom memory allocator.
// Commands:
//   malloc <size> [strategy]
//   free <ptr_id>
//   dump
//   stats
//   read <id> <offset> <size>
//   write <id> <offset> <data...>
//   help
//   exit / quit

#include <iostream>
#include <sstream>
#include <string>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <cctype>
#include "allocator.cpp"

using namespace std;

std::size_t HEAP_SIZE = 64 * 1024; // 64 KB heap size
std::uint8_t g_heap[64 * 1024];   // Heap memory

// Import allocator API implemented in allocator.cpp
// int allocator_malloc(std::size_t size);
// int allocator_malloc(std::size_t size, const std::string &strategy);
// void allocator_free(int id);
// void allocator_dump();
// void allocator_stats();
// bool allocator_read(int id, std::size_t offset, void *dst, std::size_t size);
// bool allocator_write(int id, std::size_t offset, const void *src, std::size_t size);

static void print_help()
{
	std::cout << "Available commands:\n"
			  << "  malloc <size> [strategy] - allocate <size> bytes using optional strategy (first|best|worst)\n"
			  << "  free <id>                - free the block identified by <id>\n"
			  << "  dump                     - show all memory blocks\n"
			  << "  stats                    - show allocator statistics (e.g., fragmentation)\n"
			  << "  read <id> <off> <size>   - read <size> bytes from block <id> at offset <off>\n"
			  << "  write <id> <off> <data>  - write ASCII <data> into block <id> at offset <off>\n"
			  << "  cache                    - open cache configuration menu\n"
			  << "  help                     - show this help message\n"
			  << "  exit | quit              - exit the program\n";
}

int main()
{
	std::string line;
	print_help();

	while (true)
	{
		cout << "\n";
		std::cout << "allocator> " << std::flush;
		if (!std::getline(std::cin, line))
			break;

		std::istringstream iss(line);
		std::string cmd;
		if (!(iss >> cmd))
			continue; // empty line

		if (cmd == "malloc")
		{
			std::size_t size = 0;
			if (!(iss >> size))
			{
				std::cout << "Usage: malloc <size> [strategy]\n";
				continue;
			}
			std::string strategy;
			int id;
			if (iss >> strategy)
				id = allocator_malloc(size, strategy);
			else
				id = allocator_malloc(size);
			std::cout << "Allocated id=" << id << " for size=" << size << "\n";
		}
		else if (cmd == "free")
		{
			int id = -1;
			if (!(iss >> id))
			{
				std::cout << "Usage: free <id>\n";
				continue;
			}
			allocator_free(id);
			std::cout << "Freed id=" << id << "\n";
		}
		else if (cmd == "dump")
		{
			allocator_dump();
		}
		else if (cmd == "stats")
		{
			allocator_stats();
		}
		else if (cmd == "read")
		{
			int id = -1;
			std::size_t offset = 0;
			std::size_t size = 0;
			if (!(iss >> id >> offset >> size))
			{
				std::cout << "Usage: read <id> <offset> <size>\n";
				continue;
			}

			if (size == 0)
			{
				std::cout << "Size must be > 0\n";
				continue;
			}

			std::string buffer(size, '\0');
			bool ok = allocator_read(id, offset, &buffer[0], size);
			if (!ok)
			{
				std::cout << "Read failed (invalid id/range or uninitialized/freed data).\n";
				continue;
			}

			std::cout << "Data (ASCII): ";
			for (std::size_t i = 0; i < size; ++i)
			{
				unsigned char c = static_cast<unsigned char>(buffer[i]);
				if (std::isprint(c))
					std::cout << static_cast<char>(c);
				else
					std::cout << '.';
			}
			std::cout << "\nData (hex): ";
			std::cout << std::hex << std::setfill('0');
			for (std::size_t i = 0; i < size; ++i)
			{
				std::cout << std::setw(2)
						  << static_cast<unsigned int>(static_cast<unsigned char>(buffer[i]))
						  << ' ';
			}
			std::cout << std::dec << "\n";
		}
		else if (cmd == "write")
		{
			int id = -1;
			std::size_t offset = 0;
			if (!(iss >> id >> offset))
			{
				std::cout << "Usage: write <id> <offset> <data...>\n";
				continue;
			}

			std::string data;
			std::getline(iss, data);
			// Trim leading spaces from remaining line.
			std::size_t first = data.find_first_not_of(' ');
			if (first == std::string::npos)
			{
				std::cout << "Usage: write <id> <offset> <data...>\n";    
				continue;
			}
			data.erase(0, first);

			std::size_t size = data.size();
			bool ok = allocator_write(id, offset, data.data(), size);
			if (!ok)
			{
				std::cout << "Write failed (invalid id/range or destination contains uninitialized/freed data).\n";
				continue;
			}

			std::cout << "Wrote " << size << " byte(s) to block id=" << id
					  << " at offset=" << offset << "\n";
		}
		else if (cmd == "cache")
		{
			cache_menu_loop();
		}
		else if (cmd == "help")
		{
			print_help();
		}
		else if (cmd == "exit" || cmd == "quit")
		{
			break;
		}
		else
		{
			std::cout << "Unknown command: " << cmd << " (type 'help' for usage)\n";
		}
	}

	return 0;
}

