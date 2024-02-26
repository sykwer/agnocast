#include <functional>
#include <memory>
#include <thread>
#include "rclcpp/rclcpp.hpp"

#include "sample_interfaces/msg/dynamic_size_array.hpp"
#include "agnocast.hpp"

using std::placeholders::_1;

class MinimalSubscriber : public rclcpp::Node {
  void topic_callback(const agnocast::message_ptr<sample_interfaces::msg::DynamicSizeArray> &message) {
    RCLCPP_INFO(this->get_logger(), "I heard message addr: %016lx", reinterpret_cast<uint64_t>(message.get()));

    for (size_t i = 0; i < message->data.size(); i++) {
      std::cout << message->data[i] << " ";
    }
    std::cout << std::endl;
  }

public:

  MinimalSubscriber() : Node("minimal_subscriber") {
    subscribe_topic_agnocast<sample_interfaces::msg::DynamicSizeArray>(
      "/mytopic", std::bind(&MinimalSubscriber::topic_callback, this, _1));
  }

  ~MinimalSubscriber() {}
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  initialize_agnocast();

  rclcpp::spin(std::make_shared<MinimalSubscriber>());

  shutdown_agnocast();
  rclcpp::shutdown();
  return 0;
}
