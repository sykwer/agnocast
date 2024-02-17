#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include "rclcpp/rclcpp.hpp"

#include "sample_interfaces/msg/dynamic_size_array.hpp"

using namespace std::chrono_literals;

const long long MESSAGE_SIZE = 2ll * 1024;

const char *agnocast_shm_name = "/agnocast_shm";
const size_t agnocast_shm_size = 8192;
const char *agnocast_sem_name = "/agnocast_sem";

void initialize_agnocast() {
  sem_t *sem = sem_open(agnocast_sem_name, 0);
  if (sem == SEM_FAILED) {
    std::cout << "create agnocast semaphore" << std::endl;

    sem = sem_open(agnocast_sem_name, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED) {
      perror("sem_open");
      exit(EXIT_FAILURE);
    }
  }

  if (sem_wait(sem) == -1) {
    perror("sem_wait");
    exit(EXIT_FAILURE);
  }

  // =====================================================

  int shm_fd = shm_open(agnocast_shm_name, O_RDWR, 0666);

  if (shm_fd == -1) {
    std::cout << "create agnocast shared memory" << std::endl;

    shm_fd = shm_open(agnocast_shm_name, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
      perror("shm_open");
      exit(EXIT_FAILURE);
    }

    if (ftruncate(shm_fd, agnocast_shm_size) == -1) {
      perror("ftruncate");
      exit(EXIT_FAILURE);
    }

  } else {
    std::cout << "agnocast shared memory already exists" << std::endl;
  }

  void* shm_ptr = mmap(0, agnocast_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (shm_ptr == MAP_FAILED) {
    perror("mmap");
    exit(EXIT_FAILURE);
  }

  std::cout << "shared memory fd is " << shm_fd << std::endl;

  // =====================================================
  if (sem_post(sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }
}

class MinimalPublisher : public rclcpp::Node {
public:
  MinimalPublisher() : Node("minimal_publisher"), count_(0) {
    publisher_ = this->create_publisher<sample_interfaces::msg::DynamicSizeArray>("topic", 1);
    timer_ = this->create_wall_timer(3000ms, std::bind(&MinimalPublisher::timer_callback, this));

    /* Initialize agnocast central data structure */
    initialize_agnocast();
    /* To here */
  }

  ~MinimalPublisher() {

  }

private:
  void timer_callback() {
    auto message = sample_interfaces::msg::DynamicSizeArray();
    message.id = count_++;
    message.data.resize(MESSAGE_SIZE);
    RCLCPP_INFO(this->get_logger(), "Publishing Message ID: '%ld'", message.id);
    publisher_->publish(std::move(message));
  }

  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Publisher<sample_interfaces::msg::DynamicSizeArray>::SharedPtr publisher_;
  size_t count_;
};

int main(int argc, char * argv[]) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<MinimalPublisher>());
  rclcpp::shutdown();
  return 0;
}
