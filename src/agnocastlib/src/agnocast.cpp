#include <atomic>
#include <map>

#include "agnocast.hpp"

int agnocast_fd;
std::atomic<bool> is_running = true;
std::vector<std::thread> threads;

uint64_t agnocast_get_timestamp() {
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

void shutdown_agnocast() {
  is_running = false;

  for (auto &th : threads) {
    th.join();
  }
}

void initialize_agnocast() {
  agnocast_fd = open("/dev/agnocast", O_RDWR);
  if (agnocast_fd < 0) {
      perror("Failed to open the device");
      exit(EXIT_FAILURE);
  }
}
