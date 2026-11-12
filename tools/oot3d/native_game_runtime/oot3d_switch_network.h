#pragma once
// HTTPS downloads on the Switch (libcurl on libnx's TLS service), shared by
// the OTA updater (oot3d_switch_updater.cpp) and the TopScreen HUD texture
// download (oot3d_switch_topscreen_import.cpp). Sockets and libcurl are only
// up while a NetworkSession lives.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Oot3dNativeGame::SwitchNetwork {

// Sockets and libcurl for one check or download. curl_global_init opens
// the console's SSL service (devkitPro's libcurl uses libnx's TLS backend:
// certificates are checked by the system against its trusted CAs); both
// are closed again when the session ends.
class NetworkSession {
public:
  NetworkSession();
  ~NetworkSession();
  NetworkSession(const NetworkSession &) = delete;
  NetworkSession &operator=(const NetworkSession &) = delete;
  bool Ready() const { return mCurl; }

private:
  bool mSockets = false;
  bool mCurl = false;
};

struct Transfer {
  // Receives the body; false stops the transfer (Failure, if set, says why).
  std::function<bool(const char *, size_t)> Write;
  // The body may not be longer than this.
  std::uint64_t Limit = 0;
  std::uint64_t Received = 0;
  const std::atomic<bool> *Cancel = nullptr;
  std::string Failure;
};

enum class GetResult { Ok, Cancelled, Failed };

// HTTPS GET of `url` with extra request `headers` ("Name: value"),
// following up to 5 redirects (GitHub and GameBanana send downloads to a
// CDN), HTTPS only. A stall below 1 KiB/s for 30 s fails. On failure
// `*status` is the last HTTP status (0 without one) and `*error` the reason;
// an HTTP error reads "the server answered HTTP <status>".
GetResult HttpGet(const std::string &url, const std::vector<std::string> &headers, Transfer &transfer,
                  long *status, std::string *error);

} // namespace Oot3dNativeGame::SwitchNetwork
