#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "capture_path.h"
#include "passive_session.h"
#include "uart_protocol.h"

namespace airrounds {
namespace uart {

inline uint64_t randomId() {
    const uint64_t high = esp_random();
    return (high << 32U) | esp_random();
}

class PassiveVisit final : public VisitBackend {
public:
    static PassiveVisit& instance() {
        static PassiveVisit visit;
        return visit;
    }

    bool start(const session::Target& target) override {
        if (radioFault_) {
            return false;
        }
        visit_.reset();
        if (visit_.start(&target, 1) != session::StartResult::started || !openRadio()) {
            return false;
        }
        return tuneAndListen(target.channel);
    }

    bool observationDone() const override {
        return visit_.hasSample() ||
               esp_timer_get_time() - startedUs_ >= session::PassiveSession::windowUs;
    }

    bool stopReception() override {
        portENTER_CRITICAL(&receiveLock_);
        listening_ = false;
        portEXIT_CRITICAL(&receiveLock_);
        const bool disabled = esp_wifi_set_promiscuous(false) == ESP_OK;
        closeRadio(true);
        return disabled && !radioFault_;
    }

    void beginSave(const CaptureId& id) override {
        if (!visit_.hasSample()) {
            visit_.finish(nullptr);
            result_.store(Outcome::not_observed, std::memory_order_release);
            return;
        }

        savedVisit_ = id;
        result_.store(Outcome::none, std::memory_order_relaxed);
        if (xTaskCreate(saveTask, "airrounds-save", 8192, this, tskIDLE_PRIORITY + 1, nullptr) !=
            pdPASS) {
            result_.store(Outcome::storage_error, std::memory_order_release);
        }
    }

    Outcome saveResult() const override { return result_.load(std::memory_order_acquire); }

private:
    PassiveVisit() = default;

    static void onPacket(void* buffer, wifi_promiscuous_pkt_type_t type) {
        if (buffer == nullptr || type != WIFI_PKT_MGMT) {
            return;
        }
        const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
        constexpr size_t fcsSize = 4;
        if (packet->rx_ctrl.rx_state != 0 || packet->rx_ctrl.sig_len <= fcsSize) {
            return;
        }
        auto& self = instance();
        portENTER_CRITICAL(&self.receiveLock_);
        if (self.listening_) {
            const auto elapsed = static_cast<uint64_t>(esp_timer_get_time() - self.startedUs_);
            self.visit_.receive(packet->payload, packet->rx_ctrl.sig_len - fcsSize,
                                packet->rx_ctrl.channel, elapsed);
        }
        portEXIT_CRITICAL(&self.receiveLock_);
    }

    bool openRadio() {
        const esp_err_t eventLoop = esp_event_loop_create_default();
        if (eventLoop != ESP_OK && eventLoop != ESP_ERR_INVALID_STATE) {
            return false;
        }
        wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
        config.nvs_enable = 0;
        if (esp_wifi_init(&config) != ESP_OK) {
            return false;
        }
        const bool started = esp_wifi_set_storage(WIFI_STORAGE_RAM) == ESP_OK &&
                             esp_wifi_set_mode(WIFI_MODE_NULL) == ESP_OK &&
                             esp_wifi_start() == ESP_OK;
        wifi_promiscuous_filter_t filter{WIFI_PROMIS_FILTER_MASK_MGMT};
        if (started && esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
            esp_wifi_set_promiscuous_rx_cb(onPacket) == ESP_OK) {
            return true;
        }
        closeRadio(started);
        return false;
    }

    bool tuneAndListen(uint8_t channel) {
        uint8_t actualChannel = 0;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK ||
            esp_wifi_get_channel(&actualChannel, &secondary) != ESP_OK ||
            actualChannel != channel || secondary != WIFI_SECOND_CHAN_NONE) {
            closeRadio(true);
            return false;
        }
        const int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&receiveLock_);
        startedUs_ = now;
        listening_ = true;
        portEXIT_CRITICAL(&receiveLock_);

        if (esp_wifi_set_promiscuous(true) != ESP_OK) {
            stopReception();
            return false;
        }
        return true;
    }

    void closeRadio(bool started) {
        const bool stopped = !started || esp_wifi_stop() == ESP_OK;
        const bool deinitialized = esp_wifi_deinit() == ESP_OK;
        radioFault_ = !stopped || !deinitialized;
    }

    static void saveTask(void* context) {
        auto& self = *static_cast<PassiveVisit*>(context);
        const Outcome result = self.save();
        // Publish after unmounting. The UART loop can then reuse the visit.
        self.result_.store(result, std::memory_order_release);
        vTaskDelete(nullptr);
    }

    Outcome save() {
        char path[56];
        if (!capturePath(path, sizeof(path), "/sd", savedVisit_)) {
            return Outcome::storage_error;
        }

        constexpr int sdClock = 36;
        constexpr int sdMiso = 37;
        constexpr int sdMosi = 35;
        constexpr int sdSelect = 10;
        SPI.begin(sdClock, sdMiso, sdMosi, sdSelect);
        if (!SD.begin(sdSelect)) {
            SPI.end();
            return Outcome::storage_error;
        }

        const auto result = visit_.finish(path);
        SD.end();
        SPI.end();
        return result == session::VisitState::saved ? Outcome::saved : Outcome::storage_error;
    }

    session::PassiveSession visit_;
    portMUX_TYPE receiveLock_ = portMUX_INITIALIZER_UNLOCKED;
    int64_t startedUs_ = 0;
    bool listening_ = false;
    bool radioFault_ = false;
    CaptureId savedVisit_{};
    std::atomic<Outcome> result_{Outcome::none};
};

} // namespace uart
} // namespace airrounds
