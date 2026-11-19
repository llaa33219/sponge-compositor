/* SPDX-License-Identifier: Apache-2.0
 *
 * event_adapter.cc — thin C++ 'Event' session adapter (G2).
 *
 * See event_adapter.h for the contract. Submitted batches are clamped
 * to the event dataspace capacity and routed through the shared
 * 'Input_pipeline' exactly like driver-side input: real per-session
 * delivery, no drop, no delivery claim beyond what happened.
 */

#include "event_adapter.h"


/************************************
 ** Event_session_component methods **
 ************************************/

Event_session_component::Event_session_component(
	Genode::Env     &env,
	Sponge::Input_pipeline &pipeline,
	Resources const &resources,
	Label     const &label)
:
	Genode::Session_object<Event::Session, Event_session_component>(
		env.ep(), resources, label),
	_env(env),
	_pipeline(pipeline),
	_ram(env.ram(), _ram_quota_guard(), _cap_quota_guard()),
	_ds(_ram, env.rm(), EVENT_DS_SIZE)
{
	Genode::uint8_t *const state = _pipeline.state();

	if (!state ||
	    _pipeline.state_size() < (Genode::uint64_t)SC_SCENE_STORAGE_BYTES) {
		Genode::warning("Event session without a usable scene state block; "
		                "submitted events cannot reach the scene");
		return;
	}

	_mojo_handle = sc_session_open(state, SC_SESSION_EVENT);

	if (_mojo_handle < 0) {
		Genode::warning("sc_session_open(SC_SESSION_EVENT) failed (",
		                _mojo_handle, ")");
		_mojo_handle = -1;
	}
}


Event_session_component::~Event_session_component()
{
	_close_mojo_session();
}


void Event_session_component::_close_mojo_session()
{
	if (_mojo_handle < 0)
		return;

	Genode::uint8_t *const state = _pipeline.state();

	if (!state) {
		_mojo_handle = -1;
		return;
	}

	Genode::int32_t const rc = sc_session_close(state, _mojo_handle);

	if (rc != 0)
		Genode::warning("sc_session_close failed (", rc, ")");

	_mojo_handle = -1;
}


Genode::Dataspace_capability Event_session_component::dataspace()
{
	return _ds.cap();
}


void Event_session_component::submit_batch(unsigned const count)
{
	/*
	 * The batch lives in the shared event dataspace, so the count is
	 * clamped to what the dataspace can actually hold. Every event
	 * within that bound reaches the input pipeline (the same path as
	 * driver-side input) - nothing is dropped.
	 */
	unsigned const capacity =
		EVENT_DS_SIZE / (unsigned)sizeof(Input::Event);

	unsigned const n = (count < capacity) ? count : capacity;

	if (n < count)
		Genode::warning("Event submit_batch: clamped ", count,
		                " event(s) to the dataspace capacity (", n, ")");

	if (n == 0)
		return;

	_pipeline.submit_batch(_ds.local_addr<Input::Event>(), n);
}


/*************************
 ** Event_root methods **
 *************************/

Event_root::Event_root(Genode::Env     &env,
                       Genode::Heap    &heap,
                       Sponge::Input_pipeline &pipeline)
:
	Genode::Root_component<Event_session_component>(env.ep(), heap),
	_env(env),
	_pipeline(pipeline)
{
	if (pipeline.state() == nullptr ||
	    pipeline.state_size() < (Genode::uint64_t)SC_SCENE_STORAGE_BYTES)
		Genode::warning("event_adapter: invalid scene state block; "
		                "Event sessions cannot reach the scene");
}


void Event_root::announce()
{
	_env.parent().announce(_env.ep().manage(*this));
}


Event_root::Create_result
Event_root::_create_session(const char *args, Genode::Affinity const &)
{
	return *new (md_alloc())
		Event_session_component(_env, _pipeline,
		                        Genode::session_resources_from_args(args),
		                        Genode::session_label_from_args(args));
}


void Event_root::_upgrade_session(Event_session_component &session,
                                  const char *args)
{
	session.upgrade(Genode::ram_quota_from_args(args));
	session.upgrade(Genode::cap_quota_from_args(args));
}


void Event_root::_destroy_session(Event_session_component &session)
{
	Genode::destroy(md_alloc(), &session);
}
