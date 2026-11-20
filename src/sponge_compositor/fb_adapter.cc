/* SPDX-License-Identifier: Apache-2.0
 *
 * fb_adapter.cc — per-Gui-session Framebuffer::Session adapter for the
 * Sponge compositor. See fb_adapter.h for the contract.
 */

#include "fb_adapter.h"

using namespace Sponge;


bool Fb_session::define_buffer(Framebuffer::Mode mode)
{
	Genode::size_t const num_bytes = mode.num_bytes();

	try {
		/*
		 * Replace the previous buffer wholesale: detach/free the old
		 * dataspace first (the public contract expects the client to
		 * have detached its own mapping before calling 'buffer').
		 */
		_ds.destruct();
		_ds.construct(_env.ram(), _env.rm(), num_bytes);
	}
	catch (Genode::Out_of_ram) {
		Genode::warning("out of RAM allocating ", num_bytes,
		                "-byte client framebuffer");
		return false;
	}
	catch (Genode::Out_of_caps) {
		Genode::warning("out of caps allocating client framebuffer");
		return false;
	}
	catch (Genode::Attached_dataspace::Region_conflict) {
		Genode::error("region conflict attaching client framebuffer");
		return false;
	}

	_mode = mode;

	if (_mode_sigh.valid())
		Genode::Signal_transmitter(_mode_sigh).submit();

	return true;
}


Genode::uint32_t const *Fb_session::pixels() const
{
	if (!_ds.constructed())
		return nullptr;

	return _ds->local_addr<Genode::uint32_t>();
}


Genode::int32_t Fb_session::pitch() const
{
	if (!_ds.constructed())
		return 0;

	/*
	 * Row pitch of the 4-byte pixel layer in bytes. For 'alpha' modes
	 * the alpha and input-mask layers follow the whole pixel layer
	 * ('Framebuffer::Mode'), so the pixel layer keeps this stride.
	 */
	return (Genode::int32_t)(_mode.area.w * 4);
}


Genode::uint8_t const *Fb_session::alpha() const
{
	if (!_ds.constructed() || !_mode.alpha)
		return nullptr;

	/*
	 * The alpha plane follows the whole 4-byte pixel layer
	 * ('Framebuffer::Mode'): base + area.count() * 4.
	 */
	return _ds->local_addr<Genode::uint8_t>() + _mode.area.count()*4;
}


Genode::uint8_t const *Fb_session::input_mask() const
{
	if (!_ds.constructed() || !_mode.alpha)
		return nullptr;

	/* the input-mask plane follows the alpha plane */
	return alpha() + _mode.area.count();
}


Genode::int32_t Fb_session::byte_pitch() const
{
	if (!_ds.constructed() || !_mode.alpha)
		return 0;

	/*
	 * Row pitch of the 1-byte alpha and input-mask planes: one byte
	 * per pixel, so the stride is the mode width in bytes.
	 */
	return (Genode::int32_t)_mode.area.w;
}


Genode::Dataspace_capability Fb_session::dataspace()
{
	if (!_ds.constructed())
		return Genode::Dataspace_capability();

	return _ds->cap();
}


Framebuffer::Mode Fb_session::mode() const
{
	return _mode;
}


void Fb_session::mode_sigh(Genode::Signal_context_capability sigh)
{
	_mode_sigh = sigh;
}


void Fb_session::refresh(Framebuffer::Rect rect)
{
	/*
	 * 'refresh' adds damage: the refreshed region enters the scene's
	 * pending damage through the public ABI. sc_compose re-renders any
	 * pending damage rect correctly from the current scene state; the
	 * full-screen recomposition path of the Capture adapter is what
	 * makes the composed screen exact at G1 (abi.h: "add damage first,
	 * typically the full screen at startup").
	 */
	sc_damage_add(_state,
	              (Genode::int32_t)rect.x1(), (Genode::int32_t)rect.y1(),
	              (Genode::int32_t)rect.w(),  (Genode::int32_t)rect.h());
}


Fb_session::Blit_result Fb_session::blit(Framebuffer::Blit_batch const & /* batch */)
{
	Genode::warning("not implemented before G2: blit");

	/*
	 * Safe no-op result: the call is consumed ('OK') and does
	 * nothing. 'OVERLOADED' would provoke client retry loops.
	 */
	return Blit_result::OK;
}


void Fb_session::panning(Framebuffer::Point /* pos */)
{
	Genode::warning("not implemented before G2: panning");
}


void Fb_session::sync_sigh(Genode::Signal_context_capability sigh)
{
	_sync_sigh = sigh;
}


void Fb_session::submit_sync()
{
	if (_sync_sigh.valid())
		Genode::Signal_transmitter(_sync_sigh).submit();
}


void Fb_session::sync_source(Genode::Session_label const & /* label */)
{
	/* All Gui sessions of this compositor share the same frame source. */
}
