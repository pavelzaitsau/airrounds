#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "beacon_mode.h"
#include "capture_path.h"
#include "eapol_mode.h"
#include "pcap_record.h"
#include "session_journal.h"
#include "uart_protocol.h"

#ifndef AIRROUNDS_BUILD_ID
#define AIRROUNDS_BUILD_ID unidentified
#endif
#ifndef AIRROUNDS_REVISION
#define AIRROUNDS_REVISION unknown
#endif

#define AIRROUNDS_STRING_VALUE(value) #value
#define AIRROUNDS_STRING(value) AIRROUNDS_STRING_VALUE(value)

namespace airrounds {
namespace uart {

inline uint64_t randomId() {
    const uint64_t high = esp_random();
    return (high << 32U) | esp_random();
}

class PassiveVisit final : public VisitBackend, public ScanBackend {
public:
    static PassiveVisit& instance() {
        static PassiveVisit visit;
        return visit;
    }

    uint64_t nowUs() const override { return static_cast<uint64_t>(esp_timer_get_time()); }

    // Point the capture pipeline at the round's mode. The wire IDs match the CaptureModeId values.
    void selectMode(uint8_t modeId) override {
        mode_ = modeId == eapolModeId ? static_cast<session::CaptureMode*>(&eapol_)
                                      : static_cast<session::CaptureMode*>(&beacon_);
    }

    void startScan() override {
        scanList_.clear();
        scanState_ = ScanState::error;
        if (radioFault_ || !openRadio(storage::managementFrames)) {
            return;
        }
        wifi_country_t country{};
        if (esp_wifi_get_country(&country) != ESP_OK || country.schan == 0 || country.schan > 13 ||
            country.nchan == 0) {
            closeRadio(true);
            return;
        }
        const auto last = static_cast<unsigned>(country.schan) + country.nchan - 1U;
        scanLastChannel_ = static_cast<uint8_t>(last > 13 ? 13 : last);
        if (tuneScan(country.schan) && esp_wifi_set_promiscuous(true) == ESP_OK) {
            scanState_ = ScanState::running;
        } else {
            finishScan(ScanState::error);
        }
    }

    void pollScan() override {
        if (scanState_ != ScanState::running || nowUs() < scanDeadline_) {
            return;
        }
        const auto current = scanChannel_;
        if (current == scanLastChannel_) {
            finishScan(ScanState::complete);
        } else if (!tuneScan(static_cast<uint8_t>(current + 1U))) {
            finishScan(ScanState::error);
        }
    }

    void stopScan() override { finishScan(ScanState::idle); }
    ScanState scanState() const override { return scanState_; }
    const ScanList& scanList() const override { return scanList_; }

    void beginRound(const SessionReport& report) override {
        pointCount_ = report.status.profile.size();
        for (auto& point : statistics_) {
            point = {};
        }
        scheduleJournal(report, JournalFile::open);
    }

    VisitStart start(const session::Target& target, const VisitBudget& budget) override {
        currentPoint_ = pointCount_ - budget.remainingVisits;
        if (budget.round.remainingAt(nowUs()) == 0) {
            return VisitStart::deadline;
        }
        if (radioFault_ || !openRadio(mode_->needs().frameTypes)) {
            return VisitStart::radio_error;
        }
        mode_->reset();
        return tuneAndListen(target, budget);
    }

    bool observationDone() const override {
        portENTER_CRITICAL(&receiveLock_);
        const bool done = mode_->done(nowUs() - startedUs_);
        portEXIT_CRITICAL(&receiveLock_);
        return done;
    }

    bool stopReception() override {
        portENTER_CRITICAL(&receiveLock_);
        if (listening_) {
            statistics_[currentPoint_].listenEndedUs = nowUs();
        }
        listening_ = false;
        portEXIT_CRITICAL(&receiveLock_);
        const bool disabled = esp_wifi_set_promiscuous(false) == ESP_OK;
        closeRadio(true);
        return disabled && !radioFault_;
    }

    void beginSave(const CaptureId& id) override {
        switch (mode_->finish()) {
        case session::ModeResult::found:
            break;
        case session::ModeResult::error:
            result_.store(Outcome::radio_error, std::memory_order_release);
            return;
        case session::ModeResult::absence_confirmed:
        case session::ModeResult::not_observed:
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

    void beginJournal(const SessionReport& report) override {
        scheduleJournal(report, JournalFile::final);
    }

    bool journalPending() const override { return journalPending_.load(std::memory_order_acquire); }
    bool supportsJournal() const override { return true; }
    JournalState journalState() const override {
        return journalState_.load(std::memory_order_acquire);
    }

private:
    PassiveVisit() = default;

    void scheduleJournal(const SessionReport& report, JournalFile kind) {
        journalReport_ = report;
        journalFile_ = kind;
        journalState_.store(JournalState::pending, std::memory_order_relaxed);
        journalPending_.store(true, std::memory_order_relaxed);
        if (xTaskCreate(journalTask, "airrounds-log", 8192, this, tskIDLE_PRIORITY + 1, nullptr) !=
            pdPASS) {
            completeJournal(JournalState::failed);
            log_w("AirRounds session log task did not start");
        }
    }

    void scanChannel(uint8_t channel) {
        portENTER_CRITICAL(&receiveLock_);
        scanChannel_ = channel;
        portEXIT_CRITICAL(&receiveLock_);
    }

    bool tuneScan(uint8_t channel) {
        scanChannel(0);
        if (!tune(channel)) {
            return false;
        }
        scanChannel(channel);
        scanDeadline_ = nowUs() + 180000;
        return true;
    }

    void finishScan(ScanState state) {
        scanChannel(0);
        const bool disabled = esp_wifi_set_promiscuous(false) == ESP_OK;
        closeRadio(true);
        scanState_ = disabled && !radioFault_ ? state : ScanState::error;
    }

    void observeNetwork(const wifi_promiscuous_pkt_t& packet, size_t size) {
        session::BeaconView beacon;
        if (!session::parseBeacon(scanChannel_, packet.payload, size, beacon)) {
            return;
        }
        session::Target target{{}, scanChannel_};
        memcpy(target.bssid, beacon.bssid, sizeof(target.bssid));
        scanList_.observe(target, static_cast<int8_t>(packet.rx_ctrl.rssi), beacon.ssid,
                          beacon.ssidSize);
    }

    static void onPacket(void* buffer, wifi_promiscuous_pkt_type_t type) {
        const uint8_t frameType = frameTypeOf(type);
        if (buffer == nullptr || frameType == 0) {
            return;
        }
        const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
        constexpr size_t fcsSize = 4;
        auto& self = instance();
        portENTER_CRITICAL(&self.receiveLock_);
        if (self.scanChannel_ != 0 && frameType == storage::managementFrames &&
            self.scanChannel_ == packet->rx_ctrl.channel && packet->rx_ctrl.rx_state == 0 &&
            packet->rx_ctrl.sig_len > fcsSize) {
            self.observeNetwork(*packet, packet->rx_ctrl.sig_len - fcsSize);
        } else if (self.listening_) {
            auto& point = self.statistics_[self.currentPoint_];
            // Count the mode's declared frame type, so the journal does not label a data count as
            // management and filtered_frames (observed - accepted) never underflows.
            if ((frameType & self.mode_->needs().frameTypes) != 0) {
                ++point.observedFrames;
            }
            if (packet->rx_ctrl.rx_state == 0 && packet->rx_ctrl.sig_len > fcsSize &&
                self.mode_->onFrame(packet->payload, packet->rx_ctrl.sig_len - fcsSize,
                                    packet->rx_ctrl.channel, self.nowUs() - self.startedUs_)) {
                ++point.acceptedFrames;
            }
        }
        portEXIT_CRITICAL(&self.receiveLock_);
    }

    static uint8_t frameTypeOf(wifi_promiscuous_pkt_type_t type) {
        switch (type) {
        case WIFI_PKT_MGMT:
            return storage::managementFrames;
        case WIFI_PKT_CTRL:
            return storage::controlFrames;
        case WIFI_PKT_DATA:
            return storage::dataFrames;
        default:
            return 0;
        }
    }

    // The receive filter passes only the frame types the scan or the capture mode declared.
    static uint32_t receiveFilter(uint8_t frameTypes) {
        uint32_t mask = 0;
        if ((frameTypes & storage::managementFrames) != 0) {
            mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
        }
        if ((frameTypes & storage::controlFrames) != 0) {
            mask |= WIFI_PROMIS_FILTER_MASK_CTRL;
        }
        if ((frameTypes & storage::dataFrames) != 0) {
            mask |= WIFI_PROMIS_FILTER_MASK_DATA;
        }
        return mask;
    }

    bool openRadio(uint8_t frameTypes) {
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
        wifi_promiscuous_filter_t filter{receiveFilter(frameTypes)};
        // Control frames pass a second, per-subtype filter; ask for every subtype explicitly.
        wifi_promiscuous_filter_t controlFilter{WIFI_PROMIS_CTRL_FILTER_MASK_ALL};
        if (started && filter.filter_mask != 0 &&
            esp_wifi_set_promiscuous_filter(&filter) == ESP_OK &&
            ((frameTypes & storage::controlFrames) == 0 ||
             esp_wifi_set_promiscuous_ctrl_filter(&controlFilter) == ESP_OK) &&
            esp_wifi_set_promiscuous_rx_cb(onPacket) == ESP_OK) {
            return true;
        }
        closeRadio(started);
        return false;
    }

    static bool tune(uint8_t channel) {
        uint8_t actualChannel = 0;
        wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
        return esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) == ESP_OK &&
               esp_wifi_get_channel(&actualChannel, &secondary) == ESP_OK &&
               actualChannel == channel && secondary == WIFI_SECOND_CHAN_NONE;
    }

    VisitStart tuneAndListen(const session::Target& target, const VisitBudget& budget) {
        if (!tune(target.channel)) {
            closeRadio(true);
            return VisitStart::radio_error;
        }
        if (budget.round.remainingAt(nowUs()) == 0) {
            closeRadio(true);
            return radioFault_ ? VisitStart::radio_error : VisitStart::deadline;
        }
        if (esp_wifi_set_promiscuous(true) != ESP_OK) {
            stopReception();
            return VisitStart::radio_error;
        }

        // The callback ignores frames until setup finishes and the window is armed.
        portENTER_CRITICAL(&receiveLock_);
        const auto now = nowUs();
        const auto shareUs = budget.shareAt(now);
        const bool started = shareUs != 0 && mode_->begin(target, shareUs);
        if (started) {
            startedUs_ = now;
            statistics_[currentPoint_].listenStartedUs = now;
            listening_ = true;
        }
        portEXIT_CRITICAL(&receiveLock_);
        if (!started) {
            const bool stopped = stopReception();
            return shareUs == 0 && stopped ? VisitStart::deadline : VisitStart::radio_error;
        }
        return VisitStart::listening;
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

    static bool mountCard() {
        constexpr int sdClock = 36;
        constexpr int sdMiso = 37;
        constexpr int sdMosi = 35;
        constexpr int sdSelect = 10;
        SPI.begin(sdClock, sdMiso, sdMosi, sdSelect);
        if (SD.begin(sdSelect)) {
            return true;
        }
        SPI.end();
        return false;
    }

    static void unmountCard() {
        SD.end();
        SPI.end();
    }

    Outcome save() {
        char path[56];
        if (!capturePath(path, sizeof(path), "/sd", savedVisit_) || !mountCard()) {
            return Outcome::storage_error;
        }
        const auto receipt = storage::writePcapRecords(path, *mode_, mode_->needs().frameTypes);
        auto& point = statistics_[currentPoint_];
        point.bytesWritten = receipt.bytesWritten;
        point.queueOverflows = receipt.queueOverflows;
        point.verifiedFileSize = receipt.saved ? storage::pcapFileSize(*mode_) : 0;
        unmountCard();
        return receipt.saved ? Outcome::saved : Outcome::storage_error;
    }

    static void journalTask(void* context) {
        auto& self = *static_cast<PassiveVisit*>(context);
        bool saved = false;
        if (mountCard()) {
            saved = writeSessionJournal(
                "/sd", self.journalReport_, self.statistics_,
                {AIRROUNDS_STRING(AIRROUNDS_BUILD_ID), AIRROUNDS_STRING(AIRROUNDS_REVISION)},
                self.journalFile_);
            unmountCard();
        }
        if (!saved) {
            log_w("AirRounds session log unavailable");
        }
        const auto ready =
            self.journalFile_ == JournalFile::open ? JournalState::pending : JournalState::saved;
        self.completeJournal(saved ? ready : JournalState::failed);
        vTaskDelete(nullptr);
    }

    void completeJournal(JournalState state) {
        journalState_.store(state, std::memory_order_release);
        journalPending_.store(false, std::memory_order_release);
    }

    static_assert(beaconModeId == static_cast<uint8_t>(session::CaptureModeId::beacon),
                  "the Beacon wire ID must equal its CaptureModeId value");
    static_assert(eapolModeId == static_cast<uint8_t>(session::CaptureModeId::eapol),
                  "the EAPOL wire ID must equal its CaptureModeId value");

    session::BeaconMode beacon_;
    session::EapolMode eapol_;
    session::CaptureMode* mode_ = &beacon_;
    mutable portMUX_TYPE receiveLock_ = portMUX_INITIALIZER_UNLOCKED;
    uint64_t startedUs_ = 0;
    bool listening_ = false;
    bool radioFault_ = false;
    ScanList scanList_;
    ScanState scanState_ = ScanState::idle;
    uint8_t scanChannel_ = 0;
    uint8_t scanLastChannel_ = 0;
    uint64_t scanDeadline_ = 0;
    CaptureId savedVisit_{};
    std::atomic<Outcome> result_{Outcome::none};
    VisitStatistics statistics_[session::Profile::capacity]{};
    size_t pointCount_ = 0;
    size_t currentPoint_ = 0;
    SessionReport journalReport_{};
    JournalFile journalFile_ = JournalFile::open;
    std::atomic<JournalState> journalState_{JournalState::none};
    std::atomic<bool> journalPending_{false};
};

} // namespace uart
} // namespace airrounds

#undef AIRROUNDS_STRING
#undef AIRROUNDS_STRING_VALUE
