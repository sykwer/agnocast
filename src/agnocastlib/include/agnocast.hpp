#include <fcntl.h>
#include <semaphore.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <iostream>

#define MAX_TOPIC_NAME_LEN 255
#define MAX_TOPIC_NUM 256
#define TOPIC_QUEUE_DEPTH 5

class TopicQueueEntry;
class TopicQueue;
class TopicsTable;

void initialize_agnocast();

void join_topic_agnocast(const char* topic_name);

void enqueue_msg_agnocast(const std::string &topic_name, uint64_t timestamp, uint32_t pid, uint64_t msg_addr);

void read_msg_agnocast(const std::string &topic_name, size_t entry_idx);

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
  uint64_t get_timestamp();

  void set_timestamp(uint64_t timestamp);

  uint32_t get_pid();

  void set_pid(uint32_t pid);

  uint64_t get_msg_addr();

  void set_msg_addr(uint64_t msg_addr);

  uint32_t get_rc();

  void set_rc(uint32_t rc);
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

  uint32_t get_head();

  void set_head(uint32_t head);

  uint32_t get_tail();

  void set_tail(uint32_t tail);

public:

  TopicQueueEntry* get_entry(size_t idx);

  size_t size();

  void reset();

  bool enqueue_entry(uint64_t timestamp, uint32_t pid, uint64_t msg_addr);

  bool delete_head_entry();
};


class TopicsTable {
private:
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
