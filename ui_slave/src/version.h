#ifndef UI_SLAVE_VERSION_H
#define UI_SLAVE_VERSION_H

// Replaced at release time by .github/workflows/release.yml
#define FIRMWARE_VERSION "dev"

// GitHub release endpoint queried by OtaUpdaterUI::checkForUpdate().
// Releases must publish an asset named OTA_FIRMWARE_ASSET_NAME built from
// this ui_slave project (see release.yml).
#define OTA_GITHUB_API_URL "https://api.github.com/repos/dirkessl/OpenChessMate/releases/latest"
#define OTA_FIRMWARE_ASSET_NAME "ui_slave_firmware.bin"

#endif // UI_SLAVE_VERSION_H
