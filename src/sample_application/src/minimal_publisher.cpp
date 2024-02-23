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
public:
  MinimalPublisher() : Node("minimal_publisher"), count_(0) {
    // publisher_ = this->create_publisher<sample_interfaces::msg::DynamicSizeArray>("topic", 1);
    timer_ = this->create_wall_timer(3000ms, std::bind(&MinimalPublisher::timer_callback, this));

    /* Initialize agnocast central data structure */
    initialize_agnocast();
    mytopic_publisher_idx_ = join_topic_agnocast("/mytopic");
    /* To here */
  }

  ~MinimalPublisher() {
    shutdown_agnocast();
  }

private:
  void timer_callback() {
    /*
    auto message = sample_interfaces::msg::DynamicSizeArray();
    message.id = count_++;
    message.data.resize(MESSAGE_SIZE);
    RCLCPP_INFO(this->get_logger(), "Publishing Message ID: '%ld'", message.id);
    */

    std::string topic_name = "/mytopic";

    int entry_idx = enqueue_msg_agnocast(topic_name.c_str(), count_++, getpid(), 0xdeadbeef);
    if (entry_idx < 0) {
      return;
    }

    publish_msg_agnocast(get_topic_idx_tmp(topic_name), mytopic_publisher_idx_, entry_idx);

    // publisher_->publish(std::move(message));
  }

  rclcpp::TimerBase::SharedPtr timer_;
  // rclcpp::Publisher<sample_interfaces::msg::DynamicSizeArray>::SharedPtr publisher_;
  size_t count_;
  uint32_t mytopic_publisher_idx_;
};

int main(int argc, char * argv[]) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<MinimalPublisher>());
  rclcpp::shutdown();
  return 0;
}
