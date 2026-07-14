#include "live_websocket_service.h"

#include <Arduino.h>
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
constexpr uint32_t kLiveMagic = 0x314d5056; // "VPM1" little endian

struct __attribute__((packed)) LiveFrame {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint16_t flags;
    uint32_t sequence;
    uint32_t stateRevision;
    uint32_t uptimeMs;
    uint32_t reserved;
    double unixMs;
    float inVoltage;
    float inCurrent;
    float inPower;
    float outVoltage;
    float outCurrent;
    float outPower;
    float auxPower;
    float netBatteryPower;
};
static_assert(sizeof(LiveFrame) == 64, "web live protocol V1 frame changed");

struct Client {
    int fd = -1;
    uint8_t replayIndex = 0;
    uint8_t replayRemaining = 0;
};

httpd_handle_t server = nullptr;
Client clients[kMaxClients];
LiveFrame replayFrames[kReplayFrameCount] = {};
uint8_t replayStart = 0;
uint8_t replayCount = 0;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastPublishMs = 0;
uint32_t sequence = 0;
volatile bool sendQueued = false;
LiveFrame pendingFrame{};

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

bool nextReplayFrame(size_t slot, int& fd, LiveFrame& frame) {
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

void recordReplayFrame(const LiveFrame& frame) {
    portENTER_CRITICAL(&stateMux);
    const uint8_t slot = static_cast<uint8_t>((replayStart + replayCount) % kReplayFrameCount);
    replayFrames[slot] = frame;
    if (replayCount < kReplayFrameCount) ++replayCount;
    else replayStart = static_cast<uint8_t>((replayStart + 1) % kReplayFrameCount);
    portEXIT_CRITICAL(&stateMux);
}

bool sendFrame(int fd, const LiveFrame& data) {
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_BINARY;
    frame.payload = reinterpret_cast<uint8_t*>(const_cast<LiveFrame*>(&data));
    frame.len = sizeof(data);
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

    // The V1 protocol is server-push-only. Consume a received control frame
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
    // pass keeps the initial 3.84 KiB replay bounded and complete in 1.5 s.
    for (size_t slot = 0; slot < kMaxClients; ++slot) {
        bool sendFailed = false;
        for (uint8_t sent = 0; sent < kReplayFramesPerWork; ++sent) {
            int fd = -1;
            LiveFrame replay{};
            if (!nextReplayFrame(slot, fd, replay)) break;
            if (!sendFrame(fd, replay)) { removeClient(fd); sendFailed = true; break; }
        }
        if (sendFailed) continue;
        int fd = -1;
        if (clientReadyForLive(slot, fd) && !sendFrame(fd, pendingFrame)) removeClient(fd);
    }
    sendQueued = false;
}

bool buildFrame(LiveFrame& frame) {
    sensors::Reading in{}, out{}, aux{};
    if (!sensors::getLatest(sensors::SENSOR_IN, in) ||
        !sensors::getLatest(sensors::SENSOR_OUT, out) ||
        !sensors::getLatest(sensors::SENSOR_AUX, aux)) return false;

    frame = {};
    frame.magic = kLiveMagic;
    frame.version = 1;
    frame.type = 1;
    frame.sequence = ++sequence;
    frame.stateRevision = device_state::revision();
    frame.uptimeMs = millis();
    frame.unixMs = NAN;
    time_service::Anchor anchor{};
    if (time_service::getCurrentAnchor(anchor)) {
        frame.unixMs = static_cast<double>(anchor.unixTimeMs) +
            static_cast<double>(time_service::monotonicUs() - anchor.monotonicUs) / 1000.0;
        frame.flags |= 1;
    }
    frame.inVoltage = in.voltage; frame.inCurrent = in.current; frame.inPower = in.power;
    frame.outVoltage = out.voltage; frame.outCurrent = out.current; frame.outPower = out.power;
    frame.auxPower = aux.power;
    float net = NAN;
    frame.netBatteryPower = sensors::getNetBatteryPower(net) ? net : NAN;
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

void update() {
    if (!server || sendQueued || millis() - lastPublishMs < kIntervalMs) return;
    LiveFrame frame{};
    if (!buildFrame(frame)) return;
    lastPublishMs = millis();
    // Freeze the ring briefly while a new client walks it. That preserves the
    // chronological snapshot without a second 3.84 KiB buffer per client.
    if (!hasReplayPending()) recordReplayFrame(frame);
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
