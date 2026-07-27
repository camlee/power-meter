#pragma once

#include <Arduino.h>
#include <cstdint>

// Shared signed-image installer and OTA health service. The LAN upload route
// and the outbound Internet updater both use the same verifier and writer.
namespace ota_service {

struct ManifestInfo {
    uint32_t format = 0;
    uint32_t imageSize = 0;
    char board[24]{};
    char version[64]{};
    char sha256[65]{};
    char channel[16]{};
    char releaseTag[72]{};
    char firmwareAsset[64]{};
};

// Starts HTTP endpoints on port 80. Safe to call repeatedly.
void begin();

// Must be called regularly from the main event loop after begin().
void update();

bool isRunning();

// Call after application setup has completed. A pending OTA image is only
// confirmed after this point and a short period of normal main-loop service.
void setApplicationReady();

// Called from the main loop to prove the application is still servicing its
// normal work while a newly booted image is awaiting confirmation.
void noteHealthyLoop();

// Verifies and parses a canonical signed manifest without touching flash.
bool verifySignedManifest(const String& manifest, const String& signatureBase64,
                          ManifestInfo& info, String& error);

// Claims the single OTA writer, verifies the manifest again, and starts
// writing the inactive application slot. All failures release the writer.
bool beginSignedInstall(const String& manifest, const String& signatureBase64,
                        ManifestInfo& info, String& error);
bool writeSignedInstall(const uint8_t* data, size_t size, String& error);
bool finishSignedInstall(String& error);
void abortSignedInstall(const char* reason = nullptr);
bool installInProgress();
uint32_t installBytesReceived();
uint32_t installExpectedBytes();
const char* installTargetVersion();

// Records rollback diagnostics and reboots into a successfully finalised image.
// Returns false only if the target partition could not be resolved.
bool rebootToInstalledImage();

// Small diagnostic helpers used by the on-device Debug screen.
const char* healthStatus();
const char* runningPartitionLabel();
const char* bootPartitionLabel();
const char* runningImageState();
bool rollbackDetected();
const char* rollbackVersion();
bool rollbackSupported();
uint32_t validationRemainingMs();
int64_t lastUpdateUnixSeconds();

} // namespace ota_service
