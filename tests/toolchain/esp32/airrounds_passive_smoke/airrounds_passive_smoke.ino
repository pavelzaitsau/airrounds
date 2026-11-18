#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include "passive_session.h"
#include "serial_readback.h"

using airrounds::bench::printHexFile;
using airrounds::session::BeaconDiscovery;
using airrounds::session::PassiveSession;
using airrounds::session::StartResult;
using airrounds::session::Target;
using airrounds::session::VisitState;
using airrounds::storage::pcapReceiptSize;

static constexpr int sdClock = 36;
static constexpr int sdMiso = 37;
static constexpr int sdMosi = 35;
static constexpr int sdSelect = 10;

static PassiveSession* receiver = nullptr;
static BeaconDiscovery* finder = nullptr;
static uint8_t findChannel = 0;
static portMUX_TYPE receiveLock = portMUX_INITIALIZER_UNLOCKED;
static int64_t startedUs = 0;
static bool radioFault = false;

static void onPacket(void* buffer, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT || buffer == nullptr) {
        return;
    }
    const auto* packet = static_cast<const wifi_promiscuous_pkt_t*>(buffer);
    constexpr size_t fcsSize = 4;
    if (packet->rx_ctrl.rx_state != 0 || packet->rx_ctrl.sig_len <= fcsSize) {
        return;
    }

    portENTER_CRITICAL(&receiveLock);
    if (receiver != nullptr) {
        receiver->receive(packet->payload, packet->rx_ctrl.sig_len - fcsSize,
                          packet->rx_ctrl.channel,
                          static_cast<uint64_t>(esp_timer_get_time() - startedUs));
    } else if (finder != nullptr && packet->rx_ctrl.channel == findChannel) {
        finder->receive(packet->payload, packet->rx_ctrl.sig_len - fcsSize, findChannel);
    }
    portEXIT_CRITICAL(&receiveLock);
}

static bool checkRadio(esp_err_t status, const char* operation) {
    if (status == ESP_OK) {
        return true;
    }
    Serial.printf("RADIO_ERROR %s %s\n", operation, esp_err_to_name(status));
    return false;
}

static bool tune(uint8_t channel) {
    uint8_t actualChannel = 0;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    return checkRadio(esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE), "set_channel") &&
           checkRadio(esp_wifi_get_channel(&actualChannel, &secondary), "get_channel") &&
           actualChannel == channel && secondary == WIFI_SECOND_CHAN_NONE;
}

static bool listen(PassiveSession& visit) {
    if (!tune(visit.channel())) {
        return false;
    }
    Serial.printf("TUNED channel=%u\n", static_cast<unsigned>(visit.channel()));
    const int64_t start = esp_timer_get_time();
    portENTER_CRITICAL(&receiveLock);
    startedUs = start;
    receiver = &visit;
    portEXIT_CRITICAL(&receiveLock);
    const bool enabled = checkRadio(esp_wifi_set_promiscuous(true), "enable_receive");
    if (enabled) {
        while (!visit.hasSample() && esp_timer_get_time() - start < PassiveSession::windowUs) {
            delay(1);
        }
    }
    portENTER_CRITICAL(&receiveLock);
    receiver = nullptr;
    portEXIT_CRITICAL(&receiveLock);
    const bool disabled = checkRadio(esp_wifi_set_promiscuous(false), "disable_receive");
    return enabled && disabled;
}

static bool stopRadio(bool started) {
    const bool stopped = !started || checkRadio(esp_wifi_stop(), "stop");
    const bool deinitialized = checkRadio(esp_wifi_deinit(), "deinit");
    radioFault = !stopped || !deinitialized;
    return !radioFault;
}

static bool startRadio() {
    if (radioFault) {
        return false;
    }
    const esp_err_t eventLoop = esp_event_loop_create_default();
    if (eventLoop != ESP_ERR_INVALID_STATE && !checkRadio(eventLoop, "event_loop")) {
        return false;
    }
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = 0;
    if (!checkRadio(esp_wifi_init(&config), "init")) {
        return false;
    }
    const bool started = checkRadio(esp_wifi_set_storage(WIFI_STORAGE_RAM), "ram_settings") &&
                         checkRadio(esp_wifi_set_mode(WIFI_MODE_NULL), "receive_only_mode") &&
                         checkRadio(esp_wifi_start(), "start");
    wifi_promiscuous_filter_t filter{WIFI_PROMIS_FILTER_MASK_MGMT};
    if (started && checkRadio(esp_wifi_set_promiscuous_filter(&filter), "set_filter") &&
        checkRadio(esp_wifi_set_promiscuous_rx_cb(onPacket), "set_callback")) {
        return true;
    }
    stopRadio(started);
    return false;
}

static bool observe(PassiveSession& visit) {
    if (!startRadio()) {
        return false;
    }
    const bool observed = listen(visit);
    return stopRadio(true) && observed;
}

static bool findBeacons(BeaconDiscovery& discovery) {
    wifi_country_t country{};
    if (!checkRadio(esp_wifi_get_country(&country), "get_channels") ||
        !checkRadio(esp_wifi_set_promiscuous(true), "enable_receive")) {
        return false;
    }
    bool success = true;
    const unsigned end = static_cast<unsigned>(country.schan) + country.nchan;
    for (unsigned channel = country.schan; channel < end && channel <= 13; ++channel) {
        if (!tune(static_cast<uint8_t>(channel))) {
            success = false;
            break;
        }
        portENTER_CRITICAL(&receiveLock);
        findChannel = static_cast<uint8_t>(channel);
        finder = &discovery;
        portEXIT_CRITICAL(&receiveLock);
        delay(350);
        portENTER_CRITICAL(&receiveLock);
        finder = nullptr;
        portEXIT_CRITICAL(&receiveLock);
        if (discovery.ambiguous()) {
            break;
        }
    }
    return checkRadio(esp_wifi_set_promiscuous(false), "disable_receive") && success;
}

static void findTarget(const char* name) {
    const size_t size = strlen(name);
    if (size == 0 || size > 32) {
        Serial.println("RESULT invalid_ssid");
        return;
    }
    if (!startRadio()) {
        Serial.println("RESULT radio_error");
        return;
    }
    BeaconDiscovery discovery(name, size);
    const bool received = findBeacons(discovery);
    if (!stopRadio(true) || !received) {
        Serial.println("RESULT radio_error");
        return;
    }
    const Target& target = discovery.target();
    if (discovery.ambiguous()) {
        Serial.println("RESULT ambiguous_ssid");
    } else if (target.channel == 0) {
        Serial.println("RESULT not_observed");
    } else {
        Serial.printf(
            "RESULT found %02x:%02x:%02x:%02x:%02x:%02x %u\n",
            static_cast<unsigned>(target.bssid[0]), static_cast<unsigned>(target.bssid[1]),
            static_cast<unsigned>(target.bssid[2]), static_cast<unsigned>(target.bssid[3]),
            static_cast<unsigned>(target.bssid[4]), static_cast<unsigned>(target.bssid[5]),
            static_cast<unsigned>(target.channel));
    }
}

static void runVisit(const Target* selected, size_t count) {
    PassiveSession visit;
    const StartResult start = visit.start(selected, count);
    if (start != StartResult::started) {
        Serial.println(start == StartResult::empty_list ? "RESULT empty_list"
                                                        : "RESULT invalid_target");
        return;
    }
    if (!observe(visit)) {
        Serial.println("RESULT radio_error");
        return;
    }
    if (!visit.hasSample()) {
        const VisitState result = visit.finish(nullptr);
        Serial.println(result == VisitState::not_observed ? "RESULT not_observed"
                                                          : "RESULT storage_error");
        return;
    }

    SPI.begin(sdClock, sdMiso, sdMosi, sdSelect);
    if (!SD.begin(sdSelect)) {
        SPI.end();
        Serial.println("RESULT storage_error mount");
        return;
    }

    char path[32];
    snprintf(path, sizeof(path), "/sd/t4-%08lx.pcap", static_cast<unsigned long>(esp_random()));
    Serial.printf("FINALIZING %s\n", path);
    const VisitState result = visit.finish(path);
    bool transferred = false;
    if (result == VisitState::saved) {
        const std::string receiptPath = std::string(path) + ".ok";
        transferred = printHexFile(Serial, path, visit.savedPcapSize(), "PCAP") &&
                      printHexFile(Serial, receiptPath.c_str(), pcapReceiptSize, "RECEIPT");
        if (!transferred) {
            Serial.println();
        }
    }
    SD.end();
    SPI.end();
    if (result == VisitState::saved) {
        Serial.printf("RESULT saved %s transfer=%s\n", path, transferred ? "ok" : "error");
    } else {
        Serial.println("RESULT storage_error finalize");
    }
}

static int hexDigit(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

static bool parseTarget(const char* text, Target& target) {
    constexpr size_t addressSize = 17;
    const size_t length = strlen(text);
    if ((length != addressSize + 2 && length != addressSize + 3) || text[addressSize] != ' ') {
        return false;
    }
    for (size_t index = 0; index < sizeof(target.bssid); ++index) {
        const size_t offset = index * 3;
        const int high = hexDigit(text[offset]);
        const int low = hexDigit(text[offset + 1]);
        const char separator = index + 1 == sizeof(target.bssid) ? ' ' : ':';
        if (high < 0 || low < 0 || text[offset + 2] != separator) {
            return false;
        }
        target.bssid[index] = static_cast<uint8_t>(16 * high + low);
    }
    unsigned channel = 0;
    for (size_t offset = addressSize + 1; offset < length; ++offset) {
        if (text[offset] < '0' || text[offset] > '9') {
            return false;
        }
        channel = 10 * channel + static_cast<unsigned>(text[offset] - '0');
    }
    if (channel == 0 || channel > 13) {
        return false;
    }
    target.channel = static_cast<uint8_t>(channel);
    return true;
}

static void runCommand(const char* line) {
    if (strcmp(line, "PING") == 0) {
        Serial.println("READY T4");
    } else if (strncmp(line, "FIND ", 5) == 0) {
        findTarget(line + 5);
    } else if (strcmp(line, "START") == 0) {
        runVisit(nullptr, 0);
    } else if (strncmp(line, "START ", 6) == 0) {
        Target selected{};
        if (parseTarget(line + 6, selected)) {
            runVisit(&selected, 1);
        } else {
            Serial.println("RESULT invalid_target");
        }
    } else {
        Serial.println("RESULT invalid_command");
    }
}

void setup() {
    Serial.begin(115200);
}

void loop() {
    static char line[48];
    static size_t length = 0;
    static bool invalid = false;
    while (Serial.available()) {
        const int value = Serial.read();
        if (value == '\n') {
            if (length > 0 && line[length - 1] == '\r') {
                --length;
            }
            line[length] = '\0';
            if (invalid) {
                Serial.println("RESULT invalid_command");
            } else {
                runCommand(line);
            }
            length = 0;
            invalid = false;
        } else if (value <= 0 || length == sizeof(line) - 1) {
            invalid = true;
        } else if (!invalid) {
            line[length++] = static_cast<char>(value);
        }
    }
    delay(1);
}
