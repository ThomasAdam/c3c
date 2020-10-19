// Copyright (c) 2020 Christoffer Lerno. All rights reserved.
// Use of this source code is governed by a LGPLv3.0
// a copy of which can be found in the LICENSE file.

#include "llvm_codegen_c_abi_internal.h"

#define MIN_ABI_STACK_ALIGN 4

static bool x86_try_use_free_regs(GenContext *context, Type *type);

static inline bool type_is_simd_vector(Type *type)
{
	type = type->canonical;
	return type->type_kind == TYPE_VECTOR && type_size(type) == 16;
}

static bool type_is_union_struct_with_simd_vector(Type *type)
{
	if (!type_is_union_struct(type)) return false;

	Decl **members = type->decl->strukt.members;
	VECEACH(members, i)
	{
		Type *member_type = members[i]->type;
		if (type_is_simd_vector(member_type)) return true;
		if (type_is_union_struct_with_simd_vector(type)) return true;
	}
	return false;
}


static unsigned x86_stack_alignment(Type *type, unsigned alignment)
{
	// Less than ABI, use default
	if (alignment < MIN_ABI_STACK_ALIGN) return 0;

	// On non-Darwin, the stack type alignment is always 4.
	if (!build_target.x86.is_darwin_vector_abi) return MIN_ABI_STACK_ALIGN;

	// Otherwise, if the type contains an SSE vector type, the alignment is 16.
	if (alignment >= 16 && (type_is_simd_vector(type) || type_is_union_struct_with_simd_vector(type)))
	{
		return 16;
	}
	return MIN_ABI_STACK_ALIGN;
}


static ABIArgInfo *x86_create_indirect_result(GenContext *context, Type *type, ByVal by_val)
{
	if (by_val != BY_VAL)
	{
		ABIArgInfo *info = abi_arg_new_indirect_not_by_val();

		if (context->abi.int_registers)
		{
			context->abi.int_registers--;
			if (!build_target.x86.is_mcu_api) info->attributes.by_reg = true;
		}
		return info;
	}

	// From here on everything is by val:

	// Compute alignment
	unsigned alignment = type_abi_alignment(type);
	unsigned stack_alignment = x86_stack_alignment(type, alignment);

	// Default alignment
	if (stack_alignment == 0) stack_alignment = 4;

	// Realign if alignment is greater.
	if (alignment > stack_alignment)
	{
		return abi_arg_new_indirect_realigned(stack_alignment);
	}

	return abi_arg_new_indirect_by_val();
}


ABIArgInfo *create_indirect_return_x86(GenContext *context)
{
	ABIArgInfo *info = abi_arg_new(ABI_ARG_INDIRECT);
	if (!context->abi.int_registers) return info;
	// Consume a register for the return.
	context->abi.int_registers--;
	if (build_target.x86.is_mcu_api) return info;

	return abi_arg_by_reg_attr(info);
}

static bool x86_should_return_type_in_reg(Type *type)
{
	type = type->canonical;
	unsigned size = type_size(type);
	if (size > 8) return false;

	// Require power of two for everything except mcu.
	if (!build_target.x86.is_mcu_api && !is_power_of_two(size)) return false;

	if (type->type_kind == TYPE_VECTOR)
	{
		// 64 (and 128 bit) vectors are not returned as registers
		return size < 8;
	}

	switch (type->type_kind)
	{
		case TYPE_VECTOR:
		case TYPE_POISONED:
		case TYPE_MEMBER:
		case TYPE_VOID:
		case TYPE_FUNC:
		case TYPE_TYPEDEF:
		case TYPE_TYPEINFO:
			UNREACHABLE
		case ALL_INTS:
		case ALL_FLOATS:
		case TYPE_BOOL:
		case TYPE_ENUM:
		case TYPE_POINTER:
		case TYPE_TYPEID:
		case TYPE_VARARRAY:
		case TYPE_ERR_UNION:
		case TYPE_STRING:
		case TYPE_SUBARRAY:
		case TYPE_ERRTYPE:
		case TYPE_COMPLEX:
			return true;
		case TYPE_ARRAY:
			// Small arrays <= 8 bytes.
			return x86_should_return_type_in_reg(type->array.base);
		case TYPE_STRUCT:
		case TYPE_UNION:
			// Handle below
			break;
	}
	// If all can be passed in registers, then pass in register
	// (remember we already limited the size!)
	Decl** members = type->decl->strukt.members;
	VECEACH (members, i)
	{
		Type *member_type = members[i]->type;
		if (type_is_empty_field(member_type, true)) continue;
		if (!x86_should_return_type_in_reg(member_type)) return false;
	}
	return true;
}

ABIArgInfo *x86_classify_return(GenContext *context, Type *type)
{
	if (type == type_void)
	{
		return abi_arg_new(ABI_ARG_IGNORE);
	}

	type = type_lowering(type);
	Type *base = NULL;
	unsigned elements = 0;
	if (context->abi.call_convention == CALL_CONVENTION_VECTOR || context->abi.call_convention == CALL_CONVENTION_REGCALL)
	{
		// Pass in the normal way.
		if (type_is_homogenous_aggregate(type, &base, &elements))
		{
			return abi_arg_new(ABI_ARG_DIRECT_COERCE);
		}
	}

	if (type->type_kind == TYPE_VECTOR)
	{
		// On Darwin, vectors may be returned in registers.
		if (build_target.x86.is_darwin_vector_abi)
		{
			unsigned size = type_size(type);
			if (size == 16)
			{
				// Special case, convert 128 bit vector to two 64 bit elements.
				return abi_arg_new_direct_coerce(abi_type_new_plain(type_get_vector(type_long, 2)));
			}
			// Always return in register if it fits in a general purpose
			// register, or if it is 64 bits and has a single element.
			if (size == 1 || size == 2 || size == 4 || (size == 8 && type->vector.len == 1))
			{
				return abi_arg_new_direct_coerce(abi_type_new_int_bits(size * 8));
			}
			return create_indirect_return_x86(context);
		}
		return abi_arg_new(ABI_ARG_DIRECT_COERCE);
	}

	if (type_is_abi_aggregate(type))
	{
		// If we don't allow small structs in reg:
		if (!build_target.x86.return_small_struct_in_reg_abi && type->type_kind == TYPE_COMPLEX)
		{
			return create_indirect_return_x86(context);
		}
		// Ignore empty struct/unions
		if (type_is_empty_union_struct(type, true))
		{
			return abi_arg_new(ABI_ARG_IGNORE);
		}

		// Check if we can return it in a register.
		if (x86_should_return_type_in_reg(type))
		{
			size_t size = type_size(type);
			// Special case is floats and pointers in single element structs (except for MSVC)
			Type *single_element = type_find_single_struct_element(type);
			if (single_element)
			{
				if ((type_is_float(single_element) && !build_target.x86.is_win32_float_struct_abi))
				{
					return abi_arg_new(ABI_ARG_EXPAND);
				}
				if (type_is_pointer(type))
				{
					return abi_arg_new(ABI_ARG_EXPAND);
				}
			}
			// This is not a single element struct, so we wrap it in an int.
			return abi_arg_new_direct_coerce(abi_type_new_int_bits(size * 8));
		}
		return create_indirect_return_x86(context);
	}

	// Is this small enough to need to be extended?
	if (type_is_promotable_integer(type))
	{
		return abi_arg_new_direct_int_ext(type);
	}

	// If we support something like int128, then this is an indirect return.
	if (type_is_integer(type) && type_size(type) > 8) return create_indirect_return_x86(context);

	// Otherwise we expect to just pass this nicely in the return.
	return abi_arg_new(ABI_ARG_DIRECT_COERCE);

}

static inline bool x86_should_aggregate_use_direct(GenContext *context, Type *type, bool *needs_padding)
{
	// On Windows, aggregates other than HFAs are never passed in registers, and
	// they do not consume register slots. Homogenous floating-point aggregates
	// (HFAs) have already been dealt with at this point.
	if (build_target.x86.is_win32_float_struct_abi) return false;

	*needs_padding = false;

	if (!x86_try_use_free_regs(context, type)) return false;

	if (build_target.x86.is_mcu_api) return true;

	switch (context->abi.call_convention)
	{
		case CALL_CONVENTION_FAST:
		case CALL_CONVENTION_VECTOR:
		case CALL_CONVENTION_REGCALL:
			if (type_size(type) <= 4 && context->abi.int_registers)
			{
				*needs_padding = true;
			}
			return false;
		default:
			return true;
	}
}

static inline bool x86_is_mmxtype(Type *type)
{
	// Return true if the type is an MMX type <2 x i32>, <4 x i16>, or <8 x i8>.
	if (type->type_kind != TYPE_VECTOR) return false;
	if (type_size(type->vector.base) >= 8) return false;
	if (!type_is_integer(type->vector.base)) return false;
	return type_size(type) == 8;
}

static inline bool x86_can_expand_indirect_aggregate_arg(Type *type)
{
	assert(type_is_abi_aggregate(type));

	// Test whether an argument type which is to be passed indirectly (on the
	// stack) would have the equivalent layout if it was expanded into separate
	// arguments. If so, we prefer to do the latter to avoid inhibiting
	// optimizations.

	// Error unions can always be expanded since they are two pointers wide.
	if (type->canonical->type_kind == TYPE_ERR_UNION) return true;

	if (type->canonical->type_kind == TYPE_ERRTYPE) return true;
	if (!type_is_union_struct(type)) return false;

	size_t size = 0;
	Decl **members = type->decl->strukt.members;
	VECEACH(members, i)
	{
		Type *member_type = type_lowering(members[i]->type);
		switch (member_type->type_kind)
		{
			case TYPE_I32:
			case TYPE_U32:
			case TYPE_F32:
			case TYPE_U64:
			case TYPE_I64:
			case TYPE_F64:
				break;
			case TYPE_COMPLEX:
			{
				size_t complex_type_size = type_size(member_type->complex);
				if (complex_type_size != 4 && complex_type_size != 8) return false;
				size += type_size(member_type);
				break;
			}
			default:
				return false;
		}
	}
	return size == type_size(type);
}

static bool x86_try_use_free_regs(GenContext *context, Type *type)
{
	// 1. Floats are not passed in regs on soft floats.
	if (!build_target.x86.use_soft_float && type_is_float(type)) return false;

	unsigned size = type_size(type);

	// 2. If the type is empty, don't use a register.
	if (!size) return false;

	// 3. Calculate the number of registers.
	unsigned size_in_regs = (size + 3) / 4;

	// 4. The MCU psABI allows passing parameters in-reg even if there are
	//    earlier parameters that are passed on the stack. Also,
	//	  it does not allow passing >8-byte structs in-register,
	//	  even if there are 3 free registers available.
	if (build_target.x86.is_mcu_api)
	{
		// 4a. Just return if there are not enough registers.
		if (size_in_regs > context->abi.int_registers) return false;

		// 4b. If the size in regs > 2 then refuse.
		if (size_in_regs > 2) return false;

		// 4c. Use registers, we're fine.
		context->abi.int_registers -= size_in_regs;
		return true;
	}

	// 5. The non-MCU ABI, if we don't have enough registers,
	//    clear them to prevent register use later on.
	if (size_in_regs > context->abi.int_registers)
	{
		context->abi.int_registers = 0;
		return false;
	}

	// 6. Use registers, we're fine.
	context->abi.int_registers -= size_in_regs;
	return true;

}

/**
 * Check if a primitive should be in reg, if so, remove number of free registers.
 * @return true if it should have an inreg attribute, false otherwise.
 */
static bool x86_try_put_primitive_in_reg(GenContext *context, Type *type)
{
	// 1. Try to use regs for this type,
	//    regardless whether we succeed or not, this will update
	//    the number of registers available.
	if (!x86_try_use_free_regs(context, type)) return false;

	// 2. On MCU, do not use the inreg attribute.
	if (build_target.x86.is_mcu_api) return false;

	// 3. Reg/fast/vec calls limit it to 32 bits
	//    and integer / pointer types.
	//    for all other calls we're good to go.
	//    Some questions here though – if we use 3 registers on these
	//    we don't mark it as inreg, however a later register may use a reg.
	//    to get an inreg attribute. Investigate!
	switch (context->abi.call_convention)
	{
		case CALL_CONVENTION_FAST:
		case CALL_CONVENTION_VECTOR:
		case CALL_CONVENTION_REGCALL:
			if (type_size(type) > 4) return false;
			return type_is_integer_kind(type) || type_is_pointer(type);
		default:
			return true;
	}
}

/**
 * Handle the vector/regcalls with HVAs.
 */
static inline ABIArgInfo *x86_classify_homogenous_aggregate(GenContext *context, Type *type, unsigned elements, bool is_vec_call)
{
	// We now know it's a float/double or a vector,
	// since only those are valid for x86
	// see type_is_homogenous_base_type()

	// If we don't have enough SSE registers,
	// just send this by pointer.
	if (context->abi.sse_registers < elements)
	{
		return x86_create_indirect_result(context, type, BY_VAL_SKIP);
	}

	// Use the SSE registers.
	context->abi.sse_registers -= elements;

	// In case of a vector call, pass HVA directly and
	// don't flatten.
	if (is_vec_call)
	{
		ABIArgInfo *info = abi_arg_new(ABI_ARG_DIRECT_COERCE);
		info->attributes.by_reg = true;
		return info;
	}

	// If it is a builtin, then expansion is not needed.
	if (type_is_builtin(type->type_kind) || type->type_kind == TYPE_VECTOR)
	{
		return abi_arg_new_direct();
	}

	// Otherwise just a normal expand.
	return abi_arg_new(ABI_ARG_EXPAND);
}

static inline ABIArgInfo *x86_classify_vector(GenContext *context, Type *type)
{
	unsigned size = type_size(type);

	// On Windows, vectors are passed directly if registers are available, or
	// indirectly if not. This avoids the need to align argument memory. Pass
	// user-defined vector types larger than 512 bits indirectly for simplicity.
	if (build_target.x86.is_win32_float_struct_abi)
	{
		if (size < 64 && context->abi.sse_registers)
		{
			context->abi.sse_registers--;
			return abi_arg_by_reg_attr(abi_arg_new_direct());
		}
		return x86_create_indirect_result(context, type, BY_VAL_SKIP);
	}
	// On Darwin, some vectors are passed in memory, we handle this by passing
	// it as an i8/i16/i32/i64.
	if (build_target.x86.is_darwin_vector_abi)
	{
		if ((size == 1 || size == 2 || size == 4) || (size == 8 && type->vector.len == 1))
		{
			return abi_arg_new_direct_coerce(abi_type_new_int_bits(size * 8));
		}
	}
	// MMX passed as i64
	if (x86_is_mmxtype(type))
	{
		return abi_arg_new_direct_coerce(abi_type_new_int_bits(64));
	}

	// Send as a normal parameter
	return abi_arg_new_direct();
}

/**
 * Handle:
 * error type, struct, union, subarray,
 * string, array, error union, complex.
 */
static inline ABIArgInfo *x86_classify_aggregate(GenContext *context, Type *type)
{
	// Only called for aggregates.
	assert(type_is_abi_aggregate(type));

	// Ignore empty unions / structs on non-win.
	if (!build_target.x86.is_win32_float_struct_abi && type_is_empty_union_struct(type, true))
	{
		return abi_arg_new(ABI_ARG_IGNORE);
	}

	unsigned size = type_size(type);
	bool needs_padding_in_reg = false;

	// Pass over-aligned aggregates on Windows indirectly. This behavior was
	// added in MSVC 2015.
	if (build_target.x86.is_win32_float_struct_abi && type_abi_alignment(type) > 4)
	{
		return x86_create_indirect_result(context, type, BY_VAL_SKIP);
	}

	// See if we can pass aggregates directly.
	// this never happens for MSVC
	if (x86_should_aggregate_use_direct(context, type, &needs_padding_in_reg))
	{
		// Here we coerce the aggregate into a struct { i32, i32, ... }
		// but we do not generate this struct immediately here.
		unsigned size_in_regs = (size + 3) / 4;
		ABIArgInfo *info = abi_arg_new_direct_coerce(abi_type_new_int_bits(32));
		info->direct_coerce.elements = size_in_regs;
		// Not in reg on MCU
		if (!build_target.x86.is_mcu_api) info->attributes.by_reg = true;
		return info;
	}

	// Expand small (<= 128-bit) record types when we know that the stack layout
	// of those arguments will match the struct. This is important because the
	// LLVM backend isn't smart enough to remove byval, which inhibits many
	// optimizations.
	// Don't do this for the MCU if there are still free integer registers
	// (see X86_64 ABI for full explanation).
	if (size <= 16 && (!build_target.x86.is_mcu_api || !context->abi.int_registers) &&
			x86_can_expand_indirect_aggregate_arg(type))
	{
		if (!needs_padding_in_reg) return abi_arg_new(ABI_ARG_EXPAND);

		// This is padded expansion
		ABIArgInfo *info = abi_arg_new_expand_padded(type_int);

		bool is_reg_call = context->abi.call_convention == CALL_CONVENTION_REGCALL;
		bool is_vec_call = context->abi.call_convention == CALL_CONVENTION_VECTOR;
		bool is_fast_call = context->abi.call_convention == CALL_CONVENTION_FAST;

		info->expand.padding_by_reg = is_fast_call || is_reg_call || is_vec_call;
		return info;
	}
	return x86_create_indirect_result(context, type, BY_VAL);
}

/**
 * Pointer / Vararray / int / float / bool
 * @param context
 * @param type
 * @return
 */
static ABIArgInfo *x86_classify_primitives(GenContext *context, Type *type)
{
	// f128 i128 u128 on stack.
	if (type_size(type) > 8) return x86_create_indirect_result(context, type, BY_VAL_SKIP);

	bool in_reg = x86_try_put_primitive_in_reg(context, type);

	if (type_is_promotable_integer(type))
	{
		ABIArgInfo *info = abi_arg_new_direct_int_ext(type);
		info->attributes.by_reg = in_reg;
		return info;
	}

	ABIArgInfo *info = abi_arg_new_direct();
	info->attributes.by_reg = in_reg;
	return info;

}

/**
 * Classify an argument to an x86 function.
 */
static ABIArgInfo *x86_classify_argument(GenContext *context, Type *type)
{
	// FIXME: Set alignment on indirect arguments.

	// We lower all types here first to avoid enums and typedefs.
	type = type_lowering(type);

	bool is_reg_call = context->abi.call_convention == CALL_CONVENTION_REGCALL;
	bool is_vec_call = context->abi.call_convention == CALL_CONVENTION_VECTOR;

	Type *base = NULL;
	unsigned elements = 0;

	// For vec and reg, check if we have a homogenous aggregate.
	if ((is_vec_call || is_reg_call)
		&& type_is_homogenous_aggregate(type, &base, &elements))
	{
		return x86_classify_homogenous_aggregate(context, type, elements, is_vec_call);
	}


	switch (type->type_kind)
	{
		case TYPE_POISONED:
		case TYPE_TYPEDEF:
		case TYPE_VOID:
		case TYPE_ENUM:
		case TYPE_FUNC:
		case TYPE_TYPEID:
			UNREACHABLE
		case ALL_FLOATS:
		case ALL_INTS:
		case TYPE_BOOL:
		case TYPE_VARARRAY:
		case TYPE_POINTER:
			return x86_classify_primitives(context, type);
		case TYPE_VECTOR:
			return x86_classify_vector(context, type);
		case TYPE_ERRTYPE:
		case TYPE_STRUCT:
		case TYPE_UNION:
		case TYPE_SUBARRAY:
		case TYPE_STRING:
		case TYPE_ARRAY:
		case TYPE_ERR_UNION:
		case TYPE_COMPLEX:
			return x86_classify_aggregate(context, type);
		case TYPE_TYPEINFO:
		case TYPE_MEMBER:
			UNREACHABLE
	}
	UNREACHABLE
}

void c_abi_func_create_x86(GenContext *context, FunctionSignature *signature)
{
	context->abi.call_convention = signature->convention;
	context->abi.sse_registers = 0;
	switch (signature->convention)
	{
		case CALL_CONVENTION_NORMAL:
		case CALL_CONVENTION_SYSCALL:
			if (build_target.x86.is_win32_float_struct_abi)
			{
				context->abi.sse_registers = 3;
			}
			context->abi.int_registers = build_target.default_number_regs;
			break;
		case CALL_CONVENTION_REGCALL:
			context->abi.int_registers = 5;
			context->abi.sse_registers = 8;
			break;
		case CALL_CONVENTION_VECTOR:
			context->abi.int_registers = 2;
			context->abi.sse_registers = 6;
			break;
		case CALL_CONVENTION_FAST:
			context->abi.int_registers = 2;
			break;
		default:
			UNREACHABLE
	}
	if (build_target.x86.is_mcu_api)
	{
		context->abi.sse_registers = 0;
		context->abi.int_registers = 3;
	}

	if (signature->failable)
	{
		signature->failable_abi_info = x86_classify_return(context, type_error);
		if (signature->rtype->type->type_kind != TYPE_VOID)
		{
			signature->ret_abi_info = x86_classify_argument(context, type_get_ptr(type_lowering(signature->rtype->type)));
		}
	}
	else
	{
		signature->ret_abi_info = x86_classify_return(context, signature->rtype->type);
	}

	/*
	 * // The chain argument effectively gives us another free register.
  if (FI.isChainCall())
    ++State.FreeRegs;

  // For vectorcall, do a first pass over the arguments, assigning FP and vector
  // arguments to XMM registers as available.
  if (State.CC == llvm::CallingConv::X86_VectorCall)
    runVectorCallFirstPass(FI, State);
	 */

	if (context->abi.call_convention == CALL_CONVENTION_VECTOR)
	{
		FATAL_ERROR("X86 vector call not supported");
	}
	else
	{
		Decl **params = signature->params;
		VECEACH(params, i)
		{
			params[i]->var.abi_info = x86_classify_argument(context, params[i]->type);
		}
	}
}

