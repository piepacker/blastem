#include <string.h>

void z80_read_8(z80_context *context)
{
	context->cycles += 3 * context->opts->gen.clock_divider;
	uint8_t *fast = context->fastread[context->scratch1 >> 10];
	if (fast) {
		context->scratch1 = fast[context->scratch1 & 0x3FF];
	} else {
		context->scratch1 = read_byte(context->scratch1, (void **)context->mem_pointers, &context->opts->gen, context);
	}
}

void z80_write_8(z80_context *context)
{
	context->cycles += 3 * context->opts->gen.clock_divider;
	uint8_t *fast = context->fastwrite[context->scratch2 >> 10];
	if (fast) {
		fast[context->scratch2 & 0x3FF] = context->scratch1;
	} else {
		write_byte(context->scratch2, context->scratch1, (void **)context->mem_pointers, &context->opts->gen, context);
	}
}

void z80_io_read8(z80_context *context)
{
	uint32_t tmp_mask = context->opts->gen.address_mask;
	memmap_chunk const *tmp_map = context->opts->gen.memmap;
	uint32_t tmp_chunks = context->opts->gen.memmap_chunks;
	
	context->opts->gen.address_mask = context->io_mask;
	context->opts->gen.memmap = context->io_map;
	context->opts->gen.memmap_chunks = context->io_chunks;
	
	context->cycles += 4 * context->opts->gen.clock_divider;
	context->scratch1 = read_byte(context->scratch1, (void **)context->mem_pointers, &context->opts->gen, context);
	
	context->opts->gen.address_mask = tmp_mask;
	context->opts->gen.memmap = tmp_map;
	context->opts->gen.memmap_chunks = tmp_chunks;
}

void z80_io_write8(z80_context *context)
{
	uint32_t tmp_mask = context->opts->gen.address_mask;
	memmap_chunk const *tmp_map = context->opts->gen.memmap;
	uint32_t tmp_chunks = context->opts->gen.memmap_chunks;
	
	context->opts->gen.address_mask = context->io_mask;
	context->opts->gen.memmap = context->io_map;
	context->opts->gen.memmap_chunks = context->io_chunks;
	
	context->cycles += 4 * context->opts->gen.clock_divider;
	write_byte(context->scratch2, context->scratch1, (void **)context->mem_pointers, &context->opts->gen, context);
	
	context->opts->gen.address_mask = tmp_mask;
	context->opts->gen.memmap = tmp_map;
	context->opts->gen.memmap_chunks = tmp_chunks;
}

//quick hack until I get a chance to change which init method these get passed to
static memmap_chunk const * tmp_io_chunks;
static uint32_t tmp_num_io_chunks, tmp_io_mask;
void init_z80_opts(z80_options * options, memmap_chunk const * chunks, uint32_t num_chunks, memmap_chunk const * io_chunks, uint32_t num_io_chunks, uint32_t clock_divider, uint32_t io_address_mask)
{
	memset(options, 0, sizeof(*options));
	options->gen.memmap = chunks;
	options->gen.memmap_chunks = num_chunks;
	options->gen.address_mask = 0xFFFF;
	options->gen.max_address = 0xFFFF;
	options->gen.clock_divider = clock_divider;
	tmp_io_chunks = io_chunks;
	tmp_num_io_chunks = num_io_chunks;
	tmp_io_mask = io_address_mask;
}

void z80_options_free(z80_options *opts)
{
	free(opts);
}

z80_context * init_z80_context(z80_options *options)
{
	z80_context *context = calloc(1, sizeof(z80_context));
	context->opts = options;
	context->io_map = (memmap_chunk *)tmp_io_chunks;
	context->io_chunks = tmp_num_io_chunks;
	context->io_mask = tmp_io_mask;
	context->int_cycle = context->int_end_cycle = context->nmi_cycle = 0xFFFFFFFFU;
	z80_invalidate_code_range(context, 0, 0xFFFF);
	return context;
}

void z80_sync_cycle(z80_context *context, uint32_t target_cycle)
{
	if (context->iff1 && context->int_cycle < target_cycle) {
		if (context->cycles > context->int_end_cycle) {
			context->int_cycle = 0xFFFFFFFFU;
		} else {
			target_cycle = context->int_cycle;
		}
	};
	if (context->nmi_cycle < target_cycle) {
		target_cycle = context->nmi_cycle;
	}
	context->sync_cycle = target_cycle;
}

void z80_run(z80_context *context, uint32_t target_cycle)
{
	if (context->reset || context->busack) {
		context->cycles = target_cycle;
	} else if (target_cycle > context->cycles) {
		if (context->busreq) {
			//busreq is sampled at the end of an m-cycle
			//we can approximate that by running for a single m-cycle after a bus request
			target_cycle = context->cycles + 4 * context->opts->gen.clock_divider;
		}
		z80_execute(context, target_cycle);
		if (context->busreq) {
			context->busack = 1;
		}
	}
}

void z80_assert_reset(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	context->reset = 1;
}

void z80_clear_reset(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	if (context->reset) {
		context->imode = 0;
		context->iff1 = context->iff2 = 0;
		context->pc = 0;
		context->reset = 0;
		if (context->busreq) {
			//TODO: Figure out appropriate delay
			context->busack = 1;
		}
	}
}

#define MAX_MCYCLE_LENGTH 6
void z80_assert_busreq(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	context->busreq = 1;
	//this is an imperfect aproximation since most M-cycles take less tstates than the max
	//and a short 3-tstate m-cycle can take an unbounded number due to wait states
	if (context->cycles - cycle > MAX_MCYCLE_LENGTH * context->opts->gen.clock_divider) {
		context->busack = 1;
	}
}

void z80_clear_busreq(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	context->busreq = 0;
	context->busack = 0;
	//there appears to be at least a 1 Z80 cycle delay between busreq
	//being released and resumption of execution
	context->cycles += context->opts->gen.clock_divider;
}

void z80_assert_nmi(z80_context *context, uint32_t cycle)
{
	context->nmi_cycle = cycle;
}

uint8_t z80_get_busack(z80_context * context, uint32_t cycle)
{
	z80_run(context, cycle);
	return context->busack;
}

void z80_invalidate_code_range(z80_context *context, uint32_t startA, uint32_t endA)
{
	for(startA &= ~0x3FF; startA < endA; startA += 1024)
	{
		uint8_t *start = get_native_pointer(startA, (void**)context->mem_pointers, &context->opts->gen);
		if (start) {
			uint8_t *end = get_native_pointer(startA + 1023, (void**)context->mem_pointers, &context->opts->gen);
			if (!end || end - start != 1023) {
				start = NULL;
			}
		}
		context->fastread[startA >> 10] = start;
		start = get_native_write_pointer(startA, (void**)context->mem_pointers, &context->opts->gen);
		if (start) {
			uint8_t *end = get_native_write_pointer(startA + 1023, (void**)context->mem_pointers, &context->opts->gen);
			if (!end || end - start != 1023) {
				start = NULL;
			}
		}
		context->fastwrite[startA >> 10] = start;
	}
}

void z80_adjust_cycles(z80_context * context, uint32_t deduction)
{
	context->cycles -= deduction;
	if (context->int_cycle != 0xFFFFFFFFU) {
		if (context->int_cycle > deduction) {
			context->int_cycle -= deduction;
		} else {
			context->int_cycle = 0;
		}
	}
	if (context->int_end_cycle != 0xFFFFFFFFU) {
		if (context->int_end_cycle > deduction) {
			context->int_end_cycle -= deduction;
		} else {
			context->int_end_cycle = 0;
		}
	}
	if (context->nmi_cycle != 0xFFFFFFFFU) {
		if (context->nmi_cycle > deduction) {
			context->nmi_cycle -= deduction;
		} else {
			context->nmi_cycle = 0;
		}
	}
}


void z80_serialize(z80_context *context, serialize_buffer *buf)
{
#define LOAD_SAVE(size, name) save_int##size(buf, context->name)
	LOAD_SAVE(32, sync_cycle);
	LOAD_SAVE(32, nmi_cycle);
	LOAD_SAVE(32, io_mask);
	LOAD_SAVE(32, io_chunks);
	LOAD_SAVE(32, int_end_cycle);
	LOAD_SAVE(32, int_cycle);
	LOAD_SAVE(32, cycles);
	LOAD_SAVE(16, wz);
	LOAD_SAVE(16, sp);
	LOAD_SAVE(16, scratch2);
	LOAD_SAVE(16, scratch1);
	LOAD_SAVE(16, pc);
	LOAD_SAVE(16, iy);
	LOAD_SAVE(16, ix);
	for (int i = 0; i < 8; i++) {
		LOAD_SAVE(8, main[i]);
		LOAD_SAVE(8, alt[i]);
	}
	LOAD_SAVE(8, zflag);
	LOAD_SAVE(8, rhigh);
	LOAD_SAVE(8, reset);
	LOAD_SAVE(8, r);
	LOAD_SAVE(8, pvflag);
	LOAD_SAVE(8, nflag);
	LOAD_SAVE(8, last_flag_result);
	LOAD_SAVE(8, int_value);
	LOAD_SAVE(8, imode);
	LOAD_SAVE(8, iff2);
	LOAD_SAVE(8, iff1);
	LOAD_SAVE(8, i);
	LOAD_SAVE(8, chflags);
	LOAD_SAVE(8, busreq);
	LOAD_SAVE(8, busack);
#undef LOAD_SAVE
}

void z80_deserialize(deserialize_buffer *buf, void *vcontext)
{
	z80_context *context = vcontext;
#define LOAD_SAVE(size, name) context->name = load_int##size(buf)
	LOAD_SAVE(32, sync_cycle);
	LOAD_SAVE(32, nmi_cycle);
	LOAD_SAVE(32, io_mask);
	LOAD_SAVE(32, io_chunks);
	LOAD_SAVE(32, int_end_cycle);
	LOAD_SAVE(32, int_cycle);
	LOAD_SAVE(32, cycles);
	LOAD_SAVE(16, wz);
	LOAD_SAVE(16, sp);
	LOAD_SAVE(16, scratch2);
	LOAD_SAVE(16, scratch1);
	LOAD_SAVE(16, pc);
	LOAD_SAVE(16, iy);
	LOAD_SAVE(16, ix);
	for (int i = 0; i < 8; i++) {
		LOAD_SAVE(8, main[i]);
		LOAD_SAVE(8, alt[i]);
	}
	LOAD_SAVE(8, zflag);
	LOAD_SAVE(8, rhigh);
	LOAD_SAVE(8, reset);
	LOAD_SAVE(8, r);
	LOAD_SAVE(8, pvflag);
	LOAD_SAVE(8, nflag);
	LOAD_SAVE(8, last_flag_result);
	LOAD_SAVE(8, int_value);
	LOAD_SAVE(8, imode);
	LOAD_SAVE(8, iff2);
	LOAD_SAVE(8, iff1);
	LOAD_SAVE(8, i);
	LOAD_SAVE(8, chflags);
	LOAD_SAVE(8, busreq);
	LOAD_SAVE(8, busack);
#undef LOAD_SAVE
}

void zinsert_breakpoint(z80_context * context, uint16_t address, uint8_t * bp_handler)
{
}

void zremove_breakpoint(z80_context * context, uint16_t address)
{
}
