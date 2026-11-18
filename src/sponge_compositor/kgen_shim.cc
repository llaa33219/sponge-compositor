/* SPDX-License-Identifier: Apache-2.0
 *
 * kgen_shim.cc — libc-free KGEN runtime seam on Genode::Heap (D16C.5,
 * docs/evidence/mojo-ffi-probe.md).
 *
 * Emitted Mojo objects reach their runtime through KGEN symbols. This
 * shim supplies the documented allocation pair
 * 'KGEN_CompilerRT_AlignedAlloc' / 'KGEN_CompilerRT_AlignedFree' on a
 * 'Genode::Heap', and nothing else — no libc and no full Mojo runtime.
 * The signatures match the pinned Mojo 1.0.0b2 call sites
 * (alignment first, size second; e.g. 'List._realloc' requests
 * 8-byte-aligned blocks). KGEN symbols are version-sensitive: re-verify
 * the undefined-symbol set of the emitted objects on every toolchain
 * bump (plan §6 rule 4).
 *
 * The backing heap is registered once from 'Component::construct' via
 * 'Sponge::kgen_shim_init' (declared here and in main.cc — the fixed
 * adapter file set carries no kgen_shim.h).
 *
 * Every returned block carries its raw-allocation header immediately
 * before the aligned address, so 'AlignedFree' needs no extra lookup.
 */

#include <base/heap.h>
#include <base/stdint.h>

namespace Sponge {

	/* register the heap backing this shim (called once from main.cc) */
	void kgen_shim_init(Genode::Heap &heap);

}  /* namespace Sponge */


namespace {

	/* bookkeeping stored immediately before every aligned block */
	struct Block_header
	{
		void           *raw;
		Genode::size_t  raw_size;
	};

	Genode::Heap *_heap = nullptr;

	bool power_of_two(Genode::size_t value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}
}


void Sponge::kgen_shim_init(Genode::Heap &heap)
{
	_heap = &heap;
}


extern "C" void *KGEN_CompilerRT_AlignedAlloc(Genode::size_t align,
                                              Genode::size_t size)
{
	if (!_heap)
		return nullptr;

	/* alignment must be a power of two */
	if (!power_of_two(align))
		return nullptr;

	/* keep room for the block header in front of the aligned address */
	if (align < alignof(Block_header))
		align = alignof(Block_header);

	if (size == 0)
		size = 1;

	Genode::size_t const raw_size = size + align + sizeof(Block_header);

	void *const raw = _heap->try_alloc(raw_size).convert<void *>(
		[&] (Genode::Allocator::Allocation &a) -> void * {
			a.deallocate = false;
			return a.ptr; },
		[&] (Genode::Alloc_error) -> void * { return nullptr; });

	if (!raw)
		return nullptr;

	Genode::addr_t const base = (Genode::addr_t)raw + sizeof(Block_header);
	Genode::addr_t const aligned =
		(base + align - 1) & ~(Genode::addr_t)(align - 1);

	Block_header *const header = ((Block_header *)aligned) - 1;
	header->raw      = raw;
	header->raw_size = raw_size;

	return (void *)aligned;
}


extern "C" void KGEN_CompilerRT_AlignedFree(void *ptr)
{
	if (!ptr || !_heap)
		return;

	/* only pointers returned by 'KGEN_CompilerRT_AlignedAlloc' are valid */
	Block_header const *const header = ((Block_header const *)ptr) - 1;

	_heap->free(header->raw, header->raw_size);
}
