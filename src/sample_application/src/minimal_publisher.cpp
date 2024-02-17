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

using namespace std::chrono_literals;
const long long MESSAGE_SIZE = 2ll * 1024;


const char *agnocast_shm_name = "/agnocast_shm";
const size_t agnocast_shm_size = 1024 * 1024;
const char *agnocast_sem_name = "/agnocast_sem";
void* shm_ptr = nullptr;

const size_t MAX_TOPIC_NAME_LEN = 255;
const size_t MAX_TOPIC_NUM = 256;
const size_t TOPIC_QUEUE_DEPTH = 5;

class TopicQueueEntry;
class TopicQueue;
class TopicsTable;


class TopicQueueEntry {
private:
  /*
  - timestamp (64bit, 8byte)
  - pid (32bit, 4byte)
  - msg_addr (64bit, 8byte)
  - rc (32bit, 4byte)
  */
  char data[24];

public:
  uint64_t get_timestamp() {
    uint64_t *ptr = reinterpret_cast<uint64_t*>(data);
    return *ptr;
  }

  void set_timestamp(uint64_t timestamp) {
    uint64_t *ptr = reinterpret_cast<uint64_t*>(data);
    *ptr = timestamp;
  }

  uint32_t get_pid() {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 8);
    return *ptr;
  }

  void set_pid(uint32_t pid) {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 8);
    *ptr = pid;
  }

  uint64_t get_msg_addr() {
    uint64_t *ptr = reinterpret_cast<uint64_t*>(data + 12);
    return *ptr;
  }

  void set_msg_addr(uint64_t msg_addr) {
    uint64_t *ptr = reinterpret_cast<uint64_t*>(data + 12);
    *ptr = msg_addr;
  }

  uint32_t get_rc() {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 20);
    return *ptr;
  }

  void set_rc(uint32_t rc) {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 20);
    *ptr = rc;
  }
};


class TopicQueue {
private:
  // [head, tail)

  /*
  - head (32bit, 4byte)
  - tail (32bit, 4byte)
  - entries (sizeof(TopicQueueEntry) * TOPIC_QUEUE_DEPTH)
  */
  char data[8 + sizeof(TopicQueueEntry) * TOPIC_QUEUE_DEPTH];

  uint32_t get_head() {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data);
    return *ptr;
  }

  void set_head(uint32_t head) {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data);
    *ptr = head;
  }

  uint32_t get_tail() {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 4);
    return *ptr;
  }

  void set_tail(uint32_t tail) {
    uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 4);
    *ptr = tail;
  }

  TopicQueueEntry* get_entry(size_t idx) {
    TopicQueueEntry* ptr = reinterpret_cast<TopicQueueEntry*>(data + 8 + idx * sizeof(TopicQueueEntry));
    return ptr;
  }

public:
  size_t size() {
    return (get_tail() - get_head() + TOPIC_QUEUE_DEPTH) % TOPIC_QUEUE_DEPTH;
  }

  void reset() {
    set_head(0);
    set_tail(0);
  }

  bool enqueue_entry(uint64_t timestamp, uint32_t pid, uint64_t msg_addr) {
    if (size() == TOPIC_QUEUE_DEPTH) return false;

    uint32_t target_tail = get_tail();
    set_tail((get_tail() + 1) % TOPIC_QUEUE_DEPTH);

    TopicQueueEntry* entry = get_entry(target_tail);
    entry->set_timestamp(timestamp);
    entry->set_pid(pid);
    entry->set_msg_addr(msg_addr);
    entry->set_rc(1);

    return true;
  }

  bool delete_head_entry() {
    if (size() == 0) return false;

    TopicQueueEntry *head_entry = get_entry(get_head());
    if (head_entry->get_rc() >= 1) return false;

    set_head((get_head() + 1) % TOPIC_QUEUE_DEPTH);
    return true;
  }
};


class TopicsTable {
private:
  /*
  - entries (MAX_TOPIC_NUM)
    - topic_name (MAX_TOPIC_NAME_LEN byte)
  */
  char data[MAX_TOPIC_NUM * (MAX_TOPIC_NAME_LEN + 1)];

public:
  void reset() {
    memset(data, 0, MAX_TOPIC_NUM * (MAX_TOPIC_NAME_LEN + 1));
  }

  // Returns -1 when the table is full or error, otherwise returns a topic index
  int join_topic(const char *topic_name) {
    for (size_t i = 0; i < MAX_TOPIC_NUM; i++) {
      char* ptr = reinterpret_cast<char*>(data + i * (MAX_TOPIC_NAME_LEN + 1));

      if (*ptr != '\0') {
        if (strcmp(ptr, topic_name) == 0) return i;
        continue;
      }

      if (strnlen(topic_name, MAX_TOPIC_NAME_LEN + 1) == MAX_TOPIC_NAME_LEN + 1) {
        std::cerr << "topic name length is too long: " << topic_name << std::endl;
        return -1;
      }

      strcpy(ptr, topic_name);
      TopicQueue *queue = reinterpret_cast<TopicQueue*>(reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + i * sizeof(TopicQueue));
      queue->reset();

      return i;
    }

    std::cerr << "The number of topics reached the maximum constraint" << std::endl;
    return -1;
  }
};

TopicsTable *topics_table = nullptr;
TopicQueue *topic_queues[MAX_TOPIC_NUM];

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

  shm_ptr = mmap(0, agnocast_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
  if (shm_ptr == MAP_FAILED) {
    perror("mmap");
    exit(EXIT_FAILURE);
  }

  std::cout << "shared memory fd is " << shm_fd << std::endl;

  // =====================================================
  topics_table = reinterpret_cast<TopicsTable*>(shm_ptr);
  topics_table->reset();
  // =====================================================

  if (sem_post(sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }
}

void join_topic_agnocast(const char* topic_name) {
  int topic_idx = topics_table->join_topic(topic_name);
  if (topic_idx == -1) exit(EXIT_FAILURE);

  TopicQueue *topic_queue = reinterpret_cast<TopicQueue*>(
    reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + topic_idx * sizeof(TopicQueue));
  topic_queues[topic_idx] = topic_queue;
}

class MinimalPublisher : public rclcpp::Node {
public:
  MinimalPublisher() : Node("minimal_publisher"), count_(0) {
    publisher_ = this->create_publisher<sample_interfaces::msg::DynamicSizeArray>("topic", 1);
    timer_ = this->create_wall_timer(3000ms, std::bind(&MinimalPublisher::timer_callback, this));

    /* Initialize agnocast central data structure */
    initialize_agnocast();
    join_topic_agnocast("mytopic");
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
