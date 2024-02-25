#include <atomic>
#include <map>

#include "agnocast.hpp"

std::atomic<bool> is_running = true;
std::vector<std::thread> threads;

const char *agnocast_shm_name = "/agnocast_shm";
const size_t agnocast_shm_size = 128 *1024 * 1024;
const char *agnocast_sem_name = "/agnocast_sem";
void* shm_ptr = nullptr;

TopicsTable *topics_table = nullptr;
TopicPublisherQueue *topic_publisher_queues[MAX_TOPIC_NUM];
std::map<std::string, uint32_t> topic_name_to_idx;
std::map<uint32_t, std::string> topic_idx_to_name;

void shutdown_agnocast() {
  is_running = false;

  for (auto &th : threads) {
    th.join();
  }
}

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
  bool shm_first_created = false;

  if (shm_fd == -1) {
    std::cout << "create agnocast shared memory" << std::endl;
    shm_first_created = true;

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
  if (shm_first_created) topics_table->reset();
  // =====================================================

  if (sem_post(sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }
}

// Returns publisher index
uint32_t join_topic_agnocast(const char* topic_name) {
  sem_t *sem = sem_open(agnocast_sem_name, 0);
  if (sem == SEM_FAILED) {
    perror("sem_open");
    exit(EXIT_FAILURE);
  }

  if (sem_wait(sem) == -1) {
    perror("sem_wait");
    exit(EXIT_FAILURE);
  }

  int topic_idx = topics_table->join_topic(topic_name);
  topic_name_to_idx[std::string(topic_name)] = topic_idx;
  topic_idx_to_name[topic_idx] = std::string(topic_name);

  sem_t *topic_sem = sem_open(topic_name, 0);
  if (topic_sem == SEM_FAILED) {
    std::cout << "create topic semaphore: " << topic_name << std::endl;

    topic_sem = sem_open(topic_name, O_CREAT, 0666, 1);
    if (topic_sem == SEM_FAILED) {
      perror("sem_open");
      exit(EXIT_FAILURE);
    }
  }

  if (sem_post(sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }

  if (topic_idx == -1) exit(EXIT_FAILURE);

  TopicQueues *queues = reinterpret_cast<TopicQueues*>(
    reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + topic_idx * sizeof(TopicQueues));
  uint32_t publisher_idx = queues->create_or_find_publisher_queue(getpid());

  TopicPublisherQueue *queue = reinterpret_cast<TopicPublisherQueue*>(
    reinterpret_cast<char*>(queues) + 4 + publisher_idx * sizeof(TopicPublisherQueue));
  topic_publisher_queues[topic_idx] = queue;

  return publisher_idx;
}

void publish_msg_agnocast(uint32_t topic_idx, uint32_t publisher_idx, uint32_t entry_idx) {
  std::cout << "publish_msg_agnocast(" << topic_idx << ", " << publisher_idx << ", " << entry_idx << ");" << std::endl;
  std::string topic_name = topic_idx_to_name[topic_idx];

  sem_t *topic_sem = sem_open(topic_name.c_str(), 0);
  if (topic_sem == SEM_FAILED) {
    perror("sem_open");
    exit(EXIT_FAILURE);
  }

  if (sem_wait(topic_sem) == -1) {
    perror("sem_wait");
    exit(EXIT_FAILURE);
  }

  TopicPublisherQueue* queue = topic_publisher_queues[topic_idx];
  auto pids = queue->get_subscriber_pids();
  TopicPublisherQueueEntry* entry = queue->get_entry(entry_idx);
  entry->set_unreceived_sub_num(pids.size());

  if (sem_post(topic_sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }

  for (uint32_t pid : pids) {
    std::string mq_name = std::string(topic_name) + "|" + std::to_string(pid);
    mqd_t mq = mq_open(mq_name.c_str(), O_WRONLY);

    if (mq == -1) {
      perror("mq_open");
      std::cerr << "mq_open error" << std::endl;
      continue;
    }

    ShmMsgAgnocast msg;
    msg.publisher_idx = publisher_idx;
    msg.entry_idx = entry_idx;

    if (mq_send(mq, reinterpret_cast<char*>(&msg), sizeof(msg), 0) == -1) {
      perror("mq_send");
      std::cerr << "mq_send error" << std::endl;
      continue;
    }
  }
}

uint64_t read_msg_agnocast(const std::string &topic_name, uint32_t publisher_idx, uint32_t entry_idx) {
	uint32_t topic_idx = topic_name_to_idx[topic_name];

  TopicPublisherQueue* queue = reinterpret_cast<TopicPublisherQueue*>(
    reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + topic_idx * sizeof(TopicQueues)
    + 4 + publisher_idx * sizeof(TopicPublisherQueue));

  sem_t *topic_sem = sem_open(topic_name.c_str(), 0);
  if (topic_sem == SEM_FAILED) {
    perror("sem_open");
    exit(EXIT_FAILURE);
  }

  if (sem_wait(topic_sem) == -1) {
    perror("sem_wait");
    exit(EXIT_FAILURE);
  }

	TopicPublisherQueueEntry* entry = queue->get_entry(entry_idx);
  uint64_t msg_addr = entry->get_msg_addr();

  if (sem_post(topic_sem) == -1) {
    perror("sem_post");
    exit(EXIT_FAILURE);
  }

  return msg_addr;
}

void TopicsTable::reset() {
  memset(data, 0, MAX_TOPIC_NUM * (MAX_TOPIC_NAME_LEN + 1));
}

// Returns -1 when the table is full or error, otherwise returns a topic index
int TopicsTable::join_topic(const char *topic_name) {
  for (size_t i = 0; i < MAX_TOPIC_NUM; i++) {
    char* ptr = reinterpret_cast<char*>(data + i * (MAX_TOPIC_NAME_LEN + 1));

    if (*ptr != '\0') {
      // The topic is already created
      if (strcmp(ptr, topic_name) == 0) return i;

      // The topic index is already used
      continue;
    }

    if (strnlen(topic_name, MAX_TOPIC_NAME_LEN + 1) == MAX_TOPIC_NAME_LEN + 1) {
      std::cerr << "topic name length is too long: " << topic_name << std::endl;
      return -1;
    }

    strcpy(ptr, topic_name);

    TopicQueues *queues = reinterpret_cast<TopicQueues*>(
      reinterpret_cast<char*>(shm_ptr) + sizeof(TopicsTable) + i * sizeof(TopicQueues));
    queues->reset();

    return i;
  }

  std::cerr << "The number of topics reached the maximum constraint" << std::endl;
  return -1;
}

uint32_t TopicQueues::get_publisher_num() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data);
  return *ptr;
}

uint32_t TopicQueues::get_subscriber_num() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 4 + sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM);
  return *ptr;
}

void TopicQueues::increment_publisher_num() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data);
  (*ptr)++;
}

void TopicQueues::increment_subscriber_num() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 4 + sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM);
  (*ptr)++;
}

void TopicQueues::reset() {
  memset(data, 0, 4 + sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM + 4 + 4 * MAX_SUBSCRIBER_NUM);
}

TopicPublisherQueue* TopicQueues::add_publisher_pid(uint32_t pid) {
  uint32_t n = get_publisher_num();
  if (n == MAX_PUBLISHER_NUM) return nullptr;

  TopicPublisherQueue *queue = reinterpret_cast<TopicPublisherQueue*>(
    data + 4 + sizeof(TopicPublisherQueue) * n);

  increment_publisher_num();

  queue->reset();
  queue->set_publisher_pid(pid);
  queue->set_parent_ptr(this);

  return queue;
}

bool TopicQueues::add_subscriber_pid(uint32_t pid) {
  if (get_subscriber_num() == MAX_SUBSCRIBER_NUM) return false;

  uint32_t *ptr = reinterpret_cast<uint32_t*>(
    data + 4 + sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM + 4 + 4 * get_subscriber_num());

  *ptr = pid;
  increment_subscriber_num();

  return true;
}

std::vector<uint32_t> TopicQueues::get_subscriber_pids() {
  size_t n = get_subscriber_num();
  uint32_t* ptr = reinterpret_cast<uint32_t*>(
    data + 4 + sizeof(TopicPublisherQueue) * MAX_PUBLISHER_NUM + 4);

  std::vector<uint32_t> pids;

  for (size_t i = 0; i < n; i++) {
    pids.push_back(*ptr);
    ptr++;
  }

  return pids;
}


// Returns publisher index
uint32_t TopicQueues::create_or_find_publisher_queue(uint32_t pid) {
  size_t n = get_publisher_num();

  for (uint32_t i = 0; i < n; i++) {
    TopicPublisherQueue* queue = reinterpret_cast<TopicPublisherQueue*>(
      data + 4 + sizeof(TopicPublisherQueue) * i);
    if (queue->get_publisher_pid() == pid) {
      return i;
    }
  }

  if (n == MAX_PUBLISHER_NUM) {
    std::cerr << "TopicQueues::create_or_find_publisher_queue error: too many publisher" << std::endl;
    return -1;
  }

  // Create publisher queue
  add_publisher_pid(pid);

  return n;
}

uint32_t TopicPublisherQueue::get_publisher_pid() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data);
  return *ptr;
}

void TopicPublisherQueue::set_publisher_pid(uint32_t pid) {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data);
  *ptr = pid;
}

uint32_t TopicPublisherQueue::get_head() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 4);
  return *ptr;
}

void TopicPublisherQueue::set_head(uint32_t head) {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 4);
  *ptr = head;
}

uint32_t TopicPublisherQueue::get_tail() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 8);
  return *ptr;
}

void TopicPublisherQueue::set_tail(uint32_t tail) {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 8);
  *ptr = tail;
}

TopicQueues* TopicPublisherQueue::get_parent_ptr() {
  TopicQueues** ptr = reinterpret_cast<TopicQueues**>(data + 12);
  return *ptr;
}

void TopicPublisherQueue::set_parent_ptr(TopicQueues *parent_ptr) {
  TopicQueues** ptr = reinterpret_cast<TopicQueues**>(data + 12);
  *ptr = parent_ptr;
}

TopicPublisherQueueEntry* TopicPublisherQueue::get_entry(size_t idx) {
  TopicPublisherQueueEntry* ptr = reinterpret_cast<TopicPublisherQueueEntry*>(
    data + 20 + idx * sizeof(TopicPublisherQueueEntry));
  return ptr;
}

std::vector<uint32_t> TopicPublisherQueue::get_subscriber_pids() {
  return get_parent_ptr()->get_subscriber_pids();
}

bool TopicPublisherQueue::add_subscriber_pid(uint32_t pid) {
  return get_parent_ptr()->add_subscriber_pid(pid);
}

void TopicPublisherQueue::reset() {
  memset(data, 0, 20 + sizeof(TopicPublisherQueueEntry) * TOPIC_QUEUE_DEPTH_MAX);
}

size_t TopicPublisherQueue::size() {
  // TODO: Cannot recognize between full and empty
  return (get_tail() - get_head() + TOPIC_QUEUE_DEPTH_MAX) % TOPIC_QUEUE_DEPTH_MAX;
}

int TopicPublisherQueue::enqueue_entry(uint64_t timestamp, uint32_t pid, uint64_t msg_addr) {
  // TODO: Change the condition to `size() == TOPIC_QUEUE_DEPTH_MAX`
  if (size() == TOPIC_QUEUE_DEPTH_MAX - 1) return -1;

  uint32_t target_tail = get_tail();
  set_tail((get_tail() + 1) % TOPIC_QUEUE_DEPTH_MAX);

  TopicPublisherQueueEntry* entry = get_entry(target_tail);
  entry->set_timestamp(timestamp);
  entry->set_pid(pid);
  entry->set_msg_addr(msg_addr);
  entry->set_rc(0);
  entry->set_unreceived_sub_num(0);

  return target_tail;
}

bool TopicPublisherQueue::delete_head_entry() {
  if (size() == 0) return false;

  TopicPublisherQueueEntry *head_entry = get_entry(get_head());
  if (head_entry->get_rc() >= 1 || head_entry->get_unreceived_sub_num() >= 1) return false;

  set_head((get_head() + 1) % TOPIC_QUEUE_DEPTH_MAX);
  return true;
}

uint64_t TopicPublisherQueueEntry::get_timestamp() {
  uint64_t *ptr = reinterpret_cast<uint64_t*>(data);
  return *ptr;
}

void TopicPublisherQueueEntry::set_timestamp(uint64_t timestamp) {
  uint64_t *ptr = reinterpret_cast<uint64_t*>(data);
  *ptr = timestamp;
}

uint32_t TopicPublisherQueueEntry::get_pid() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 8);
  return *ptr;
}

void TopicPublisherQueueEntry::set_pid(uint32_t pid) {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 8);
  *ptr = pid;
}

uint64_t TopicPublisherQueueEntry::get_msg_addr() {
  uint64_t *ptr = reinterpret_cast<uint64_t*>(data + 12);
  return *ptr;
}

void TopicPublisherQueueEntry::set_msg_addr(uint64_t msg_addr) {
  uint64_t *ptr = reinterpret_cast<uint64_t*>(data + 12);
  *ptr = msg_addr;
}

uint32_t TopicPublisherQueueEntry::get_rc() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 20);
  return *ptr;
}

void TopicPublisherQueueEntry::set_rc(uint32_t rc) {
  std::cout << "TopicPublisherQueueEntry::set_rc(" <<
    rc << "): publisher's pid is " << get_pid() << std::endl;

  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 20);
  *ptr = rc;
}

uint32_t TopicPublisherQueueEntry::get_unreceived_sub_num() {
  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 24);
  return *ptr;
}

void TopicPublisherQueueEntry::set_unreceived_sub_num(uint32_t unreceived_sub_num) {
  std::cout << "TopicPublisherQueueEntry::set_unreceived_sub_num(" <<
    unreceived_sub_num << "): publisher's pid is " << get_pid() << std::endl;

  uint32_t *ptr = reinterpret_cast<uint32_t*>(data + 24);
  *ptr = unreceived_sub_num;
}
