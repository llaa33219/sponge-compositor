/* SPDX-License-Identifier: Apache-2.0
 *
 * event_adapter.cc — thin C++ 'Event' session adapter (G1).
 *
 * See event_adapter.h for the contract. The event dataspace is real
 * for eager clients; submitted events are dropped with an explicit
 * warning because event injection is not implemented before G2. No
 * delivery is ever claimed.
 */

#include "event_adapter.h"


/************************************
 ** Event_session_component methods **
 ************************************/

Event_session_component::Event_session_component(
	Genode::Env     &env,
	Genode::uint8_t *state,
	Genode::uint64_t state_size,
	Resources const &resources,
	Label     const &label)
:
	Genode::Session_object<Event::Session, Event_session_component>(
		env.ep(), resources, label),
	_env(env),
	_state(state), _state_size(state_size),
	_ram(env.ram(), _ram_quota_guard(), _cap_quota_guard()),
	_ds(_ram, env.rm(), EVENT_DS_SIZE)
{
	if (!_state_usable()) {
		Genode::warning("not implemented: Event session without a usable "
		                "scene state block (state_size=", state_size,
		                "); submitted events cannot reach the scene");
		return;
	}

	_mojo_handle = sc_session_open(_state, SC_SESSION_EVENT);

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
	if (_mojo_handle < 0 || !_state_usable())
		return;

	Genode::int32_t const rc = sc_session_close(_state, _mojo_handle);

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
	 * G1 delivers no input. The events are dropped and delivery is
	 * not claimed: the RPC cannot report failure, so this warning is
	 * the only honest signal that the batch never reached any view.
	 */
	Genode::warning("not implemented before G2: Event input injection "
	                "(dropped ", count, " event(s); no delivery)");
}


/*************************
 ** Event_root methods **
 *************************/

Event_root::Event_root(Genode::Env     &env,
                       Genode::Heap    &heap,
                       Genode::uint8_t *state,
                       Genode::uint64_t state_size)
:
	Genode::Root_component<Event_session_component>(env.ep(), heap),
	_env(env),
	_state(state), _state_size(state_size)
{
	if (_state == nullptr ||
	    _state_size < (Genode::uint64_t)SC_SCENE_STORAGE_BYTES)
		Genode::warning("event_adapter: invalid scene state block "
		                "(state_size=", state_size,
		                "); Event sessions cannot reach the scene");
}


void Event_root::announce()
{
	_env.parent().announce(_env.ep().manage(*this));
}


Event_root::Create_result
Event_root::_create_session(const char *args, Genode::Affinity const &)
{
	return *new (md_alloc())
		Event_session_component(_env, _state, _state_size,
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
