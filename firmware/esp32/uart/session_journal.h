#pragma once

#include <string.h>

#include <string>

#include "capture_path.h"
#include "pending_write.h"
#include "sd_file.h"

namespace airrounds {
namespace uart {

enum class JournalFile { open, final };

struct JournalBuild {
    const char* id;
    const char* revision;
};

struct VisitStatistics {
    uint64_t listenStartedUs;
    uint64_t listenEndedUs;
    uint32_t managementFrames;
    uint32_t acceptedBeacons;
    uint32_t queueOverflows;
    uint64_t bytesWritten;
    size_t verifiedFileSize;
};

inline bool sessionJournalPath(char* path, size_t size, const char* directory, const VisitId& id) {
    const int count = snprintf(path, size, "%s/session-%016" PRIx64 "-%08" PRIx32 ".log", directory,
                               id.boot, id.session);
    return count > 0 && static_cast<size_t>(count) < size;
}

namespace detail {

inline size_t formatSessionJournal(char* text, size_t capacity, const SessionReport& report,
                                   const VisitStatistics* stats, JournalBuild build) {
    const auto count = report.status.profile.size();
    const int header =
        snprintf(text, capacity,
                 "format=airrounds-session-v1\nbuild=%s revision=%s protocol=%02x radio=passive\n"
                 "boot=%016" PRIx64 " session=%08" PRIx32 " state=%s outcome=%s points=%zu\n"
                 "started_us=%" PRIu64 " ended_us=%" PRIu64 " budget_us=%" PRIu64 "\n",
                 build.id, build.revision, static_cast<unsigned>(version), report.id.boot,
                 report.id.session, wireName(report.state), wireName(report.outcome), count,
                 report.budget.startedUs, report.endedUs, report.budget.durationUs);
    if (header <= 0 || static_cast<size_t>(header) >= capacity) {
        return 0;
    }
    size_t used = static_cast<size_t>(header);
    for (size_t index = 0; index < count; ++index) {
        const auto& target = report.status.profile[index];
        const auto result = report.status.results[index];
        const auto& point = stats[index];
        char pcap[56];
        const char* filename = "-";
        if (result == Outcome::saved) {
            const auto slot = count == 1 ? 0U : static_cast<unsigned>(index + 1);
            if (!capturePath(pcap, sizeof(pcap), "", {report.id, slot})) {
                return 0;
            }
            filename = pcap + 1;
        }
        const int line = snprintf(
            text + used, capacity - used,
            "point=%zu bssid=%012" PRIx64 " channel=%u outcome=%s"
            " listen_started_us=%" PRIu64 " listen_ended_us=%" PRIu64 " management_frames=%" PRIu32
            " accepted_beacons=%" PRIu32 " filtered_frames=%" PRIu32 " queue_overflows=%" PRIu32
            " bytes_written=%" PRIu64 " verified_file_size=%zu pcap=%s\n",
            index + 1, session::addressValue(target), static_cast<unsigned>(target.channel),
            wireName(result), point.listenStartedUs, point.listenEndedUs, point.managementFrames,
            point.acceptedBeacons, point.managementFrames - point.acceptedBeacons,
            point.queueOverflows, point.bytesWritten, point.verifiedFileSize, filename);
        if (line <= 0 || static_cast<size_t>(line) >= capacity - used) {
            return 0;
        }
        used += static_cast<size_t>(line);
    }
    return used;
}

template <typename File> bool verifyJournal(const char* path, size_t size, const char* expected) {
    File input;
    if (!input.openRead(path)) {
        return false;
    }
    uint8_t buffer[64];
    for (size_t offset = 0; offset < size;) {
        const size_t remaining = size - offset;
        const size_t limit = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const auto read = input.read(buffer, limit);
        if (read <= 0 || static_cast<size_t>(read) > limit ||
            memcmp(buffer, expected + offset, static_cast<size_t>(read)) != 0) {
            return false;
        }
        offset += static_cast<size_t>(read);
    }
    return input.read(buffer, 1) == 0 && input.close();
}

} // namespace detail

template <typename File = storage::SdFile>
bool writeSessionJournal(const char* directory, const SessionReport& report,
                         const VisitStatistics (&stats)[session::Profile::capacity],
                         JournalBuild build, JournalFile kind = JournalFile::final) {
    char filename[48];
    if (!sessionJournalPath(filename, sizeof(filename), "", report.id)) {
        return false;
    }
    const auto final = std::string(directory) + filename;
    const auto open = final + ".open";
    const auto& destination = kind == JournalFile::open ? open : final;
    const auto stage = destination + ".part";
    if (File::probe(final.c_str()) != storage::FilePresence::missing ||
        (kind == JournalFile::open &&
         File::probe(open.c_str()) != storage::FilePresence::missing)) {
        return false;
    }
    char text[2304];
    const size_t size = detail::formatSessionJournal(text, sizeof(text), report, stats, build);
    File output;
    if (size == 0 || !output.openNew(stage.c_str())) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    const bool synced = storage::writeAll(output, bytes, size) && output.sync();
    const bool closed = output.close();
    const bool published = synced && closed &&
                           detail::verifyJournal<File>(stage.c_str(), size, text) &&
                           File::publish(stage.c_str(), destination.c_str());
    if (published && kind == JournalFile::final) {
        File::remove(open.c_str());
    }
    return published;
}

} // namespace uart
} // namespace airrounds
