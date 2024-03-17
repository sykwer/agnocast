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
#include <chrono>
#include <sys/ioctl.h>

struct ioctl_subscriber_args {
    const char *topic_name;
    uint32_t pid;
};

struct ioctl_publisher_args {
    const char *topic_name;
    uint32_t pid;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
union ioctl_release_oldest_args {
    struct {
        const char *topic_name;
        uint32_t publisher_pid;
        uint32_t buffer_depth;
    };
    uint64_t ret;
};
#pragma GCC diagnostic pop

struct ioctl_enqueue_entry_args {
    const char *topic_name;
    uint32_t publisher_pid;
    uint64_t msg_virtual_address;
    uint64_t timestamp;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
union ioctl_update_entry_args {
    struct {
        const char *topic_name;
        uint32_t publisher_pid;
        uint64_t msg_timestamp;
    };
    uint64_t ret;
};
#pragma GCC diagnostic pop

#define MAX_SUBSCRIBER_NUM 16

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
union ioctl_publish_args {
    struct {
        const char *topic_name;
        uint32_t publisher_pid;
        uint64_t msg_timestamp;
    };
    struct {
        uint32_t ret_pids[MAX_SUBSCRIBER_NUM];
        uint32_t ret_len;
    };
};
#pragma GCC diagnostic pop

#define AGNOCAST_TOPIC_ADD_CMD _IOW('T', 1, char *)
#define AGNOCAST_SUBSCRIBER_ADD_CMD _IOW('S', 1, struct ioctl_subscriber_args)
#define AGNOCAST_SUBSCRIBER_REMOVE_CMD _IOW('S', 2, struct ioctl_subscriber_args)
#define AGNOCAST_PUBLISHER_ADD_CMD _IOW('P', 1, struct ioctl_publisher_args)
#define AGNOCAST_PUBLISHER_REMOVE_CMD _IOW('P', 2, struct ioctl_publisher_args)
#define AGNOCAST_RELEASE_OLDEST_CMD _IOW('P', 3, union ioctl_release_oldest_args)
#define AGNOCAST_ENQUEUE_ENTRY_CMD _IOW('E', 1, struct ioctl_enqueue_entry_args)
#define AGNOCAST_INCREMENT_RC_CMD _IOW('M', 1, union ioctl_update_entry_args)
#define AGNOCAST_DECREMENT_RC_CMD _IOW('M', 2, union ioctl_update_entry_args)
#define AGNOCAST_RECEIVE_MSG_CMD _IOW('M', 3, union ioctl_update_entry_args)
#define AGNOCAST_PUBLISH_MSG_CMD _IOW('M', 4, union ioctl_publish_args)

struct MqMsgAgnocast {
  uint32_t publisher_pid;
  uint64_t timestamp;
};

extern int agnocast_fd;
extern std::atomic<bool> is_running;
extern std::vector<std::thread> threads;

uint64_t agnocast_get_timestamp();

void initialize_agnocast();

void shutdown_agnocast();

namespace agnocast {
template<typename T> class message_ptr;
}

template<typename T>
void subscribe_topic_agnocast(const char* topic_name, std::function<void(const agnocast::message_ptr<T> &)> callback) {
  if (ioctl(agnocast_fd, AGNOCAST_TOPIC_ADD_CMD, topic_name) < 0) {
      perror("Failed to execute ioctl");
      close(agnocast_fd);
      exit(EXIT_FAILURE);
  }

  uint32_t subscriber_pid = getpid();

  std::string mq_name = std::string(topic_name) + "|" + std::to_string(getpid());
  mqd_t mq = mq_open(mq_name.c_str(), O_RDONLY);

  if (mq == -1) {
    std::cout << "create agnocast topic mq: " << mq_name << std::endl;

    struct mq_attr attr;
    attr.mq_flags = 0; // Blocking queue
    attr.mq_maxmsg = 10; // Maximum number of messages in the queue
    attr.mq_msgsize = sizeof(MqMsgAgnocast); // Maximum message size
    attr.mq_curmsgs = 0; // Number of messages currently in the queue (not set by mq_open)

    mq = mq_open(mq_name.c_str(), O_CREAT | O_RDONLY, 0666, &attr);
    if (mq == -1) {
      perror("mq_open");
      exit(EXIT_FAILURE);
    }
  }

  struct ioctl_subscriber_args args;
  args.pid = subscriber_pid;
  args.topic_name = topic_name;
  if (ioctl(agnocast_fd, AGNOCAST_SUBSCRIBER_ADD_CMD, &args) < 0) {
    perror("AGNOCAST_SUBSCRIBER_ADD_CMD failed");
    close(agnocast_fd);
    exit(EXIT_FAILURE);
  }

  // Create a thread that handles the messages to execute the callback
  auto th = std::thread([=]() {
    std::cout << "callback thread for " << topic_name << " has been started" << std::endl;
    MqMsgAgnocast mq_msg;

    while (is_running) {
      auto ret = mq_receive(mq, reinterpret_cast<char*>(&mq_msg), sizeof(mq_msg), NULL);

      if (ret == -1) {
        std::cerr << "mq_receive error" << std::endl;
        perror("mq_receive error");
        return;
      }

      union ioctl_update_entry_args entry_args;
      entry_args.topic_name = topic_name;
      entry_args.publisher_pid = mq_msg.publisher_pid;
      entry_args.msg_timestamp = mq_msg.timestamp;
      if (ioctl(agnocast_fd, AGNOCAST_RECEIVE_MSG_CMD, &entry_args) < 0) {
        perror("AGNOCAST_RECEIVE_MSG_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
      }

      if (entry_args.ret == 0) {
        std::cerr << "The received message has message address zero" << std::endl;
        continue;
      }

      T* ptr = reinterpret_cast<T*>(entry_args.ret); 
      agnocast::message_ptr<T> agnocast_ptr = agnocast::message_ptr<T>(ptr, topic_name, mq_msg.publisher_pid, mq_msg.timestamp, true);

      if (subscriber_pid == mq_msg.publisher_pid) {
        return;
      }

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
  const char *topic_name_;
  std::string topic_name_cpp_;
  uint32_t publisher_pid_;

public:

  Publisher(std::string topic_name) {
    topic_name_cpp_ = topic_name;
    topic_name_ = topic_name_cpp_.c_str();
    publisher_pid_ = getpid();

    if (ioctl(agnocast_fd, AGNOCAST_TOPIC_ADD_CMD, topic_name_) < 0) {
        perror("Failed to execute ioctl");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }

    struct ioctl_publisher_args pub_args;
    pub_args.pid = publisher_pid_;
    pub_args.topic_name = topic_name_;
     if (ioctl(agnocast_fd, AGNOCAST_PUBLISHER_ADD_CMD, &pub_args) < 0) {
        perror("AGNOCAST_SUBSCRIBER_ADD_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }
  }

  message_ptr<MessageT> borrow_loaded_message() {
    MessageT *ptr = new MessageT();
    return borrow_loaded_message(ptr);
  }

  message_ptr<MessageT> borrow_loaded_message(MessageT *ptr) {
    union ioctl_release_oldest_args release_args;
    release_args.topic_name = topic_name_;
    release_args.publisher_pid = publisher_pid_;
    release_args.buffer_depth = 5;
    if (ioctl(agnocast_fd, AGNOCAST_RELEASE_OLDEST_CMD, &release_args) < 0) {
        perror("AGNOCAST_RELEASE_OLDEST_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }

    if (release_args.ret != 0) {
      MessageT *release_ptr = reinterpret_cast<MessageT*>(release_args.ret);
      delete release_ptr;
    }

    uint64_t timestamp = agnocast_get_timestamp();

    struct ioctl_enqueue_entry_args enqueue_args;
    enqueue_args.topic_name = topic_name_;
    enqueue_args.publisher_pid = publisher_pid_;
    enqueue_args.msg_virtual_address = reinterpret_cast<uint64_t>(ptr);
    enqueue_args.timestamp = timestamp;
    if (ioctl(agnocast_fd, AGNOCAST_ENQUEUE_ENTRY_CMD, &enqueue_args) < 0) {
        perror("AGNOCAST_ENQUEUE_ENTRY_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }

    return message_ptr<MessageT>(ptr, topic_name_, publisher_pid_, timestamp);
  }

  void publish(message_ptr<MessageT>&& message) {
    if (topic_name_ != message.get_topic_name()) return; // string comparison?
    if (publisher_pid_ != message.get_publisher_pid()) return;

    union ioctl_publish_args publish_args;
    publish_args.topic_name = topic_name_;
    publish_args.publisher_pid = publisher_pid_;
    publish_args.msg_timestamp = message.get_timestamp();
    if (ioctl(agnocast_fd, AGNOCAST_PUBLISH_MSG_CMD, &publish_args) < 0) {
        perror("AGNOCAST_PUBLISH_MSG_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }

    for (uint32_t i = 0; i < publish_args.ret_len; i++) {
      uint32_t pid = publish_args.ret_pids[i];

      std::string mq_name = std::string(topic_name_) + "|" + std::to_string(pid);
      mqd_t mq = mq_open(mq_name.c_str(), O_WRONLY);

      if (mq == -1) {
        perror("mq_open");
        std::cerr << "mq_open error" << std::endl;
        continue;
      }

      MqMsgAgnocast mq_msg;
      mq_msg.publisher_pid = publisher_pid_;
      mq_msg.timestamp = message.get_timestamp();

      if (mq_send(mq, reinterpret_cast<char*>(&mq_msg), sizeof(mq_msg), 0) == -1) {
        perror("mq_send");
        std::cerr << "mq_send error" << std::endl;
        continue;
      }
    }
  }
};

template<typename T>
class message_ptr {
  T *ptr_ = nullptr;
  const char *topic_name_;
  uint32_t publisher_pid_;
  uint64_t timestamp_;

  void release() {
    if (ptr_ == nullptr) return;

    union ioctl_update_entry_args entry_args;
    entry_args.topic_name = topic_name_;
    entry_args.publisher_pid = publisher_pid_;
    entry_args.msg_timestamp = timestamp_;
    if (ioctl(agnocast_fd, AGNOCAST_DECREMENT_RC_CMD, &entry_args) < 0) {
        perror("AGNOCAST_DECREMENT_RC_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }

    ptr_ = nullptr;
  }

  void increment_rc() {
    union ioctl_update_entry_args entry_args;
    entry_args.topic_name = topic_name_;
    entry_args.publisher_pid = publisher_pid_;
    entry_args.msg_timestamp = timestamp_;
    if (ioctl(agnocast_fd, AGNOCAST_INCREMENT_RC_CMD, &entry_args) < 0) {
        perror("AGNOCAST_INCREMENT_RC_CMD failed");
        close(agnocast_fd);
        exit(EXIT_FAILURE);
    }
  }

public:
  const char* get_topic_name() { return topic_name_; }
  uint32_t get_publisher_pid() { return publisher_pid_; }
  uint64_t get_timestamp() { return timestamp_; }

  message_ptr() { }

  explicit message_ptr(T *ptr, const char *topic_name, uint32_t publisher_pid, uint64_t timestamp, bool already_rc_incremented = false) :
      ptr_(ptr), topic_name_(topic_name), publisher_pid_(publisher_pid), timestamp_(timestamp) {
    if (!already_rc_incremented) {
      increment_rc();
    }
  }

  ~message_ptr() {
    release();
  }

  message_ptr(const message_ptr &r) :
      ptr_(r.ptr_), topic_name_(r.topic_name_), publisher_pid_(r.publisher_pid_), timestamp_(r.timestamp_) {
    increment_rc();
  }

  message_ptr& operator =(const message_ptr &r) {
    if (this == &r) return *this;

    release();

    ptr_ = r.ptr_;
    topic_name_ = r.topic_name_;
    publisher_pid_ = r.publisher_pid_;
    timestamp_ = r.timestamp_;

    increment_rc();
  }

  message_ptr(message_ptr &&r) :
      ptr_(r.ptr_), topic_name_(r.topic_name_), publisher_pid_(r.publisher_pid_), timestamp_(r.timestamp_) {
    r.ptr_ = nullptr;
  }

  message_ptr& operator =(message_ptr &&r) {
    release();
    
    ptr_ = r.ptr_;
    topic_name_ = r.topic_name_;
    publisher_pid_ = r.publisher_pid_;
    timestamp_ = r.timestamp_;

    r.ptr_ = nullptr;
  }

  T& operator *() const noexcept { return *ptr_; }

  T* operator ->() const noexcept { return ptr_; }

  T* get() const noexcept { return ptr_; }
};

}
