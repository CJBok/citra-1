// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <tuple>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace Core {

// Sort by time, unless the times are the same, in which case sort by the order added to the queue
bool TimingManager::Event::operator>(const TimingManager::Event& right) const {
    return std::tie(time, fifo_order) > std::tie(right.time, right.fifo_order);
}

bool TimingManager::Event::operator<(const TimingManager::Event& right) const {
    return std::tie(time, fifo_order) < std::tie(right.time, right.fifo_order);
}

TimingManager::TimingManager(std::size_t num_cores) {
    for (std::size_t i = 0; i < num_cores; ++i) {
        timers[i] = std::make_shared<Timer>();
    }
    current_timer = timers[0];
}

TimingEventType* TimingManager::RegisterEvent(const std::string& name, TimedCallback callback) {
    // check for existing type with same name.
    // we want event type names to remain unique so that we can use them for serialization.
    ASSERT_MSG(event_types.find(name) == event_types.end(),
               "CoreTiming Event \"{}\" is already registered. Events should only be registered "
               "during Init to avoid breaking save states.",
               name);

    auto info = event_types.emplace(name, TimingEventType{callback, nullptr});
    TimingEventType* event_type = &info.first->second;
    event_type->name = &info.first->first;
    return event_type;
}

void TimingManager::ScheduleEvent(s64 cycles_into_future, const TimingEventType* event_type,
                                  u64 userdata, std::size_t core_id) {
    ASSERT(event_type != nullptr);
    SharedTimer timer;
    if (core_id == std::numeric_limits<std::size_t>::max()) {
        timer = current_timer;
    } else {
        auto timer_it = timers.find(core_id);
        ASSERT(timer_it != timers.end());
        timer = timer_it->second;
    }

    s64 timeout = timer->GetTicks() + cycles_into_future;
    if (current_timer == timer) {
        // If this event needs to be scheduled before the next advance(), force one early
        if (!timer->is_timer_sane)
            timer->ForceExceptionCheck(cycles_into_future);

        timer->event_queue.emplace_back(
            Event{timeout, timer->event_fifo_id++, userdata, event_type});
        std::push_heap(timer->event_queue.begin(), timer->event_queue.end(), std::greater<>());
    } else {
        timer->ts_queue.Push(Event{static_cast<s64>(timer->GetTicks() + cycles_into_future), 0,
                                   userdata, event_type});
    }
}

void TimingManager::UnscheduleEvent(const TimingEventType* event_type, u64 userdata) {
    for (auto timer : timers) {
        auto itr = std::remove_if(
            timer.second->event_queue.begin(), timer.second->event_queue.end(),
            [&](const Event& e) { return e.type == event_type && e.userdata == userdata; });

        // Removing random items breaks the invariant so we have to re-establish it.
        if (itr != timer.second->event_queue.end()) {
            timer.second->event_queue.erase(itr, timer.second->event_queue.end());
            std::make_heap(timer.second->event_queue.begin(), timer.second->event_queue.end(),
                           std::greater<>());
        }
    }
    // TODO:remove events from ts_queue
}

void TimingManager::RemoveEvent(const TimingEventType* event_type) {
    for (auto timer : timers) {
        auto itr =
            std::remove_if(timer.second->event_queue.begin(), timer.second->event_queue.end(),
                           [&](const Event& e) { return e.type == event_type; });

        // Removing random items breaks the invariant so we have to re-establish it.
        if (itr != timer.second->event_queue.end()) {
            timer.second->event_queue.erase(itr, timer.second->event_queue.end());
            std::make_heap(timer.second->event_queue.begin(), timer.second->event_queue.end(),
                           std::greater<>());
        }
    }
    // TODO:remove events from ts_queue
}

void TimingManager::SetCurrentTimer(std::size_t core_id) {
    current_timer = timers[core_id];
}

s64 TimingManager::GetTicks() const {
    return current_timer->GetTicks();
}

s64 TimingManager::GetGlobalTicks() const {
    return global_timer;
}

std::chrono::microseconds TimingManager::GetGlobalTimeUs() const {
    return std::chrono::microseconds{GetTicks() * 1000000 / BASE_CLOCK_RATE_ARM11};
}

SharedTimer TimingManager::GetTimer(std::size_t cpu_id) {
    return timers[cpu_id];
}

TimingManager::Timer::~Timer() {
    MoveEvents();
}

u64 TimingManager::Timer::GetTicks() const {
    u64 ticks = static_cast<u64>(executed_ticks);
    if (!is_timer_sane) {
        ticks += slice_length - downcount;
    }
    return ticks;
}

void TimingManager::Timer::AddTicks(u64 ticks) {
    downcount -= ticks;
}

u64 TimingManager::Timer::GetIdleTicks() const {
    return static_cast<u64>(idled_cycles);
}

void TimingManager::Timer::ForceExceptionCheck(s64 cycles) {
    cycles = std::max<s64>(0, cycles);
    if (downcount > cycles) {
        slice_length -= downcount - cycles;
        downcount = cycles;
    }
}

void TimingManager::Timer::MoveEvents() {
    for (Event ev; ts_queue.Pop(ev);) {
        ev.fifo_order = event_fifo_id++;
        event_queue.emplace_back(std::move(ev));
        std::push_heap(event_queue.begin(), event_queue.end(), std::greater<>());
    }
}

s64 TimingManager::Timer::GetMaxSliceLength() const {
    auto next_event = std::find_if(event_queue.begin(), event_queue.end(),
                                   [&](const Event& e) { return e.time - executed_ticks > 0; });
    if (next_event != event_queue.end()) {
        return next_event->time - executed_ticks;
    }
    return MAX_SLICE_LENGTH;
}

void TimingManager::Timer::Advance(s64 max_slice_length) {
    MoveEvents();

    s64 cycles_executed = slice_length - downcount;
    idled_cycles = 0;
    executed_ticks += cycles_executed;
    slice_length = max_slice_length;

    is_timer_sane = true;

    while (!event_queue.empty() && event_queue.front().time <= executed_ticks) {
        Event evt = std::move(event_queue.front());
        std::pop_heap(event_queue.begin(), event_queue.end(), std::greater<>());
        event_queue.pop_back();
        evt.type->callback(evt.userdata, executed_ticks - evt.time);
    }

    is_timer_sane = false;

    // Still events left (scheduled in the future)
    if (!event_queue.empty()) {
        slice_length = static_cast<int>(
            std::min<s64>(event_queue.front().time - executed_ticks, max_slice_length));
    }

    downcount = slice_length;
}

void TimingManager::Timer::Idle() {
    idled_cycles += downcount;
    downcount = 0;
}

s64 TimingManager::Timer::GetDowncount() const {
    return downcount;
}

} // namespace Core
