#include <functional>
#include <memory>
#include <thread>
#include "rclcpp/rclcpp.hpp"

#include "sample_interfaces/msg/dynamic_size_array.hpp"
#include "agnocast.hpp"

using std::placeholders::_1;

class MinimalSubscriber : public rclcpp::Node {
public:
  MinimalSubscriber() : Node("minimal_subscriber") {
    /*
    subscription_ = this->create_subscription<sample_interfaces::msg::DynamicSizeArray>(
      "topic", 1, std::bind(&MinimalSubscriber::topic_callback, this, _1));
    */

    /* Initialize agnocast central data structure */
    initialize_agnocast();
    join_topic_agnocast("/mytopic");
    subscribe_topic_agnocast<uint64_t>("/mytopic", std::bind(&MinimalSubscriber::topic_callback, this, _1));
    /* To here */
  }

  ~MinimalSubscriber() {
    shutdown_agnocast();
  }

private:
  /*
  void topic_callback(const sample_interfaces::msg::DynamicSizeArray::SharedPtr msg) {
    RCLCPP_INFO(this->get_logger(), "I heard message ID: '%ld'", msg->id);
  }
  */
  void topic_callback(uint64_t msg_addr) {
    RCLCPP_INFO(this->get_logger(), "I heard message addr: %ld", msg_addr);
  }

  //rclcpp::Subscription<sample_interfaces::msg::DynamicSizeArray>::SharedPtr subscription_;
};

int main(int argc, char * argv[])
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<MinimalSubscriber>());
  rclcpp::shutdown();
  return 0;
}
