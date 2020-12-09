#include "m68k_core.h"

typedef void (*m68k_debug_handler)(m68k_context *context, uint32_t pc);

void resume_68k(m68k_context *context)
{
}

m68k_context * m68k_handle_code_write(uint32_t address, m68k_context * context)
{
	return NULL;
}

void m68k_invalidate_code_range(m68k_context *context, uint32_t start, uint32_t end)
{
}

uint32_t get_instruction_start(m68k_options *opts, uint32_t address)
{
	return 0;
}

code_ptr get_native_address_trans(m68k_context * context, uint32_t address)
{
	return NULL;
}

void start_68k_context(m68k_context * context, uint32_t address)
{
}

void insert_breakpoint(m68k_context * context, uint32_t address, m68k_debug_handler bp_handler)
{
}

void remove_breakpoint(m68k_context * context, uint32_t address)
{
}

void m68k_options_free(m68k_options *opts)
{
}

void m68k_serialize(m68k_context *context, uint32_t pc, serialize_buffer *buf)
{
}

void m68k_deserialize(deserialize_buffer *buf, void *vcontext)
{
}
