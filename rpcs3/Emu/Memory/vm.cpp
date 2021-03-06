#include "stdafx.h"
#include "Utilities/Log.h"
#include "Memory.h"
#include "Emu/System.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/ARMv7/ARMv7Thread.h"

#include "Emu/SysCalls/lv2/sys_time.h"

#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

/* OS X uses MAP_ANON instead of MAP_ANONYMOUS */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

namespace vm
{
#ifdef _WIN32
	HANDLE g_memory_handle;
#endif

	void* g_priv_addr;

	void* initialize()
	{
#ifdef _WIN32
		g_memory_handle = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE | SEC_RESERVE, 0x1, 0x0, NULL);

		void* base_addr = MapViewOfFile(g_memory_handle, FILE_MAP_WRITE, 0, 0, 0x100000000); // main memory
		g_priv_addr = MapViewOfFile(g_memory_handle, FILE_MAP_WRITE, 0, 0, 0x100000000); // memory mirror for privileged access

		return base_addr;

		//return VirtualAlloc(nullptr, 0x100000000, MEM_RESERVE, PAGE_NOACCESS);
#else
		//shm_unlink("/rpcs3_vm");

		int memory_handle = shm_open("/rpcs3_vm", O_RDWR | O_CREAT | O_EXCL, 0);

		if (memory_handle == -1)
		{
			printf("shm_open() failed\n");
			return (void*)-1;
		}

		ftruncate(memory_handle, 0x100000000);

		void* base_addr = mmap(nullptr, 0x100000000, PROT_NONE, MAP_SHARED, memory_handle, 0);
		g_priv_addr = mmap(nullptr, 0x100000000, PROT_NONE, MAP_SHARED, memory_handle, 0);

		shm_unlink("/rpcs3_vm");

		return base_addr;

		//return mmap(nullptr, 0x100000000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
#endif
	}

	void finalize()
	{
#ifdef _WIN32
		UnmapViewOfFile(g_base_addr);
		UnmapViewOfFile(g_priv_addr);
		CloseHandle(g_memory_handle);
#else
		munmap(g_base_addr, 0x100000000);
		munmap(g_priv_addr, 0x100000000);
#endif
	}

	void* const g_base_addr = (atexit(finalize), initialize());

	class reservation_mutex_t
	{
		std::atomic<NamedThreadBase*> m_owner;
		std::condition_variable m_cv;
		std::mutex m_cv_mutex;

	public:
		reservation_mutex_t()
			: m_owner(nullptr)
		{
		}

		bool do_notify;

		__noinline void lock()
		{
			NamedThreadBase* owner = GetCurrentNamedThread();
			NamedThreadBase* old = nullptr;

			while (!m_owner.compare_exchange_strong(old, owner))
			{
				std::unique_lock<std::mutex> cv_lock(m_cv_mutex);

				m_cv.wait_for(cv_lock, std::chrono::milliseconds(1));

				if (old == owner)
				{
					throw __FUNCTION__;
				}

				old = nullptr;
			}

			do_notify = true;
		}

		__noinline void unlock()
		{
			NamedThreadBase* owner = GetCurrentNamedThread();

			if (!m_owner.compare_exchange_strong(owner, nullptr))
			{
				throw __FUNCTION__;
			}

			if (do_notify)
			{
				m_cv.notify_one();
			}
		}

	};

	std::function<void()> g_reservation_cb = nullptr;
	NamedThreadBase* g_reservation_owner = nullptr;

	u32 g_reservation_addr = 0;
	u32 g_reservation_size = 0;

	reservation_mutex_t g_reservation_mutex;

	void _reservation_set(u32 addr, bool no_access = false)
	{
		//const auto stamp0 = get_time();

#ifdef _WIN32
		DWORD old;
		if (!VirtualProtect(vm::get_ptr(addr & ~0xfff), 4096, no_access ? PAGE_NOACCESS : PAGE_READONLY, &old))
#else
		if (mprotect(vm::get_ptr(addr & ~0xfff), 4096, no_access ? PROT_NONE : PROT_READ))
#endif
		{
			throw fmt::format("vm::_reservation_set() failed (addr=0x%x)", addr);
		}

		//LOG_NOTICE(MEMORY, "VirtualProtect: %f us", (get_time() - stamp0) / 80.f);
	}

	bool _reservation_break(u32 addr)
	{
		if (g_reservation_addr >> 12 == addr >> 12)
		{
			//const auto stamp0 = get_time();

#ifdef _WIN32
			DWORD old;
			if (!VirtualProtect(vm::get_ptr(addr & ~0xfff), 4096, PAGE_READWRITE, &old))
#else
			if (mprotect(vm::get_ptr(addr & ~0xfff), 4096, PROT_READ | PROT_WRITE))
#endif
			{
				throw fmt::format("vm::_reservation_break() failed (addr=0x%x)", addr);
			}

			//LOG_NOTICE(MEMORY, "VirtualAlloc: %f us", (get_time() - stamp0) / 80.f);

			if (g_reservation_cb)
			{
				g_reservation_cb();
				g_reservation_cb = nullptr;
			}

			g_reservation_owner = nullptr;
			g_reservation_addr = 0;
			g_reservation_size = 0;

			return true;
		}

		return false;
	}

	bool reservation_break(u32 addr)
	{
		std::lock_guard<reservation_mutex_t> lock(g_reservation_mutex);

		return _reservation_break(addr);
	}

	bool reservation_acquire(void* data, u32 addr, u32 size, const std::function<void()>& callback)
	{
		//const auto stamp0 = get_time();

		bool broken = false;

		assert(size == 1 || size == 2 || size == 4 || size == 8 || size == 128);
		assert((addr + size - 1 & ~0xfff) == (addr & ~0xfff));

		{
			std::lock_guard<reservation_mutex_t> lock(g_reservation_mutex);

			// silent unlocking to prevent priority boost for threads going to break reservation
			//g_reservation_mutex.do_notify = false;

			// break previous reservation
			if (g_reservation_owner)
			{
				broken = _reservation_break(g_reservation_addr);
			}

			// change memory protection to read-only
			_reservation_set(addr);

			// may not be necessary
			_mm_mfence();

			// set additional information
			g_reservation_addr = addr;
			g_reservation_size = size;
			g_reservation_owner = GetCurrentNamedThread();
			g_reservation_cb = callback;

			// copy data
			memcpy(data, vm::get_ptr(addr), size);
		}

		return broken;
	}

	bool reservation_update(u32 addr, const void* data, u32 size)
	{
		assert(size == 1 || size == 2 || size == 4 || size == 8 || size == 128);
		assert((addr + size - 1 & ~0xfff) == (addr & ~0xfff));

		std::lock_guard<reservation_mutex_t> lock(g_reservation_mutex);

		if (g_reservation_owner != GetCurrentNamedThread() || g_reservation_addr != addr || g_reservation_size != size)
		{
			// atomic update failed
			return false;
		}

		// change memory protection to no access
		_reservation_set(addr, true);

		// update memory using privileged access
		memcpy(vm::get_priv_ptr(addr), data, size);

		// remove callback to not call it on successful update
		g_reservation_cb = nullptr;

		// free the reservation and restore memory protection
		_reservation_break(addr);

		// atomic update succeeded
		return true;
	}

	bool reservation_query(u32 addr, bool is_writing)
	{
		std::lock_guard<reservation_mutex_t> lock(g_reservation_mutex);

		{
			LV2_LOCK(0);

			if (!Memory.IsGoodAddr(addr))
			{
				return false;
			}
		}

		if (is_writing)
		{
			// break the reservation
			_reservation_break(addr);
		}
		
		return true;
	}

	void reservation_free()
	{
		std::lock_guard<reservation_mutex_t> lock(g_reservation_mutex);

		if (g_reservation_owner == GetCurrentNamedThread())
		{
			_reservation_break(g_reservation_addr);
		}
	}

	void reservation_op(u32 addr, u32 size, std::function<void()> proc)
	{
		assert(size == 1 || size == 2 || size == 4 || size == 8 || size == 128);
		assert((addr + size - 1 & ~0xfff) == (addr & ~0xfff));

		std::lock_guard<reservation_mutex_t> lock(g_reservation_mutex);

		// break previous reservation
		if (g_reservation_owner != GetCurrentNamedThread() || g_reservation_addr != addr || g_reservation_size != size)
		{
			if (g_reservation_owner)
			{
				_reservation_break(g_reservation_addr);
			}
		}

		// change memory protection to no access
		_reservation_set(addr, true);

		// set additional information
		g_reservation_addr = addr;
		g_reservation_size = size;
		g_reservation_owner = GetCurrentNamedThread();
		g_reservation_cb = nullptr;

		// may not be necessary
		_mm_mfence();

		// do the operation
		proc();

		// remove the reservation
		_reservation_break(addr);
	}

	bool check_addr(u32 addr)
	{
		// Checking address before using it is unsafe.
		// The only safe way to check it is to protect both actions (checking and using) with mutex that is used for mapping/allocation.
		return false;
	}

	//TODO
	bool map(u32 addr, u32 size, u32 flags)
	{
		return Memory.Map(addr, size);
	}

	bool unmap(u32 addr, u32 size, u32 flags)
	{
		return Memory.Unmap(addr);
	}

	u32 alloc(u32 addr, u32 size, memory_location location)
	{
		return g_locations[location].fixed_allocator(addr, size);
	}

	u32 alloc(u32 size, memory_location location)
	{
		return g_locations[location].allocator(size);
	}

	void dealloc(u32 addr, memory_location location)
	{
		return g_locations[location].deallocator(addr);
	}

	u32 get_addr(const void* real_pointer)
	{
		const u64 diff = (u64)real_pointer - (u64)g_base_addr;
		const u32 res = (u32)diff;

		if (res == diff)
		{
			return res;
		}

		if (real_pointer)
		{
			throw fmt::format("vm::get_addr(0x%016llx) failed: not a part of virtual memory", (u64)real_pointer);
		}

		return 0;
	}

	void error(const u64 addr, const char* func)
	{
		throw fmt::format("%s(): failed to cast 0x%llx (too big value)", func, addr);
	}

	namespace ps3
	{
		u32 main_alloc(u32 size)
		{
			return Memory.MainMem.AllocAlign(size, 1);
		}
		u32 main_fixed_alloc(u32 addr, u32 size)
		{
			return Memory.MainMem.AllocFixed(addr, size) ? addr : 0;
		}
		void main_dealloc(u32 addr)
		{
			Memory.MainMem.Free(addr);
		}

		u32 g_stack_offset = 0;

		u32 stack_alloc(u32 size)
		{
			return Memory.StackMem.AllocAlign(size, 0x10);
		}
		u32 stack_fixed_alloc(u32 addr, u32 size)
		{
			return Memory.StackMem.AllocFixed(addr, size) ? addr : 0;
		}
		void stack_dealloc(u32 addr)
		{
			Memory.StackMem.Free(addr);
		}

		u32 sprx_alloc(u32 size)
		{
			return Memory.SPRXMem.AllocAlign(size, 1);
		}
		u32 sprx_fixed_alloc(u32 addr, u32 size)
		{
			return Memory.SPRXMem.AllocFixed(Memory.SPRXMem.GetStartAddr() + addr, size) ? Memory.SPRXMem.GetStartAddr() + addr : 0;
		}
		void sprx_dealloc(u32 addr)
		{
			Memory.SPRXMem.Free(addr);
		}

		u32 user_space_alloc(u32 size)
		{
			return Memory.PRXMem.AllocAlign(size, 1);
		}
		u32 user_space_fixed_alloc(u32 addr, u32 size)
		{
			return Memory.PRXMem.AllocFixed(addr, size) ? addr : 0;
		}
		void user_space_dealloc(u32 addr)
		{
			Memory.PRXMem.Free(addr);
		}

		void init()
		{
			Memory.Init(Memory_PS3);
		}
	}

	namespace psv
	{
		void init()
		{
			Memory.Init(Memory_PSV);
		}
	}

	namespace psp
	{
		void init()
		{
			Memory.Init(Memory_PSP);
		}
	}

	location_info g_locations[memory_location_count] =
	{
		{ 0x00010000, 0x2FFF0000, ps3::main_alloc, ps3::main_fixed_alloc, ps3::main_dealloc },
		{ 0xD0000000, 0x10000000, ps3::stack_alloc, ps3::stack_fixed_alloc, ps3::stack_dealloc },

		//remove me
		{ 0x00010000, 0x2FFF0000, ps3::sprx_alloc, ps3::sprx_fixed_alloc, ps3::sprx_dealloc },

		{ 0x30000000, 0x10000000, ps3::user_space_alloc, ps3::user_space_fixed_alloc, ps3::user_space_dealloc },
	};

	void close()
	{
		Memory.Close();
	}

	u32 stack_push(CPUThread& CPU, u32 size, u32 align_v, u32& old_pos)
	{
		assert(align_v);

		switch (CPU.GetType())
		{
		case CPU_THREAD_PPU:
		{
			PPUThread& PPU = static_cast<PPUThread&>(CPU);

			old_pos = (u32)PPU.GPR[1];
			PPU.GPR[1] -= align(size, 8); // room minimal possible size
			PPU.GPR[1] &= ~(align_v - 1); // fix stack alignment

			if (PPU.GPR[1] < CPU.GetStackAddr())
			{
				// stack overflow
				PPU.GPR[1] = old_pos;
				return 0;
			}
			else
			{
				return (u32)PPU.GPR[1];
			}
		}

		case CPU_THREAD_SPU:
		case CPU_THREAD_RAW_SPU:
		{
			assert(!"stack_push(): SPU not supported");
			return 0;
		}

		case CPU_THREAD_ARMv7:
		{
			ARMv7Context& context = static_cast<ARMv7Thread&>(CPU).context;

			old_pos = context.SP;
			context.SP -= align(size, 4); // room minimal possible size
			context.SP &= ~(align_v - 1); // fix stack alignment

			if (context.SP < CPU.GetStackAddr())
			{
				// stack overflow
				context.SP = old_pos;
				return 0;
			}
			else
			{
				return context.SP;
			}
		}

		default:
		{
			assert(!"stack_push(): invalid thread type");
			return 0;
		}
		}
	}

	void stack_pop(CPUThread& CPU, u32 addr, u32 old_pos)
	{
		switch (CPU.GetType())
		{
		case CPU_THREAD_PPU:
		{
			PPUThread& PPU = static_cast<PPUThread&>(CPU);

			assert(PPU.GPR[1] == addr);
			PPU.GPR[1] = old_pos;
			return;
		}

		case CPU_THREAD_SPU:
		case CPU_THREAD_RAW_SPU:
		{
			assert(!"stack_pop(): SPU not supported");
			return;
		}

		case CPU_THREAD_ARMv7:
		{
			ARMv7Context& context = static_cast<ARMv7Thread&>(CPU).context;

			assert(context.SP == addr);
			context.SP = old_pos;
			return;
		}

		default:
		{
			assert(!"stack_pop(): invalid thread type");
			return;
		}
		}
	}
}
