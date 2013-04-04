#include <algorithm>
#include "base/timeutil.h"
#include "GeDisasm.h"
#include "GPUCommon.h"
#include "GPUState.h"
#include "ChunkFile.h"
#include "Core/CoreTiming.h"
#include "Core/MemMap.h"
#include "Core/Host.h"
#include "Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernelInterrupt.h"
#include "Core/HLE/sceGe.h"

static int dlIdGenerator = 1;

void init() {
	dlIdGenerator = 1;
}

u32 GPUCommon::DrawSync(int mode) {
	if (mode < 0 || mode > 1)
		return SCE_KERNEL_ERROR_INVALID_MODE;

	if (mode == 0) {
		// TODO: Wait.
		return 0;
	}

	if (!currentList)
		return PSP_GE_LIST_COMPLETED;

	if (currentList->pc == currentList->stall)
		return PSP_GE_LIST_STALLING;

	return PSP_GE_LIST_DRAWING;
}

int GPUCommon::ListSync(int listid, int mode)
{
	if (mode < 0 || mode > 1)
		return SCE_KERNEL_ERROR_INVALID_MODE;

	if (mode == 1) {
		DisplayList *dl = NULL;
		for (DisplayListQueue::iterator it(dlQueue.begin()); it != dlQueue.end(); ++it) {
			if (it->id == listid) {
				dl = &*it;
			}
		}

		if (!dl)
			return SCE_KERNEL_ERROR_INVALID_ID;

		switch (dl->state) {
		case PSP_GE_DL_STATE_QUEUED:
			// TODO: interrupted -> return PSP_GE_LIST_PAUSED;
			return PSP_GE_LIST_QUEUED;

		case PSP_GE_DL_STATE_RUNNING:
			if (dl->pc == dl->stall)
				return PSP_GE_LIST_STALLING;
			return PSP_GE_LIST_DRAWING;

		case PSP_GE_DL_STATE_COMPLETED:
			return PSP_GE_LIST_COMPLETED;

		case PSP_GE_DL_STATE_PAUSED:
			return PSP_GE_LIST_PAUSED;

		default:
			return SCE_KERNEL_ERROR_INVALID_ID;
		}
	}

	// TODO: Wait here for mode == 0.
	return PSP_GE_LIST_COMPLETED;
}

u32 GPUCommon::EnqueueList(u32 listpc, u32 stall, int subIntrBase, bool head)
{
	DisplayList dl;
	dl.id = dlIdGenerator++;
	dl.startpc = listpc & 0xFFFFFFF;
	dl.pc = listpc & 0xFFFFFFF;
	dl.stall = stall & 0xFFFFFFF;
	dl.state = PSP_GE_DL_STATE_QUEUED;
	dl.subIntrBase = std::max(subIntrBase, -1);
	dl.stackptr = 0;
	dl.signal = PSP_GE_SIGNAL_NONE;
	if(head)
		dlQueue.push_front(dl);
    else
		dlQueue.push_back(dl);
	ProcessDLQueue();
	return dl.id;
}

u32 GPUCommon::DequeueList(int listid)
{
	// TODO
	return 0;
}

u32 GPUCommon::UpdateStall(int listid, u32 newstall)
{
	for (auto iter = dlQueue.begin(); iter != dlQueue.end(); ++iter)
	{
		DisplayList &cur = *iter;
		if (cur.id == listid)
		{
			cur.stall = newstall & 0xFFFFFFF;
		}
	}
	
	ProcessDLQueue();

	return 0;
}

u32 GPUCommon::Continue()
{
	// TODO
	return 0;
}

u32 GPUCommon::Break(int mode)
{
	if (mode < 0 || mode > 1)
		return SCE_KERNEL_ERROR_INVALID_MODE;

	// TODO
	return 0;
}

bool GPUCommon::InterpretList(DisplayList &list)
{
	time_update();
	double start = time_now_d();
	currentList = &list;
	u32 op = 0;
	prev = 0;
	finished = false;

	// I don't know if this is the correct place to zero this, but something
	// need to do it. See Sol Trigger title screen.
	gstate_c.offsetAddr = 0;

	if (!Memory::IsValidAddress(list.pc)) {
		ERROR_LOG(G3D, "DL PC = %08x WTF!!!!", list.pc);
		return true;
	}
#if defined(USING_QT_UI)
		if(host->GpuStep())
		{
			host->SendGPUStart();
		}
#endif

	cycleLastPC = list.pc;
	list.state = PSP_GE_DL_STATE_RUNNING;

	while (!finished)
	{
		if (list.pc == list.stall)
			return false;

		op = Memory::ReadUnchecked_U32(list.pc); //read from memory
		u32 cmd = op >> 24;

#if defined(USING_QT_UI)
		if(host->GpuStep())
		{
			host->SendGPUWait(cmd, list.pc, &gstate);
		}
#endif
		u32 diff = op ^ gstate.cmdmem[cmd];
		PreExecuteOp(op, diff);
		// TODO: Add a compiler flag to remove stuff like this at very-final build time.
		if (dumpThisFrame_) {
			char temp[256];
			GeDisassembleOp(list.pc, op, prev, temp);
			NOTICE_LOG(HLE, "%s", temp);
		}
		gstate.cmdmem[cmd] = op;	 // crashes if I try to put the whole op there??
		
		ExecuteOp(op, diff);
		
		list.pc += 4;
		prev = op;
	}

	UpdateCycles(list.pc);

	time_update();
	gpuStats.msProcessingDisplayLists += time_now_d() - start;
	return true;
}

inline void GPUCommon::UpdateCycles(u32 pc, u32 newPC)
{
	cyclesExecuted += (pc - cycleLastPC) / 4;
	cycleLastPC = newPC == 0 ? pc : newPC;
}

bool GPUCommon::ProcessDLQueue()
{
	startingTicks = CoreTiming::GetTicks();
	cyclesExecuted = 0;

	DisplayListQueue::iterator iter = dlQueue.begin();
	while (iter != dlQueue.end())
	{
		DisplayList &l = *iter;
		DEBUG_LOG(G3D,"Okay, starting DL execution at %08x - stall = %08x", l.pc, l.stall);
		if (!InterpretList(l))
		{
			return false;
		}
		else
		{
			//At the end, we can remove it from the queue and continue
			dlQueue.erase(iter);
			//this invalidated the iterator, let's fix it
			iter = dlQueue.begin();
		}
	}
	currentList = NULL;
	return true; //no more lists!
}

void GPUCommon::PreExecuteOp(u32 op, u32 diff) {
	// Nothing to do
}

void GPUCommon::ExecuteOp(u32 op, u32 diff) {
	u32 cmd = op >> 24;
	u32 data = op & 0xFFFFFF;

	// Handle control and drawing commands here directly. The others we delegate.
	switch (cmd) {
	case GE_CMD_NOP:
		break;

	case GE_CMD_OFFSETADDR:
		gstate_c.offsetAddr = data << 8;
		// ???
		break;

	case GE_CMD_ORIGIN:
		gstate_c.offsetAddr = currentList->pc;
		break;

	case GE_CMD_JUMP:
		{
			u32 target = gstate_c.getRelativeAddress(data);
			if (Memory::IsValidAddress(target)) {
				UpdateCycles(currentList->pc, target - 4);
				currentList->pc = target - 4; // pc will be increased after we return, counteract that
			} else {
				ERROR_LOG(G3D, "JUMP to illegal address %08x - ignoring! data=%06x", target, data);
			}
		}
		break;

	case GE_CMD_CALL:
		{
			// Saint Seiya needs correct support for relative calls.
			u32 retval = currentList->pc + 4;
			u32 target = gstate_c.getRelativeAddress(data);
			if (currentList->stackptr == ARRAY_SIZE(currentList->stack)) {
				ERROR_LOG(G3D, "CALL: Stack full!");
			} else if (!Memory::IsValidAddress(target)) {
				ERROR_LOG(G3D, "CALL to illegal address %08x - ignoring! data=%06x", target, data);
			} else {
				currentList->stack[currentList->stackptr++] = retval;
				UpdateCycles(currentList->pc, target - 4);
				currentList->pc = target - 4;	// pc will be increased after we return, counteract that
			}
		}
		break;

	case GE_CMD_RET:
		{
			if (currentList->stackptr == 0) {
				ERROR_LOG(G3D, "RET: Stack empty!");
			} else {
				u32 target = (currentList->pc & 0xF0000000) | (currentList->stack[--currentList->stackptr] & 0x0FFFFFFF);
				//target = (target + gstate_c.originAddr) & 0xFFFFFFF;
				UpdateCycles(currentList->pc, target - 4);
				currentList->pc = target - 4;
				if (!Memory::IsValidAddress(currentList->pc)) {
					ERROR_LOG(G3D, "Invalid DL PC %08x on return", currentList->pc);
					finished = true;
				}
			}
		}
		break;

	case GE_CMD_SIGNAL:
	case GE_CMD_FINISH:
		// Processed in GE_END.
		break;

	case GE_CMD_END:
		UpdateCycles(currentList->pc);
		switch (prev >> 24) {
		case GE_CMD_SIGNAL:
			{
				// TODO: see http://code.google.com/p/jpcsp/source/detail?r=2935#
				int behaviour = (prev >> 16) & 0xFF;
				int signal = prev & 0xFFFF;
				int enddata = data & 0xFFFF;
				currentList->subIntrToken = signal;

				switch (behaviour) {
				case PSP_GE_SIGNAL_HANDLER_SUSPEND:
					ERROR_LOG(G3D, "Signal with Wait UNIMPLEMENTED! signal/end: %04x %04x", signal, enddata);
					break;
				case PSP_GE_SIGNAL_HANDLER_CONTINUE:
					ERROR_LOG(G3D, "Signal without wait. signal/end: %04x %04x", signal, enddata);
					break;
				case PSP_GE_SIGNAL_HANDLER_PAUSE:
					ERROR_LOG(G3D, "Signal with Pause UNIMPLEMENTED! signal/end: %04x %04x", signal, enddata);
					break;
				case PSP_GE_SIGNAL_SYNC:
					ERROR_LOG(G3D, "Signal with Sync UNIMPLEMENTED! signal/end: %04x %04x", signal, enddata);
					break;
				case PSP_GE_SIGNAL_JUMP:
					ERROR_LOG(G3D, "Signal with Jump UNIMPLEMENTED! signal/end: %04x %04x", signal, enddata);
					break;
				case PSP_GE_SIGNAL_CALL:
					ERROR_LOG(G3D, "Signal with Call UNIMPLEMENTED! signal/end: %04x %04x", signal, enddata);
					break;
				case PSP_GE_SIGNAL_RET:
					ERROR_LOG(G3D, "Signal with Return UNIMPLEMENTED! signal/end: %04x %04x", signal, enddata);
					break;
				default:
					ERROR_LOG(G3D, "UNKNOWN Signal UNIMPLEMENTED %i ! signal/end: %04x %04x", behaviour, signal, enddata);
					break;
				}
				if (currentList->subIntrBase >= 0 && interruptsEnabled_)
					__GeTriggerInterrupt(currentList->id, currentList->pc, currentList->subIntrBase, currentList->subIntrToken);
			}
			break;
		case GE_CMD_FINISH:
			currentList->state = PSP_GE_DL_STATE_COMPLETED;
			finished = true;
			currentList->subIntrToken = prev & 0xFFFF;
			if (interruptsEnabled_)
				__GeTriggerInterrupt(currentList->id, currentList->pc, currentList->subIntrBase, currentList->subIntrToken);
			break;
		default:
			DEBUG_LOG(G3D,"Ah, not finished: %06x", prev & 0xFFFFFF);
			break;
		}
		break;

	default:
		DEBUG_LOG(G3D,"DL Unknown: %08x @ %08x", op, currentList == NULL ? 0 : currentList->pc);
		break;
	}
}

void GPUCommon::DoState(PointerWrap &p) {
	p.Do(dlIdGenerator);
	p.Do<DisplayList>(dlQueue);
	int currentID = currentList == NULL ? 0 : currentList->id;
	p.Do(currentID);
	if (currentID == 0) {
		currentList = 0;
	} else {
		for (auto it = dlQueue.begin(), end = dlQueue.end(); it != end; ++it) {
			if (it->id == currentID) {
				currentList = &*it;
				break;
			}
		}
	}
	p.Do(interruptRunning);
	p.Do(prev);
	p.Do(finished);
	p.DoMarker("GPUCommon");
}

void GPUCommon::InterruptStart()
{
	interruptRunning = true;
}
void GPUCommon::InterruptEnd()
{
	interruptRunning = false;
	ProcessDLQueue();
}
