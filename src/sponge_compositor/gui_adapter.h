/* SPDX-License-Identifier: Apache-2.0
 *
 * gui_adapter.h — Gui service root and per-session Gui::Session adapter
 * for the Sponge compositor (G1, plan D16C.3).
 *
 * The Gui session implementation maps the public Gui surface onto the
 * Mojo-owned scene ABI (abi.h):
 *
 *   per session   sc_session_open / sc_session_close
 *   view()        sc_view_create   (+ sc_view_bind_pixels when a
 *   destroy_view() sc_view_destroy    buffer is already defined)
 *   buffer()      real 4-byte pixel dataspace, rebound through
 *                 sc_view_bind_pixels on every buffer call
 *   execute()     the shared C++ 'Command' union is decoded 1:1 into
 *                 sc_apply_command
 *   info()        real 'Dynamic_rom_session' with the root 'panorama'
 *                 node (xpos/ypos/width/height) plus a capture sub-node
 *
 * No scene state or logic exists in C++ (abi.h, D16C.10): the session
 * keeps only dataspace/session glue and the client-chosen view-id list
 * needed to bind the session buffer to the session's views on every
 * buffer call. Every scene field, reducer decision, stacking rule,
 * clipping/damage computation, and painter operation is Mojo-owned.
 *
 * The root is a public 'Genode::Root_component' (never 'Static_root'),
 * announced to the parent as 'Gui'. Unsupported surface
 * (child_view/associate/view_capability/release_view_id/focus and the
 * vestigial title/background command opcodes) degrades to explicit
 * "not implemented before G2" warnings or safe no-op/null results;
 * nothing crashes.
 */

#pragma once

#include <base/attached_ram_dataspace.h>
#include <base/heap.h>
#include <base/session_object.h>
#include <gui_session/capability.h>
#include <gui_session/gui_session.h>
#include <os/dynamic_rom_session.h>
#include <root/component.h>
#include <util/reconstructible.h>
#include <util/xml_node.h>

#include "abi.h"
#include "fb_adapter.h"
#include "input_adapter.h"

namespace Sponge {


class Gui_session_component : public Genode::Session_object<Gui::Session>
{
	private:

		Genode::Env       &_env;
		Genode::uint8_t   *const _state;    /* Mojo scene state block */
		Genode::int32_t    const _handle;   /* scene session handle   */
		Genode::int32_t    const _screen_w; /* info-ROM panorama      */
		Genode::int32_t    const _screen_h;

		/*
		 * Active view ids of this session (client-chosen 'View_id'
		 * values, as passed to 'view()'). This is RPC bookkeeping
		 * only: it exists so that 'buffer()' can (re)bind the session
		 * pixel dataspace to every view of the session through
		 * 'sc_view_bind_pixels', exactly as the buffer-hand-off
		 * contract of abi.h requires. No geometry, stacking, pixel,
		 * or damage state is kept here.
		 */
		enum { MAX_VIEWS = SC_MAX_VIEWS };

		struct View_slot
		{
			bool                used { false };
			Genode::uint64_t    id   { 0 };
		};

		View_slot _views[MAX_VIEWS] { };

		/* the session's virtual framebuffer (client pixel buffer) */
		Fb_session _fb;

		/* the session's Input::Session (eagerly used by Gui::Connection) */
		Input_adapter _input;

		/*
		 * The 'Gui::Session::info' ROM. Root node 'panorama' with the
		 * xpos/ypos/width/height attributes of the screen plus one
		 * 'capture' sub-node (the captured screen area).
		 */
		struct Info_rom : Genode::Dynamic_rom_session::Xml_producer,
		                  Genode::Dynamic_rom_session
		{
			Gui_session_component &_session;

			void produce_xml(Genode::Xml_generator &xml) override;

			Info_rom(Gui_session_component &session);
		};

		Genode::Constructible<Info_rom> _info_rom { };

		/* client-side command buffer (Gui::Session::command_dataspace) */
		Genode::Attached_ram_dataspace _command_ds {
			_env.ram(), _env.rm(), sizeof(Gui::Session::Command_buffer) };

		Gui::Session::Command_buffer &_command_buffer {
			*_command_ds.local_addr<Gui::Session::Command_buffer>() };

		void _register_view(Genode::uint64_t id);
		void _forget_view(Genode::uint64_t id);

		/* bind the session pixel buffer to one view (no-op without buffer) */
		void _bind_view(Genode::uint64_t id);

		/* bind the session pixel buffer to every registered view */
		void _bind_all_views();

		void _execute_command(Gui::Session::Command const &cmd);

	public:

		/* noncopyable (pointer member, -Werror=effc++) */
		Gui_session_component(Gui_session_component const &) = delete;
		Gui_session_component &operator = (Gui_session_component const &) = delete;

		/**
		 * Constructor
		 *
		 * \param env           execution environment
		 * \param state         Mojo scene state block (never inspected)
		 * \param scene_handle  handle returned by 'sc_session_open'
		 *                      (owned by this session; closed at
		 *                      destruction)
		 * \param resources     client-donated session resources
		 * \param label         client session label
		 * \param screen_w      screen width for the info ROM
		 * \param screen_h      screen height for the info ROM
		 */
		Gui_session_component(Genode::Env                  &env,
		                      Genode::uint8_t              *state,
		                      Genode::int32_t               scene_handle,
		                      Genode::Session::Resources    const &resources,
		                      Genode::Session::Label        const &label,
		                      Genode::int32_t               screen_w,
		                      Genode::int32_t               screen_h);

		~Gui_session_component();


		/**************************
		 ** Gui::Session surface **
		 **************************/

		Framebuffer::Session_capability framebuffer() override;
		Input::Session_capability        input() override;
		Info_result                      info() override;

		View_result          view(Gui::View_id, View_attr const &) override;
		Child_view_result    child_view(Gui::View_id, Gui::View_id parent,
		                               View_attr const &) override;
		void                 destroy_view(Gui::View_id) override;
		Associate_result     associate(Gui::View_id, Gui::View_capability) override;
		View_capability_result view_capability(Gui::View_id) override;
		void                 release_view_id(Gui::View_id) override;

		Genode::Dataspace_capability command_dataspace() override;
		void                 execute() override;
		Buffer_result        buffer(Framebuffer::Mode) override;
		void                 focus(Gui::Session_capability) override;
};


/*
 * Gui service root. Public 'Genode::Root_component' (never
 * 'Static_root'): session creation is routed through this root and
 * announced to the parent as 'Gui'.
 */
struct Gui_root : Genode::Root_component<Gui_session_component>
{
	private:

		Genode::Env         &_env;
		Genode::uint8_t     *const _state;
		Genode::int32_t      const _screen_w;
		Genode::int32_t      const _screen_h;

	protected:

		Create_result _create_session(char const *args,
		                              Genode::Affinity const &) override;
		void _upgrade_session(Gui_session_component &, char const *) override;
		void _destroy_session(Gui_session_component &) override;

	public:

		/* noncopyable (pointer member, -Werror=effc++) */
		Gui_root(Gui_root const &) = delete;
		Gui_root &operator = (Gui_root const &) = delete;

		/**
		 * Constructor
		 *
		 * \param env        execution environment
		 * \param heap       allocator for the session objects
		 * \param state      Mojo scene state block shared with the
		 *                   Mojo core (never inspected)
		 * \param state_size block size in bytes; validated together
		 *                   with the block at 'sc_scene_init' time
		 *                   (the session-scoped sc_* calls take the
		 *                   state pointer only)
		 * \param config     native config node ('width'/'height'
		 *                   attributes feed the info-ROM panorama)
		 */
		Gui_root(Genode::Env &env, Genode::Heap &heap,
		         Genode::uint8_t *state, Genode::uint64_t state_size,
		         Genode::Xml_node const &config);

		/**
		 * Announce the 'Gui' root to the parent
		 */
		void announce();
};


}  /* namespace Sponge */
