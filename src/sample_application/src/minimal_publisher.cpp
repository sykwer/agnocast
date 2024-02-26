#include <fcntl.h>
#include <semaphore.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include "rclcpp/rclcpp.hpp"

#include "sample_interfaces/msg/dynamic_size_array.hpp"
#include "agnocast.hpp"

using namespace std::chrono_literals;
const long long MESSAGE_SIZE = 2ll * 1024;

class MinimalPublisher : public rclcpp::Node {
  void timer_callback() {
    std::cout << std::endl;
    agnocast::message_ptr<sample_interfaces::msg::DynamicSizeArray> message = publisher_->borrow_loaded_message();

    for (size_t i = 0; i < 10; i++) {
      message->data.push_back(i + count_);
    }
    count_++;

    publisher_->publish(std::move(message));
  }

  rclcpp::TimerBase::SharedPtr timer_;
  std::shared_ptr<agnocast::Publisher<sample_interfaces::msg::DynamicSizeArray>> publisher_;
  uint8_t count_;

public:

  MinimalPublisher() : Node("minimal_publisher") {
    timer_ = this->create_wall_timer(3000ms, std::bind(&MinimalPublisher::timer_callback, this));
    publisher_ = agnocast::create_publisher<sample_interfaces::msg::DynamicSizeArray>("/mytopic");
    count_ = 0;
  }

  ~MinimalPublisher() {}
};

int main(int argc, char * argv[]) {
  rclcpp::init(argc, argv);
  initialize_agnocast();

  rclcpp::spin(std::make_shared<MinimalPublisher>());

  shutdown_agnocast();
  rclcpp::shutdown();

  return 0;
}
