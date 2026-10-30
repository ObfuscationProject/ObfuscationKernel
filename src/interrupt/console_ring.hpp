#pragma once

#include "ok/interrupt/platform.hpp"

#include <array>

namespace ok::interrupt
{

// Console bytes a UART receive interrupt's bottom half moved out of the FIFO.
// fill() runs from the bound handler and posts EventSource::console; take()
// runs when the kernel loop serves the console, hands over one byte and posts
// the console again while bytes remain. A full ring leaves the rest in the
// FIFO; take() refills from it once the ring is empty, because an edge line
// does not fire again for bytes that were already waiting. Until the console
// is driven take() reads the UART itself. Boot CPU, interrupts disabled.
template <usize Capacity> class ConsoleRing final
{
  public:
    template <typename Read> void fill(Read read)
    {
        refill(read);
        if (head_ != tail_)
            post_event(EventSource::console);
    }

    template <typename Read> int take(Read read)
    {
        if (head_ == tail_)
        {
            if (!event_driven(EventSource::console))
                return read();
            if (!backlog_)
                return -1;
            refill(read);
            if (head_ == tail_)
                return -1;
        }
        const int value = bytes_[tail_++ % Capacity];
        if (head_ != tail_ || backlog_)
            post_event(EventSource::console);
        return value;
    }

  private:
    template <typename Read> void refill(Read read)
    {
        backlog_ = true;
        while (head_ - tail_ < Capacity)
        {
            const int value = read();
            if (value < 0)
            {
                backlog_ = false;
                return;
            }
            bytes_[head_++ % Capacity] = static_cast<u8>(value);
        }
    }

    std::array<u8, Capacity> bytes_{};
    usize head_{0};
    usize tail_{0};
    bool backlog_{false};
};

} // namespace ok::interrupt
