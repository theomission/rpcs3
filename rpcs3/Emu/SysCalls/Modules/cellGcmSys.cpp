#include "stdafx.h"
#include "Emu/Memory/Memory.h"
#include "Emu/System.h"
#include "Emu/SysCalls/Modules.h"

#include "sysPrxForUser.h"

//#include "Emu/RSX/GCM.h"
#include "Emu/RSX/GSManager.h"
#include "Emu/RSX/GSRender.h"
//#include "Emu/SysCalls/lv2/sys_process.h"
#include "cellGcmSys.h"

extern Module<> cellGcmSys;

const u32 tiled_pitches[] = {
	0x00000000, 0x00000200, 0x00000300, 0x00000400,
	0x00000500, 0x00000600, 0x00000700, 0x00000800,
	0x00000A00, 0x00000C00, 0x00000D00, 0x00000E00,
	0x00001000, 0x00001400, 0x00001800, 0x00001A00,
	0x00001C00, 0x00002000, 0x00002800, 0x00003000,
	0x00003400, 0x00003800, 0x00004000, 0x00005000,
	0x00006000, 0x00006800, 0x00007000, 0x00008000,
	0x0000A000, 0x0000C000, 0x0000D000, 0x0000E000,
	0x00010000
};

u32 local_size = 0;
u32 local_addr = 0;
u64 system_mode = 0;

CellGcmConfig current_config;
CellGcmContextData current_context;
gcmInfo gcm_info;

u32 map_offset_addr = 0;
u32 map_offset_pos = 0;

// Auxiliary functions

/*
 * Get usable local memory size for a specific game SDK version
 * Example: For 0x00446000 (FW 4.46) we get a localSize of 0x0F900000 (249MB)
 */ 
u32 gcmGetLocalMemorySize(u32 sdk_version)
{
	if (sdk_version >= 0x00220000)
	{
		return 0x0F900000; // 249MB
	}
	if (sdk_version >= 0x00200000)
	{
		return 0x0F200000; // 242MB
	}
	if (sdk_version >= 0x00190000)
	{
		return 0x0EA00000; // 234MB
	}
	if (sdk_version >= 0x00180000)
	{
		return 0x0E800000; // 232MB
	}
	return 0x0E000000; // 224MB
}

CellGcmOffsetTable offsetTable;

void InitOffsetTable()
{
	offsetTable.ioAddress.set(vm::alloc(3072 * sizeof(u16), vm::main));
	offsetTable.eaAddress.set(vm::alloc(512 * sizeof(u16), vm::main));

	memset(offsetTable.ioAddress.get_ptr(), 0xFF, 3072 * sizeof(u16));
	memset(offsetTable.eaAddress.get_ptr(), 0xFF, 512 * sizeof(u16));
}

//----------------------------------------------------------------------------
// Data Retrieval
//----------------------------------------------------------------------------

u32 cellGcmGetLabelAddress(u8 index)
{
	cellGcmSys.Log("cellGcmGetLabelAddress(index=%d)", index);
	return gcm_info.label_addr + 0x10 * index;
}

vm::ptr<CellGcmReportData> cellGcmGetReportDataAddressLocation(u32 index, u32 location)
{
	cellGcmSys.Warning("cellGcmGetReportDataAddressLocation(index=%d, location=%d)", index, location);

	if (location == CELL_GCM_LOCATION_LOCAL) {
		if (index >= 2048) {
			cellGcmSys.Error("cellGcmGetReportDataAddressLocation: Wrong local index (%d)", index);
			return vm::null;
		}
		return vm::ptr<CellGcmReportData>::make(0xC0000000 + index * 0x10);
	}

	if (location == CELL_GCM_LOCATION_MAIN) {
		if (index >= 1024 * 1024) {
			cellGcmSys.Error("cellGcmGetReportDataAddressLocation: Wrong main index (%d)", index);
			return vm::null;
		}
		// TODO: It seems m_report_main_addr is not initialized
		return vm::ptr<CellGcmReportData>::make(Emu.GetGSManager().GetRender().report_main_addr + index * 0x10);
	}

	cellGcmSys.Error("cellGcmGetReportDataAddressLocation: Wrong location (%d)", location);
	return vm::null;
}

u64 cellGcmGetTimeStamp(u32 index)
{
	cellGcmSys.Log("cellGcmGetTimeStamp(index=%d)", index);

	if (index >= 2048) {
		cellGcmSys.Error("cellGcmGetTimeStamp: Wrong local index (%d)", index);
		return 0;
	}
	return vm::read64(0xC0000000 + index * 0x10);
}

s32 cellGcmGetCurrentField()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

u32 cellGcmGetNotifyDataAddress(u32 index)
{
	cellGcmSys.Warning("cellGcmGetNotifyDataAddress(index=%d)", index);

	// If entry not in use, return NULL
	u16 entry = offsetTable.eaAddress[241];
	if (entry == 0xFFFF) {
		return 0;
	}

	return (entry << 20) + (index * 0x20);
}

/*
 *  Get base address of local report data area
 */
vm::ptr<CellGcmReportData> _cellGcmFunc12()
{
	return vm::ptr<CellGcmReportData>::make(0xC0000000); // TODO
}

u32 cellGcmGetReport(u32 type, u32 index)
{
	cellGcmSys.Warning("cellGcmGetReport(type=%d, index=%d)", type, index);

	if (index >= 2048) {
		cellGcmSys.Error("cellGcmGetReport: Wrong local index (%d)", index);
		return -1;
	}

	if (type < 1 || type > 5) {
		return -1;
	}

	vm::ptr<CellGcmReportData> local_reports = _cellGcmFunc12();
	return local_reports[index].value;
}

u32 cellGcmGetReportDataAddress(u32 index)
{
	cellGcmSys.Warning("cellGcmGetReportDataAddress(index=%d)",  index);

	if (index >= 2048) {
		cellGcmSys.Error("cellGcmGetReportDataAddress: Wrong local index (%d)", index);
		return 0;
	}
	return 0xC0000000 + index * 0x10;
}

u32 cellGcmGetReportDataLocation(u32 index, u32 location)
{
	cellGcmSys.Warning("cellGcmGetReportDataLocation(index=%d, location=%d)", index, location);

	vm::ptr<CellGcmReportData> report = cellGcmGetReportDataAddressLocation(index, location);
	return report->value;
}

u64 cellGcmGetTimeStampLocation(u32 index, u32 location)
{
	cellGcmSys.Warning("cellGcmGetTimeStampLocation(index=%d, location=%d)", index, location);

	if (location == CELL_GCM_LOCATION_LOCAL) {
		if (index >= 2048) {
			cellGcmSys.Error("cellGcmGetTimeStampLocation: Wrong local index (%d)", index);
			return 0;
		}
		return vm::read64(0xC0000000 + index * 0x10);
	}

	if (location == CELL_GCM_LOCATION_MAIN) {
		if (index >= 1024 * 1024) {
			cellGcmSys.Error("cellGcmGetTimeStampLocation: Wrong main index (%d)", index);
			return 0;
		}
		// TODO: It seems m_report_main_addr is not initialized
		return vm::read64(Emu.GetGSManager().GetRender().report_main_addr + index * 0x10);
	}

	cellGcmSys.Error("cellGcmGetTimeStampLocation: Wrong location (%d)", location);
	return 0;
}

//----------------------------------------------------------------------------
// Command Buffer Control
//----------------------------------------------------------------------------

u32 cellGcmGetControlRegister()
{
	cellGcmSys.Log("cellGcmGetControlRegister()");

	return gcm_info.control_addr;
}

u32 cellGcmGetDefaultCommandWordSize()
{
	cellGcmSys.Log("cellGcmGetDefaultCommandWordSize()");
	return 0x400;
}

u32 cellGcmGetDefaultSegmentWordSize()
{
	cellGcmSys.Log("cellGcmGetDefaultSegmentWordSize()");
	return 0x100;
}

s32 cellGcmInitDefaultFifoMode(s32 mode)
{
	cellGcmSys.Warning("cellGcmInitDefaultFifoMode(mode=%d)", mode);
	return CELL_OK;
}

s32 cellGcmSetDefaultFifoSize(u32 bufferSize, u32 segmentSize)
{
	cellGcmSys.Warning("cellGcmSetDefaultFifoSize(bufferSize=0x%x, segmentSize=0x%x)", bufferSize, segmentSize);
	return CELL_OK;
}

//----------------------------------------------------------------------------
// Hardware Resource Management
//----------------------------------------------------------------------------

s32 cellGcmBindTile(u8 index)
{
	cellGcmSys.Warning("cellGcmBindTile(index=%d)", index);

	if (index >= rsx::limits::tiles_count)
	{
		cellGcmSys.Error("cellGcmBindTile: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	auto& tile = Emu.GetGSManager().GetRender().tiles[index];
	tile.binded = true;

	return CELL_OK;
}

s32 cellGcmBindZcull(u8 index)
{
	cellGcmSys.Warning("cellGcmBindZcull(index=%d)", index);

	if (index >= rsx::limits::zculls_count)
	{
		cellGcmSys.Error("cellGcmBindZcull: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	auto& zcull = Emu.GetGSManager().GetRender().zculls[index];
	zcull.binded = true;

	return CELL_OK;
}

s32 cellGcmGetConfiguration(vm::ptr<CellGcmConfig> config)
{
	cellGcmSys.Log("cellGcmGetConfiguration(config=*0x%x)", config);

	*config = current_config;

	return CELL_OK;
}

s32 cellGcmGetFlipStatus()
{
	s32 status = Emu.GetGSManager().GetRender().flip_status;

	cellGcmSys.Log("cellGcmGetFlipStatus() -> %d", status);

	return status;
}

u32 cellGcmGetTiledPitchSize(u32 size)
{
	cellGcmSys.Log("cellGcmGetTiledPitchSize(size=%d)", size);

	for (size_t i=0; i < sizeof(tiled_pitches) / sizeof(tiled_pitches[0]) - 1; i++) {
		if (tiled_pitches[i] < size && size <= tiled_pitches[i+1]) {
			return tiled_pitches[i+1];
		}
	}
	return 0;
}

void _cellGcmFunc1()
{
	cellGcmSys.Todo("_cellGcmFunc1()");
	return;
}

void _cellGcmFunc15(vm::ptr<CellGcmContextData> context)
{
	cellGcmSys.Todo("_cellGcmFunc15(context=*0x%x)", context);
	return;
}

u32 g_defaultCommandBufferBegin, g_defaultCommandBufferFragmentCount;

// Called by cellGcmInit
s32 _cellGcmInitBody(vm::pptr<CellGcmContextData> context, u32 cmdSize, u32 ioSize, u32 ioAddress)
{
	cellGcmSys.Warning("_cellGcmInitBody(context=**0x%x, cmdSize=0x%x, ioSize=0x%x, ioAddress=0x%x)", context, cmdSize, ioSize, ioAddress);

	if (!local_size && !local_addr)
	{
		local_size = 0xf900000; // TODO: Get sdk_version in _cellGcmFunc15 and pass it to gcmGetLocalMemorySize
		local_addr = 0xC0000000;
		vm::falloc(0xC0000000, local_size, vm::video);
	}

	cellGcmSys.Warning("*** local memory(addr=0x%x, size=0x%x)", local_addr, local_size);

	InitOffsetTable();
	if (system_mode == CELL_GCM_SYSTEM_MODE_IOMAP_512MB)
	{
		cellGcmSys.Warning("cellGcmInit(): 512MB io address space used");
		RSXIOMem.SetRange(0, 0x20000000 /*512MB*/);
	}
	else
	{
		cellGcmSys.Warning("cellGcmInit(): 256MB io address space used");
		RSXIOMem.SetRange(0, 0x10000000 /*256MB*/);
	}

	if (gcmMapEaIoAddress(ioAddress, 0, ioSize, false) != CELL_OK)
	{
		cellGcmSys.Error("cellGcmInit: CELL_GCM_ERROR_FAILURE");
		return CELL_GCM_ERROR_FAILURE;
	}

	map_offset_addr = 0;
	map_offset_pos = 0;
	current_config.ioSize = ioSize;
	current_config.ioAddress = ioAddress;
	current_config.localSize = local_size;
	current_config.localAddress = local_addr;
	current_config.memoryFrequency = 650000000;
	current_config.coreFrequency = 500000000;

	// Create contexts

	g_defaultCommandBufferBegin = ioAddress;
	g_defaultCommandBufferFragmentCount = cmdSize / (32 * 1024);

	current_context.begin.set(g_defaultCommandBufferBegin + 4096); // 4 kb reserved at the beginning
	current_context.end.set(g_defaultCommandBufferBegin + 32 * 1024 - 4); // 4b at the end for jump
	current_context.current = current_context.begin;
	current_context.callback.set(Emu.GetRSXCallback() - 4);

	gcm_info.context_addr = vm::alloc(0x1000, vm::main);
	gcm_info.control_addr = gcm_info.context_addr + 0x40;

	gcm_info.label_addr = vm::alloc(0x1000, vm::main); // ???

	vm::get_ref<CellGcmContextData>(gcm_info.context_addr) = current_context;
	context->set(gcm_info.context_addr);

	auto& ctrl = vm::get_ref<CellGcmControl>(gcm_info.control_addr);
	ctrl.put = 0;
	ctrl.get = 0;
	ctrl.ref = -1;

	auto& render = Emu.GetGSManager().GetRender();
	render.ctxt_addr = context.addr();
	render.gcm_buffers = { vm::alloc(sizeof(CellGcmDisplayInfo) * 8, vm::main) };
	render.zculls_addr = vm::alloc(sizeof(CellGcmZcullInfo) * 8, vm::main);
	render.tiles_addr = vm::alloc(sizeof(CellGcmTileInfo) * 15, vm::main);
	render.gcm_buffers_count = 0;
	render.gcm_current_buffer = 0;
	render.main_mem_addr = 0;
	render.label_addr = gcm_info.label_addr;
	render.init(g_defaultCommandBufferBegin, cmdSize, gcm_info.control_addr, local_addr);

	return CELL_OK;
}

s32 cellGcmResetFlipStatus()
{
	cellGcmSys.Log("cellGcmResetFlipStatus()");

	Emu.GetGSManager().GetRender().flip_status = CELL_GCM_DISPLAY_FLIP_STATUS_WAITING;

	return CELL_OK;
}

s32 cellGcmSetDebugOutputLevel(s32 level)
{
	cellGcmSys.Warning("cellGcmSetDebugOutputLevel(level=%d)", level);

	switch (level)
	{
	case CELL_GCM_DEBUG_LEVEL0:
	case CELL_GCM_DEBUG_LEVEL1:
	case CELL_GCM_DEBUG_LEVEL2:
		Emu.GetGSManager().GetRender().debug_level = level;
		break;

	default: return CELL_EINVAL;
	}

	return CELL_OK;
}

s32 cellGcmSetDisplayBuffer(u32 id, u32 offset, u32 pitch, u32 width, u32 height)
{
	cellGcmSys.Log("cellGcmSetDisplayBuffer(id=0x%x,offset=0x%x,pitch=%d,width=%d,height=%d)", id, offset, width ? pitch / width : pitch, width, height);

	if (id > 7)
	{
		cellGcmSys.Error("cellGcmSetDisplayBuffer: CELL_EINVAL");
		return CELL_EINVAL;
	}

	auto buffers = Emu.GetGSManager().GetRender().gcm_buffers;

	buffers[id].offset = offset;
	buffers[id].pitch = pitch;
	buffers[id].width = width;
	buffers[id].height = height;

	if (id + 1 > Emu.GetGSManager().GetRender().gcm_buffers_count)
	{
		Emu.GetGSManager().GetRender().gcm_buffers_count = id + 1;
	}

	return CELL_OK;
}

void cellGcmSetFlipHandler(vm::ptr<void(u32)> handler)
{
	cellGcmSys.Warning("cellGcmSetFlipHandler(handler=*0x%x)", handler);

	Emu.GetGSManager().GetRender().flip_handler = handler;
}

s32 cellGcmSetFlipMode(u32 mode)
{
	cellGcmSys.Warning("cellGcmSetFlipMode(mode=%d)", mode);

	switch (mode)
	{
	case CELL_GCM_DISPLAY_HSYNC:
	case CELL_GCM_DISPLAY_VSYNC:
	case CELL_GCM_DISPLAY_HSYNC_WITH_NOISE:
		Emu.GetGSManager().GetRender().flip_mode = mode;
		break;

	default:
		return CELL_EINVAL;
	}

	return CELL_OK;
}

void cellGcmSetFlipStatus()
{
	cellGcmSys.Warning("cellGcmSetFlipStatus()");

	Emu.GetGSManager().GetRender().flip_status = 0;
}

s32 cellGcmSetPrepareFlip(PPUThread& ppu, vm::ptr<CellGcmContextData> ctxt, u32 id)
{
	cellGcmSys.Log("cellGcmSetPrepareFlip(ctx=*0x%x, id=0x%x)", ctxt, id);

	if (id > 7)
	{
		cellGcmSys.Error("cellGcmSetPrepareFlip: CELL_GCM_ERROR_FAILURE");
		return CELL_GCM_ERROR_FAILURE;
	}

	if (ctxt->current + 2 >= ctxt->end)
	{
		if (s32 res = ctxt->callback(ppu, ctxt, 8 /* ??? */))
		{
			cellGcmSys.Error("cellGcmSetPrepareFlip: callback failed (0x%08x)", res);
			return res;
		}
	}
#ifdef __GNUC__
	//gcc internal compiler error, try to avoid it for now
	*ctxt->current++ = (GCM_FLIP_COMMAND << 2) | (1 << 18);
	*ctxt->current++ = id;

	if (ctxt.addr() == gcm_info.context_addr)
	{
		vm::get_ref<CellGcmControl>(gcm_info.control_addr).put += 2 * sizeof(u32);
	}
#else
	u32 command_size = rsx::make_command(ctxt->current, GCM_FLIP_COMMAND, id);

	if (ctxt.addr() == gcm_info.context_addr)
	{
		vm::get_ref<CellGcmControl>(gcm_info.control_addr).put += command_size * sizeof(u32);
	}
#endif

	return id;
}

s32 cellGcmSetFlip(PPUThread& ppu, vm::ptr<CellGcmContextData> ctxt, u32 id)
{
	cellGcmSys.Log("cellGcmSetFlip(ctxt=*0x%x, id=0x%x)", ctxt, id);

	if (s32 res = cellGcmSetPrepareFlip(ppu, ctxt, id))
	{
		if (res < 0) return CELL_GCM_ERROR_FAILURE;
	}

	return CELL_OK;
}

s32 cellGcmSetSecondVFrequency(u32 freq)
{
	cellGcmSys.Warning("cellGcmSetSecondVFrequency(level=%d)", freq);

	switch (freq)
	{
	case CELL_GCM_DISPLAY_FREQUENCY_59_94HZ:
		Emu.GetGSManager().GetRender().frequency_mode = freq; Emu.GetGSManager().GetRender().fps_limit = 59.94; break;
	case CELL_GCM_DISPLAY_FREQUENCY_SCANOUT:
		Emu.GetGSManager().GetRender().frequency_mode = freq; cellGcmSys.Todo("Unimplemented display frequency: Scanout"); break;
	case CELL_GCM_DISPLAY_FREQUENCY_DISABLE:
		Emu.GetGSManager().GetRender().frequency_mode = freq; cellGcmSys.Todo("Unimplemented display frequency: Disabled"); break;
	default: cellGcmSys.Error("Improper display frequency specified!"); return CELL_OK;
	}

	return CELL_OK;
}

s32 cellGcmSetTileInfo(u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base, u8 bank)
{
	cellGcmSys.Warning("cellGcmSetTileInfo(index=%d, location=%d, offset=%d, size=%d, pitch=%d, comp=%d, base=%d, bank=%d)",
		index, location, offset, size, pitch, comp, base, bank);

	if (index >= rsx::limits::tiles_count || base >= 800 || bank >= 4)
	{
		cellGcmSys.Error("cellGcmSetTileInfo: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	if (offset & 0xffff || size & 0xffff || pitch & 0xf)
	{
		cellGcmSys.Error("cellGcmSetTileInfo: CELL_GCM_ERROR_INVALID_ALIGNMENT");
		return CELL_GCM_ERROR_INVALID_ALIGNMENT;
	}

	if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12)))
	{
		cellGcmSys.Error("cellGcmSetTileInfo: CELL_GCM_ERROR_INVALID_ALIGNMENT");
		return CELL_GCM_ERROR_INVALID_ENUM;
	}

	if (comp)
	{
		cellGcmSys.Error("cellGcmSetTileInfo: bad compression mode! (%d)", comp);
	}

	auto& tile = Emu.GetGSManager().GetRender().tiles[index];
	tile.location = location;
	tile.offset = offset;
	tile.size = size;
	tile.pitch = pitch;
	tile.comp = comp;
	tile.base = base;
	tile.bank = bank;

	vm::get_ptr<CellGcmTileInfo>(Emu.GetGSManager().GetRender().tiles_addr)[index] = tile.pack();
	return CELL_OK;
}

void cellGcmSetUserHandler(vm::ptr<void(u32)> handler)
{
	cellGcmSys.Warning("cellGcmSetUserHandler(handler=*0x%x)", handler);

	Emu.GetGSManager().GetRender().user_handler = handler;
}

s32 cellGcmSetUserCommand()
{
	throw EXCEPTION("");
}

void cellGcmSetVBlankHandler(vm::ptr<void(u32)> handler)
{
	cellGcmSys.Warning("cellGcmSetVBlankHandler(handler=*0x%x)", handler);

	Emu.GetGSManager().GetRender().vblank_handler = handler;
}

s32 cellGcmSetWaitFlip(vm::ptr<CellGcmContextData> ctxt)
{
	cellGcmSys.Warning("cellGcmSetWaitFlip(ctx=*0x%x)", ctxt);

	// TODO: emit RSX command for "wait flip" operation

	return CELL_OK;
}

s32 cellGcmSetWaitFlipUnsafe()
{
	throw EXCEPTION("");
}

s32 cellGcmSetZcull(u8 index, u32 offset, u32 width, u32 height, u32 cullStart, u32 zFormat, u32 aaFormat, u32 zCullDir, u32 zCullFormat, u32 sFunc, u32 sRef, u32 sMask)
{
	cellGcmSys.Todo("cellGcmSetZcull(index=%d, offset=0x%x, width=%d, height=%d, cullStart=0x%x, zFormat=0x%x, aaFormat=0x%x, zCullDir=0x%x, zCullFormat=0x%x, sFunc=0x%x, sRef=0x%x, sMask=0x%x)",
		index, offset, width, height, cullStart, zFormat, aaFormat, zCullDir, zCullFormat, sFunc, sRef, sMask);

	if (index >= rsx::limits::zculls_count)
	{
		cellGcmSys.Error("cellGcmSetZcull: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	auto& zcull = Emu.GetGSManager().GetRender().zculls[index];
	zcull.offset = offset;
	zcull.width = width; 
	zcull.height = height;
	zcull.cullStart = cullStart;
	zcull.zFormat = zFormat;
	zcull.aaFormat = aaFormat;
	zcull.zcullDir = zCullDir;
	zcull.zcullFormat = zCullFormat;
	zcull.sFunc = sFunc;
	zcull.sRef = sRef;
	zcull.sMask = sMask;

	vm::get_ptr<CellGcmZcullInfo>(Emu.GetGSManager().GetRender().zculls_addr)[index] = zcull.pack();
	return CELL_OK;
}

s32 cellGcmUnbindTile(u8 index)
{
	cellGcmSys.Warning("cellGcmUnbindTile(index=%d)", index);

	if (index >= rsx::limits::tiles_count)
	{
		cellGcmSys.Error("cellGcmUnbindTile: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	auto& tile = Emu.GetGSManager().GetRender().tiles[index];
	tile.binded = false;

	return CELL_OK;
}

s32 cellGcmUnbindZcull(u8 index)
{
	cellGcmSys.Warning("cellGcmUnbindZcull(index=%d)", index);

	if (index >= 8)
	{
		cellGcmSys.Error("cellGcmUnbindZcull: CELL_EINVAL");
		return CELL_EINVAL;
	}

	auto& zcull = Emu.GetGSManager().GetRender().zculls[index];
	zcull.binded = false;

	return CELL_OK;
}

u32 cellGcmGetTileInfo()
{
	cellGcmSys.Warning("cellGcmGetTileInfo()");
	return Emu.GetGSManager().GetRender().tiles_addr;
}

u32 cellGcmGetZcullInfo()
{
	cellGcmSys.Warning("cellGcmGetZcullInfo()");
	return Emu.GetGSManager().GetRender().zculls_addr;
}

u32 cellGcmGetDisplayInfo()
{
	cellGcmSys.Warning("cellGcmGetDisplayInfo() = 0x%x", Emu.GetGSManager().GetRender().gcm_buffers.addr());
	return Emu.GetGSManager().GetRender().gcm_buffers.addr();
}

s32 cellGcmGetCurrentDisplayBufferId(vm::ptr<u8> id)
{
	cellGcmSys.Warning("cellGcmGetCurrentDisplayBufferId(id=*0x%x)", id);

	if (Emu.GetGSManager().GetRender().gcm_current_buffer > UINT8_MAX)
	{
		throw EXCEPTION("Unexpected");
	}

	*id = Emu.GetGSManager().GetRender().gcm_current_buffer;

	return CELL_OK;
}

s32 cellGcmSetInvalidateTile()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmTerminate()
{
	throw EXCEPTION("");
}

s32 cellGcmDumpGraphicsError()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmGetDisplayBufferByFlipIndex()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

u64 cellGcmGetLastFlipTime()
{
	cellGcmSys.Log("cellGcmGetLastFlipTime()");

	return Emu.GetGSManager().GetRender().last_flip_time;
}

u64 cellGcmGetLastSecondVTime()
{
	cellGcmSys.Todo("cellGcmGetLastSecondVTime()");
	return CELL_OK;
}

u64 cellGcmGetVBlankCount()
{
	cellGcmSys.Log("cellGcmGetVBlankCount()");

	return Emu.GetGSManager().GetRender().vblank_count;
}

s32 cellGcmSysGetLastVBlankTime()
{
	throw EXCEPTION("");
}

s32 cellGcmInitSystemMode(u64 mode)
{
	cellGcmSys.Log("cellGcmInitSystemMode(mode=0x%x)", mode);

	system_mode = mode;

	return CELL_OK;
}

s32 cellGcmSetFlipImmediate(u8 id)
{
	cellGcmSys.Todo("cellGcmSetFlipImmediate(fid=0x%x)", id);

	if (id > 7)
	{
		cellGcmSys.Error("cellGcmSetFlipImmediate: CELL_GCM_ERROR_FAILURE");
		return CELL_GCM_ERROR_FAILURE;
	}

	cellGcmSetFlipMode(id);

	return CELL_OK;
}

s32 cellGcmSetGraphicsHandler()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetQueueHandler()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetSecondVHandler()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetVBlankFrequency()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSortRemapEaIoAddress()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

//----------------------------------------------------------------------------
// Memory Mapping
//----------------------------------------------------------------------------
s32 cellGcmAddressToOffset(u32 address, vm::ptr<u32> offset)
{
	cellGcmSys.Log("cellGcmAddressToOffset(address=0x%x, offset=*0x%x)", address, offset);

	// Address not on main memory or local memory
	if (address >= 0xD0000000)
	{
		return CELL_GCM_ERROR_FAILURE;
	}

	u32 result;

	// Address in local memory
	if ((address >> 28) == 0xC)
	{
		result = address - 0xC0000000;
	}
	// Address in main memory else check 
	else
	{
		const u32 upper12Bits = offsetTable.ioAddress[address >> 20];

		// If the address is mapped in IO
		if (upper12Bits != 0xFFFF)
		{
			result = (upper12Bits << 20) | (address & 0xFFFFF);
		}
		else
		{
			return CELL_GCM_ERROR_FAILURE;
		}
	}

	*offset = result;
	return CELL_OK;
}

u32 cellGcmGetMaxIoMapSize()
{
	cellGcmSys.Log("cellGcmGetMaxIoMapSize()");

	return RSXIOMem.GetSize() - RSXIOMem.GetReservedAmount();
}

void cellGcmGetOffsetTable(vm::ptr<CellGcmOffsetTable> table)
{
	cellGcmSys.Log("cellGcmGetOffsetTable(table=*0x%x)", table);

	table->ioAddress = offsetTable.ioAddress;
	table->eaAddress = offsetTable.eaAddress;
}

s32 cellGcmIoOffsetToAddress(u32 ioOffset, vm::ptr<u32> address)
{
	cellGcmSys.Log("cellGcmIoOffsetToAddress(ioOffset=0x%x, address=*0x%x)", ioOffset, address);

	u32 realAddr;

	if (!RSXIOMem.getRealAddr(ioOffset, realAddr)) 
		return CELL_GCM_ERROR_FAILURE;

	*address = realAddr;

	return CELL_OK;
}

s32 gcmMapEaIoAddress(u32 ea, u32 io, u32 size, bool is_strict)
{
	if ((ea & 0xFFFFF) || (io & 0xFFFFF) || (size & 0xFFFFF)) return CELL_GCM_ERROR_FAILURE;

	// Check if the mapping was successfull
	if (RSXIOMem.Map(ea, size, io))
	{
		// Fill the offset table
		for (u32 i = 0; i<(size >> 20); i++)
		{
			offsetTable.ioAddress[(ea >> 20) + i] = (io >> 20) + i;
			offsetTable.eaAddress[(io >> 20) + i] = (ea >> 20) + i;
			Emu.GetGSManager().GetRender().strict_ordering[(io >> 20) + i] = is_strict;
		}
	}
	else
	{
		cellGcmSys.Error("cellGcmMapEaIoAddress: CELL_GCM_ERROR_FAILURE");
		return CELL_GCM_ERROR_FAILURE;
	}

	return CELL_OK;
}

s32 cellGcmMapEaIoAddress(u32 ea, u32 io, u32 size)
{
	cellGcmSys.Warning("cellGcmMapEaIoAddress(ea=0x%x, io=0x%x, size=0x%x)", ea, io, size);

	return gcmMapEaIoAddress(ea, io, size, false);
}

s32 cellGcmMapEaIoAddressWithFlags(u32 ea, u32 io, u32 size, u32 flags)
{
	cellGcmSys.Warning("cellGcmMapEaIoAddressWithFlags(ea=0x%x, io=0x%x, size=0x%x, flags=0x%x)", ea, io, size, flags);

	assert(flags == 2 /*CELL_GCM_IOMAP_FLAG_STRICT_ORDERING*/);

	return gcmMapEaIoAddress(ea, io, size, true);
}

s32 cellGcmMapLocalMemory(vm::ptr<u32> address, vm::ptr<u32> size)
{
	cellGcmSys.Warning("cellGcmMapLocalMemory(address=*0x%x, size=*0x%x)", address, size);

	if (!local_addr && !local_size && vm::falloc(local_addr = 0xC0000000, local_size = 0xf900000 /* TODO */, vm::video))
	{
		*address = local_addr;
		*size = local_size;
	}
	else
	{
		cellGcmSys.Error("RSX local memory already mapped");
		return CELL_GCM_ERROR_FAILURE;
	}

	return CELL_OK;
}

s32 cellGcmMapMainMemory(u32 ea, u32 size, vm::ptr<u32> offset)
{
	cellGcmSys.Warning("cellGcmMapMainMemory(ea=0x%x, size=0x%x, offset=*0x%x)", ea, size, offset);

	if ((ea & 0xFFFFF) || (size & 0xFFFFF)) return CELL_GCM_ERROR_FAILURE;

	u32 io = RSXIOMem.Map(ea, size);

	//check if the mapping was successfull
	if (RSXIOMem.RealAddr(io) == ea)
	{
		//fill the offset table
		for (u32 i = 0; i<(size >> 20); i++)
		{
			offsetTable.ioAddress[(ea >> 20) + i] = (u16)((io >> 20) + i);
			offsetTable.eaAddress[(io >> 20) + i] = (u16)((ea >> 20) + i);
			Emu.GetGSManager().GetRender().strict_ordering[(io >> 20) + i] = false;
		}

		*offset = io;
	}
	else
	{
		cellGcmSys.Error("cellGcmMapMainMemory: CELL_GCM_ERROR_NO_IO_PAGE_TABLE");
		return CELL_GCM_ERROR_NO_IO_PAGE_TABLE;
	}

	Emu.GetGSManager().GetRender().main_mem_addr = Emu.GetGSManager().GetRender().ioAddress;

	return CELL_OK;
}

s32 cellGcmReserveIoMapSize(u32 size)
{
	cellGcmSys.Log("cellGcmReserveIoMapSize(size=0x%x)", size);

	if (size & 0xFFFFF)
	{
		cellGcmSys.Error("cellGcmReserveIoMapSize: CELL_GCM_ERROR_INVALID_ALIGNMENT");
		return CELL_GCM_ERROR_INVALID_ALIGNMENT;
	}

	if (size > cellGcmGetMaxIoMapSize())
	{
		cellGcmSys.Error("cellGcmReserveIoMapSize: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	RSXIOMem.Reserve(size);
	return CELL_OK;
}

s32 cellGcmUnmapEaIoAddress(u32 ea)
{
	cellGcmSys.Log("cellGcmUnmapEaIoAddress(ea=0x%x)", ea);

	u32 size;
	if (RSXIOMem.UnmapRealAddress(ea, size))
	{
		const u32 io = offsetTable.ioAddress[ea >>= 20];

		for (u32 i = 0; i < size >> 20; i++)
		{
			offsetTable.ioAddress[ea + i] = 0xFFFF;
			offsetTable.eaAddress[io + i] = 0xFFFF;
		}
	}
	else
	{
		cellGcmSys.Error("cellGcmUnmapEaIoAddress(ea=0x%x): UnmapRealAddress() failed");
		return CELL_GCM_ERROR_FAILURE;
	}

	return CELL_OK;
}

s32 cellGcmUnmapIoAddress(u32 io)
{
	cellGcmSys.Log("cellGcmUnmapIoAddress(io=0x%x)", io);

	u32 size;
	if (RSXIOMem.UnmapAddress(io, size))
	{
		const u32 ea = offsetTable.eaAddress[io >>= 20];

		for (u32 i = 0; i < size >> 20; i++)
		{
			offsetTable.ioAddress[ea + i] = 0xFFFF;
			offsetTable.eaAddress[io + i] = 0xFFFF;
		}
	}
	else
	{
		cellGcmSys.Error("cellGcmUnmapIoAddress(io=0x%x): UnmapAddress() failed");
		return CELL_GCM_ERROR_FAILURE;
	}

	return CELL_OK;
}

s32 cellGcmUnreserveIoMapSize(u32 size)
{
	cellGcmSys.Log("cellGcmUnreserveIoMapSize(size=0x%x)", size);

	if (size & 0xFFFFF)
	{
		cellGcmSys.Error("cellGcmReserveIoMapSize: CELL_GCM_ERROR_INVALID_ALIGNMENT");
		return CELL_GCM_ERROR_INVALID_ALIGNMENT;
	}

	if (size > RSXIOMem.GetReservedAmount())
	{
		cellGcmSys.Error("cellGcmReserveIoMapSize: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	RSXIOMem.Unreserve(size);
	return CELL_OK;
}

//----------------------------------------------------------------------------
// Cursor
//----------------------------------------------------------------------------

s32 cellGcmInitCursor()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetCursorPosition(s32 x, s32 y)
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetCursorDisable()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmUpdateCursor()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetCursorEnable()
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

s32 cellGcmSetCursorImageOffset(u32 offset)
{
	UNIMPLEMENTED_FUNC(cellGcmSys);
	return CELL_OK;
}

//------------------------------------------------------------------------
// Functions for Maintaining Compatibility
//------------------------------------------------------------------------

void cellGcmSetDefaultCommandBuffer()
{
	cellGcmSys.Warning("cellGcmSetDefaultCommandBuffer()");
	vm::write32(Emu.GetGSManager().GetRender().ctxt_addr, gcm_info.context_addr);
}

s32 cellGcmSetDefaultCommandBufferAndSegmentWordSize()
{
	throw EXCEPTION("");
}

//------------------------------------------------------------------------
// Other
//------------------------------------------------------------------------

s32 _cellGcmSetFlipCommand(PPUThread& ppu, vm::ptr<CellGcmContextData> ctx, u32 id)
{
	cellGcmSys.Log("cellGcmSetFlipCommand(ctx=*0x%x, id=0x%x)", ctx, id);

	return cellGcmSetPrepareFlip(ppu, ctx, id);
}

s32 _cellGcmSetFlipCommandWithWaitLabel(PPUThread& ppu, vm::ptr<CellGcmContextData> ctx, u32 id, u32 label_index, u32 label_value)
{
	cellGcmSys.Log("cellGcmSetFlipCommandWithWaitLabel(ctx=*0x%x, id=0x%x, label_index=0x%x, label_value=0x%x)", ctx, id, label_index, label_value);

	s32 res = cellGcmSetPrepareFlip(ppu, ctx, id);
	vm::write32(gcm_info.label_addr + 0x10 * label_index, label_value);
	return res < 0 ? CELL_GCM_ERROR_FAILURE : CELL_OK;
}

s32 cellGcmSetTile(u8 index, u8 location, u32 offset, u32 size, u32 pitch, u8 comp, u16 base, u8 bank)
{
	cellGcmSys.Warning("cellGcmSetTile(index=%d, location=%d, offset=%d, size=%d, pitch=%d, comp=%d, base=%d, bank=%d)",
		index, location, offset, size, pitch, comp, base, bank);

	// Copied form cellGcmSetTileInfo
	if (index >= rsx::limits::tiles_count || base >= 800 || bank >= 4)
	{
		cellGcmSys.Error("cellGcmSetTile: CELL_GCM_ERROR_INVALID_VALUE");
		return CELL_GCM_ERROR_INVALID_VALUE;
	}

	if (offset & 0xffff || size & 0xffff || pitch & 0xf)
	{
		cellGcmSys.Error("cellGcmSetTile: CELL_GCM_ERROR_INVALID_ALIGNMENT");
		return CELL_GCM_ERROR_INVALID_ALIGNMENT;
	}

	if (location >= 2 || (comp != 0 && (comp < 7 || comp > 12)))
	{
		cellGcmSys.Error("cellGcmSetTile: CELL_GCM_ERROR_INVALID_ENUM");
		return CELL_GCM_ERROR_INVALID_ENUM;
	}

	if (comp)
	{
		cellGcmSys.Error("cellGcmSetTile: bad compression mode! (%d)", comp);
	}

	auto& tile = Emu.GetGSManager().GetRender().tiles[index];
	tile.location = location;
	tile.offset = offset;
	tile.size = size;
	tile.pitch = pitch;
	tile.comp = comp;
	tile.base = base;
	tile.bank = bank;

	vm::get_ptr<CellGcmTileInfo>(Emu.GetGSManager().GetRender().tiles_addr)[index] = tile.pack();
	return CELL_OK;
}

s32 _cellGcmFunc2()
{
	throw EXCEPTION("");
}

s32 _cellGcmFunc3()
{
	throw EXCEPTION("");
}

s32 _cellGcmFunc4()
{
	throw EXCEPTION("");
}

s32 _cellGcmFunc13()
{
	throw EXCEPTION("");
}

s32 _cellGcmFunc38()
{
	throw EXCEPTION("");
}

s32 cellGcmGpadGetStatus()
{
	throw EXCEPTION("");
}

s32 cellGcmGpadNotifyCaptureSurface()
{
	throw EXCEPTION("");
}

s32 cellGcmGpadCaptureSnapshot()
{
	throw EXCEPTION("");
}


//----------------------------------------------------------------------------


/**
 * Using current to determine what is the next useable command buffer.
 * Caller may wait for RSX not to use the command buffer.
 */
static std::pair<u32, u32> getNextCommandBufferBeginEnd(u32 current)
{
	u32 currentRange = (current - g_defaultCommandBufferBegin) / (32 * 1024);
	if (currentRange >= g_defaultCommandBufferFragmentCount - 1)
		return std::make_pair(g_defaultCommandBufferBegin + 4096, g_defaultCommandBufferBegin + 32 * 1024 - 4);
	return std::make_pair(g_defaultCommandBufferBegin + (currentRange + 1) * 32 * 1024,
		g_defaultCommandBufferBegin + (currentRange + 2) * 32 * 1024 - 4);
}

static u32 getOffsetFromAddress(u32 address)
{
	const u32 upper = offsetTable.ioAddress[address >> 20]; // 12 bits
	assert(upper != 0xFFFF);
	return (upper << 20) | (address & 0xFFFFF);
}

/**
 * Returns true if getPos is a valid position in command buffer and not between
 * bufferBegin and bufferEnd which are absolute memory address
 */
static bool isInCommandBufferExcept(u32 getPos, u32 bufferBegin, u32 bufferEnd)
{
	// Is outside of default command buffer :
	// It's in a call/return statement
	// Conservatively return false here
	if (getPos < getOffsetFromAddress(g_defaultCommandBufferBegin + 4096) &&
		getPos > getOffsetFromAddress(g_defaultCommandBufferBegin + g_defaultCommandBufferFragmentCount * 32 * 1024))
		return false;
	if (getPos >= getOffsetFromAddress(bufferBegin) &&
		getPos <= getOffsetFromAddress(bufferEnd))
		return false;
	return true;
}

// TODO: Avoid using syscall 1023 for calling this function
s32 cellGcmCallback(vm::ptr<CellGcmContextData> context, u32 count)
{
	cellGcmSys.Log("cellGcmCallback(context=*0x%x, count=0x%x)", context, count);

	auto& ctrl = vm::get_ref<CellGcmControl>(gcm_info.control_addr);
	const std::chrono::time_point<std::chrono::system_clock> enterWait = std::chrono::system_clock::now();
	// Flush command buffer (ie allow RSX to read up to context->current)
	ctrl.put.exchange(getOffsetFromAddress(context->current.addr()));

	std::pair<u32, u32> newCommandBuffer = getNextCommandBufferBeginEnd(context->current.addr());
	u32 offset = getOffsetFromAddress(newCommandBuffer.first);
	// Write jump instruction
	*context->current = CELL_GCM_METHOD_FLAG_JUMP | offset;
	// Update current command buffer
	context->begin.set(newCommandBuffer.first);
	context->current.set(newCommandBuffer.first);
	context->end.set(newCommandBuffer.second);

	// Wait for rsx to "release" the new command buffer
	while (!Emu.IsStopped())
	{
		u32 getPos = ctrl.get.load().value();
		if (isInCommandBufferExcept(getPos, newCommandBuffer.first, newCommandBuffer.second))
			break;
		std::chrono::time_point<std::chrono::system_clock> waitPoint = std::chrono::system_clock::now();
		long long elapsedTime = std::chrono::duration_cast<std::chrono::seconds>(waitPoint - enterWait).count();
		if (elapsedTime > 0)
			cellGcmSys.Error("Has wait for more than a second for command buffer to be released by RSX");
		std::this_thread::yield();
	}

	return CELL_OK;
}

//----------------------------------------------------------------------------

Module<> cellGcmSys("cellGcmSys", []()
{
	current_config.ioAddress = 0;
	current_config.localAddress = 0;
	local_size = 0;
	local_addr = 0;

	// Data Retrieval
	REG_FUNC(cellGcmSys, cellGcmGetCurrentField);
	REG_FUNC(cellGcmSys, cellGcmGetLabelAddress);
	REG_FUNC(cellGcmSys, cellGcmGetNotifyDataAddress);
	REG_FUNC(cellGcmSys, _cellGcmFunc12);
	REG_FUNC(cellGcmSys, cellGcmGetReport);
	REG_FUNC(cellGcmSys, cellGcmGetReportDataAddress);
	REG_FUNC(cellGcmSys, cellGcmGetReportDataAddressLocation);
	REG_FUNC(cellGcmSys, cellGcmGetReportDataLocation);
	REG_FUNC(cellGcmSys, cellGcmGetTimeStamp);
	REG_FUNC(cellGcmSys, cellGcmGetTimeStampLocation);

	// Command Buffer Control
	REG_FUNC(cellGcmSys, cellGcmGetControlRegister);
	REG_FUNC(cellGcmSys, cellGcmGetDefaultCommandWordSize);
	REG_FUNC(cellGcmSys, cellGcmGetDefaultSegmentWordSize);
	REG_FUNC(cellGcmSys, cellGcmInitDefaultFifoMode);
	REG_FUNC(cellGcmSys, cellGcmSetDefaultFifoSize);

	// Hardware Resource Management
	REG_FUNC(cellGcmSys, cellGcmBindTile);
	REG_FUNC(cellGcmSys, cellGcmBindZcull);
	REG_FUNC(cellGcmSys, cellGcmDumpGraphicsError);
	REG_FUNC(cellGcmSys, cellGcmGetConfiguration);
	REG_FUNC(cellGcmSys, cellGcmGetDisplayBufferByFlipIndex);
	REG_FUNC(cellGcmSys, cellGcmGetFlipStatus);
	REG_FUNC(cellGcmSys, cellGcmGetLastFlipTime);
	REG_FUNC(cellGcmSys, cellGcmGetLastSecondVTime);
	REG_FUNC(cellGcmSys, cellGcmGetTiledPitchSize);
	REG_FUNC(cellGcmSys, cellGcmGetVBlankCount);
	REG_FUNC(cellGcmSys, cellGcmSysGetLastVBlankTime);
	REG_FUNC(cellGcmSys, _cellGcmFunc1);
	REG_FUNC(cellGcmSys, _cellGcmFunc15);
	REG_FUNC(cellGcmSys, _cellGcmInitBody);
	REG_FUNC(cellGcmSys, cellGcmInitSystemMode);
	REG_FUNC(cellGcmSys, cellGcmResetFlipStatus);
	REG_FUNC(cellGcmSys, cellGcmSetDebugOutputLevel);
	REG_FUNC(cellGcmSys, cellGcmSetDisplayBuffer);
	REG_FUNC(cellGcmSys, cellGcmSetFlip); //
	REG_FUNC(cellGcmSys, cellGcmSetFlipHandler);
	REG_FUNC(cellGcmSys, cellGcmSetFlipImmediate);
	REG_FUNC(cellGcmSys, cellGcmSetFlipMode);
	REG_FUNC(cellGcmSys, cellGcmSetFlipStatus);
	REG_FUNC(cellGcmSys, cellGcmSetGraphicsHandler);
	REG_FUNC(cellGcmSys, cellGcmSetPrepareFlip);
	REG_FUNC(cellGcmSys, cellGcmSetQueueHandler);
	REG_FUNC(cellGcmSys, cellGcmSetSecondVFrequency);
	REG_FUNC(cellGcmSys, cellGcmSetSecondVHandler);
	REG_FUNC(cellGcmSys, cellGcmSetTileInfo);
	REG_FUNC(cellGcmSys, cellGcmSetUserHandler);
	REG_FUNC(cellGcmSys, cellGcmSetUserCommand); //
	REG_FUNC(cellGcmSys, cellGcmSetVBlankFrequency);
	REG_FUNC(cellGcmSys, cellGcmSetVBlankHandler);
	REG_FUNC(cellGcmSys, cellGcmSetWaitFlip); //
	REG_FUNC(cellGcmSys, cellGcmSetWaitFlipUnsafe); //
	REG_FUNC(cellGcmSys, cellGcmSetZcull);
	REG_FUNC(cellGcmSys, cellGcmSortRemapEaIoAddress);
	REG_FUNC(cellGcmSys, cellGcmUnbindTile);
	REG_FUNC(cellGcmSys, cellGcmUnbindZcull);
	REG_FUNC(cellGcmSys, cellGcmGetTileInfo);
	REG_FUNC(cellGcmSys, cellGcmGetZcullInfo);
	REG_FUNC(cellGcmSys, cellGcmGetDisplayInfo);
	REG_FUNC(cellGcmSys, cellGcmGetCurrentDisplayBufferId);
	REG_FUNC(cellGcmSys, cellGcmSetInvalidateTile);
	REG_FUNC(cellGcmSys, cellGcmTerminate);

	// Memory Mapping
	REG_FUNC(cellGcmSys, cellGcmAddressToOffset);
	REG_FUNC(cellGcmSys, cellGcmGetMaxIoMapSize);
	REG_FUNC(cellGcmSys, cellGcmGetOffsetTable);
	REG_FUNC(cellGcmSys, cellGcmIoOffsetToAddress);
	REG_FUNC(cellGcmSys, cellGcmMapEaIoAddress);
	REG_FUNC(cellGcmSys, cellGcmMapEaIoAddressWithFlags);
	REG_FUNC(cellGcmSys, cellGcmMapLocalMemory);
	REG_FUNC(cellGcmSys, cellGcmMapMainMemory);
	REG_FUNC(cellGcmSys, cellGcmReserveIoMapSize);
	REG_FUNC(cellGcmSys, cellGcmUnmapEaIoAddress);
	REG_FUNC(cellGcmSys, cellGcmUnmapIoAddress);
	REG_FUNC(cellGcmSys, cellGcmUnreserveIoMapSize);

	// Cursor
	REG_FUNC(cellGcmSys, cellGcmInitCursor);
	REG_FUNC(cellGcmSys, cellGcmSetCursorEnable);
	REG_FUNC(cellGcmSys, cellGcmSetCursorDisable);
	REG_FUNC(cellGcmSys, cellGcmSetCursorImageOffset);
	REG_FUNC(cellGcmSys, cellGcmSetCursorPosition);
	REG_FUNC(cellGcmSys, cellGcmUpdateCursor);

	// Functions for Maintaining Compatibility
	REG_FUNC(cellGcmSys, cellGcmSetDefaultCommandBuffer);
	REG_FUNC(cellGcmSys, cellGcmSetDefaultCommandBufferAndSegmentWordSize);

	// Other
	REG_FUNC(cellGcmSys, _cellGcmSetFlipCommand);
	REG_FUNC(cellGcmSys, _cellGcmSetFlipCommandWithWaitLabel);
	REG_FUNC(cellGcmSys, cellGcmSetTile);
	REG_FUNC(cellGcmSys, _cellGcmFunc2);
	REG_FUNC(cellGcmSys, _cellGcmFunc3);
	REG_FUNC(cellGcmSys, _cellGcmFunc4);
	REG_FUNC(cellGcmSys, _cellGcmFunc13);
	REG_FUNC(cellGcmSys, _cellGcmFunc38);

	// GPAD
	REG_FUNC(cellGcmSys, cellGcmGpadGetStatus);
	REG_FUNC(cellGcmSys, cellGcmGpadNotifyCaptureSurface);
	REG_FUNC(cellGcmSys, cellGcmGpadCaptureSnapshot);
});
