#ifndef NETWORKED_PERIODIC_SIGNAL_QUANTIZER_HPP
#define NETWORKED_PERIODIC_SIGNAL_QUANTIZER_HPP

#include "sbpt_generated_includes.hpp"
#include <deque>

/*
 * Before networking gets fully involved I want to  preface this with the concept that clocks on two different computers
 * can and most likely have different timescales. If computers time was continuosly synced to some third party in common
 * then many this issue could be gone, but computers still tell the time when there is no network in place either.
 *
 * With that in place we now need to understand how this effects the producer and consumer problem. In a client server
 * setup and supposing that the client is the consumer and the server the producer, then even when both say they're
 * producing and consuming respectively, in reality each one has a different rate.
 *
 * 1. Different time scales
 *
 * For example we could have the situation where the client consumer which rate of 60hz with respect to "perfect time"
 * and a producer that produces at a rate of 59.97hz with respect to "perfect time" this means that eventually the
 * consumer will run out of states and then have to occasionally wait for one extra period to start getting states
 * again. On the other hand if the producer produces at a rate of under 60Hz then it might take two client iterations
 * until there is something to consume again. If the situation was mirrored then instead of running out of states, the
 * client would instead start building up extra states to consume until its buffer is completely filled up.
 *
 * 2. Network Variance
 *
 * Now that we understand these issues, we can add in the over the network component. What this adds is that state being
 * produced will have variance as to when it arrives, if both the client and the server had identical rates (so we're
 * ignoring reality again), then what this means is that the client could go and try and grab some state, and find out
 * that there's no such state because the state was a little later then usual which would lead to a situation where the
 * consumer found nothing. On the flip side if one came in a little earlier it might be the case the client would have
 * two things in the buffer.
 *
 * If we assume that the variance is equal in both directions (both late and early) then we can effectively ignore this
 * variance and just note that will will have moments where there is nothing to consume.
 *
 * 3. Connection Loss
 *
 * Now in the worst case the internet connect could completely die, and depending for how long that occurs the client
 * will most likely run into an empty buffer. There's almost no remedy for this.
 *
 * Solution:
 *
 * In order to manage different timescales (which really just means that the producer and consumer have slightly
 * different production and consume rates). Than if dynamic adjustment of the consumer rate is allowed, then we can
 * monitor the receive rate, and then based on that have the consumer match that rate, which would theoretically solve
 * the different time scales issue. If you can do this then you can assume that the client consume and server production
 * rates match very closely, and error behavior acts more like variance rather than the typical rollover effect that
 * occurs when the consumer and producer run at different rates.
 *
 * In order to manage network variance, we understand that there would theoretically be times when the client consumer
 * tries to grab something and it wouldn't be there. If we first allow the client consumer buffer to build up a few
 * states so that if we try and grab something and it's not there we grab the one that was already there. By doing this
 * we introduce more delay on the client sides consuming which is a big tradeoff but will make sure you don't have a
 * consumer that tries to take from an empty buffer. So the question is how many extra states you'd need, in general I
 * think its fine to wait until the buffer has 2 states (s1, s2) and then the moment that occurs then we start consuming
 * s1, this will immediately drop the number of states in the client buffer back to 1 (s2), now if s3 does not arrive
 * before the next time the client needs to consume, then we will consume s2, we then hope that variance will then make
 * the next one come in sooner or something so that
 *
 * Even if you do the above two things, connection loss or temporarily loss connection will still eventually lead you to
 * having an empty buffer, and in that case the best thing is pretend like you're starting up again in the variance
 * case, so wait until you have 1
 *
 * TODO: we still need a solution for an overflowing client buffer and how we deal with that... Probably if it gets too
 * large we should process or remove a bunch at once to bring it back down.
 *
 */

/**
 * @brief A networked periodic signal quantizer for processing and emitting server states at a controlled rate.
 *
 * This templated class buffers states received from a server and emits them at a regular interval
 * defined by an internal `PeriodicSignal`. It is designed to take incoming server data that is being sent at a fixed
 * rate, but due to network variance might not be received at a steady rate, and act as an adapter that takes this noisy
 * signal and actually emits the data at a fixed frequency by buffering a few elements so there is always something to
 * grab.
 *
 * @warn This does not solve the issue when the internet connection is lost for a few seconds or anything like that, as
 * it only buffers a few elements, in such cases the buffer will be depleted and then the output emitter will output
 * nullopt values in the output emitter
 *
 *
 * @tparam T The type of the server state to be buffered and emitted.
 *
 * @details
 * The class maintains a deque of received states (`received_server_states`) and uses a `Stopwatch`
 * (`empirical_server_signal_stopwatch`) to measure the actual arrival times of these states. It emits
 * quantized output signals via a `SignalEmitter` (`output_emitter`) according to a `PeriodicSignal`
 * (`quantized_output_signal`). The class also tracks whether the buffer was empty on the previous update
 * and computes an exponential moving average of the buffer size for monitoring purposes.
 *
 * Usage:
 * - Call `push()` whenever a new server state arrives.
 * - Call `update()` periodically to process and emit buffered states at the quantized rate.
 * - Use `get_missed_emit_percentage()` and `get_average_received_server_states_size()` for metrics.
 *
 * @note
 * The first pushed state initializes the quantized signal timing.
 */
template <typename T> class NetworkedPeriodicSignalQuantizer {
  public:
    explicit NetworkedPeriodicSignalQuantizer() : was_empty_on_last_update(false) {}

    std::deque<T> received_server_states;

    /** @brief Stopwatch used to measure the timing statistics of when we receive states from the server  */
    Stopwatch received_state_stopwatch;

    /**
     * @brief the clean smooth output signal that is used to drive the emitter
     * @note you can use get get_cycle_progess to see how close we are to the next signal, which can be used for
     * interpolation purposess
     */
    PeriodicSignal output_signal{60};
    /// @brief the emitter which you should bind to receive the states
    SignalEmitter output_emitter;

    bool pushed_first_element = false;
    bool logging_enabled = false;

    math_utils::ExponentialMovingAverage average_received_server_states_size;

    /**
     * @brief Push a new state into the buffer.
     */
    void push(const T &item) {
        LogSection _(global_logger, "npsq push", logging_enabled);

        received_server_states.push_back(item);
        received_state_stopwatch.press();

        if (not pushed_first_element) {
            pushed_first_element = true;
            output_signal.restart();
        }

        global_logger.debug("size is now: {}", received_server_states.size());

        received_state_stopwatch.get_micro_stats();
        // TODO: adjust the quantlized output signal to match the server micro mean period
    }

    /**
     * @brief Call periodically to emit states at the proper rate.
     */
    void update() {
        LogSection _(global_logger, "npsq update", logging_enabled);

        if (not pushed_first_element)
            return;

        average_received_server_states_size.add_sample(static_cast<double>(received_server_states.size()));

        if (output_signal.process_and_get_signal()) {
            total_emit_opportunities++;

            std::optional<T> emitted_value = std::nullopt;

            if (received_server_states.empty()) {
                global_logger.debug("would've emitted a signal but the received states was empty, this is suboptimal "
                                    "because we won't be emitting the signal");
                missed_emit_opportunities++;
                was_empty_on_last_update = true;
            } else if (was_empty_on_last_update && received_server_states.size() < 2) {

                global_logger.debug("would've emitted a signal but the received states only has one element after "
                                    "being empty waiting for 2 before we "
                                    "get started emitting again");

                missed_emit_opportunities++;
            } else {

                auto first = received_server_states.front();
                emitted_value = first;
                received_server_states.pop_front();
                global_logger.debug("size is now: {}", received_server_states.size());
            }
            if (emitted_value == std::nullopt) {
                global_logger.debug("emitting empty");
            } else {
                global_logger.debug("emitting value now");
            }
            global_logger.debug("emitting now");
            output_emitter.emit(emitted_value);
        }
    }

    double get_missed_emit_percentage() const {
        if (total_emit_opportunities == 0)
            return 0.0;
        return (double)missed_emit_opportunities * 100.0 / (double)total_emit_opportunities;
    }

    double get_average_received_server_states_size() const { return average_received_server_states_size.get(); }

  private:
    bool was_empty_on_last_update;
    size_t total_emit_opportunities = 0;  // every time enough_time_has_passed()
    size_t missed_emit_opportunities = 0; // times we wanted to emit but couldn't

    // For average deque size calculation
    size_t running_total_deque_size = 0;
    size_t running_deque_samples = 0;
};

#endif // NETWORKED_PERIODIC_SIGNAL_QUANTIZER_HPP
