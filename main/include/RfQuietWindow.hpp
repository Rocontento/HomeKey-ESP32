#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdint>

/**
 * @brief A cooperative "stay off the air" window held while a tap is running.
 *
 * The NFC exchange with a phone is the most power-hungry and the most timing
 * sensitive thing this device does. Auth0 alone is a 112-byte command and an
 * 85-byte answer, roughly 100 ms of continuous 13.56 MHz carrier, and on the
 * common boards the reader shares its 3V3 rail with the ESP32. A WiFi
 * transmit burst landing in the middle of that drops the rail just far enough
 * to corrupt the phone's reply, which the PN532 reports back as a CRC or
 * parity error and which the user experiences as a tap that has to be
 * repeated two or three times.
 *
 * HomeKit and MQTT traffic cannot simply be withheld, but the device talking
 * *about itself* can be. The WebSocket log stream is by far the chattiest
 * thing on the air during a tap -- one frame per log line, some fifteen of
 * them inside 200 ms at DEBUG level -- and none of it is urgent. Holding
 * those frames for the length of the tap and flushing them immediately after
 * costs nothing and takes the bursts off the RF critical path.
 *
 * Deliberately advisory: producers never block, and every wait is bounded, so
 * a window that is somehow never closed degrades to a short delay rather than
 * to silence.
 */
class RfQuietWindow {
public:
    /// Longest a window is ever honoured. A clean tap is ~200 ms and the
    /// link-error retry path stretches it to ~1.2 s; past this something has
    /// gone wrong and deferred traffic must flow again regardless.
    static constexpr uint32_t kMaxWindowMs = 2000;

    /// RAII handle. Nestable, and safe to hold across early returns.
    class Scope {
    public:
        Scope() { open(); }
        ~Scope() { close(); }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;
    };

    /// True while a tap is on the air and the window has not outlived its cap.
    static bool active() {
        if (depth().load(std::memory_order_acquire) == 0) {
            return false;
        }
        const TickType_t age =
            xTaskGetTickCount() - openedAt().load(std::memory_order_relaxed);
        return age * portTICK_PERIOD_MS < kMaxWindowMs;
    }

    /// Park the calling task while a tap is on the air. Bounded: the caller
    /// waits at most kMaxWindowMs and then proceeds anyway.
    static void waitWhileActive() {
        const TickType_t start = xTaskGetTickCount();
        while (active()) {
            if ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS >= kMaxWindowMs) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }

private:
    static std::atomic<uint32_t>& depth() {
        static std::atomic<uint32_t> d{0};
        return d;
    }
    static std::atomic<TickType_t>& openedAt() {
        static std::atomic<TickType_t> t{0};
        return t;
    }
    static void open() {
        if (depth().fetch_add(1, std::memory_order_acq_rel) == 0) {
            openedAt().store(xTaskGetTickCount(), std::memory_order_relaxed);
        }
    }
    static void close() {
        depth().fetch_sub(1, std::memory_order_acq_rel);
    }
};
