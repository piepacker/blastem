#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "romdb.h"
#include "genesis.h"
#include "util.h"

typedef struct {
	uint8_t rom_space[512*1024];
	uint8_t regs[3];
} realtec;


uint8_t realtec_detect(uint8_t *rom, uint32_t rom_size)
{
	//All Realtec mapper games are 512KB total
	if (rom_size != 512*1024) {
		return 0;
	}
	return memcmp(rom + 0x7E100, "SEGA", 4) == 0;
}

static void *realtec_write_b(uint32_t address, void *context, uint8_t value)
{
	if (address & 1) {
		return context;
	}
	m68k_context *m68k = context;
	genesis_context *gen = m68k->system;
	if (!gen->extra) {
		gen->extra = m68k->mem_pointers[0];
	}
	realtec *r = gen->extra;
	uint32_t offset = address >> 13;
	uint8_t dirty = 0;
	if (offset < 3 && r->regs[offset] != value) {
		r->regs[offset] = value;
		//other regs are only 3 bits, so assume 3 for this one too
		uint32_t size = (r->regs[1] & 0x7) << 17;
		uint32_t start = (r->regs[2] & 7) << 17 | (r->regs[0] & 6) << 19;
		if (size > 512*1024) {
			size = 512*1024;
		}
		for(uint32_t cur = 0; cur < 512*1024; cur += size)
		{
			if (start + size > 512*1024) {
				memcpy(r->rom_space + cur, gen->cart + start/2, 512*1024-start);
				//assume it wraps
				memcpy(r->rom_space + cur + 512*1024-start, gen->cart, size - (512*1024-start));
			} else {
				memcpy(r->rom_space + cur, gen->cart + start/2, size);
			}
		}
		m68k_invalidate_code_range(gen->m68k, 0, 0x400000);
	}
	return context;
}

static void *realtec_write_w(uint32_t address, void *context, uint16_t value)
{
	return realtec_write_b(address, context, value >> 8);
}

rom_info realtec_configure_rom(uint8_t *rom, uint32_t rom_size, memmap_chunk const *base_map, uint32_t base_chunks)
{
	rom_info info;
	realtec *r = calloc(sizeof(realtec), 1);
	for (uint32_t i = 0; i < 512*1024; i += 8*1024)
	{
		memcpy(r->rom_space + i, rom + 0x7E000, 8*1024);
	}
	byteswap_rom(512*1024, (uint16_t *)r->rom_space);
	
	uint8_t *name_start = NULL, *name_end = NULL;
	for (int i = 0x90; i < 0xE0; i++)
	{
		if (name_start) {
			if (rom[i] < ' ' || rom[i] > 0x80 || !memcmp(rom+i, "ARE", 3)) {
				name_end = rom+i;
				break;
			}
		} else if (rom[i] > ' ' && rom[i] < 0x80) {
			name_start = rom + i;
		}
	}
	if (name_start && !name_end) {
		name_end = rom + 0xE0;
	}
	if (name_end) {
		while (name_end > name_start && name_end[-1] == ' ')
		{
			name_end--;
		}
		info.name = malloc(name_end-name_start+1);
		memcpy(info.name, name_start, name_end-name_start);
		info.name[name_end-name_start] = 0;
	} else {
		info.name = strdup("Realtec Game");
	}
	info.save_type = SAVE_NONE;
	info.save_size = 0;
	info.save_buffer = NULL;
	info.num_eeprom = 0;
	info.eeprom_map = NULL;
	info.port1_override = info.port2_override = info.ext_override = info.mouse_mode = NULL;
	info.map_chunks = base_chunks + 2;
	info.map = calloc(sizeof(memmap_chunk), info.map_chunks);
	info.map[0].mask = sizeof(r->rom_space)-1;
	info.map[0].flags = MMAP_READ|MMAP_CODE|MMAP_PTR_IDX;
	info.map[0].start = 0;
	info.map[0].end = 0x400000;
	info.map[0].buffer = r->rom_space;
	info.map[0].write_16 = NULL;
	info.map[0].read_16 = NULL;
	info.map[0].write_8 = NULL;
	info.map[0].read_8 = NULL;
	info.map[1].mask = 0x7FFF;
	info.map[1].flags = 0;
	info.map[1].start = 0x400000;
	info.map[1].end = 0x800000;
	info.map[1].write_16 = realtec_write_w;
	info.map[1].write_8 = realtec_write_b;
	info.map[1].read_16 = NULL;
	info.map[1].read_8 = NULL;
	memcpy(info.map + 2, base_map, base_chunks * sizeof(memmap_chunk));
	
	return info;
}
