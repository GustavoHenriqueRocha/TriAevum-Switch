#include "oot3d_switch_network.h"

#include "oot3d_build_version.h"

#include <curl/curl.h>
#include <switch.h>

#include <array>

namespace Oot3dNativeGame::SwitchNetwork {
namespace {

constexpr long kConnectTimeoutSeconds = 15;
// A transfer that stays below 1 KiB/s for this long fails.
constexpr long kStallSeconds = 30;

size_t WriteCallback(char *data, size_t size, size_t count, void *user) {
  auto &transfer = *static_cast<Transfer *>(user);
  const size_t length = size * count;
  if (transfer.Cancel->load(std::memory_order_relaxed)) return 0;
  if (transfer.Received + length > transfer.Limit) {
    transfer.Failure = "the download is longer than expected";
    return 0;
  }
  if (!transfer.Write(data, length)) {
    if (transfer.Failure.empty()) transfer.Failure = "cannot write to the SD card";
    return 0;
  }
  transfer.Received += length;
  return length;
}

int ProgressCallback(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<Transfer *>(user)->Cancel->load(std::memory_order_relaxed) ? 1 : 0;
}

} // namespace

NetworkSession::NetworkSession() {
  mSockets = R_SUCCEEDED(socketInitializeDefault());
  mCurl = mSockets && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
}

NetworkSession::~NetworkSession() {
  if (mCurl) curl_global_cleanup();
  if (mSockets) socketExit();
}

GetResult HttpGet(const std::string &url, const std::vector<std::string> &headers, Transfer &transfer,
                  long *status, std::string *error) {
  *status = 0;
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    *error = "libcurl could not start a request";
    return GetResult::Failed;
  }
  curl_slist *headerList = nullptr;
  for (const std::string &header : headers) headerList = curl_slist_append(headerList, header.c_str());
  const std::string userAgent = std::string("oot3dre-switch/") + BuildVersion();
  std::array<char, CURL_ERROR_SIZE> message{};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kStallSeconds);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(transfer.Limit));
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message.data());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
  const CURLcode code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headerList);
  if (code == CURLE_OK) return GetResult::Ok;
  if (transfer.Cancel->load(std::memory_order_relaxed) || code == CURLE_ABORTED_BY_CALLBACK)
    return GetResult::Cancelled;
  if (code == CURLE_WRITE_ERROR && !transfer.Failure.empty()) {
    *error = transfer.Failure;
  } else if (code == CURLE_FILESIZE_EXCEEDED) {
    *error = "the download is longer than expected";
  } else if (code == CURLE_HTTP_RETURNED_ERROR) {
    *error = "the server answered HTTP " + std::to_string(*status);
  } else {
    *error = message[0] != '\0' ? std::string(message.data()) : curl_easy_strerror(code);
  }
  return GetResult::Failed;
}

} // namespace Oot3dNativeGame::SwitchNetwork
