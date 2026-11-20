/* SPDX-License-Identifier: Apache-2.0
 *
 * gui_adapter.h — Gui service root and per-session Gui::Session adapter
 * for the Sponge compositor (G2, plan D16C.3).
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
 *   focus()       focus forwarding through the input pipeline
 *                 ('Input_pipeline::focus_forward' -> sc_focus_forward)
 *   info()        real 'Dynamic_rom_session' with the root 'panorama'
 *                 node (xpos/ypos/width/height) plus a capture sub-node,
 *                 both reporting the session's domain rect (the
 *                 '<domain>'/'<policy>' config surface below)
 *
 * Input integration (G2): each session owns a real per-session
 * 'Input::Session_component' (input_adapter.h) and registers it with
 * the shared 'Input_pipeline' at creation, keyed by the Mojo session
 * handle and the client session label. Every routed input event is
 * delivered to that component's event queue (input_pipeline.h); the
 * registration is dropped again at destruction.
 *
 * Focus forwarding ('Gui::Session::focus', the external focus-policy
 * seam): the session capability argument is resolved to its
 * server-side session object and the input focus is forwarded to that
 * session immediately ('sc_focus_forward' + immediate delivery of the
 * queued leave-before-enter tuples). G2 forwards to the target
 * session's first-created view: all views of one session share one
 * Input channel, so the per-session event stream is the same for any
 * representative view choice (per-view focus selection within one
 * session is not modeled). A session without views cannot be focused
 * (explicit warning). An invalid capability (focus reset) is not
 * modeled in G2 (explicit warning).
 *
 * No scene state or logic exists in C++ (abi.h, D16C.10): the session
 * keeps only dataspace/session glue and the client-chosen view-id list
 * needed to bind the session buffer to the session's views on every
 * buffer call. Every scene field, reducer decision, stacking rule,
 * clipping/damage computation, and painter operation is Mojo-owned.
 *
 * The root is a public 'Genode::Root_component' (never 'Static_root'),
 * announced to the parent as 'Gui'. Unsupported surface
 * (child_view/associate/view_capability/release_view_id and the
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
#include <base/node.h>

#include "abi.h"
#include "fb_adapter.h"
#include "input_adapter.h"
#include "input_pipeline.h"

namespace Sponge {


/*
 * A session's domain rectangle: the panorama/capture rect reported by
 * 'Gui_session_component::Info_rom'. 'Gui_root' resolves it from the
 * public '<domain>'/'<policy>'/'<default-policy>' config at session
 * creation, with the width/height alignment semantics of upstream
 * nitpicker's 'Domain_registry::Entry::screen_rect' (see
 * 'Gui_root::_domain_rect_for').
 */
struct Domain_rect
{
	Genode::int32_t x { 0 }, y { 0 }, w { 0 }, h { 0 };
};


class Gui_session_component : public Genode::Session_object<Gui::Session>
{
	private:

		Genode::Env       &_env;
		Genode::uint8_t   *const _state;    /* Mojo scene state block */
		Input_pipeline    &_pipeline;       /* input routing seam (G2) */
		Genode::int32_t    const _handle;   /* scene session handle   */
		Domain_rect        const _domain_rect; /* info-ROM panorama   */

		/*
		 * Active view ids of this session (client-chosen 'View_id'
		 * values, as passed to 'view()'). This is RPC bookkeeping
		 * only: it exists so that 'buffer()' can (re)bind the session
		 * pixel dataspace and any alpha/input-mask planes to every view
		 * through 'sc_view_bind_surfaces', exactly as the buffer-hand-off
		 * contract of abi.h requires, and so that focus forwarding can
		 * pick the session's first-created view. No geometry,
		 * stacking, pixel, or damage state is kept here.
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
		 * xpos/ypos/width/height attributes of the session's domain
		 * rect plus one 'capture' sub-node (the captured screen area).
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

		/* focus forwarding target: first-created view of this session */
		void _forward_scene_focus();

	public:

		/* noncopyable (pointer member, -Werror=effc++) */
		Gui_session_component(Gui_session_component const &) = delete;
		Gui_session_component &operator = (Gui_session_component const &) = delete;

		/**
		 * Constructor
		 *
		 * \param env           execution environment
		 * \param state         Mojo scene state block (never inspected)
		 * \param pipeline      shared input pipeline; this session's
		 *                      Input component is registered there
		 *                      (and unregistered at destruction)
		 * \param scene_handle  handle returned by 'sc_session_open'
		 *                      (owned by this session; closed at
		 *                      destruction)
		 * \param resources     client-donated session resources
		 * \param label         client session label (feeds the input
		 *                      pipeline's hover report)
		 * \param domain_rect   session domain rect for the info ROM,
		 *                      resolved by 'Gui_root' from the
		 *                      '<domain>'/'<policy>' config
		 */
		Gui_session_component(Genode::Env                  &env,
		                      Genode::uint8_t              *state,
		                      Input_pipeline               &pipeline,
		                      Genode::int32_t               scene_handle,
		                      Genode::Session::Resources    const &resources,
		                      Genode::Session::Label        const &label,
		                      Domain_rect             const &domain_rect);

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
		void                 submit_sync();
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
		Input_pipeline      &_pipeline;
		Genode::int32_t      const _screen_w;
		Genode::int32_t      const _screen_h;

		/*
		 * Fixed-capacity domain and policy tables parsed from the
		 * native config (the public '<domain>', '<policy>', and
		 * '<default-policy>' surface of upstream nitpicker, reduced
		 * to what the info-ROM panorama rect needs: no layering,
		 * hover, focus, color, content, or label-visibility state).
		 * Plain member arrays only: no heap and no second scene
		 * model. Overflow and malformed entries drop with loud
		 * warnings.
		 */
		enum { MAX_DOMAINS = 8, MAX_POLICIES = 8 };

		struct Domain
		{
			/* upstream 'Domain_registry::Entry::Origin' values */
			enum class Origin { TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT,
			                    BOTTOM_RIGHT, POINTER };

			using Name = Genode::String<64>;

			bool   used   { false };
			Name   name   { };

			/*
			 * Parsed and kept for config fidelity with upstream;
			 * 'screen_rect' (the info-ROM panorama rect) is
			 * origin-independent exactly like upstream, and the
			 * origin-dependent coordinate translation
			 * ('Domain_registry::Entry::phys_pos') is not part of
			 * the info ROM.
			 */
			Origin origin { Origin::TOP_LEFT };

			/* raw attributes, see '_domain_rect_for' */
			Genode::int32_t xpos   { 0 };
			Genode::int32_t ypos   { 0 };
			Genode::int32_t width  { 0 };
			Genode::int32_t height { 0 };
		};

		struct Policy
		{
			using Selector = Genode::String<160>;

			bool used { false };

			/*
			 * Selector attributes ('label' exact, 'label_prefix'
			 * starts-with, 'label_last' equals the label's last
			 * element). Every present selector must match (upstream
			 * 'Node_label_score' conflict semantics).
			 */
			bool     has_label        { false };
			Selector label            { };
			bool     has_label_prefix { false };
			Selector label_prefix     { };
			bool     has_label_last   { false };
			Selector label_last       { };

			Domain::Name domain { };
		};

		Domain _domains[MAX_DOMAINS] { };
		Policy _policies[MAX_POLICIES] { };
		unsigned _num_domains { 0 };
		unsigned _num_policies { 0 };
		bool           _has_default_policy { false };
		Domain::Name   _default_policy_domain { };

		Domain const *_lookup_domain(Domain::Name const &name) const;

		static Domain::Origin _parse_origin(Genode::Node const &domain);

		void _parse_domains(Genode::Node const &config);
		void _parse_policies(Genode::Node const &config);

		/*
		 * Resolve the session's domain rect: exact 'label', then
		 * 'label_prefix', then 'label_last', then '<default-policy>',
		 * else the full screen.
		 */
		Domain_rect _domain_rect_for(Genode::Session::Label const &label) const;

		Gui_session_component *_sessions[SC_MAX_SESSIONS] { };

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
		 *                   attributes feed the info-ROM panorama;
		 *                   '<domain>'/'<policy>'/'<default-policy>'
		 *                   sub-nodes feed the per-session domain
		 *                   rects)
		 * \param pipeline   shared input pipeline handed to every
		 *                   session (input registration and focus
		 *                   forwarding)
		 */
		Gui_root(Genode::Env &env, Genode::Heap &heap,
		         Genode::uint8_t *state, Genode::uint64_t state_size,
		         Genode::Node const &config,
		         Input_pipeline &pipeline);

		/**
		 * Announce the 'Gui' root to the parent
		 */
		void announce();
		void submit_sync();
};


}  /* namespace Sponge */
