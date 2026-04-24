#include "OtaUpdater.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <strings.h>

#include <cstdlib>

#include "CrossPointSettings.h"
#include "bootloader_common.h"
#include "esp_flash_partitions.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_wifi.h"

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases/latest";
constexpr char releaseListUrl[] = "https://api.github.com/repos/" CROSSPOINT_GIT_REPOSITORY "/releases?per_page=2";
constexpr int maxReleaseMetadataBytes = 64 * 1024;

/* This is buffer and size holder to keep upcoming data from latestReleaseUrl */
char* local_buf;
int output_len;
bool metadata_too_large;

/*
 * When esp_crt_bundle.h included, it is pointing wrong header file
 * which is something under WifiClientSecure because of our framework based on arduno platform.
 * To manage this obstacle, don't include anything, just extern and it will point correct one.
 */
extern "C" {
extern esp_err_t esp_crt_bundle_attach(void* conf);
}

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

esp_err_t event_handler(esp_http_client_event_t* event) {
	/* We are only interested in HTTP_EVENT_ON_HEADER and HTTP_EVENT_ON_DATA
	 * events. For headers, we are only interested in Content-Length. */

  if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key != nullptr && event->header_value != nullptr &&
      strcasecmp(event->header_key, "Content-Length") == 0) {
    const unsigned long contentLength = strtoul(event->header_value, nullptr, 10);
    if (contentLength > maxReleaseMetadataBytes) {
      metadata_too_large = true;
      LOG_ERR("OTA", "Release metadata too large: %lu bytes", contentLength);
      return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
  }

  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

  if (event->data == nullptr || event->data_len == 0) {
    return ESP_OK;
  }

  const int newSize = output_len + event->data_len + 1;
  char* newBuf = static_cast<char*>(realloc(local_buf, static_cast<size_t>(newSize)));
  if (newBuf == nullptr) {
    LOG_ERR("OTA", "HTTP Client Out of Memory Failed, Allocation %d", newSize);
    return ESP_ERR_NO_MEM;
  }

  local_buf = newBuf;
  memcpy(local_buf + output_len, event->data, event->data_len);
  output_len += event->data_len;
  local_buf[output_len] = '\0';

  return ESP_OK;
} /* event_handler */

const char* getReleaseApiUrl() { return SETTINGS.includeBetaUpdates ? releaseListUrl : latestReleaseUrl; }

JsonVariantConst selectRelease(const JsonDocument& doc) {
  if (doc.is<JsonArrayConst>()) {
    for (JsonObjectConst release : doc.as<JsonArrayConst>()) {
      if (release["draft"] | false) {
        continue;
      }
      return release;
    }
    return JsonVariantConst();
  }

  if (doc.is<JsonObjectConst>()) {
    return doc.as<JsonObjectConst>();
  }

  return JsonVariantConst();
}
} /* namespace */

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  // Reset globals so retries start clean regardless of previous outcome
  local_buf = nullptr;
  output_len = 0;
  metadata_too_large = false;

  JsonDocument filter;
  esp_err_t esp_err;
  JsonDocument doc;

  updateAvailable = false;
  latestVersion.clear();
  otaUrl.clear();
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  render = false;

  const char* releaseApiUrl = getReleaseApiUrl();

  esp_http_client_config_t client_config = {
      .url = releaseApiUrl,
      .timeout_ms = 10000,
      .event_handler = event_handler,
      /* Default HTTP client buffer size 512 byte only */
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  /* To track life time of local_buf, dtor will be called on exit from that function */
  struct localBufCleaner {
    char** bufPtr;
    ~localBufCleaner() {
      if (*bufPtr) {
        free(*bufPtr);
        *bufPtr = NULL;
      }
    }
  } localBufCleaner = {&local_buf};

  esp_http_client_handle_t client_handle = esp_http_client_init(&client_config);
  if (!client_handle) {
    LOG_ERR("OTA", "HTTP Client Handle Failed");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_set_header(client_handle, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_set_header Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_http_client_perform(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_perform Failed : %s", esp_err_to_name(esp_err));
    esp_http_client_cleanup(client_handle);
    if (metadata_too_large) {
      return METADATA_TOO_LARGE_ERROR;
    }
    return HTTP_ERROR;
  }

  /* esp_http_client_close will be called inside cleanup as well*/
  esp_err = esp_http_client_cleanup(client_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_http_client_cleanup Failed : %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  if (SETTINGS.includeBetaUpdates) {
    filter[0]["tag_name"] = true;
    filter[0]["draft"] = true;
    filter[0]["assets"][0]["name"] = true;
    filter[0]["assets"][0]["browser_download_url"] = true;
    filter[0]["assets"][0]["size"] = true;
  } else {
    filter["tag_name"] = true;
    filter["assets"][0]["name"] = true;
    filter["assets"][0]["browser_download_url"] = true;
    filter["assets"][0]["size"] = true;
  }
  const DeserializationError error = deserializeJson(doc, local_buf, DeserializationOption::Filter(filter));
  if (error) {
    LOG_ERR("OTA", "JSON parse failed: %s", error.c_str());
    return JSON_PARSE_ERROR;
  }

  const JsonVariantConst release = selectRelease(doc);
  if (release.isNull()) {
    LOG_ERR("OTA", "No release found in response");
    return JSON_PARSE_ERROR;
  }

  if (!release["tag_name"].is<std::string>()) {
    LOG_ERR("OTA", "No tag_name found");
    return JSON_PARSE_ERROR;
  }

  if (!release["assets"].is<JsonArrayConst>()) {
    LOG_ERR("OTA", "No assets found");
    return JSON_PARSE_ERROR;
  }

  latestVersion = release["tag_name"].as<std::string>();

  for (JsonObjectConst asset : release["assets"].as<JsonArrayConst>()) {
    if (asset["name"] == "firmware.bin") {
      otaUrl = asset["browser_download_url"].as<std::string>();
      otaSize = asset["size"].as<size_t>();
      totalSize = otaSize;
      updateAvailable = true;
      break;
    }
  }

  if (!updateAvailable) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  LOG_DBG("OTA", "Found %s update: %s", SETTINGS.includeBetaUpdates ? "beta" : "stable", latestVersion.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor = 0, currentMinor = 0, currentPatch = 0, currentBetaRelease = 0, currentBetaBuild = 0;
  int latestMajor = 0, latestMinor = 0, latestPatch = 0, latestBetaRelease = 0, latestBetaBuild = 0;

  const auto currentVersion = CROSSPOINT_VERSION;
  const bool currentIsBeta = strstr(currentVersion, "-rc.") != nullptr;
  const bool latestIsBeta = latestVersion.find("-rc.") != std::string::npos;

  // Semantic version check with optional RC suffix. `sscanf()` will stop when
  // it reaches part of the input string that doesn't match the format, so this
  // format string works for versions like "1.31", "1.34.2", "1.35.0-rc.1", and
  // "1.36.0-rc.2.5".
  // This does not handle versions using the old "rc.<hash>" format, but
  // considering that people will need to manually install this release or later
  // to get this functionality anyway that should be fine.
  sscanf(latestVersion.c_str(), "%d.%d.%d-rc.%d.%d", &latestMajor, &latestMinor, &latestPatch, &latestBetaRelease,
         &latestBetaBuild);
  sscanf(currentVersion, "%d.%d.%d-rc.%d.%d", &currentMajor, &currentMinor, &currentPatch, &currentBetaRelease,
         &currentBetaBuild);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  /*
   * If we reach here, the stable version segments are equal. A stable release
   * is newer than an RC with the same version.
   */
  if (!latestIsBeta && currentIsBeta) {
    return true;
  }

  if (latestIsBeta && !currentIsBeta) {
    return false;
  }

  /*
   * If both versions are RCs, compare their RC release and build numbers.
   */
  if (latestIsBeta && currentIsBeta) {
    if (latestBetaRelease != currentBetaRelease) {
      return latestBetaRelease > currentBetaRelease;
    }
    if (latestBetaBuild != currentBetaBuild) {
      return latestBetaBuild > currentBetaBuild;
    }
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::cleanupUpdate() {
  if (otaHandle) {
    const esp_err_t err = esp_https_ota_finish(otaHandle);
    if (err != ESP_OK) {
      LOG_ERR("OTA", "esp_https_ota_finish on cleanup: %s", esp_err_to_name(err));
    }
    otaHandle = nullptr;
  }
  cancelRequested = false;
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void OtaUpdater::cancelUpdate() {
  if (otaHandle) {
    cleanupUpdate();
  } else {
    cancelRequested = true;
  }
}

OtaUpdater::OtaUpdaterError OtaUpdater::beginInstallUpdate() {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  cleanupUpdate();
  render = false;
  cancelRequested = false;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 10000,
      .max_redirection_count = 5,
      .buffer_size = 8192,
      .buffer_size_tx = 8192,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  esp_err_t esp_err = esp_https_ota_begin(&ota_config, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    cleanupUpdate();
    return INTERNAL_UPDATE_ERROR;
  }

  return UPDATE_IN_PROGRESS;
}

/* Writes the otadata entry to boot from the most recently flashed OTA partition,
 * bypassing esp_ota_set_boot_partition()'s image_validate() call.
 * Used when esp_https_ota_finish() returns ESP_ERR_OTA_VALIDATE_FAILED on
 * unsigned Arduino builds (boot_comm efuse revision check false-positive). */
int OtaUpdater::forceSetOtaBootPartition() {
  const esp_partition_t* newPartition = esp_ota_get_next_update_partition(nullptr);
  if (newPartition == nullptr) {
    return ESP_ERR_NOT_FOUND;
  }

  const esp_partition_t* otaDataPartition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (otaDataPartition == nullptr) {
    return ESP_ERR_NOT_FOUND;
  }

  esp_ota_select_entry_t otadata[2];
  esp_err_t err = esp_partition_read(otaDataPartition, 0, &otadata[0], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) return err;
  err = esp_partition_read(otaDataPartition, otaDataPartition->erase_size, &otadata[1], sizeof(esp_ota_select_entry_t));
  if (err != ESP_OK) return err;

  int activeSlot = bootloader_common_get_active_otadata(otadata);
  int nextSlot = (activeSlot == -1) ? 0 : (~activeSlot & 1);

  uint8_t otaAppCount = 0;
  while (esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + otaAppCount),
                                  nullptr) != nullptr) {
    otaAppCount++;
  }
  if (otaAppCount == 0) return ESP_ERR_NOT_FOUND;

  const uint8_t subTypeId = newPartition->subtype & 0x0F;
  uint32_t newSeq;
  if (activeSlot == -1) {
    newSeq = subTypeId + 1;
  } else {
    uint32_t currentSeq = otadata[activeSlot].ota_seq;
    newSeq = currentSeq;
    while (newSeq % otaAppCount != static_cast<uint32_t>(subTypeId)) {
      newSeq++;
    }
    if (newSeq == currentSeq) newSeq += otaAppCount;
  }

  otadata[nextSlot].ota_seq = newSeq;
  otadata[nextSlot].ota_state = ESP_OTA_IMG_VALID;
  otadata[nextSlot].crc = bootloader_common_ota_select_crc(&otadata[nextSlot]);

  err = esp_partition_erase_range(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                                  otaDataPartition->erase_size);
  if (err != ESP_OK) return err;

  return esp_partition_write(otaDataPartition, otaDataPartition->erase_size * static_cast<uint32_t>(nextSlot),
                             &otadata[nextSlot], sizeof(esp_ota_select_entry_t));
}

OtaUpdater::OtaUpdaterError OtaUpdater::performInstallUpdateStep() {
  if (cancelRequested) {
    cleanupUpdate();
    return UPDATE_CANCELLED;
  }

  if (!otaHandle) {
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err_t esp_err = esp_https_ota_perform(otaHandle);
  processedSize = esp_https_ota_get_image_len_read(otaHandle);
  render = true;

  if (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
    return UPDATE_IN_PROGRESS;
  }

  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    cleanupUpdate();
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(otaHandle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed");
    cleanupUpdate();
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err_t finish_err = esp_https_ota_finish(otaHandle);
  otaHandle = nullptr;
  if (finish_err == ESP_ERR_OTA_VALIDATE_FAILED) {
    /* Arduino unsigned builds fail boot_comm validation even though the image
     * is fully written. Force the boot partition to the new OTA slot by writing
     * the otadata entry directly, bypassing image_validate(). */
    LOG_INF("OTA", "Validation failed (expected for unsigned Arduino builds) - forcing boot partition");
    finish_err = forceSetOtaBootPartition();
    if (finish_err != ESP_OK) {
      LOG_ERR("OTA", "forceSetOtaBootPartition failed: %s", esp_err_to_name(finish_err));
      cleanupUpdate();
      return VALIDATE_FAILED;
    }
  } else if (finish_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(finish_err));
    cleanupUpdate();
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
