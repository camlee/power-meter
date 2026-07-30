#include "live_websocket_service.h"

#include <Arduino.h>
#include <cstddef>
#include <esp_http_server.h>
#include <math.h>

#include "device/device_state.h"
#include "sensors/sensors.h"
#include "time/time_service.h"

namespace live_websocket_service {
namespace {

constexpr uint16_t kPort = 81;
constexpr uint32_t kIntervalMs = 500;
constexpr size_t kMaxClients = 5;
constexpr size_t kReplayFrameCount = 60; // 30 seconds at the 2 Hz publish rate.
constexpr uint8_t kReplayFramesPerWork = 20;
constexpr uint32_t kLiveMagicV4 = 0x344d5056; // "VPM4" little endian
constexpr uint32_t kLiveMagicV5 = 0x354d5056; // "VPM5" little endian

// Replay remains on the compact logical-only V4 wire layout. Physical
// diagnostics have no chart-history consumer, so retaining them in all 60
// replay slots would duplicate data and permanently spend internal RAM.
struct __attribute__((packed)) LiveReplayFrameV4 {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t sequence;
    uint32_t stateRevision;
    uint32_t uptimeMs;
    uint32_t sessionId;
    double unixMs;
    float inVoltage;
    float inCurrent;
    float inPower;
    float outVoltage;
    float outCurrent;
    float outPower;
    float auxVoltage;
    float auxCurrent;
    float auxPower;
    float netBatteryPower;
    float inDuty;
    float outDuty;
    float auxDuty;
};
static_assert(sizeof(LiveReplayFrameV4) == 84,
              "web live protocol V4 frame changed");

// Current V5 frames append physical Sensor 1/2/3 diagnostics. The first 84
// bytes intentionally retain the V4 field offsets, with only magic/version
// changed. Three reserved bytes align the float arrays on a four-byte offset.
struct __attribute__((packed)) LiveFrameV5 {
    LiveReplayFrameV4 logical;
    uint16_t physicalFlags;
    uint8_t physicalStates[sensors::SENSOR_COUNT];
    uint8_t reserved[3];
    float physicalVoltage[sensors::SENSOR_COUNT];
    float physicalCurrent[sensors::SENSOR_COUNT];
    float physicalPower[sensors::SENSOR_COUNT];
};
static_assert(sizeof(LiveFrameV5) == 128,
              "web live protocol V5 frame changed");
static_assert(offsetof(LiveFrameV5, physicalFlags) == 84,
              "web live protocol V5 physical flags moved");
static_assert(offsetof(LiveFrameV5, physicalVoltage) == 92,
              "web live protocol V5 physical values moved");
static_assert(sensors::SENSOR_COUNT == 3,
              "web live protocol V5 requires three physical sensors");
static_assert(static_cast<uint8_t>(sensors::ReadingState::NotConfigured) == 0 &&
              static_cast<uint8_t>(sensors::ReadingState::Waiting) == 1 &&
              static_cast<uint8_t>(sensors::ReadingState::Valid) == 2 &&
              static_cast<uint8_t>(sensors::ReadingState::OutOfRange) == 3 &&
              static_cast<uint8_t>(sensors::ReadingState::Invalid) == 4 &&
              static_cast<uint8_t>(sensors::ReadingState::Stale) == 5,
              "web live protocol V5 reading-state codes changed");

struct Client {
    int fd = -1;
    uint8_t replayIndex = 0;
    uint8_t replayRemaining = 0;
};

httpd_handle_t server = nullptr;
Client clients[kMaxClients];
LiveReplayFrameV4 replayFrames[kReplayFrameCount] = {};
uint8_t replayStart = 0;
uint8_t replayCount = 0;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastPublishMs = 0;
uint32_t sequence = 0;
volatile bool sendQueued = false;
LiveFrameV5 pendingFrame{};

void rejectClient(void* arg) {
    const int fd = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    constexpr char message[] = "limit:5";
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(message));
    frame.len = sizeof(message) - 1;
    if (httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_send_frame_async(server, fd, &frame);
    }
    httpd_sess_trigger_close(server, fd);
}

void removeClient(int fd) {
    portENTER_CRITICAL(&stateMux);
    for (Client& client : clients) if (client.fd == fd) client = {};
    portEXIT_CRITICAL(&stateMux);
}

bool addClient(int fd) {
    portENTER_CRITICAL(&stateMux);
    for (Client& client : clients) {
        if (client.fd == fd) { portEXIT_CRITICAL(&stateMux); return true; }
    }
    for (Client& client : clients) {
        if (client.fd < 0) {
            client.fd = fd;
            client.replayIndex = replayStart;
            client.replayRemaining = replayCount;
            portEXIT_CRITICAL(&stateMux);
            return true;
        }
    }
    portEXIT_CRITICAL(&stateMux);
    return false;
}

bool hasClients() {
    portENTER_CRITICAL(&stateMux);
    bool found = false;
    for (const Client& client : clients) found |= client.fd >= 0;
    portEXIT_CRITICAL(&stateMux);
    return found;
}

bool hasReplayPending() {
    portENTER_CRITICAL(&stateMux);
    bool pending = false;
    for (const Client& client : clients) pending |= client.fd >= 0 && client.replayRemaining;
    portEXIT_CRITICAL(&stateMux);
    return pending;
}

bool nextReplayFrame(size_t slot, int& fd, LiveReplayFrameV4& frame) {
    portENTER_CRITICAL(&stateMux);
    Client& client = clients[slot];
    if (client.fd < 0 || !client.replayRemaining) {
        portEXIT_CRITICAL(&stateMux);
        return false;
    }
    fd = client.fd;
    frame = replayFrames[client.replayIndex];
    client.replayIndex = static_cast<uint8_t>((client.replayIndex + 1) % kReplayFrameCount);
    --client.replayRemaining;
    portEXIT_CRITICAL(&stateMux);
    return true;
}

bool clientReadyForLive(size_t slot, int& fd) {
    portENTER_CRITICAL(&stateMux);
    const Client& client = clients[slot];
    const bool ready = client.fd >= 0 && !client.replayRemaining;
    if (ready) fd = client.fd;
    portEXIT_CRITICAL(&stateMux);
    return ready;
}

void recordReplayFrame(const LiveReplayFrameV4& frame) {
    portENTER_CRITICAL(&stateMux);
    const uint8_t slot = static_cast<uint8_t>((replayStart + replayCount) % kReplayFrameCount);
    replayFrames[slot] = frame;
    if (replayCount < kReplayFrameCount) ++replayCount;
    else replayStart = static_cast<uint8_t>((replayStart + 1) % kReplayFrameCount);
    portEXIT_CRITICAL(&stateMux);
}

bool sendFrame(int fd, const void* data, size_t size) {
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<void*>(data));
    frame.len = size;
    return httpd_ws_get_fd_info(server, fd) == HTTPD_WS_CLIENT_WEBSOCKET &&
           httpd_ws_send_frame_async(server, fd, &frame) == ESP_OK;
}

esp_err_t websocketHandler(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        if (!addClient(fd)) {
            if (httpd_queue_work(server, rejectClient, reinterpret_cast<void*>(static_cast<intptr_t>(fd))) != ESP_OK) {
                httpd_sess_trigger_close(server, fd);
            }
            return ESP_OK;
        }
        return ESP_OK;
    }

    // The live protocol is server-push-only. Consume a received control frame
    // so a curious client cannot make the HTTPD task retain buffered input.
    httpd_ws_frame_t frame{};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) { removeClient(fd); return ESP_FAIL; }
    if (frame.len > 0 && frame.len <= 64) {
        uint8_t discard[64];
        frame.payload = discard;
        if (httpd_ws_recv_frame(req, &frame, sizeof(discard)) != ESP_OK) { removeClient(fd); return ESP_FAIL; }
    } else if (frame.len > 64) {
        removeClient(fd);
        httpd_sess_trigger_close(server, fd);
    }
    return ESP_OK;
}

void sendPending(void*) {
    // A new client receives historical frames before the current frame, so
    // browser arrival order remains chronological. Twenty frames per worker
    // pass keeps the initial 5.04 KiB replay bounded and complete in 1.5 s.
    for (size_t slot = 0; slot < kMaxClients; ++slot) {
        bool sendFailed = false;
        for (uint8_t sent = 0; sent < kReplayFramesPerWork; ++sent) {
            int fd = -1;
            LiveReplayFrameV4 replay{};
            if (!nextReplayFrame(slot, fd, replay)) break;
            if (!sendFrame(fd, &replay, sizeof(replay))) {
                removeClient(fd);
                sendFailed = true;
                break;
            }
        }
        if (sendFailed) continue;
        int fd = -1;
        if (clientReadyForLive(slot, fd) &&
            !sendFrame(fd, &pendingFrame, sizeof(pendingFrame))) {
            removeClient(fd);
        }
    }
    sendQueued = false;
}

bool buildFrame(LiveFrameV5& frame) {
    sensors::Reading in{}, out{}, aux{};
    if (!sensors::getLatest(sensors::SENSOR_SOLAR, in) ||
        !sensors::getLatest(sensors::SENSOR_LOAD, out) ||
        !sensors::getLatest(sensors::SENSOR_BATTERY, aux)) return false;

    frame = {};
    LiveReplayFrameV4& live = frame.logical;
    live.magic = kLiveMagicV5;
    live.version = 5;
    live.type = 1;
    live.sequence = ++sequence;
    live.stateRevision = device_state::revision();
    live.uptimeMs = millis();
    live.sessionId = time_service::currentSessionId();
    live.unixMs = NAN;
    time_service::Anchor anchor{};
    if (time_service::getCurrentAnchor(anchor)) {
        live.unixMs = static_cast<double>(anchor.unixTimeMs) +
            static_cast<double>(time_service::monotonicUs() - anchor.monotonicUs) / 1000.0;
        live.flags |= 1;
    }
    const sensors::Reading readings[] = {in, out, aux};
    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; ++i) {
        if (sensors::isConfigured(readings[i])) live.flags |= static_cast<uint16_t>(1U << (1 + i));
        if (sensors::isCalculationEligible(readings[i])) live.flags |= static_cast<uint16_t>(1U << (4 + i));
        if (readings[i].state == sensors::ReadingState::Valid ||
            readings[i].state == sensors::ReadingState::OutOfRange) {
            live.flags |= static_cast<uint16_t>(1U << (7 + i));
        }
    }
    const auto observed = [](const sensors::Reading& reading) {
        return reading.state == sensors::ReadingState::Valid ||
               reading.state == sensors::ReadingState::OutOfRange;
    };
    live.inVoltage = observed(in) ? in.voltage : NAN;
    live.inCurrent = observed(in) ? in.current : NAN;
    live.inPower = observed(in) ? in.power : NAN;
    live.outVoltage = observed(out) ? out.voltage : NAN;
    live.outCurrent = observed(out) ? out.current : NAN;
    live.outPower = observed(out) ? out.power : NAN;
    live.auxVoltage = observed(aux) ? aux.voltage : NAN;
    live.auxCurrent = observed(aux) ? aux.current : NAN;
    live.auxPower = observed(aux) ? aux.power : NAN;
    float net = NAN;
    live.netBatteryPower = sensors::getNetBatteryPower(net) ? net : NAN;
    const auto duty = [](sensors::SensorId id, const sensors::Reading& reading) {
        if (!sensors::isCalculationEligible(reading)) return NAN;
        const float value = sensors::getDutyCycle(id);
        return std::isfinite(value) ? value : NAN;
    };
    live.inDuty = duty(sensors::SENSOR_SOLAR, in);
    live.outDuty = duty(sensors::SENSOR_LOAD, out);
    live.auxDuty = duty(sensors::SENSOR_BATTERY, aux);

    for (uint8_t i = 0; i < sensors::SENSOR_COUNT; ++i) {
        sensors::Reading physical{};
        const bool hasReading = sensors::getLatestPhysical(i, physical);
        frame.physicalStates[i] = static_cast<uint8_t>(
            hasReading ? physical.state : sensors::ReadingState::Waiting);
        if (hasReading && sensors::isConfigured(physical)) {
            frame.physicalFlags |= static_cast<uint16_t>(1U << i);
        }
        if (hasReading && sensors::isCalculationEligible(physical)) {
            frame.physicalFlags |= static_cast<uint16_t>(1U << (3 + i));
        }
        const bool physicalObserved = hasReading && observed(physical);
        if (physicalObserved) {
            frame.physicalFlags |= static_cast<uint16_t>(1U << (6 + i));
        }
        frame.physicalVoltage[i] = physicalObserved ? physical.voltage : NAN;
        frame.physicalCurrent[i] = physicalObserved ? physical.current : NAN;
        frame.physicalPower[i] = physicalObserved ? physical.power : NAN;
    }
    return true;
}

} // namespace

bool begin() {
    if (server) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = kPort;
    config.ctrl_port = kPort + 1;
    config.max_open_sockets = 8; // five browser clients plus HTTPD reserves
    config.max_uri_handlers = 2;
    config.stack_size = 4096;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    if (httpd_start(&server, &config) != ESP_OK) { server = nullptr; return false; }
    httpd_uri_t live = {};
    live.uri = "/api/v1/live";
    live.method = HTTP_GET;
    live.handler = websocketHandler;
    live.is_websocket = true;
    if (httpd_register_uri_handler(server, &live) != ESP_OK) {
        httpd_stop(server); server = nullptr; return false;
    }
    return true;
}

bool stop() {
    if (!server) return false;
    if (httpd_stop(server) != ESP_OK) return false;
    server = nullptr;
    portENTER_CRITICAL(&stateMux);
    for (Client& client : clients) client = {};
    sendQueued = false;
    portEXIT_CRITICAL(&stateMux);
    return true;
}

void update() {
    if (!server || sendQueued || millis() - lastPublishMs < kIntervalMs) return;
    LiveFrameV5 frame{};
    if (!buildFrame(frame)) return;
    lastPublishMs = millis();
    // Freeze the ring briefly while a new client walks it. That preserves the
    // chronological snapshot without a second 5.04 KiB buffer per client.
    if (!hasReplayPending()) {
        LiveReplayFrameV4 replay = frame.logical;
        replay.magic = kLiveMagicV4;
        replay.version = 4;
        recordReplayFrame(replay);
    }
    if (!hasClients()) return;
    pendingFrame = frame;
    sendQueued = true;
    if (httpd_queue_work(server, sendPending, nullptr) != ESP_OK) sendQueued = false;
}

uint8_t clientCount() {
    portENTER_CRITICAL(&stateMux);
    uint8_t count = 0;
    for (const Client& client : clients) if (client.fd >= 0) ++count;
    portEXIT_CRITICAL(&stateMux);
    return count;
}

uint8_t clientLimit() { return kMaxClients; }

} // namespace live_websocket_service
