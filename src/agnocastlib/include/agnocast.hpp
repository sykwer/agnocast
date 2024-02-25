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
#include <memory>
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

void publish_msg_agnocast(uint32_t topic_idx, uint32_t publisher_idx, uint32_t entry_idx);

uint64_t read_msg_agnocast(const std::string &topic_name, uint32_t publisher_idx, uint32_t entry_idx);

void shutdown_agnocast();

uint32_t get_topic_idx_tmp(const std::string &topic_name) {
  return topic_name_to_idx[topic_name];
}

template <typename MessageT>
int enqueue_msg_agnocast(const std::string &topic_name, uint64_t timestamp, uint32_t pid, uint64_t msg_addr) {
  std::cout << "enqueue_msg_agnocast(" << topic_name << ", " << timestamp << ", " << pid << ", " << msg_addr << ");" << std::endl;
  uint32_t topic_idx = topic_name_to_idx[topic_name];

  sem_t *topic_sem = sem_open(topic_name.c_str(), 0);
  if (topic_sem == SEM_FAILED) {
    perror("sem_open");
    exit(EXIT_FAILURE);
  }

  if (sem_wait(topic_sem) == -1) {
    perror("sem_wait");
    exit(EXIT_FAILURE);
  }

  // Release ring buffer entry before enqueue
  TopicPublisherQueue* queue = topic_publisher_queues[topic_idx];
  size_t curr_size = queue->size();

  if (curr_size >= TOPIC_QUEUE_DEPTH) {
    uint32_t head_idx = queue->get_head();
    TopicPublisherQueueEntry *head_entry = queue->get_entry(head_idx);
    MessageT* msg_ptr = reinterpret_cast<MessageT*>(head_entry->get_msg_addr());

    if (head_entry->get_rc() == 0 && head_entry->get_unreceived_sub_num() == 0) {
      bool success = queue->delete_head_entry();

      if (!success) {
        std::cerr << "delete_head_entry() failed" << std::endl;
        exit(EXIT_FAILURE);
      }

      delete msg_ptr;

      std::cout << "Release ring buffer entry: topic_name=" << topic_name <<
        ", size(" << curr_size << "->" << queue->size() << "), head_idx=" << head_idx << std::endl;
    } else {
      std::cout << "Tried to release ring buffer entry but skipped: " << topic_name << ", topic_idx=" << topic_idx << ", pid=" << pid <<
        "curr_size=" << curr_size << ", TOPIC_QUEUE_DEPTH=" << TOPIC_QUEUE_DEPTH << ", TOPIC_QUEUE_DEPTH_MAX=" << TOPIC_QUEUE_DEPTH_MAX <<
        ", head_entry->get_rc()=" << head_entry->get_rc() << ", head_entry->get_unreceived_sub_num()=" << head_entry->get_unreceived_sub_num() << std::endl;
    }
  }
  // To here

  int entry_idx = topic_publisher_queues[topic_idx]->enqueue_entry(timestamp, pid, msg_addr);

  if (sem_post(topic_sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }

  if (entry_idx < 0) {
    std::cerr << "failed to publish message to " << topic_name << std::endl;
    return -1;
  }

  return entry_idx;
}

namespace agnocast {
template<typename T> class message_ptr;
}

template<typename T>
void subscribe_topic_agnocast(const char* topic_name, std::function<void(const agnocast::message_ptr<T> &)> callback) {
  join_topic_agnocast("/mytopic");

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

      T* ptr = reinterpret_cast<T*>(read_msg_agnocast(topic_name, msg.publisher_idx, msg.entry_idx));
      agnocast::message_ptr<T> agnocast_ptr = agnocast::message_ptr<T>(ptr, topic_idx, msg.publisher_idx, msg.entry_idx);

      // Decrement unreceived_sub_num
      if (sem_wait(topic_sem) == -1) {
        perror("sem_wait");
        exit(EXIT_FAILURE);
      }

      TopicQueues *queues = reinterpret_cast<TopicQueues*>(
        reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + topic_idx * sizeof(TopicQueues));
      TopicPublisherQueue *queue = reinterpret_cast<TopicPublisherQueue*>(
        reinterpret_cast<char*>(queues) + 4 + msg.publisher_idx * sizeof(TopicPublisherQueue));
      TopicPublisherQueueEntry* entry = queue->get_entry(msg.entry_idx);
      entry->set_unreceived_sub_num(entry->get_unreceived_sub_num() - 1);

      if (sem_post(topic_sem) == -1) {
        perror("sem_post");
        exit(EXIT_FAILURE);
      }
      // To here

      callback(agnocast_ptr);
    }
  });

  threads.push_back(std::move(th));
}


namespace agnocast {

template<typename MessageT> class Publisher;
template<typename T> class message_ptr;

template<typename MessageT>
std::shared_ptr<Publisher<MessageT>> create_publisher(std::string topic_name) {
  return std::make_shared<Publisher<MessageT>>(topic_name);
}


template<typename MessageT>
class Publisher {
  std::string topic_name_;
  uint32_t topic_idx_;
  uint32_t publisher_idx_;

public:

  Publisher(std::string topic_name) {
    topic_name_ = topic_name;
    publisher_idx_ = join_topic_agnocast(topic_name.c_str());
    topic_idx_ = topic_name_to_idx[topic_name];
  }

  message_ptr<MessageT> borrow_loaded_message() {
    MessageT* ptr = new MessageT();
    int entry_idx = enqueue_msg_agnocast<MessageT>(topic_name_, 0 /*timestamp*/, getpid(), reinterpret_cast<uint64_t>(ptr));

    if (entry_idx < 0) {
      std::cerr << "failed to borrow message" << std::endl;
      exit(EXIT_FAILURE);
    }

    return message_ptr<MessageT>(ptr, topic_idx_, publisher_idx_, entry_idx);
  }

  void publish(message_ptr<MessageT>&& message) {
    publish_msg_agnocast(topic_idx_, publisher_idx_, message.get_entry_idx());
  }
};


template<typename T>
class message_ptr {
  T *ptr_ = nullptr;
  uint32_t topic_idx_;
  uint32_t publisher_idx_;
  uint32_t entry_idx_;

  TopicPublisherQueue* get_topic_publisher_queue() {
    TopicQueues *queues = reinterpret_cast<TopicQueues*>(
      reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + topic_idx_ * sizeof(TopicQueues));

    TopicPublisherQueue *queue = reinterpret_cast<TopicPublisherQueue*>(
      reinterpret_cast<char*>(queues) + 4 + publisher_idx_ * sizeof(TopicPublisherQueue));

    return queue;
  }

  TopicPublisherQueueEntry* get_queue_entry() {
    return get_topic_publisher_queue()->get_entry(entry_idx_);
  }

  void release() {
    if (ptr_ == nullptr) return;

    TopicPublisherQueueEntry *entry = get_queue_entry();

    sem_t *topic_sem = sem_open(topic_idx_to_name[topic_idx_].c_str(), 0);
    if (topic_sem == SEM_FAILED) {
      perror("sem_open");
      exit(EXIT_FAILURE);
    }

    if (sem_wait(topic_sem) == -1) {
      perror("sem_wait");
      exit(EXIT_FAILURE);
    }

    entry->set_rc(entry->get_rc() - 1);

    if (sem_post(topic_sem) == -1) {
      perror("sem_post");
      exit(EXIT_FAILURE);
    }

    ptr_ = nullptr;
  }

  void increment_rc() {
    TopicPublisherQueueEntry *entry = get_queue_entry();

    sem_t *topic_sem = sem_open(topic_idx_to_name[topic_idx_].c_str(), 0);
    if (topic_sem == SEM_FAILED) {
      perror("sem_open");
      exit(EXIT_FAILURE);
    }

    if (sem_wait(topic_sem) == -1) {
      perror("sem_wait");
      exit(EXIT_FAILURE);
    }

    entry->set_rc(entry->get_rc() + 1);

    if (sem_post(topic_sem) == -1) {
      perror("sem_post");
      exit(EXIT_FAILURE);
    }
  }

public:

  message_ptr() { }

  explicit message_ptr(T *ptr, uint32_t topic_idx, uint32_t publisher_idx, uint32_t entry_idx) :
      ptr_(ptr), topic_idx_(topic_idx), publisher_idx_(publisher_idx), entry_idx_(entry_idx) {
    increment_rc();
  }

  ~message_ptr() {
    release();
  }

  message_ptr(const message_ptr &r) :
      ptr_(r.ptr_), topic_idx_(r.topic_idx_), publisher_idx_(r.publisher_idx_), entry_idx_(r.entry_idx_) {
    increment_rc();
  }

  message_ptr& operator =(const message_ptr &r) {
    if (this == &r) return *this;

    release();

    ptr_ = r.ptr_;
    topic_idx_ = r.topic_idx_;
    publisher_idx_ = r.publisher_idx_;
    entry_idx_ = r.entry_idx_;

    increment_rc();
  }

  message_ptr(message_ptr &&r) :
      ptr_(r.ptr_), topic_idx_(r.topic_idx_), publisher_idx_(r.publisher_idx_), entry_idx_(r.entry_idx_) {
    r.ptr_ = nullptr;
  }

  message_ptr& operator =(message_ptr &&r) {
    release();
    
    ptr_ = r.ptr_;
    topic_idx_ = r.topic_idx_;
    publisher_idx_ = r.publisher_idx_;
    entry_idx_ = r.entry_idx_;

    r.ptr_ = nullptr;
  }

  T& operator *() const noexcept { return *ptr_; }

  T* operator ->() const noexcept { return ptr_; }

  T* get() const noexcept { return ptr_; }

  uint32_t get_entry_idx() {
    return entry_idx_;
  }
};

}
