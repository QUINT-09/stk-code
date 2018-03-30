//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2013 Joerg Henrichs
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "network/rewind_queue.hpp"

#include "config/stk_config.hpp"
#include "modes/world.hpp"
#include "network/network_config.hpp"
#include "network/rewind_info.hpp"
#include "network/rewind_manager.hpp"

#include <algorithm>

/** The RewindQueue stores one TimeStepInfo for each time step done.
 *  The TimeStepInfo stores all states and events to be used at the
 *  given timestep. 
 *  All network events (i.e. new states or client events) are stored in a
 *  separate list m_network_events. At the very start of a new time step
 *  a new TimeStepInfo object is added. Then all network events that are
 *  supposed to happen between t and t+dt are added to this newly added
 *  TimeStep (see mergeNetworkData), and are then being executed.
 *  In case of a rewind the RewindQueue finds the last TimeStepInfo with
 *  a confirmed server state (undoing the events, see undoUntil). Then
 *  the state is restored from the TimeStepInfo object (see replayAllStates)
 *  then the rewind manager re-executes the time steps (using the events
 *  stored at each timestep).
 */
RewindQueue::RewindQueue()
{
    reset();
}   // RewindQueue

// ----------------------------------------------------------------------------
/** Frees all saved state information. Note that the Rewinder data must be
 *  freed elsewhere.
 */
RewindQueue::~RewindQueue()
{
    // This frees all current data
    reset();
}   // ~RewindQueue

// ----------------------------------------------------------------------------
/** Frees all saved state information and all destroyable rewinder.
 */
void RewindQueue::reset()
{
    m_network_events.lock();

    AllNetworkRewindInfo &info = m_network_events.getData();
    for (AllNetworkRewindInfo::const_iterator i  = info.begin(); 
                                              i != info.end(); ++i)
    {
        delete *i;
    }
    m_network_events.getData().clear();
    m_network_events.unlock();

    AllRewindInfo::const_iterator i;
    for (i = m_all_rewind_info.begin(); i != m_all_rewind_info.end(); ++i)
        delete *i;

    m_all_rewind_info.clear();
    m_current = m_all_rewind_info.end();
}   // reset

// ----------------------------------------------------------------------------
/** Inserts a RewindInfo object in the list of all events at the correct time.
 *  If there are several RewindInfo at the exact same time, state RewindInfo
 *  will be insert at the front, and event info at the end of the RewindInfo
 *  with the same time.
 *  \param ri The RewindInfo object to insert.
 *  \param update_current If set, the current pointer will be updated if
 *         necessary to point to the new event
 */
void RewindQueue::insertRewindInfo(RewindInfo *ri)
{
    AllRewindInfo::iterator i = m_all_rewind_info.end();

    while (i != m_all_rewind_info.begin())
    {
        AllRewindInfo::iterator i_prev = i;
        i_prev--;
        // Now test if 'ri' needs to be inserted after the
        // previous element, i.e. before the current element:
        if ((*i_prev)->getTicks() < ri->getTicks()) break;
        if ((*i_prev)->getTicks() == ri->getTicks() &&
            (*i_prev)->isState() && ri->isEvent()      ) break;
        i = i_prev;
    }
    if(m_current == m_all_rewind_info.end())
        m_current = m_all_rewind_info.insert(i, ri);
    else
        m_all_rewind_info.insert(i, ri);  
}   // insertRewindInfo

// ----------------------------------------------------------------------------
/** Adds an event to the rewind data. The data to be stored must be allocated
 *  and not freed by the caller!
 *  \param buffer Pointer to the event data. 
 *  \param ticks Time at which the event happened.
 */
void RewindQueue::addLocalEvent(EventRewinder *event_rewinder,
                                BareNetworkString *buffer, bool confirmed,
                                int ticks                                  )
{
    RewindInfo *ri = new RewindInfoEvent(ticks, event_rewinder,
                                         buffer, confirmed);
    insertRewindInfo(ri);
}   // addLocalEvent

// ----------------------------------------------------------------------------
/** Adds a state from the local simulation to the last created TimeStepInfo
 *  container with the current world time. It is not thread-safe, so needs
 *  to be called from the main thread.
 *  \param rewinder The rewinder object for this state.
 *  \param buffer The state information.
 *  \param confirmed If this state is confirmed to be correct (e.g. is
 *         being received from the servrer), or just a local state for
 *         faster rewinds.
 *  \param ticks Time at which the event happened.
 */
void RewindQueue::addLocalState(Rewinder *rewinder, BareNetworkString *buffer,
                                bool confirmed, int ticks)
{
    RewindInfo *ri = new RewindInfoState(ticks, rewinder, buffer, confirmed);
    assert(ri);
    insertRewindInfo(ri);
}   // addLocalState

// ----------------------------------------------------------------------------
/** Adds an event to the list of network rewind data. This function is
 *  threadsafe so can be called by the network thread. The data is synched
 *  to m_tRewindInformation list by the main thread. The data to be stored
 *  must be allocated and not freed by the caller!
 *  \param buffer Pointer to the event data.
 *  \param ticks Time at which the event happened.
 */
void RewindQueue::addNetworkEvent(EventRewinder *event_rewinder,
                                  BareNetworkString *buffer, int ticks)
{
    RewindInfo *ri = new RewindInfoEvent(ticks, event_rewinder,
                                         buffer, /*confirmed*/true);

    m_network_events.lock();
    m_network_events.getData().push_back(ri);
    m_network_events.unlock();
}   // addNetworkEvent

// ----------------------------------------------------------------------------
/** Adds a state to the list of network rewind data. This function is
 *  threadsafe so can be called by the network thread. The data is synched
 *  to RewindInfo list by the main thread. The data to be stored must be
 *  allocated and not freed by the caller!
 *  \param buffer Pointer to the event data.
 *  \param ticks Time at which the event happened.
 */
void RewindQueue::addNetworkState(Rewinder *rewinder, BareNetworkString *buffer,
                                  int ticks)
{
    RewindInfo *ri = new RewindInfoState(ticks, rewinder,
                                         buffer, /*confirmed*/true);

    m_network_events.lock();
    m_network_events.getData().push_back(ri);
    m_network_events.unlock();
}   // addNetworkState

// ----------------------------------------------------------------------------
/** Merges thread-safe all data received from the network up to and including
 *  the current time (tick) with the current local rewind information.
 *  \param world_ticks[in] Current world time up to which network events will be
 *         merged in.
 *  \param needs_rewind[out] True if a network event/state was received
 *         which was in the past (of this simulation), so a rewind must be
 *         performed.
 *  \param rewind_time[out] If needs_rewind is true, the time to which a rewind
 *         must be performed (at least). Otherwise undefined.
 */
void RewindQueue::mergeNetworkData(int world_ticks, bool *needs_rewind,
                                   int *rewind_ticks)
{
    *needs_rewind = false;
    m_network_events.lock();
    if(m_network_events.getData().empty())
    {
        m_network_events.unlock();
        return;
    }

    // Merge all newly received network events into the main event list.
    // Only a client ever rewinds. So the rewind time should be the latest
    // received state before current world time (if any)
    *rewind_ticks = -9999;
    bool adjust_next = false;

    // FIXME: making m_network_events sorted would prevent the need to 
    // go through the whole list of events
    AllNetworkRewindInfo::iterator i = m_network_events.getData().begin();
    while (i != m_network_events.getData().end())
    {
        // Ignore any events that will happen in the future. The current
        // time step is world_ticks.
        if ((*i)->getTicks() > world_ticks)
        {
            i++;
            continue;
        }
        // A server never rewinds (otherwise we would have to handle 
        // duplicated states, which in the best case would then have
        // a negative effect for every player, when in fact only one
        // player might have a network hickup).
        if (NetworkConfig::get()->isServer() && (*i)->getTicks() < world_ticks)
        {
            Log::warn("RewindQueue", "At %d received message from %d",
                world_ticks, (*i)->getTicks());
            // Server received an event in the past. Adjust this event
            // to be executed 'now' - at least we get a bit closer to the
            // client state.
            (*i)->setTicks(world_ticks);
        }

        insertRewindInfo(*i);

        Log::info("Rewind", "Inserting %s from time %d",
                  (*i)->isEvent() ? "event" : "state",
                  (*i)->getTicks()                       );

        // Check if a rewind is necessary, i.e. a message is received in the
        // past of client (server never rewinds).
        if (NetworkConfig::get()->isClient() && (*i)->getTicks() < world_ticks)
        {
            // We need rewind if we receive an event in the past. This will
            // then trigger a rewind later. Note that we only rewind to the
            // latest event that happened earlier than 'now' - if there is
            // more than one event in the past, we rewind to the last event.
            // Since we restore a state before the rewind, this state will
            // either include the earlier event or the state will be before
            // the earlier event, and the event will be replayed anyway. This
            // makes it easy to handle lost event messages.
            *needs_rewind = true;
            if ((*i)->getTicks() > *rewind_ticks)
                *rewind_ticks = (*i)->getTicks();
        }   // if client and ticks < world_ticks

        i = m_network_events.getData().erase(i);
    }   // for i in m_network_events

    m_network_events.unlock();

}   // mergeNetworkData

// ----------------------------------------------------------------------------
bool RewindQueue::isEmpty() const
{
    return m_current == m_all_rewind_info.end();
}   // isEmpty

// ----------------------------------------------------------------------------
/** Returns true if there is at least one more RewindInfo available.
 */
bool RewindQueue::hasMoreRewindInfo() const
{
    return m_current != m_all_rewind_info.end();
}   // hasMoreRewindInfo

// ----------------------------------------------------------------------------
/** Rewinds the rewind queue and undos all events/states stored. It stops
 *  when the first confirmed state is reached that was recorded before the
 *  undo_time and sets the internal 'current' pointer to this state. 
 *  \param undo_time To what at least events need to be undone.
 *  \return The time in ticks of the confirmed state
 */
int RewindQueue::undoUntil(int undo_ticks)
{
    // m_current points to the next not yet executed event (or state)
    // or end() if nothing else is in the queue
    if (m_current != m_all_rewind_info.begin())
        m_current--;

    do 
    {
        // Undo all events and states from the current time
        (*m_current)->undo();

        if ( (*m_current)->getTicks() <= undo_ticks && 
             (*m_current)->isState() && (*m_current)->isConfirmed() )
        {
            return (*m_current)->getTicks();
        }
        m_current--;
    } while (m_current != m_all_rewind_info.end());

    // Shouldn't happen
    Log::error("RewindManager", "No state for rewind to %d",
               undo_ticks);
    return -1;
}   // undoUntil

// ----------------------------------------------------------------------------
/** Replays all events (not states) that happened at the specified time.
 *  \param ticks Time in ticks.
 */
void RewindQueue::replayAllEvents(int ticks)
{
    // Replay all events that happened at the current time step
    while ( m_current != m_all_rewind_info.end() && 
            (*m_current)->getTicks() == ticks        )
    {
        if ((*m_current)->isEvent())
            (*m_current)->rewind();
        m_current++;
        if (!hasMoreRewindInfo()) break;
    }   // while current->getTIcks == ticks

}   // replayAllEvents

// ----------------------------------------------------------------------------
/** Unit tests for RewindQueue. It tests:
 *  - Sorting order of RewindInfos at the same time (i.e. state before time
 *    before events).
 *  - Sorting order of RewindInfos with different timestamps (and a mixture
 *    of types).
 *  - Special cases that triggered incorrect behaviour previously.
 */
void RewindQueue::unitTesting()
{
    // Some classes need the RewindManager (to register themselves with)
    RewindManager::create();

    // A dummy Rewinder and EventRewinder class since some of the calls being
    // tested here need an instance.
    class DummyRewinder : public Rewinder, public EventRewinder
    {
    public:
        BareNetworkString* saveState() const { return NULL; }
        virtual void undoEvent(BareNetworkString *s) {}
        virtual void rewindToEvent(BareNetworkString *s) {}
        virtual void rewindToState(BareNetworkString *s) {}
        virtual void undoState(BareNetworkString *s) {}
        virtual void undo(BareNetworkString *s) {}
        virtual void rewind(BareNetworkString *s) {}
        virtual void saveTransform() {}
        virtual void computeError() {}
        DummyRewinder() : Rewinder(true) {}
    };
    DummyRewinder *dummy_rewinder = new DummyRewinder();

    // First tests: add a state first, then an event, and make
    // sure the state stays first
    RewindQueue q0;
    assert(q0.isEmpty());
    assert(!q0.hasMoreRewindInfo());

    q0.addLocalState(NULL, NULL, /*confirmed*/true, 0);
    assert(q0.m_all_rewind_info.front()->isState());
    assert(!q0.m_all_rewind_info.front()->isEvent());
    assert(q0.hasMoreRewindInfo());
    assert(q0.undoUntil(0) == 0);

    q0.addNetworkEvent(dummy_rewinder, NULL, 0);
    // Network events are not immediately merged
    assert(q0.m_all_rewind_info.size() == 1);

    bool needs_rewind;
    int rewind_ticks;
    int world_ticks = 0;
    q0.mergeNetworkData(world_ticks, &needs_rewind, &rewind_ticks);
    assert(q0.hasMoreRewindInfo());
    assert(q0.m_all_rewind_info.size() == 2);
    AllRewindInfo::iterator rii = q0.m_all_rewind_info.begin();
    assert((*rii)->isState());
    rii++;
    assert((*rii)->isEvent());

    // Another state must be sorted before the event:
    q0.addNetworkState(dummy_rewinder, NULL, 0);
    assert(q0.hasMoreRewindInfo());
    q0.mergeNetworkData(world_ticks, &needs_rewind, &rewind_ticks);
    assert(q0.m_all_rewind_info.size() == 3);
    rii = q0.m_all_rewind_info.begin();
    assert((*rii)->isState());
    rii++;
    assert((*rii)->isState());
    rii++;
    assert((*rii)->isEvent());

    // Test time base comparisons: adding an event to the end
    q0.addLocalEvent(dummy_rewinder, NULL, true, 4);
    // Then adding an earlier event
    q0.addLocalEvent(dummy_rewinder, NULL, false, 1);
    // rii points to the 3rd element, the ones added just now
    // should be elements4 and 5:
    rii++;
    assert((*rii)->getTicks()==1);
    rii++;
    assert((*rii)->getTicks()==4);

    // Now test inserting an event first, then the state
    RewindQueue q1;
    q1.addLocalEvent(NULL, NULL, true, 5);
    q1.addLocalState(NULL, NULL, true, 5);
    rii = q1.m_all_rewind_info.begin();
    assert((*rii)->isState());
    rii++;
    assert((*rii)->isEvent());

    // Bugs seen before
    // ----------------
    // 1) Current pointer was not reset from end of list when an event
    //    was added and the pointer was already at end of list
    RewindQueue b1;
    b1.addLocalEvent(NULL, NULL, true, 1);
    b1.next();    // Should now point at end of list
    assert(!b1.hasMoreRewindInfo());
    b1.addLocalEvent(NULL, NULL, true, 2);
    RewindInfo *ri = b1.getCurrent();
    assert(ri->getTicks() == 2);
}   // unitTesting
