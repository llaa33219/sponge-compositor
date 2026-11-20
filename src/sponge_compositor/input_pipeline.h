/* SPDX-License-Identifier: Apache-2.0
 *
 * input_pipeline.h — C++ input pipeline and real Event delivery for the
 * Sponge compositor (G2, plan D16C.3).
 *
 * The pipeline is the single input-routing seam in front of the
 * Mojo-owned input core (mojo/input.mojo, abi.h "Hit testing and input
 * delivery"). It flattens every arriving 'Input::Event' into the scalar
 * tuple form of 'sc_input_step', lets the Mojo reducer decide where the
 * event goes, and dispatches every resulting delivery tuple to the
 * registered per-session 'Input::Session_component'. There is no C++
 * scene model and no C++ hit testing here (D16C.10): hit testing,
 * pointer/focus/hover routing, click focus, drag semantics, and the
 * delivery queue are all Mojo-owned. The pipeline only forwards events
 * in, tuples out, and reports what the Mojo state says.
 *
 * Two input sources feed the same 'submit_batch' path:
 *
 *   - the driver side: an optional 'Input::Connection' owned by the
 *     pipeline when the native config carries 'request_input="yes"'
 *     (upstream nitpicker's cascaded-input seam). Host input (e.g. the
 *     PS/2 chain) arrives over that session and is batch-processed on
 *     its signal.
 *   - the 'Event' service: 'Event_session_component::submit_batch'
 *     clamps the batch to its event dataspace capacity and hands the
 *     events to the same pipeline (deterministic injection).
 *
 * Per-session delivery: 'register_session' maps the Mojo session handle
 * ('sc_session_open' handle of the owning Gui session) to that session's
 * public 'Input::Session_component'. Every delivery tuple of
 * 'sc_input_step' is rebuilt into the equivalent 'Input::Event'
 * (tuple words [1..5]: type/x/y/key/codepoint) and submitted to the
 * registered component's real event queue, in tuple (FIFO) order, with
 * no drop. Tuples whose session is not registered are counted and
 * reported with one explicit warning per batch.
 *
 * Focus forwarding: 'focus_forward' wraps 'sc_focus_forward' (the
 * external focus-policy seam of abi.h) and immediately drains the
 * queued leave-before-enter tuples through the same dispatch path, so
 * the delivery is observable without waiting for the next input event.
 * The pipeline therefore never claims a focus change it did not
 * deliver.
 *
 * Reports (upstream nitpicker report parity, public 'os/reporter.h'):
 *
 *   - 'pointer' report with 'xpos'/'ypos' from 'sc_pointer_pos'
 *     (regenerated whenever the position changed),
 *   - 'hover' report with 'label'/'color'/'xpos'/'ypos'/'active'/
 *     'seq_number' (attribute order as upstream
 *     'User_state::report_hovered_view_owner' + 'report_pointer_position').
 *
 * Both reporters are enabled through the upstream configuration shape
 * '<report pointer="yes" hover="yes"/>'.
 *
 * Report-glue state (NOT a second scene model): the hover report needs
 * the hovered session's label and the ABI exposes no hover getter, so
 * the pipeline derives the hovered session from the dispatched tuple
 * stream - a delivered SC_EV_ABS_MOTION marks its session hovered while
 * no button is held and a delivered SC_EV_HOVER_LEAVE clears the mark
 * of that session. The Mojo reducer stays the sole routing authority;
 * this mark exists only to fill the report. Documented G2 divergences:
 * 'color' is the fixed color of the single implicit G2 domain
 * (per-domain colors are not modeled; FIXME: per-session/domain colors
 * once the policy seam exists), 'active="yes"' marks a present hover
 * (upstream gates 'active' on 500 ms motion activity, which needs a
 * timer G2 does not own), and 'seq_number' is the delivery sequence
 * number of the most recent dispatched tuple (the Mojo state's
 * delivery counter, tuple word [6]) so report consumers can
 * order-check deliveries.
 *
 * Integration contract (main.cc): the pipeline is constructed after
 * 'sc_scene_init' and before the roots,
 *
 *   Input_pipeline(Genode::Env &, Genode::uint8_t *state,
 *                  Genode::uint64_t state_size,
 *                  Genode::Node const &config)
 *
 *   ...
 *   Gui_root(..., state, state_size, config, pipeline)
 *   Event_root(env, heap, pipeline)
 *
 * and 'Gui_session_component' registers its Input session at creation
 * (and unregisters at destruction).
 *
 * The pipeline never touches damage state: 'sc_damage_count' remains
 * the Capture adapter's documented non-consuming wakeup-signaling seam
 * (abi.h) and is neither consumed nor cleared here.
 */

#pragma once

/* Genode includes */
#include <base/attached_dataspace.h>
#include <base/env.h>
#include <base/signal.h>
#include <input/component.h>
#include <input_session/connection.h>
#include <os/reporter.h>
#include <util/reconstructible.h>
#include <util/string.h>
#include <base/node.h>

/* local includes */
#include "abi.h"


namespace Sponge {


class Input_pipeline
{
	private:

		Genode::Env &_env;

		/*
		 * Shared Mojo scene-state block (first argument of every ABI
		 * call). Opaque to C++ (abi.h state-storage contract).
		 */
		Genode::uint8_t  *const _state      { nullptr };
		Genode::uint64_t  const _state_size { 0 };

		bool _state_usable() const
		{
			return _state != nullptr &&
			       _state_size >= (Genode::uint64_t)SC_SCENE_STORAGE_BYTES;
		}

		/*
		 * Per-session dispatch registry. 'handle' is the Mojo session
		 * handle of the owning Gui session; 'label' feeds the hover
		 * report. Registration bookkeeping only - no scene state.
		 */
		enum { MAX_SESSIONS = SC_MAX_SESSIONS };

		struct Registered_session
		{
			bool                      used      { false };
			Genode::int32_t           handle    { -1 };
			Input::Session_component *component { nullptr };
			Genode::Session::Label    label     { };

			Registered_session() { }

			Registered_session(Registered_session const &) = delete;
			Registered_session &operator=(Registered_session const &) = delete;
		};

		Registered_session _sessions[MAX_SESSIONS] { };

		Registered_session *_lookup(Genode::int32_t handle);

		/*
		 * Report glue (see the file header): hovered-session mark and
		 * button count derived from the dispatched tuple stream, and
		 * the last dispatched delivery sequence number (tuple word
		 * [6] of the Mojo delivery queue).
		 */
		enum { HOVER_NONE = -1 };

		Genode::int32_t _hovered_handle { HOVER_NONE };
		Genode::int32_t _key_cnt        { 0 };
		bool            _have_seq       { false };
		Genode::int32_t _last_seq       { 0 };

		/* last reported pointer position ('pointer' report change gate) */
		bool _have_reported_pos { false };
		int  _reported_x        { 0 };
		int  _reported_y        { 0 };

		/* last generated hover-report content (change gate) */
		bool            _have_reported_hover { false };
		Genode::int32_t _reported_hover      { HOVER_NONE };
		bool            _reported_have_seq   { false };
		Genode::int32_t _reported_seq        { 0 };

		Genode::Reporter _pointer_reporter { _env, "pointer" };
		Genode::Reporter _hover_reporter   { _env, "hover" };

		/*
		 * Driver-side input back end (upstream nitpicker
		 * 'Input_connection'): owned when the config carries
		 * 'request_input="yes"'.
		 */
		struct Driver_input
		{
			Input_pipeline            &_pipeline;
			Input::Connection          _connection;
			Genode::Attached_dataspace _ev_ds;
			Genode::Signal_handler<Driver_input> _handler;

			void _handle();

			Driver_input(Input_pipeline &pipeline, Genode::Env &env)
			:
				_pipeline(pipeline), _connection(env),
				_ev_ds(env.rm(), _connection.dataspace()),
				_handler(env.ep(), *this, &Driver_input::_handle)
			{
				_connection.sigh(_handler);
			}

			~Driver_input()
			{
				_connection.sigh(Genode::Signal_context_capability());
			}

			/* noncopyable (reference and handler members) */
			Driver_input(Driver_input const &) = delete;
			Driver_input &operator=(Driver_input const &) = delete;
		};

		Genode::Constructible<Driver_input> _driver_input { };

		/* flattened-event submission, one sc_input_step + full drain */
		void _submit_flattened(Genode::int32_t ev_type,
		                       Genode::int32_t a, Genode::int32_t b,
		                       Genode::int32_t c, Genode::int32_t d,
		                       unsigned &lost, unsigned &unsupported);

		/* drain the Mojo delivery queue through sc_input_step */
		void _drain_tuples(unsigned &lost, unsigned &unsupported);

		/* dispatch 'n' tuples from 'buf' to the registered sessions */
		void _dispatch_tuples(Genode::int32_t const *buf,
		                      Genode::int32_t n,
		                      unsigned &lost,
		                      unsigned &unsupported);

		/* generate pointer/hover reports when their content changed */
		void _update_reports();

	public:

		/**
		 * Constructor
		 *
		 * \param env        execution environment (signal dispatch of
		 *                   the optional driver-side input back end)
		 * \param state      Mojo scene state block initialized by
		 *                   'sc_scene_init' (never inspected)
		 * \param state_size block size in bytes (validated against
		 *                   SC_SCENE_STORAGE_BYTES)
		 * \param config     native config node:
		 *                   'request_input="yes"' opens the driver-side
		 *                   'Input::Connection'; the '<report .../>'
		 *                   node enables the 'pointer'/'hover'
		 *                   reporters (attribute names are the report
		 *                   names, as upstream nitpicker)
		 */
		Input_pipeline(Genode::Env           &env,
		               Genode::uint8_t       *state,
		               Genode::uint64_t       state_size,
		               Genode::Node const     &config);

		Input_pipeline(Input_pipeline const &) = delete;
		Input_pipeline &operator=(Input_pipeline const &) = delete;


		/*************************
		 ** Mojo state seam      **
		 *************************/

		Genode::uint8_t  *state() const      { return _state; }
		Genode::uint64_t  state_size() const { return _state_size; }


		/*****************************
		 ** Per-session registration **
		 *****************************/

		/**
		 * Register the per-session Input component of the Gui session
		 * owning the Mojo session 'handle' ('sc_session_open' handle).
		 *
		 * 'label' is the client session label for the hover report;
		 * the 2-argument form registers with an empty label.
		 */
		void register_session(Genode::int32_t handle,
		                      Input::Session_component *component,
		                      Genode::Session::Label const &label =
		                          Genode::Session::Label());

		/**
		 * Drop the registration of 'handle' (session destruction).
		 * Queued Mojo tuples of a closed session are dropped by the
		 * Mojo core on 'sc_session_close'; this clears the dispatch
		 * target so no tuple can reach a destroyed component.
		 */
		void unregister_session(Genode::int32_t handle);


		/*************************
		 ** Input event intake   **
		 *************************/

		/**
		 * Consume 'count' input events and deliver the resulting
		 * per-session event streams.
		 *
		 * Every event is flattened per the 'sc_input_step' contract of
		 * abi.h and reduced by the Mojo core; every resulting delivery
		 * tuple is dispatched immediately (and any queued remainder is
		 * drained), so no delivery is ever lost. Unsupported G2 input
		 * corners (touch/axis/invalid events) are accepted by the ABI
		 * with no state change and no delivery; the batch reports them
		 * with one explicit warning.
		 */
		void submit_batch(Input::Event const *events, unsigned count);


		/*************************
		 ** Focus forwarding     **
		 *************************/

		/**
		 * Forward the input focus to the view 'target' (64-bit view
		 * id) of the Mojo session 'session' ('sc_focus_forward', the
		 * external focus-policy seam of abi.h) and deliver the queued
		 * leave-before-enter tuples right away.
		 */
		void focus_forward(Genode::int32_t session, Genode::uint64_t target);
		void focus_clear();
};


}  /* namespace Sponge */
