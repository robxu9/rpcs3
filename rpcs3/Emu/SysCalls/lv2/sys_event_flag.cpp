#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/SysCalls/SysCalls.h"
#include "Emu/Memory/atomic_type.h"

#include "Emu/CPU/CPUThreadManager.h"
#include "Emu/Cell/PPUThread.h"
#include "sleep_queue_type.h"
#include "sys_event_flag.h"

SysCallBase sys_event_flag("sys_event_flag");

u32 EventFlag::check()
{
	u32 target = 0;
	u64 highest_prio = ~0ull;
	const u64 flag_set = flags.read_sync();

	for (u32 i = 0; i < waiters.size(); i++)
	{
		if (((waiters[i].mode & SYS_EVENT_FLAG_WAIT_AND) && (flag_set & waiters[i].bitptn) == waiters[i].bitptn) ||
			((waiters[i].mode & SYS_EVENT_FLAG_WAIT_OR) && (flag_set & waiters[i].bitptn)))
		{
			if (protocol == SYS_SYNC_FIFO)
			{
				target = waiters[i].tid;
				break;
			}
			else if (protocol == SYS_SYNC_PRIORITY)
			{
				if (std::shared_ptr<CPUThread> t = Emu.GetCPU().GetThread(waiters[i].tid))
				{
					const u64 prio = t->GetPrio();
					if (prio < highest_prio)
					{
						highest_prio = prio;
						target = waiters[i].tid;
					}
				}
				else
				{
					assert(!"EventFlag::check(): waiter not found");
				}
			}
			else
			{
				assert(!"EventFlag::check(): unknown protocol");
			}
		}
	}

	return target;
}

s32 sys_event_flag_create(vm::ptr<u32> eflag_id, vm::ptr<sys_event_flag_attr> attr, u64 init)
{
	sys_event_flag.Warning("sys_event_flag_create(eflag_id_addr=0x%x, attr_addr=0x%x, init=0x%llx)", eflag_id.addr(), attr.addr(), init);

	if (!eflag_id)
	{
		sys_event_flag.Error("sys_event_flag_create(): invalid memory access (eflag_id_addr=0x%x)", eflag_id.addr());
		return CELL_EFAULT;
	}

	if (!attr)
	{
		sys_event_flag.Error("sys_event_flag_create(): invalid memory access (attr_addr=0x%x)", attr.addr());
		return CELL_EFAULT;
	}

	switch (attr->protocol.data())
	{
	case se32(SYS_SYNC_PRIORITY): break;
	case se32(SYS_SYNC_RETRY): sys_event_flag.Todo("SYS_SYNC_RETRY"); break;
	case se32(SYS_SYNC_PRIORITY_INHERIT): sys_event_flag.Todo("SYS_SYNC_PRIORITY_INHERIT"); break;
	case se32(SYS_SYNC_FIFO): break;
	default: sys_event_flag.Error("Unknown protocol (0x%x)", attr->protocol); return CELL_EINVAL;
	}

	if (attr->pshared.data() != se32(0x200))
	{
		sys_event_flag.Error("Unknown pshared attribute (0x%x)", attr->pshared);
		return CELL_EINVAL;
	}

	switch (attr->type.data())
	{
	case se32(SYS_SYNC_WAITER_SINGLE): break;
	case se32(SYS_SYNC_WAITER_MULTIPLE): break;
	default: sys_event_flag.Error("Unknown event flag type (0x%x)", attr->type); return CELL_EINVAL;
	}

	std::shared_ptr<EventFlag> ef(new EventFlag(init, attr->protocol, attr->type, attr->name_u64));
	u32 id = sys_event_flag.GetNewId(ef, TYPE_EVENT_FLAG);
	*eflag_id = id;
	sys_event_flag.Warning("*** event_flag created [%s] (protocol=0x%x, type=0x%x): id = %d", std::string(attr->name, 8).c_str(), attr->protocol, attr->type, id);

	return CELL_OK;
}

s32 sys_event_flag_destroy(u32 eflag_id)
{
	sys_event_flag.Warning("sys_event_flag_destroy(eflag_id=%d)", eflag_id);

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;

	if (ef->waiters.size()) // ???
	{
		return CELL_EBUSY;
	}

	Emu.GetIdManager().RemoveID(eflag_id);

	return CELL_OK;
}

s32 sys_event_flag_wait(u32 eflag_id, u64 bitptn, u32 mode, vm::ptr<u64> result, u64 timeout)
{
	sys_event_flag.Log("sys_event_flag_wait(eflag_id=%d, bitptn=0x%llx, mode=0x%x, result_addr=0x%x, timeout=%lld)",
		eflag_id, bitptn, mode, result.addr(), timeout);

	if (result) *result = 0;

	switch (mode & 0xf)
	{
	case SYS_EVENT_FLAG_WAIT_AND: break;
	case SYS_EVENT_FLAG_WAIT_OR: break;
	default: return CELL_EINVAL;
	}

	switch (mode & ~0xf)
	{
	case 0: break; // ???
	case SYS_EVENT_FLAG_WAIT_CLEAR: break;
	case SYS_EVENT_FLAG_WAIT_CLEAR_ALL: break;
	default: return CELL_EINVAL;
	}

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;

	const u32 tid = GetCurrentPPUThread().GetId();

	{
		std::lock_guard<std::mutex> lock(ef->mutex);

		if (ef->type == SYS_SYNC_WAITER_SINGLE && ef->waiters.size() > 0)
		{
			return CELL_EPERM;
		}

		EventFlagWaiter rec;
		rec.bitptn = bitptn;
		rec.mode = mode;
		rec.tid = tid;
		ef->waiters.push_back(rec);

		if (ef->check() == tid)
		{
			const u64 flag_set = ef->flags.read_sync();

			ef->waiters.erase(ef->waiters.end() - 1);

			if (mode & SYS_EVENT_FLAG_WAIT_CLEAR)
			{
				ef->flags &= ~bitptn;
			}
			else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL)
			{
				ef->flags &= 0;
			}

			if (result)
			{
				*result = flag_set;
			}
			return CELL_OK;
		}
	}

	u64 counter = 0;
	const u64 max_counter = timeout ? (timeout / 1000) : ~0;

	while (true)
	{
		u32 signaled;
		if (ef->signal.try_peek(signaled) && signaled == tid)
		{
			std::lock_guard<std::mutex> lock(ef->mutex);

			const u64 flag_set = ef->flags.read_sync();

			ef->signal.pop(signaled);

			for (u32 i = 0; i < ef->waiters.size(); i++)
			{
				if (ef->waiters[i].tid == tid)
				{
					ef->waiters.erase(ef->waiters.begin() + i);

					if (mode & SYS_EVENT_FLAG_WAIT_CLEAR)
					{
						ef->flags &= ~bitptn;
					}
					else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL)
					{
						ef->flags &= 0;
					}

					if (u32 target = ef->check())
					{
						ef->signal.push(target);
					}

					if (result)
					{
						*result = flag_set;
					}
					return CELL_OK;
				}
			}

			return CELL_ECANCELED;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hack

		if (counter++ > max_counter)
		{
			std::lock_guard<std::mutex> lock(ef->mutex);

			for (u32 i = 0; i < ef->waiters.size(); i++)
			{
				if (ef->waiters[i].tid == tid)
				{
					ef->waiters.erase(ef->waiters.begin() + i);
					break;
				}
			}
			return CELL_ETIMEDOUT;
		}

		if (Emu.IsStopped())
		{
			sys_event_flag.Warning("sys_event_flag_wait(id=%d) aborted", eflag_id);
			return CELL_OK;
		}
	}
}

s32 sys_event_flag_trywait(u32 eflag_id, u64 bitptn, u32 mode, vm::ptr<u64> result)
{
	sys_event_flag.Log("sys_event_flag_trywait(eflag_id=%d, bitptn=0x%llx, mode=0x%x, result_addr=0x%x)",
		eflag_id, bitptn, mode, result.addr());

	if (result) *result = 0;

	switch (mode & 0xf)
	{
	case SYS_EVENT_FLAG_WAIT_AND: break;
	case SYS_EVENT_FLAG_WAIT_OR: break;
	default: return CELL_EINVAL;
	}

	switch (mode & ~0xf)
	{
	case 0: break; // ???
	case SYS_EVENT_FLAG_WAIT_CLEAR: break;
	case SYS_EVENT_FLAG_WAIT_CLEAR_ALL: break;
	default: return CELL_EINVAL;
	}

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;
	
	std::lock_guard<std::mutex> lock(ef->mutex);

	const u64 flag_set = ef->flags.read_sync();

	if (((mode & SYS_EVENT_FLAG_WAIT_AND) && (flag_set & bitptn) == bitptn) ||
		((mode & SYS_EVENT_FLAG_WAIT_OR) && (flag_set & bitptn)))
	{
		if (mode & SYS_EVENT_FLAG_WAIT_CLEAR)
		{
			ef->flags &= ~bitptn;
		}
		else if (mode & SYS_EVENT_FLAG_WAIT_CLEAR_ALL)
		{
			ef->flags &= 0;
		}

		if (result)
		{
			*result = flag_set;
		}

		return CELL_OK;
	}

	return CELL_EBUSY;
}

s32 sys_event_flag_set(u32 eflag_id, u64 bitptn)
{
	sys_event_flag.Log("sys_event_flag_set(eflag_id=%d, bitptn=0x%llx)", eflag_id, bitptn);

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;

	std::lock_guard<std::mutex> lock(ef->mutex);

	ef->flags |= bitptn;
	if (u32 target = ef->check())
	{
		ef->signal.push(target);
	}
	return CELL_OK;
}

s32 sys_event_flag_clear(u32 eflag_id, u64 bitptn)
{
	sys_event_flag.Log("sys_event_flag_clear(eflag_id=%d, bitptn=0x%llx)", eflag_id, bitptn);

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;

	std::lock_guard<std::mutex> lock(ef->mutex);
	ef->flags &= bitptn;
	return CELL_OK;
}

s32 sys_event_flag_cancel(u32 eflag_id, vm::ptr<u32> num)
{
	sys_event_flag.Log("sys_event_flag_cancel(eflag_id=%d, num_addr=0x%x)", eflag_id, num.addr());

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;

	std::vector<u32> tids;
	{
		std::lock_guard<std::mutex> lock(ef->mutex);

		tids.resize(ef->waiters.size());
		for (u32 i = 0; i < ef->waiters.size(); i++)
		{
			tids[i] = ef->waiters[i].tid;
		}
		ef->waiters.clear();
	}

	for (auto& v : tids)
	{
		ef->signal.push(v);
	}

	if (Emu.IsStopped())
	{
		sys_event_flag.Warning("sys_event_flag_cancel(id=%d) aborted", eflag_id);
		return CELL_OK;
	}

	if (num)
	{
		*num = (u32)tids.size();
	}

	return CELL_OK;
}

s32 sys_event_flag_get(u32 eflag_id, vm::ptr<u64> flags)
{
	sys_event_flag.Log("sys_event_flag_get(eflag_id=%d, flags_addr=0x%x)", eflag_id, flags.addr());

	if (!flags)
	{
		sys_event_flag.Error("sys_event_flag_create(): invalid memory access (flags_addr=0x%x)", flags.addr());
		return CELL_EFAULT;
	}

	std::shared_ptr<EventFlag> ef;
	if (!sys_event_flag.CheckId(eflag_id, ef)) return CELL_ESRCH;

	*flags = ef->flags.read_sync();
	return CELL_OK;
}