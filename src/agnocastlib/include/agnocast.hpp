#pragma once

#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

#define MAX_TOPIC_NAME_LEN 255
#define MAX_TOPIC_NUM 256
#define MAX_SUBSCRIBER_NUM 16
#define MAX_PUBLISHER_NUM 16
#define TOPIC_QUEUE_DEPTH_MAX 20
#define TOPIC_QUEUE_DEPTH 5

class TopicQueues;

class TopicPublisherQueueEntry {
  /*
  - timestamp (64bit, 8byte)
  - pid (32bit, 4byte)
  - msg_addr (64bit, 8byte)
  - rc (32bit, 4byte)
  - unreceived_sub_num (32bit, 4byte)
  */
  char data[28];

public:
  uint64_t get_timestamp();

  void set_timestamp(uint64_t timestamp);

  uint32_t get_pid();

  void set_pid(uint32_t pid);

  uint64_t get_msg_addr();

  void set_msg_addr(uint64_t msg_addr);

  uint32_t get_rc();

  void set_rc(uint32_t rc);

  uint32_t get_unreceived_sub_num();

  void set_unreceived_sub_num(uint32_t unreceived_sub_num);
};


class TopicPublisherQueue {
  // [head, tail)

  // TODO: Add `full` field

  /*
  - publisher_pid (32bit, 4byte)
  - head (32bit, 4byte)
  - tail (32bit, 4byte)
  - parent_ptr (64bit, 8byte)
  - entries (sizeof(TopicPublisherQueueEntry) * TOPIC_QUEUE_DEPTH_MAX)
  */
  char data[20 + sizeof(TopicPublisherQueueEntry) * TOPIC_QUEUE_DEPTH_MAX];

public:

  uint32_t get_publisher_pid();

  void set_publisher_pid(uint32_t pid);

  uint32_t get_head();

  void set_head(uint32_t head);

  uint32_t get_tail();

  void set_tail(uint32_t tail);

  TopicQueues* get_parent_ptr();

  void set_parent_ptr(TopicQueues *parent_ptr);

  TopicPublisherQueueEntry* get_entry(size_t idx);

  std::vector<uint32_t> get_subscriber_pids();

  bool add_subscriber_pid(uint32_t pid);

  void reset();

  size_t size();

  int enqueue_entry(uint64_t timestamp, uint32_t pid, uint64_t msg_addr);

  bool delete_head_entry();
};


class TopicQueues {
  /*
  - publisher_num (32bit, 4byte)
  - queues (sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM)
  - subscriber_num (32bit, 4byte)
  - subscriber_pids (4byte * MAX_SUBSCRIBER_NUM)
  */
  char data[4 + sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM + 4 + 4 * MAX_SUBSCRIBER_NUM];

public:

  uint32_t get_publisher_num();

  uint32_t get_subscriber_num();

  void increment_publisher_num();

  void increment_subscriber_num();

  void reset();

  TopicPublisherQueue* add_publisher_pid(uint32_t pid);

  bool add_subscriber_pid(uint32_t pid);

  std::vector<uint32_t> get_subscriber_pids();

  // Returns publisher index
  uint32_t create_or_find_publisher_queue(uint32_t pid);
};


class TopicsTable {
  /*
  - entries (MAX_TOPIC_NUM)
    - topic_name (MAX_TOPIC_NAME_LEN byte)
  */
  char data[MAX_TOPIC_NUM * (MAX_TOPIC_NAME_LEN + 1)];

public:
  void reset();

  // Returns -1 when the table is full or error, otherwise returns a topic index
  int join_topic(const char *topic_name);
};

struct ShmMsgAgnocast {
  uint32_t publisher_idx;
  uint32_t entry_idx;
};

extern std::atomic<bool> is_running;
extern std::vector<std::thread> threads;

extern const char *agnocast_shm_name;
extern const size_t agnocast_shm_size;
extern const char *agnocast_sem_name;
extern void* shm_ptr;

extern TopicsTable *topics_table;
extern TopicPublisherQueue *topic_publisher_queues[MAX_TOPIC_NUM];
extern std::map<std::string, uint32_t> topic_name_to_idx;
extern std::map<uint32_t, std::string> topic_idx_to_name;

void initialize_agnocast();

// Returns publisher index
uint32_t join_topic_agnocast(const char* topic_name);

int enqueue_msg_agnocast(const std::string &topic_name, uint64_t timestamp, uint32_t pid, uint64_t msg_addr);

void publish_msg_agnocast(uint32_t topic_idx, uint32_t publisher_idx, uint32_t entry_idx);

uint64_t read_msg_agnocast(const std::string &topic_name, uint32_t publisher_idx, uint32_t entry_idx);

void shutdown_agnocast();

uint32_t get_topic_idx_tmp(const std::string &topic_name) {
  return topic_name_to_idx[topic_name];
}

template<typename T>
void subscribe_topic_agnocast(const char* topic_name, std::function<void(T)> callback) {
  // Get topic's lock
  sem_t *topic_sem = sem_open(topic_name, 0);
  if (topic_sem == SEM_FAILED) {
    perror("sem_open");
    exit(EXIT_FAILURE);
  }

  if (sem_wait(topic_sem) == -1) {
    perror("sem_wait");
    exit(EXIT_FAILURE);
  }

  uint32_t topic_idx = topic_name_to_idx[topic_name];
  TopicQueues *queues = reinterpret_cast<TopicQueues*>(
    reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + topic_idx * sizeof(TopicQueues));
  queues->add_subscriber_pid(getpid());

  // Create POSIX message queue
  std::string mq_name = std::string(topic_name) + "|" + std::to_string(getpid());
  mqd_t mq = mq_open(mq_name.c_str(), O_RDONLY);

  if (mq == -1) {
    std::cout << "create agnocast topic mq: " << mq_name << std::endl;

    struct mq_attr attr;
    attr.mq_flags = 0; // Blocking queue
    attr.mq_maxmsg = 10; // Maximum number of messages in the queue
    attr.mq_msgsize = sizeof(ShmMsgAgnocast); // Maximum message size
    attr.mq_curmsgs = 0; // Number of messages currently in the queue (not set by mq_open)

    mq = mq_open(mq_name.c_str(), O_CREAT | O_RDONLY, 0666, &attr);
    if (mq == -1) {
      perror("mq_open");
      exit(EXIT_FAILURE);
    }
  }

  // Release topic's lock
  if (sem_post(topic_sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }

  // Create a thread that handles the messages to execute the callback
  auto th = std::thread([=]() {
    std::cout << "callback thread for " << topic_name << " has been started" << std::endl;
    ShmMsgAgnocast msg;

    while (is_running) {
      auto ret = mq_receive(mq, reinterpret_cast<char*>(&msg), sizeof(msg), NULL);
      if (ret == -1) {
        std::cerr << "mq_receive error" << std::endl;
        perror("mq_receive error");
        return;
      }

      uint64_t msg_addr = read_msg_agnocast(topic_name, msg.publisher_idx, msg.entry_idx);

      callback(msg_addr);
    }
  });

  threads.push_back(std::move(th));
}

