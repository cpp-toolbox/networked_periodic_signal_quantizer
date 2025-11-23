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
 * TODO: we still need a solution for an overflowing client buffer and how we deal with that...
 *
 */
template <typename T> class NetworkedPeriodicSignalQuantizer {
  public:
    explicit NetworkedPeriodicSignalQuantizer(size_t capacity = 10)
        : was_empty_on_last_update(false), received_server_states(10) {}

    std::deque<T> received_server_states;
    Stopwatch empirical_server_signal_stopwatch;

    PeriodicSignal quantized_output_signal{60};
    SignalEmitter output_emitter;

    math_utils::ExponentialMovingAverage average_received_server_states_size;

    /**
     * @brief Push a new state into the buffer.
     */
    void push(const T &item) {
        received_server_states.push_back(item);
        empirical_server_signal_stopwatch.press();
        empirical_server_signal_stopwatch.get_micro_stats();
        // TODO: adjust the quantlized output signal to match the server micro mean period
    }

    /**
     * @brief Call periodically to emit states at the proper rate.
     */
    void update() {
        LogSection _(global_logger, "npsq update");

        average_received_server_states_size.add_sample(static_cast<double>(received_server_states.size()));

        if (quantized_output_signal.enough_time_has_passed()) {
            total_emit_opportunities++;
        }

        if (received_server_states.empty()) {
            if (quantized_output_signal.enough_time_has_passed()) {
                global_logger.debug("would've emitted a signal but the received states was empty, this is suboptimal "
                                    "because we won't be emitting the signal");
                missed_emit_opportunities++;
            }
            was_empty_on_last_update = true;
            return;
        }

        // if buffer had been empty, wait until we have at least 2 elements
        if (was_empty_on_last_update && received_server_states.size() < 2) {
            if (quantized_output_signal.enough_time_has_passed()) {
                global_logger.debug("would've emitted a signal but the received states only has one element after "
                                    "being empty waiting for 2 before we "
                                    "get started emitting again");

                missed_emit_opportunities++;
            }
            return;
        }

        if (quantized_output_signal.process_and_get_signal()) {
            global_logger.debug("emitting now");
            auto first = received_server_states.front();
            output_emitter.emit(first);
            received_server_states.pop_front();
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
